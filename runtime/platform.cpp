// SDL2 + OpenGL platform layer: window, presentation of the PS1 display, audio, input, pause menu.
#include "hw.h"
#include "config.h"
#include "ui.h"

#include "imgui.h"
#include "imgui_impl_opengl3.h"
#include "imgui_impl_sdl2.h"

#include <SDL.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <dirent.h>
#include <sys/stat.h>
#include <ctime>

extern "C" {
double g_speed = 1.0;                  // time scale (debug)
int g_max_frames = 0;                  // debug: quit after N vblanks
int g_shot_every = 0;                  // debug: screenshot every N vblanks
const char* g_shot_prefix = nullptr;
int g_back_to_launcher = 0;
int g_replaying = 0;                   // fast-forwarding a debug state
}

static SDL_Window* win;
static SDL_GLContext ctx;
static SDL_AudioDeviceID audio;
static GLuint tex, tex16, cur_tex, prog, vao;   // tex: RGBA8 images, tex16: VRAM uploaded as-is
static std::vector<uint32_t> pixels;
static int img_w = 320, img_h = 240;   // last presented image
static int frame;
static void toast(const std::string& msg);
// A pad input that already reads as held when the controller is opened (a badly mapped trigger or button,
// common with Bluetooth Xbox pads on generic drivers) is ignored until it is released once; otherwise it
// would hold that PS1 button forever and swallow the keyboard's presses of it too.
struct Pad { SDL_GameController* c; uint64_t blocked; };
static std::vector<Pad> pads;
enum { IN_LTRIG = SDL_CONTROLLER_BUTTON_MAX, IN_RTRIG, IN_STICK_UP, IN_STICK_DOWN, IN_STICK_LEFT, IN_STICK_RIGHT, IN_COUNT };
static bool pad_raw(SDL_GameController* c, int in) {
    switch (in) {
        case IN_LTRIG: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 12000;
        case IN_RTRIG: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) > 12000;
        case IN_STICK_UP: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) < -14000;
        case IN_STICK_DOWN: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY) > 14000;
        case IN_STICK_LEFT: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) < -14000;
        case IN_STICK_RIGHT: return SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX) > 14000;
        default: return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)in);
    }
}
static const char* pad_in_name(int in) {
    static const char* extra[] = {"lefttrigger", "righttrigger", "stick-up", "stick-down", "stick-left", "stick-right"};
    return in < SDL_CONTROLLER_BUTTON_MAX ? SDL_GameControllerGetStringForButton((SDL_GameControllerButton)in)
                                          : extra[in - SDL_CONTROLLER_BUTTON_MAX];
}
static bool pad_down(Pad& p, int in) {
    bool d = pad_raw(p.c, in);
    if (p.blocked >> in & 1) {
        if (d) return false;
        p.blocked &= ~(1ull << in);
        LOGI("pad: %s released, now usable", pad_in_name(in));
    }
    return d;
}
static uint16_t buttons = 0xFFFF;
static bool paused;

static void pads_refresh(void) {
    for (auto& p : pads) SDL_GameControllerClose(p.c);
    pads.clear();
    SDL_GameControllerUpdate();
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (!SDL_IsGameController(i)) {
            LOGI("pad: '%s' has no controller mapping, ignored", SDL_JoystickNameForIndex(i));
            continue;
        }
        SDL_GameController* c = SDL_GameControllerOpen(i);
        if (!c) continue;
        Pad p = {c, 0};
        for (int in = 0; in < IN_COUNT; in++)
            if (pad_raw(c, in)) {
                p.blocked |= 1ull << in;
                LOGI("pad: %s reads as held at connect, ignored until released", pad_in_name(in));
            }
        char* m = SDL_GameControllerMapping(c);
        LOGI("pad: '%s' connected", SDL_GameControllerName(c));
        if (dbg_env("AC2_INPUTLOG")) LOGI("pad mapping: %s", m ? m : "(none)");
        SDL_free(m);
        pads.push_back(p);
    }
}

// ------------------------------------------------------------------ debug input script
struct ScriptPress { int frame, bit, len; };
static std::vector<ScriptPress> script;
extern "C" void platform_set_input_script(const char* s) {
    static const char* names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                    "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
    std::string str = s;
    size_t i = 0;
    while (i < str.size()) {
        size_t e = str.find(',', i);
        std::string tok = str.substr(i, e == std::string::npos ? std::string::npos : e - i);
        i = e == std::string::npos ? str.size() : e + 1;
        int f = 0, len = 6;
        char name[32] = {0};
        if (sscanf(tok.c_str(), "%d:%31[a-z0-9]:%d", &f, name, &len) < 2) continue;
        for (int b = 0; b < 16; b++)
            if (!strcmp(name, names[b])) script.push_back({f, b, len});
    }
}

