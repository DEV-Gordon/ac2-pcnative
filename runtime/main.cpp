// Entry point: launcher, then the recompiled game.
#include "hw.h"
#include "config.h"

#include <SDL.h>
#include <unistd.h>

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

extern "C" {
void core_init(void);
void core_start(CPU* c, uint32_t entry);
uint32_t load_psx_exe(const uint8_t* exe, uint32_t* gp, uint32_t* sp);
void platform_init(void);
void platform_set_input_script(const char*);
void platform_record_wav(const char*);
int iso_open(const char* path);
uint8_t* iso_read_file(const char* name, uint32_t* size);
void bios_set_card_dir(const char* dir);
extern double g_speed;
extern int g_max_frames, g_shot_every, g_no_movies, g_trace_on, g_back_to_launcher;
extern const char* g_shot_prefix;
int platform_state_prepare(const char* path);
const char* platform_state_card_dir(void);
void platform_state_boot(const char* card_dir);
const char* platform_pending_state(void);
int movie_play(int start_lba, int nsectors);
}
bool run_launcher(Config& cfg, const std::string& cfgPath);

static std::string g_cfg_path;
static char** g_argv;

// Called when the game asks to quit (window closed, menu Quit / Back to launcher).
extern "C" void app_exit(void) {
    SDL_Quit();
    if (const char* st = platform_pending_state()) {
        // load a debug state: restart and replay its input
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            std::string path = st;
            std::vector<char*> args = {g_argv[0], (char*)"--play", (char*)"--config", (char*)g_cfg_path.c_str(),
                                       (char*)"--load-state", (char*)path.c_str()};
            if (g_log_level >= 2) args.push_back((char*)"-v");
            args.push_back(nullptr);
            execv(exe, args.data());
        }
    }
    if (g_back_to_launcher) {
        char exe[4096];
        ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
        if (n > 0) {
            exe[n] = 0;
            std::vector<char*> args = {g_argv[0], (char*)"--launcher", (char*)"--config", (char*)g_cfg_path.c_str(), nullptr};
            execv(exe, args.data());
        }
    }
    exit(0);
}

// A raw MODE2/2352 disc image whose ISO9660 volume descriptor (sector 16) is valid.
static bool looks_like_disc(const std::filesystem::path& p) {
    std::error_code ec;
    auto size = std::filesystem::file_size(p, ec);
    if (ec || size < (100u << 20) || size % 2352 != 0) return false;
    FILE* f = fopen(p.string().c_str(), "rb");
    if (!f) return false;
    char id[5] = {0};
    bool ok = fseek(f, 16L * 2352 + 25, SEEK_SET) == 0 && fread(id, 1, 5, f) == 5 && !memcmp(id, "CD001", 5);
    fclose(f);
    return ok;
}

static bool name_matches(const std::filesystem::path& p) {
    std::string n = p.filename().string(), e = p.extension().string();
    for (auto& c : n) c = (char)tolower((unsigned char)c);
    for (auto& c : e) c = (char)tolower((unsigned char)c);
    if (e != ".bin" && e != ".img") return false;
    return n.find("ace combat 2") != std::string::npos || n.find("ace_combat_2") != std::string::npos ||
           n.find("acecombat2") != std::string::npos;
}

// Looks for the disc image next to the program and in the usual places (a few folder levels deep).
// Called when the configured path does not exist (e.g. the disc was moved).
static std::string find_disc(void) {
    namespace fs = std::filesystem;
    std::vector<fs::path> roots;
    { char* b = SDL_GetBasePath(); fs::path base = b ? b : "./"; SDL_free(b);
      for (int up = 0; up < 3; up++) { roots.push_back(base); base = base.parent_path().parent_path(); if (base.empty()) break; } }
    if (const char* h = getenv("HOME"))
        for (auto d : {"Documents", "Games", "Downloads", "Desktop", "roms", "ROMs", "retroarch-files"}) roots.push_back(fs::path(h) / d);
    static const char* skip[] = {"work", "build", "gen", "third_party", ".git", "node_modules", "Trash"};
    for (auto& root : roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        int visited = 0;
        for (fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end; !ec && it != end; it.increment(ec)) {
            if (++visited > 30000) break;
            const fs::path& p = it->path();
            std::string name = p.filename().string();
            if (it->is_directory(ec)) {
                bool s = it.depth() >= 4 || (!name.empty() && name[0] == '.');
                for (auto k : skip) if (name == k) s = true;
                if (s) it.disable_recursion_pending();
                continue;
            }
            if (name_matches(p) && looks_like_disc(p)) return p.string();
        }
    }
    return "";
}

