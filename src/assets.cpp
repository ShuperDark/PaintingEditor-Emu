// Picked pictures: stored once in <home>/Documents/PaintingEditor/ under a name made from
// their contents (so the same picture on many paintings is stored once), decoded in the
// background, and uploaded to the game's TextureGroup frame by frame when first drawn.
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "pe.h"

namespace {

std::mutex g_assetsLock;
std::map<std::string, std::shared_ptr<Asset>> g_assets;

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    uint8_t buf[65536];
    size_t n;
    out.clear();
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    fclose(f);
    return !out.empty();
}

void decodeAsync(std::shared_ptr<Asset> a, std::vector<uint8_t> bytes) {
    std::thread([a, bytes = std::move(bytes)]() mutable {
        if (bytes.empty() && !readFile(picturesDir() + "/" + a->file, bytes)) {
            a->error = "file not found";
            std::atomic_thread_fence(std::memory_order_release);
            a->state = 2;
            peLog("picture %s: cannot read %s", a->file.c_str(), (picturesDir() + "/" + a->file).c_str());
            return;
        }
        DecodedImage img;
        std::string err;
        double t0 = peNow();
        bool ok = decodePicture(bytes.data(), bytes.size(), img, err);
        if (ok) {
            a->img = std::move(img);
            a->tex.resize(a->img.frames.size());
            for (auto& t : a->tex) g::texturePtrInit(&t);
            a->uploaded.assign(a->img.frames.size(), 0);
            peLog("picture %s: %dx%d, %zu frame(s), %d ms, decoded in %.0f ms", a->file.c_str(), a->img.w, a->img.h,
                  a->img.frames.size(), a->img.totalMs, (peNow() - t0) * 1000);
        } else {
            a->error = err;
            peLog("picture %s: cannot decode (%s)", a->file.c_str(), err.c_str());
        }
        std::atomic_thread_fence(std::memory_order_release);
        a->state = ok ? 1 : 2;
    }).detach();
}

}  // namespace

std::string picturesDir() {
    const char* home = getenv("HOME");
    return std::string(home ? home : ".") + "/Documents/PaintingEditor";
}

std::string storePicture(const std::vector<uint8_t>& bytes) {
    const char* type = pictureType(bytes.data(), bytes.size());
    if (!*type) return "";
    char name[64];
    snprintf(name, sizeof name, "%016llx.%s", (unsigned long long)fnv1a(bytes.data(), bytes.size()), type);
    std::string dir = picturesDir();
    mkdir((std::string(getenv("HOME") ? getenv("HOME") : ".") + "/Documents").c_str(), 0755);
    if (mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            peLog("cannot create %s (errno %d)", dir.c_str(), errno);
            return "";
        }
    }
    std::string path = dir + "/" + name;
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && (size_t)st.st_size == bytes.size()) return name;  // already stored
    std::string tmp = path + ".tmp";
    FILE* f = fopen(tmp.c_str(), "wb");
    if (!f) {
        peLog("cannot write %s", tmp.c_str());
        return "";
    }
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp.c_str(), path.c_str()) != 0) {
        remove(tmp.c_str());
        peLog("cannot write %s", path.c_str());
        return "";
    }
    peLog("stored the picture as %s (%zu bytes)", path.c_str(), bytes.size());
    return name;
}

std::shared_ptr<Asset> getAsset(const std::string& file, const std::vector<uint8_t>* bytes) {
    std::lock_guard lk(g_assetsLock);
    auto it = g_assets.find(file);
    if (it != g_assets.end()) return it->second;
    auto a = std::make_shared<Asset>();
    a->file = file;
    g_assets[file] = a;
    decodeAsync(a, bytes ? *bytes : std::vector<uint8_t>());
    return a;
}

bool pictureStored(const std::string& file) {
    struct stat st;
    return validPictureName(file) && stat((picturesDir() + "/" + file).c_str(), &st) == 0 && st.st_size > 0;
}

bool validPictureName(const std::string& f) {
    if (f.size() != 20 || f[16] != '.') return false;
    for (int i = 0; i < 16; i++)
        if (!((f[i] >= '0' && f[i] <= '9') || (f[i] >= 'a' && f[i] <= 'f'))) return false;
    std::string ext = f.substr(17);
    return ext == "png" || ext == "jpg" || ext == "gif" || ext == "bmp";
}

