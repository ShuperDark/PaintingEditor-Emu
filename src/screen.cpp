// The editor screen: a legacy Screen (like the game's MobEffectsScreen) with its own
// vtable. It asks the emulator for a picture, then shows a preview of the painting that can
// be turned by dragging, with tall arrow buttons on both sides that change the size and an
// X button (or Esc) that closes it and keeps the size shown.
//
// The game keeps screens in std::shared_ptr<AbstractScreen>; the object is a Screen built
// with Screen::Screen(MinecraftClient&) whose vtable pointer is replaced by a copy of
// Screen's vtable with the methods below.
#include <dlfcn.h>
#include <math.h>
#include <string.h>

#include <atomic>

#include "pe.h"

namespace {

enum class Phase { Picking, Loading, Preview, Failed };
enum Button { kNone = 0, kLeft, kRight, kClose, kRotate, kFlip };

struct Editor {
    void* screen = nullptr;
    void* client = nullptr;
    void* painting = nullptr;
    Phase phase = Phase::Picking;
    std::string file;
    std::shared_ptr<Asset> asset;
    int sizeIndex = 0;
    int rot = 0;        // clockwise quarter turns of the picture
    bool flip = false;  // mirrored left-right
    float yaw = -25, pitch = 12;
    bool dragging = false;
    int lastX = 0, lastY = 0;
    int pressed = kNone;
    bool closing = false;
    double failedAt = 0;
};
Editor* g_ed = nullptr;               // the open editor (main thread)
std::atomic<void*> g_edPainting{nullptr};  // its painting (read by Entity::~Entity on any thread)
std::atomic<bool> g_paintingGone{false};

uint64_t g_vt[2 + g::kScreenVtSlots];
uint64_t g_screenDtor = 0;

// ---- picture picker of the emulator (emu_mod.h); looked up with dlsym so that an older
// emulator without emu_pick_media_begin still works (PNG / JPEG / BMP only)
struct Picker {
    void (*begin)() = nullptr;
    int (*poll)(size_t*) = nullptr;
    void (*take)(void*) = nullptr;
    void (*cancel)() = nullptr;  // newer emulators only
    bool media = false;
} g_picker;

void initPicker() {
    if (g_picker.poll) return;
    g_picker.begin = (void (*)())dlsym(RTLD_DEFAULT, "emu_pick_media_begin");
    g_picker.media = g_picker.begin != nullptr;
    if (!g_picker.begin) g_picker.begin = (void (*)())dlsym(RTLD_DEFAULT, "emu_pick_image_begin");
    g_picker.poll = (int (*)(size_t*))dlsym(RTLD_DEFAULT, "emu_pick_image_poll");
    g_picker.take = (void (*)(void*))dlsym(RTLD_DEFAULT, "emu_pick_image_take");
    g_picker.cancel = (void (*)())dlsym(RTLD_DEFAULT, "emu_pick_image_cancel");
    if (!g_picker.media) peLog("emu_pick_media_begin not found (old emulator): GIF files cannot be picked on the PC");
}

// ---- GUI textures of the vanilla resource pack
struct GuiTex {
    const char* path;
    g::TexturePtr tex;
    bool loaded = false;
};
GuiTex g_tex[] = {
    {"textures/gui/newgui/buttons/border/base", {}},      // 0 (7x7)
    {"textures/gui/newgui/buttons/border/hover", {}},     // 1
    {"textures/gui/newgui/buttons/border/basePress", {}}, // 2
    {"textures/gui/newgui/arrowLeft", {}},              // 3
    {"textures/gui/newgui/arrowRight", {}},             // 4
    {"textures/gui/newgui/XHover", {}},                 // 5
    {"textures/gui/newgui/XPress", {}},                 // 6
    {"textures/painting/kz", {}},                       // 7
    {"textures/gui/newgui/undoArrow", {}},              // 8 (14x14, drawn mirrored: "turn clockwise")
    {"textures/gui/newgui/Rotate", {}},                 // 9 (45x7, "<- o ->": mirror left-right)
    {"textures/gui/spritesheet", {}},                   // 10 (pocket close button at 60,0 / 78,0)
};
enum { kTexButton, kTexButtonHover, kTexButtonPressed, kTexArrowLeft, kTexArrowRight, kTexX, kTexXPress, kTexKz,
       kTexTurn, kTexMirror, kTexSpritesheet };

const g::TexturePtr* guiTex(int i, void* group) {
    GuiTex& t = g_tex[i];
    if (!t.loaded && group) {
        g::ResourceLocation loc{t.path, "InUserPackage"};
        g::texturePtrLoad(&t.tex, group, loc);
        t.loaded = true;
    }
    return t.loaded ? &t.tex : nullptr;
}

constexpr g::Color kWhite{1, 1, 1, 1};

void quad(const g::TexturePtr* t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    if (!t) return;
    g::tessBegin(4);
    g::tessVertexUV(x0, y1, 0, u0, v1);
    g::tessVertexUV(x1, y1, 0, u1, v1);
    g::tessVertexUV(x1, y0, 0, u1, v0);
    g::tessVertexUV(x0, y0, 0, u0, v0);
    g::tessDraw(g::screenMaterial(0), t);
}

// quad() with a per-vertex color (screen material 3, "ui_texture_and_color": texture x color)
void quadTinted(const g::TexturePtr* t, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1,
                const g::Color& c) {
    if (!t) return;
    g::tessBegin(4);
    g::tessColor(c);
    g::tessVertexUV(x0, y1, 0, u0, v1);
    g::tessVertexUV(x1, y1, 0, u1, v1);
    g::tessVertexUV(x1, y0, 0, u1, v0);
    g::tessVertexUV(x0, y0, 0, u0, v0);
    g::tessDraw(g::screenMaterial(3), t);
}

// 9-slice of a 7x7 button texture (buttons/border/*) with 2-texel borders
void nineSlice(const g::TexturePtr* t, float x, float y, float w, float h) {
    const float b = 2, tb = 2.0f / 7;
    float xs[4] = {x, x + b, x + w - b, x + w}, ys[4] = {y, y + b, y + h - b, y + h};
    float us[4] = {0, tb, 1 - tb, 1};
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) quad(t, xs[i], ys[j], xs[i + 1], ys[j + 1], us[i], us[j], us[i + 1], us[j + 1]);
}

