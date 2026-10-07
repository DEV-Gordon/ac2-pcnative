// Launcher window (Dear ImGui): disc, video, controls. Returns true when the player presses PLAY.
#include "config.h"
#include "ui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <SDL.h>
#include <GL/gl.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <vector>

static std::vector<SDL_GameController*> pads;
static void padsRefresh() {
    for (auto* p : pads) SDL_GameControllerClose(p);
    pads.clear();
    for (int i = 0; i < SDL_NumJoysticks(); i++)
        if (SDL_IsGameController(i))
            if (auto* c = SDL_GameControllerOpen(i)) pads.push_back(c);
}

static std::string pickPath(bool directory) {
    const char* cmds[2];
    if (directory) {
        cmds[0] = "zenity --file-selection --directory --title='Select folder' 2>/dev/null";
        cmds[1] = "kdialog --getexistingdirectory . 2>/dev/null";
    } else {
        cmds[0] = "zenity --file-selection --title='Select the disc image (.bin)' --file-filter='Disc images | *.bin *.BIN' 2>/dev/null";
        cmds[1] = "kdialog --getopenfilename . '*.bin' 2>/dev/null";
    }
    for (auto c : cmds) {
        FILE* p = popen(c, "r");
        if (!p) continue;
        char buf[4096] = {0};
        bool got = fgets(buf, sizeof buf, p) != nullptr;
        int rc = pclose(p);
        if (got && rc == 0) {
            std::string s = buf;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) return s;
        }
    }
    return "";
}

struct Res { int w, h; };

struct Launcher {
    Config& cfg;
    std::string cfgPath;
    std::vector<Res> modes;
    int waitAction = -1, waitKind = 0;
    int preset = 0;
    std::string status;
    Uint32 statusUntil = 0;
    bool play = false, done = false;

    void setStatus(const std::string& s) { status = s; statusUntil = SDL_GetTicks() + 4000; }
    bool discOk() const { return fileExists(cfg.discPath); }

