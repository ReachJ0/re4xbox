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

### Simbolos baratos (hecho)
- 13 simbolos con el mismo nombre y distinto tipo/alias se resuelven con `xbox/alt.inc` (`-alternatename`), generado por
  `tools/xbox/gen_alternatenames.py` (GCC 2.95 no ponia el tipo en el nombre de las variables; los alias `asm("etiqueta")`
  se leen del codigo). Hay que regenerarlo cuando cambie la lista de unidades.
- `inline` en funciones definidas en un .cpp y usadas desde otros: `RE4_XINLINE` (cModel::isTrans, cPlayer::subCharLiveCheck).
- `src/xbox/dev_stubs.cpp`: servidor de ficheros PC, DBIsDebuggerPresent, PPCSync, `__builtin_new/delete`, `memset_v`, fuente ROM
  del IPL (OSGetFontEncode/OSInitFont/OSGetFontTexture: sin fuente hasta que haya una de reemplazo; OSGetFontTexture devuelve imagen nula).
- `src/xbox/chp_xbox.cpp`: DSList y Strcpy/Strcmp/Strlen de CharPipeline (11 comprobaciones en host).
- `DVDReadPrio` (lectura bloqueante) en `dvd_xbox.cpp` (3 comprobaciones nuevas).
- Quedan 197 simbolos: GX (96 + `__GXSetIndirectMask` + `GXWGFifo`), audio (AX/AXFX/MIX/SYN/SEQ/AI), VI, Sofdec/ADX.

### Capa GX (software GP)
Enfoque: no se reescriben las funciones GX. El SDK real (`src/lib/GX*.c`) compila y corre tal cual sobre un procesador de
comandos GP por software (`src/xbox/gp_xbox.cpp`): decodifica el flujo de bytes (registros CP/XF/BP, listas de display,
vertices directos e indexados con todos los formatos VAT) y entrega primitivas a un backend (`include/xbox/xgp.h`).
- Escrituras al FIFO: `GXFIFO_WRITE`/`GXFIFOP_WRITE` (`include/xbox/xgxfifo.h`); en GameCube expanden a la sentencia original.
- Rafagas de 32 bytes del write-gather y registros PI/CP emulados; las listas de display se graban en memoria.
- Direcciones: `GX_PHYS`/`GX_PHYS5` (offset en el bloque de memoria); `OSPhysicalTo*` redirigido en os.h.
- Interrupciones PE (token/finish) en cola, entregadas tras `OSRestoreInterrupts`/en `GXWaitDrawDone` (`gx_xbox.cpp`).
- Intrinsecos MWCC (`__rlwimi`, `__rlwinm`) en `include/xbox/xppc.h`; asm de GXTransform/GXLight en C.
- Test: `tools/xbox/run_gx_test.sh` (109 comprobaciones sobre el SDK real, UBSan limpio). Limitacion: en host de 64 bits los
  `GXTexObj` con punteros no caben (ASan da falso positivo); en Xbox son de 32 bits.
- Estado: 0 simbolos GX sin resolver en el enlace de nxdk. FALTA el backend de dibujo (NV2A): transformacion/iluminacion,
  texturas GC -> RGBA, TEV -> combinadores, copia de EFB, y la capa VI.

## VI y audio (stubs)

- `src/xbox/vi_xbox.cpp` + `xvi_backend_sys.cpp`: VI con reloj de retrace virtual (ver `tools/xbox/run_vi_test.sh`). La presentación (`XViBackend_Present`) está vacía hasta que exista el renderer NV2A.
- `src/xbox/audio_stubs.cpp`: AX/AXFX/AXART/MIX/SYN/SEQ/AI, ADX y Sofdec (`mw*`) silenciosos. `AXAcquireVoice` devuelve NULL y `mwPlyCreateSofdec` devuelve NULL: sin sonido ni vídeos. Sin verificar si el juego tolera NULL en todos los usos.
- Estado: `default.xbe` enlaza con 0 símbolos sin resolver. Nada probado en consola ni emulador.

## Pantalla (pbkit)

- `xvi_backend_sys.cpp` inicia pbkit en el primer retrace y en cada uno limpia el back buffer y hace el swap: negro mientras el juego tiene la pantalla en blanco, azul oscuro de relleno cuando la activa. No dibuja nada del juego: faltan GXCopyDisp (EFB a XFB) y el dibujo NV2A.
- Traza en pantalla (`xbox/xtrace.h`): el texto de OSReport/OSPanic y las líneas de progreso de `main.cpp` se dibujan sobre cada frame; OSPanic se queda en pantalla (HALTED) en vez de reiniciar. Las excepciones de CPU no se capturan (nxdk no ofrece SetUnhandledExceptionFilter): un fallo así reinicia la máquina.
- `main.cpp` solo arranca el juego con `make -C xbox RUN_GAME=1`; por defecto sigue siendo la prueba de humo. Ninguna de las dos se ha probado en consola ni en emulador.

## Montar nxdk y compilar (Linux, macOS o WSL2)

- `tools/xbox/setup_nxdk.sh [dir]` clona nxdk (commit fijado fafbe41), construye sus herramientas y compila `xbox/bin/default.xbe`.
- `.vscode/tasks.json`: tareas de compilación (prueba de humo o `RUN_GAME=1`) y de tests de host. Ctrl+Shift+B compila. En Windows, abrir la carpeta con la extensión WSL.
- `.vscode/c_cpp_properties.json` asume nxdk en `~/nxdk`. Sin probar en una máquina limpia.

