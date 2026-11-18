/* re4_mac: the macOS front end (arm64, native). The game runs in re4_coreproc (x86_64, Rosetta) and
 * hands its GX chunks, sound and rumble over shared memory (runtime/core_ipc*.cpp); this program
 * draws them with the GX renderer of the Android app (GLES 3 code on macOS's OpenGL 4.1 core) in an
 * SDL3 window, reads keyboard, mouse and game controllers, and has its own settings menu (Dear ImGui):
 * Esc, the PS / Guide button or Share / Back opens it. Settings live in
 * ~/Library/Application Support/RE4 Mac/settings.ini. */
#include <SDL3/SDL.h>
#include <unistd.h>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "gx_gl.h"
#include "re4_gx_frame.h"
#include "minimap_view.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_opengl3.h"
#if !defined(__aarch64__)
#error "re4_mac is the native (arm64) front end; the game itself runs in re4_coreproc"
#endif
#include "re4_core_client.h"
#include "texture_pack.h"
#include <climits>
#include <cstdlib>

// ---------------------------------------------------------------------------------------------
// Settings
struct Settings {
    // display
    bool fullscreen = true;
    bool vsync = true;
    bool smooth = true;       // 60 FPS (in-between pictures)
    int scale = 3;            // EFB scale 1..6
    int format = 5;           // 5 Native, 0 16:9, 3 4:3, 2 stretched
    int fov = 115;            // field of view in the 16:9 formats, % of the original letterbox (100..133)
    int samples = 2;          // MSAA 1/2/4/8
    int anisotropy = 16;      // 1..16
    bool fxaa = false;
    int sharpen = 1;          // 0 off, 1 light, 2 strong
    int dof = 1;              // 0 off, 1 light, 2 strong
    int flares = 0;           // 0 off, 1 lens flares, 2 halos + flares
    int viewDistance = 100;   // 100 / 75 / 50
    int effects = 0;          // 0 all, 1 reduced, 2 minimal
    bool tvFilter = false;
    int crt = 0;              // 0 off, 1 light, 2 strong
    int colour = 0;           // 0 off, 1 warm film, 2 survival horror
    int laser = 0;            // 0 red, 1 blue, 2 green, 3 yellow
    bool psButtons = true;    // PlayStation button icons (the bundled pack)
    bool hdTextures = true;   // the player's texture pack folder
    int hdMaxSize = 2048;     // the packs' pictures scaled down to this side (0: as they are)
    // controls
    int preset = 0;           // 0 HD classic, 1 modern, 2 GameCube
    bool invertY = false;
    bool autoAim = false;
    bool alwaysRun = false;
    int aimSpeed = 100;       // 50..300 %
    bool rumble = true;
    int mouseSens = 100;      // 25..400 %
    // audio
    int music = 100, effectsVol = 100;
    // cheats and game options: the core keeps them (RE4_CHEATS bits, runtime/cheats.cpp)
    bool cheatHealth = false, cheatAmmo = false, cheatOneHit = false, cheatQte = false, cheatInvisible = false,
         cheatExtras = false;
    bool itemRandom = false; int itemSeed = 1;     // random items (rooms loaded from then on), seed 1..0xFFFFF
    bool enemyRandom = false; int enemySeed = 1;   // random enemies (from the next room on)
    bool gentleQte = false;                        // Professional's quick-time events as Normal's
    int character = 0;                             // the story's player: 0 Leon, 2 Ada, 3 HUNK, 4 Krauser, 5 Wesker
};

static unsigned g_texCount = 0;
static std::string userTextureDir();

static std::string settingsPath() {
    char* p = SDL_GetPrefPath(nullptr, "RE4 Mac");
    std::string s = p ? std::string(p) + "settings.ini" : "settings.ini";
    SDL_free(p);
    return s;
}

#define RE4_SETTINGS_FIELDS(X) \
    X(fullscreen) X(vsync) X(smooth) X(scale) X(format) X(fov) X(samples) X(anisotropy) X(fxaa) X(sharpen) X(dof) \
    X(flares) X(viewDistance) X(effects) X(tvFilter) X(crt) X(colour) X(laser) X(psButtons) X(hdTextures) X(hdMaxSize) X(preset) X(invertY) X(autoAim) \
    X(alwaysRun) X(aimSpeed) X(rumble) X(mouseSens) X(music) X(effectsVol) \
    X(cheatHealth) X(cheatAmmo) X(cheatOneHit) X(cheatQte) X(cheatInvisible) X(cheatExtras) X(itemRandom) X(itemSeed) \
    X(enemyRandom) X(enemySeed) X(gentleQte) X(character)

static void setField(int& f, int v) { f = v; }
static void setField(bool& f, int v) { f = v != 0; }

static void loadSettings(Settings& s) {
    FILE* f = fopen(settingsPath().c_str(), "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char key[128]; int v;
        if (sscanf(line, "%127[^=]=%d", key, &v) != 2) continue;
#define X(name) if (!strcmp(key, #name)) setField(s.name, v);
        RE4_SETTINGS_FIELDS(X)
#undef X
    }
    fclose(f);
    s.scale = SDL_clamp(s.scale, 1, 6);
    s.aimSpeed = SDL_clamp(s.aimSpeed, 50, 300);
    s.mouseSens = SDL_clamp(s.mouseSens, 25, 400);
    s.itemSeed = SDL_clamp(s.itemSeed, 1, 0xFFFFF);
    s.enemySeed = SDL_clamp(s.enemySeed, 1, 0xFFFFF);
    if (s.character < 2 || s.character > 5) s.character = 0;
}

static void saveSettings(const Settings& s) {
    FILE* f = fopen(settingsPath().c_str(), "w");
    if (!f) return;
#define X(name) fprintf(f, "%s=%d\n", #name, (int) s.name);
    RE4_SETTINGS_FIELDS(X)
#undef X
    fclose(f);
}

// What the core is told (any time; it keeps it).
static void sendCoreSettings(const Settings& s) {
    re4ipc_set_dof(s.dof != 0);
    re4ipc_set_view_distance(s.viewDistance);
    re4ipc_set_potato(s.effects == 0 ? 0 : s.effects == 1 ? 0x4 : (0x1 | 0x2 | 0x8 | 0x10));
    re4ipc_set_aim((s.invertY ? 1 : 0) | (s.autoAim ? 2 : 0) | (s.alwaysRun ? 4 : 0) | (s.aimSpeed << 8));
    re4ipc_set_sound_volume(SDL_clamp(s.music, 0, 100) | SDL_clamp(s.effectsVol, 0, 100) << 8);
    re4ipc_set_laser_colour(s.laser);
    // the cheat switches (runtime/cheats.cpp): 1 health, 2 ammo, 4 one-hit kill, 8 automatic QTEs, 16 extra games kept
    // unlocked, bits 5..7 the story's player (pl_type), 512 invisibility, 1024 random items with the seed in bits 11..30
    int bits = (s.cheatHealth ? 1 : 0) | (s.cheatAmmo ? 2 : 0) | (s.cheatOneHit ? 4 : 0) | (s.cheatQte ? 8 : 0) |
               (s.cheatExtras ? 16 : 0) | (s.character << 5) | (s.cheatInvisible ? 512 : 0);
    if (s.itemRandom) bits |= 1024 | (s.itemSeed & 0xFFFFF) << 11;
    re4ipc_set_cheats(bits);
    re4ipc_set_enemy_random(s.enemyRandom ? s.enemySeed : 0);
    re4ipc_set_gentle_qte(s.gentleQte ? 1 : 0);
}

