// Edited paintings: which picture and size each one has, saved with the painting (an extra
// string tag "PaintingEditor" = "WxH:<file>" next to the vanilla "Motive"), and drawn in
// the world with the picture on the front instead of the vanilla art.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

#include <mutex>
#include <unordered_map>

#include "pe.h"

// ---------------------------------------------------------------- sizes
// width x height in blocks. The sizes that exist in vanilla use a vanilla motive of the same
// size (so the world still opens with plain paintings of the right size without the mod);
// the others get a motive of their own.
const PaintingSize kSizes[] = {{1, 1}, {2, 1}, {1, 2}, {2, 2}, {2, 4}, {3, 3}, {3, 4}, {4, 3}, {4, 4}};
const int kSizeCount = sizeof kSizes / sizeof kSizes[0];

namespace {
const char* const kVanillaMotive[] = {"Kebab", "Pool", "Wanderer", "Match", nullptr, nullptr, nullptr, "Skeleton", "Pointer"};
g::Motive* g_motives[sizeof kSizes / sizeof kSizes[0]];
const std::string kTagName = "PaintingEditor";

std::mutex g_lock;
std::unordered_map<void*, Edit> g_edits;   // edited paintings
std::unordered_map<void*, double> g_live;  // paintings seen by HangingEntity::normalTick (time)
}  // namespace

void* g_textureGroup = nullptr;

g::Motive* motiveForSize(int i) {
    if (i < 0 || i >= kSizeCount) return nullptr;
    if (!g_motives[i]) {
        g::Motive* m = nullptr;
        if (kVanillaMotive[i]) {
            m = g::motiveByName(kVanillaMotive[i]);
            if (m && (m->w != kSizes[i].w * 16 || m->h != kSizes[i].h * 16)) m = nullptr;
        }
        if (!m) {
            char name[48];
            snprintf(name, sizeof name, "PaintingEditor_%dx%d", kSizes[i].w, kSizes[i].h);
            m = new g::Motive{name, kSizes[i].w * 16, kSizes[i].h * 16, 0, 0, false};
        }
        g_motives[i] = m;
    }
    return g_motives[i];
}

int sizeIndexOf(int w, int h) {
    for (int i = 0; i < kSizeCount; i++)
        if (kSizes[i].w == w && kSizes[i].h == h) return i;
    return -1;
}

int currentSizeIndex(void* p) {
    Edit e;
    if (getEdit(p, e)) return sizeIndexOf(e.w, e.h);
    g::Motive* m = g::paintingMotive(p);
    if (!m) return 0;
    int w = m->w / 16, h = m->h / 16;
    int i = sizeIndexOf(w, h);
    if (i >= 0) return i;
    // a vanilla size that is not in the list (4x2): the closest one
    int best = 0, bestScore = 1 << 30;
    for (int k = 0; k < kSizeCount; k++) {
        int s = abs(kSizes[k].w - w) * 4 + abs(kSizes[k].h - h) * 4 + abs(kSizes[k].w * kSizes[k].h - w * h);
        if (s < bestScore) bestScore = s, best = k;
    }
    return best;
}

bool getEdit(void* p, Edit& out) {
    std::lock_guard lk(g_lock);
    auto it = g_edits.find(p);
    if (it == g_edits.end()) return false;
    out = it->second;
    return true;
}

static void applySize(void* p, int w, int h) {
    g::Motive* m = motiveForSize(sizeIndexOf(w, h));
    if (!m) return;
    g::paintingMotive(p) = m;
    g::setDir(p, g::hangingDir(p));  // recomputes position and bounding box for the new size
}

void setEdit(void* p, const Edit& e, EditFrom from) {
    {
        std::lock_guard lk(g_lock);
        g_edits[p] = e;
    }
    applySize(p, e.w, e.h);
    static const char* const kFrom[] = {"edited", "loaded", "from the network"};
    peLog("painting %p (%s): %dx%d, turned %d, %s, %s", p, kFrom[(int)from], e.w, e.h, e.rot * 90,
          e.flip ? "mirrored" : "not mirrored", e.file.c_str());
    netEditApplied(p, e, from);
}

std::vector<std::pair<void*, Edit>> allEdits() {
    std::lock_guard lk(g_lock);
    return {g_edits.begin(), g_edits.end()};
}

bool paintingAlive(void* p) {
    std::lock_guard lk(g_lock);
    return g_live.count(p) || g_edits.count(p);
}

