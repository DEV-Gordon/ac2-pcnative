#include "config.h"
#include <cstdlib>
#include <fstream>
#include <sys/stat.h>

Config g_cfg;

const char* const kActionLabel[A_COUNT] = {
    "Pitch down  (D-Pad Up)", "Pitch up  (D-Pad Down)", "Roll left  (D-Pad Left)", "Roll right  (D-Pad Right)",
    "Machine gun  (Cross)", "Missile  (Circle)", "Ground map  (Square)", "Change target  (Triangle)",
    "Brake  (L1)", "Accelerate  (R1)", "Rudder left  (L2)", "Rudder right  (R2)",
    "L3", "R3", "Start / Pause", "Select",
    "Menu", "Toggle fullscreen"};

static const char* const kActionKey[A_COUNT] = {
    "up", "down", "left", "right", "cross", "circle", "square", "triangle",
    "l1", "r1", "l2", "r2", "l3", "r3", "start", "select", "menu", "fullscreen"};

// PS1 digital pad bits (active low): 0 Select 1 L3 2 R3 3 Start 4 Up 5 Right 6 Down 7 Left
// 8 L2 9 R2 10 L1 11 R1 12 Triangle 13 Circle 14 Cross 15 Square
const int kPsxBit[A_FIRST_HOTKEY] = {4, 6, 7, 5, 14, 13, 15, 12, 10, 11, 8, 9, 1, 2, 3, 0};

const char* const kKeyPreset[3] = {"WASD flight  (Space = missile)", "Arrows + Space  (right hand flies)",
                                   "PlayStation-like  (Z X A S ...)"};

std::string padName(int pad) {
    if (pad == PAD_NONE) return "";
    if (pad == PAD_LTRIG) return "lefttrigger";
    if (pad == PAD_RTRIG) return "righttrigger";
    const char* s = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)pad);
    return s ? s : "";
}
std::string padLabel(int pad) {
    switch (pad) {
        case PAD_NONE: return "";
        case PAD_LTRIG: return "LT / L2";
        case PAD_RTRIG: return "RT / R2";
        case SDL_CONTROLLER_BUTTON_A: return "A / Cross";
        case SDL_CONTROLLER_BUTTON_B: return "B / Circle";
        case SDL_CONTROLLER_BUTTON_X: return "X / Square";
        case SDL_CONTROLLER_BUTTON_Y: return "Y / Triangle";
        case SDL_CONTROLLER_BUTTON_BACK: return "Back / Select";
        case SDL_CONTROLLER_BUTTON_GUIDE: return "Guide / PS";
        case SDL_CONTROLLER_BUTTON_START: return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK: return "Left stick click";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK: return "Right stick click";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER: return "LB / L1";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER: return "RB / R1";
        case SDL_CONTROLLER_BUTTON_DPAD_UP: return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN: return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT: return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT: return "D-Pad Right";
        default: return padName(pad);
    }
}
static int padFromName(const std::string& n) {
    if (n.empty()) return PAD_NONE;
    if (n == "lefttrigger") return PAD_LTRIG;
    if (n == "righttrigger") return PAD_RTRIG;
    auto b = SDL_GameControllerGetButtonFromString(n.c_str());
    return b == SDL_CONTROLLER_BUTTON_INVALID ? PAD_NONE : (int)b;
}
std::string keyName(SDL_Scancode sc) { return sc == SDL_SCANCODE_UNKNOWN ? "" : SDL_GetScancodeName(sc); }

std::string prefDir() {
    char* p = SDL_GetPrefPath("AC2Native", "AceCombat2");
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}
void setWindowIcon(SDL_Window* win) {
    for (auto p : {baseDir() + "icon/ac2.bmp", baseDir() + "assets/icon/ac2.bmp", baseDir() + "../assets/icon/ac2.bmp"}) {
        if (SDL_Surface* icon = SDL_LoadBMP(p.c_str())) {
            SDL_SetWindowIcon(win, icon);
            SDL_FreeSurface(icon);
            return;
        }
    }
}

std::string baseDir() {
    char* p = SDL_GetBasePath();
    std::string s = p ? p : "./";
    SDL_free(p);
    return s;
}
bool fileExists(const std::string& path) {
    struct stat st;
    return !path.empty() && stat(path.c_str(), &st) == 0;
}

void applyKeyPreset(Config& c, int preset) {
    auto K = [&](Action a, SDL_Scancode k) { c.bind[a].key = k; };
    K(A_L3, SDL_SCANCODE_UNKNOWN);
    K(A_R3, SDL_SCANCODE_UNKNOWN);
    K(A_START, SDL_SCANCODE_RETURN);
    switch (preset) {
        case 0:
            K(A_UP, SDL_SCANCODE_W); K(A_DOWN, SDL_SCANCODE_S); K(A_LEFT, SDL_SCANCODE_A); K(A_RIGHT, SDL_SCANCODE_D);
            K(A_CIRCLE, SDL_SCANCODE_SPACE); K(A_CROSS, SDL_SCANCODE_F); K(A_TRIANGLE, SDL_SCANCODE_R);
            K(A_SQUARE, SDL_SCANCODE_M); K(A_R1, SDL_SCANCODE_LSHIFT); K(A_L1, SDL_SCANCODE_LCTRL);
            K(A_L2, SDL_SCANCODE_Q); K(A_R2, SDL_SCANCODE_E); K(A_SELECT, SDL_SCANCODE_BACKSPACE);
            break;
        case 1:
            K(A_UP, SDL_SCANCODE_UP); K(A_DOWN, SDL_SCANCODE_DOWN); K(A_LEFT, SDL_SCANCODE_LEFT); K(A_RIGHT, SDL_SCANCODE_RIGHT);
            K(A_CIRCLE, SDL_SCANCODE_SPACE); K(A_CROSS, SDL_SCANCODE_Z); K(A_TRIANGLE, SDL_SCANCODE_X);
            K(A_SQUARE, SDL_SCANCODE_C); K(A_R1, SDL_SCANCODE_A); K(A_L1, SDL_SCANCODE_S);
            K(A_L2, SDL_SCANCODE_Q); K(A_R2, SDL_SCANCODE_E); K(A_SELECT, SDL_SCANCODE_RSHIFT);
            break;
        default:
            K(A_UP, SDL_SCANCODE_UP); K(A_DOWN, SDL_SCANCODE_DOWN); K(A_LEFT, SDL_SCANCODE_LEFT); K(A_RIGHT, SDL_SCANCODE_RIGHT);
            K(A_CROSS, SDL_SCANCODE_Z); K(A_CIRCLE, SDL_SCANCODE_X); K(A_SQUARE, SDL_SCANCODE_A); K(A_TRIANGLE, SDL_SCANCODE_S);
            K(A_L1, SDL_SCANCODE_Q); K(A_R1, SDL_SCANCODE_W); K(A_L2, SDL_SCANCODE_1); K(A_R2, SDL_SCANCODE_2);
            K(A_SELECT, SDL_SCANCODE_RSHIFT);
            break;
    }
}