struct Rect {
    int x, y, w, h;
    bool contains(int px, int py) const { return px >= x && py >= y && px < x + w && py < y + h; }
};
struct Layout {
    Rect left, right, close, area, rotate, flip;
};
Layout layout(void* self) {
    int W = *(int*)((char*)self + g::kScreenWidth), H = *(int*)((char*)self + g::kScreenHeight);
    Layout l;
    int bw = 24, bh = H * 55 / 100;
    if (bh > 150) bh = 150;
    if (bh < 40) bh = 40;
    int margin = W / 14 > 12 ? W / 14 : 12;
    l.left = {margin, (H - bh) / 2, bw, bh};
    l.right = {W - margin - bw, (H - bh) / 2, bw, bh};
    // PC: the X of newgui (like the PC screens); phone: the close button of the pocket
    // container screens (chest, furnace...): 18x18 at the top right corner
    l.close = touchMode() ? Rect{W - 18, 1, 18, 18} : Rect{W - 22, 4, 18, 18};
    int ax0 = l.left.x + bw + 12, ax1 = l.right.x - 12;
    // preview, then the row with the turn / mirror buttons, then the size, then the hint
    const int rowH = 20, rowGap = 4;
    l.area = {ax0, 30, ax1 - ax0, H - 30 - 34 - rowH - rowGap};
    const int bw2 = 52, gap = 6;
    int ry = l.area.y + l.area.h + rowGap;
    l.rotate = {W / 2 - gap / 2 - bw2, ry, bw2, rowH};
    l.flip = {W / 2 + gap / 2, ry, bw2, rowH};
    return l;
}

int hit(void* self, int x, int y) {
    Layout l = layout(self);
    if (l.close.contains(x, y)) return kClose;
    if (!g_ed || g_ed->phase != Phase::Preview) return kNone;
    if (l.left.contains(x, y)) return kLeft;
    if (l.right.contains(x, y)) return kRight;
    if (l.rotate.contains(x, y)) return kRotate;
    if (l.flip.contains(x, y)) return kFlip;
    return kNone;
}

