#include <switch.h>
#include <SDL2/SDL.h>
#include <SDL2/SDL_image.h>
#include <SDL2/SDL_ttf.h>
#include <sys/stat.h>
#include <cstdio>
#include <algorithm>
#include <string>
#include <vector>

#include "apk.h"
#include "compat/loader.h"

static const char* APK_DIR  = "sdmc:/BareDroidNX/apks";
static const char* LOG_FILE = "sdmc:/BareDroidNX/log.txt";

// ---------------------------------------------------------------------------
// Layout (1280×720)
// ---------------------------------------------------------------------------
static const int SW       = 1280;
static const int SH       = 720;
static const int HEADER_H = 72;
static const int FOOTER_H = 48;
static const int LIST_Y   = HEADER_H;
static const int LIST_H   = SH - HEADER_H - FOOTER_H;
static const int ITEM_H   = 100;
static const int ICON_SZ  = 72;
static const int VISIBLE  = LIST_H / ITEM_H;   // 6 items

// Colors
static const SDL_Color C_BG     = {15,  15,  26,  255};
static const SDL_Color C_HEADER = {22,  22,  56,  255};
static const SDL_Color C_FOOTER = {10,  10,  20,  255};
static const SDL_Color C_SEL    = {38,  68, 128,  255};
static const SDL_Color C_DIV    = {35,  35,  65,  255};
static const SDL_Color C_WHITE  = {255, 255, 255, 255};
static const SDL_Color C_GRAY   = {160, 160, 180, 255};
static const SDL_Color C_DIM    = {100, 100, 120, 255};
static const SDL_Color C_ICON_PH= {50,  50,  75,  255};
static const SDL_Color C_OK     = {80,  200, 80,  255};
static const SDL_Color C_ERR    = {220, 80,  80,  255};
static const SDL_Color C_WARN   = {220, 180, 60,  255};

// ---------------------------------------------------------------------------
// Log helpers — writes to SD card so we can inspect errors without a screen
// ---------------------------------------------------------------------------
static FILE* g_log = nullptr;
static void logOpen()  { g_log = fopen(LOG_FILE, "w"); }
static void logClose() { if (g_log) { fclose(g_log); g_log = nullptr; } }
static void logMsg(const char* msg) {
    if (g_log) { fputs(msg, g_log); fputc('\n', g_log); fflush(g_log); }
}
static void logSDL(const char* prefix) {
    if (!g_log) return;
    fputs(prefix, g_log);
    fputs(": ", g_log);
    fputs(SDL_GetError(), g_log);
    fputc('\n', g_log);
    fflush(g_log);
}

// ---------------------------------------------------------------------------
// Switch joystick button indices (SDL2 joystick mode)
// ---------------------------------------------------------------------------
static const int BTN_A     = 0;
static const int BTN_B     = 1;
static const int BTN_Y     = 3;
static const int BTN_PLUS  = 10;

// ---------------------------------------------------------------------------
struct App {
    SDL_Window*    win  = nullptr;
    SDL_Renderer*  rdr  = nullptr;
    TTF_Font*      fLg  = nullptr;  // 28px
    TTF_Font*      fSm  = nullptr;  // 18px
    SDL_Joystick*  joy  = nullptr;

    std::vector<ApkInfo>      apks;
    std::vector<SDL_Texture*> icons;
    int selected = 0;
    int scroll   = 0;

    // ------------------------------------------------------------------
    TTF_Font* openFont(int ptsize) {
        plInitialize(PlServiceType_User);
        PlFontData fd = {};
        if (plGetSharedFontByType(&fd, PlSharedFontType_Standard) == 0 && fd.size > 8) {
            SDL_RWops* rw = SDL_RWFromConstMem(
                (const uint8_t*)fd.address + 8, (int)fd.size - 8);
            TTF_Font* f = TTF_OpenFontRW(rw, 1, ptsize);
            if (f) { logMsg("  font: system BFTTF"); return f; }
            logSDL("  BFTTF open failed");
        } else {
            logMsg("  plGetSharedFontByType failed");
        }
        romfsInit();
        TTF_Font* f = TTF_OpenFont("romfs:/fonts/DejaVuSans.ttf", ptsize);
        if (f) { logMsg("  font: romfs DejaVuSans"); return f; }
        logSDL("  romfs font open failed");
        return nullptr;
    }