void Config::setDefaults() {
    for (auto& b : bind) b = Binding{};
    auto pad = [&](Action a, int p) { bind[a].pad = p; };
    pad(A_UP, SDL_CONTROLLER_BUTTON_DPAD_UP); pad(A_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN);
    pad(A_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT); pad(A_RIGHT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT);
    pad(A_CROSS, SDL_CONTROLLER_BUTTON_A); pad(A_CIRCLE, SDL_CONTROLLER_BUTTON_B);
    pad(A_SQUARE, SDL_CONTROLLER_BUTTON_X); pad(A_TRIANGLE, SDL_CONTROLLER_BUTTON_Y);
    pad(A_L1, SDL_CONTROLLER_BUTTON_LEFTSHOULDER); pad(A_R1, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
    pad(A_L2, PAD_LTRIG); pad(A_R2, PAD_RTRIG);
    pad(A_L3, SDL_CONTROLLER_BUTTON_LEFTSTICK); pad(A_R3, SDL_CONTROLLER_BUTTON_RIGHTSTICK);
    pad(A_START, SDL_CONTROLLER_BUTTON_START); pad(A_SELECT, SDL_CONTROLLER_BUTTON_BACK);
    pad(A_MENU, SDL_CONTROLLER_BUTTON_GUIDE);
    bind[A_MENU].key = SDL_SCANCODE_ESCAPE;
    bind[A_FULLSCREEN].key = SDL_SCANCODE_F11;
    applyKeyPreset(*this, 0);
}

static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? "" : s.substr(a, b - a + 1);
}
static int clampi(int v, int lo, int hi, int def) { return v < lo || v > hi ? def : v; }

bool Config::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        auto eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = trim(line.substr(0, eq)), v = trim(line.substr(eq + 1));
        int n = atoi(v.c_str());
        if (k == "disc") discPath = v;
        else if (k == "card_dir") cardDir = v;
        else if (k == "window_mode") windowMode = clampi(n, 0, 2, 0);
        else if (k == "win_w") winW = clampi(n, 320, 16384, 1280);
        else if (k == "win_h") winH = clampi(n, 240, 16384, 960);
        else if (k == "fs_w") fsW = clampi(n, 320, 16384, 1920);
        else if (k == "fs_h") fsH = clampi(n, 240, 16384, 1080);
        else if (k == "vsync") vsync = n != 0;
        else if (k == "aspect") aspect = clampi(n, 0, 2, 0);
        else if (k == "screen_filter") screenFilter = clampi(n, 0, 2, 0);
        else if (k == "internal_res") internalRes = clampi(n, 0, 2, 0);
        else if (k == "widescreen") widescreen = n != 0;
        else if (k == "movies") movies = n != 0;
        else if (k == "volume") volume = clampi(n, 0, 100, 100);
        else if (k == "stick_as_dpad") stickAsDpad = n != 0;
        else if (k == "show_launcher") showLauncher = n != 0;
        else
            for (int a = 0; a < A_COUNT; a++) {
                std::string base = kActionKey[a];
                if (k == base + ".key") bind[a].key = v.empty() ? SDL_SCANCODE_UNKNOWN : SDL_GetScancodeFromName(v.c_str());
                else if (k == base + ".pad") bind[a].pad = padFromName(v);
            }
    }
    return true;
}

bool Config::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "# Ace Combat 2 native port configuration (" AC2NATIVE_VERSION ")\n";
    f << "disc=" << discPath << "\ncard_dir=" << cardDir << "\n";
    f << "window_mode=" << windowMode << "\nwin_w=" << winW << "\nwin_h=" << winH << "\nfs_w=" << fsW << "\nfs_h=" << fsH
      << "\nvsync=" << vsync << "\naspect=" << aspect << "\nscreen_filter=" << screenFilter
      << "\ninternal_res=" << internalRes << "\nwidescreen=" << widescreen << "\n";
    f << "movies=" << movies << "\nvolume=" << volume << "\nstick_as_dpad=" << stickAsDpad
      << "\nshow_launcher=" << showLauncher << "\n";
    for (int a = 0; a < A_COUNT; a++) {
        f << kActionKey[a] << ".key=" << keyName(bind[a].key) << "\n";
        f << kActionKey[a] << ".pad=" << padName(bind[a].pad) << "\n";
    }
    return (bool)f;
}
