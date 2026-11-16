// RE4 for the original Xbox: statically linked REL modules.
//
// The GameCube game loads ~114 relocatable modules (rooms, enemies, weapons, the sub screen) from the disc
// with OSLink and calls their _prolog / _epilog. PowerPC code cannot run on the Xbox, so every module is
// compiled into the executable and "linking" one only means finding the built-in copy by its module id (the
// `id` in the REL header, recorded in config/G4BE08/modules/<mod>/rel.json) and handing the game the
// addresses of its entry points. Nothing is relocated.
//
// Per module the build must give the three entry points unique names (<mod>_prolog / _epilog / _unresolved,
// e.g. by compiling the module with -D_prolog=<mod>_prolog ...); src/xbox/rel_registry.cpp is generated from
// the rel.json files by tools/xbox/gen_rel_registry.py and lists them.
#ifndef XBOX_XREL_H
#define XBOX_XREL_H

#ifdef RE4_XBOX

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*XRelFunc)(void);

typedef struct XRelModule {
    unsigned int id;        // OSModuleInfo.id of the REL
    const char* name;       // "em10", "wep12", "st1_0" ...
    XRelFunc prolog;        // may be NULL (the module has none)
    XRelFunc epilog;
    XRelFunc unresolved;
} XRelModule;

// The built-in modules. Replaces any earlier table; the table must outlive the program (it is not copied).
// Linked modules keep the pointers they were linked with. XRel_Init() registers the generated table.
void XRel_Register(const XRelModule* table, unsigned int count);
void XRel_Init(void);
const XRelModule* XRel_Find(unsigned int id);
// Number of modules linked right now.
unsigned int XRel_LinkedCount(void);

// REL headers come from the disc in the PowerPC's byte order. Every field the layer reads goes through this,
// so the one place that knows about byte order is here: identity until the disc data is byte-swapped on load.
#define XREL_U32(x) (x)

#ifdef __cplusplus
}
#endif

#endif
#endif
