// API for iOS MCPE (v0.16.2) Emulator mods.
//
// A mod is an arm64 dylib in <data>/mods (%LOCALAPPDATA%\iOS-MCPE-0.16.2\mods). The
// emulator loads it together with the game; functions marked
// __attribute__((constructor)) run before the game's own initialization.
// Mods can call everything the game can: libc, libc++, Foundation / UIKit (Objective-C
// runtime: objc_getClass, class_replaceMethod, ...), the game's own code by address
// (the game is loaded at emu_game_base(), 0x100000000), and the functions below.
#pragma once
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

// Writes a line to emulator.log (and the console), prefixed with "[mod]".
void emu_log(const char* message);

// Load address of the game binary (addresses from a disassembler with base 0x100000000
// can be used as they are while this returns 0x100000000).
uint64_t emu_game_base(void);

// Redirects the function at `target` to `replacement`; `*original` (optional) receives a
// trampoline that calls the replaced function. Call it from a constructor. Returns 0
// on success (fails if the first 4 instructions of the target are PC-relative; a function
// that the emulator or another mod already hooked can be hooked again - the chain is kept).
int emu_hook(uint64_t target, uint64_t replacement, uint64_t* original);

// Window title.
void emu_set_title(const char* title);

// Picture picker: a Windows file dialog, or the photo / file picker of the phone connected
// to the remote screen. emu_pick_media_begin() asks for a picture (PNG / JPEG / BMP / GIF)
// and returns at once; then call emu_pick_image_poll() (from the game's thread, e.g. once a
// frame) until it is not 0: 1 = done, *len bytes ready (copy them with emu_pick_image_take),
// -1 = cancelled. The file's bytes are returned as they are (decode them yourself).
// emu_pick_image_cancel() closes a picker that is still open (its answer is dropped).
void emu_pick_media_begin(void);
int emu_pick_image_poll(size_t* len);
void emu_pick_image_take(void* out);
void emu_pick_image_cancel(void);

#ifdef __cplusplus
}
#endif

// printf-style logging
#define EMU_LOGF(...)                                   \
    do {                                                \
        char emu_log_buf_[512];                         \
        snprintf(emu_log_buf_, sizeof emu_log_buf_, __VA_ARGS__); \
        emu_log(emu_log_buf_);                          \
    } while (0)
