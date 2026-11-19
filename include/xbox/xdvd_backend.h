// xbox/xdvd_backend.h: the only thing the Xbox disc layer needs from the platform.
//
// src/xbox/dvd_xbox.cpp implements the Dolphin DVD calls the game makes (DVDConvertPathToEntrynum,
// DVDReadAsyncPrio, DVDCancelAsync, ...) as reads from a folder of plain files that holds the game's disc
// contents. A backend only has to answer two questions about that folder: "does this file exist, and how big
// is it" and "give me these bytes". It knows nothing about the game, entry numbers or queues. Two backends:
//   src/xbox/xdvd_backend_file.cpp   stdio on a folder (nxdk: the folder next to the .xbe)
//   tools/xbox/dvd_test.cpp          in-memory fake with fault injection, used by the host unit test
//
// Paths arrive already normalised by the layer: relative to the disc root, '/' as separator, no leading
// slash, no "." or ".." parts, shorter than XDVD_PATH_MAX. Case is whatever the game used; the disc was
// case-insensitive, so a backend should match without regard to case (the Xbox file systems do).
// Plain C types only, same reason as xpad_backend.h.
#ifndef XBOX_XDVD_BACKEND_H
#define XBOX_XDVD_BACKEND_H

#ifdef __cplusplus
extern "C" {
#endif

#define XDVD_PATH_MAX 96

// One-time start. Returns 1 on success, 0 if there is no disc folder at all (every lookup then fails,
// which the game already handles as "file not on disc").
int XDvdBackend_Init(void);

// Looks `path` up. Returns 1 and sets *size (bytes, 0 for a directory) and *is_dir if it exists, else 0.
// The empty path is the disc root and must answer "directory".
int XDvdBackend_Stat(const char* path, unsigned int* size, int* is_dir);

// Reads up to `len` bytes of the file at `off`. Sets *got to the number of bytes delivered, which is less
// than `len` only at the end of the file. Returns 1 on success, 0 on an I/O error (the layer treats that
// like the drive failing: a fatal error).
int XDvdBackend_Read(const char* path, unsigned int off, void* buf, unsigned int len, unsigned int* got);

// Only the stdio backend has this: where the disc folder is (default: the build's XDVD_ROOT). Mainly for
// tests. Closes any open files.
void XDvdBackend_SetRoot(const char* dir);

// The game swaps discs (0 / 1) by itself. A backend that can, serves the files of that disc: the stdio one
// uses "<root>/disc1/" or "<root>/disc2/" if that folder exists and the root itself otherwise.
void XDvdBackend_SetDisc(int disc);

#ifdef __cplusplus
}
#endif

#endif