void refreshAsset(const std::string& file) {
    std::lock_guard lk(g_assetsLock);
    auto it = g_assets.find(file);
    if (it != g_assets.end() && it->second->state == 2) g_assets.erase(it);  // drawn code keeps its shared_ptr
}

bool assetReady(const Asset& a) {
    bool r = a.state == 1;
    std::atomic_thread_fence(std::memory_order_acquire);
    return r;
}

const g::TexturePtr* assetTexture(Asset& a, void* group) {
    if (!assetReady(a) || !group || a.img.frames.empty()) return nullptr;
    size_t frame = 0;
    if (a.img.totalMs > 0 && a.img.frames.size() > 1) {
        long long t = (long long)(peNow() * 1000.0) % a.img.totalMs;
        while (frame + 1 < a.img.frames.size() && t >= a.img.delays[frame]) t -= a.img.delays[frame++];
    }
    if (!a.uploaded[frame]) {
        g::ResourceLocation loc;
        char name[96];
        snprintf(name, sizeof name, "paintingeditor/%s/%zu", a.file.c_str(), frame);
        loc.path = name;
        loc.fileSystem = "InAppPackage";
        g::uploadRGBA(group, loc, a.img.w, a.img.h, a.img.frames[frame].data());
        g::texturePtrDestroy(&a.tex[frame]);
        g::texturePtrLoad(&a.tex[frame], group, loc);
        a.uploaded[frame] = 1;
    }
    return &a.tex[frame];
}

void pictureUV(const Asset& a, int w, int h, int rot, bool flip, float u[4], float v[4]) {
    rot &= 3;
    // size of the picture as shown (after turning)
    float iw = (float)a.img.w, ih = (float)a.img.h;
    if (rot & 1) std::swap(iw, ih);
    // crop (in the shown picture, 0..1)
    float s0 = 0, t0 = 0, s1 = 1, t1 = 1;
    if (iw > 0 && ih > 0 && w > 0 && h > 0) {
        float ia = iw / ih, pa = (float)w / h;
        if (ia > pa) {  // wider than the painting: cut the sides
            s0 = (1 - pa / ia) / 2;
            s1 = 1 - s0;
        } else {  // taller: cut top and bottom
            t0 = (1 - ia / pa) / 2;
            t1 = 1 - t0;
        }
    }
    const float cs[4] = {s0, s1, s1, s0}, ct[4] = {t0, t0, t1, t1};
    for (int k = 0; k < 4; k++) {
        float s = flip ? 1 - cs[k] : cs[k], t = ct[k];
        // shown point (s, t) -> point of the original picture
        switch (rot) {
        case 0: u[k] = s; v[k] = t; break;
        case 1: u[k] = t; v[k] = 1 - s; break;      // turned 90 degrees clockwise
        case 2: u[k] = 1 - s; v[k] = 1 - t; break;
        default: u[k] = 1 - t; v[k] = s; break;     // 270
        }
    }
}

namespace g {
// MapRenderer::MapInstance::updateTexture: TexturePair(w, h), ImageDescription(w, h,
// R8G8B8A8_UNORM = 28), TextureData::setTextureData(desc, std::vector<uint8_t>),
// TextureGroup::uploadTexture(location, pair, true), then ~TextureData / ~Texture.
void uploadRGBA(void* group, const ResourceLocation& loc, int w, int h, const uint8_t* rgba) {
    alignas(16) unsigned char pair[512] = {};  // TexturePair: mce::Texture (56 bytes) + TextureData (40)
    alignas(16) unsigned char desc[64] = {};   // mce::ImageDescription (16 bytes)
    fn<void (*)(void*, int, int)>(0x100047AE4)(pair, w, h);
    fn<void (*)(void*, int, int, int)>(0x100009778)(desc, w, h, 28);
    std::vector<uint8_t> data(rgba, rgba + (size_t)w * h * 4);
    fn<void (*)(void*, const void*, std::vector<uint8_t>*)>(0x10000A3F4)(pair + 56, desc, &data);
    fn<void* (*)(void*, const ResourceLocation*, void*, bool)>(0x100044BB4)(group, &loc, pair, true);
    fn<void (*)(void*)>(0x10000A354)(pair + 56);  // ~TextureData
    fn<void (*)(void*)>(0x100034EE8)(pair);       // ~Texture
}
}  // namespace g
