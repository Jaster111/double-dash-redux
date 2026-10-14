// Headless testing aids: scripted controller input, course lap patching and a
// hang detector.  All are off unless their environment variable is set.
//
// DD_SCRIPT=file   controller input script, one step per line (times are VI fields, ~60/s):
//                    wait N                  neutral input for N fields
//                    hold N BTN...           hold buttons / stick directions for N fields
//                    tap BTN... [xCOUNT]     4 fields pressed + 8 released, COUNT times (default 1)
//                    mark TEXT               log "[script] TEXT" with the current field / game frame
//                    shot NAME               save the next presented frame to shots/NAME.png
//                    waitframes N            wait until the game has rendered N more frames
//                    dumpram NAME            write main RAM (24 MiB) to shots/NAME.bin
//                    pads N                  report controllers 1..N as connected (default 1)
//                    until ADDR OP VAL [BTN...] [tN]
//                                            tap BTN... (none: neutral) until the u32 at ADDR (hex) OP (== or !=)
//                                            VAL (hex), at most N fields when tN is given
//                    autoshot PREFIX N       from now on save shots/PREFIX_NNN.png every N fields (autoshot off)
//                    quit [CODE]             exit the process
//                  BTN: A B X Y Z L R START UP DOWN LEFT RIGHT (d-pad) SL SR SU SD (main stick), for
//                  controller 1; BTN@P (e.g. A@2) for controller P (1-4).
//                  When the script ends the pads stay neutral.  '#' starts a comment.
// DD_LAPS=N        patch the lap count of every course (BOL header byte 0x18) as it is read from disc.
// DD_HANG_SECS=S   after the game has started rendering, report a hang when no game frame was
//                  produced for S seconds (default 6): thread backtraces and device state.
// DD_HANG_EXIT=1   exit with code 3 after reporting a hang (and 0 on `quit`).
// DD_AUTOPILOT=1   the game's CPU AI drives the players' karts (their pads are removed from gRaceInfo when a
//                  race is set up); menus and the pause menu still take the scripted/real input.
//                  DD_AUTOPILOT=win also keeps kart 0 first in the GP points (reaches the trophy ceremony).
//                  DD_AUTOPILOT=ghost (time trials) removes the pads only once the pad recorder exists and puts them
//                  back at the goal, so the AI drives but the race still records a ghost that can be saved (it
//                  replays the recorded pad, not what the AI did).
// DD_UNLOCK=1      unlock all secret karts, characters, cups, mirror mode and battle courses (gSystemRecord), and
//                  the staff ghosts of all courses in time trials.
// Elsewhere: DD_PROF=1 sampling profiler (host/prof.cpp), DD_DBG_SLOW=ms extra time per game frame and
// DD_NO_SHADER_CACHE=1 (gx/render.cpp), DD_60HZ=0 50 Hz only (hw/exi.cpp).  Most user
// settings live in settings.ini (host/config.cpp); their DD_* variables override it.  [hitch] log lines report VI
// fields that repeated the previous frame while the game was rendering every field (visible stutter).
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

struct PortInput { u16 buttons = 0; int sx = 0, sy = 0; };
struct ScriptInput { PortInput port[4]; };
struct Step {
    enum Kind { Wait, Hold, Tap, Mark, Shot, WaitFrames, DumpRam, Pads, Until, AutoShot, Quit } kind;
    u32 n = 0;
    ScriptInput in;
    std::string text;
    u32 addr = 0, val = 0, timeout = 0;
    bool ne = false;
};

static std::vector<Step> s_steps;
static size_t s_pc;
static u32 s_step_start_field, s_step_start_frame;
static bool s_step_started;
static ScriptInput s_cur;
static bool s_script_on;
static u32 s_script_pads = 1;
static int s_script_line_err;
static std::string s_autoshot;
static u32 s_autoshot_every, s_autoshot_n;