// ------------------------------------------------------------------ GL
#define GLF(T, N) static T N;
GLF(PFNGLCREATESHADERPROC, pglCreateShader)
GLF(PFNGLSHADERSOURCEPROC, pglShaderSource)
GLF(PFNGLCOMPILESHADERPROC, pglCompileShader)
GLF(PFNGLCREATEPROGRAMPROC, pglCreateProgram)
GLF(PFNGLATTACHSHADERPROC, pglAttachShader)
GLF(PFNGLLINKPROGRAMPROC, pglLinkProgram)
GLF(PFNGLUSEPROGRAMPROC, pglUseProgram)
GLF(PFNGLGENVERTEXARRAYSPROC, pglGenVertexArrays)
GLF(PFNGLBINDVERTEXARRAYPROC, pglBindVertexArray)
GLF(PFNGLGETUNIFORMLOCATIONPROC, pglGetUniformLocation)
GLF(PFNGLUNIFORM4FPROC, pglUniform4f)

static const char* VS = R"(#version 330
uniform vec4 rect;      // u scale, v scale
out vec2 uv;
void main() {
    vec2 p = vec2(gl_VertexID & 1, gl_VertexID >> 1);
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
    uv = vec2(p.x * rect.x, (1.0 - p.y) * rect.y);
})";
// Sharp bilinear when enlarging, area average when shrinking.
static const char* FS = R"(#version 330
uniform sampler2D tex;
uniform vec4 src;       // image size (xy), texture size (zw)
uniform vec4 dst;       // viewport size (xy), mode (z): 0 sharp, 1 smooth, 2 pixelated, w: 1 = swap R/B
in vec2 uv;
out vec4 color;
vec2 clampUV(vec2 p) { vec2 h = 0.5 / src.zw; return clamp(p, h, src.xy / src.zw - h); }
vec3 areaSample(vec2 p) {
    vec2 foot = max(src.xy / dst.xy, vec2(1.0));
    vec2 n = clamp(ceil(foot), vec2(1.0), vec2(4.0));
    vec3 acc = vec3(0.0);
    for (int y = 0; y < 4; y++) for (int x = 0; x < 4; x++) {
        if (float(x) >= n.x || float(y) >= n.y) continue;
        acc += texture(tex, clampUV(p + ((vec2(x, y) + 0.5) / n - 0.5) * foot / src.zw)).rgb;
    }
    return acc / (n.x * n.y);
}
vec3 pick() {
    int mode = int(dst.z + 0.5);
    if (mode == 2) return texelFetch(tex, ivec2(min(uv * src.zw, src.xy - 1.0)), 0).rgb;
    if (src.x > dst.x + 0.5 || src.y > dst.y + 0.5) return areaSample(uv);
    if (mode == 1) return texture(tex, clampUV(uv)).rgb;
    vec2 scale = max(floor(dst.xy / src.xy), vec2(1.0));
    vec2 texel = uv * src.zw;
    vec2 region = 0.5 - 0.5 / scale;
    vec2 c = fract(texel) - 0.5;
    vec2 f = (c - clamp(c, -region, region)) * scale + 0.5;
    return texture(tex, clampUV((floor(texel) + f) / src.zw)).rgb;
}
void main() {
    vec3 c = pick();
    color = vec4(dst.w > 0.5 ? c.bgr : c, 1.0);
})";

static GLuint shader(GLenum t, const char* s) {
    GLuint h = pglCreateShader(t);
    pglShaderSource(h, 1, &s, nullptr);
    pglCompileShader(h);
    return h;
}

#define TEX_W 4096
#define TEX_H 2048

extern "C" void platform_init(void) {
    SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");   // let Ctrl+C / timeout kill the process directly
    SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER);
    SDL_GL_ResetAttributes();
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 3);
    const Config& c = g_cfg;
    Uint32 flags = SDL_WINDOW_OPENGL | SDL_WINDOW_ALLOW_HIGHDPI;
    int w = c.winW, h = c.winH;
    if (c.windowMode == WM_FULLSCREEN) { flags |= SDL_WINDOW_FULLSCREEN; w = c.fsW; h = c.fsH; }
    else if (c.windowMode == WM_BORDERLESS) flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
    else flags |= SDL_WINDOW_RESIZABLE;
    win = SDL_CreateWindow("Ace Combat 2", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, w, h, flags);
    setWindowIcon(win);
    ctx = SDL_GL_CreateContext(win);
    SDL_GL_SetSwapInterval(c.vsync && g_speed <= 1.0 ? 1 : 0);
    if (c.windowMode != WM_WINDOWED) SDL_ShowCursor(SDL_DISABLE);