    // ------------------------------------------------------------------
    bool init() {
        mkdir("sdmc:/BareDroidNX", 0777);
        logOpen();
        logMsg("BareDroidNX starting");

        if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_JOYSTICK) != 0) {
            logSDL("SDL_Init failed"); logClose(); return false;
        }
        logMsg("SDL_Init OK");

        if (IMG_Init(IMG_INIT_PNG | IMG_INIT_JPG) == 0)
            logSDL("IMG_Init warning");
        if (TTF_Init() != 0) {
            logSDL("TTF_Init failed"); logClose(); return false;
        }
        logMsg("TTF_Init OK");

        win = SDL_CreateWindow("BareDroidNX",
            SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
            SW, SH, SDL_WINDOW_SHOWN);
        if (!win) { logSDL("CreateWindow failed"); logClose(); return false; }
        logMsg("Window OK");

        rdr = SDL_CreateRenderer(win, -1,
            SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
        if (!rdr) {
            logSDL("Accelerated renderer failed, trying software");
            rdr = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
        }
        if (!rdr) { logSDL("CreateRenderer failed"); logClose(); return false; }
        logMsg("Renderer OK");

        SDL_SetRenderDrawBlendMode(rdr, SDL_BLENDMODE_BLEND);

        fLg = openFont(28);
        fSm = openFont(18);
        if (!fLg || !fSm) {
            logMsg("Font load failed"); logClose(); return false;
        }
        logMsg("Fonts OK");

        if (SDL_NumJoysticks() > 0) {
            joy = SDL_JoystickOpen(0);
            if (!joy) logSDL("JoystickOpen warning");
            else       logMsg("Joystick OK");
        }

        logMsg("init complete");
        return true;
    }

    // ------------------------------------------------------------------
    void cleanup() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        if (fLg)  TTF_CloseFont(fLg);
        if (fSm)  TTF_CloseFont(fSm);
        if (joy)  SDL_JoystickClose(joy);
        if (rdr)  SDL_DestroyRenderer(rdr);
        if (win)  SDL_DestroyWindow(win);
        romfsExit();
        plExit();
        TTF_Quit(); IMG_Quit(); SDL_Quit();
        logMsg("cleanup done");
        logClose();
    }

    // ------------------------------------------------------------------
    void fill(int x, int y, int w, int h, SDL_Color c) {
        SDL_SetRenderDrawColor(rdr, c.r, c.g, c.b, c.a);
        SDL_Rect r = {x, y, w, h};
        SDL_RenderFillRect(rdr, &r);
    }

    int drawText(TTF_Font* f, const std::string& s, SDL_Color col, int x, int y) {
        if (s.empty() || !f) return 0;
        SDL_Surface* surf = TTF_RenderUTF8_Blended(f, s.c_str(), col);
        if (!surf) return 0;
        SDL_Texture* tex = SDL_CreateTextureFromSurface(rdr, surf);
        int w = surf->w;
        SDL_FreeSurface(surf);
        if (!tex) return 0;
        int tw, th;
        SDL_QueryTexture(tex, nullptr, nullptr, &tw, &th);
        SDL_Rect dst = {x, y, tw, th};
        SDL_RenderCopy(rdr, tex, nullptr, &dst);
        SDL_DestroyTexture(tex);
        return w;
    }

    std::string clamp(TTF_Font* f, const std::string& s, int maxW) {
        int w = 0, h = 0;
        TTF_SizeUTF8(f, s.c_str(), &w, &h);
        if (w <= maxW) return s;
        std::string t = s;
        while (!t.empty()) {
            t.pop_back();
            std::string try_ = t + "...";
            TTF_SizeUTF8(f, try_.c_str(), &w, &h);
            if (w <= maxW) return try_;
        }
        return "...";
    }

    // ------------------------------------------------------------------
    void loadIcons() {
        icons.assign(apks.size(), nullptr);
        for (size_t i = 0; i < apks.size(); i++) {
            if (apks[i].iconPng.empty()) continue;
            SDL_RWops* rw = SDL_RWFromConstMem(
                apks[i].iconPng.data(), (int)apks[i].iconPng.size());
            SDL_Surface* surf = IMG_Load_RW(rw, 1);
            if (!surf) continue;
            icons[i] = SDL_CreateTextureFromSurface(rdr, surf);
            SDL_FreeSurface(surf);
            apks[i].iconPng.clear();
        }
    }

    void rescan() {
        for (auto* t : icons) if (t) SDL_DestroyTexture(t);
        icons.clear();
        apks = ::scanApks(APK_DIR);
        loadIcons();
        selected = 0; scroll = 0;
    }

    // ------------------------------------------------------------------
    void render() {
        fill(0, 0, SW, SH, C_BG);

        // Header
        fill(0, 0, SW, HEADER_H, C_HEADER);
        drawText(fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);
        if (!apks.empty()) {
            std::string cnt = std::to_string(apks.size()) +
                              (apks.size() == 1 ? " APK" : " APKs");
            int w = 0, h = 0;
            TTF_SizeUTF8(fSm, cnt.c_str(), &w, &h);
            drawText(fSm, cnt, C_DIM, SW - w - 30, (HEADER_H - 18) / 2);
        }

        // List
        if (apks.empty()) {
            drawText(fSm,
                "No APKs found — place .apk files in sdmc:/BareDroidNX/apks/",
                C_GRAY, 30, LIST_Y + 30);
        } else {
            int end = std::min((int)apks.size(), scroll + VISIBLE);
            for (int i = scroll; i < end; i++) {
                int iy = LIST_Y + (i - scroll) * ITEM_H;

                if (i == selected) fill(0, iy, SW, ITEM_H, C_SEL);

                SDL_SetRenderDrawColor(rdr, C_DIV.r, C_DIV.g, C_DIV.b, 255);
                SDL_RenderDrawLine(rdr, 0, iy + ITEM_H - 1, SW, iy + ITEM_H - 1);

                int iconY = iy + (ITEM_H - ICON_SZ) / 2;
                if (i < (int)icons.size() && icons[i]) {
                    SDL_Rect dst = {20, iconY, ICON_SZ, ICON_SZ};
                    SDL_RenderCopy(rdr, icons[i], nullptr, &dst);
                } else {
                    fill(20, iconY, ICON_SZ, ICON_SZ, C_ICON_PH);
                }

                int tx   = 20 + ICON_SZ + 16;
                int maxW = SW - tx - 30;
                drawText(fLg, clamp(fLg, apks[i].appName, maxW), C_WHITE, tx, iy + 16);

                std::string pkgLine =
                    (apks[i].packageName.empty() ? apks[i].filename : apks[i].packageName);
                if (!apks[i].versionName.empty())
                    pkgLine += "  v" + apks[i].versionName;
                drawText(fSm, clamp(fSm, pkgLine, maxW), C_GRAY, tx, iy + 58);
            }
            // Scrollbar
            if ((int)apks.size() > VISIBLE) {
                int barH = LIST_H * VISIBLE / (int)apks.size();
                int barY = LIST_Y + LIST_H * scroll / (int)apks.size();
                fill(SW - 6, barY, 6, barH, {80, 80, 130, 200});
            }
        }

        // Footer
        fill(0, SH - FOOTER_H, SW, FOOTER_H, C_FOOTER);
        drawText(fSm, "A: Launch     Y: Rescan     +: Quit",
            C_DIM, 30, SH - FOOTER_H + (FOOTER_H - 18) / 2);

        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Show the current launch stage on-screen so the user isn't staring
    // at a frozen display while extraction / ELF loading runs.
    // ------------------------------------------------------------------
    void showProgress(const char* stage, const char* detail) {
        fill(0, 0, SW, SH, C_BG);
        fill(0, 0, SW, HEADER_H, C_HEADER);
        drawText(fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);
        drawText(fSm, "Launching...", C_DIM, SW - 160, (HEADER_H - 18) / 2);

        int y = LIST_Y + 50;
        drawText(fLg, stage ? stage : "Working...", C_WHITE, 40, y);
        y += 48;
        if (detail && detail[0])
            drawText(fSm, detail, C_GRAY, 40, y);

        fill(0, SH - FOOTER_H, SW, FOOTER_H, C_FOOTER);
        drawText(fSm, "Please wait — check sdmc:/BareDroidNX/compat_log.txt for details",
                 C_DIM, 30, SH - FOOTER_H + (FOOTER_H - 18) / 2);

        SDL_RenderPresent(rdr);
    }

    // ------------------------------------------------------------------
    // Show the result of a launch attempt with full diagnostic info.
    // ------------------------------------------------------------------
    void showLaunchResult(const LaunchResult& res, int idx) {
        if (idx < 0 || idx >= (int)apks.size()) return;
        const ApkInfo& apk = apks[idx];

        bool done = false;
        while (!done) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev)) {
                if (ev.type == SDL_QUIT) { done = true; }
                if (ev.type == SDL_JOYBUTTONDOWN &&
                    ev.jbutton.button == BTN_B)   { done = true; }
                if (ev.type == SDL_KEYDOWN &&
                    ev.key.keysym.sym == SDLK_ESCAPE) { done = true; }
            }

            fill(0, 0, SW, SH, C_BG);
            fill(0, 0, SW, HEADER_H, C_HEADER);
            drawText(fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);

            // Game icon (small)
            int iconSz = 96;
            if (idx < (int)icons.size() && icons[idx]) {
                SDL_Rect dst = {(SW - iconSz) / 2, LIST_Y + 16, iconSz, iconSz};
                SDL_RenderCopy(rdr, icons[idx], nullptr, &dst);
            }

            // Game name
            int nameY = LIST_Y + 16 + iconSz + 12;
            {
                int w = 0, h = 0;
                std::string nm = clamp(fLg, apk.appName, SW - 80);
                TTF_SizeUTF8(fLg, nm.c_str(), &w, &h);
                drawText(fLg, nm, C_WHITE, (SW - w) / 2, nameY);
            }

            // Status banner
            std::string statusStr = res.ok ? "Launch OK" : "Launch Failed";
            SDL_Color   statusCol = res.ok ? C_OK : C_ERR;
            {
                int w = 0, h = 0;
                TTF_SizeUTF8(fLg, statusStr.c_str(), &w, &h);
                drawText(fLg, statusStr, statusCol, (SW - w) / 2, nameY + 44);
            }

            int y = nameY + 100;

            // Failure details
            if (!res.ok) {
                if (!res.errorStage.empty()) {
                    std::string s = "Failed at:  " + res.errorStage;
                    drawText(fSm, s, C_WARN, 60, y);  y += 28;
                }
                if (!res.errorDetail.empty()) {
                    drawText(fSm, res.errorDetail, C_GRAY, 60, y);  y += 28;
                }
            }

            // Unresolved symbols — shown for both success and failure
            if (res.unresolved > 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "Unresolved symbols: %d  (may crash when those code paths execute)",
                         res.unresolved);
                drawText(fSm, buf, C_WARN, 60, y);  y += 28;
            }

            // svc memory permission failure (0xD801 = kernel won't allow Rx on heap)
            if (res.svcPermCode != 0) {
                char buf[128];
                snprintf(buf, sizeof(buf),
                         "svcSetMemPerm: 0x%08X — code pages not executable (JIT blocked)",
                         res.svcPermCode);
                drawText(fSm, buf, C_ERR, 60, y);  y += 28;
                drawText(fSm,
                         "Heap memory cannot be made Rx on Switch. JIT-capable alloc needed.",
                         C_GRAY, 60, y);  y += 28;
            }

            y += 8;
            drawText(fSm, "Full log: sdmc:/BareDroidNX/compat_log.txt", C_DIM, 60, y);

            fill(0, SH - FOOTER_H, SW, FOOTER_H, C_FOOTER);
            drawText(fSm, "B: Back to menu",
                     C_DIM, 30, SH - FOOTER_H + (FOOTER_H - 18) / 2);

            SDL_RenderPresent(rdr);
            SDL_Delay(16);
        }
    }
};