## Dónde está parado el juego (monitor de xemu + mapa de símbolos)

Si el juego se cuelga sin mensaje: en xemu, menú Debug → Monitor; escribir `stop`, `info registers` (anotar EIP) y `x /64xw $esp` (la pila). Después, con el mapa que genera el enlace (`xbox/main.map`, hecho con el mismo build que se ejecutó):
`python tools/xbox/addr2sym.py xbox/main.map 0xEIP` — o `--stack < volcado.txt` para recorrer la pila. Las direcciones de pila que caen en el código son llamadas pendientes: dan la cadena de funciones hasta el cuelgue (con falsos positivos: son valores sueltos de la pila).

- `make -C xbox RUN_GAME=1 DEBUGSCREEN=1`: sin pbkit; la traza se escribe con `debugPrint` (pantalla de texto de nxdk, la CPU dibuja, sin GPU). Sirve cuando el cambio de buffer de pbkit se bloquea y congela la pantalla (se vio en xemu hacia el frame 31). Al cambiar de modo borrar `xbox/main.obj` y `src/xbox/xvi_backend_sys.obj` (make no vigila las banderas).

- Caídas: el juego corre en ring 0 y una excepción sin manejar acaba en `KeBugCheckEx`, que congela la máquina (`cli; hlt`; en el monitor de xemu se ve `EFL=00000046` sin el bit IF y `HLT=1`; no es una espera, es una caída). `XCrash_Install()` (src/xbox/xcrash_sys.cpp, solo con `RUN_GAME=1`) parchea el inicio de `KeBugCheckEx` para escribir el código, la dirección que falló y las direcciones de código de la pila; se traducen con `tools/xbox/addr2sym.py xbox/main.map`. Sin verificar en xemu.

- Funciones sin `return` (peculiaridad de la decompilación: el PowerPC dejaba el resultado en r3): con clang caer al final de una función que devuelve valor es una instrucción ilegal (`ud2`). Se vio como `bugcheck 0x1E`, excepción `c000001d`, en `cDvd::ReadCheck`. Corregidas las 4 que hay (`ReadCheck`, `cItemMgr::available`, `cEmWindow::SetBreakEsp`, `MercSysSetAddTime`) con `XRET`/`XRETV` de `include/types.h` (vacías en GC; el preprocesado de GC es idéntico). `tools/xbox/check_returns.sh` vuelve a buscarlas.

## Dos discos

El juego de GameCube son dos discos (id de disco 0 / 1) y pide el cambio él mismo (`cDvd::DiscChange`). En Xbox no hay cambio físico:
`DVDChangeDiskAsync` llama a `XDvdBackend_SetDisc(n)` y el backend sirve `D:\disc1\` o `D:\disc2\` si esas carpetas existen
(si no, usa `D:\` para los dos, como antes). Al arrancar se usa el disco 1. Para el ISO: copiar el contenido de cada disco en
`xbox/bin/disc1/` y `xbox/bin/disc2/`. Sin verificar en xemu; probado en host (`run_dvd_test.sh`).

## Dibujado con la NV2A (`NV2A=1`)

`make -C xbox RUN_GAME=1 NV2A=1` (sin `DEBUGSCREEN`) hace que los píxeles los dibuje la GPU. Sin `NV2A=1` todo sigue por software.

- `xraster.cpp` sigue haciendo en la CPU lo que ya hacía bien: transformar, recortar (near/far y una banda de guarda de ±1024..2048 px), culling,
  decodificar texturas GX a ARGB y codificar las copias del EFB. Con un backend GPU (`XRaster_SetGpu`, `include/xbox/xrgpu.h`) en lugar de rellenar
  píxeles entrega triángulos terminados (coordenadas de ventana, z de 24 bits, w de clip) y el estado de píxel (TEV, blend, z, alpha, scissor).
- `src/xbox/xnv2a.cpp` (pbkit) los dibuja: vertex program de paso (`tools/xbox/shaders/gx.vs.cg`, compilado con vp20compiler), etapas TEV -> combiners
  generales (una etapa TEV = 1 o 2 combiners; bias -0.5, escalas x1/x2/x4/x0.5; máx. 8 combiners y 4 unidades de textura por draw), texturas subidas
  swizzled en potencia de dos con caché por contenido, blend/z/alpha test/máscara de color/scissor en sus registros NV097.
- El EFB es el extra buffer 0 de pbkit. `GXCopyDisp` lo renderiza al extra buffer 1 (instantánea); el retrace de VI dibuja la instantánea en el back buffer,
  imprime el HUD (texto de pbkit) y hace el swap. `GXCopyTex` lee el EFB de vuelta a la CPU y lo codifica como antes.
- Sin hacer todavía: texgen/iluminación (igual que en software), bias +0.5, líneas y puntos, mipmaps, copias de profundidad. Sin verificar en xemu ni en
  hardware: el HUD muestra contadores (`gpu dr.. tri.. tex up.. hit..`, `stg.. over.. TO b.. f.. v..`) para ver qué hizo la GPU.