// ---- preview of the painting, projected on the CPU (the screen draws in 2D)
struct P3 { float x, y, z; };
struct Proj {
    float m[9];  // rotation
    float D, ppb, cx, cy;
    P3 rot(P3 p) const {
        return {m[0] * p.x + m[1] * p.y + m[2] * p.z, m[3] * p.x + m[4] * p.y + m[5] * p.z, m[6] * p.x + m[7] * p.y + m[8] * p.z};
    }
    void screen(P3 p, float& sx, float& sy) const {
        P3 q = rot(p);
        float k = D / (q.z + D);
        sx = cx + ppb * q.x * k;
        sy = cy - ppb * q.y * k;
    }
    // the face with outward normal n through point c faces the camera at (0, 0, -D)
    bool facing(P3 n, P3 c) const {
        P3 rn = rot(n), rc = rot(c);
        return rn.x * -rc.x + rn.y * -rc.y + rn.z * (-D - rc.z) > 0;
    }
};

// A quad (corners a b c d in 3D, in order) with the texture coordinates cu / cv at its
// corners, split into n x n pieces so that the texture follows the perspective.
void face(const Proj& pr, P3 a, P3 b, P3 c, P3 d, const float cu[4], const float cv[4], int n) {
    auto lerp = [](P3 p, P3 q, float t) { return P3{p.x + (q.x - p.x) * t, p.y + (q.y - p.y) * t, p.z + (q.z - p.z) * t}; };
    auto bil = [](const float* c, float s, float t) {
        float top = c[0] + (c[1] - c[0]) * s, bottom = c[3] + (c[2] - c[3]) * s;
        return top + (bottom - top) * t;
    };
    for (int i = 0; i < n; i++)
        for (int j = 0; j < n; j++) {
            float s0 = (float)i / n, s1 = (float)(i + 1) / n, t0 = (float)j / n, t1 = (float)(j + 1) / n;
            P3 p[4] = {lerp(lerp(a, b, s0), lerp(d, c, s0), t0), lerp(lerp(a, b, s1), lerp(d, c, s1), t0),
                       lerp(lerp(a, b, s1), lerp(d, c, s1), t1), lerp(lerp(a, b, s0), lerp(d, c, s0), t1)};
            const float ss[4] = {s0, s1, s1, s0}, tt[4] = {t0, t0, t1, t1};
            float uu[4], vv[4];
            for (int k = 0; k < 4; k++) uu[k] = bil(cu, ss[k], tt[k]), vv[k] = bil(cv, ss[k], tt[k]);
            float sx[4], sy[4], area2 = 0;
            for (int k = 0; k < 4; k++) pr.screen(p[k], sx[k], sy[k]);
            for (int k = 0; k < 4; k++) area2 += sx[k] * sy[(k + 1) % 4] - sx[(k + 1) % 4] * sy[k];
            // the screen materials cull back faces: emit every piece with the winding quad()
            // uses (negative area with y pointing down)
            for (int k = 0; k < 4; k++) {
                int q = area2 > 0 ? 3 - k : k;
                g::tessVertexUV(sx[q], sy[q], 0, uu[q], vv[q]);
            }
        }
}
// the same with an axis-aligned texture rectangle: a = (u0, v0), b = (u1, v0), c = (u1, v1), d = (u0, v1)
void face(const Proj& pr, P3 a, P3 b, P3 c, P3 d, float u0, float v0, float u1, float v1, int n) {
    const float cu[4] = {u0, u1, u1, u0}, cv[4] = {v0, v0, v1, v1};
    face(pr, a, b, c, d, cu, cv, n);
}

