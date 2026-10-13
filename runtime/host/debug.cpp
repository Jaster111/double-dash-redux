// Headless testing aids: scripted controller input, course lap patching and a
// hang detector.  All are off unless their environment variable is set.
//
// DD_SCRIPT=file   controller-1 input script, one step per line (times are VI fields, ~60/s):
//                    wait N                  neutral input for N fields
//                    hold N BTN...           hold buttons / stick directions for N fields
//                    tap BTN... [xCOUNT]     4 fields pressed + 8 released, COUNT times (default 1)
//                    mark TEXT               log "[script] TEXT" with the current field / game frame
//                    shot NAME               save the next presented frame to shots/NAME.png
//                    waitframes N            wait until the game has rendered N more frames
//                    quit [CODE]             exit the process
//                  BTN: A B X Y Z L R START UP DOWN LEFT RIGHT (d-pad) SL SR SU SD (main stick)
//                  When the script ends the pad stays neutral.  '#' starts a comment.
// DD_LAPS=N        patch the lap count of every course (BOL header byte 0x18) as it is read from disc.
// DD_HANG_SECS=S   after the game has started rendering, report a hang when no game frame was
//                  produced for S seconds (default 6): thread backtraces and device state.
// DD_HANG_EXIT=1   exit with code 3 after reporting a hang (and 0 on `quit`).
#include "../common.h"
#include <windows.h>
#include <string>
#include <vector>
#include <sstream>
#include <algorithm>

u32 vi_field_count();
u32 gx_game_frames();
void gx_request_shot(const char* path);
bool gx_shot_pending();
void os_debug_dump_full();
void dsp_debug_dump();
void ev_debug_dump();

// ---------------------------------------------------------------- input script
enum : u16 {
    PAD_LEFT = 0x0001, PAD_RIGHT = 0x0002, PAD_DOWN = 0x0004, PAD_UP = 0x0008, PAD_Z = 0x0010, PAD_R = 0x0020,
    PAD_L = 0x0040, PAD_A = 0x0100, PAD_B = 0x0200, PAD_X = 0x0400, PAD_Y = 0x0800, PAD_START = 0x1000,
};

struct ScriptInput { u16 buttons = 0; int sx = 0, sy = 0; };
struct Step {
    enum Kind { Wait, Hold, Tap, Mark, Shot, WaitFrames, Quit } kind;
    u32 n = 0;
    ScriptInput in;
    std::string text;
};

static std::vector<Step> s_steps;
static size_t s_pc;
static u32 s_step_start_field, s_step_start_frame;
static bool s_step_started;
static ScriptInput s_cur;
static bool s_script_on;
static int s_script_line_err;

static bool parse_button(const std::string& w, ScriptInput& in) {
    struct { const char* name; u16 b; int sx, sy; } tab[] = {
        {"A", PAD_A}, {"B", PAD_B}, {"X", PAD_X}, {"Y", PAD_Y}, {"Z", PAD_Z}, {"L", PAD_L}, {"R", PAD_R},
        {"START", PAD_START}, {"UP", PAD_UP}, {"DOWN", PAD_DOWN}, {"LEFT", PAD_LEFT}, {"RIGHT", PAD_RIGHT},
        {"SL", 0, -1, 0}, {"SR", 0, 1, 0}, {"SU", 0, 0, 1}, {"SD", 0, 0, -1},
    };
    for (auto& t : tab)
        if (!_stricmp(w.c_str(), t.name)) {
            in.buttons |= t.b;
            if (t.sx) in.sx = t.sx;
            if (t.sy) in.sy = t.sy;
            return true;
        }
    return false;
}