// What the renderer is told (cheap setters, after every Init and on change).
static void sendRendererSettings(re4gx::GxRenderer* gx, const Settings& s) {
    if (!gx) return;
    gx->SetFxaa(s.fxaa);
    gx->SetSharpen(s.sharpen);
    gx->SetTvFilter(s.tvFilter);
    gx->SetCrt(s.crt);
    gx->SetColour(s.colour, s.colour ? 2 : 0);
    gx->SetFlares(s.flares != 0);
    gx->SetFlareHalos(s.flares == 2);
    gx->SetDofStrong(s.dof == 2);
    gx->SetWiden(1.0f);
}

// ---------------------------------------------------------------------------------------------
// Sound

static void SDLCALL audioCb(void*, SDL_AudioStream* s, int additional, int) {
    int frames = additional / 4;
    static int16_t buf[4096 * 2];
    while (frames > 0) {
        int n = frames > 4096 ? 4096 : frames;
        std::memset(buf, 0, n * 4);
        re4ipc_audio(buf, n);
        SDL_PutAudioStreamData(s, buf, n * 4);
        frames -= n;
    }
}

// ---------------------------------------------------------------------------------------------
// Input. GameCube pad bits.
enum : uint16_t {
    GC_LEFT = 0x0001, GC_RIGHT = 0x0002, GC_DOWN = 0x0004, GC_UP = 0x0008,
    GC_Z = 0x0010, GC_R = 0x0020, GC_L = 0x0040,
    GC_A = 0x0100, GC_B = 0x0200, GC_X = 0x0400, GC_Y = 0x0800, GC_START = 0x1000,
};

// The controller in the chosen layout. The game: R aims, A acts (fires while aiming), B runs
// (reloads while aiming), L readies the knife (A slashes), X calls Ashley, Y the inventory, Z the
// map, START the options.
static uint16_t padButtons(SDL_Gamepad* pad, int preset, int* lt, int* rt) {
    auto bt = [&](SDL_GamepadButton b) { return SDL_GetGamepadButton(pad, b); };
    const bool l2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER) > 12000;
    const bool r2 = SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) > 12000;
    const bool cross = bt(SDL_GAMEPAD_BUTTON_SOUTH), circle = bt(SDL_GAMEPAD_BUTTON_EAST);
    const bool square = bt(SDL_GAMEPAD_BUTTON_WEST), triangle = bt(SDL_GAMEPAD_BUTTON_NORTH);
    const bool l1 = bt(SDL_GAMEPAD_BUTTON_LEFT_SHOULDER), r1 = bt(SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    const bool touch = bt(SDL_GAMEPAD_BUTTON_TOUCHPAD);
    uint16_t b = 0;
    if (bt(SDL_GAMEPAD_BUTTON_START)) b |= GC_START;
    if (bt(SDL_GAMEPAD_BUTTON_DPAD_UP)) b |= GC_UP;
    if (bt(SDL_GAMEPAD_BUTTON_DPAD_DOWN)) b |= GC_DOWN;
    if (bt(SDL_GAMEPAD_BUTTON_DPAD_LEFT)) b |= GC_LEFT;
    if (bt(SDL_GAMEPAD_BUTTON_DPAD_RIGHT)) b |= GC_RIGHT;
    switch (preset) {
    case 1:  // modern: L2 aims, R2 fires, L1 knife, square reloads, R1 Ashley
        if (l2) b |= GC_R;
        if (r2 || cross) b |= GC_A;
        if (circle || square) b |= GC_B;
        if (triangle) b |= GC_Y;
        if (l1) b |= GC_L;
        if (r1) b |= GC_X;
        if (touch) b |= GC_Z;
        break;
    case 2:  // GameCube: as the original pad (L1 / R1 = Z, the touch pad = START)
        if (cross) b |= GC_A;
        if (circle) b |= GC_B;
        if (square) b |= GC_X;
        if (triangle) b |= GC_Y;
        if (l2) b |= GC_L;
        if (r2) b |= GC_R;
        if (l1 || r1) b |= GC_Z;
        if (touch) b |= GC_START;
        break;
    default:  // HD classic (2014 Type I): R2 aims, cross acts / fires, L2 knife, circle runs / reloads
        if (cross) b |= GC_A;
        if (circle) b |= GC_B;
        if (square) b |= GC_X;
        if (triangle) b |= GC_Y;
        if (l2 || l1) b |= GC_L;
        if (r2) b |= GC_R;
        if (r1 || touch) b |= GC_Z;
        break;
    }
    *lt = (b & GC_L) ? 255 : 0;
    *rt = (b & GC_R) ? 255 : 0;
    return b;
}

// Keyboard and mouse, like the PC versions: WASD move, right mouse aims, left mouse fires,
// Shift runs, E / Space act, R reloads, C / V knife, Q Ashley, Tab / I inventory, M map,
// Enter / P options, arrows the d-pad.
static uint16_t keyButtons(const bool* k, SDL_MouseButtonFlags mouse, bool gameMenu) {
    uint16_t b = 0;
    const bool aim = (mouse & SDL_BUTTON_RMASK) != 0;
    const bool knife = k[SDL_SCANCODE_C] || k[SDL_SCANCODE_V];
    if (aim) b |= GC_R;
    if (knife) b |= GC_L;
    if (mouse & SDL_BUTTON_LMASK) {
        if (gameMenu || knife || aim) b |= GC_A;
        else b |= GC_R | GC_A;  // fire straight away
    }
    if (k[SDL_SCANCODE_E] || k[SDL_SCANCODE_SPACE]) b |= GC_A;
    if (k[SDL_SCANCODE_LSHIFT] || k[SDL_SCANCODE_RSHIFT] || k[SDL_SCANCODE_BACKSPACE]) b |= GC_B;
    if (k[SDL_SCANCODE_R]) b |= gameMenu ? GC_B : (GC_R | GC_B);
    if (k[SDL_SCANCODE_Q]) b |= GC_X;
    if (k[SDL_SCANCODE_TAB] || k[SDL_SCANCODE_I]) b |= GC_Y;
    if (k[SDL_SCANCODE_M]) b |= GC_Z;
    if (k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_P]) b |= GC_START;
    if (k[SDL_SCANCODE_UP]) b |= GC_UP;
    if (k[SDL_SCANCODE_DOWN]) b |= GC_DOWN;
    if (k[SDL_SCANCODE_LEFT]) b |= GC_LEFT;
    if (k[SDL_SCANCODE_RIGHT]) b |= GC_RIGHT;
    return b;
}

// ---------------------------------------------------------------------------------------------
// Picture placement

struct Shown {
    int x = 0, y = 0, w = 0, h = 0;  // the picture's box, from the bottom left (GL)
    int top = 0, rows = 448;          // its rows of the game's 448
};

struct CoreSent {
    int letterbox = -1, aspect = -1;
    re4mm::HudLayout hud{-1, -1, -1, -1};
};