static bool parse_button(std::string w, ScriptInput& si) {
    int port = 0;
    if (size_t at = w.find('@'); at != std::string::npos) {
        port = atoi(w.c_str() + at + 1) - 1;
        if (port < 0 || port > 3) return false;
        w.resize(at);
    }
    PortInput& in = si.port[port];
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
        else if (cmd == "pads") { st.kind = Step::Pads; is >> st.n; if (st.n < 1 || st.n > 4) bad(); }
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
        } else if (cmd == "mark" || cmd == "shot" || cmd == "dumpram") {
            st.kind = cmd == "mark" ? Step::Mark : cmd == "shot" ? Step::Shot : Step::DumpRam;
            std::getline(is >> std::ws, st.text);
            while (!st.text.empty() && isspace((u8)st.text.back())) st.text.pop_back();
        } else if (cmd == "until") {
            st.kind = Step::Until;
            std::string op;
            is >> std::hex >> st.addr >> op >> st.val >> std::dec;
            if (op != "==" && op != "!=") bad();
            st.ne = op == "!=";
            while (is >> w) {
                if ((w[0] == 't' || w[0] == 'T') && w.size() > 1 && isdigit((u8)w[1])) st.timeout = atoi(w.c_str() + 1);
                else if (!parse_button(w, st.in)) bad();
            }
        } else if (cmd == "autoshot") {
            st.kind = Step::AutoShot;
            is >> st.text >> st.n;
            if (st.text != "off" && !st.n) bad();
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
    if (s_autoshot_every && field % s_autoshot_every == 0 && !gx_shot_pending() &&
        !(s_pc < s_steps.size() && s_steps[s_pc].kind == Step::Shot)) {
        char p[256];
        snprintf(p, sizeof p, "shots/%s_%03u.png", s_autoshot.c_str(), s_autoshot_n++);
        CreateDirectoryA("shots", nullptr);
        gx_request_shot(p);
    }
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
        case Step::DumpRam: {
            CreateDirectoryA("shots", nullptr);
            // snapshot now, write on a worker thread so the game does not stall
            struct Dump { std::string path; std::vector<u8> ram; };
            Dump* d = new Dump{"shots/" + st.text + ".bin", std::vector<u8>(host_ptr(0x80000000), host_ptr(0x80000000) + 0x01800000)};
            LOG("script", "dumpram %s (field %u, game frame %u)", d->path.c_str(), field, frame);
            CloseHandle(CreateThread(nullptr, 0, [](void* p) -> DWORD {
                Dump* d = (Dump*)p;
                if (FILE* f = fopen(d->path.c_str(), "wb")) { fwrite(d->ram.data(), 1, d->ram.size(), f); fclose(f); }
                delete d;
                return 0;
            }, d, 0, nullptr));
            done = true;
            break;
        }
        case Step::Pads:
            s_script_pads = st.n;
            done = true;
            break;
        case Step::Until:
            if ((rd32(st.addr) == st.val) != st.ne) {
                LOG("script", "until %08X %s %X: reached after %u fields (field %u, game frame %u)", st.addr,
                    st.ne ? "!=" : "==", st.val, el, field, frame);
                done = true;
            } else if (st.timeout && el >= st.timeout) {
                LOG("script", "until %08X %s %X: TIMEOUT after %u fields (value %X, field %u)", st.addr,
                    st.ne ? "!=" : "==", st.val, el, rd32(st.addr), field);
                done = true;
            } else if (el % 12 < 4) s_cur = st.in;
            break;
        case Step::AutoShot:
            s_autoshot = st.text;
            s_autoshot_every = st.text == "off" ? 0 : st.n;
            s_autoshot_n = 0;
            LOG("script", "autoshot %s every %u fields (field %u)", st.text.c_str(), s_autoshot_every, field);
            done = true;
            break;
        case Step::Quit:
            LOG("script", "quit (field %u, game frame %u)", field, frame);
            void gx_wait_screenshots();   // gx/render.cpp: PNGs are written on worker threads
            gx_wait_screenshots();
            host_shutdown();
            ExitProcess(st.n);
        }
        if (!done) return;
        s_pc++;
        s_step_started = false;
        if (s_pc == s_steps.size()) LOG("script", "finished (field %u)", field);
    }
}

bool dbg_script_active() { return s_script_on; }

