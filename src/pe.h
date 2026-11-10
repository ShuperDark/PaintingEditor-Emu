// PaintingEditor: shared declarations of the mod's modules.
//
//   main.cpp      constructor, hooks (interaction, phone button, death screen)
//   paintings.cpp edited paintings (sizes, save data, tracking) and their rendering in the world
//   assets.cpp    picked pictures: files on disk, decoding, textures (animated GIF frames)
//   screen.cpp    the editor screen (picker, preview, size buttons)
//   net.cpp       multiplayer: edits and pictures sent between the games (RakNet messages)
//   image.cpp     PNG / JPEG / BMP / GIF decoding (stb_image)
#pragma once
#include <stdint.h>

#include <memory>
#include <string>
#include <vector>

#include "game.h"
#include "image.h"

void peLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double peNow();  // seconds, monotonic

// ---------------------------------------------------------------- sizes
struct PaintingSize { int w, h; };  // blocks (width x height)
extern const PaintingSize kSizes[];
extern const int kSizeCount;
g::Motive* motiveForSize(int index);
int sizeIndexOf(int w, int h);  // -1 if not one of kSizes

// ---------------------------------------------------------------- pictures (assets.cpp)
struct Asset {
    std::string file;        // file name in the pictures folder ("<hash>.<ext>")
    int state = 0;           // 0 decoding, 1 ready, 2 failed (written by the decoder thread)
    std::string error;
    DecodedImage img;
    std::vector<g::TexturePtr> tex;  // one texture per frame, uploaded when first drawn
    std::vector<uint8_t> uploaded;
};
std::string picturesDir();
// Stores picked bytes in the pictures folder (named by content hash). Returns the file
// name, or "" on failure.
std::string storePicture(const std::vector<uint8_t>& bytes);
// The picture `file` (decoding starts in the background on first use).
std::shared_ptr<Asset> getAsset(const std::string& file, const std::vector<uint8_t>* bytes = nullptr);
bool assetReady(const Asset& a);
// Texture of the frame to show now (uploads it on first use; render thread only). Null
// while the picture is still decoding or failed.
const g::TexturePtr* assetTexture(Asset& a, void* textureGroup);
// Texture coordinates of the picture at the corners of a w x h painting's front, as seen from
// the front: [0] top left, [1] top right, [2] bottom right, [3] bottom left. The picture is
// turned clockwise by rot * 90 degrees, then mirrored left-right if `flip`, then cropped
// around its center to the painting's aspect ratio.
void pictureUV(const Asset& a, int w, int h, int rot, bool flip, float u[4], float v[4]);
bool pictureStored(const std::string& file);   // the file is in the pictures folder
bool validPictureName(const std::string& file); // "<16 hex digits>.<png|jpg|gif|bmp>"
// Forgets a picture that could not be read (it has just been received): the next use
// decodes it again.
void refreshAsset(const std::string& file);

// ---------------------------------------------------------------- paintings (paintings.cpp)
struct Edit {
    std::string file;
    int w = 1, h = 1;
    int rot = 0;        // clockwise quarter turns of the picture (0..3)
    bool flip = false;  // mirrored left-right (after turning)
    bool operator==(const Edit& o) const { return file == o.file && w == o.w && h == o.h && rot == o.rot && flip == o.flip; }
};
// Where an edit comes from: the editor of this game, the world's save data, or another game
enum class EditFrom { Local, Save, Network };
bool getEdit(void* painting, Edit& out);
// Also changes the painting's size, and tells the other games of a multiplayer game.
void setEdit(void* painting, const Edit& e, EditFrom from = EditFrom::Local);
std::vector<std::pair<void*, Edit>> allEdits();
int currentSizeIndex(void* painting);
bool paintingAlive(void* painting);
// The nearest painting within `radius` blocks of the player (same level and dimension).
void* nearestPainting(void* player, float radius);
void installPaintingHooks();
extern void* g_textureGroup;  // the game's TextureGroup (from the painting renderer / client)

// ---------------------------------------------------------------- screen (screen.cpp)
void openEditor(void* client, void* painting);
bool editorOpen();
// Closes the editor now: keeps the size being previewed if a picture was picked.
void forceCloseEditor(const char* why);
bool touchMode();  // the game is in touch input mode (phone connected)
void editorPaintingRemoved(void* painting);
void* currentClient();  // MinecraftClient (null until the first input)

// ---------------------------------------------------------------- multiplayer (net.cpp)
void installNetHooks();
void netEditApplied(void* painting, const Edit& e, EditFrom from);  // from setEdit
void netPaintingTick(void* painting);  // every tick of every painting (applies edits that came from the network)
