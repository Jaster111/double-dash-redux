// The launcher: a settings window shown before the game boots.  Drawn with Direct2D / DirectWrite; the art
// (title screen illustration, menu star pattern) is read from the user's disc image, so nothing of the game
// ships with the port.  Every change is written to settings.ini at once (cfg_set); Play continues the boot.
//
// Shown unless launcher.show = 0 (Shift held at start or --launcher shows it anyway), --play is given, or a
// test run (DD_SCRIPT, DD_DISC_NOUI, DD_LAUNCHER=0).  DD_LAUNCHER_SHOT=file.png renders one frame to a PNG
// and exits (DD_LAUNCHER_TAB / DD_LAUNCHER_ROW pick the page and row), for checking the look headlessly.
//
// Input: mouse; keyboard (arrows / WASD, Q/E or Tab for pages, Space to change, Enter to play, Esc to quit);
// XInput controllers (d-pad / stick, LB/RB pages, A change, Start play, B quit).
#include "../common.h"
#include "../gx/texture.h"
#include "disc_image.h"
#include <windows.h>
#include <windowsx.h>
#include <d2d1.h>
#include <dwrite.h>
#include <wincodec.h>
#include <xinput.h>
#include <shellapi.h>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

std::string disc_find(int argc, char** argv, std::string* refused);   // game_disc.cpp
std::string disc_check(const std::string& path);
std::string disc_browse(void* owner);

namespace {

template <class T> void release(T*& p) { if (p) { p->Release(); p = nullptr; } }
u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    if (n > 1) MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], n);
    return w;
}

// ---------------------------------------------------------------- art from the disc
// a file of the disc's file system (FST), by path
bool disc_file(DiscImage* d, const char* want, std::vector<u8>& out) {
    u8 h[0x440];
    if (!d->read(0, h, sizeof h)) return false;
    u32 fst = be32(h + 0x424), size = be32(h + 0x428);
    if (size < 12 || size > 0x400000) return false;
    std::vector<u8> t(size + 1);
    if (!d->read(fst, t.data(), size)) return false;
    u32 n = be32(&t[8]);
    if (!n || (u64)n * 12 >= size) return false;
    const char* strs = (const char*)&t[n * 12];
    u32 strs_len = size - n * 12;
    std::vector<std::pair<u32, std::string>> dirs = {{n, ""}};   // (end entry, path) of the open directories
    for (u32 i = 1; i < n; i++) {
        while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
        const u8* e = &t[i * 12];
        u32 noff = be32(e) & 0xFFFFFF;
        if (noff >= strs_len) return false;
        std::string path = dirs.back().second + (strs + noff);
        if (e[0]) dirs.push_back({be32(e + 8), path + "/"});
        else if (!_stricmp(path.c_str(), want)) {
            out.resize(be32(e + 8));
            return d->read(be32(e + 4), out.data(), (u32)out.size());
        }
    }
    return false;
}

std::vector<u8> yaz0(const std::vector<u8>& in) {
    if (in.size() < 16 || memcmp(in.data(), "Yaz0", 4)) return in;
    u32 size = be32(&in[4]);
    std::vector<u8> out;
    out.reserve(size);
    size_t i = 16;
    while (out.size() < size && i < in.size()) {
        u8 code = in[i++];
        for (int b = 0; b < 8 && out.size() < size; b++) {
            if (code & (0x80 >> b)) {
                if (i >= in.size()) return out;
                out.push_back(in[i++]);
                continue;
            }
            if (i + 1 >= in.size()) return out;
            u32 b1 = in[i], b2 = in[i + 1];
            i += 2;
            u32 dist = ((b1 & 15) << 8 | b2) + 1, len = b1 >> 4;
            if (len == 0) {
                if (i >= in.size()) return out;
                len = in[i++] + 0x12;
            } else
                len += 2;
            if (dist > out.size()) return out;
            for (u32 k = 0; k < len; k++) out.push_back(out[out.size() - dist]);
        }
    }
    return out;
}

// a file of a RARC archive, by path inside it
bool rarc_file(const std::vector<u8>& d, const char* want, std::vector<u8>& out) {
    if (d.size() < 0x40 || memcmp(d.data(), "RARC", 4)) return false;
    u32 data_off = be32(&d[0x0C]) + 0x20, nnodes = be32(&d[0x20]), node_off = be32(&d[0x24]) + 0x20;
    u32 nent = be32(&d[0x28]), ent_off = be32(&d[0x2C]) + 0x20, str_off = be32(&d[0x34]) + 0x20;
    auto name = [&](u32 o) -> std::string {
        std::string s;
        for (u32 a = str_off + o; a < d.size() && d[a] && s.size() < 256; a++) s += (char)d[a];
        return s;
    };
    std::vector<std::pair<u32, std::string>> todo = {{0, ""}};
    for (int guard = 0; !todo.empty() && guard < 4096; guard++) {
        auto [node, path] = todo.back();
        todo.pop_back();
        if (node >= nnodes || node_off + node * 16 + 16 > d.size()) return false;
        const u8* nd = &d[node_off + node * 16];
        u32 count = be16(nd + 10), first = be32(nd + 12);
        for (u32 k = 0; k < count; k++) {
            if (first + k >= nent || ent_off + (first + k) * 20 + 20 > d.size()) return false;
            const u8* e = &d[ent_off + (first + k) * 20];
            u32 w = be32(e + 4), a = be32(e + 8), s = be32(e + 12);
            std::string nm = name(w & 0xFFFFFF);
            if ((w >> 24) & 2) {
                if (nm != "." && nm != "..") todo.push_back({a, path + nm + "/"});
            } else if (!_stricmp((path + nm).c_str(), want)) {
                if ((u64)data_off + a + s > d.size()) return false;
                out.assign(d.begin() + data_off + a, d.begin() + data_off + a + s);
                return true;
            }
        }
    }
    return false;
}

struct Image { u32 w = 0, h = 0; std::vector<u32> bgra; };   // premultiplied B8G8R8A8

bool decode_bti(const std::vector<u8>& b, Image& img) {
    if (b.size() < 0x20) return false;
    u32 fmt = b[0], w = be16(&b[2]), h = be16(&b[4]), tlut_fmt = b[9], ntlut = be16(&b[0x0A]);
    u32 tlut_off = be32(&b[0x0C]), img_off = be32(&b[0x1C]);
    if (!w || !h || w > 1024 || h > 1024) return false;
    if ((u64)img_off + gx_tex_size_bytes(fmt, w, h) > b.size()) return false;
    if (gx_tex_is_paletted(fmt) && (u64)tlut_off + ntlut * 2 > b.size()) return false;
    std::vector<u32> rgba(w * h);
    gx_decode_texture(fmt, w, h, &b[img_off], gx_tex_is_paletted(fmt) ? &b[tlut_off] : nullptr, tlut_fmt, rgba.data());
    img.w = w; img.h = h;
    img.bgra.resize(w * h);
    for (u32 i = 0; i < w * h; i++) {
        u32 c = rgba[i], r = c & 255, g = (c >> 8) & 255, bl = (c >> 16) & 255, a = c >> 24;
        img.bgra[i] = (bl * a / 255) | ((g * a / 255) << 8) | ((r * a / 255) << 16) | (a << 24);
    }
    return true;
}

bool load_art(DiscImage* d, const char* arc, const char* file, Image& img) {
    std::vector<u8> a, f;
    return disc_file(d, arc, a) && rarc_file(yaz0(a), file, f) && decode_bti(f, img);
}