// called by host_get_pad: merge the scripted input; true if the script connects this port
bool dbg_script_pad(int port, u16& buttons, u8& sx, u8& sy) {
    if (!s_script_on || port < 0 || port >= (int)s_script_pads) return false;
    const PortInput& in = s_cur.port[port];
    buttons |= in.buttons;
    if (in.sx) sx = (u8)(128 + in.sx * 100);
    if (in.sy) sy = (u8)(128 + in.sy * 100);
    return true;
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

// ---------------------------------------------------------------- autopilot
// gRaceInfo (GM4P01): KartInfo[8] at 0x803BB2AC, stride 0x18 = {kartDB, pad0, charDB0, pad1, charDB1, ?};
// CPU karts have null pads.  Nulling the players' pads before the race scene is built hands them to the AI
// (the menus set them during character selection, long before the race loads).  The GP points live in an
// s32[8] by kart at 0x803BAFEC (copied to gRaceInfo+0x100 at every race setup); "win" keeps kart 0 more than
// one race win (10) ahead, so it stays first even when the game ranks the frame it adds a race's points.
static bool s_autopilot, s_autopilot_win, s_autopilot_ghost;
// only the four controller pads (static KartGamePad[4] at 0x803BF2B8, 0xB8 each); ghosts and replays have pads
// of their own on the heap and must keep them
static bool is_controller_pad(u32 pad) { return pad >= 0x803BF2B8 && pad < 0x803BF2B8 + 4 * 0xB8; }
static u32 s_ghost_pads[8][2];
// DD_AUTOPILOT=ghost: give the pads back once kart 0 has a finish time (gRaceInfo+0x138, 5999999 = none), so
// RaceApp::ctrlRecorder finalizes the record through them on its next frame
static void ghost_restore_field() {
    bool any = false;
    for (auto& k : s_ghost_pads) any |= k[0] || k[1];
    u32 t = rd32(0x803BB27C + 0x138);
    if (!any || t == 5999999 || t == 0) return;
    for (u32 k = 0; k < 8; k++)
        for (u32 d = 0; d < 2; d++)
            if (s_ghost_pads[k][d]) {
                wr32(0x803BB2B0 + k * 0x18 + d * 8, s_ghost_pads[k][d]);
                LOG("auto", "kart %u driver %u: pad %08X back at the goal (time %u, field %u)", k, d, s_ghost_pads[k][d], t,
                    vi_field_count());
                s_ghost_pads[k][d] = 0;
            }
}
static void autopilot_field() {
    if (!s_autopilot) return;
    // race mode (gRaceInfo+8): 1 time trial, 2 GP, 3 versus, 4-7 battles.  Battle arenas have no CPU
    // routes (the game crashes when the AI drives there), so battles stay under player control.
    u32 mode = rd32(0x803BB284);
    if (mode >= 4 && mode <= 7) return;
    if (s_autopilot_ghost && mode == 1) { ghost_restore_field(); return; }   // HLE_RaceMgr_ct removes them
    for (u32 k = 0; k < 8; k++)
        for (u32 d = 0; d < 2; d++) {
            u32 a = 0x803BB2B0 + k * 0x18 + d * 8, pad = rd32(a);
            if (!is_controller_pad(pad)) continue;
            LOG("auto", "kart %u driver %u: pad %08X -> COM (mode %u, field %u)", k, d, pad, mode, vi_field_count());
            wr32(a, 0);
        }
    if (s_autopilot_win) {
        s32 best = 0;
        for (u32 k = 1; k < 8; k++) best = std::max(best, (s32)rd32(0x803BAFEC + k * 4));
        if ((s32)rd32(0x803BAFEC) <= best + 10) wr32(0x803BAFEC, (u32)(best + 11));
    }
}

// ---------------------------------------------------------------- unlock everything
// gSystemRecord (GM4P01 0x803C5FE0, symbols from the mkdd decomp's Release_eu map): {s32 options; u16 secretKart;
// u16 gameFlag; ...; char defaultName[4] at +0x0B}.  gameFlag bits: 0 Toad+Toadette, 1 Petey+King Boo, 2 mirror,
// 3 Special Cup, 4 All-Cup Tour, 5/6 (special) ending, 7/8 Luigi's Mansion / Tilt-A-Kart; secretKart bits 0-12.
// The flags are re-applied every field (the card load and SystemRecord::init overwrite them), but only while
// the record looks initialised and unencrypted (default name NUL-terminated after 3 letters).
// The staff ghosts: RaceApp::msStaffGhostFlag (u16 at 0x803D5B64, bit = SystemRecord::convCourseID), set when a
// time trial ends within 3 s of the staff time; it is not saved.
static bool s_unlock;
static void unlock_field() {
    if (!s_unlock) return;
    if (rd16(0x803D5B64) != 0xFFFF) wr16(0x803D5B64, 0xFFFF);
    const u32 rec = 0x803C5FE0;
    if (!rd8(rec + 0x0B) || rd8(rec + 0x0E)) return;
    u16 karts = rd16(rec + 4), flags = rd16(rec + 6);
    if (karts == 0x1FFF && (flags & 0x19F) == 0x19F) return;
    LOG("unlock", "gSystemRecord karts %04X flags %04X -> unlocked (field %u)", karts, flags, vi_field_count());
    wr16(rec + 4, 0x1FFF);
    wr16(rec + 6, flags | 0x19F);
}

// ---------------------------------------------------------------- coverage log
// Logs "[course]" whenever the loaded course (ResMgr::msCourseID, 0x803D5B84) or the race setup (gRaceInfo mode
// +0x08, cup +0x0C, level +0x10, mirror +0x2C) changes, so a test run shows which courses and modes it went through.
static const char* course_name(u32 id) {
    static const char* names[] = {
        "Baby Park", "Peach Beach", "Daisy Cruiser", "Luigi Circuit", "Mario Circuit", "Yoshi Circuit", "?27",
        "Mushroom Bridge", "Mushroom City", "Waluigi Stadium", "Wario Colosseum", "Dino Dino Jungle", "DK Mountain",
        "?2E", "Bowser's Castle", "?30", "Rainbow Road", "Dry Dry Desert", "Sherbet Land",
        // battle arenas (Course/MiniN.arc; Mini4 and Mini6 are not in the stage select)
        "Luigi's Mansion", "Nintendo GameCube", "Block City", "Mini4", "Tilt-A-Kart", "Mini6", "Cookie Land", "Pipe Plaza",
    };
    if (id >= 0x21 && id < 0x21 + sizeof names / sizeof *names) return names[id - 0x21];
    return id == 0x44 ? "Award" : id == 0x45 ? "Ending" : "?";
}
static void coverage_field() {
    static u32 last[5] = {~0u, ~0u, ~0u, ~0u, ~0u};
    u32 cur[5] = {rd32(0x803D5B84), rd32(0x803BB284), rd32(0x803BB288), rd32(0x803BB28C), rd8(0x803BB2A8)};
    if (!memcmp(cur, last, sizeof cur)) return;
    memcpy(last, cur, sizeof cur);
    // ERaceMode; Shine Thief is ESCAPE_BATTLE (7), ROBBERY_BATTLE (5) is not in the menus
    static const char* modes[] = {"-", "time trial", "GP", "versus", "balloon", "robbery", "bob-omb", "shine thief",
                                  "award", "staff roll"};
    LOG("course", "course %02X %s | mode %s cup %d level %d%s (field %u)", cur[0], course_name(cur[0]),
        cur[1] < 10 ? modes[cur[1]] : "?", (s32)cur[2], (s32)cur[3], cur[4] ? " MIRROR" : "", vi_field_count());
}

// RaceMgr::RaceMgr(RaceInfo*) (GM4P01 0x80186824, hle_functions.txt) builds the karts.  RaceApp has created the
// pad recorder's records just before (only for karts with pads), so removing the pads here gives the karts to
// the AI and still records the player's pad.  RaceMgr reads gRaceInfo live (a kart without a pad is driven by
// the AI every frame), so the pads stay off until the goal (ghost_restore_field).
void orig_80186824(CPU* c);
void HLE_RaceMgr_ct(CPU* c) {
    if (s_autopilot_ghost && rd32(0x803BB284) == 1) {
        for (u32 k = 0; k < 8; k++)
            for (u32 d = 0; d < 2; d++) {
                u32 a = 0x803BB2B0 + k * 0x18 + d * 8;
                s_ghost_pads[k][d] = 0;
                if (!is_controller_pad(rd32(a))) continue;
                s_ghost_pads[k][d] = rd32(a);
                wr32(a, 0);
                LOG("auto", "kart %u driver %u: pad %08X -> COM until the goal", k, d, s_ghost_pads[k][d]);
            }
        wr32(0x803BB27C + 0x138, 5999999);   // kart 0's finish time: a retry still has the previous one
    }
    orig_80186824(c);
}

// RaceApp::ctrlRecorder (0x801CDE4C) finalizes a kart's record at the goal through the kart's pad, so it sees the
// pads while the AI keeps driving everywhere else
void orig_801CDE4C(CPU* c);
void HLE_RaceApp_ctrlRecorder(CPU* c) {
    for (u32 k = 0; k < 8; k++)
        for (u32 d = 0; d < 2; d++)
            if (s_ghost_pads[k][d]) wr32(0x803BB2B0 + k * 0x18 + d * 8, s_ghost_pads[k][d]);
    orig_801CDE4C(c);
    for (u32 k = 0; k < 8; k++)
        for (u32 d = 0; d < 2; d++)
            if (s_ghost_pads[k][d]) wr32(0x803BB2B0 + k * 0x18 + d * 8, 0);
}

// called once per VI field (hw/vi.cpp)
void dbg_on_field() {
    coverage_field();
    unlock_field();
    autopilot_field();
    script_field();
    hang_field();
}

void dbg_init() {
    if (const char* p = getenv("DD_SCRIPT")) load_script(p);
    s_hang_secs = getenv("DD_HANG_SECS") ? (u32)atoi(getenv("DD_HANG_SECS")) : 6;
    s_hang_exit = getenv("DD_HANG_EXIT") != nullptr;
    s_unlock = getenv("DD_UNLOCK") && atoi(getenv("DD_UNLOCK"));
    if (const char* a = getenv("DD_AUTOPILOT")) {
        s_autopilot = true;
        s_autopilot_win = !_stricmp(a, "win");
        s_autopilot_ghost = !_stricmp(a, "ghost");
    }
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