#define L(N) N = (decltype(N))SDL_GL_GetProcAddress(#N + 1);
    L(pglCreateShader) L(pglShaderSource) L(pglCompileShader) L(pglCreateProgram) L(pglAttachShader)
    L(pglLinkProgram) L(pglUseProgram) L(pglGenVertexArrays) L(pglBindVertexArray) L(pglGetUniformLocation)
    L(pglUniform4f)
    prog = pglCreateProgram();
    pglAttachShader(prog, shader(GL_VERTEX_SHADER, VS));
    pglAttachShader(prog, shader(GL_FRAGMENT_SHADER, FS));
    pglLinkProgram(prog);
    pglUseProgram(prog);
    pglGenVertexArrays(1, &vao);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, TEX_W, TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    // VRAM goes up as-is: the PS1 15-bit format (R in the low bits) read as BGRA 1_5_5_5_REV, the layout GPUs
    // support natively, with R and B swapped back in the shader
    glGenTextures(1, &tex16);
    glBindTexture(GL_TEXTURE_2D, tex16);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB5_A1, TEX_W, TEX_H, 0, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    cur_tex = tex;
    pixels.resize((size_t)TEX_W * TEX_H);

    // pause menu UI
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    int ww, wh;
    SDL_GetWindowSize(win, &ww, &wh);
    ui::setup(std::clamp(wh / 720.0f, 1.0f, 2.5f));
    ImGui_ImplSDL2_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 330");

    SDL_AudioSpec want{}, have{};
    want.freq = 44100;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    audio = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (audio) SDL_PauseAudioDevice(audio, 0);
    pads_refresh();
}

// ------------------------------------------------------------------ audio
static FILE* wav;
static uint32_t wav_frames;
static void wav_close(void) {
    if (!wav) return;
    uint32_t data = wav_frames * 4, riff = 36 + data;
    fseek(wav, 4, SEEK_SET); fwrite(&riff, 4, 1, wav);
    fseek(wav, 40, SEEK_SET); fwrite(&data, 4, 1, wav);
    fclose(wav);
    wav = nullptr;
}
extern "C" void platform_record_wav(const char* path) {
    wav = fopen(path, "wb");
    if (!wav) return;
    const uint8_t hdr[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                             0x44,0xAC,0,0,0x10,0xB1,2,0,4,0,16,0,'d','a','t','a',0,0,0,0};
    fwrite(hdr, 1, 44, wav);
    atexit(wav_close);
}

extern "C" void platform_audio_push(const int16_t* lr, int frames) {
    if (g_replaying) return;
    if (wav) { fwrite(lr, 4, frames, wav); wav_frames += frames; }
    if (!audio || paused) return;
    if (SDL_GetQueuedAudioSize(audio) > 44100 * 4 / 5) return;   // keep latency bounded
    if (g_cfg.volume >= 100) { SDL_QueueAudio(audio, lr, (Uint32)(frames * 4)); return; }
    static std::vector<int16_t> tmp;
    tmp.assign(lr, lr + frames * 2);
    for (auto& s : tmp) s = (int16_t)(s * g_cfg.volume / 100);
    SDL_QueueAudio(audio, tmp.data(), (Uint32)(frames * 4));
}

extern "C" int platform_audio_queued_ms(void) {
    return audio ? (int)(SDL_GetQueuedAudioSize(audio) / 4 * 1000 / 44100) : 0;
}

// ------------------------------------------------------------------ presentation
static void write_ppm(const char* path, const uint32_t* px, int w, int h, int stride) {
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            uint32_t p = px[y * stride + x];
            unsigned char rgb[3] = {(unsigned char)p, (unsigned char)(p >> 8), (unsigned char)(p >> 16)};
            fwrite(rgb, 1, 3, f);
        }
    fclose(f);
}

static bool is_fullscreen(void) { return SDL_GetWindowFlags(win) & (SDL_WINDOW_FULLSCREEN | SDL_WINDOW_FULLSCREEN_DESKTOP); }
static void toggle_fullscreen(void) {
    if (is_fullscreen()) SDL_SetWindowFullscreen(win, 0);
    else SDL_SetWindowFullscreen(win, g_cfg.windowMode == WM_FULLSCREEN ? SDL_WINDOW_FULLSCREEN : SDL_WINDOW_FULLSCREEN_DESKTOP);
}

