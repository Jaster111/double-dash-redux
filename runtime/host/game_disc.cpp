// Finding and checking the disc image.  The recompiled code is the code of one exact main.dol (Mario Kart:
// Double Dash!! Europe, GM4P01), so the image is accepted only when its game ID and DOL match; the runtime
// boots from the image alone (the DOL is read from it, main.cpp).
//
// Search order: --iso PATH, settings.ini game.disc (DD_ISO), orig/game.iso (development tree), then *.iso /
// *.gcm next to the executable.  When none of them is the right game a file dialog asks for the image, and
// the choice is saved as game.disc.  Paths are UTF-8.  DD_DISC_NOUI=1 (headless tests): no message boxes or
// dialog, the process exits with code 2 when no image fits.
#include "../common.h"
#include <windows.h>
#include <commdlg.h>
#include <string>
#include <vector>

extern const char* g_iso_path;   // hw/di.cpp

static const char GAME_ID[] = "GM4P01";
static const u32 DOL_SIZE = 3613184, DOL_CRC32 = 0x3B7E37E3;
static const char* const TITLE = "Mario Kart: Double Dash!!";

static std::wstring widen(const std::string& s) {
    std::wstring w(MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0), 0);
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, &w[0], (int)w.size());
    w.pop_back();
    return w;
}
static std::string narrow(const wchar_t* w) {
    std::string s(WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr), 0);
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], (int)s.size(), nullptr, nullptr);
    s.pop_back();
    return s;
}

static u32 crc32(const u8* p, size_t n) {
    static u32 table[256];
    if (!table[1])
        for (u32 i = 0; i < 256; i++) {
            u32 c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320 ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
    u32 c = ~0u;
    while (n--) c = table[(c ^ *p++) & 0xFF] ^ (c >> 8);
    return ~c;
}

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

static bool read_at(HANDLE f, u64 off, void* dst, u32 len) {
    OVERLAPPED ov{};
    ov.Offset = (DWORD)off;
    ov.OffsetHigh = (DWORD)(off >> 32);
    DWORD got = 0;
    return ReadFile(f, dst, len, &got, &ov) && got == len;
}

// empty if the image at `path` is the game this build runs, otherwise why not (for the user)
static std::string check_image(const std::string& path) {
    HANDLE f = CreateFileW(widen(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "The file could not be opened.";
    std::string why;
    u8 h[0x440];
    if (!read_at(f, 0, h, sizeof h)) why = "The file is too small to be a GameCube disc image.";
    else if (!memcmp(h, "RVZ\x01", 4) || !memcmp(h, "WIA\x01", 4) || !memcmp(h, "CISO", 4) ||
             be32(h) == 0x01C00BB1 /* GCZ */)
        why = "This is a compressed disc image (RVZ / WIA / GCZ / CISO). Convert it to a plain ISO in Dolphin: "
              "right-click the game, Convert File..., Format: ISO.";
    else if (be32(h + 0x1C) != 0xC2339F3D) why = "This is not a GameCube disc image.";
    else if (memcmp(h, GAME_ID, 6)) {
        char name[0x41] = {};
        memcpy(name, h + 0x20, 0x40);
        char id[7] = {};
        memcpy(id, h, 6);
        if (!memcmp(h, "GM4", 3))
            why = std::string("This is the ") + (h[3] == 'E' ? "North American" : h[3] == 'J' ? "Japanese" : "other") +
                  " release of the game (" + id + "). This port is built for the European release (GM4P01).";
        else
            why = std::string("This is \"") + name + "\" (" + id + "), not Mario Kart: Double Dash!! (GM4P01).";
    } else {
        u32 off = be32(h + 0x420), end = 0;
        u8 dh[0x100];
        if (read_at(f, off, dh, sizeof dh))
            for (int i = 0; i < 18; i++) {
                u32 o = be32(dh + 4 * i), s = be32(dh + 0x90 + 4 * i);
                if (s && o + s > end) end = o + s;
            }
        std::vector<u8> dol(end);
        if (end != DOL_SIZE || !read_at(f, off, dol.data(), end) || crc32(dol.data(), end) != DOL_CRC32)
            why = "The game code (main.dol) in this image is not the one this port was built from (a different "
                  "revision, or a modified image).";
    }
    CloseHandle(f);
    return why;
}

static std::vector<std::string> images_in_dir() {
    std::vector<std::string> out;
    for (const wchar_t* pat : {L"*.iso", L"*.gcm"}) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW(pat, &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(narrow(fd.cFileName));
        while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    return out;
}

static const bool s_noui = getenv("DD_DISC_NOUI") != nullptr;

static void message(const std::string& text, UINT icon) {
    LOG("disc", "%s", text.c_str());
    if (s_noui) return;
    MessageBoxW(nullptr, widen(text).c_str(), widen(TITLE).c_str(), MB_OK | icon);
}

static std::string ask_for_image() {
    if (s_noui) ExitProcess(2);
    wchar_t file[MAX_PATH * 4] = {};
    OPENFILENAMEW ofn = {sizeof ofn};
    ofn.lpstrTitle = L"Select your Mario Kart: Double Dash!! (Europe) disc image";
    ofn.lpstrFilter = L"GameCube disc images (*.iso, *.gcm)\0*.iso;*.gcm\0All files (*.*)\0*.*\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = sizeof file / sizeof *file;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    return GetOpenFileNameW(&ofn) ? narrow(file) : std::string();
}

void disc_select(int argc, char** argv) {
    static std::string chosen;
    std::vector<std::pair<std::string, bool>> cands;   // path, given explicitly (checked even if it is missing)
    for (int i = 1; i + 1 < argc; i++)
        if (!strcmp(argv[i], "--iso")) {
            wchar_t w[MAX_PATH * 4];
            MultiByteToWideChar(CP_ACP, 0, argv[i + 1], -1, w, MAX_PATH * 4);
            cands.push_back({narrow(w), true});
        }
    const char* configured = cfg_str("game.disc", "DD_ISO");
    if (configured && *configured) cands.push_back({configured, true});
    cands.push_back({"orig/game.iso", false});
    for (auto& p : images_in_dir()) cands.push_back({p, false});

    std::string refused;   // the candidates that are not the right image, and why
    for (auto& c : cands) {
        if (!c.second && GetFileAttributesW(widen(c.first).c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string why = check_image(c.first);
        if (why.empty()) { chosen = c.first; break; }
        refused += c.first + "\n" + why + "\n\n";
    }
    if (chosen.empty())
        message(refused + "Please select your disc image of Mario Kart: Double Dash!! (Europe, GM4P01) as a plain "
                "ISO (.iso or .gcm), for example one dumped from your own disc with Dolphin.",
                refused.empty() ? MB_ICONINFORMATION : MB_ICONWARNING);
    while (chosen.empty()) {
        std::string p = ask_for_image();
        if (p.empty()) ExitProcess(1);
        std::string why = check_image(p);
        if (why.empty()) {
            chosen = p;
            cfg_set("game.disc", p.c_str());
        } else
            message(p + "\n\n" + why, MB_ICONWARNING);
    }
    g_iso_path = chosen.c_str();
    LOG("disc", "disc image %s", g_iso_path);
}