// ---------------------------------------------------------------- settings model
enum Kind { TOGGLE, CHOICE, SLIDER, ACTION };
enum Tab { VIDEO, AUDIO, CONTROLS, SYSTEM, NTABS };
const wchar_t* const kTabNames[NTABS] = {L"VIDEO", L"AUDIO", L"CONTROLS", L"SYSTEM"};

struct Choice { std::wstring label; std::string value; };
struct Item {
    Tab tab;
    const wchar_t* label;
    const wchar_t* desc;
    Kind kind;
    const char* key;
    std::vector<Choice> choices;   // CHOICE (and TOGGLE: off, on)
    int lo = 0, hi = 100, step = 10;
    int cur = 0;                   // choice index, toggle 0/1, slider value
    bool invert = false;           // TOGGLE stored inverted (audio.mute)
    int id = 0;                    // special handling
};
enum { ID_NONE, ID_WINSIZE, ID_ASPECT, ID_SCALE, ID_DISC, ID_FOLDER };

// ---------------------------------------------------------------- state
struct Hit { D2D1_RECT_F r; int what, index; };
enum { H_TAB = 1, H_ROW, H_LEFT, H_RIGHT, H_SLIDER, H_PLAY, H_QUIT, H_CLOSE, H_MIN, H_DISC };

const float W = 1120, H = 700;   // layout size in DIPs

struct Launcher {
    int argc;
    char** argv;
    HWND wnd = nullptr;
    float scale = 1;               // DIPs -> pixels
    ID2D1Factory* d2d = nullptr;
    IDWriteFactory* dw = nullptr;
    ID2D1RenderTarget* rt = nullptr;
    ID2D1SolidColorBrush* brush = nullptr;
    ID2D1PathGeometry* tri = nullptr;   // unit right-pointing triangle
    ID2D1Bitmap* art = nullptr;
    ID2D1Bitmap* star = nullptr;
    ID2D1BitmapBrush* star_brush = nullptr;
    IDWriteTextFormat *f_title = nullptr, *f_tab = nullptr, *f_label = nullptr, *f_value = nullptr, *f_desc = nullptr,
                      *f_small = nullptr, *f_button = nullptr, *f_path = nullptr, *f_logo = nullptr, *f_logo2 = nullptr;
    Image art_img, star_img;

    std::vector<Item> items;
    int tab = VIDEO;
    int row[NTABS] = {};
    float sel_y = -1;              // animated highlight position
    float tab_x = -1;
    std::vector<Hit> hits;
    int hover_what = 0, hover_index = -1;
    int drag_slider = -1;
    bool pad_mode = false;         // last input from a controller (hints show its buttons)
    double t0 = 0, now = 0;

    std::string disc;              // valid disc image path, or ""
    std::string disc_problem;      // why the configured / chosen image is not usable
    bool done = false, play = false;

    // ------------------------------------------------------------ model
    void build() {
        auto toggle = [&](Tab t, const wchar_t* label, const wchar_t* desc, const char* key, bool def, bool invert = false) {
            Item it{t, label, desc, TOGGLE, key};
            it.invert = invert;
            bool v = cfg_bool(key, def);
            it.cur = (invert ? !v : v) ? 1 : 0;
            items.push_back(it);
        };
        auto choice = [&](Tab t, const wchar_t* label, const wchar_t* desc, const char* key, std::vector<Choice> ch,
                          const char* def, int id = ID_NONE) {
            Item it{t, label, desc, CHOICE, key, ch};
            it.id = id;
            const char* v = cfg_str(key);
            std::string cur = v && *v ? v : def;
            for (size_t i = 0; i < ch.size(); i++)
                if (ch[i].value == cur) it.cur = (int)i;
            items.push_back(it);
        };
        auto slider = [&](Tab t, const wchar_t* label, const wchar_t* desc, const char* key, int lo, int hi, int step, int def) {
            Item it{t, label, desc, SLIDER, key};
            it.lo = lo; it.hi = hi; it.step = step;
            it.cur = std::clamp(cfg_int(key, def), lo, hi);
            items.push_back(it);
        };
        auto action = [&](Tab t, const wchar_t* label, const wchar_t* desc, int id) {
            Item it{t, label, desc, ACTION, nullptr};
            it.id = id;
            items.push_back(it);
        };
        choice(VIDEO, L"Display", L"Fullscreen is borderless, on the display the window is on. Alt+Enter or F11 "
               L"switches while playing.", "video.fullscreen", {{L"Window", "0"}, {L"Fullscreen", "1"}}, "0");
        choice(VIDEO, L"Window size", L"Size of the game window when it is not fullscreen.", "video.window_width", {}, "",
               ID_WINSIZE);
        choice(VIDEO, L"Aspect ratio", L"16:9 uses the “16:9 Widescreen v2” code by Ralf and Kester: races show a "
               L"wider view with the HUD at the screen edges. The opening movie stays 4:3.", "video.widescreen",
               {{L"4:3  Original", "0"}, {L"16:9  Widescreen", "1"}}, "0", ID_ASPECT);
        std::vector<Choice> scales;
        for (int s = 1; s <= 8; s++) scales.push_back({L"", std::to_string(s)});
        choice(VIDEO, L"Resolution", L"How sharply the 3D is drawn, as a multiple of the GameCube's 640 × 528. "
               L"Higher settings need a faster graphics card; 3× suits most screens.", "video.internal_scale", scales,
               "3", ID_SCALE);
        choice(VIDEO, L"Refresh rate", L"60 Hz (PAL60) is smoother than the original 50 Hz. The game confirms the "
               L"choice when it boots.", "video.pal60", {{L"50 Hz", "0"}, {L"60 Hz", "1"}}, "1");
        toggle(VIDEO, L"Distance blur", L"The courses blur faraway scenery, and some add a heat shimmer, as on the "
               L"console. Turn it off to keep everything sharp.", "video.depth_blur", true);
        toggle(AUDIO, L"Sound", L"Turn off for no sound output at all.", "audio.mute", false, true);
        slider(AUDIO, L"Volume", L"Master volume.", "audio.volume", 0, 100, 5, 100);
        toggle(CONTROLS, L"Rumble", L"Controller vibration (Xbox-style controllers).", "input.rumble", true);
        slider(CONTROLS, L"Rumble strength", L"How strongly the controllers vibrate.", "input.rumble_strength", 10, 100, 10, 100);
        action(SYSTEM, L"Game disc", L"Your disc image of Mario Kart: Double Dash!! (Europe, GM4P01): .iso, .gcm or "
               L"Dolphin .rvz.", ID_DISC);
        toggle(SYSTEM, L"Show launcher", L"Off: the game starts directly. Hold Shift while starting it to see this "
               L"window again.", "launcher.show", true);
        action(SYSTEM, L"Data folder", L"Opens the folder with settings.ini, the memory card saves and DoubleDashRedux.log.",
               ID_FOLDER);
        update_winsizes(false);
    }

    Item* find(int id) {
        for (auto& it : items) if (it.id == id) return &it;
        return nullptr;
    }
    bool wide() { Item* a = find(ID_ASPECT); return a && a->cur == 1; }