void drawPreview(const Rect& area, int w, int h, const g::TexturePtr* picture, const float* pu, const float* pv,
                 const g::TexturePtr* kz) {
    Proj pr;
    float cy_ = cosf(g_ed->yaw * 0.017453292f), sy_ = sinf(g_ed->yaw * 0.017453292f);
    float cp = cosf(g_ed->pitch * 0.017453292f), sp = sinf(g_ed->pitch * 0.017453292f);
    // R = Rx(pitch) * Ry(yaw)
    float ry[9] = {cy_, 0, sy_, 0, 1, 0, -sy_, 0, cy_};
    float rx[9] = {1, 0, 0, 0, cp, -sp, 0, sp, cp};
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) pr.m[r * 3 + c] = rx[r * 3] * ry[c] + rx[r * 3 + 1] * ry[3 + c] + rx[r * 3 + 2] * ry[6 + c];
    float diag = sqrtf((float)(w * w + h * h));
    pr.D = 3.2f * diag;
    pr.ppb = fminf(area.w / (diag * 1.08f), area.h / (diag * 1.08f));
    float fit = fminf(area.w / (float)w, area.h / (float)h) * 0.9f;  // flat view: fill more of the area
    pr.ppb = fminf(fmaxf(pr.ppb, fit * 0.75f), fit);
    pr.cx = area.x + area.w / 2.0f;
    pr.cy = area.y + area.h / 2.0f;

    const float X = w / 2.0f, Y = h / 2.0f, Z = 1 / 32.0f;  // 1 pixel thick
    // back: a 16x16 tile of kz.png per block
    if (pr.facing({0, 0, 1}, {0, 0, Z}) && kz) {
        g::setShaderColor({0.55f, 0.55f, 0.55f, 1});
        g::tessBegin(4 * w * h);
        for (int i = 0; i < w; i++)
            for (int j = 0; j < h; j++) {
                float x0 = X - i, x1 = X - i - 1, y0 = Y - j, y1 = Y - j - 1;  // seen from behind
                face(pr, {x0, y0, Z}, {x1, y0, Z}, {x1, y1, Z}, {x0, y1, Z}, 0.75f, 0, 0.8125f, 0.0625f, 1);
            }
        g::tessDraw(g::screenMaterial(0), kz);
    }
    // edges (1-pixel lines of the back tile)
    struct Edge { P3 n, c, a, b, cc, d; float shade; } edges[4] = {
        {{0, 1, 0}, {0, Y, 0}, {-X, Y, -Z}, {X, Y, -Z}, {X, Y, Z}, {-X, Y, Z}, 0.85f},
        {{0, -1, 0}, {0, -Y, 0}, {-X, -Y, Z}, {X, -Y, Z}, {X, -Y, -Z}, {-X, -Y, -Z}, 0.6f},
        {{1, 0, 0}, {X, 0, 0}, {X, Y, -Z}, {X, Y, Z}, {X, -Y, Z}, {X, -Y, -Z}, 0.72f},
        {{-1, 0, 0}, {-X, 0, 0}, {-X, Y, Z}, {-X, Y, -Z}, {-X, -Y, -Z}, {-X, -Y, Z}, 0.72f},
    };
    for (auto& e : edges) {
        if (!kz || !pr.facing(e.n, e.c)) continue;
        g::setShaderColor({e.shade, e.shade, e.shade, 1});
        g::tessBegin(4 * 16);
        face(pr, e.a, e.b, e.cc, e.d, 0.75f, 0.0019531f, 0.8125f, 0.0019531f + 0.001f, 4);
        g::tessDraw(g::screenMaterial(0), kz);
    }
    // front: the picture (or a blank canvas while it loads)
    if (pr.facing({0, 0, -1}, {0, 0, -Z})) {
        g::setShaderColor(kWhite);
        int n = 4 * (w > h ? w : h);
        if (n > 24) n = 24;
        g::tessBegin(4 * n * n);
        if (picture) {
            face(pr, {-X, Y, -Z}, {X, Y, -Z}, {X, -Y, -Z}, {-X, -Y, -Z}, pu, pv, n);
            g::tessDraw(g::screenMaterial(0), picture);
        } else if (kz) {
            face(pr, {-X, Y, -Z}, {X, Y, -Z}, {X, -Y, -Z}, {-X, -Y, -Z}, 0.75f, 0, 0.8125f, 0.0625f, n);
            g::tessDraw(g::screenMaterial(0), kz);
        }
    }
    g::setShaderColor(kWhite);
}

// ---- closing
void closeEditor(bool keep, const char* why) {
    Editor* ed = g_ed;
    if (!ed || ed->closing) return;
    ed->closing = true;
    bool picked = ed->phase == Phase::Loading || ed->phase == Phase::Preview;
    if (ed->phase == Phase::Picking && g_picker.cancel) g_picker.cancel();  // close the file dialog / phone picker
    bool alive = !g_paintingGone.load() && paintingAlive(ed->painting);
    if (keep && picked && alive) {
        Edit e;
        e.file = ed->file;
        e.w = kSizes[ed->sizeIndex].w;
        e.h = kSizes[ed->sizeIndex].h;
        e.rot = ed->rot;
        e.flip = ed->flip;
        setEdit(ed->painting, e);
        peLog("editor closed (%s): %dx%d kept", why, e.w, e.h);
    } else {
        peLog("editor closed (%s): painting not changed", why);
    }
    if (ed->client) g::schedulePopScreen(g::screenChooser(ed->client));
}