// ---------------------------------------------------------------------------
// Global progress callback — bridges loader.cpp → App::showProgress
// Must be defined after App so the method is in scope.
// ---------------------------------------------------------------------------
static App* g_app_ptr = nullptr;

static void progressCallback(const char* stage, const char* detail) {
    if (g_app_ptr) g_app_ptr->showProgress(stage, detail);
}

// ---------------------------------------------------------------------------
int main(int, char**) {
    App app;
    g_app_ptr = &app;

    if (!app.init()) return 1;

    mkdir(APK_DIR, 0777);

    // Scanning splash
    app.fill(0, 0, SW, SH, C_BG);
    app.fill(0, 0, SW, HEADER_H, C_HEADER);
    app.drawText(app.fLg, "BareDroidNX", C_WHITE, 30, (HEADER_H - 28) / 2);
    app.drawText(app.fSm, "Scanning for APKs...", C_GRAY, 30, LIST_Y + 30);
    SDL_RenderPresent(app.rdr);

    app.apks = scanApks(APK_DIR);
    app.loadIcons();
    app.render();

    bool   quit      = false;
    Uint32 lastStick = 0;

    while (!quit) {
        SDL_Event ev;
        bool redraw = false;

        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_QUIT) { quit = true; break; }

            if (ev.type == SDL_JOYBUTTONDOWN) {
                switch (ev.jbutton.button) {
                    case BTN_PLUS:
                        quit = true;
                        break;

                    case BTN_A:
                        if (!app.apks.empty()) {
                            const ApkInfo& apk = app.apks[app.selected];
                            std::string pkg = apk.packageName.empty()
                                                ? apk.filename : apk.packageName;

                            // Show a "preparing" frame before the loader starts
                            app.showProgress("Preparing launch", apk.appName.c_str());

                            LaunchResult res = launchApk(apk.path, pkg, progressCallback);
                            app.showLaunchResult(res, app.selected);
                            redraw = true;
                        }
                        break;

                    case BTN_Y:
                        app.rescan();
                        redraw = true;
                        break;

                    case BTN_B:
                        break;
                }
            }

            if (ev.type == SDL_JOYHATMOTION) {
                if (ev.jhat.value & SDL_HAT_DOWN) {
                    if (!app.apks.empty() && app.selected < (int)app.apks.size() - 1) {
                        app.selected++;
                        if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                        redraw = true;
                    }
                }
                if (ev.jhat.value & SDL_HAT_UP) {
                    if (!app.apks.empty() && app.selected > 0) {
                        app.selected--;
                        if (app.selected < app.scroll) app.scroll--;
                        redraw = true;
                    }
                }
            }

            // Left stick fallback
            if (ev.type == SDL_JOYAXISMOTION && ev.jaxis.axis == 1) {
                Uint32 now = SDL_GetTicks();
                if (now - lastStick > 180) {
                    if (ev.jaxis.value > 16384 && !app.apks.empty() &&
                        app.selected < (int)app.apks.size() - 1) {
                        app.selected++;
                        if (app.selected >= app.scroll + VISIBLE) app.scroll++;
                        lastStick = now; redraw = true;
                    } else if (ev.jaxis.value < -16384 && !app.apks.empty() &&
                               app.selected > 0) {
                        app.selected--;
                        if (app.selected < app.scroll) app.scroll--;
                        lastStick = now; redraw = true;
                    }
                }
            }
        }

        if (redraw) app.render();
        SDL_Delay(8);
    }

    app.cleanup();
    return 0;
}