    // window sizes that suit the aspect ratio and fit the screen; keeps the current size when it is custom
    void update_winsizes(bool aspect_changed) {
        Item* it = find(ID_WINSIZE);
        int cw = cfg_int("video.window_width", 1280), ch = cfg_int("video.window_height", 960);
        static const int k43[][2] = {{800, 600}, {960, 720}, {1280, 960}, {1440, 1080}, {1600, 1200}, {1920, 1440}, {2400, 1800}};
        static const int k169[][2] = {{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440}, {3200, 1800}, {3840, 2160}};
        int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
        std::vector<std::pair<int, int>> sizes;
        auto add = [&](const int (*t)[2], int n) {
            for (int i = 0; i < n; i++)
                if (t[i][0] <= sw && t[i][1] <= sh) sizes.push_back({t[i][0], t[i][1]});
        };
        if (wide()) add(k169, 6); else add(k43, 7);
        if (sizes.empty()) sizes.push_back(wide() ? std::make_pair(1280, 720) : std::make_pair(800, 600));
        if (aspect_changed) {   // the size closest in height in the new list
            auto best = sizes[0];
            for (auto& s : sizes) if (abs(s.second - ch) < abs(best.second - ch)) best = s;
            cw = best.first; ch = best.second;
            set_winsize(cw, ch);
        }
        it->choices.clear();
        it->cur = -1;
        for (auto& s : sizes) {
            if (s.first == cw && s.second == ch) it->cur = (int)it->choices.size();
            it->choices.push_back({std::to_wstring(s.first) + L" × " + std::to_wstring(s.second),
                                   std::to_string(s.first) + "x" + std::to_string(s.second)});
        }
        if (it->cur < 0) {   // a size set by hand in settings.ini
            it->cur = (int)it->choices.size();
            it->choices.push_back({std::to_wstring(cw) + L" × " + std::to_wstring(ch) + L" (custom)",
                                   std::to_string(cw) + "x" + std::to_string(ch)});
        }
    }
    void set_winsize(int w, int h) {
        cfg_set("video.window_width", std::to_string(w).c_str());
        cfg_set("video.window_height", std::to_string(h).c_str());
    }