// fov (> 100, "16:9" and "Native" at 16:9 or wider): the camera further out, as the original's letterbox at
// `fov` %: the game's 3D that much wider (the core's wide projection: the aiming, the laser and the effects follow
// it) and that many more of its 448 rows on the screen (the bars off), both directions alike; at most all 448 rows
// (133 %). Pictures that stay 4:3 (movies, the scope, the menus) show the letterbox's rows as before.
static Shown placePicture(re4gx::GxRenderer* gx, int format, int width, int height, int fov, CoreSent& sent) {
    Shown s;
    unsigned xw = 0, xh = 0;
    if (!gx->Xfb(&xw, &xh) || !xh) xh = 448;
    const bool wideChunk = gx->XfbWide();
    re4mm::NativeView native = re4mm::Native(width, height, wideChunk);
    const double k = std::min(448.0 / 336.0, std::max(1.0, fov / 100.0));
    const bool wider = k > 1.001 && (format == 0 || (format == 5 && native.mode != -1));
    const int noBars = format == 3 || (format == 5 && native.mode == -1) || wider ? 1 : 0;
    if (noBars != sent.letterbox) { re4ipc_set_letterbox(noBars); sent.letterbox = noBars; }
    int aspect3d = format == 5 ? re4mm::NativeAspect3d(width, height) : 0;
    if (wider) aspect3d = (int) ((aspect3d ? aspect3d : 13333.33) * k + 0.5);
    if (aspect3d != sent.aspect) { re4ipc_set_view_aspect(aspect3d); sent.aspect = aspect3d; }
    int top = 56, rows = 336;
    if (wider && wideChunk) {
        rows = std::min(448, (int) (336 * k + 0.5)) & ~1;
        top = (448 - rows) / 2;
        native.top = top; native.rows = rows;
    }
    re4mm::HudLayout hud = re4mm::Hud(format, width, height, native);
    if (wider && wideChunk) { hud.top = top; hud.bottom = top + rows; }
    if (hud.top != sent.hud.top || hud.bottom != sent.hud.bottom || hud.stretch != sent.hud.stretch || hud.flags != sent.hud.flags) {
        re4ipc_set_hud_layout(hud.top | hud.bottom << 16, hud.stretch, hud.flags);
        sent.hud = hud;
    }
    if (format == 5) {
        if (wider && !wideChunk) native.top = 56, native.rows = 336;
        gx->SetShownRows(native.top, native.top + native.rows);
        s = {native.pic.x, native.pic.y, native.pic.w, native.pic.h, native.top, native.rows};
    } else if (format == 3) {
        gx->SetShownRows(0, 1 << 16);
        int boxW = width, boxH = (int) (width * 3.0 / 4.0 + 0.5);
        if (boxH > height) { boxH = height; boxW = (int) (height * 4.0 / 3.0 + 0.5); }
        s = {(width - boxW) / 2, (height - boxH) / 2, boxW, boxH, 0, 448};
    } else {
        gx->SetShownRows(top, top + rows);
        int boxW = width, boxH = width * 9 / 16;
        if (format == 2) boxH = height;
        else if (boxH > height) { boxH = height; boxW = height * 16 / 9; }
        s = {(width - boxW) / 2, (height - boxH) / 2, boxW, boxH, top, rows};
    }
    if (s.top == 0 && s.rows == 448) gx->Present(s.x, s.y, s.w, s.h);
    else gx->Present(s.x, s.y, s.w, s.h, xh * (unsigned) s.top / 448, xh * (unsigned) s.rows / 448);
    return s;
}

// ---------------------------------------------------------------------------------------------
// The settings menu

static void menuStyle(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    ImGui::StyleColorsDark(&st);
    st.WindowRounding = 10; st.FrameRounding = 6; st.GrabRounding = 6; st.TabRounding = 6; st.PopupRounding = 6;
    st.WindowPadding = ImVec2(22, 18); st.FramePadding = ImVec2(12, 7); st.ItemSpacing = ImVec2(12, 10);
    st.WindowBorderSize = 1; st.FrameBorderSize = 0;
    ImVec4* c = st.Colors;
    const ImVec4 accent(0.66f, 0.12f, 0.10f, 1.0f), accentHi(0.82f, 0.18f, 0.14f, 1.0f), accentLo(0.45f, 0.08f, 0.07f, 1.0f);
    c[ImGuiCol_WindowBg] = ImVec4(0.06f, 0.06f, 0.06f, 0.94f);
    c[ImGuiCol_Border] = ImVec4(0.55f, 0.45f, 0.30f, 0.45f);
    c[ImGuiCol_Text] = ImVec4(0.93f, 0.90f, 0.84f, 1.0f);
    c[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.57f, 0.52f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.16f, 0.15f, 0.14f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.24f, 0.21f, 0.19f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.30f, 0.25f, 0.22f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.20f, 0.18f, 0.17f, 1.0f);
    c[ImGuiCol_ButtonHovered] = accentLo; c[ImGuiCol_ButtonActive] = accent;
    c[ImGuiCol_Header] = accentLo; c[ImGuiCol_HeaderHovered] = accent; c[ImGuiCol_HeaderActive] = accentHi;
    c[ImGuiCol_Tab] = ImVec4(0.14f, 0.13f, 0.12f, 1.0f); c[ImGuiCol_TabHovered] = accent; c[ImGuiCol_TabSelected] = accentLo;
    c[ImGuiCol_CheckMark] = accentHi; c[ImGuiCol_SliderGrab] = accent; c[ImGuiCol_SliderGrabActive] = accentHi;
    c[ImGuiCol_NavCursor] = ImVec4(0.95f, 0.80f, 0.45f, 1.0f);
    c[ImGuiCol_Separator] = ImVec4(0.55f, 0.45f, 0.30f, 0.35f);
    c[ImGuiCol_PopupBg] = ImVec4(0.09f, 0.09f, 0.09f, 0.98f);
    st.ScaleAllSizes(scale);
}