static void load_script(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) { LOG("script", "cannot open %s", path); return; }
    char line[512];
    int ln = 0;
    while (fgets(line, sizeof line, f)) {
        ln++;
        std::string s = line;
        if (size_t h = s.find('#'); h != std::string::npos) s.resize(h);
        std::istringstream is(s);
        std::string cmd;
        if (!(is >> cmd)) continue;
        Step st{};
        std::string w;
        auto bad = [&] { LOG("script", "%s:%d: cannot parse: %s", path, ln, line); s_script_line_err = ln; };
        if (cmd == "wait") { st.kind = Step::Wait; is >> st.n; }
        else if (cmd == "waitframes") { st.kind = Step::WaitFrames; is >> st.n; }
        else if (cmd == "hold") {
            st.kind = Step::Hold;
            is >> st.n;
            while (is >> w) if (!parse_button(w, st.in)) bad();
        } else if (cmd == "tap") {
            st.kind = Step::Tap;
            st.n = 1;
            while (is >> w) {
                if ((w[0] == 'x' || w[0] == 'X') && w.size() > 1 && isdigit((u8)w[1])) st.n = atoi(w.c_str() + 1);
                else if (!parse_button(w, st.in)) bad();
            }
        } else if (cmd == "mark" || cmd == "shot") {
            st.kind = cmd == "mark" ? Step::Mark : Step::Shot;
            std::getline(is >> std::ws, st.text);
            while (!st.text.empty() && isspace((u8)st.text.back())) st.text.pop_back();
        } else if (cmd == "quit") { st.kind = Step::Quit; is >> st.n; }
        else { bad(); continue; }
        s_steps.push_back(st);
    }
    fclose(f);
    s_script_on = true;
    LOG("script", "%s: %zu steps", path, s_steps.size());
}

// advance the script by one VI field
static void script_field() {
    u32 field = vi_field_count(), frame = gx_game_frames();
    s_cur = {};
    while (s_pc < s_steps.size()) {
        Step& st = s_steps[s_pc];
        if (!s_step_started) { s_step_started = true; s_step_start_field = field; s_step_start_frame = frame; }
        u32 el = field - s_step_start_field;
        bool done = false;
        switch (st.kind) {
        case Step::Wait: done = el >= st.n; break;
        case Step::Hold: if (el < st.n) s_cur = st.in; else done = true; break;
        case Step::Tap:
            if (el < st.n * 12) { if (el % 12 < 4) s_cur = st.in; }
            else done = true;
            break;
        case Step::WaitFrames: done = frame - s_step_start_frame >= st.n; break;
        case Step::Mark:
            LOG("script", "%s  (field %u, game frame %u)", st.text.c_str(), field, frame);
            done = true;
            break;
        case Step::Shot: {
            // request on the first field, then wait until the next presented frame has been saved
            std::string p = "shots/" + st.text + ".png";
            if (el == 0) {
                CreateDirectoryA("shots", nullptr);
                gx_request_shot(p.c_str());
            } else if (!gx_shot_pending()) {
                LOG("script", "shot %s (field %u)", p.c_str(), field);
                done = true;
            } else if (el > 30) {
                LOG("script", "shot %s: nothing presented (black screen?)", p.c_str());
                gx_request_shot("");
                done = true;
            }
            break;
        }
        case Step::Quit:
            LOG("script", "quit (field %u, game frame %u)", field, frame);
            ExitProcess(st.n);
        }
        if (!done) return;
        s_pc++;
        s_step_started = false;
        if (s_pc == s_steps.size()) LOG("script", "finished (field %u)", field);
    }
}

// called by host_get_pad for port 0: merge the scripted input
void dbg_script_pad(u16& buttons, u8& sx, u8& sy) {
    if (!s_script_on) return;
    buttons |= s_cur.buttons;
    if (s_cur.sx) sx = (u8)(128 + s_cur.sx * 100);
    if (s_cur.sy) sy = (u8)(128 + s_cur.sy * 100);
}

// ---------------------------------------------------------------- hang detector
static u32 s_hang_secs;
static bool s_hang_exit;
static u32 s_last_frame;
static u64 s_last_frame_ms;
static bool s_hang_reported;