// draws the current image (pixels, img_w x img_h; display aspect from `ar`)
static double cur_ar = 4.0 / 3.0;
static void draw_image(void) {
    int ww, wh;
    SDL_GL_GetDrawableSize(win, &ww, &wh);
    glViewport(0, 0, ww, wh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    double ar = g_cfg.aspect == AR_16_9 ? 16.0 / 9.0 : g_cfg.aspect == AR_STRETCH ? (double)ww / wh : cur_ar;
    if (g_cfg.widescreen && g_cfg.aspect == AR_4_3) ar = 16.0 / 9.0;
    int vw = ww, vh = (int)lround(ww / ar);
    if (vh > wh) { vh = wh; vw = (int)lround(wh * ar); }
    glViewport((ww - vw) / 2, (wh - vh) / 2, vw, vh);
    pglUseProgram(prog);
    pglBindVertexArray(vao);
    glBindTexture(GL_TEXTURE_2D, cur_tex);
    pglUniform4f(pglGetUniformLocation(prog, "rect"), (float)img_w / TEX_W, (float)img_h / TEX_H, 0, 0);
    pglUniform4f(pglGetUniformLocation(prog, "src"), (float)img_w, (float)img_h, TEX_W, TEX_H);
    pglUniform4f(pglGetUniformLocation(prog, "dst"), (float)vw, (float)vh, (float)g_cfg.screenFilter, cur_tex == tex16 ? 1.0f : 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

static void upload_image(int w, int h) {
    img_w = w;
    img_h = h;
    cur_tex = tex;
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_UNPACK_ROW_LENGTH, TEX_W);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels.data());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

static void capture_frame(const DisplayInfo& d) {
    const uint16_t* vram = gpu_vram();
    const int S = gpu_scale(), VW = 1024 * S;
    int nw = std::min(d.w, 640), nh = std::min(d.h, 512);
    int w = nw * S, h = nh * S;
    // the display aspect is always 4:3 (the PS1 stretches every mode to the TV)
    cur_ar = 4.0 / 3.0;
    if (!d.enabled) {
        for (int y = 0; y < h; y++) std::fill(&pixels[(size_t)y * TEX_W], &pixels[(size_t)y * TEX_W + w], 0xFF000000u);
    } else if (d.rgb24) {
        // 24-bit mode (still images): decode at native resolution and replicate
        static std::vector<uint16_t> row(1024);
        for (int y = 0; y < nh; y++) {
            for (int x = 0; x < 1024; x++) row[x] = vram[(size_t)(((d.y + y) & 511) * S) * VW + x * S];
            const uint8_t* rb = (const uint8_t*)row.data();
            for (int x = 0; x < nw; x++) {
                int bx = (d.x * 2 + x * 3) % 2048;
                uint32_t px = 0xFF000000u | rb[bx] | (rb[(bx + 1) % 2048] << 8) | (rb[(bx + 2) % 2048] << 16);
                for (int j = 0; j < S; j++)
                    for (int i = 0; i < S; i++) pixels[(size_t)(y * S + j) * TEX_W + x * S + i] = px;
            }
        }
    } else if (!(g_shot_every && g_shot_prefix && frame % g_shot_every == 0) && d.x * S + w <= VW &&
               d.y * S + h <= 512 * S) {
        // common case: hand the display area of the VRAM straight to GL (screenshots take the path below)
        img_w = w;
        img_h = h;
        cur_tex = tex16;
        glBindTexture(GL_TEXTURE_2D, tex16);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, VW);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV,
                        vram + (size_t)d.y * S * VW + (size_t)d.x * S);
        glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
        return;
    } else {
        static std::vector<uint32_t> lut;   // 15-bit BGR -> RGBA8 (the mask bit is ignored)
        if (lut.empty()) {
            lut.resize(32768);
            for (uint32_t c = 0; c < 32768; c++) {
                uint32_t r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
                lut[c] = 0xFF000000u | (r | (r >> 5)) | ((g | (g >> 5)) << 8) | ((b | (b >> 5)) << 16);
            }
        }
        const uint32_t* L = lut.data();
        const int x0 = d.x * S;
        for (int y = 0; y < h; y++) {
            const uint16_t* src = &vram[(size_t)((d.y * S + y) & (512 * S - 1)) * VW];
            uint32_t* dst = &pixels[(size_t)y * TEX_W];
            if (x0 + w <= VW)
                for (int x = 0; x < w; x++) dst[x] = L[src[x0 + x] & 0x7FFF];
            else
                for (int x = 0; x < w; x++) dst[x] = L[src[(x0 + x) & (VW - 1)] & 0x7FFF];
        }
    }
    upload_image(w, h);
    if (g_shot_every && g_shot_prefix && frame % g_shot_every == 0) {
        char path[512];
        snprintf(path, sizeof path, "%s%05d.ppm", g_shot_prefix, frame);
        write_ppm(path, pixels.data(), w, h, TEX_W);
        if (dbg_env("AC2_VRAM")) {
            std::vector<uint32_t> v(1024 * 512);
            for (int i = 0; i < 1024 * 512; i++) {
                uint16_t c = vram[(size_t)((i / 1024) * S) * VW + (i % 1024) * S];
                v[i] = ((c & 31) << 3) | (((c >> 5) & 31) << 11) | (((c >> 10) & 31) << 19);
            }
            snprintf(path, sizeof path, "%s%05d_vram.ppm", g_shot_prefix, frame);
            write_ppm(path, v.data(), 1024, 512, 1024);
        }
    }
}

// Draws an RGB24 image (e.g. an FMV frame) centred in a 4:3 screen of 320x240 "PS1 pixels".
extern "C" void platform_present_rgb24(const uint8_t* rgb, int w, int h) {
    int cw = std::max(w, 320), ch = std::max(h, 240);
    for (int y = 0; y < ch; y++) std::fill(&pixels[(size_t)y * TEX_W], &pixels[(size_t)y * TEX_W + cw], 0xFF000000u);
    int ox = (cw - w) / 2, oy = (ch - h) / 2;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const uint8_t* p = rgb + (y * w + x) * 3;
            pixels[(size_t)(oy + y) * TEX_W + ox + x] = 0xFF000000u | p[0] | (p[1] << 8) | (p[2] << 16);
        }
    cur_ar = 4.0 / 3.0;
    upload_image(cw, ch);
    draw_image();
    SDL_GL_SwapWindow(win);
    static int movie_frame;   // movie frames do not advance the VBlank counter (keeps --input deterministic)
    movie_frame++;
    if (g_shot_every && g_shot_prefix && movie_frame % (g_shot_every * 4) == 0) {
        char path[512];
        snprintf(path, sizeof path, "%smovie_%05d.ppm", g_shot_prefix, movie_frame);
        write_ppm(path, pixels.data(), cw, ch, TEX_W);
    }
}