int main(int argc, char** argv) {
    g_argv = argv;
    int cli_scale = 0, cli_ws = 0;
    int cli_res = 0;
    const char* disc_arg = nullptr;
    const char* state_arg = nullptr;
    int movie_arg = -1;
    bool force_launcher = false, skip_launcher = false;
    for (int i = 1; i < argc; i++) {
        auto is = [&](const char* s) { return !strcmp(argv[i], s); };
        if (is("-v")) g_log_level = 2;
        else if (is("-vv")) g_log_level = 3;
        else if (is("--launcher")) force_launcher = true;
        else if (is("--play")) skip_launcher = true;
        else if (is("--config") && i + 1 < argc) g_cfg_path = argv[++i];
        else if (is("--speed") && i + 1 < argc) g_speed = atof(argv[++i]);
        else if (is("--frames") && i + 1 < argc) g_max_frames = atoi(argv[++i]);
        else if (is("--shot-every") && i + 2 < argc) { g_shot_every = atoi(argv[++i]); g_shot_prefix = argv[++i]; }
        else if (is("--input") && i + 1 < argc) platform_set_input_script(argv[++i]);
        else if (is("--wav") && i + 1 < argc) platform_record_wav(argv[++i]);
        else if (is("--no-movies")) g_no_movies = 1;
        else if (is("--scale") && i + 1 < argc) { int v = atoi(argv[++i]); g_cfg.internalRes = v >= 4 ? 2 : v >= 2 ? 1 : 0; cli_scale = 1; }
        else if (is("--widescreen")) { g_cfg.widescreen = true; cli_ws = 1; }
        else if (is("--trace")) g_trace_on = 1;
        else if (is("--load-state") && i + 1 < argc) { state_arg = argv[++i]; skip_launcher = true; }
        else if (is("--play-movie") && i + 1 < argc) { movie_arg = atoi(argv[++i]); skip_launcher = true; }
        else if (argv[i][0] != '-') disc_arg = argv[i];
        else { printf("usage: ac2native [--launcher|--play] [--config file] [disc.bin]\n"); return 1; }
    }
    // a disc on the command line means "just play" (tests, scripts)
    if (disc_arg) skip_launcher = true;

    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    if (g_cfg_path.empty()) g_cfg_path = prefDir() + "config.ini";
    g_cfg.setDefaults();
    int keep_res = g_cfg.internalRes; bool keep_ws = g_cfg.widescreen;
    bool had = g_cfg.load(g_cfg_path);
    if (cli_scale) g_cfg.internalRes = keep_res;
    if (cli_ws) g_cfg.widescreen = keep_ws;
    (void)cli_res;
    if (disc_arg) g_cfg.discPath = disc_arg;
    if (!fileExists(g_cfg.discPath)) g_cfg.discPath = find_disc();
    if (g_cfg.cardDir.empty()) g_cfg.cardDir = prefDir() + "card0";

    if (force_launcher || (!skip_launcher && (g_cfg.showLauncher || !had)))
        if (!run_launcher(g_cfg, g_cfg_path)) { SDL_Quit(); return 0; }
    if (!g_cfg.movies) g_no_movies = 1;

    if (!iso_open(g_cfg.discPath.c_str())) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ace Combat 2", ("Cannot open the disc image:\n" + g_cfg.discPath).c_str(), nullptr);
        return 1;
    }
    uint32_t size = 0;
    uint8_t* exe = iso_read_file("SLUS_004.04", &size);
    if (!exe || memcmp(exe, "PS-X EXE", 8)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ace Combat 2",
                                 "This disc image is not Ace Combat 2 (USA) (SLUS_004.04 not found).", nullptr);
        return 1;
    }

    if (state_arg && !platform_state_prepare(state_arg)) {
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Ace Combat 2", (std::string("Cannot load the state:\n") + state_arg).c_str(), nullptr);
        state_arg = nullptr;
    }
    core_init();
    platform_init();
    gpu_set_scale(g_cfg.internalRes == 2 ? 4 : g_cfg.internalRes == 1 ? 2 : 1);
    { extern int g_gte_widescreen; g_gte_widescreen = g_cfg.widescreen; }
    gpu_init();
    if (movie_arg >= 0) {
        // debug: play one of the four STR movies (0 = intro) straight from ACE2.STH, then quit
        uint32_t sth_size = 0;
        uint8_t* sth = iso_read_file("ACE2.STH", &sth_size);
        int seen = 0;
        for (uint32_t k = 0; sth && k + 8 <= sth_size; k += 8) {
            uint32_t a, b;
            memcpy(&a, sth + k, 4);
            memcpy(&b, sth + k + 4, 4);
            if (a >> 28 != 8 || seen++ != movie_arg) continue;
            const int stp_lba = 47884;   // ACE2.STP
            movie_play(stp_lba + (int)b, (int)(a & 0x0FFFFFFF));
            break;
        }
        SDL_Quit();
        return 0;
    }
    spu_init();
    cd_init(g_cfg.discPath.c_str());
    bios_set_card_dir(g_cfg.cardDir.c_str());
    platform_state_boot(g_cfg.cardDir.c_str());
    if (const char* replay_dir = platform_state_card_dir()) bios_set_card_dir(replay_dir);

    static CPU cpu;
    bios_init(&cpu);
    uint32_t gp = 0, sp = 0x801FFF00;   // STACK from SYSTEM.CNF
    uint32_t entry = load_psx_exe(exe, &gp, &sp);
    cpu.r[28] = gp;
    cpu.r[29] = cpu.r[30] = sp;
    core_start(&cpu, entry);
    return 0;
}
