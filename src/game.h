// Addresses and data layouts of MCPE 0.16.2 (iOS, arm64) used by PaintingEditor.
//
// The iOS binary has no symbols: every function here was located by comparing it with
// the Android build (libminecraftpe.so, which has symbols) - the Android name is given
// for each one. Addresses are for the game loaded at 0x100000000 (g::addr() adds the
// slide in case the emulator ever loads it elsewhere).
#pragma once
#include <stdint.h>
#include <string>

#include "emu_mod.h"

namespace g {

inline uint64_t slide() {
    static uint64_t s = emu_game_base() - 0x100000000ull;
    return s;
}
inline uint64_t addr(uint64_t a) { return a + slide(); }
template <class F>
inline F fn(uint64_t a) { return reinterpret_cast<F>(addr(a)); }
template <class T>
inline T* ptr(uint64_t a) { return reinterpret_cast<T*>(addr(a)); }

// ---------------------------------------------------------------- basic types
struct Vec3 { float x, y, z; };
struct Color { float r, g, b, a; };
// ResourceLocation { std::string path; std::string fileSystem; } (libc++ = the game's ABI)
struct ResourceLocation {
    std::string path;
    std::string fileSystem;
};
// mce::TexturePtr (64 bytes, opaque)
struct alignas(8) TexturePtr { unsigned char raw[64]; };
// Motive (painting art): name, size in pixels (16 per block), texture offset, public flag
struct Motive {
    std::string name;
    int w, h, uo, vo;
    bool isPublic;
};
static_assert(sizeof(Motive) == 48, "Motive layout");

// ---------------------------------------------------------------- hooks (targets)
constexpr uint64_t kPaintingRendererRender = 0x1003D65E0;   // PaintingRenderer::render(Entity&, Vec3 const&, float, float)
constexpr uint64_t kPaintingAddSaveData = 0x1006AAA9C;      // Painting::addAdditionalSaveData(CompoundTag&)
constexpr uint64_t kPaintingReadSaveData = 0x1006AAB44;     // Painting::readAdditionalSaveData(CompoundTag const&)
constexpr uint64_t kEntityDtor = 0x100655878;               // Entity::~Entity() (every entity, Painting's too)
constexpr uint64_t kHangingNormalTick = 0x10069C010;        // HangingEntity::normalTick()
constexpr uint64_t kHangingWouldSurvive = 0x10069C114;      // HangingEntity::wouldSurvive(BlockSource&)
constexpr uint64_t kPlayerInteract = 0x10071244C;           // Player::interact(Entity&)
constexpr uint64_t kEntityGetInteraction = 0x1006639B4;     // Entity::getInteraction(Player&, EntityInteraction&)
constexpr uint64_t kInputInteractWithEntity = 0x1002DD9C8;  // MinecraftInputHandler::_interactWithEntity()
constexpr uint64_t kClientInteractButton = 0x10007A958;     // MinecraftClient::handleInteractButtonPress()
constexpr uint64_t kClientBuildAction = 0x100079F6C;        // MinecraftClient::handleBuildAction(BuildActionIntention const&)
constexpr uint64_t kPushDeathScreen = 0x100141070;          // ScreenChooser::pushDeathScreen()

// ---------------------------------------------------------------- entities
constexpr uint64_t kPaintingVtable = 0x1011223D8;  // vtable for Painting (address point)
constexpr int kEntityDimension = 56;               // int   Entity::mDimensionId
constexpr int kEntityLevel = 64;                   // Level* Entity::mLevel
constexpr int kEntityPos = 96;                     // Vec3  Entity::mPos (Entity::getPos)
constexpr int kHangingDir = 3492;                  // int   HangingEntity::mDir
constexpr int kHangingBlockPos = 3500;             // BlockPos HangingEntity::mBlockPos
constexpr int kPaintingMotive = 3512;              // Motive const* Painting::mMotive
constexpr int kVtSetDir = 149;                     // HangingEntity::setDir(int) (vtable slot)
constexpr int kVtIsAlive = 67;                     // Entity::isAlive() (vtable slot)

inline bool isPainting(const void* e) { return e && *reinterpret_cast<const uint64_t*>(e) == addr(kPaintingVtable); }
inline const Vec3& entityPos(const void* e) { return *reinterpret_cast<const Vec3*>((const char*)e + kEntityPos); }
inline int entityDim(const void* e) { return *reinterpret_cast<const int*>((const char*)e + kEntityDimension); }
inline void* entityLevel(const void* e) { return *reinterpret_cast<void* const*>((const char*)e + kEntityLevel); }
inline Motive*& paintingMotive(void* e) { return *reinterpret_cast<Motive**>((char*)e + kPaintingMotive); }
inline int hangingDir(const void* e) { return *reinterpret_cast<const int*>((const char*)e + kHangingDir); }
template <class F>
inline F vfn(const void* obj, int slot) { return reinterpret_cast<F>((*reinterpret_cast<uint64_t* const*>(obj))[slot]); }
inline void setDir(void* e, int dir) { vfn<void (*)(void*, int)>(e, kVtSetDir)(e, dir); }
inline bool isAlive(void* e) { return vfn<bool (*)(void*)>(e, kVtIsAlive)(e); }

// Entity::getUniqueID() -> EntityUniqueID const& (int64 at +0x58; made on first use). The
// server sends it in AddPaintingPacket and the client's copy of the entity gets the same id
// (MultiPlayerLevel::putEntity), so it names a painting on every machine of a game.
inline int64_t entityUniqueId(void* e) { return *fn<const int64_t* (*)(void*)>(0x100657960)(e); }
// Level::isClientSide() (0x1007AA610): true for the level of a player who joined someone's game
inline bool levelIsClientSide(const void* level) { return *((const uint8_t*)level + 0x16E0) != 0; }

// Motive::getMotiveByName(std::string const&) (unknown names give Kebab)
constexpr uint64_t kMotiveByName = 0x1006A96A0;
inline Motive* motiveByName(const std::string& n) { return fn<Motive* (*)(const std::string*)>(kMotiveByName)(&n); }

// CompoundTag
inline void tagPutString(void* tag, const std::string& key, std::string value) {
    fn<void (*)(void*, const std::string*, std::string*)>(0x10049AC40)(tag, &key, &value);  // CompoundTag::putString
}
inline const std::string& tagGetString(const void* tag, const std::string& key) {
    return *fn<const std::string* (*)(const void*, const std::string*)>(0x10049B1F8)(tag, &key);  // CompoundTag::getString
}

// EntityInteraction::setInteractText(std::string const&) / shouldCapture()
inline void setInteractText(void* ei, const std::string& s) { fn<void (*)(void*, const std::string*)>(0x100690034)(ei, &s); }
inline bool interactionShouldCapture(void* ei) { return fn<bool (*)(void*)>(0x10069019C)(ei); }

// ---------------------------------------------------------------- client / input / screens
constexpr int kClientLocalPlayer = 0x210;  // MinecraftClient::getLocalPlayer() = *(client + 0x210)
constexpr int kInputClient = 56;           // MinecraftInputHandler -> MinecraftClient*
constexpr int kScreenClient = 32;          // Screen::mClient
constexpr int kScreenWidth = 24, kScreenHeight = 28;
constexpr int kInputModeTouch = 2;         // InputMode: 1 mouse, 2 touch, 3 gamepad

inline void* localPlayer(void* client) { return client ? *reinterpret_cast<void**>((char*)client + kClientLocalPlayer) : nullptr; }
inline int currentInputMode(void* input) { return fn<int (*)(void*)>(0x1002DD564)(input); }  // MinecraftInputHandler::getCurrentInputMode
inline void* screenChooser(void* client) { return fn<void* (*)(void*)>(0x100085048)(client); }  // MinecraftClient::getScreenChooser
inline void* clientFont(void* client) { return fn<void* (*)(void*)>(0x1000855A8)(client); }     // MinecraftClient::getFont
// MinecraftClient::playUI(std::string const& sound, float volume, float pitch): the sound of
// the game's buttons is playUI("random.click", 1, 1)
inline void playUI(void* client, const std::string& sound, float volume = 1.0f, float pitch = 1.0f) {
    fn<void (*)(void*, const std::string*, float, float)>(0x100083828)(client, &sound, volume, pitch);
}
inline void* clientTextures(void* client) { return fn<void* (*)(void*)>(0x100085598)(client); } // MinecraftClient::getTextures
// ScreenChooser::_pushScreen(std::shared_ptr<AbstractScreen>, bool) / schedulePopScreen(int)
inline void pushScreen(void* chooser, void* sharedPtr) { fn<void (*)(void*, void*, bool)>(0x1001407F4)(chooser, sharedPtr, false); }
inline void schedulePopScreen(void* chooser) { fn<void (*)(void*, int)>(0x1001406FC)(chooser, 1); }

constexpr uint64_t kScreenCtor = 0x10013D710;    // Screen::Screen(MinecraftClient&)
constexpr uint64_t kScreenVtable = 0x1010F5148;  // vtable for Screen (address point, 91 slots)
constexpr int kScreenVtSlots = 91;
// Screen vtable slots (same order as Android's)
enum ScreenSlot {
    kSlotDtor = 0, kSlotDeletingDtor = 1, kSlotLeaveScreen = 12, kSlotHandleBackEvent = 27,
    kSlotRenderGameBehind = 37, kSlotAbsorbsInput = 38, kSlotCloseOnPlayerHurt = 39, kSlotIsPauseScreen = 47,
    kSlotGetScreenName = 57, kSlotRender = 62, kSlotInit = 63, kSlotSetupPositions = 64, kSlotTick = 67,
    kSlotPointerPressed = 87, kSlotPointerReleased = 88,
};
constexpr int kScreenRenderer = 16;  // Screen's ScreenRenderer (GuiComponent) base

// ---------------------------------------------------------------- rendering
constexpr uint64_t kTessellator = 0x10126B860;  // Tessellator::instance
inline void* tess() { return ptr<void>(kTessellator); }
inline void tessBegin(int reserve) { fn<void (*)(void*, int)>(0x10041A7C4)(tess(), reserve); }  // quads
inline void tessVertexUV(float x, float y, float z, float u, float v) {
    fn<void (*)(void*, float, float, float, float, float)>(0x10041AA38)(tess(), x, y, z, u, v);
}
inline void tessColor(const Color& c) { fn<void (*)(void*, const Color*)>(0x10041A8D8)(tess(), &c); }  // per-vertex color
inline void tessNormal(const Vec3& n) { fn<void (*)(void*, const Vec3*)>(0x10041B31C)(tess(), &n); }
// Tessellator::draw(MaterialPtr const&, TexturePtr const&) - immediate draw of what was built
inline void tessDraw(const void* material, const TexturePtr* tex) {
    fn<void (*)(void*, const void*, const TexturePtr*)>(0x10041B844)(tess(), material, tex);
}

// ScreenRenderer::mScreenMaterials (40 bytes each): 0 ui_textured (blit), 1 ui_fill_color,
// 2 ui_fill_gradient, 3 ui_texture_and_color (texture x vertex color)
inline const void* screenMaterial(int i) { return ptr<char>(0x101267908) + 40 * i; }
// ShaderColor::setColor(currentShaderColor, Color const&)
inline void setShaderColor(const Color& c) { fn<void (*)(void*, const Color*)>(0x1000283E4)(ptr<void>(0x1012675A4), &c); }
// ScreenRenderer::fill(int, int, int, int, Color const&)
inline void fillRect(void* sr, int x0, int y0, int x1, int y1, const Color& c) {
    fn<void (*)(void*, int, int, int, int, const Color*)>(0x100110010)(sr, x0, y0, x1, y1, &c);
}
// ScreenRenderer::drawCenteredString(Font&, std::string const&, int, int, Color const&)
inline void drawCenteredString(void* sr, void* font, const std::string& s, int x, int y, const Color& c) {
    fn<void (*)(void*, void*, const std::string*, int, int, const Color*)>(0x10010FA14)(sr, font, &s, x, y, &c);
}

// MatrixStack::World, MatrixStack::push() -> MatrixStack::Ref { stack, matrix }. Ref has a
// destructor, so push() returns it through x8; the empty destructor here makes the
// compiler do the same. The game's ~Ref (which pops the matrix) is called by popMatrix().
struct MatrixRef {
    void* stack;
    float* m;
    MatrixRef() = default;
    MatrixRef(const MatrixRef&) = delete;
    ~MatrixRef() {}
};
inline MatrixRef pushWorldMatrix() { return fn<MatrixRef (*)(void*)>(0x10003B950)(ptr<void>(0x101267688)); }
inline void popMatrix(MatrixRef* r) { fn<void (*)(MatrixRef*)>(0x10003BC1C)(r); }  // MatrixStack::Ref::~Ref
inline void markDirty(MatrixRef* r) { reinterpret_cast<uint8_t*>(r->stack)[24] = 1; }

// EntityShaderManager (base of EntityRenderer)
inline void setupShaderParameters(void* renderer, void* entity, float alpha) {
    // _setupShaderParameters(Entity&, Color const& overlay, float alpha, bool)
    fn<void (*)(void*, void*, const Color*, float, bool)>(0x100412368)(renderer, entity, ptr<Color>(0x101265450), alpha, false);
}
inline const void* entityMaterial(void* renderer) { return fn<const void* (*)(void*)>(0x100412210)(renderer); }  // getEntityMaterial
constexpr int kRendererAlphaTestMaterial = 136;  // EntityRenderer: "entity_alphatest" MaterialPtr
constexpr int kRendererTextures = 280;           // EntityRenderer: mce::TextureGroup&
constexpr int kPaintingRendererTexture = 464;    // PaintingRenderer: TexturePtr "textures/painting/kz"

// ---------------------------------------------------------------- textures
inline void texturePtrInit(TexturePtr* t) { fn<void (*)(TexturePtr*)>(0x100048140)(t); }          // TexturePtr()
inline void texturePtrDestroy(TexturePtr* t) { fn<void (*)(TexturePtr*)>(0x10004816C)(t); }       // ~TexturePtr()
// TexturePtr(TextureGroup&, ResourceLocation const&) - loads the texture if needed. (This is
// what TextureGroup::getTexture(ResourceLocation const&) at 0x100044930 does; that one
// returns the TexturePtr through x8, so it is not called directly.)
inline void texturePtrLoad(TexturePtr* out, void* group, const ResourceLocation& loc) {
    fn<void (*)(TexturePtr*, void*, const ResourceLocation*)>(0x1000482AC)(out, group, &loc);
}
// Upload RGBA pixels as the texture `loc` of `group` (replaces an existing one), the way
// MapRenderer::MapInstance::updateTexture does it.
void uploadRGBA(void* group, const ResourceLocation& loc, int w, int h, const uint8_t* rgba);

// ---------------------------------------------------------------- network (RakNet 4)
// RakNet::RakPeer (vtable 0x1011933B0, same slots as Android's): RakNetInstance::runEvents
// (0x1004B2AC4) calls Receive() until it returns null and hands each packet whose first
// byte it does not know (anything but 0x10-0x19 and 0xFE) straight to DeallocatePacket, so
// a game without this mod ignores our messages.
constexpr uint64_t kRakPeerReceive = 0x100D92770;     // RakPeer::Receive() (slot 24)
constexpr uint64_t kRakPeerDeallocate = 0x100D92D00;  // RakPeer::DeallocatePacket(Packet*) (slot 25)
constexpr uint64_t kRakPeerSend = 0x100D91D80;        // RakPeer::Send(char const*, int, ...) (slot 20)
// RakNet::Packet: SystemAddress (136 bytes) at 0, RakNetGUID {uint64 g; uint16 index} at
// 0x88, length (bytes) at 0x98, data at 0xA0
constexpr int kPacketGuid = 0x88, kPacketLength = 0x98, kPacketData = 0xA0, kSystemAddressSize = 0x88;
// RakNet::AddressOrGUID { RakNetGUID (16 bytes); SystemAddress (136) }, passed by value
// (that is, as a pointer to a copy)
struct alignas(8) AddressOrGUID { unsigned char raw[16 + kSystemAddressSize]; };
enum { kLowPriority = 3, kReliableOrdered = 3 };
inline void rakDeallocate(void* peer, void* packet) { fn<void (*)(void*, void*)>(kRakPeerDeallocate)(peer, packet); }
inline uint32_t rakSend(void* peer, const void* data, int len, int priority, int reliability, char channel,
                        const AddressOrGUID& to) {
    AddressOrGUID copy = to;
    return fn<uint32_t (*)(void*, const void*, int, int, int, char, AddressOrGUID*, bool, uint32_t)>(kRakPeerSend)(
        peer, data, len, priority, reliability, channel, &copy, false, 0);
}

}  // namespace g