// ------------------------------------------------------------------ debug states (input replay)
// A recompiled game cannot snapshot its native call stack, so a state is "boot + every input since": the
// buttons the game saw at each VBlank (stored as changes) and the memory card as it was at boot. Loading
// restarts the program and replays that input as fast as possible without drawing or sound; emulation is
// deterministic (virtual clock), so it arrives at exactly the saved moment.
struct InputChange { uint32_t vblank; uint16_t buttons; };
struct CardFile { std::string suffix, name, data; };   // suffix "" = card 1, "_2" = card 2
static std::vector<InputChange> rec;
static std::vector<CardFile> boot_card;
static uint16_t game_buttons = 0xFFFF;
static uint64_t replay_end;
static size_t replay_pos;
static std::string real_card_dir, pending_load, replay_tmp;
static int state_widescreen;
static uint16_t buttons_live(void);

static std::string state_path(int slot) { return prefDir() + "states/slot" + std::to_string(slot) + ".state"; }

static void read_card_dir(const std::string& dir, const std::string& suffix) {
    DIR* d = opendir((dir + suffix).c_str());
    if (!d) return;
    while (dirent* e = readdir(d)) {
        if (e->d_name[0] == '.') continue;
        std::string path = dir + suffix + "/" + e->d_name;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) continue;
        CardFile cf{suffix, e->d_name, ""};
        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof buf, f)) > 0) cf.data.append(buf, n);
        fclose(f);
        boot_card.push_back(cf);
    }
    closedir(d);
}

// Normal start: remember the card as the game will first see it.
extern "C" void platform_state_boot(const char* card_dir) {
    real_card_dir = card_dir;
    if (g_replaying) return;
    boot_card.clear();
    read_card_dir(real_card_dir, "");
    read_card_dir(real_card_dir, "_2");
}

static bool state_save(int slot) {
    if (g_replaying) return false;
    uint64_t now = core_vblank_count();
    mkdir((prefDir() + "states").c_str(), 0755);
    FILE* f = fopen(state_path(slot).c_str(), "wb");
    if (!f) { toast("Could not write state " + std::to_string(slot)); return false; }
    fprintf(f, "AC2STATE 1\nvblank %llu\nwidescreen %d\ntime %lld\ncards %zu\n", (unsigned long long)now,
            (int)g_cfg.widescreen, (long long)time(nullptr), boot_card.size());
    for (auto& c : boot_card) {
        fprintf(f, "%s|%s|%zu\n", c.suffix.c_str(), c.name.c_str(), c.data.size());
        fwrite(c.data.data(), 1, c.data.size(), f);
    }
    size_t n = 0;
    while (n < rec.size() && rec[n].vblank <= now) n++;
    fprintf(f, "inputs %zu\n", n);
    for (size_t i = 0; i < n; i++) {
        uint8_t b[6] = {(uint8_t)rec[i].vblank, (uint8_t)(rec[i].vblank >> 8), (uint8_t)(rec[i].vblank >> 16),
                        (uint8_t)(rec[i].vblank >> 24), (uint8_t)rec[i].buttons, (uint8_t)(rec[i].buttons >> 8)};
        fwrite(b, 1, 6, f);
    }
    fclose(f);
    LOGI("state: saved slot %d at VBlank %llu (%zu input changes)", slot, (unsigned long long)now, n);
    toast("State " + std::to_string(slot) + " saved");
    return true;
}

struct StateInfo { bool ok; uint64_t vblank; long long when; int widescreen; };
static StateInfo state_read(const std::string& path, bool full) {
    StateInfo si{false, 0, 0, 0};
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return si;
    char line[1024];
    unsigned long long vb = 0;
    long long when = 0;
    size_t ncards = 0, ninputs = 0;
    int ws = 0;
    bool ok = fgets(line, sizeof line, f) && !strncmp(line, "AC2STATE 1", 10) &&
              fgets(line, sizeof line, f) && sscanf(line, "vblank %llu", &vb) == 1 &&
              fgets(line, sizeof line, f) && sscanf(line, "widescreen %d", &ws) == 1 &&
              fgets(line, sizeof line, f) && sscanf(line, "time %lld", &when) == 1 &&
              fgets(line, sizeof line, f) && sscanf(line, "cards %zu", &ncards) == 1;
    if (ok && full) {
        boot_card.clear();
        for (size_t i = 0; ok && i < ncards; i++) {
            ok = fgets(line, sizeof line, f) != nullptr;
            char* a = strchr(line, '|');
            char* b = a ? strchr(a + 1, '|') : nullptr;
            if (!ok || !a || !b) { ok = false; break; }
            CardFile cf{std::string(line, a), std::string(a + 1, b), std::string(strtoull(b + 1, nullptr, 10), '\0')};
            ok = fread(&cf.data[0], 1, cf.data.size(), f) == cf.data.size();
            boot_card.push_back(cf);
        }
        ok = ok && fgets(line, sizeof line, f) && sscanf(line, "inputs %zu", &ninputs) == 1;
        rec.clear();
        for (size_t i = 0; ok && i < ninputs; i++) {
            uint8_t b[6];
            ok = fread(b, 1, 6, f) == 6;
            rec.push_back({(uint32_t)(b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24), (uint16_t)(b[4] | b[5] << 8)});
        }
    }
    fclose(f);
    si = {ok, vb, when, ws};
    return si;
}