static bool comboInt(const char* label, int* v, const char* const* names, const int* values, int n) {
    int cur = 0;
    for (int i = 0; i < n; i++) if (values[i] == *v) cur = i;
    bool changed = false;
    if (ImGui::BeginCombo(label, names[cur])) {
        for (int i = 0; i < n; i++) {
            if (ImGui::Selectable(names[i], i == cur)) { *v = values[i]; changed = true; }
            if (i == cur) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

static void hint(const char* text) {
    if (ImGui::IsItemHovered() || ImGui::IsItemFocused()) ImGui::SetItemTooltip("%s", text);
}

struct MenuResult { bool textures = false, core = false, renderer = false, rebuild = false, rescale = false, window = false, close = false, quit = false; };

static MenuResult drawMenu(Settings& s, int maxSamples, const char* gpuInfo) {
    MenuResult r;
    ImGuiIO& io = ImGui::GetIO();
    const float fs = ImGui::GetFontSize();
    const ImVec2 size(std::min(io.DisplaySize.x * 0.92f, fs * 46.0f), std::min(io.DisplaySize.y * 0.90f, fs * 38.0f));
    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(size, ImGuiCond_Always);
    ImGui::Begin("##menu", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    ImGui::TextColored(ImVec4(0.82f, 0.18f, 0.14f, 1), "RESIDENT EVIL 4");
    ImGui::SameLine(); ImGui::TextDisabled("  macOS  ·  Configuración");
    ImGui::Separator();
    const float labelW = ImGui::GetContentRegionAvail().x * 0.45f;
    auto row = [&](const char* label) { ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted(label); ImGui::SameLine(labelW); ImGui::SetNextItemWidth(-1); };

    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Gráficos")) {
            ImGui::BeginChild("g", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
            ImGui::SeparatorText("Pantalla");
            row("Pantalla completa"); if (ImGui::Checkbox("##fs", &s.fullscreen)) r.window = true;
            row("Sincronización vertical"); if (ImGui::Checkbox("##vs", &s.vsync)) r.window = true;
            row("60 FPS (cuadros intermedios)"); ImGui::Checkbox("##60", &s.smooth);
            hint("El juego corre a 30 Hz; el renderizador dibuja una imagen intermedia entre cada par.");
            {
                static const char* n[] = {"Nativo (pantalla completa)", "16:9", "4:3", "Estirado"};
                static const int v[] = {5, 0, 3, 2};
                row("Formato de imagen"); comboInt("##fmt", &s.format, n, v, 4);
            }
            if (s.format == 0 || s.format == 5) {
                static const char* n[] = {"Original (GameCube)", "Algo más amplio", "Amplio", "Muy amplio", "Máximo"};
                static const int v[] = {100, 108, 115, 124, 133};
                row("Campo de visión"); comboInt("##fov", &s.fov, n, v, 5);
                hint("Aleja la cámara: muestra más imagen arriba, abajo y a los lados. Original = la franja de la película.");
            }
            ImGui::SeparatorText("Calidad");
            {
                static const char* n[] = {"1x (640×528)", "2x (1280×1056)", "3x (1920×1584)", "4x (2560×2112)", "5x (3200×2640)", "6x (3840×3168)"};
                static const int v[] = {1, 2, 3, 4, 5, 6};
                row("Resolución interna"); if (comboInt("##res", &s.scale, n, v, 6)) r.rescale = true;
            }
            {
                const char* n[] = {"Desactivado", "2x", "4x", "8x"};
                const int v[] = {1, 2, 4, 8};
                int cnt = maxSamples >= 8 ? 4 : maxSamples >= 4 ? 3 : maxSamples >= 2 ? 2 : 1;
                row("Antialiasing (MSAA)"); if (comboInt("##msaa", &s.samples, n, v, cnt)) r.rebuild = true;
            }
            {
                static const char* n[] = {"Desactivado", "2x", "4x", "8x", "16x"};
                static const int v[] = {1, 2, 4, 8, 16};
                row("Filtro anisotrópico"); if (comboInt("##aniso", &s.anisotropy, n, v, 5)) r.rebuild = true;
            }
            row("FXAA"); if (ImGui::Checkbox("##fxaa", &s.fxaa)) r.renderer = true;
            {
                static const char* n[] = {"Desactivado", "Suave", "Fuerte"};
                static const int v[] = {0, 1, 2};
                row("Nitidez"); if (comboInt("##sharp", &s.sharpen, n, v, 3)) r.renderer = true;
            }
            ImGui::SeparatorText("Texturas");
            row("Iconos de botones"); {
                int v = s.psButtons ? 1 : 0;
                static const char* n[] = {"GameCube", "PlayStation"};
                static const int vv[] = {0, 1};
                if (comboInt("##btn", &v, n, vv, 2)) { s.psButtons = v != 0; r.textures = true; }
            }
            row("Texturas HD (paquete)"); if (ImGui::Checkbox("##hd", &s.hdTextures)) r.textures = true;
            row("Resolución máx. HD"); {
                static const char* n[] = {"1024 (rápido)", "2048", "Original (más memoria)"};
                static const int v[] = {1024, 2048, 0};
                if (comboInt("##hdmax", &s.hdMaxSize, n, v, 3)) r.textures = true;
            }
            row("");
            if (ImGui::SmallButton("Abrir carpeta")) {
                const std::string d = userTextureDir();
                SDL_CreateDirectory(d.c_str());
                SDL_OpenURL(("file://" + d).c_str());
            }
            ImGui::TextDisabled("Paquetes en formato Dolphin (tex1_*.png): carpeta \"textures\" o la carpeta\n"
                                "G4BE08 junto al juego. Los iconos de botones tienen prioridad.\n"
                                "%u reemplazos cargados. Los cambios se ven al entrar a una zona nueva.", g_texCount);
            ImGui::SeparatorText("Efectos");
            {
                static const char* n[] = {"Desactivada", "Suave", "Fuerte"};
                static const int v[] = {0, 1, 2};
                row("Profundidad de campo"); if (comboInt("##dof", &s.dof, n, v, 3)) r.core = r.renderer = true;
            }
            {
                static const char* n[] = {"Desactivados", "Destellos de lente", "Halos + destellos"};
                static const int v[] = {0, 1, 2};
                row("Destellos de luz"); if (comboInt("##fl", &s.flares, n, v, 3)) r.renderer = true;
            }
            {
                static const char* n[] = {"Normal", "Media", "Corta"};
                static const int v[] = {100, 75, 50};
                row("Distancia de visión"); if (comboInt("##vd", &s.viewDistance, n, v, 3)) r.core = true;
            }
            {
                static const char* n[] = {"Completos", "Reducidos", "Mínimos"};
                static const int v[] = {0, 1, 2};
                row("Partículas y efectos"); if (comboInt("##fx", &s.effects, n, v, 3)) r.core = true;
            }
            {
                static const char* n[] = {"Original", "Película cálida", "Survival horror"};
                static const int v[] = {0, 1, 2};
                row("Color"); if (comboInt("##col", &s.colour, n, v, 3)) r.renderer = true;
            }
            {
                static const char* n[] = {"Desactivado", "Suave", "Fuerte"};
                static const int v[] = {0, 1, 2};
                row("Filtro CRT"); if (comboInt("##crt", &s.crt, n, v, 3)) r.renderer = true;
            }
            row("Filtro de TV original"); if (ImGui::Checkbox("##tv", &s.tvFilter)) r.renderer = true;
            {
                static const char* n[] = {"Rojo", "Azul", "Verde", "Amarillo"};
                static const int v[] = {0, 1, 2, 3};
                row("Color del láser"); if (comboInt("##laser", &s.laser, n, v, 4)) r.core = true;
            }
            ImGui::Spacing(); ImGui::TextDisabled("%s", gpuInfo);
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controles")) {
            ImGui::BeginChild("c", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
            ImGui::SeparatorText("Mando");
            {
                static const char* n[] = {"Clásico HD (Tipo I)", "Moderno (Tipo II)", "GameCube"};
                static const int v[] = {0, 1, 2};
                row("Esquema"); comboInt("##preset", &s.preset, n, v, 3);
            }
            static const char* help[] = {
                "R2 apuntar  ·  ✕ acción / disparar  ·  ○ correr / recargar (apuntando)\n"
                "L2 o L1 cuchillo  ·  □ Ashley  ·  △ inventario  ·  R1 / panel táctil mapa\n"
                "OPTIONS opciones del juego  ·  PS / SHARE este menú",
                "L2 apuntar  ·  R2 disparar  ·  □ recargar  ·  ✕ acción  ·  ○ correr\n"
                "L1 cuchillo  ·  R1 Ashley  ·  △ inventario  ·  panel táctil mapa\n"
                "OPTIONS opciones del juego  ·  PS / SHARE este menú",
                "Como el mando de GameCube: ✕ A  ·  ○ B  ·  □ X  ·  △ Y\n"
                "L2 L (cuchillo)  ·  R2 R (apuntar)  ·  L1 / R1 Z (mapa)\n"
                "OPTIONS / panel táctil START  ·  PS / SHARE este menú"};
            ImGui::TextDisabled("%s", help[SDL_clamp(s.preset, 0, 2)]);
            row("Invertir eje vertical"); if (ImGui::Checkbox("##inv", &s.invertY)) r.core = true;
            row("Apuntado automático"); if (ImGui::Checkbox("##auto", &s.autoAim)) r.core = true;
            row("Correr siempre"); if (ImGui::Checkbox("##run", &s.alwaysRun)) r.core = true;
            row("Velocidad de apuntado"); if (ImGui::SliderInt("##aim", &s.aimSpeed, 50, 300, "%d %%")) r.core = true;
            row("Vibración"); ImGui::Checkbox("##rum", &s.rumble);
            ImGui::SeparatorText("Teclado y ratón");
            row("Sensibilidad del ratón"); ImGui::SliderInt("##ms", &s.mouseSens, 25, 400, "%d %%");
            ImGui::TextDisabled(
                "WASD mover  ·  clic derecho apuntar (ratón mueve la mira)  ·  clic izquierdo disparar\n"
                "Shift correr  ·  E / Espacio acción  ·  R recargar  ·  C / V cuchillo  ·  Q Ashley\n"
                "Tab / I inventario  ·  M mapa  ·  Enter / P opciones  ·  flechas cruceta\n"
                "Esc este menú  ·  F pantalla completa");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Trucos y mods")) {
            ImGui::BeginChild("c", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
            static std::string msg;
            static bool askRestart = false;
            ImGui::SeparatorText("Personaje de la campaña");
            {
                static const char* n[] = {"Leon (original)", "Ada", "HUNK", "Krauser", "Wesker"};
                static const int v[] = {0, 2, 3, 4, 5};
                row("Jugar la historia con"); if (comboInt("##char", &s.character, n, v, 5)) r.core = true;
                hint("Se aplica al cargar la siguiente sala (la sala actual se reinicia donde estás). Solo en la historia: "
                     "no en la parte de Ashley, Assignment Ada ni The Mercenaries. El guardado siempre es de Leon.");
            }
            ImGui::TextDisabled("Se aplica al cargar la siguiente sala. Solo en la historia.");
            ImGui::SeparatorText("Trucos");
            row("Salud infinita"); if (ImGui::Checkbox("##th", &s.cheatHealth)) r.core = true;
            row("Munición infinita"); if (ImGui::Checkbox("##ta", &s.cheatAmmo)) r.core = true;
            row("Muerte de un golpe"); if (ImGui::Checkbox("##to", &s.cheatOneHit)) r.core = true;
            row("QTE automáticos"); if (ImGui::Checkbox("##tq", &s.cheatQte)) r.core = true;
            hint("Cada prompt de quick-time event se responde solo, sin pulsar nada.");
            row("Invisible para los enemigos"); if (ImGui::Checkbox("##ti", &s.cheatInvisible)) r.core = true;
            row("Extras siempre desbloqueados"); if (ImGui::Checkbox("##te", &s.cheatExtras)) r.core = true;
            ImGui::SeparatorText("Aleatorizadores");
            row("Objetos aleatorios"); if (ImGui::Checkbox("##ri", &s.itemRandom)) r.core = true;
            hint("Reparte de nuevo los objetos sueltos de cada sala (solo en la historia; a partir de las salas que se carguen).");
            row("Enemigos aleatorios"); if (ImGui::Checkbox("##re", &s.enemyRandom)) r.core = true;
            hint("Cambia qué enemigos aparecen en cada sala (más o menos un 10 %). Los importantes para la historia se quedan. "
                 "Se aplica desde la siguiente sala.");
            row("Semilla (nueva al pulsar)");
            if (ImGui::Button("Nueva semilla", ImVec2(-1, 0))) {
                s.itemSeed = 1 + (int) (SDL_rand_bits() % 0xFFFFF);
                s.enemySeed = 1 + (int) (SDL_rand_bits() % 0xFFFFF);
                r.core = true;
            }
            ImGui::SeparatorText("Dificultad");
            row("QTE más suaves (Professional)"); if (ImGui::Checkbox("##gq", &s.gentleQte)) r.core = true;
            hint("En Professional, los QTE usan los tiempos y el número de pulsaciones de Normal. El daño y los enemigos "
                 "siguen siendo de Professional.");
            ImGui::SeparatorText("Una vez (cambian lo que guardas; solo con Leon, dentro de una sala)");
            {
                struct Gift { int what; const char* label; };
                static const Gift gifts[] = {
                    {1, "Killer7 al máximo"}, {2, "Maletín L"}, {3, "+1.000.000 ptas"}, {5, "Handcannon"},
                    {6, "Chicago Typewriter"}, {7, "Lanzacohetes infinito"}, {8, "Mine Thrower"}, {9, "Matilda"}};
                const float w = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 3) / 4.0f;
                for (int i = 0; i < 8; i++) {
                    if (i % 4) ImGui::SameLine();
                    if (ImGui::Button(gifts[i].label, ImVec2(w, 0))) {
                        const int res = re4ipc_cheat_action(gifts[i].what);
                        msg = res == 1 ? "Hecho." : res == 2 ? "Ya lo tenías." : res == 3 ? "Se hará en el siguiente frame del juego."
                            : res == -1 ? "No hay sitio libre en el maletín." : "Ahora no se puede (solo Leon, dentro de una sala).";
                        msg = std::string(gifts[i].label) + ": " + msg;
                    }
                }
                if (ImGui::Button("Desbloquear extras (Assignment Ada, Mercenaries)", ImVec2(-1, 0))) {
                    const int res = re4ipc_cheat_action(4);
                    msg = res > 0 ? "Extras desbloqueados." : "Ahora no se puede.";
                    if (res > 0) askRestart = true;
                }
            }
            if (!msg.empty()) ImGui::TextWrapped("%s", msg.c_str());
            if (askRestart) { ImGui::OpenPopup("restart?"); askRestart = false; }
            if (ImGui::BeginPopupModal("restart?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
                ImGui::TextUnformatted("Para ver los extras en el menú de título hay que reiniciar el juego.\nLo no guardado se pierde. ¿Reiniciar?");
                if (ImGui::Button("Reiniciar", ImVec2(160, 0))) { re4ipc_restart_game(0); r.close = true; ImGui::CloseCurrentPopup(); }
                ImGui::SameLine();
                if (ImGui::Button("Más tarde", ImVec2(160, 0))) ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            ImGui::SeparatorText("Mods de texturas");
            ImGui::TextDisabled("Los paquetes HD en formato Dolphin se cargan desde la carpeta 'textures' (ver pestaña Gráficos).");
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            ImGui::BeginChild("a", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() * 1.6f));
            row("Música"); if (ImGui::SliderInt("##mus", &s.music, 0, 100, "%d %%")) r.core = true;
            row("Efectos de sonido"); if (ImGui::SliderInt("##sfx", &s.effectsVol, 0, 100, "%d %%")) r.core = true;
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    const float bw = (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x) * 0.5f;
    if (ImGui::Button("Volver al juego", ImVec2(bw, 0))) r.close = true;
    ImGui::SameLine();
    if (ImGui::Button("Salir del juego", ImVec2(bw, 0))) ImGui::OpenPopup("quit?");
    if (ImGui::BeginPopupModal("quit?", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted("¿Salir del juego? Lo no guardado se pierde.");
        if (ImGui::Button("Salir", ImVec2(160, 0))) r.quit = true;
        ImGui::SameLine();
        if (ImGui::Button("Cancelar", ImVec2(160, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
    return r;
}

// ---------------------------------------------------------------------------------------------

// The texture folders: the bundled PlayStation icons (Contents/Resources/textures/ps-buttons, or
// RE4_PS_BUTTONS), then the player's own pack (~/Library/Application Support/RE4 Mac/textures, Dolphin's
// Load/Textures layout: any subfolders). RE4_TEXTURE_DUMP=1 saves every texture to .../RE4 Mac/dump.
static std::string g_resources;
static std::string userTextureDir() {
    char* p = SDL_GetPrefPath(nullptr, "RE4 Mac");
    std::string s = p ? std::string(p) + "textures" : "textures";
    SDL_free(p);
    return s;
}
static void setupTextures(const Settings& s) {
    std::vector<std::string> dirs;
    if (s.psButtons) dirs.push_back(getenv("RE4_PS_BUTTONS") ? getenv("RE4_PS_BUTTONS") : g_resources + "/textures/ps-buttons");
    if (s.hdTextures) {
        dirs.push_back(userTextureDir());
        // RE4_TEXTURE_DIRS (the launcher: the G4BE08 / textures folders next to the app), separated by ':'
        if (const char* e = getenv("RE4_TEXTURE_DIRS"))
            for (std::string rest = e; !rest.empty();) {
                const size_t c = rest.find(':');
                dirs.push_back(rest.substr(0, c));
                rest = c == std::string::npos ? "" : rest.substr(c + 1);
            }
    }
    std::string dump;
    if (getenv("RE4_TEXTURE_DUMP") && *getenv("RE4_TEXTURE_DUMP") == '1') {
        char* p = SDL_GetPrefPath(nullptr, "RE4 Mac");
        dump = std::string(p ? p : "") + "dump/G4BE08";
        SDL_free(p);
    }
    g_texCount = re4tex::Setup(dirs, dump, (unsigned) std::max(0, s.hdMaxSize));
}

static re4gx::GxRenderer* makeRenderer(const Settings& s, re4gx::GxRenderer* old, std::string* err) {
    auto* gx = new re4gx::GxRenderer();
    re4gx::GxOptions o;
    o.samples = s.samples;
    o.anisotropy = (float) s.anisotropy;
    gx->SetOptions(o);
    if (!getenv("RE4_GX_NO_DEPTH_MAP"))
        gx->SetDepthSink([](const std::uint32_t* cells, std::uint32_t serial) { re4ipc_gx_depth(cells, serial); });
    const int scale = getenv("RE4_SCALE") ? atoi(getenv("RE4_SCALE")) : s.scale;
    if (!gx->Init(scale, err)) { delete gx; return nullptr; }
    if (old) { old->HandOverCopies(*gx); old->Release(); delete old; }
    sendRendererSettings(gx, s);
    printf("renderer up: %s (scale %d, MSAA %d, aniso %.0f)\n", gx->ApiInfo().c_str(), scale, gx->Samples(), gx->Anisotropy());
    return gx;
}

int main(int argc, char** argv) {
    setvbuf(stdout, 0, _IONBF, 0);
    if (argc < 4) { fprintf(stderr, "usage: %s <libre4_main.dylib> <module dir> <data dir>\n", argv[0]); return 2; }
    // no debug overlays (process bars at the sides, the debug room menu): the game as released
    setenv("RE4_DEBUG_HUD", "0", 0);
    setenv("RE4_ROOM_MENU", "0", 0);
    {
        // re4_coreproc next to libre4_main.dylib
        std::string dir = argv[1];
        dir = dir.substr(0, dir.rfind('/'));
        re4ipc::Spawn spawn;
        spawn.program = dir + "/re4_coreproc";
        re4ipc::configure(spawn);
    }
    {
        char real[PATH_MAX];
        std::string exe = realpath(argv[0], real) ? real : argv[0];
        g_resources = exe.substr(0, exe.rfind('/')) + "/../Resources";
    }
    Settings set;
    loadSettings(set);
    if (!re4tex::SelfTest()) fprintf(stderr, "[tex] XXH64 self test failed\n");
    setupTextures(set);
    if (getenv("RE4_60FPS")) set.smooth = *getenv("RE4_60FPS") != '0';
    if (!re4ipc_reserve(argv[3], argv[2])) { fprintf(stderr, "reserve: %s\n", re4ipc_message()); return 3; }

    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMEPAD)) { fprintf(stderr, "SDL: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 4);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    SDL_WindowFlags wflags = SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (set.fullscreen && !getenv("RE4_WINDOWED")) wflags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* win = SDL_CreateWindow("Resident Evil 4", 1280, 720, wflags);
    if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "GL context: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetSwapInterval(set.vsync ? 1 : 0);
    printf("GL %s / %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));
    GLuint vao; glGenVertexArrays(1, &vao); glBindVertexArray(vao);
    int maxSamples = 1; float maxAniso = 1;
    re4gx::GxRenderer::Limits(&maxSamples, &maxAniso);
    if (set.samples > maxSamples) set.samples = maxSamples;

    // the menu
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    const float dpi = SDL_GetWindowDisplayScale(win) > 0 ? SDL_GetWindowDisplayScale(win) : 1.0f;
    const float density = SDL_GetWindowPixelDensity(win) > 0 ? SDL_GetWindowPixelDensity(win) : 1.0f;
    {
        static const ImWchar ranges[] = {0x0020, 0x00FF, 0x2010, 0x2027, 0x25A0, 0x25FF, 0x2715, 0x2716, 0};
        ImFontConfig cfg; cfg.OversampleH = 2;
        const float px = 19.0f * density;
        ImFont* f = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", px, &cfg, ranges);
        if (!f) f = io.Fonts->AddFontFromFileTTF("/System/Library/Fonts/Supplemental/Arial.ttf", px, &cfg, ranges);
        if (!f) io.Fonts->AddFontDefault();
        io.FontGlobalScale = 1.0f / density;
    }
    menuStyle(dpi / density > 0 ? dpi / density : 1.0f);
    ImGui_ImplSDL3_InitForOpenGL(win, ctx);
    ImGui_ImplOpenGL3_Init("#version 410");

    SDL_AudioSpec spec = {SDL_AUDIO_S16, 2, 32000};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, audioCb, nullptr);
    if (audio) SDL_ResumeAudioStreamDevice(audio); else fprintf(stderr, "audio: %s\n", SDL_GetError());

    if (!re4ipc_start()) { fprintf(stderr, "start: %s\n", re4ipc_message()); return 4; }
    printf("game started\n");
    sendCoreSettings(set);

    re4gx::GxRenderer* gx = nullptr;
    SDL_Gamepad* pad = nullptr;
    {
        int n = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&n);
        if (ids && n > 0) { pad = SDL_OpenGamepad(ids[0]); printf("gamepad: %s\n", pad ? SDL_GetGamepadName(pad) : SDL_GetError()); }
        SDL_free(ids);
    }
    std::string gpuInfo = std::string("GPU: ") + (const char*) glGetString(GL_RENDERER) + "  ·  " + (const char*) glGetString(GL_VERSION);

    unsigned shown = 0, lastSeq = 0;
    const Re4GxChunk *prev = nullptr, *pendingNext = nullptr, *peeked = nullptr;
    Uint64 lastArrive = 0; double chunkGapMs = 33.0;
    unsigned betweenShown = 0, betweenDropped = 0;
    double msRun = 0, msPresent = 0, msSwap = 0, worstFrame = 0;
    Uint64 lastSwap = 0; unsigned hitches = 0;
    Uint64 tWin = SDL_GetTicks(); unsigned winStart = 0;
    bool haveSeq = false, quit = false;
    bool menuOpen = false, holdInput = false, rebuild = false, rescale = false;
    Uint64 hintUntil = SDL_GetTicks() + 7000, lastMouseMove = 0;
    unsigned rumbleFrames = 0;
    float aimDX = 0, aimDY = 0;
    bool relMouse = false;
    CoreSent coreSent;
    Shown pic;
    const int maxFrames = getenv("RE4_VIEW_FRAMES") ? atoi(getenv("RE4_VIEW_FRAMES")) : 0;
    const char* shot = getenv("RE4_VIEW_SHOT");

    bool hadFocus = false;
    auto setMenu = [&](bool open, const char* why) {
        if (open == menuOpen) return;
        menuOpen = open;
        re4ipc_pause(open ? 1 : 0, 0);
        if (!open) { saveSettings(set); holdInput = true; }
        SDL_SetWindowRelativeMouseMode(win, false); relMouse = false;
        if (open) SDL_ShowCursor();
        printf("[view] menu %s (%s)\n", open ? "open" : "closed", why);
    };

    while (!quit) {
        const bool gameMenu = (re4ipc_rumble() >> 4) & 1;  // the game shows a menu / sub screen
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            ImGui_ImplSDL3_ProcessEvent(&e);
            switch (e.type) {
            case SDL_EVENT_QUIT: fprintf(stderr, "[view] quit event\n"); quit = true; break;
            case SDL_EVENT_GAMEPAD_ADDED:
                if (!pad) { pad = SDL_OpenGamepad(e.gdevice.which); printf("gamepad: %s\n", pad ? SDL_GetGamepadName(pad) : SDL_GetError()); }
                break;
            case SDL_EVENT_GAMEPAD_REMOVED:
                if (pad && SDL_GetGamepadID(pad) == e.gdevice.which) { SDL_CloseGamepad(pad); pad = nullptr; setMenu(true, "pad gone"); }
                break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                if (e.gbutton.button == SDL_GAMEPAD_BUTTON_GUIDE || e.gbutton.button == SDL_GAMEPAD_BUTTON_BACK) setMenu(!menuOpen, "pad button");
                break;
            case SDL_EVENT_KEY_DOWN:
                if (e.key.repeat) break;
                if (e.key.key == SDLK_ESCAPE) {
                    if (!menuOpen || !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) setMenu(!menuOpen, "escape");
                } else if (!menuOpen && e.key.key == SDLK_F) {
                    set.fullscreen = !(SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN);
                    SDL_SetWindowFullscreen(win, set.fullscreen); saveSettings(set);
                } else if (!menuOpen && e.key.key == SDLK_H) {
                    set.smooth = !set.smooth; printf("[view] 60 FPS %s\n", set.smooth ? "on" : "off");
                }
                break;
            case SDL_EVENT_MOUSE_MOTION:
                lastMouseMove = SDL_GetTicks();
                if (relMouse) { aimDX += e.motion.xrel; aimDY += e.motion.yrel; }
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
                // a click on the game's sub screens (inventory, map, files, merchant) picks what is under it
                if (!menuOpen && gameMenu && e.button.button == SDL_BUTTON_LEFT && pic.w > 0) {
                    int ww, wh, pw, ph; SDL_GetWindowSize(win, &ww, &wh); SDL_GetWindowSizeInPixels(win, &pw, &ph);
                    const float mx = e.button.x * pw / (float) ww, my = e.button.y * ph / (float) wh;
                    const float boxTop = (float) (ph - (pic.y + pic.h));
                    const float u = (mx - pic.x) / pic.w;
                    const float v = (pic.top + (my - boxTop) / pic.h * pic.rows) / 448.0f;
                    if (u >= 0 && u <= 1 && v >= 0 && v <= 1) re4ipc_touch(u, v);
                }
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: hadFocus = true; break;
            case SDL_EVENT_WINDOW_FOCUS_LOST:
                if (!menuOpen && hadFocus) setMenu(true, "focus lost");
                break;
            }
        }

        // input to the game
        {
            Re4CorePad p; std::memset(&p, 0, sizeof p);
            const bool* k = SDL_GetKeyboardState(nullptr);
            float mxf, myf;
            const SDL_MouseButtonFlags mouse = (SDL_GetWindowFlags(win) & SDL_WINDOW_INPUT_FOCUS) ? SDL_GetMouseState(&mxf, &myf) : 0;
            if (!menuOpen) {
                p.buttons = keyButtons(k, mouse, gameMenu);
                int sx = (k[SDL_SCANCODE_D] ? 1 : 0) - (k[SDL_SCANCODE_A] ? 1 : 0);
                int sy = (k[SDL_SCANCODE_W] ? 1 : 0) - (k[SDL_SCANCODE_S] ? 1 : 0);
                p.stickX = (int8_t) (sx * 90); p.stickY = (int8_t) (sy * 90);
                int lt = 0, rt = 0;
                if (pad) {
                    auto ax = [&](SDL_GamepadAxis a) { return (int) (SDL_GetGamepadAxis(pad, a) / 328); };
                    if (!sx) p.stickX = (int8_t) ax(SDL_GAMEPAD_AXIS_LEFTX);
                    if (!sy) p.stickY = (int8_t) -ax(SDL_GAMEPAD_AXIS_LEFTY);
                    p.substickX = (int8_t) (ax(SDL_GAMEPAD_AXIS_RIGHTX) * 9 / 10);
                    p.substickY = (int8_t) (-ax(SDL_GAMEPAD_AXIS_RIGHTY) * 9 / 10);
                    p.buttons |= padButtons(pad, set.preset, &lt, &rt);
                }
                if (p.buttons & GC_L) lt = 255;
                if (p.buttons & GC_R) rt = 255;
                p.triggerL = (uint8_t) lt; p.triggerR = (uint8_t) rt;
                // after the menu closes: nothing until every button is up (the one that closed it)
                if (holdInput) { if (p.buttons) std::memset(&p, 0, sizeof p); else holdInput = false; }
                // mouse aiming while the right button is held
                const bool wantRel = !gameMenu && (mouse & SDL_BUTTON_RMASK);
                if (wantRel != relMouse) { SDL_SetWindowRelativeMouseMode(win, wantRel); relMouse = wantRel; aimDX = aimDY = 0; }
                if (relMouse && (aimDX != 0 || aimDY != 0)) {
                    const float k2 = 0.0016f * set.mouseSens / 100.0f;
                    re4ipc_aim_turn(-aimDX * k2, (set.invertY ? 1.0f : -1.0f) * aimDY * k2);
                    aimDX = aimDY = 0;
                }
                // the pointer only while it moves
                const bool showCursor = !relMouse && SDL_GetTicks() - lastMouseMove < 2000;
                if (showCursor != SDL_CursorVisible()) { if (showCursor) SDL_ShowCursor(); else SDL_HideCursor(); }
            }
            re4ipc_pad(&p);
        }
        // rumble: the game's motor
        {
            const unsigned r = re4ipc_rumble();
            const unsigned frames = r >> 8;
            if (pad && frames != rumbleFrames) {
                rumbleFrames = frames;
                if (set.rumble && !menuOpen && (r & 1)) SDL_RumbleGamepad(pad, 0x7000, 0xD000, 90);
            }
        }

        // a frame
        if (!set.smooth && prev) { re4ipc_gx_release(prev); prev = nullptr; }
        const Re4GxChunk* c = nullptr;
        bool deferred = false;
        if (pendingNext) { c = pendingNext; pendingNext = nullptr; deferred = true; }
        else if (peeked) { c = peeked; peeked = nullptr; }
        else c = re4ipc_gx_acquire();
        if (!c && !(menuOpen && gx)) {
            SDL_Delay(1);
            if (re4ipc_state() != 1) { fprintf(stderr, "[view] core state %d: %s\n", re4ipc_state(), re4ipc_message()); break; }
            continue;
        }
        Uint64 tA = SDL_GetTicksNS(), tB = tA;
        if (c) {
            if (!deferred) {
                const bool follows = gx && haveSeq && c->sequence == lastSeq + 1;
                if (!follows && (c->flags & RE4_GX_CHUNK_RESET)) {
                    if (prev) { re4ipc_gx_release(prev); prev = nullptr; }
                    if (gx) { gx->Release(); delete gx; gx = nullptr; }
                    std::string err;
                    gx = makeRenderer(set, nullptr, &err);
                    if (!gx) { fprintf(stderr, "renderer: %s\n", err.c_str()); re4ipc_gx_release(c); return 5; }
                    rebuild = rescale = false;
                } else if (!gx) { re4ipc_gx_release(c); continue; }
                haveSeq = true; lastSeq = c->sequence;
                const Uint64 at = SDL_GetTicksNS();
                if (lastArrive) chunkGapMs = chunkGapMs * 0.8 + (at - lastArrive) / 1e6 * 0.2;
                lastArrive = at;
            }
            // new picture options: a new renderer takes over (MSAA, filtering), or the EFB at a new scale
            if (rebuild) {
                if (prev) { re4ipc_gx_release(prev); prev = nullptr; }
                std::string err;
                if (re4gx::GxRenderer* n = makeRenderer(set, gx, &err)) gx = n;
                else fprintf(stderr, "renderer: %s (kept the old one)\n", err.c_str());
                rebuild = rescale = false;
            } else if (rescale) {
                gx->Rescale(set.scale);
                rescale = false;
            }
            bool behind = false;
            if (set.smooth && !deferred && prev) {
                if (const Re4GxChunk* n = re4ipc_gx_acquire()) { behind = true; peeked = n; }
            }
            const bool stream60 = chunkGapMs < 24.0;
            gx->SetRecording(set.smooth);
            tA = SDL_GetTicksNS();
            bool between = false;
            if (set.smooth && !deferred && prev && !behind && !stream60) {
                between = gx->Interpolate(*prev, *c, 0.5f);
                if (between) { re4ipc_gx_release(prev); prev = nullptr; pendingNext = c; betweenShown++; }
            }
            if (!between) {
                gx->Run(*c);
                if (prev) { re4ipc_gx_release(prev); prev = nullptr; }
                if (set.smooth) prev = c; else re4ipc_gx_release(c);
                if (set.smooth && behind) betweenDropped++;
            }
            tB = SDL_GetTicksNS();
        }
        {
            int w, h; SDL_GetWindowSizeInPixels(win, &w, &h);
            glBindFramebuffer(GL_FRAMEBUFFER, 0);
            glViewport(0, 0, w, h); glDisable(GL_SCISSOR_TEST);
            glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
            pic = placePicture(gx, set.format, w, h, set.fov, coreSent);
            if (shot && shown == (unsigned) (maxFrames ? maxFrames - 1 : 0)) {
                std::vector<uint8_t> px; unsigned xw, xh;
                if (gx->ReadXfb(&px, &xw, &xh)) {
                    FILE* f = fopen(shot, "wb"); fprintf(f, "P6\n%u %u\n255\n", xw, xh);
                    for (unsigned y = 0; y < xh; y++) for (unsigned x = 0; x < xw; x++) fwrite(&px[(y * xw + x) * 4], 1, 3, f);
                    fclose(f); printf("screenshot %ux%u -> %s\n", xw, xh, shot);
                }
            }
            const bool showHint = !menuOpen && SDL_GetTicks() < hintUntil;
            if (menuOpen || showHint) {
                ImGui_ImplOpenGL3_NewFrame();
                ImGui_ImplSDL3_NewFrame();
                ImGui::NewFrame();
                if (menuOpen) {
                    const Settings before = set;
                    MenuResult r = drawMenu(set, maxSamples, gpuInfo.c_str());
                    if (r.textures) setupTextures(set);
                    if (r.core) sendCoreSettings(set);
                    if (r.renderer) sendRendererSettings(gx, set);
                    if (r.rebuild) rebuild = true;
                    if (r.rescale) rescale = true;
                    if (r.window) {
                        if (set.fullscreen != before.fullscreen) SDL_SetWindowFullscreen(win, set.fullscreen);
                        SDL_GL_SetSwapInterval(set.vsync ? 1 : 0);
                    }
                    if (r.close) setMenu(false, "button");
                    if (r.quit) { saveSettings(set); quit = true; }
                } else {
                    ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - 16, io.DisplaySize.y - 16), ImGuiCond_Always, ImVec2(1, 1));
                    ImGui::SetNextWindowBgAlpha(0.55f);
                    ImGui::Begin("##hint", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                                 ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings);
                    ImGui::TextUnformatted("Esc o botón PS: configuración");
                    ImGui::End();
                }
                ImGui::Render();
                ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
                glBindVertexArray(vao);
            }
            static int menuFrames = 0;
            if (menuOpen && getenv("RE4_MENU_SHOT") && ++menuFrames == 30) {
                std::vector<uint8_t> px((size_t) w * h * 4);
                glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
                FILE* f = fopen(getenv("RE4_MENU_SHOT"), "wb"); fprintf(f, "P6\n%d %d\n255\n", w, h);
                for (int y = h - 1; y >= 0; y--) for (int x = 0; x < w; x++) fwrite(&px[((size_t) y * w + x) * 4], 1, 3, f);
                fclose(f); printf("menu screenshot %dx%d\n", w, h);
            }
            Uint64 tC = SDL_GetTicksNS();
            SDL_GL_SwapWindow(win);
            Uint64 tD = SDL_GetTicksNS();
            msRun += (tB - tA) / 1e6; msPresent += (tC - tB) / 1e6; msSwap += (tD - tC) / 1e6;
            if (lastSwap) { const double f = (tD - lastSwap) / 1e6; worstFrame = std::max(worstFrame, f); if (f > 40) hitches++; }
            lastSwap = tD;
        }
        if (!c) continue;  // (the menu over the paused game)
        shown++;
        if (shown == 300 && getenv("RE4_OPEN_MENU")) setMenu(true, "test");
        if (shown % 120 == 0) {
            Uint64 now = SDL_GetTicks();
            printf("[view] %u frames, %.1f fps now (run %.1f ms, present %.1f, swap %.1f), 60 FPS %s: %u in-between, %u left out; worst %.0f ms, %u over 40\n", shown,
                   (shown - winStart) * 1000.0 / (double) (now - tWin), msRun / 120, msPresent / 120, msSwap / 120,
                   set.smooth ? "on" : "off", betweenShown, betweenDropped, worstFrame, hitches);
            worstFrame = 0; hitches = 0;
            betweenShown = betweenDropped = 0;
            tWin = now; winStart = shown; msRun = msPresent = msSwap = 0;
        }
        if (maxFrames && (int) shown >= maxFrames) break;
    }
    saveSettings(set);
    fflush(stdout);
    _exit(0);
}