static void hang_field() {
    if (!s_hang_secs) return;
    u32 frame = gx_game_frames();
    u64 now = GetTickCount64();
    if (frame != s_last_frame || frame == 0) {
        if (s_hang_reported && frame != s_last_frame) LOG("hang", "game frames resumed (frame %u)", frame);
        s_last_frame = frame;
        s_last_frame_ms = now;
        s_hang_reported = false;
        return;
    }
    if (s_hang_reported || now - s_last_frame_ms < s_hang_secs * 1000ull) return;
    s_hang_reported = true;
    LOG("hang", "no game frame for %u s (stuck at frame %u, field %u)", s_hang_secs, frame, vi_field_count());
    os_debug_dump_full();
    dsp_debug_dump();
    ev_debug_dump();
    if (s_hang_exit) {
        LOG("hang", "exiting (DD_HANG_EXIT)");
        ExitProcess(3);
    }
}

// called once per VI field (hw/vi.cpp)
void dbg_on_field() {
    script_field();
    hang_field();
}

void dbg_init() {
    if (const char* p = getenv("DD_SCRIPT")) load_script(p);
    s_hang_secs = getenv("DD_HANG_SECS") ? (u32)atoi(getenv("DD_HANG_SECS")) : 6;
    s_hang_exit = getenv("DD_HANG_EXIT") != nullptr;
}

// ---------------------------------------------------------------- course lap patch
// Course archives (files/Course/*.arc) are uncompressed RARCs; the course's BOL
// data starts with "0015" and holds the lap count at +0x18.
struct DiscFile { u64 off; u32 size; std::string path; };
static std::vector<DiscFile> s_course_files;
static int s_laps = -2;   // -2: not initialised, -1: off

static void load_fst() {
    u32 fst = rd32(0x80000038);
    if (!fst) return;
    u32 n = rd32(fst + 8);
    u32 strtab = fst + n * 12;
    // walk the tree keeping the directory path for each entry
    std::vector<std::pair<u32, std::string>> dirs = {{n, ""}};
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        u32 w0 = rd32(fst + i * 12), w1 = rd32(fst + i * 12 + 4), w2 = rd32(fst + i * 12 + 8);
        std::string name;
        for (u32 a = strtab + (w0 & 0xFFFFFF); rd8(a) && name.size() < 256; a++) name.push_back((char)rd8(a));
        std::string path = dirs.back().second + name;
        if (w0 >> 24) dirs.push_back({w2, path + "/"});
        else if (!_strnicmp(path.c_str(), "Course/", 7)) s_course_files.push_back({w1, w2, path});
    }
    LOG("laps", "patching %zu course files to %d laps", s_course_files.size(), s_laps);
}

// hw/di.cpp: `n` bytes of disc offset `off` were just written to guest physical address `mar`
void dbg_on_disc_read(u64 off, u32 mar, u32 n) {
    if (s_laps == -2) {
        s_laps = getenv("DD_LAPS") ? atoi(getenv("DD_LAPS")) : -1;
        if (s_laps >= 0) load_fst();
    }
    if (s_laps < 0) return;
    for (const DiscFile& f : s_course_files) {
        if (off + n <= f.off || off >= f.off + f.size) continue;
        u64 lo = std::max<u64>(off, f.off), hi = std::min<u64>(off + n, f.off + f.size);
        for (u64 p = f.off + ((lo - f.off + 31) & ~31ull); p + 0x20 <= hi; p += 32) {
            u8* d = host_ptr(0x80000000u | (u32)(mar + (p - off)));
            if (memcmp(d, "0015", 4) || d[0x18] < 1 || d[0x18] > 9) continue;
            LOG("laps", "%s: BOL at +%X, laps %u -> %d", f.path.c_str(), (u32)(p - f.off), d[0x18], s_laps);
            d[0x18] = (u8)s_laps;
        }
    }
}