void* nearestPainting(void* player, float radius) {
    if (!player) return nullptr;
    const g::Vec3& pp = g::entityPos(player);
    int dim = g::entityDim(player);
    void* level = g::entityLevel(player);
    double now = peNow();
    void* best = nullptr;
    float bestD = radius * radius;
    std::lock_guard lk(g_lock);
    for (auto it = g_live.begin(); it != g_live.end();) {
        void* p = it->first;
        if (now - it->second > 5.0) {  // not ticked for a while (should have been destroyed)
            it = g_live.erase(it);
            continue;
        }
        ++it;
        if (g::entityLevel(p) != level || g::entityDim(p) != dim) continue;
        // distance from the player's body (the position is at eye height) to the nearest point
        // of the painting's front: a large painting counts as near when its edge is near
        const g::Vec3& q = g::entityPos(p);  // center of the painting
        g::Motive* m = g::paintingMotive(p);
        float hw = m ? m->w / 32.0f : 0.5f, hh = m ? m->h / 32.0f : 0.5f;
        float dx = pp.x - q.x, dy = (pp.y - 0.6f) - q.y, dz = pp.z - q.z;
        dy = dy > hh ? dy - hh : (dy < -hh ? dy + hh : 0);
        if (g::hangingDir(p) & 1) dz = dz > hw ? dz - hw : (dz < -hw ? dz + hw : 0);  // spans z (east / west wall)
        else dx = dx > hw ? dx - hw : (dx < -hw ? dx + hw : 0);                      // spans x
        float d = dx * dx + dy * dy + dz * dz;
        if (d < bestD) bestD = d, best = p;
    }
    return best;
}

