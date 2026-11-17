# RE4 → original Xbox (nxdk)

Port of the GameCube decompilation to the original Xbox, one platform layer at a time. Everything is behind
`RE4_XBOX`; with it undefined the GameCube build is unchanged (the memory-map macros expand to the original
expressions). Nothing here has been built with nxdk or run on hardware yet: every layer is verified on a PC
with host tests that replace the console backend by a fake.

## Layers

| Layer | Replaces | Files (include/xbox, src/xbox) | Test |
|---|---|---|---|
| Controls | PAD | `xpad_backend.h`, `pad_xbox.cpp`, `xpad_backend_sdl.cpp` | `run_pad_test.sh` |
| Save | CARD | `xcard_backend.h`, `card_xbox.cpp`, `xcard_backend_file.cpp` (`E:\UDATA\RE4\RE4_A.CARD`) | `run_card_test.sh` |
| Disc | DVD, AR/ARQ | `xdvd.h`, `xdvd_backend.h`, `dvd_xbox.cpp`, `ar_xbox.cpp`, `xdvd_backend_file.cpp` | `run_dvd_test.sh` |
| OS | clock, calendar, arena, report/panic | `xos_backend.h`, `os_xbox.cpp`, `xos_backend_sys.cpp` | `run_os_test.sh` |
| Threads | OSContext, scheduler support | `xthread_backend.h`, `thread_xbox.cpp`, `xthread_backend_sys.cpp` | `run_thread_test.sh` |
| Memory map | the `0x80000000` window | `xmem.h`, `xmem_xbox.cpp` | `run_xmem_test.sh` |
| Math | paired singles, GQR, PSMTX/PSVEC/PSQUAT, locked cache, DC/IC cache calls | `xps.h`, `ps_xbox.cpp`, `ps_wrappers.inc` (generated), `cache_xbox.cpp` | `run_math_test.sh` |
| REL modules | OSLink / OSUnlink | `xrel.h`, `link_xbox.cpp`, `rel_registry.cpp` (generated) | `run_rel_test.sh` |

Pattern: a shim implements exactly the Dolphin calls the game uses; a small plain-C backend interface
(`x*_backend.h`) hides the platform; an nxdk backend and a host backend implement it. Tests live in `tools/xbox/`;
`tools/xbox/run_all_tests.sh` runs them all (`EXTRA="-fsanitize=address,undefined"` adds sanitizers).

Game files patched (small, guarded): `src/game/pad.cpp`, `dvd.cpp`, `snd_main.cpp`, `main_sub.cpp`, `debug.cpp`,
and ~40 files whose address checks now use the `XMEM_*` macros (`include/xbox/xmem.h`).

## Decisions worth knowing

- Math: the game's paired-single assembly (SINF/COSF, skinning CalcSk1_x/x2, PSQ_L/ST macros, GQR setup) is rewritten in
  C on `xps.h`, one helper per instruction (fused multiply-adds, saturating truncating quantized stores). SDK `PS*`
  routines forward to their `C_*` reference twins. `SQRTF` uses the exact square root (the original's `frsqrte`
  table estimate could not be reproduced); the locked cache is a 16 KB buffer.

- Disc reads and ARQ transfers complete in `XDvd_Pump()`, never inside the posting call; two spin loops in the game
  call it.
- Threads: the SDK scheduler is unchanged; an OS context is a key into a table that owns a platform thread; only one
  runs at a time.
- Memory: one block (`XMEM_SPAN`) mapped at `0x20000000` plays the GameCube's main memory. It must sit far above
  any file offset, because the game tells offsets from pointers by size (`XMEM_ISPTR`).
- REL modules are compiled into the executable; `OSLink` finds the built-in copy by module id and relocates nothing.

## Not done yet (roadmap)

1. Remaining compiler-specific asm (empty `asm("")` register hints, `.section` directives): build step.
2. Endianness of disc and save data (REL headers read through `XREL_U32`).
3. Rendering: GX → NV2A. Audio: AX/DSP. Video: Sofdec/ADX.
4. ROM font replacement (`OSInitFont` ...).
5. Build system: nxdk Makefile defining `RE4_XBOX`; per-module entry-point renames
   (`-D_prolog=<mod>_prolog` ...) and isolation of the ~600 global names several modules define
   (`tools/xbox/rel_symbols.py`); module constructors (`_ctors`).
6. nxdk/clang porting of MWCC/GCC 2.95 extensions, then hardware verification.


## Estado del enlace del juego (xbox-game-main)

`xbox/Makefile` compila los 293 `src/game/*.cpp` (todos menos `memset_2` y `yz2asm`, que son asm PowerPC) con nxdk.
El enlace falla solo por simbolos sin resolver: la lista completa esta en `docs/xbox_undefined_symbols.txt` (230).

| Familia | Simbolos | Capa que falta |
|---|---|---|
| GX* | 96 | renderizado GX -> NV2A |
| AX*, AXFX*, MIX*, SYN*, SEQ*, AXART*, AI* | ~75 | audio AX/DSP |
| mwPly*, ADX* | ~15 | video Sofdec / audio ADX (middleware) |
| VI*, OSFont*, PC*, PPCSync, DB* | ~25 | video, fuente ROM, depuracion en host (stubs) |
| `__builtin_new/delete/vec_*`, `memset_v` | 5 | alias de GCC 2.95 a operator new/delete |
| propios del juego | ~10 | `cModel::isTrans`, `ScreenReSizeI`, `TEXGet`, ... (viven en otros modulos/REL) |

Portado a C para x86: `memset_asm`, `memclr_asm`, `mtxclr_asm` (`src/xbox/native_asm.cpp`).
SIN portar: `yz2Decode_Decode` (descompresor yz2, ~500 lineas de asm); hoy llama a OSPanic.
Dialecto: `include/xbox/xcompat.h` (strstr const, `asm(...)` vacio) y `RE4_REGPIN` (pines de registro).