    float labelW() const { return ImGui::GetFontSize() * 13.5f; }
    void label(const char* text, const char* tip = nullptr) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(text);
        if (tip) ui::help(tip);
        ImGui::SameLine(labelW());
        ImGui::SetNextItemWidth(-FLT_MIN);
    }
    void statusDot(bool good, const char* text, bool neutral = false) {
        ImVec2 p = ImGui::GetCursorScreenPos();
        float r = ImGui::GetFontSize() * 0.28f;
        ImVec4 col = good ? ui::ok() : neutral ? ui::dim() : ui::bad();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + r, p.y + ImGui::GetTextLineHeight() * 0.5f), r,
                                                    ImGui::GetColorU32(col));
        ImGui::Dummy(ImVec2(r * 2.6f, 0));
        ImGui::SameLine(0, 0);
        ImGui::TextColored(good ? ImVec4(0.85f, 0.9f, 0.95f, 1) : col, "%s", text);
    }
    bool pathField(const char* id, std::string& value, const char* button) {
        char buf[2048];
        snprintf(buf, sizeof buf, "%s", value.c_str());
        float bw = ImGui::CalcTextSize(button).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
        ImGui::SetNextItemWidth(-FLT_MIN - bw);
        if (ImGui::InputText(id, buf, sizeof buf)) value = buf;
        ImGui::SameLine();
        return ImGui::Button(button);
    }

    void tabGame() {
        ui::section("Game disc");
        if (pathField("##disc", cfg.discPath, "Browse...")) {
            auto p = pickPath(false);
            if (!p.empty()) cfg.discPath = p;
            else setStatus("No file dialog available (install zenity) - type the path instead.");
        }
        statusDot(discOk(), discOk() ? "Disc image found" : "Select the Ace Combat 2 (USA) .bin file");
        ImGui::TextDisabled("No BIOS needed: the PlayStation system calls are reimplemented natively.");

        ui::section("Memory card");
        if (pathField("##card", cfg.cardDir, "Browse...##card")) {
            auto p = pickPath(true);
            if (!p.empty()) cfg.cardDir = p;
        }
        ImGui::TextDisabled("The game saves to its memory card; each save is a file in this folder.");

        ui::section("Game");
        ImGui::Checkbox("Play the intro movies", &cfg.movies);
        label("Volume");
        ImGui::SliderInt("##vol", &cfg.volume, 0, 100, "%d %%");
        ImGui::Checkbox("Show this launcher every time the game starts", &cfg.showLauncher);
        ImGui::TextDisabled("If disabled, start the game with --launcher to see it again.");
    }

    void tabVideo() {
        ui::section("Display");
        const char* wm[] = {"Windowed", "Fullscreen (exclusive)", "Borderless fullscreen"};
        label("Window mode");
        ImGui::Combo("##wm", &cfg.windowMode, wm, 3);
        if (cfg.windowMode == WM_WINDOWED) {
            static const Res presets[] = {{640, 480}, {960, 720}, {1280, 960}, {1280, 720}, {1600, 1200},
                                          {1920, 1080}, {2560, 1440}};
            char cur[32];
            snprintf(cur, sizeof cur, "%d x %d", cfg.winW, cfg.winH);
            label("Window size");
            if (ImGui::BeginCombo("##ws", cur)) {
                for (auto r : presets) {
                    char l[32];
                    snprintf(l, sizeof l, "%d x %d", r.w, r.h);
                    if (ImGui::Selectable(l, r.w == cfg.winW && r.h == cfg.winH)) { cfg.winW = r.w; cfg.winH = r.h; }
                }
                ImGui::EndCombo();
            }
        } else if (cfg.windowMode == WM_FULLSCREEN) {
            char cur[32];
            snprintf(cur, sizeof cur, "%d x %d", cfg.fsW, cfg.fsH);
            label("Resolution");
            if (ImGui::BeginCombo("##fs", cur)) {
                for (auto r : modes) {
                    char l[32];
                    snprintf(l, sizeof l, "%d x %d", r.w, r.h);
                    if (ImGui::Selectable(l, r.w == cfg.fsW && r.h == cfg.fsH)) { cfg.fsW = r.w; cfg.fsH = r.h; }
                }
                ImGui::EndCombo();
            }
        }
        const char* ar[] = {"4:3 (original)", "16:9", "Stretch to window"};
        label("Aspect ratio");
        ImGui::Combo("##ar", &cfg.aspect, ar, 3);
        if (cfg.aspect == AR_16_9 && !cfg.widescreen)
            ImGui::TextColored(ui::warn(), "16:9 without the widescreen mode stretches the 4:3 picture.");
        const char* sf[] = {"Sharp (recommended)", "Smooth", "Pixelated"};
        label("Screen filter");
        ImGui::Combo("##sf", &cfg.screenFilter, sf, 3);
        ImGui::Indent(labelW());
        ImGui::Checkbox("VSync", &cfg.vsync);
        ImGui::Unindent(labelW());

        ui::section("Rendering");
        const char* ir[] = {"1x (native 320x240)", "2x (640x480)", "4x (1280x960)"};
        label("Internal resolution", "Draws the 3D at a multiple of the original resolution.\n"
                                     "2x runs on any PC; 4x wants a quad-core CPU.");
        ImGui::Combo("##ir", &cfg.internalRes, ir, 3);
        ImGui::Indent(labelW());
        ImGui::Checkbox("Widescreen 16:9 (wider field of view)", &cfg.widescreen);
        ui::help("Widens the 3D view to 16:9. The 2D HUD keeps its original layout, stretched horizontally.");
        ImGui::Unindent(labelW());
    }

    void tabControls() {
        ui::section("Keyboard preset");
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 20);
        ImGui::Combo("##preset", &preset, kKeyPreset, 3);
        ImGui::SameLine();
        if (ImGui::Button("Apply preset")) { applyKeyPreset(cfg, preset); setStatus("Keyboard preset applied."); }
        ImGui::SameLine();
        if (ImGui::Button("Reset all to defaults")) {
            Config d;
            d.setDefaults();
            for (int i = 0; i < A_COUNT; i++) cfg.bind[i] = d.bind[i];
            setStatus("Controls reset to defaults.");
        }
        ui::section("Gamepad");
        if (pads.empty()) ImGui::TextDisabled("No gamepad detected. Connect one at any time.");
        for (auto* p : pads) statusDot(true, SDL_GameControllerName(p) ? SDL_GameControllerName(p) : "Gamepad");
        ImGui::Checkbox("Left stick moves the plane (as D-pad)", &cfg.stickAsDpad);

        ui::section("Bindings");
        ImGui::TextDisabled("Click a cell, then press a key or gamepad button. Esc cancels.");
        if (ImGui::BeginTable("bind", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_PadOuterX)) {
            float fs = ImGui::GetFontSize();
            ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthFixed, fs * 14);
            ImGui::TableSetupColumn("Keyboard", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("Gamepad", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, fs * 3.5f);
            ImGui::TableHeadersRow();
            for (int a = 0; a < A_COUNT; a++) {
                if (a == A_FIRST_HOTKEY) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::PushFont(ui::bold);
                    ImGui::TextColored(ui::accent(), "Hotkeys");
                    ImGui::PopFont();
                }
                ImGui::TableNextRow();
                ImGui::PushID(a);
                ImGui::TableSetColumnIndex(0);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(kActionLabel[a]);
                bool wk = waitAction == a && waitKind == 0, wp = waitAction == a && waitKind == 1;
                ImGui::TableSetColumnIndex(1);
                std::string kn = wk ? "press a key..." : keyName(cfg.bind[a].key);
                if (kn.empty()) kn = "-";
                if (ImGui::Button((kn + "##k").c_str(), ImVec2(-FLT_MIN, 0))) { waitAction = a; waitKind = 0; }
                ImGui::TableSetColumnIndex(2);
                std::string pn = wp ? "press a button..." : padLabel(cfg.bind[a].pad);
                if (pn.empty()) pn = "-";
                if (ImGui::Button((pn + "##p").c_str(), ImVec2(-FLT_MIN, 0))) { waitAction = a; waitKind = 1; }
                ImGui::TableSetColumnIndex(3);
                if (ImGui::SmallButton("Clear")) { cfg.bind[a].key = SDL_SCANCODE_UNKNOWN; cfg.bind[a].pad = PAD_NONE; }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    void tabAbout() {
        ImGui::PushFont(ui::bold);
        ImGui::Text("Ace Combat 2 - native PC port  %s", AC2NATIVE_VERSION);
        ImGui::PopFont();
        ImGui::TextWrapped("The game's MIPS code is statically recompiled to C and runs natively; the PlayStation "
                           "hardware (graphics, sound, CD, BIOS) is reimplemented in native code. "
                           "You need your own copy of the game.");
        ui::section("In game");
        ImGui::BulletText("%s: menu (resume, fullscreen, quit)", keyName(cfg.bind[A_MENU].key).c_str());
        ImGui::BulletText("%s: toggle fullscreen", keyName(cfg.bind[A_FULLSCREEN].key).c_str());
        ImGui::BulletText("Progress is saved by the game itself to its memory card.");
        ui::section("Credits and licenses");
        ImGui::BulletText("SDL2 - zlib license");
        ImGui::BulletText("Dear ImGui - MIT license");
        ImGui::BulletText("Roboto font - Apache License 2.0");
        ImGui::TextDisabled("Ace Combat is a trademark of Bandai Namco Entertainment. Not affiliated.");
    }

    void header() {
        ImGui::PushFont(ui::title);
        ImGui::TextColored(ui::accent(), "ACE COMBAT 2");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + ImGui::GetFontSize() * 0.55f);
        ImGui::TextDisabled("native PC port");
        ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - ImGui::GetFontSize() * 10);
        statusDot(discOk(), "Disc");
        ImGui::SameLine();
        statusDot(!pads.empty(), pads.empty() ? "No gamepad" : "Gamepad", true);
    }

    void footer() {
        float h = ImGui::GetFontSize() * 2.6f;
        ImGui::BeginDisabled(!discOk());
        ImGui::PushStyleColor(ImGuiCol_Button, ui::accent());
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.38f, 0.70f, 1.0f, 1));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.18f, 0.48f, 0.85f, 1));
        ImGui::PushFont(ui::bold);
        if (ImGui::Button("PLAY", ImVec2(ImGui::GetFontSize() * 10, h))) { play = true; done = true; }
        ImGui::PopFont();
        ImGui::PopStyleColor(3);
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Save settings", ImVec2(0, h)))
            setStatus(cfg.save(cfgPath) ? "Settings saved." : "Could not write " + cfgPath);
        ImGui::SameLine();
        if (ImGui::Button("Quit", ImVec2(ImGui::GetFontSize() * 5, h))) done = true;
        ImGui::SameLine();
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (h - ImGui::GetTextLineHeight()) * 0.5f);
        if (!discOk()) ImGui::TextColored(ui::warn(), "Select the game disc in the Game tab.");
        else if (SDL_GetTicks() < statusUntil) ImGui::TextColored(ui::ok(), "%s", status.c_str());
        else ImGui::TextDisabled("Settings are saved when you press PLAY.");
    }

    void frame() {
        ImGuiIO& io = ImGui::GetIO();
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(io.DisplaySize);
        ImGui::Begin("##launcher", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                                ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        header();
        ImGui::Spacing();
        float footerH = ImGui::GetFontSize() * 2.6f + ImGui::GetStyle().ItemSpacing.y * 2 + 2;
        if (ImGui::BeginTabBar("tabs")) {
            static int forceTab = getenv("AC2_LAUNCHER_TAB") ? atoi(getenv("AC2_LAUNCHER_TAB")) : -1;
            struct Tab { const char* name; void (Launcher::*fn)(); } tabs[] = {
                {"  Game  ", &Launcher::tabGame}, {"  Video  ", &Launcher::tabVideo},
                {"  Controls  ", &Launcher::tabControls}, {"  About  ", &Launcher::tabAbout}};
            int idx = 0;
            for (auto& t : tabs) {
                ImGuiTabItemFlags fl = forceTab == idx++ ? ImGuiTabItemFlags_SetSelected : 0;
                if (ImGui::BeginTabItem(t.name, nullptr, fl)) {
                    ImGui::BeginChild("##page", ImVec2(0, -footerH));
                    ImGui::Dummy(ImVec2(0, 2));
                    (this->*t.fn)();
                    ImGui::EndChild();
                    ImGui::EndTabItem();
                }
            }
            ImGui::EndTabBar();
            forceTab = -1;
        }
        ImGui::Separator();
        footer();
        ImGui::End();
    }

    bool captureBinding(const SDL_Event& e) {
        if (waitAction < 0) return false;
        if (e.type == SDL_KEYDOWN) {
            if (waitKind == 0 && e.key.keysym.scancode != SDL_SCANCODE_ESCAPE) cfg.bind[waitAction].key = e.key.keysym.scancode;
            if (waitKind == 0 || e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) waitAction = -1;
            return true;
        }
        if (e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT) return true;
        if (waitKind == 1 && e.type == SDL_CONTROLLERBUTTONDOWN) { cfg.bind[waitAction].pad = e.cbutton.button; waitAction = -1; return true; }
        if (waitKind == 1 && e.type == SDL_CONTROLLERAXISMOTION && e.caxis.value > 20000) {
            if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERLEFT) { cfg.bind[waitAction].pad = PAD_LTRIG; waitAction = -1; }
            else if (e.caxis.axis == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) { cfg.bind[waitAction].pad = PAD_RTRIG; waitAction = -1; }
            return true;
        }
        return e.type == SDL_CONTROLLERBUTTONDOWN || e.type == SDL_CONTROLLERBUTTONUP || e.type == SDL_CONTROLLERAXISMOTION;
    }
};

