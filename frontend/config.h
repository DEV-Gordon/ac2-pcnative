#pragma once
#include <SDL.h>
#include <string>

#define AC2NATIVE_VERSION "0.9.0"

enum Action {
    A_UP, A_DOWN, A_LEFT, A_RIGHT,
    A_CROSS, A_CIRCLE, A_SQUARE, A_TRIANGLE,
    A_L1, A_R1, A_L2, A_R2, A_L3, A_R3,
    A_START, A_SELECT,
    A_MENU, A_FULLSCREEN,
    A_COUNT
};
constexpr int A_FIRST_HOTKEY = A_MENU;

constexpr int PAD_NONE = -1;
constexpr int PAD_LTRIG = 100;
constexpr int PAD_RTRIG = 101;

struct Binding {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    int pad = PAD_NONE;
};

enum WindowMode { WM_WINDOWED, WM_FULLSCREEN, WM_BORDERLESS };
enum AspectMode { AR_4_3, AR_16_9, AR_STRETCH };
enum ScreenFilter { SF_SHARP, SF_SMOOTH, SF_PIXEL };

struct Config {
    std::string discPath;
    std::string cardDir;

    int windowMode = WM_WINDOWED;
    int winW = 1280, winH = 960;
    int fsW = 1920, fsH = 1080;
    bool vsync = true;
    int aspect = AR_4_3;
    int screenFilter = SF_SHARP;
    int internalRes = 0;     // 0 = 1x, 1 = 2x, 2 = 4x
    bool widescreen = false;

    bool movies = true;
    int volume = 100;
    bool stickAsDpad = true;
    bool showLauncher = true;
    Binding bind[A_COUNT];

    void setDefaults();
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

extern Config g_cfg;
extern const char* const kActionLabel[A_COUNT];
extern const char* const kKeyPreset[3];
extern const int kPsxBit[A_FIRST_HOTKEY];    // PS1 pad bit for each game action

void applyKeyPreset(Config& cfg, int preset);
std::string padName(int pad);
std::string padLabel(int pad);
std::string keyName(SDL_Scancode sc);
std::string prefDir();
std::string baseDir();
void setWindowIcon(struct SDL_Window* win);   // assets/icon/ac2.bmp next to the program (or in the source tree)
bool fileExists(const std::string& path);
