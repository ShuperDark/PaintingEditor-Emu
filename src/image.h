// Decoding of the pictures the player picks (PNG / JPEG / BMP / GIF, animated GIFs too).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string>
#include <vector>

struct DecodedImage {
    int w = 0, h = 0;                         // size of every frame (after downscaling)
    std::vector<std::vector<uint8_t>> frames; // RGBA, w * h * 4 each
    std::vector<int> delays;                  // milliseconds per frame (animated GIF)
    int totalMs = 0;                          // sum of delays (0 = still picture)
};

// File type from the first bytes: "png", "jpg", "gif", "bmp" or "" (not supported).
const char* pictureType(const uint8_t* data, size_t len);

// Decodes a picture, downscaling it (long side at most 512 pixels; animated GIFs
// get a smaller size so that all frames fit in memory). Returns false with `error` set
// when the file cannot be decoded.
bool decodePicture(const uint8_t* data, size_t len, DecodedImage& out, std::string& error);
