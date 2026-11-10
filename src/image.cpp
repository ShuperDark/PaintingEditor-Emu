// Picture decoding with stb_image (public domain, third_party/stb). Animated GIFs are
// decoded one frame at a time (the way stbi_load_gif_from_memory does it internally) so
// that every frame can be downscaled at once instead of keeping the whole animation at
// full size in memory.
#include "image.h"

#include <string.h>

#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_ONLY_GIF
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#pragma clang diagnostic ignored "-Wsign-compare"
#endif
#include "stb_image.h"
#if defined(__clang__)
#pragma clang diagnostic pop
#endif

namespace {

constexpr int kMaxStillSide = 512;              // still pictures
constexpr int kMaxGifSide = 256;                // first try for animated GIFs
constexpr int kMinGifSide = 64;
constexpr size_t kMaxGifBytes = 64u << 20;      // all frames together (RGBA)
constexpr int kMaxGifFrames = 600;

// Box filter (area average) from sw x sh to dw x dh.
std::vector<uint8_t> resize(const uint8_t* src, int sw, int sh, int dw, int dh) {
    std::vector<uint8_t> dst((size_t)dw * dh * 4);
    if (dw == sw && dh == sh) {
        memcpy(dst.data(), src, dst.size());
        return dst;
    }
    for (int y = 0; y < dh; y++) {
        int y0 = (int)((long long)y * sh / dh), y1 = (int)((long long)(y + 1) * sh / dh);
        if (y1 <= y0) y1 = y0 + 1;
        for (int x = 0; x < dw; x++) {
            int x0 = (int)((long long)x * sw / dw), x1 = (int)((long long)(x + 1) * sw / dw);
            if (x1 <= x0) x1 = x0 + 1;
            // alpha-weighted color so that transparent pixels do not darken the edges
            unsigned long long r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = y0; yy < y1; yy++) {
                const uint8_t* p = src + ((size_t)yy * sw + x0) * 4;
                for (int xx = x0; xx < x1; xx++, p += 4) {
                    r += p[0] * p[3];
                    g += p[1] * p[3];
                    b += p[2] * p[3];
                    a += p[3];
                    n++;
                }
            }
            uint8_t* d = dst.data() + ((size_t)y * dw + x) * 4;
            if (a) {
                d[0] = (uint8_t)(r / a);
                d[1] = (uint8_t)(g / a);
                d[2] = (uint8_t)(b / a);
            } else {
                d[0] = d[1] = d[2] = 0;
            }
            d[3] = (uint8_t)(a / n);
        }
    }
    return dst;
}

void fitSize(int w, int h, int maxSide, int& ow, int& oh) {
    if (w <= maxSide && h <= maxSide) {
        ow = w;
        oh = h;
        return;
    }
    if (w >= h) {
        ow = maxSide;
        oh = (int)((long long)h * maxSide / w);
    } else {
        oh = maxSide;
        ow = (int)((long long)w * maxSide / h);
    }
    if (ow < 1) ow = 1;
    if (oh < 1) oh = 1;
}

bool decodeGif(const uint8_t* data, size_t len, DecodedImage& out, std::string& error) {
    stbi__context s;
    stbi__start_mem(&s, data, (int)len);
    if (!stbi__gif_test(&s)) {
        error = "not a GIF";
        return false;
    }
    stbi__gif gif;
    memset(&gif, 0, sizeof gif);
    std::vector<uint8_t> prev[2];  // the last two full-size frames (for "restore previous")
    int side = kMaxGifSide;
    int comp = 0;
    for (;;) {
        stbi_uc* two_back = prev[1].empty() ? nullptr : prev[1].data();
        stbi_uc* u = stbi__gif_load_next(&s, &gif, &comp, 4, two_back);
        if (u == (stbi_uc*)&s || !u) break;  // end marker / error
        size_t full = (size_t)gif.w * gif.h * 4;
        prev[1].swap(prev[0]);
        prev[0].assign(u, u + full);
        int dw, dh;
        fitSize(gif.w, gif.h, side, dw, dh);
        out.frames.push_back(resize(u, gif.w, gif.h, dw, dh));
        out.w = dw;
        out.h = dh;
        int d = gif.delay;
        if (d < 20) d = 100;  // like browsers: 0 / 10 ms frames play at 100 ms
        out.delays.push_back(d);
        out.totalMs += d;
        // too big for memory: halve every frame and continue at the smaller size
        while ((size_t)out.w * out.h * 4 * out.frames.size() > kMaxGifBytes && side > kMinGifSide) {
            side /= 2;
            int nw, nh;
            fitSize(gif.w, gif.h, side, nw, nh);
            for (auto& f : out.frames) f = resize(f.data(), out.w, out.h, nw, nh);
            out.w = nw;
            out.h = nh;
        }
        if ((int)out.frames.size() >= kMaxGifFrames) break;
    }
    STBI_FREE(gif.out);
    STBI_FREE(gif.history);
    STBI_FREE(gif.background);
    if (out.frames.empty()) {
        const char* r = stbi_failure_reason();
        error = r ? r : "GIF without frames";
        return false;
    }
    if (out.frames.size() == 1) {  // a still GIF
        out.delays.clear();
        out.totalMs = 0;
    }
    return true;
}

}  // namespace

const char* pictureType(const uint8_t* d, size_t n) {
    if (n >= 8 && !memcmp(d, "\x89PNG\r\n\x1a\n", 8)) return "png";
    if (n >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF) return "jpg";
    if (n >= 6 && (!memcmp(d, "GIF87a", 6) || !memcmp(d, "GIF89a", 6))) return "gif";
    if (n >= 2 && d[0] == 'B' && d[1] == 'M') return "bmp";
    return "";
}

bool decodePicture(const uint8_t* data, size_t len, DecodedImage& out, std::string& error) {
    out = DecodedImage();
    const char* type = pictureType(data, len);
    if (!*type) {
        error = "unsupported file type (PNG, JPEG, BMP and GIF are supported)";
        return false;
    }
    if (!strcmp(type, "gif")) return decodeGif(data, len, out, error);
    int w = 0, h = 0, comp = 0;
    stbi_uc* px = stbi_load_from_memory(data, (int)len, &w, &h, &comp, 4);
    if (!px) {
        const char* r = stbi_failure_reason();
        error = r ? r : "decode failed";
        return false;
    }
    int dw, dh;
    fitSize(w, h, kMaxStillSide, dw, dh);
    out.frames.push_back(resize(px, w, h, dw, dh));
    out.w = dw;
    out.h = dh;
    stbi_image_free(px);
    return true;
}