// ---- Screen methods
void* s_dtor(void* self) {
    if (g_ed && g_ed->screen == self) {
        if (!g_ed->closing) peLog("editor screen removed by the game");
        delete g_ed;
        g_ed = nullptr;
        g_edPainting = nullptr;
    }
    return ((void* (*)(void*))g_screenDtor)(self);
}
void s_deletingDtor(void* self) {
    s_dtor(self);
    ::operator delete(self);
}
void s_leaveScreen(void* self) {  // the player was hurt (closeOnPlayerHurt)
    if (g_ed && g_ed->screen == self) closeEditor(true, "hurt");
}
bool s_handleBackEvent(void* self, bool down) {
    if (!down && g_ed && g_ed->screen == self) closeEditor(true, "back");
    return true;
}
bool s_true(void*) { return true; }
bool s_false(void*) { return false; }
std::string s_getScreenName(void*) { return "painting_editor_screen"; }
void s_noop(void*) {}

void pollPicker() {
    Editor* ed = g_ed;
    if (!ed || ed->phase != Phase::Picking || !g_picker.poll) return;
    size_t len = 0;
    int r = g_picker.poll(&len);
    if (r == 0) return;
    if (r < 0 || !len) {
        closeEditor(false, "picker cancelled");
        return;
    }
    std::vector<uint8_t> bytes(len);
    g_picker.take(bytes.data());
    if (!*pictureType(bytes.data(), bytes.size())) {
        peLog("picked file is not PNG / JPEG / BMP / GIF (%zu bytes)", len);
        ed->phase = Phase::Failed;
        ed->failedAt = peNow();
        return;
    }
    ed->file = storePicture(bytes);
    if (ed->file.empty()) {
        ed->phase = Phase::Failed;
        ed->failedAt = peNow();
        return;
    }
    ed->asset = getAsset(ed->file, &bytes);
    ed->phase = Phase::Loading;
}

void s_tick(void* self) {
    if (!g_ed || g_ed->screen != self || g_ed->closing) return;
    if (g_paintingGone) {
        closeEditor(false, "painting removed");
        return;
    }
    void* player = g::localPlayer(g_ed->client);
    if (player && !g::isAlive(player)) closeEditor(true, "player died");
}

void s_pointerPressed(void* self, int x, int y) {
    if (!g_ed || g_ed->screen != self) return;
    g_ed->pressed = hit(self, x, y);
    if (g_ed->pressed == kNone && g_ed->phase == Phase::Preview && layout(self).area.contains(x, y)) {
        g_ed->dragging = true;
        g_ed->lastX = x;
        g_ed->lastY = y;
    }
}

void s_pointerReleased(void* self, int x, int y) {
    if (!g_ed || g_ed->screen != self) return;
    int b = hit(self, x, y);
    int pressed = g_ed->pressed;
    g_ed->pressed = kNone;
    g_ed->dragging = false;
    if (b == kNone || b != pressed) return;
    g::playUI(g_ed->client, "random.click");  // the sound of the game's buttons
    if (b == kClose) closeEditor(true, "close button");
    else if (b == kLeft) g_ed->sizeIndex = (g_ed->sizeIndex + kSizeCount - 1) % kSizeCount;
    else if (b == kRight) g_ed->sizeIndex = (g_ed->sizeIndex + 1) % kSizeCount;
    else if (b == kRotate) g_ed->rot = (g_ed->rot + 1) & 3;
    else if (b == kFlip) g_ed->flip = !g_ed->flip;
}