// Called from main() when started with --load-state: loads the state and arranges the replay.
extern "C" int platform_state_prepare(const char* path) {
    StateInfo si = state_read(path, true);
    if (!si.ok) { LOGE("state: cannot read %s", path); return 0; }
    replay_end = si.vblank;
    state_widescreen = si.widescreen;
    g_cfg.widescreen = si.widescreen != 0;   // the projection must match the recording
    g_replaying = 1;
    g_gpu_skip_draw = 1;
    LOGI("state: replaying %zu input changes up to VBlank %llu", rec.size(), (unsigned long long)replay_end);
    return 1;
}

// After the card directory is known: the replay plays against a copy of the card as it was at boot.
extern "C" const char* platform_state_card_dir(void) {
    static std::string dir;
    if (!g_replaying) return nullptr;
    dir = std::string(getenv("XDG_RUNTIME_DIR") ? getenv("XDG_RUNTIME_DIR") : "/tmp") + "/ac2native-replay-" +
          std::to_string(getpid()) + "/card0";
    std::string base = dir.substr(0, dir.size() - 6);
    replay_tmp = base;
    mkdir(base.c_str(), 0700);
    mkdir(dir.c_str(), 0700);
    mkdir((dir + "_2").c_str(), 0700);
    for (auto& c : boot_card) {
        FILE* f = fopen((dir + c.suffix + "/" + c.name).c_str(), "wb");
        if (f) { fwrite(c.data.data(), 1, c.data.size(), f); fclose(f); }
    }
    return dir.c_str();
}

extern "C" void bios_set_card_dir(const char* dir);
static void replay_finish(void) {
    g_replaying = 0;
    g_gpu_skip_draw = 0;
    bios_set_card_dir(real_card_dir.c_str());
    if (!replay_tmp.empty()) {   // the replay's card copy is no longer needed (open files survive unlinking)
        std::string cmd = "rm -rf '" + replay_tmp + "'";
        if (system(cmd.c_str()) != 0) LOGD("state: could not remove %s", replay_tmp.c_str());
    }
    core_pace_reset();
    LOGI("state: reached VBlank %llu, playing", (unsigned long long)replay_end);
    toast("State loaded");
}

extern "C" void platform_latch_input(uint64_t vblank) {
    if (g_replaying) {
        while (replay_pos < rec.size() && rec[replay_pos].vblank <= vblank) game_buttons = rec[replay_pos++].buttons;
        if (vblank + 30 >= replay_end) g_gpu_skip_draw = 0;   // redraw a few frames before handing over
        if (vblank >= replay_end) replay_finish();
        return;
    }
    uint16_t b = buttons_live();
    if (rec.empty() || b != game_buttons) rec.push_back({(uint32_t)vblank, b});
    game_buttons = b;
}

extern "C" uint16_t platform_game_buttons(int port) { return port == 0 ? game_buttons : 0xFFFF; }

static void state_request_load(int slot) {
    if (g_replaying) return;
    if (!state_read(state_path(slot), false).ok) { toast("State " + std::to_string(slot) + " is empty"); return; }
    pending_load = state_path(slot);
    g_quit = 1;   // app_exit() restarts the program with --load-state
}
extern "C" const char* platform_pending_state(void) { return pending_load.empty() ? nullptr : pending_load.c_str(); }

// ------------------------------------------------------------------ input
static bool binding_down(int a) {
    const Binding& b = g_cfg.bind[a];
    const Uint8* k = SDL_GetKeyboardState(nullptr);
    if (b.key != SDL_SCANCODE_UNKNOWN && k[b.key]) return true;
    if (b.pad == PAD_NONE) return false;
    int in = b.pad == PAD_LTRIG ? IN_LTRIG : b.pad == PAD_RTRIG ? IN_RTRIG : b.pad;
    if (in < 0 || in >= IN_COUNT) return false;
    for (auto& p : pads)
        if (pad_down(p, in)) return true;
    return false;
}

static void set_paused(bool on);