    std::wstring value_text(Item& it) {
        if (it.kind == SLIDER) return std::to_wstring(it.cur) + L"%";
        if (it.kind == TOGGLE) return it.cur ? L"ON" : L"OFF";
        if (it.id == ID_SCALE) {
            int s = it.cur + 1, sx = wide() ? (s * 4 + 2) / 3 : s;
            return std::to_wstring(s) + L"×   " + std::to_wstring(640 * sx) + L" × " + std::to_wstring(528 * s);
        }
        if (it.id == ID_DISC) {
            if (disc.empty()) return L"Select…";
            return widen(disc);
        }
        if (it.id == ID_FOLDER) {
            wchar_t dir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, dir);
            return dir;
        }
        return it.cur >= 0 && it.cur < (int)it.choices.size() ? it.choices[it.cur].label : L"";
    }

    void store(Item& it) {
        if (it.kind == TOGGLE) cfg_set(it.key, (it.invert ? !it.cur : it.cur) ? "1" : "0");
        else if (it.kind == SLIDER) cfg_set(it.key, std::to_string(it.cur).c_str());
        else if (it.id == ID_WINSIZE) {
            int w = 0, h = 0;
            if (sscanf(it.choices[it.cur].value.c_str(), "%dx%d", &w, &h) == 2) set_winsize(w, h);
        } else if (it.kind == CHOICE) cfg_set(it.key, it.choices[it.cur].value.c_str());
        if (it.id == ID_ASPECT) update_winsizes(true);
    }

    void change(Item& it, int d) {
        if (it.kind == TOGGLE) it.cur = d ? (d > 0) : !it.cur;
        else if (it.kind == SLIDER) it.cur = std::clamp(it.cur + (d ? d : 1) * it.step, it.lo, it.hi);
        else if (it.kind == CHOICE) {
            int n = (int)it.choices.size();
            if (!n) return;
            it.cur = d == 0 ? (it.cur + 1) % n : std::clamp(it.cur + d, 0, n - 1);
        } else {
            if (d == 0) run_action(it);
            return;
        }
        store(it);
    }

    void run_action(Item& it) {
        if (it.id == ID_DISC) browse_disc();
        else if (it.id == ID_FOLDER) {
            wchar_t dir[MAX_PATH];
            GetCurrentDirectoryW(MAX_PATH, dir);
            ShellExecuteW(wnd, L"open", dir, nullptr, nullptr, SW_SHOWNORMAL);
        }
    }

    void browse_disc() {
        std::string p = disc_browse(wnd);
        if (p.empty()) return;
        std::string why = disc_check(p);
        if (!why.empty()) { disc_problem = p + ": " + why; return; }
        cfg_set("game.disc", p.c_str());
        disc = p;
        disc_problem.clear();
        load_disc_art();
    }

    void load_disc_art() {
        std::string why;
        DiscImage* d = disc.empty() ? nullptr : disc_image_open(disc, &why);
        if (!d) return;
        Image a, s;
        if (load_art(d, "SceneData/English/title.arc", "timg/titlemariokartdd.bti", a)) art_img = a;
        if (load_art(d, "SceneData/back.arc", "timg/star1.bti", s)) star_img = s;
        delete d;
        release(art); release(star); release(star_brush);
        if (rt) create_bitmaps();
    }

    std::vector<int> rows_of(int t) {
        std::vector<int> r;
        for (size_t i = 0; i < items.size(); i++) if (items[i].tab == t) r.push_back((int)i);
        return r;
    }
    Item& selected() { auto r = rows_of(tab); return items[r[std::clamp(row[tab], 0, (int)r.size() - 1)]]; }

    // ------------------------------------------------------------ actions
    void nav(int dy) {
        int n = (int)rows_of(tab).size();
        row[tab] = std::clamp(row[tab] + dy, 0, n - 1);
    }
    void switch_tab(int d) { tab = (tab + d + NTABS) % NTABS; }
    void do_play() {
        if (disc.empty()) { browse_disc(); return; }
        play = true;
        done = true;
    }
    void do_quit() { done = true; }

    // ------------------------------------------------------------ D2D resources
    void create_text() {
        auto fmt = [&](float size, DWRITE_FONT_WEIGHT w, DWRITE_FONT_STYLE st, bool wrap = false) {
            IDWriteTextFormat* f = nullptr;
            dw->CreateTextFormat(L"Segoe UI", nullptr, w, st, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
            if (!wrap) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
            f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
            return f;
        };
        f_title = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL);
        f_tab = fmt(17, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC);
        f_label = fmt(19, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL);
        f_value = fmt(17, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL);
        f_desc = fmt(15, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, true);
        f_desc->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        f_small = fmt(14, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL);
        f_button = fmt(30, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC);
        f_path = fmt(15, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL);
        f_logo = fmt(54, DWRITE_FONT_WEIGHT_BLACK, DWRITE_FONT_STYLE_ITALIC);
        f_logo2 = fmt(30, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_ITALIC);
        // long values end in an ellipsis; paths keep their last part ("...\MKDD.rvz")
        for (IDWriteTextFormat* f : {f_value, f_small}) {
            IDWriteInlineObject* ell = nullptr;
            dw->CreateEllipsisTrimmingSign(f, &ell);
            DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
            f->SetTrimming(&tr, ell);
            release(ell);
        }
        IDWriteInlineObject* ell = nullptr;
        dw->CreateEllipsisTrimmingSign(f_path, &ell);
        DWRITE_TRIMMING tr = {DWRITE_TRIMMING_GRANULARITY_CHARACTER, L'\\', 1};
        f_path->SetTrimming(&tr, ell);
        release(ell);
    }

    void create_bitmaps() {
        auto bmp = [&](Image& img, ID2D1Bitmap** out) {
            if (!img.w) return;
            D2D1_BITMAP_PROPERTIES p = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
            rt->CreateBitmap(D2D1::SizeU(img.w, img.h), img.bgra.data(), img.w * 4, p, out);
        };
        bmp(art_img, &art);
        bmp(star_img, &star);
        if (star)
            rt->CreateBitmapBrush(star, D2D1::BitmapBrushProperties(D2D1_EXTEND_MODE_WRAP, D2D1_EXTEND_MODE_WRAP,
                                                                    D2D1_BITMAP_INTERPOLATION_MODE_LINEAR), &star_brush);
    }

    void create_target_resources() {
        rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &brush);
        create_bitmaps();
    }
    void release_target_resources() {
        release(brush); release(art); release(star); release(star_brush);
    }

    // ------------------------------------------------------------ drawing helpers
    D2D1_COLOR_F rgb(u32 c, float a = 1) { return D2D1::ColorF(((c >> 16) & 255) / 255.f, ((c >> 8) & 255) / 255.f, (c & 255) / 255.f, a); }
    ID2D1SolidColorBrush* col(u32 c, float a = 1) { brush->SetColor(rgb(c, a)); return brush; }
    ID2D1LinearGradientBrush* grad(D2D1_POINT_2F a, D2D1_POINT_2F b, std::initializer_list<std::pair<float, D2D1_COLOR_F>> stops) {
        std::vector<D2D1_GRADIENT_STOP> s;
        for (auto& p : stops) s.push_back({p.first, p.second});
        ID2D1GradientStopCollection* c = nullptr;
        rt->CreateGradientStopCollection(s.data(), (UINT32)s.size(), &c);
        ID2D1LinearGradientBrush* g = nullptr;
        if (c) rt->CreateLinearGradientBrush(D2D1::LinearGradientBrushProperties(a, b), c, &g);
        release(c);
        return g;
    }
    void fill_round(D2D1_RECT_F r, float rad, ID2D1Brush* b) { if (b) rt->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), b); }
    void fill_round_grad(D2D1_RECT_F r, float rad, u32 top, u32 bottom, float a = 1) {
        ID2D1LinearGradientBrush* g = grad({0, r.top}, {0, r.bottom}, {{0.f, rgb(top, a)}, {1.f, rgb(bottom, a)}});
        fill_round(r, rad, g);
        release(g);
    }
    void text(const std::wstring& s, IDWriteTextFormat* f, D2D1_RECT_F r, D2D1_COLOR_F c,
              DWRITE_TEXT_ALIGNMENT al = DWRITE_TEXT_ALIGNMENT_LEADING, float shadow = 0) {
        f->SetTextAlignment(al);
        if (shadow > 0) {
            brush->SetColor(D2D1::ColorF(0, 0, 0.1f, 0.55f * c.a));
            D2D1_RECT_F o = {r.left + shadow, r.top + shadow, r.right + shadow, r.bottom + shadow};
            rt->DrawText(s.c_str(), (UINT32)s.size(), f, o, brush);
        }
        brush->SetColor(c);
        rt->DrawText(s.c_str(), (UINT32)s.size(), f, r, brush);
    }
    void triangle(float cx, float cy, float size, bool left, D2D1_COLOR_F c) {
        D2D1_MATRIX_3X2_F m = D2D1::Matrix3x2F::Translation(-0.5f, -0.5f) *
                              D2D1::Matrix3x2F::Scale(left ? -size : size, size) * D2D1::Matrix3x2F::Translation(cx, cy);
        rt->SetTransform(m);
        brush->SetColor(c);
        rt->FillGeometry(tri, brush);
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
    }
    void hit(D2D1_RECT_F r, int what, int index = 0) { hits.push_back({r, what, index}); }
    bool hovered(int what, int index = 0) { return hover_what == what && hover_index == index; }
    // a controller / key glyph followed by its action, left to right from x; returns the next x
    float hint(float x, float y, const wchar_t* key, u32 key_col, const wchar_t* what, bool round) {
        IDWriteTextLayout* l = nullptr;
        dw->CreateTextLayout(key, (UINT32)wcslen(key), f_small, 200, 30, &l);
        DWRITE_TEXT_METRICS m = {};
        if (l) l->GetMetrics(&m);
        release(l);
        float kw = round ? 26 : std::max(26.f, m.width + 16);
        fill_round({x, y - 13, x + kw, y + 13}, 13, col(key_col));
        text(key, f_small, {x, y - 13, x + kw, y + 13}, rgb(round ? 0xFFFFFF : 0x101830), DWRITE_TEXT_ALIGNMENT_CENTER);
        float tx = x + kw + 8;
        dw->CreateTextLayout(what, (UINT32)wcslen(what), f_small, 400, 30, &l);
        if (l) l->GetMetrics(&m);
        release(l);
        text(what, f_small, {tx, y - 13, tx + m.width + 4, y + 13}, rgb(0xDDE6FF));
        return tx + m.width + 22;
    }

    // ------------------------------------------------------------ the frame
    void draw() {
        hits.clear();
        float t = (float)(now - t0);
        rt->BeginDraw();
        rt->SetTransform(D2D1::Matrix3x2F::Identity());
        rt->Clear(rgb(0x081440));

        // background: blue gradient, the menus' scrolling star pattern, a few diagonal light bands
        {
            ID2D1LinearGradientBrush* g = grad({0, 0}, {W * 0.35f, H}, {{0.f, rgb(0x1A47C8)}, {0.55f, rgb(0x0C2478)}, {1.f, rgb(0x060E36)}});
            rt->FillRectangle({0, 0, W, H}, g);
            release(g);
            if (star_brush) {
                star_brush->SetTransform(D2D1::Matrix3x2F::Scale(0.75f, 0.75f) *
                                         D2D1::Matrix3x2F::Translation(fmodf(t * 18, 96), fmodf(t * 9, 48)));
                star_brush->SetOpacity(0.07f);
                rt->FillRectangle({0, 0, W, H}, star_brush);
            }
            for (int i = 0; i < 3; i++) {
                ID2D1PathGeometry* p = nullptr;
                d2d->CreatePathGeometry(&p);
                ID2D1GeometrySink* s = nullptr;
                p->Open(&s);
                float x0 = 520 + i * 150.f, wdt = 46 - i * 12.f;
                s->BeginFigure({x0, H}, D2D1_FIGURE_BEGIN_FILLED);
                s->AddLine({x0 + 420, 0});
                s->AddLine({x0 + 420 + wdt, 0});
                s->AddLine({x0 + wdt, H});
                s->EndFigure(D2D1_FIGURE_END_CLOSED);
                s->Close();
                release(s);
                rt->FillGeometry(p, col(0xFFFFFF, 0.025f));
                release(p);
            }
        }

        // title bar
        text(L"DOUBLE DASH REDUX  ·  LAUNCHER", f_title, {24, 0, 600, 40}, rgb(0xFFFFFF, 0.75f));
        {
            D2D1_RECT_F mn = {W - 92, 4, W - 50, 36}, cl = {W - 46, 4, W - 4, 36};
            if (hovered(H_MIN)) fill_round(mn, 6, col(0xFFFFFF, 0.12f));
            if (hovered(H_CLOSE)) fill_round(cl, 6, col(0xE60012, 0.9f));
            rt->DrawLine({mn.left + 15, 21}, {mn.right - 15, 21}, col(0xFFFFFF, 0.85f), 1.6f);
            rt->DrawLine({cl.left + 15, 14}, {cl.right - 15, 28}, col(0xFFFFFF, 0.85f), 1.6f);
            rt->DrawLine({cl.left + 15, 28}, {cl.right - 15, 14}, col(0xFFFFFF, 0.85f), 1.6f);
            hit(mn, H_MIN);
            hit(cl, H_CLOSE);
        }

        // left: the title screen illustration (or a drawn logo before a disc is known)
        D2D1_RECT_F art_r = {24, 48, 24 + 560, 48 + 560 * 448.f / 608.f};
        {
            D2D1_RECT_F sh = {art_r.left + 6, art_r.top + 8, art_r.right + 6, art_r.bottom + 8};
            fill_round(sh, 16, col(0x000010, 0.35f));
            if (art) {
                ID2D1RoundedRectangleGeometry* clip = nullptr;
                d2d->CreateRoundedRectangleGeometry(D2D1::RoundedRect(art_r, 14, 14), &clip);
                ID2D1Layer* layer = nullptr;
                rt->CreateLayer(&layer);
                rt->PushLayer(D2D1::LayerParameters(D2D1::InfiniteRect(), clip), layer);
                rt->DrawBitmap(art, art_r, 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
                rt->PopLayer();
                release(layer);
                release(clip);
            } else {
                fill_round_grad(art_r, 14, 0x2A66E8, 0x0B2E8C);
                text(L"MARIO KART", f_logo, {art_r.left, art_r.top + 110, art_r.right, art_r.top + 190}, rgb(0xFFD200),
                     DWRITE_TEXT_ALIGNMENT_CENTER, 4);
                text(L"Double Dash!!", f_logo2, {art_r.left, art_r.top + 190, art_r.right, art_r.top + 240},
                     rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER, 3);
            }
            rt->DrawRoundedRectangle(D2D1::RoundedRect(art_r, 14, 14), col(0xFFFFFF, 0.9f), 3);
        }

        // left, below: the disc
        {
            D2D1_RECT_F card = {24, art_r.bottom + 18, 24 + 560, 606};
            bool hov = hovered(H_DISC);
            fill_round(card, 12, col(0x061038, hov ? 0.75f : 0.6f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(card, 12, 12), col(0xFFFFFF, hov ? 0.35f : 0.15f), 1.5f);
            hit(card, H_DISC);
            u32 dot = !disc.empty() ? 0x35D05A : disc_problem.empty() ? 0xFFC400 : 0xFF4A3D;
            float cy = card.top + 30;
            rt->FillEllipse(D2D1::Ellipse({card.left + 24, cy}, 7, 7), col(dot));
            rt->FillEllipse(D2D1::Ellipse({card.left + 24, cy}, 12, 12), col(dot, 0.25f + 0.1f * sinf(t * 3)));
            std::wstring head = !disc.empty() ? L"Mario Kart: Double Dash!!  (Europe)" : L"No game disc selected";
            text(head, f_label, {card.left + 46, cy - 16, card.right - 120, cy + 16}, rgb(0xFFFFFF));
            if (!disc.empty()) {   // the image format, after the name
                IDWriteTextLayout* l = nullptr;
                dw->CreateTextLayout(head.c_str(), (UINT32)head.size(), f_label, 1000, 40, &l);
                DWRITE_TEXT_METRICS m = {};
                if (l) l->GetMetrics(&m);
                release(l);
                std::string ext = disc.size() > 4 ? disc.substr(disc.size() - 4) : "";
                const wchar_t* tag = !_stricmp(ext.c_str(), ".rvz") ? L"RVZ" : L"ISO";
                float tx = card.left + 46 + m.width + 12;
                fill_round({tx, cy - 11, tx + 44, cy + 11}, 11, col(0xFFFFFF, 0.14f));
                text(tag, f_small, {tx, cy - 11, tx + 44, cy + 11}, rgb(0xFFFFFF, 0.85f), DWRITE_TEXT_ALIGNMENT_CENTER);
            }
            text(L"CHANGE", f_tab, {card.right - 120, cy - 16, card.right - 18, cy + 16},
                 rgb(hov ? 0xFFD200 : 0xFFFFFF, hov ? 1 : 0.6f), DWRITE_TEXT_ALIGNMENT_TRAILING);
            std::wstring sub = !disc.empty() ? widen(disc)
                             : !disc_problem.empty() ? widen(disc_problem)
                             : L"Choose your disc image: .iso, .gcm or Dolphin .rvz";
            IDWriteTextFormat* f = !disc.empty() ? f_path : f_desc;
            text(sub, f, {card.left + 46, cy + 18, card.right - 18, card.bottom - 10}, rgb(0xC8D4FF, 0.85f));
        }

        // right: the settings panel
        D2D1_RECT_F panel = {608, 48, W - 24, 606};
        fill_round(panel, 16, col(0x050C30, 0.62f));
        rt->DrawRoundedRectangle(D2D1::RoundedRect(panel, 16, 16), col(0xFFFFFF, 0.14f), 1.5f);

        // pages
        {
            float tw = (panel.right - panel.left - 24) / NTABS, ty = panel.top + 14;
            float target = panel.left + 12 + tab * tw;
            tab_x = tab_x < 0 ? target : tab_x + (target - tab_x) * 0.25f;
            ID2D1LinearGradientBrush* g = grad({0, ty}, {0, ty + 38}, {{0.f, rgb(0xFFE24A)}, {1.f, rgb(0xFFB300)}});
            fill_round({tab_x + 3, ty, tab_x + tw - 3, ty + 38}, 19, g);
            release(g);
            for (int i = 0; i < NTABS; i++) {
                D2D1_RECT_F r = {panel.left + 12 + i * tw, ty, panel.left + 12 + (i + 1) * tw, ty + 38};
                bool on = i == tab;
                text(kTabNames[i], f_tab, r, on ? rgb(0x3A1A00) : rgb(0xFFFFFF, hovered(H_TAB, i) ? 0.95f : 0.6f),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
                hit(r, H_TAB, i);
            }
        }

        // rows
        std::vector<int> rows = rows_of(tab);
        row[tab] = std::clamp(row[tab], 0, (int)rows.size() - 1);
        float y0 = panel.top + 70, rh = 52;
        {
            float target = y0 + row[tab] * (rh + 6);
            sel_y = sel_y < 0 ? target : sel_y + (target - sel_y) * 0.3f;
            D2D1_RECT_F s = {panel.left + 12, sel_y, panel.right - 12, sel_y + rh};
            ID2D1LinearGradientBrush* g = grad({s.left, 0}, {s.right, 0}, {{0.f, rgb(0xFFFFFF, 0.2f)}, {1.f, rgb(0xFFFFFF, 0.06f)}});
            fill_round(s, 12, g);
            release(g);
            fill_round({s.left, s.top + 8, s.left + 5, s.bottom - 8}, 2.5f, col(0xFF2A1F));
        }
        for (size_t k = 0; k < rows.size(); k++) {
            Item& it = items[rows[k]];
            bool sel = (int)k == row[tab];
            float y = y0 + k * (rh + 6), cy = y + rh / 2;
            D2D1_RECT_F r = {panel.left + 12, y, panel.right - 12, y + rh};
            hit(r, H_ROW, rows[k]);
            if (!sel && hovered(H_ROW, rows[k])) fill_round(r, 12, col(0xFFFFFF, 0.05f));
            text(it.label, f_label, {r.left + 22, y, r.left + 200, y + rh}, sel ? rgb(0xFFFFFF) : rgb(0xC8D4FF, 0.9f));
            float vr = r.right - 16, vl = r.left + 196;
            if (it.kind == TOGGLE) {
                float x1 = vr, x0 = vr - 64;
                D2D1_RECT_F p = {x0, cy - 15, x1, cy + 15};
                if (it.cur) fill_round_grad(p, 15, 0x5BE36F, 0x1FA33D);
                else fill_round(p, 15, col(0x2B3566));
                float kx = it.cur ? x1 - 15 : x0 + 15;
                rt->FillEllipse(D2D1::Ellipse({kx, cy}, 11, 11), col(0xFFFFFF));
                text(it.cur ? L"ON" : L"OFF", f_small, {x0 - 60, y, x0 - 10, y + rh},
                     rgb(it.cur ? 0x9DF5AA : 0x8C97C4), DWRITE_TEXT_ALIGNMENT_TRAILING);
                hit({x0, cy - 15, x1, cy + 15}, H_RIGHT, rows[k]);
            } else if (it.kind == CHOICE) {
                float al = vl, ar = vr;
                D2D1_COLOR_F ac = sel ? rgb(0xFFD200) : rgb(0xFFFFFF, 0.35f);
                bool can_l = it.cur > 0, can_r = it.cur + 1 < (int)it.choices.size();
                triangle(al + 8, cy, 13, true, can_l ? ac : rgb(0xFFFFFF, 0.12f));
                triangle(ar - 8, cy, 13, false, can_r ? ac : rgb(0xFFFFFF, 0.12f));
                text(value_text(it), f_value, {al + 22, y, ar - 22, y + rh}, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER);
                hit({al - 6, y, al + 24, y + rh}, H_LEFT, rows[k]);
                hit({ar - 24, y, ar + 6, y + rh}, H_RIGHT, rows[k]);
            } else if (it.kind == SLIDER) {
                float tx0 = vl + 4, tx1 = vr - 62;
                D2D1_RECT_F tr = {tx0, cy - 4, tx1, cy + 4};
                fill_round(tr, 4, col(0x2B3566));
                float f = (float)(it.cur - it.lo) / (it.hi - it.lo), kx = tx0 + f * (tx1 - tx0);
                ID2D1LinearGradientBrush* g = grad({tx0, 0}, {tx1, 0}, {{0.f, rgb(0xFFE24A)}, {1.f, rgb(0xFF7A00)}});
                fill_round({tx0, cy - 4, kx, cy + 4}, 4, g);
                release(g);
                rt->FillEllipse(D2D1::Ellipse({kx, cy}, sel ? 11.f : 9.f, sel ? 11.f : 9.f), col(0xFFFFFF));
                text(value_text(it), f_value, {tx1 + 8, y, vr, y + rh}, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_TRAILING);
                hit({tx0 - 12, y, tx1 + 12, y + rh}, H_SLIDER, rows[k]);
            } else {
                bool is_path = (it.id == ID_DISC && !disc.empty()) || it.id == ID_FOLDER;
                float bx0 = vr - 92;
                text(value_text(it), is_path ? f_path : f_value, {vl, y, bx0 - 10, y + rh}, rgb(0xFFFFFF, 0.85f),
                     DWRITE_TEXT_ALIGNMENT_TRAILING);
                D2D1_RECT_F b = {bx0, cy - 15, vr, cy + 15};
                fill_round(b, 15, col(sel ? 0xFFD200 : 0x2B3566));
                text(it.id == ID_DISC ? L"BROWSE" : L"OPEN", f_small, b, sel ? rgb(0x3A1A00) : rgb(0xFFFFFF, 0.8f),
                     DWRITE_TEXT_ALIGNMENT_CENTER);
                hit(b, H_RIGHT, rows[k]);
            }
        }

        // the controls reference (CONTROLS page), under its rows
        if (tab == CONTROLS) {
            float y = y0 + rows.size() * (rh + 6) + 14;
            static const wchar_t* const pad[][2] = {{L"A  B  X  Y", L"A  B  X  Y"}, {L"Left stick", L"Stick"},
                {L"D-pad", L"D-pad"}, {L"LT  /  RT", L"L  /  R"}, {L"LB  or  RB", L"Z"}, {L"Start  or  Back", L"Start"}};
            static const wchar_t* const key[][2] = {{L"Arrows / WASD", L"Stick"}, {L"Space  or  X", L"A"},
                {L"Z  or  Backspace", L"B"}, {L"C  /  V", L"X  /  Y"}, {L"Q  /  E", L"L  /  R"}, {L"Enter", L"Start"}};
            float cx = (panel.left + panel.right) / 2;
            text(L"CONTROLLER", f_tab, {panel.left + 30, y, cx, y + 28}, rgb(0xFFD200));
            text(L"KEYBOARD", f_tab, {cx + 10, y, panel.right - 20, y + 28}, rgb(0xFFD200));
            for (int i = 0; i < 6; i++) {
                float ly = y + 32 + i * 25;
                text(pad[i][0], f_small, {panel.left + 30, ly, cx - 84, ly + 24}, rgb(0xFFFFFF, 0.9f));
                text(pad[i][1], f_small, {cx - 90, ly, cx - 6, ly + 24}, rgb(0xC8D4FF, 0.7f));
                text(key[i][0], f_small, {cx + 10, ly, panel.right - 80, ly + 24}, rgb(0xFFFFFF, 0.9f));
                text(key[i][1], f_small, {panel.right - 86, ly, panel.right - 20, ly + 24}, rgb(0xC8D4FF, 0.7f));
            }
        }

        // the selected setting's description
        {
            D2D1_RECT_F d = {panel.left + 16, panel.bottom - 96, panel.right - 16, panel.bottom - 14};
            fill_round(d, 10, col(0x000820, 0.45f));
            text(selected().desc, f_desc, {d.left + 16, d.top + 12, d.right - 16, d.bottom - 8}, rgb(0xDDE6FF, 0.92f));
        }

        // bottom: hints, Quit, Play
        {
            float hy = 646;
            float x = 24;
            if (pad_mode) {
                x = hint(x, hy, L"A", 0x2FB34A, L"Change", true);
                x = hint(x, hy, L"B", 0xE23B2E, L"Quit", true);
                x = hint(x, hy, L"LB / RB", 0xDDE6FF, L"Page", false);
                x = hint(x, hy, L"START", 0xDDE6FF, L"Play", false);
            } else {
                x = hint(x, hy, L"← →", 0xDDE6FF, L"Change", false);
                x = hint(x, hy, L"Q / E", 0xDDE6FF, L"Page", false);
                x = hint(x, hy, L"Enter", 0xDDE6FF, L"Play", false);
                x = hint(x, hy, L"Esc", 0xDDE6FF, L"Quit", false);
            }
            D2D1_RECT_F q = {W - 24 - 220 - 16 - 120, 620, W - 24 - 220 - 16, 672};
            fill_round(q, 14, col(0xFFFFFF, hovered(H_QUIT) ? 0.2f : 0.1f));
            rt->DrawRoundedRectangle(D2D1::RoundedRect(q, 14, 14), col(0xFFFFFF, 0.25f), 1.5f);
            text(L"QUIT", f_tab, q, rgb(0xFFFFFF, 0.9f), DWRITE_TEXT_ALIGNMENT_CENTER);
            hit(q, H_QUIT);

            float pulse = 1 + 0.025f * (0.5f + 0.5f * sinf(t * 4));
            D2D1_RECT_F p = {W - 24 - 220, 614, W - 24, 678};
            float cx = (p.left + p.right) / 2, cy = (p.top + p.bottom) / 2;
            rt->SetTransform(D2D1::Matrix3x2F::Scale(pulse, pulse, {cx, cy}));
            bool ok = !disc.empty(), hov = hovered(H_PLAY);
            fill_round({p.left - 4, p.top - 4, p.right + 4, p.bottom + 4}, 20, col(ok ? 0xFF6A00 : 0x3C7BFF, 0.25f));
            if (ok) fill_round_grad(p, 16, hov ? 0xFF6A3A : 0xFF4B2B, hov ? 0xE0101E : 0xC8000F);
            else fill_round_grad(p, 16, 0x4C8DFF, 0x1F4FC8);
            ID2D1LinearGradientBrush* gl = grad({0, p.top}, {0, cy}, {{0.f, rgb(0xFFFFFF, 0.28f)}, {1.f, rgb(0xFFFFFF, 0)}});
            fill_round({p.left + 3, p.top + 3, p.right - 3, cy}, 13, gl);
            release(gl);
            text(ok ? L"PLAY" : L"SELECT DISC", ok ? f_button : f_tab, p, rgb(0xFFFFFF), DWRITE_TEXT_ALIGNMENT_CENTER, 2.5f);
            rt->SetTransform(D2D1::Matrix3x2F::Identity());
            hit(p, H_PLAY);
        }

        // fade in
        float fade = 1 - std::min(1.f, t / 0.25f);
        if (fade > 0) rt->FillRectangle({0, 0, W, H}, col(0x000000, fade));
        HRESULT hr = rt->EndDraw();
        if (hr == D2DERR_RECREATE_TARGET) {
            release_target_resources();
            release(rt);
        }
    }

    // ------------------------------------------------------------ input
    void key(WPARAM k) {
        pad_mode = false;
        bool shift = GetKeyState(VK_SHIFT) < 0;
        switch (k) {
        case VK_UP: case 'W': nav(-1); break;
        case VK_DOWN: case 'S': nav(1); break;
        case VK_LEFT: case 'A': change(selected(), -1); break;
        case VK_RIGHT: case 'D': change(selected(), 1); break;
        case VK_SPACE: change(selected(), 0); break;
        case 'Q': case VK_PRIOR: switch_tab(-1); break;
        case 'E': case VK_NEXT: switch_tab(1); break;
        case VK_TAB: switch_tab(shift ? -1 : 1); break;
        case VK_RETURN: do_play(); break;
        case VK_ESCAPE: do_quit(); break;
        }
    }

    Hit* hit_at(float x, float y) {
        for (auto it = hits.rbegin(); it != hits.rend(); ++it)   // later = on top
            if (x >= it->r.left && x < it->r.right && y >= it->r.top && y < it->r.bottom) return &*it;
        return nullptr;
    }
    void slider_to(int idx, float x) {
        Item& it = items[idx];
        for (auto& h : hits)
            if (h.what == H_SLIDER && h.index == idx) {
                float x0 = h.r.left + 12, x1 = h.r.right - 12;
                float f = std::clamp((x - x0) / (x1 - x0), 0.f, 1.f);
                int v = it.lo + (int)lroundf(f * (it.hi - it.lo) / it.step) * it.step;
                if (v != it.cur) { it.cur = v; store(it); }
            }
    }
    void select_row(int idx) {
        auto r = rows_of(tab);
        for (size_t k = 0; k < r.size(); k++) if (r[k] == idx) row[tab] = (int)k;
    }
    void mouse_move(float x, float y) {
        if (drag_slider >= 0) { slider_to(drag_slider, x); return; }
        Hit* h = hit_at(x, y);
        int what = h ? h->what : 0, index = h ? h->index : -1;
        if (what == H_LEFT || what == H_RIGHT || what == H_SLIDER) {   // hovering a control selects its row
            hover_what = what; hover_index = index;
            select_row(index);
            return;
        }
        hover_what = what; hover_index = index;
        if (what == H_ROW) select_row(index);
    }
    void mouse_down(float x, float y) {
        pad_mode = false;
        Hit* h = hit_at(x, y);
        if (!h) return;
        switch (h->what) {
        case H_TAB: tab = h->index; break;
        case H_ROW: select_row(h->index); change(items[h->index], 0); break;
        case H_LEFT: select_row(h->index); change(items[h->index], -1); break;
        case H_RIGHT: select_row(h->index); change(items[h->index], items[h->index].kind == CHOICE ? 1 : 0); break;
        case H_SLIDER: select_row(h->index); drag_slider = h->index; SetCapture(wnd); slider_to(h->index, x); break;
        case H_PLAY: do_play(); break;
        case H_QUIT: case H_CLOSE: do_quit(); break;
        case H_MIN: ShowWindow(wnd, SW_MINIMIZE); break;
        case H_DISC: browse_disc(); break;
        }
    }
    void mouse_up() {
        if (drag_slider >= 0) ReleaseCapture();
        drag_slider = -1;
    }

    // controllers: edges, with auto-repeat for held directions
    WORD pad_prev[4] = {};
    double repeat_at[4] = {};
    void poll_pads() {
        for (DWORD p = 0; p < 4; p++) {
            XINPUT_STATE st = {};
            if (XInputGetState(p, &st) != ERROR_SUCCESS) { pad_prev[p] = 0; continue; }
            WORD b = st.Gamepad.wButtons;
            short lx = st.Gamepad.sThumbLX, ly = st.Gamepad.sThumbLY;
            if (ly > 16000) b |= XINPUT_GAMEPAD_DPAD_UP;
            if (ly < -16000) b |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (lx > 16000) b |= XINPUT_GAMEPAD_DPAD_RIGHT;
            if (lx < -16000) b |= XINPUT_GAMEPAD_DPAD_LEFT;
            WORD down = b & ~pad_prev[p];
            const WORD dirs = XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN | XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT;
            if (down & dirs) repeat_at[p] = now + 0.4;
            else if ((b & dirs) && now >= repeat_at[p]) { down |= b & dirs; repeat_at[p] = now + 0.11; }
            pad_prev[p] = b;
            if (!down) continue;
            pad_mode = true;
            if (down & XINPUT_GAMEPAD_DPAD_UP) nav(-1);
            if (down & XINPUT_GAMEPAD_DPAD_DOWN) nav(1);
            if (down & XINPUT_GAMEPAD_DPAD_LEFT) change(selected(), -1);
            if (down & XINPUT_GAMEPAD_DPAD_RIGHT) change(selected(), 1);
            if (down & XINPUT_GAMEPAD_A) change(selected(), 0);
            if (down & XINPUT_GAMEPAD_LEFT_SHOULDER) switch_tab(-1);
            if (down & XINPUT_GAMEPAD_RIGHT_SHOULDER) switch_tab(1);
            if (down & XINPUT_GAMEPAD_START) do_play();
            if (down & XINPUT_GAMEPAD_B) do_quit();
        }
    }

    // ------------------------------------------------------------ window
    static LRESULT CALLBACK wndproc(HWND h, UINT m, WPARAM w, LPARAM l) {
        Launcher* L = (Launcher*)GetWindowLongPtrW(h, GWLP_USERDATA);
        if (!L) return DefWindowProcW(h, m, w, l);
        float x = GET_X_LPARAM(l) / L->scale, y = GET_Y_LPARAM(l) / L->scale;
        switch (m) {
        case WM_NCHITTEST: {
            POINT p = {GET_X_LPARAM(l), GET_Y_LPARAM(l)};
            ScreenToClient(h, &p);
            float px = p.x / L->scale, py = p.y / L->scale;
            if (py < 40 && px < W - 96) return HTCAPTION;
            return HTCLIENT;
        }
        case WM_KEYDOWN: L->key(w); return 0;
        case WM_MOUSEMOVE: {
            TRACKMOUSEEVENT tme = {sizeof tme, TME_LEAVE, h, 0};
            TrackMouseEvent(&tme);
            L->mouse_move(x, y);
            return 0;
        }
        case WM_LBUTTONDOWN: L->mouse_down(x, y); return 0;
        case WM_LBUTTONUP: L->mouse_up(); return 0;
        case WM_MOUSEWHEEL: L->nav(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1); return 0;
        case WM_MOUSELEAVE: L->hover_what = 0; L->hover_index = -1; return 0;
        case WM_CLOSE: L->do_quit(); return 0;
        case WM_ERASEBKGND: return 1;
        }
        return DefWindowProcW(h, m, w, l);
    }

    void create_window() {
        WNDCLASSW wc = {};
        wc.style = CS_DROPSHADOW;
        wc.lpfnWndProc = wndproc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"DoubleDashReduxLauncher";
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        RegisterClassW(&wc);
        // the display: DD_MONITOR (tests), else the primary one; fit the layout into its work area
        const char* mon = getenv("DD_MONITOR");
        struct Find { int want, n; RECT work; bool found; } f = {mon ? atoi(mon) : 0, 0, {}, false};
        if (f.want > 0)
            EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR m, HDC, LPRECT, LPARAM p) -> BOOL {
                Find& f = *(Find*)p;
                if (++f.n != f.want) return TRUE;
                MONITORINFO mi = {sizeof mi};
                GetMonitorInfoW(m, &mi);
                f.work = mi.rcWork;
                f.found = true;
                return FALSE;
            }, (LPARAM)&f);
        if (!f.found) SystemParametersInfoW(SPI_GETWORKAREA, 0, &f.work, 0);
        HDC dc = GetDC(nullptr);
        float dpi = (float)GetDeviceCaps(dc, LOGPIXELSX);
        ReleaseDC(nullptr, dc);
        float ww = (float)(f.work.right - f.work.left), wh = (float)(f.work.bottom - f.work.top);
        scale = std::min({dpi / 96.f, ww * 0.95f / W, wh * 0.95f / H});
        int pw = (int)(W * scale), ph = (int)(H * scale);
        int x = f.work.left + (int)(ww - pw) / 2, y = f.work.top + (int)(wh - ph) / 2;
        wnd = CreateWindowExW(WS_EX_APPWINDOW, L"DoubleDashReduxLauncher", L"Double Dash Redux",
                              WS_POPUP | WS_MINIMIZEBOX | WS_SYSMENU, x, y, pw, ph, nullptr, nullptr, wc.hInstance, nullptr);
        SetWindowLongPtrW(wnd, GWLP_USERDATA, (LONG_PTR)this);
        ShowWindow(wnd, f.found ? SW_SHOWNOACTIVATE : SW_SHOW);
        if (!f.found) SetForegroundWindow(wnd);
    }

    bool create_hwnd_target() {
        RECT rc;
        GetClientRect(wnd, &rc);
        ID2D1HwndRenderTarget* h = nullptr;
        if (FAILED(d2d->CreateHwndRenderTarget(
                D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                             D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                             96 * scale, 96 * scale),
                D2D1::HwndRenderTargetProperties(wnd, D2D1::SizeU(rc.right, rc.bottom)), &h)))
            return false;
        rt = h;
        rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
        create_target_resources();
        return true;
    }

    bool init_d2d() {
        if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &d2d))) return false;
        if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown**)&dw))) return false;
        create_text();
        d2d->CreatePathGeometry(&tri);
        ID2D1GeometrySink* s = nullptr;
        tri->Open(&s);
        s->BeginFigure({0.15f, 0}, D2D1_FIGURE_BEGIN_FILLED);
        s->AddLine({1, 0.5f});
        s->AddLine({0.15f, 1});
        s->EndFigure(D2D1_FIGURE_END_CLOSED);
        s->Close();
        release(s);
        return true;
    }

    // DD_LAUNCHER_SHOT: one frame into a PNG, no window
    bool shot(const char* path) {
        IWICImagingFactory* wic = nullptr;
        if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)))) return false;
        HDC dc = GetDC(nullptr);
        scale = GetDeviceCaps(dc, LOGPIXELSX) / 96.f;
        ReleaseDC(nullptr, dc);
        UINT pw = (UINT)(W * scale), ph = (UINT)(H * scale);
        IWICBitmap* bmp = nullptr;
        wic->CreateBitmap(pw, ph, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bmp);
        d2d->CreateWicBitmapRenderTarget(bmp, D2D1::RenderTargetProperties(D2D1_RENDER_TARGET_TYPE_DEFAULT,
                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
                                         96 * scale, 96 * scale), &rt);
        if (!rt) return false;
        create_target_resources();
        if (const char* t = getenv("DD_LAUNCHER_TAB")) tab = std::clamp(atoi(t), 0, NTABS - 1);
        if (const char* r = getenv("DD_LAUNCHER_ROW")) row[tab] = atoi(r);
        if (getenv("DD_LAUNCHER_PAD")) pad_mode = true;
        t0 = -1;   // past the fade-in
        now = 0;
        draw();
        IWICStream* st = nullptr;
        IWICBitmapEncoder* enc = nullptr;
        IWICBitmapFrameEncode* fr = nullptr;
        bool ok = SUCCEEDED(wic->CreateStream(&st)) && SUCCEEDED(st->InitializeFromFilename(widen(path).c_str(), GENERIC_WRITE)) &&
                  SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) &&
                  SUCCEEDED(enc->Initialize(st, WICBitmapEncoderNoCache)) && SUCCEEDED(enc->CreateNewFrame(&fr, nullptr)) &&
                  SUCCEEDED(fr->Initialize(nullptr)) && SUCCEEDED(fr->WriteSource(bmp, nullptr)) && SUCCEEDED(fr->Commit()) &&
                  SUCCEEDED(enc->Commit());
        release(fr); release(enc); release(st); release(bmp); release(wic);
        LOG("launcher", "shot %s: %s", path, ok ? "ok" : "failed");
        return ok;
    }

    double clock() {
        LARGE_INTEGER f, c;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&c);
        return (double)c.QuadPart / f.QuadPart;
    }

    bool run() {
        std::string refused;
        disc = disc_find(argc, argv, &refused);
        if (disc.empty() && !refused.empty()) {
            disc_problem = refused.substr(0, refused.find("\n\n"));
            for (char& c : disc_problem) if (c == '\n') c = ' ';
        }
        build();
        if (!init_d2d()) { LOG("launcher", "Direct2D unavailable: starting the game directly"); return true; }
        load_disc_art();
        if (const char* path = getenv("DD_LAUNCHER_SHOT")) {
            shot(path);
            return false;
        }
        create_window();
        if (!create_hwnd_target()) { DestroyWindow(wnd); return true; }
        t0 = clock();
        while (!done) {
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
            now = clock();
            if (GetForegroundWindow() == wnd) poll_pads();
            if (!rt && !create_hwnd_target()) break;
            if (IsIconic(wnd)) { Sleep(30); continue; }
            draw();   // EndDraw waits for the display's refresh
        }
        DestroyWindow(wnd);
        return play;
    }

    ~Launcher() {
        release_target_resources();
        release(rt); release(tri);
        for (IDWriteTextFormat* f : {f_title, f_tab, f_label, f_value, f_desc, f_small, f_button, f_path, f_logo, f_logo2})
            if (f) f->Release();
        release(dw); release(d2d);
    }
};

}   // namespace

bool launcher_wanted(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--play")) return false;
        if (!strcmp(argv[i], "--launcher")) return true;
    }
    if (getenv("DD_LAUNCHER_SHOT")) return true;
    if (getenv("DD_SCRIPT") || getenv("DD_DISC_NOUI")) return false;
    if (const char* e = getenv("DD_LAUNCHER")) return atoi(e) != 0;
    if (GetAsyncKeyState(VK_SHIFT) < 0) return true;
    return cfg_bool("launcher.show", true);
}

// true: boot the game; false: the user quit
bool launcher_run(int argc, char** argv) {
    HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);   // (file dialog, WIC); undone before the boot
    bool play;
    {
        Launcher L;
        L.argc = argc;
        L.argv = argv;
        play = L.run();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    LOG("launcher", "%s", play ? "play" : "quit");
    return play;
}