void s_render(void* self, int mx, int my, float) {
    Editor* ed = g_ed;
    if (!ed || ed->screen != self) return;
    pollPicker();
    if (ed->phase == Phase::Loading && ed->asset) {
        if (assetReady(*ed->asset)) ed->phase = Phase::Preview;
        else if (ed->asset->state == 2) ed->phase = Phase::Failed, ed->failedAt = peNow();
    }
    if (ed->phase == Phase::Failed && peNow() - ed->failedAt > 2.5) closeEditor(false, "picture could not be read");
    if (ed->dragging) {
        ed->yaw += (mx - ed->lastX) * 0.9f;
        ed->pitch += (my - ed->lastY) * 0.9f;
        if (ed->pitch > 85) ed->pitch = 85;
        if (ed->pitch < -85) ed->pitch = -85;
        ed->yaw = fmodf(ed->yaw, 360.0f);
        ed->lastX = mx;
        ed->lastY = my;
    }

    void* sr = (char*)self + g::kScreenRenderer;
    void* font = g::clientFont(ed->client);
    void* group = g_textureGroup ? g_textureGroup : g::clientTextures(ed->client);
    int W = *(int*)((char*)self + g::kScreenWidth), H = *(int*)((char*)self + g::kScreenHeight);
    Layout l = layout(self);

    g::fillRect(sr, 0, 0, W, H, {0.32f, 0.32f, 0.32f, 0.62f});  // translucent gray
    g::setShaderColor(kWhite);

    if (ed->phase == Phase::Picking) {
        g::drawCenteredString(sr, font, "画像を選んでください…", W / 2, H / 2 - 10, kWhite);
        g::drawCenteredString(sr, font, g_picker.media ? "写真、画像、GIF (PNG / JPEG / GIF / BMP)" : "写真、画像 (PNG / JPEG / BMP)",
                              W / 2, H / 2 + 4, {0.8f, 0.8f, 0.8f, 1});
    } else if (ed->phase == Phase::Failed) {
        g::drawCenteredString(sr, font, "この画像は読み込めませんでした", W / 2, H / 2 - 4, {1, 0.6f, 0.6f, 1});
    } else {
        const PaintingSize& s = kSizes[ed->sizeIndex];
        const g::TexturePtr* pic = nullptr;
        float pu[4] = {0, 1, 1, 0}, pv[4] = {0, 0, 1, 1};
        if (ed->phase == Phase::Preview && ed->asset) {
            pic = assetTexture(*ed->asset, group);
            pictureUV(*ed->asset, s.w, s.h, ed->rot, ed->flip, pu, pv);
        }
        drawPreview(l.area, s.w, s.h, pic, pu, pv, guiTex(kTexKz, group));

        // size buttons
        int hov = (ed->pressed == kNone) ? hit(self, mx, my) : kNone;
        for (int b = kLeft; b <= kRight; b++) {
            const Rect& r = b == kLeft ? l.left : l.right;
            bool down = ed->pressed == b && r.contains(mx, my);
            int t = down ? kTexButtonPressed : (hov == b ? kTexButtonHover : kTexButton);
            nineSlice(guiTex(t, group), r.x, r.y, r.w, r.h);
            const float is = 24;  // arrowLeft/Right.png: 8x8, drawn 3x
            float ix = r.x + (r.w - is) / 2.0f, iy = r.y + (r.h - is) / 2.0f + (down ? 1 : 0);
            quad(guiTex(b == kLeft ? kTexArrowLeft : kTexArrowRight, group), ix, iy, ix + is, iy + is, 0, 0, 1, 1);
        }
        // turn / mirror buttons (icons of the vanilla GUI, drawn dark like the text of these buttons)
        for (int b = kRotate; b <= kFlip; b++) {
            const Rect& r = b == kRotate ? l.rotate : l.flip;
            bool down = ed->pressed == b && r.contains(mx, my);
            int t = down ? kTexButtonPressed : (hov == b ? kTexButtonHover : kTexButton);
            nineSlice(guiTex(t, group), r.x, r.y, r.w, r.h);
            const g::Color dark{0.3f, 0.3f, 0.3f, 1};
            float oy = down ? 1 : 0;
            if (b == kRotate) {  // undoArrow.png mirrored: an arrow turning clockwise
                float ix = r.x + (r.w - 14) / 2.0f, iy = r.y + (r.h - 14) / 2.0f + oy;
                quadTinted(guiTex(kTexTurn, group), ix, iy, ix + 14, iy + 14, 1, 0, 0, 1, dark);
            } else {  // Rotate.png: "<- o ->"
                float ix = r.x + (r.w - 45) / 2.0f, iy = r.y + (r.h - 7) / 2.0f + oy;
                quadTinted(guiTex(kTexMirror, group), ix, iy, ix + 45, iy + 7, 0, 0, 1, 1, dark);
            }
        }
        char label[64];
        snprintf(label, sizeof label, "%d × %d", s.w, s.h);
        g::drawCenteredString(sr, font, label, W / 2, l.rotate.y + l.rotate.h + 5, kWhite);
        if (ed->phase == Phase::Loading)
            g::drawCenteredString(sr, font, "読み込み中…", W / 2, l.area.y + l.area.h / 2 - 4, kWhite);
        g::drawCenteredString(sr, font, touchMode() ? "ドラッグで回転 / × で決定" : "ドラッグで回転 / × か Esc で決定", W / 2, H - 14,
                              {0.85f, 0.85f, 0.85f, 1});
    }
    g::drawCenteredString(sr, font, "絵画の編集", W / 2, 9, kWhite);

    // close button
    bool overClose = l.close.contains(mx, my);
    bool closeDown = ed->pressed == kClose && overClose;
    if (touchMode()) {  // pocket container screens: spritesheet.png (60, 0) / pressed (78, 0), 18x18
        float u0 = (closeDown ? 78 : 60) / 256.0f, u1 = u0 + 18 / 256.0f;
        quad(guiTex(kTexSpritesheet, group), l.close.x, l.close.y, l.close.x + l.close.w, l.close.y + l.close.h, u0, 0, u1,
             18 / 256.0f);
    } else {
        if (!closeDown && !overClose) g::setShaderColor({0.85f, 0.85f, 0.85f, 1});
        quad(guiTex(closeDown ? kTexXPress : kTexX, group), l.close.x, l.close.y, l.close.x + l.close.w, l.close.y + l.close.h,
             0, 0, 1, 1);
        g::setShaderColor(kWhite);
    }
}