bool run_launcher(Config& cfg, const std::string& cfgPath) {
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    SDL_DisplayMode desk{};
    SDL_GetDesktopDisplayMode(0, &desk);
    float scale = desk.h >= 2000 ? 2.0f : desk.h >= 1400 ? 1.5f : 1.0f;
    int ww = std::min((int)(1040 * scale), desk.w ? desk.w - 40 : 1040);
    int wh = std::min((int)(730 * scale), desk.h ? desk.h - 90 : 730);
    SDL_Window* win = SDL_CreateWindow("Ace Combat 2 - Launcher", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh,
                                       SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    if (!win) return false;
    setWindowIcon(win);
    SDL_SetWindowMinimumSize(win, 760, 520);
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    SDL_GL_MakeCurrent(win, ctx);
    SDL_GL_SetSwapInterval(1);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ui::setup(scale);
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330 core");
    padsRefresh();

    Launcher L{cfg, cfgPath};
    std::set<std::pair<int, int>> seen;
    for (int i = 0; i < SDL_GetNumDisplayModes(0); i++) {
        SDL_DisplayMode m;
        if (SDL_GetDisplayMode(0, i, &m) == 0 && m.w >= 640 && seen.insert({m.w, m.h}).second) L.modes.push_back({m.w, m.h});
    }
    std::sort(L.modes.begin(), L.modes.end(), [](Res a, Res b) { return a.w * a.h > b.w * b.h; });

    while (!L.done) {
        SDL_Event e;
        if (SDL_WaitEventTimeout(&e, 250)) {
            do {
                if (e.type == SDL_QUIT) L.done = true;
                if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) padsRefresh();
                if (!L.captureBinding(e)) ImGui_ImplSDL2_ProcessEvent(&e);
            } while (SDL_PollEvent(&e));
        }
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplSDL2_NewFrame();
        ImGui::NewFrame();
        L.frame();
        ImGui::Render();
        int dw, dh;
        SDL_GL_GetDrawableSize(win, &dw, &dh);
        glViewport(0, 0, dw, dh);
        glClearColor(0.055f, 0.071f, 0.098f, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(win);
    }
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    for (auto* p : pads) SDL_GameControllerClose(p);
    pads.clear();
    SDL_GL_DeleteContext(ctx);
    SDL_DestroyWindow(win);
    if (L.play) cfg.save(cfgPath);
    return L.play;
}