static int poll_input(void) {
    SDL_Event e;
    int closed = 0;
    while (SDL_PollEvent(&e)) {
        if (paused) ImGui_ImplSDL2_ProcessEvent(&e);
        if (e.type == SDL_QUIT) closed = 1;
        if (e.type == SDL_CONTROLLERDEVICEADDED || e.type == SDL_CONTROLLERDEVICEREMOVED) pads_refresh();
        bool menu = (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == g_cfg.bind[A_MENU].key) ||
                    (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == g_cfg.bind[A_MENU].pad);
        bool fs = (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == g_cfg.bind[A_FULLSCREEN].key) ||
                  (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == g_cfg.bind[A_FULLSCREEN].pad);
        if (menu) set_paused(!paused);
        if (fs) toggle_fullscreen();
        if (paused && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_B) set_paused(false);
        if (e.type == SDL_WINDOWEVENT && e.window.event == SDL_WINDOWEVENT_FOCUS_LOST && !g_max_frames) set_paused(true);
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F5) state_save(1);
        if (e.type == SDL_KEYDOWN && !e.key.repeat && e.key.keysym.scancode == SDL_SCANCODE_F9) state_request_load(1);
    }
    uint16_t b = 0xFFFF;
    if (!paused && script.empty()) {   // a test script drives the pad alone (keeps replays deterministic)
        for (int a = 0; a < A_FIRST_HOTKEY; a++)
            if (binding_down(a)) b &= ~(1u << kPsxBit[a]);
        if (g_cfg.stickAsDpad)
            for (auto& p : pads) {
                if (pad_down(p, IN_STICK_UP)) b &= ~(1u << 4);
                if (pad_down(p, IN_STICK_DOWN)) b &= ~(1u << 6);
                if (pad_down(p, IN_STICK_LEFT)) b &= ~(1u << 7);
                if (pad_down(p, IN_STICK_RIGHT)) b &= ~(1u << 5);
            }
    }
    for (auto& p : script)
        if (frame >= p.frame && frame < p.frame + p.len) b &= ~(1u << p.bit);
    if (b != buttons && dbg_env("AC2_INPUTLOG")) {
        static const char* names[16] = {"select", "l3", "r3", "start", "up", "right", "down", "left",
                                        "l2", "r2", "l1", "r1", "triangle", "circle", "cross", "square"};
        char held[160] = "";
        for (int i = 0; i < 16; i++)
            if (!(b >> i & 1)) { strcat(held, " "); strcat(held, names[i]); }
        LOGI("input frame %d:%s", frame, held[0] ? held : " (none)");
    }
    buttons = b;
    return closed;
}

extern "C" int platform_poll(void) { return poll_input(); }
extern "C" uint16_t platform_pad_buttons(int port) { return port == 0 ? buttons : 0xFFFF; }
static uint16_t buttons_live(void) { return buttons; }

// ------------------------------------------------------------------ pause menu
static void set_paused(bool on) {
    if (on == paused) return;
    paused = on;
    if (audio) {
        SDL_PauseAudioDevice(audio, on ? 1 : 0);
        if (on) SDL_ClearQueuedAudio(audio);
    }
    SDL_ShowCursor(on || !is_fullscreen() ? SDL_ENABLE : SDL_DISABLE);
}

static void pause_menu_frame(void) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, 150));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 22, 0));
    ImGui::Begin("##pause", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_AlwaysAutoResize);
    ImGui::PushFont(ui::title);
    ImGui::TextUnformatted("Paused");
    ImGui::PopFont();
    ImGui::TextDisabled("Ace Combat 2");
    ImGui::Spacing();
    const ImVec2 full(-FLT_MIN, 0);
    static bool focus = true;
    if (focus) { ImGui::SetKeyboardFocusHere(); focus = false; }
    if (ImGui::Button("Resume", full)) set_paused(false);
    ui::section("Display");
    const char* filters[] = {"Sharp", "Smooth", "Pixelated"};
    const char* aspects[] = {"4:3 (original)", "16:9", "Stretch"};
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##filter", &g_cfg.screenFilter, filters, 3);
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::Combo("##aspect", &g_cfg.aspect, aspects, 3);
    if (ImGui::Button(is_fullscreen() ? "Leave fullscreen" : "Fullscreen", full)) toggle_fullscreen();
    ui::section("Sound");
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::SliderInt("##vol", &g_cfg.volume, 0, 100, "Volume %d %%");
    ui::section("Debug states  (F5 save 1, F9 load 1)");
    {
        static StateInfo info[4];
        static bool fresh;
        if (!fresh) { for (int i = 1; i <= 3; i++) info[i] = state_read(state_path(i), false); fresh = true; }
        for (int i = 1; i <= 3; i++) {
            ImGui::PushID(i);
            char label[96];
            if (info[i].ok) {
                char when[32];
                time_t t = (time_t)info[i].when;
                strftime(when, sizeof when, "%d/%m %H:%M", localtime(&t));
                int secs = (int)(info[i].vblank / 59.94);
                snprintf(label, sizeof label, "%d  -  %d:%02d of play  -  %s", i, secs / 60, secs % 60, when);
            } else {
                snprintf(label, sizeof label, "%d  -  empty", i);
            }
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(label);
            float bw = ImGui::GetFontSize() * 3.6f;
            ImGui::SameLine(ImGui::GetContentRegionMax().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x);
            if (ImGui::Button("Save", ImVec2(bw, 0)) && state_save(i)) info[i] = state_read(state_path(i), false);
            ImGui::SameLine();
            ImGui::BeginDisabled(!info[i].ok);
            if (ImGui::Button("Load", ImVec2(bw, 0))) state_request_load(i);
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        if (!paused) fresh = false;
    }
    ui::section("Game");
    ImGui::TextDisabled("Progress is saved by the game to its memory card.");
    if (ImGui::Button("Back to launcher", full)) { g_back_to_launcher = 1; g_quit = 1; }
    if (ImGui::Button("Quit", full)) g_quit = 1;
    ImGui::End();
    if (!paused) focus = true;
    ImGui::Render();
    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static std::string toast_msg;
static int toast_until;
static void toast(const std::string& msg) { toast_msg = msg; toast_until = frame + 120; }
static void overlay_frame(bool progress) {
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    ImGuiIO& io = ImGui::GetIO();
    if (progress) {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 20, 0));
        ImGui::Begin("##replay", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs);
        ImGui::TextUnformatted("Loading state...");
        ImGui::ProgressBar(replay_end ? (float)core_vblank_count() / replay_end : 0.0f, ImVec2(-FLT_MIN, 0));
        ImGui::End();
    } else {
        ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16, 16), ImGuiCond_Always, ImVec2(1, 0));
        ImGui::Begin("##toast", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs |
                                           ImGuiWindowFlags_AlwaysAutoResize);
        ImGui::TextUnformatted(toast_msg.c_str());
        ImGui::End();
    }
    ImGui::Render();
    int dw, dh;
    SDL_GL_GetDrawableSize(win, &dw, &dh);
    glViewport(0, 0, dw, dh);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