void buildVtable() {
    if (g_vt[2]) return;
    const uint64_t* src = g::ptr<const uint64_t>(g::kScreenVtable);
    memcpy(g_vt, src - 2, sizeof g_vt);  // offset-to-top, type info, slots
    g_screenDtor = src[g::kSlotDtor];
    uint64_t* vt = g_vt + 2;
    vt[g::kSlotDtor] = (uint64_t)&s_dtor;
    vt[g::kSlotDeletingDtor] = (uint64_t)&s_deletingDtor;
    vt[g::kSlotLeaveScreen] = (uint64_t)&s_leaveScreen;
    vt[g::kSlotHandleBackEvent] = (uint64_t)&s_handleBackEvent;
    vt[g::kSlotRenderGameBehind] = (uint64_t)&s_true;
    vt[g::kSlotAbsorbsInput] = (uint64_t)&s_true;
    vt[g::kSlotCloseOnPlayerHurt] = (uint64_t)&s_true;
    vt[g::kSlotIsPauseScreen] = (uint64_t)&s_false;
    vt[g::kSlotGetScreenName] = (uint64_t)&s_getScreenName;
    vt[g::kSlotRender] = (uint64_t)&s_render;
    vt[g::kSlotInit] = (uint64_t)&s_noop;
    vt[g::kSlotSetupPositions] = (uint64_t)&s_noop;
    vt[g::kSlotTick] = (uint64_t)&s_tick;
    vt[g::kSlotPointerPressed] = (uint64_t)&s_pointerPressed;
    vt[g::kSlotPointerReleased] = (uint64_t)&s_pointerReleased;
}

}  // namespace

void openEditor(void* client, void* painting) {
    if (g_ed || !client || !painting) return;
    initPicker();
    if (!g_picker.begin || !g_picker.poll || !g_picker.take) {
        peLog("the emulator has no picture picker (emu_pick_image_*)");
        return;
    }
    buildVtable();
    void* obj = ::operator new(1024);
    memset(obj, 0, 1024);
    g::fn<void* (*)(void*, void*)>(g::kScreenCtor)(obj, client);
    *(uint64_t**)obj = g_vt + 2;

    g_ed = new Editor;
    g_ed->screen = obj;
    g_ed->client = client;
    g_ed->painting = painting;
    g_ed->sizeIndex = currentSizeIndex(painting);
    g_paintingGone = false;
    g_edPainting = painting;

    std::shared_ptr<void> sp(obj, [](void* p) { g::vfn<void (*)(void*)>(p, g::kSlotDeletingDtor)(p); });
    g::pushScreen(g::screenChooser(client), &sp);
    peLog("editor opened for painting %p (size %dx%d)", painting, kSizes[g_ed->sizeIndex].w, kSizes[g_ed->sizeIndex].h);
    g_picker.begin();
}

bool editorOpen() { return g_ed != nullptr; }
void forceCloseEditor(const char* why) { closeEditor(true, why); }
void editorPaintingRemoved(void* p) {
    if (p && g_edPainting.load() == p) g_paintingGone = true;
}