// ---------------------------------------------------------------- hooks
namespace {

using RenderFn = void (*)(void*, void*, const g::Vec3*, float, float);
using SaveFn = void (*)(void*, void*);
using DtorFn = void* (*)(void*);
using TickFn = void (*)(void*);
using SurviveFn = bool (*)(void*, void*);
RenderFn o_render;
SaveFn o_addSave, o_readSave;
DtorFn o_entityDtor;
TickFn o_hangingTick;
SurviveFn o_wouldSurvive;

const g::Vec3 kNegZ{0, 0, -1}, kPosZ{0, 0, 1}, kPosY{0, 1, 0}, kNegY{0, -1, 0}, kPosX{1, 0, 0}, kNegX{-1, 0, 0};

// kz.png: the back of a painting is the 16x16 tile at (192, 0); the edges use a 1-pixel
// line of it (same UVs as PaintingRenderer::_buildPainting).
constexpr float kBackU0 = 0.75f, kBackU1 = 0.8125f, kBackV0 = 0.0f, kBackV1 = 0.0625f;
constexpr float kEdgeV = 0.0019531f, kSideU = 0.75195f;

// Back and edges of a W x H pixel painting, 1 pixel thick (z -0.5 .. 0.5).
void buildFrame(float W, float H) {
    int bw = (int)(W / 16), bh = (int)(H / 16);
    float x0 = -W / 2, y0 = -H / 2;
    g::tessBegin(4 * (bw * bh + 2 * bw + 2 * bh));
    g::tessNormal(kPosZ);
    for (int i = 0; i < bw; i++)
        for (int j = 0; j < bh; j++) {
            float a = x0 + 16 * i, b = a + 16, c = y0 + 16 * j, d = c + 16;
            g::tessVertexUV(b, d, 0.5f, kBackU0, kBackV0);
            g::tessVertexUV(a, d, 0.5f, kBackU1, kBackV0);
            g::tessVertexUV(a, c, 0.5f, kBackU1, kBackV1);
            g::tessVertexUV(b, c, 0.5f, kBackU0, kBackV1);
        }
    for (int i = 0; i < bw; i++) {
        float a = x0 + 16 * i, b = a + 16;
        g::tessNormal(kPosY);  // top
        g::tessVertexUV(b, -y0, -0.5f, kBackU0, kEdgeV);
        g::tessVertexUV(a, -y0, -0.5f, kBackU1, kEdgeV);
        g::tessVertexUV(a, -y0, 0.5f, kBackU1, kEdgeV);
        g::tessVertexUV(b, -y0, 0.5f, kBackU0, kEdgeV);
        g::tessNormal(kNegY);  // bottom
        g::tessVertexUV(b, y0, 0.5f, kBackU0, kEdgeV);
        g::tessVertexUV(a, y0, 0.5f, kBackU1, kEdgeV);
        g::tessVertexUV(a, y0, -0.5f, kBackU1, kEdgeV);
        g::tessVertexUV(b, y0, -0.5f, kBackU0, kEdgeV);
    }
    for (int j = 0; j < bh; j++) {
        float c = y0 + 16 * j, d = c + 16;
        g::tessNormal(kPosX);  // right (x = +W/2)
        g::tessVertexUV(-x0, d, 0.5f, kSideU, kBackV0);
        g::tessVertexUV(-x0, c, 0.5f, kSideU, kBackV1);
        g::tessVertexUV(-x0, c, -0.5f, kSideU, kBackV1);
        g::tessVertexUV(-x0, d, -0.5f, kSideU, kBackV0);
        g::tessNormal(kNegX);  // left
        g::tessVertexUV(x0, d, -0.5f, kSideU, kBackV0);
        g::tessVertexUV(x0, c, -0.5f, kSideU, kBackV1);
        g::tessVertexUV(x0, c, 0.5f, kSideU, kBackV1);
        g::tessVertexUV(x0, d, 0.5f, kSideU, kBackV0);
    }
}

// Front (z = -0.5, facing -z). Seen from the front, +x is on the left (like the vanilla art).
// u / v: texture coordinates at the top left, top right, bottom right, bottom left corners.
void buildFront(float W, float H, const float u[4], const float v[4]) {
    g::tessBegin(4);
    g::tessNormal(kNegZ);
    g::tessVertexUV(W / 2, -H / 2, -0.5f, u[3], v[3]);
    g::tessVertexUV(-W / 2, -H / 2, -0.5f, u[2], v[2]);
    g::tessVertexUV(-W / 2, H / 2, -0.5f, u[1], v[1]);
    g::tessVertexUV(W / 2, H / 2, -0.5f, u[0], v[0]);
}

// PaintingRenderer::render for edited paintings: the same transform as the vanilla one,
// then the frame with kz.png and the front with the picture.
void renderEdited(void* renderer, void* e, const g::Vec3* pos, float rot, float alpha, const Edit& ed) {
    if (!g_textureGroup) g_textureGroup = *reinterpret_cast<void**>((char*)renderer + g::kRendererTextures);
    std::shared_ptr<Asset> a = getAsset(ed.file);
    float W = ed.w * 16.0f, H = ed.h * 16.0f;

    g::MatrixRef ref = g::pushWorldMatrix();
    float* m = ref.m;
    // translate(pos)
    for (int k = 0; k < 4; k++) m[12 + k] += m[k] * pos->x + m[4 + k] * pos->y + m[8 + k] * pos->z;
    g::markDirty(&ref);
    // rotate(rot, 0, 1, 0)
    float s = sinf(rot * 0.017453292f), c = cosf(rot * 0.017453292f);
    for (int k = 0; k < 4; k++) {
        float c0 = m[k], c2 = m[8 + k];
        m[k] = c0 * c - c2 * s;
        m[8 + k] = c0 * s + c2 * c;
    }
    g::setupShaderParameters(renderer, e, alpha);
    g::markDirty(&ref);
    for (int k = 0; k < 12; k++) m[k] *= 0.0625f;  // 1/16: units are pixels from here

    const void* material = g::entityMaterial(renderer);
    const g::TexturePtr* kz = reinterpret_cast<const g::TexturePtr*>((char*)renderer + g::kPaintingRendererTexture);
    buildFrame(W, H);
    g::tessDraw(material, kz);

    const g::TexturePtr* tex = a ? assetTexture(*a, g_textureGroup) : nullptr;
    if (tex) {
        float u[4], v[4];
        pictureUV(*a, ed.w, ed.h, ed.rot, ed.flip, u, v);
        buildFront(W, H, u, v);
        // alpha-tested material: transparent parts of the picture stay transparent
        g::tessDraw((char*)renderer + g::kRendererAlphaTestMaterial, tex);
    } else {  // still decoding (or failed): a blank canvas
        const float u[4] = {kBackU1, kBackU0, kBackU0, kBackU1}, v[4] = {kBackV0, kBackV0, kBackV1, kBackV1};
        buildFront(W, H, u, v);
        g::tessDraw(material, kz);
    }
    g::popMatrix(&ref);
}

void h_render(void* renderer, void* e, const g::Vec3* pos, float rot, float alpha) {
    Edit ed;
    if (!g_textureGroup) g_textureGroup = *reinterpret_cast<void**>((char*)renderer + g::kRendererTextures);
    if (g::isPainting(e) && getEdit(e, ed)) {
        renderEdited(renderer, e, pos, rot, alpha, ed);
        return;
    }
    o_render(renderer, e, pos, rot, alpha);
}

void h_addSave(void* p, void* tag) {
    o_addSave(p, tag);
    Edit ed;
    if (getEdit(p, ed)) {
        // "WxH:file", or "WxH/turns/flip:file" when the picture is turned or mirrored
        char v[48];
        if (ed.rot || ed.flip) snprintf(v, sizeof v, "%dx%d/%d/%d:", ed.w, ed.h, ed.rot & 3, ed.flip ? 1 : 0);
        else snprintf(v, sizeof v, "%dx%d:", ed.w, ed.h);
        g::tagPutString(tag, kTagName, std::string(v) + ed.file);
    }
}

void h_readSave(void* p, void* tag) {
    o_readSave(p, tag);
    const std::string& v = g::tagGetString(tag, kTagName);
    if (v.empty()) return;
    int w = 0, h = 0, rot = 0, flip = 0, n = 0;
    bool ok = sscanf(v.c_str(), "%dx%d/%d/%d:%n", &w, &h, &rot, &flip, &n) == 4 && n > 0;
    if (!ok) {
        n = 0;
        rot = flip = 0;
        ok = sscanf(v.c_str(), "%dx%d:%n", &w, &h, &n) == 2 && n > 0;
    }
    if (!ok || sizeIndexOf(w, h) < 0 || (size_t)n >= v.size()) {
        peLog("painting %p: unknown save data \"%s\"", p, v.c_str());
        return;
    }
    Edit ed;
    ed.w = w;
    ed.h = h;
    ed.rot = rot & 3;
    ed.flip = flip != 0;
    ed.file = v.substr(n);
    setEdit(p, ed, EditFrom::Save);
}

void* h_entityDtor(void* e) {
    bool had = false;
    {
        std::lock_guard lk(g_lock);
        had = g_live.erase(e) + g_edits.erase(e) > 0;
    }
    if (had) editorPaintingRemoved(e);
    return o_entityDtor(e);
}

void h_hangingTick(void* e) {
    o_hangingTick(e);
    if (g::isPainting(e)) {
        {
            std::lock_guard lk(g_lock);
            g_live[e] = peNow();
        }
        netPaintingTick(e);
    }
}

// The motives of the sizes that vanilla does not have ("PaintingEditor_3x3"): a game that
// joins a game hosting such a painting gets its size from AddPaintingPacket's motive name.
using MotiveByNameFn = g::Motive* (*)(const std::string*);
MotiveByNameFn o_motiveByName;
g::Motive* h_motiveByName(const std::string* name) {
    int w = 0, h = 0;
    if (name && name->compare(0, 15, "PaintingEditor_") == 0 && sscanf(name->c_str() + 15, "%dx%d", &w, &h) == 2) {
        if (g::Motive* m = motiveForSize(sizeIndexOf(w, h))) return m;
    }
    return o_motiveByName(name);
}

// An edited painting keeps the size it was given even where a vanilla painting of that
// size would not fit (it does not drop as an item every 5 seconds); it can still be broken.
bool h_wouldSurvive(void* e, void* region) {
    if (o_wouldSurvive(e, region)) return true;
    if (g::isPainting(e)) {
        std::lock_guard lk(g_lock);
        if (g_edits.count(e)) return true;
    }
    return false;
}

template <class F>
void hook(uint64_t target, void* repl, F& orig, const char* name) {
    uint64_t o = 0;
    if (emu_hook(g::addr(target), (uint64_t)repl, &o) == 0) orig = reinterpret_cast<F>(o);
    else peLog("cannot hook %s (0x%llx)", name, (unsigned long long)target);
}

}  // namespace

void installPaintingHooks() {
    hook(g::kPaintingRendererRender, (void*)h_render, o_render, "PaintingRenderer::render");
    hook(g::kPaintingAddSaveData, (void*)h_addSave, o_addSave, "Painting::addAdditionalSaveData");
    hook(g::kPaintingReadSaveData, (void*)h_readSave, o_readSave, "Painting::readAdditionalSaveData");
    hook(g::kEntityDtor, (void*)h_entityDtor, o_entityDtor, "Entity::~Entity");
    hook(g::kHangingNormalTick, (void*)h_hangingTick, o_hangingTick, "HangingEntity::normalTick");
    hook(g::kHangingWouldSurvive, (void*)h_wouldSurvive, o_wouldSurvive, "HangingEntity::wouldSurvive");
    hook(g::kMotiveByName, (void*)h_motiveByName, o_motiveByName, "Motive::getMotiveByName");
}
