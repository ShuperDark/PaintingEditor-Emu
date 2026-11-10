// PaintingEditor for the iOS MCPE (v0.16.2) Emulator.
//
// Opening the editor:
//  - PC (mouse / keyboard): right click on a painting (Player::interact on a painting).
//  - Phone (touch controls): when a painting is near, the HUD's interact button shows
//    "開く" for the nearest one (MinecraftInputHandler::_interactWithEntity picks it,
//    Entity::getInteraction gives the text, the button press opens the editor).
// The editor closes by itself (keeping the size being previewed if a picture was picked)
// when the player is hurt (Screen::closeOnPlayerHurt -> leaveScreen) or dies (before the
// death screen is pushed).
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "pe.h"

void peLog(const char* fmt, ...) {
    char buf[512] = "[PaintingEditor] ";
    size_t n = strlen(buf);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf + n, sizeof buf - n, fmt, ap);
    va_end(ap);
    emu_log(buf);
}

double peNow() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

namespace {

constexpr float kPhoneRadius = 4.0f;  // blocks: the "開く" button appears within this distance
const std::string kOpenText = "開く";

void* g_client = nullptr;         // MinecraftClient (captured from the input functions)
void* g_phonePainting = nullptr;  // the painting the phone's interact button opens
void* g_input = nullptr;          // MinecraftInputHandler (captured like the client)

using BuildActionFn = bool (*)(void*, void*);
using InteractFn = bool (*)(void*, void*);
using GetInteractionFn = bool (*)(void*, void*, void*);
using InteractWithEntityFn = void* (*)(void*);
using ClientFn = void (*)(void*);
BuildActionFn o_buildAction;
InteractFn o_playerInteract;
GetInteractionFn o_getInteraction;
InteractWithEntityFn o_interactWithEntity;
ClientFn o_interactButton, o_pushDeath;

bool h_buildAction(void* client, void* intention) {
    g_client = client;
    return o_buildAction(client, intention);
}

// right click (PC) / tap (phone) on a painting
bool h_playerInteract(void* player, void* entity) {
    if (g::isPainting(entity) && g_client && player == g::localPlayer(g_client)) {
        if (!editorOpen()) openEditor(g_client, entity);
        return true;
    }
    return o_playerInteract(player, entity);
}

// text of the touch HUD's interact button
bool h_getInteraction(void* entity, void* player, void* interaction) {
    if (g::isPainting(entity) && !g::interactionShouldCapture(interaction)) {
        g::setInteractText(interaction, kOpenText);
        return true;
    }
    return o_getInteraction(entity, player, interaction);
}

// the entity the touch HUD's interact button is for: the nearest painting (unless the
// player is aiming at another entity that has its own button, like a boat or a dog)
void* h_interactWithEntity(void* input) {
    void* e = o_interactWithEntity(input);
    void* client = *reinterpret_cast<void**>((char*)input + g::kInputClient);
    if (client) g_client = client;
    g_input = input;
    g_phonePainting = nullptr;
    if (!client || g::currentInputMode(input) != g::kInputModeTouch || editorOpen()) return e;
    if (e && !g::isPainting(e)) return e;
    void* p = nearestPainting(g::localPlayer(client), kPhoneRadius);
    g_phonePainting = p;
    return p ? p : e;
}

void h_interactButton(void* client) {
    g_client = client;
    void* p = g_phonePainting;
    if (p && paintingAlive(p)) {
        if (!editorOpen()) openEditor(client, p);
        return;
    }
    o_interactButton(client);
}

void h_pushDeath(void* chooser) {
    if (editorOpen()) forceCloseEditor("player died");
    o_pushDeath(chooser);
}

}  // namespace

bool touchMode() { return g_input && g::currentInputMode(g_input) == g::kInputModeTouch; }
void* currentClient() { return g_client; }

namespace {

template <class F>
void hook(uint64_t target, void* repl, F& orig, const char* name) {
    uint64_t o = 0;
    if (emu_hook(g::addr(target), (uint64_t)repl, &o) == 0) orig = reinterpret_cast<F>(o);
    else peLog("cannot hook %s (0x%llx)", name, (unsigned long long)target);
}

}  // namespace

__attribute__((constructor)) static void paintingEditorInit() {
    if (emu_game_base() != 0x100000000ull) peLog("game at 0x%llx", (unsigned long long)emu_game_base());
    installPaintingHooks();
    installNetHooks();
    hook(g::kClientBuildAction, (void*)h_buildAction, o_buildAction, "MinecraftClient::handleBuildAction");
    hook(g::kPlayerInteract, (void*)h_playerInteract, o_playerInteract, "Player::interact");
    hook(g::kEntityGetInteraction, (void*)h_getInteraction, o_getInteraction, "Entity::getInteraction");
    hook(g::kInputInteractWithEntity, (void*)h_interactWithEntity, o_interactWithEntity, "MinecraftInputHandler::_interactWithEntity");
    hook(g::kClientInteractButton, (void*)h_interactButton, o_interactButton, "MinecraftClient::handleInteractButtonPress");
    hook(g::kPushDeathScreen, (void*)h_pushDeath, o_pushDeath, "ScreenChooser::pushDeathScreen");
    peLog("loaded (pictures in %s)", picturesDir().c_str());
}