extern "C" void platform_vblank(void) {
    frame++;
    if (g_replaying) {
        // fast-forward: keep the window alive and show progress now and then
        SDL_Event e;
        while (SDL_PollEvent(&e))
            if (e.type == SDL_QUIT) g_quit = 1;
        if (frame % 30 == 0) {
            int dw, dh;
            SDL_GL_GetDrawableSize(win, &dw, &dh);
            glViewport(0, 0, dw, dh);
            glClearColor(0, 0, 0, 1);
            glClear(GL_COLOR_BUFFER_BIT);
            overlay_frame(true);
            SDL_GL_SwapWindow(win);
        }
        if (g_max_frames && frame >= g_max_frames) g_quit = 1;
        return;
    }
    if (poll_input()) g_quit = 1;
    if (dbg_env("AC2_PERFLOG") && frame % 300 == 0) {   // debug: achieved speed per 300 VBlanks
        static Uint64 last;
        Uint64 now = SDL_GetPerformanceCounter();
        extern int g_xa_stats[3], g_xa_last_file, g_xa_last_chan;
        extern int cd_filter(void);
        if (last) LOGI("perf frame %d: %.1f fps  xa played %d muted %d filtered %d (filter %d/%d, seen %d/%d)", frame,
                       300.0 * SDL_GetPerformanceFrequency() / (double)(now - last), g_xa_stats[0], g_xa_stats[1],
                       g_xa_stats[2], cd_filter() >> 8, cd_filter() & 255, g_xa_last_file, g_xa_last_chan);
        g_xa_stats[0] = g_xa_stats[1] = g_xa_stats[2] = 0;
        extern double g_mix_energy[3];
        extern uint16_t spu_reg_debug(uint32_t a);
        if (g_mix_energy[2] > 0)
            LOGI("perf mix: xa %.1f dB  voices %.1f dB  spucnt %04x cdvol %04x/%04x mvol %04x/%04x", 10 * log10(g_mix_energy[0] / g_mix_energy[2] / 1073741824.0 + 1e-12),
                 10 * log10(g_mix_energy[1] / g_mix_energy[2] / 1073741824.0 + 1e-12), spu_reg_debug(0x1F801DAA),
                 spu_reg_debug(0x1F801DB0), spu_reg_debug(0x1F801DB2), spu_reg_debug(0x1F801D80), spu_reg_debug(0x1F801D82));
        g_mix_energy[0] = g_mix_energy[1] = g_mix_energy[2] = 0;
        extern int spu_active_voices(void);
        LOGI("perf voices: active %d", spu_active_voices());
        extern int g_spu_stats[4];
        LOGI("perf spu: key-ons %d (silent start %d) dma words %d fifo %d", g_spu_stats[0], g_spu_stats[3], g_spu_stats[1], g_spu_stats[2]);
        memset(g_spu_stats, 0, sizeof g_spu_stats);
        last = now;
    }
    {   // debug: AC2_SAVE_AT="frame:slot" saves a state automatically (tests)
        static int at = -1, slot = 1;
        if (at < 0) { const char* e = getenv("AC2_SAVE_AT"); at = 0; if (e) sscanf(e, "%d:%d", &at, &slot); }
        if (at && frame == at) state_save(slot);
    }
    capture_frame(gpu_display());
    draw_image();
    if (frame < toast_until && !paused) overlay_frame(false);
    SDL_GL_SwapWindow(win);
    // the game is frozen while paused: we loop here, outside PS1 time
    while (paused && !g_quit) {
        if (poll_input()) g_quit = 1;
        draw_image();
        pause_menu_frame();
        SDL_GL_SwapWindow(win);
        SDL_Delay(16);
    }
    if (g_max_frames && frame >= g_max_frames) g_quit = 1;
}
