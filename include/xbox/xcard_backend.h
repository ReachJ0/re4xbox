// xbox/xcard_backend.h: the only thing the Xbox save layer needs from the platform.
//
// src/xbox/card_xbox.cpp implements the Dolphin CARD calls the game makes (CARDMountAsync, CARDOpen,
// CARDWriteAsync, ...) as a memory card emulated inside one image file per slot. A backend only has to
// provide that file: a byte-addressable, growable blob per slot. It knows nothing about the game or the
// card format. Two backends exist:
//   src/xbox/xcard_backend_file.cpp   stdio files on the console's hard disk (nxdk)
//   tools/xbox/card_test.cpp          in-memory fake used by the host unit test
//
// Slot numbering is the game's: 0 = slot A, 1 = slot B. Plain C types only, same reason as xpad_backend.h.
#ifndef XBOX_XCARD_BACKEND_H
#define XBOX_XCARD_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#define XCARD_SLOTS 2

// One-time start. Returns 1 on success, 0 if there is no storage at all (every slot then reports
// "no card", which the game already handles).
int XCardBackend_Init(void);

// 1 if `slot` has storage behind it right now. Checked before every operation, so a backend can model a
// removable device by returning 0 while it is out. Slot A is the hard disk; slot B has no storage in the
// default backend, so the game sees an empty second slot.
int XCardBackend_Present(int slot);

// Current size in bytes of the slot's image. Returns 1 and sets *size (0 for a brand-new, empty image),
// or 0 on an I/O error.
int XCardBackend_Size(int slot, unsigned int* size);

// Reads `len` bytes at `off`. Returns 1 on success, 0 on error or if [off, off+len) is not inside the
// image.
int XCardBackend_Read(int slot, unsigned int off, void* buf, unsigned int len);

// Writes `len` bytes at `off`, growing the image if needed (any gap is zero-filled). Returns 1 / 0.
int XCardBackend_Write(int slot, unsigned int off, const void* buf, unsigned int len);

// Cuts the image to `size` bytes (never grows it). Returns 1 / 0.
int XCardBackend_Truncate(int slot, unsigned int size);

// Makes everything written so far durable. The layer calls this after every operation that changes the
// image, because the player may turn the console off right after the save screen. Returns 1 / 0.
int XCardBackend_Flush(int slot);

// Current time as seconds since 2000-01-01 00:00:00, the epoch a GameCube card stores in a file's date.
unsigned int XCardBackend_Seconds2000(void);

#ifdef __cplusplus
}
#endif

#endif
