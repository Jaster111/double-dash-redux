// User settings: settings.ini in the data directory (the working directory, see main.cpp find_data_dir).
// Written with defaults and comments on first run.  Keys are "section.name"; a DD_* environment
// variable given to cfg_str/cfg_int/... overrides the file (test runs set those, see tools/run.ps1).
// cfg_set rewrites one value in place and keeps the rest of the file (comments, order) untouched.
#include "../common.h"
#include <map>
#include <string>
#include <vector>

static const char* const SETTINGS_FILE = "settings.ini";

static const char* const DEFAULT_SETTINGS =
    "; Mario Kart: Double Dash!! settings.  Changes take effect the next time the game starts.\n"
    "\n"
    "[game]\n"
    "; disc image (Europe, GM4P01: .iso / .gcm, or Dolphin .rvz); empty = look next to the executable, or ask\n"
    "disc =\n"
    "\n"
    "[video]\n"
    "; borderless fullscreen on the display the window is on (Alt+Enter or F11 toggles while playing)\n"
    "fullscreen = 0\n"
    "; window client size in pixels\n"
    "window_width = 1280\n"
    "window_height = 960\n"
    "; internal resolution as a multiple of the GameCube's 640x528 (1-8)\n"
    "internal_scale = 3\n"
    "; 1 = offer 60 Hz (PAL60) at boot, preselected; 0 = 50 Hz only (no prompt)\n"
    "pal60 = 1\n"
    "; 1 = 16:9 widescreen (the \"16:9 Widescreen v2\" code by Ralf and Kester, with its menu backgrounds add-on)\n"
    "widescreen = 0\n"
    "; 1 = the race courses' distance blur and heat shimmer, as on the console; 0 = everything stays sharp\n"
    "depth_blur = 1\n"
    "\n"
    "[audio]\n"
    "; master volume in percent (0-100)\n"
    "volume = 100\n"
    "; 1 = no sound output at all\n"
    "mute = 0\n"
    "\n"
    "[input]\n"
    "; controller rumble (XInput pads) and its strength in percent\n"
    "rumble = 1\n"
    "rumble_strength = 100\n"
    "\n"
    "[launcher]\n"
    "; 1 = open the settings window (launcher) at start; 0 = start the game directly (hold Shift while\n"
    "; starting, or pass --launcher, to get the launcher anyway)\n"
    "show = 1\n"
    "\n"
    "[memcard]\n"
    "; memory card image per slot (Dolphin .raw format), or none for an empty slot\n"
    "slot_a = saves/MemoryCardA.raw\n"
    "slot_b = none\n";

static std::map<std::string, std::string> s_values;

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

static std::vector<std::string> read_lines() {
    std::vector<std::string> lines;
    FILE* f = fopen(SETTINGS_FILE, "rb");
    if (!f) return lines;
    std::string cur;
    for (int c; (c = fgetc(f)) != EOF;) {
        if (c == '\n') { lines.push_back(cur); cur.clear(); }
        else if (c != '\r') cur += (char)c;
    }
    if (!cur.empty()) lines.push_back(cur);
    fclose(f);
    return lines;
}

// "[section]" -> section, "name = value" -> fills name/value; false for blank lines and comments
static bool parse_line(const std::string& raw, std::string& section, std::string& name, std::string& value) {
    std::string l = trim(raw);
    if (l.empty() || l[0] == ';' || l[0] == '#') return false;
    if (l[0] == '[') {
        size_t e = l.find(']');
        section = trim(l.substr(1, e == std::string::npos ? std::string::npos : e - 1));
        return false;
    }
    size_t eq = l.find('=');
    if (eq == std::string::npos) return false;
    name = trim(l.substr(0, eq));
    value = trim(l.substr(eq + 1));
    return !name.empty();
}

static void parse(const std::vector<std::string>& lines) {
    std::string section, name, value;
    for (auto& l : lines)
        if (parse_line(l, section, name, value)) s_values[section + "." + name] = value;
}

void cfg_load() {
    std::vector<std::string> lines = read_lines();
    if (lines.empty()) {
        if (FILE* f = fopen(SETTINGS_FILE, "wb")) {
            fputs(DEFAULT_SETTINGS, f);
            fclose(f);
            LOG("config", "created %s with defaults", SETTINGS_FILE);
        }
        lines = read_lines();
    }
    parse(lines);
}

const char* cfg_str(const char* key, const char* env) {
    if (env)
        if (const char* e = getenv(env)) return e;
    auto it = s_values.find(key);
    return it == s_values.end() ? nullptr : it->second.c_str();
}

int cfg_int(const char* key, int def, const char* env) {
    const char* s = cfg_str(key, env);
    return s && *s ? atoi(s) : def;
}

bool cfg_bool(const char* key, bool def, const char* env) {
    const char* s = cfg_str(key, env);
    if (!s || !*s) return def;
    return !(s[0] == '0' || _stricmp(s, "false") == 0 || _stricmp(s, "off") == 0 || _stricmp(s, "no") == 0);
}

void cfg_set(const char* key, const char* value) {
    s_values[key] = value;
    std::string k = key, want_section = k.substr(0, k.find('.')), want_name = k.substr(k.find('.') + 1);
    std::vector<std::string> lines = read_lines();
    std::string section, name, v;
    int section_end = -1;   // insertion point for a key missing from an existing section
    bool done = false;
    for (size_t i = 0; i < lines.size() && !done; i++) {
        std::string l = trim(lines[i]);
        if (!l.empty() && l[0] == '[') {
            parse_line(l, section, name, v);
            if (section == want_section) section_end = (int)i + 1;   // (a section with no keys yet)
            continue;
        }
        if (section == want_section && !l.empty()) section_end = (int)i + 1;
        if (parse_line(lines[i], section, name, v) && section == want_section && name == want_name) {
            lines[i] = want_name + " = " + value;
            done = true;
        }
    }
    if (!done) {
        std::string line = want_name + " = " + value;
        if (section_end >= 0) lines.insert(lines.begin() + section_end, line);
        else { lines.push_back("[" + want_section + "]"); lines.push_back(line); }
    }
    FILE* f = fopen(SETTINGS_FILE, "wb");
    if (!f) { LOG("config", "cannot write %s", SETTINGS_FILE); return; }
    for (auto& l : lines) fprintf(f, "%s\n", l.c_str());
    fclose(f);
}
