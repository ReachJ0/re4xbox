// OSLink / OSUnlink for statically linked REL modules (see include/xbox/xrel.h).
//
// Same contract as the Dolphin SDK (src/lib/OSLink.c): the module goes on the module list, its header
// offsets become addresses (section table, relocation and import tables, sections, name), `prolog`,
// `epilog` and `unresolved` become callable addresses, the bss it is given is cleared; OSUnlink undoes all
// of it. What is not done is relocation: the code is already in the executable.
//
// Differences from the SDK:
//  - the module must be one of the built-in ones (XRel_Find(id)), otherwise OSLink returns FALSE;
//  - OSUnlink also undoes fixSize (version 3) and the import table cut of OSLinkFixed, which the SDK leaves;
//  - linking a module that is already linked (same header, or same id) returns FALSE instead of corrupting
//    the list;
//  - the entry points are the built-in functions, not "section base + offset": the original offsets are
//    remembered and restored by OSUnlink.
#ifdef RE4_XBOX

#include <dolphin/os.h>
#include <stdint.h>
#include <string.h>

#include "xbox/xrel.h"

#define MAX_LINKED 128

// u32 holds a console address; on a 64 bit host the casts go through uintptr_t
#define ADDR(p) ((u32) (uintptr_t) (p))
#define PTR(T, a) ((T) (uintptr_t) (a))

extern "C" {

OSModuleQueue __OSModuleInfoList;
const void* __OSStringTable;

}

namespace {

struct Linked {
    OSModuleInfo* info;
    const XRelModule* mod;
    u32 prolog, epilog, unresolved;  // the header's values before linking
    u32 impSize;                     // and the import table size (OSLinkFixed cuts it)
};

const XRelModule* s_table;
unsigned int s_count;
Linked s_linked[MAX_LINKED];

Linked* FindLinked(const OSModuleInfo* info)
{
    for (int i = 0; i < MAX_LINKED; i++) {
        if (s_linked[i].info == info) {
            return &s_linked[i];
        }
    }
    return 0;
}

Linked* FindLinkedId(u32 id)
{
    for (int i = 0; i < MAX_LINKED; i++) {
        if (s_linked[i].info && XREL_U32(s_linked[i].info->id) == id) {
            return &s_linked[i];
        }
    }
    return 0;
}

#define ENQUEUE_INFO(queue, info, link) \
    do { \
        OSModuleInfo* __prev = (queue)->tail; \
        if (__prev == NULL) (queue)->head = (info); else __prev->link.next = (info); \
        (info)->link.prev = __prev; \
        (info)->link.next = NULL; \
        (queue)->tail = (info); \
    } while (0)

#define DEQUEUE_INFO(info, queue, link) \
    do { \
        OSModuleInfo* __next = (info)->link.next; \
        OSModuleInfo* __prev = (info)->link.prev; \
        if (__next == NULL) (queue)->tail = __prev; else __next->link.prev = __prev; \
        if (__prev == NULL) (queue)->head = __next; else __prev->link.next = __next; \
    } while (0)

BOOL Link(OSModuleInfo* newModule, void* bss, BOOL fixed)
{
    OSModuleHeader* h = (OSModuleHeader*) newModule;
    OSSectionInfo* si;
    OSImportInfo* imp;
    Linked* slot = 0;
    const XRelModule* mod;
    u32 i;

    h->bssSection = 0;

    if (OS_MODULE_VERSION < newModule->version ||
        (2 <= newModule->version &&
         ((h->align && ADDR(newModule) % h->align != 0) || (h->bssAlign && ADDR(bss) % h->bssAlign != 0)))) {
        return FALSE;
    }
    if (FindLinked(newModule) || FindLinkedId(newModule->id)) {
        return FALSE;
    }
    mod = XRel_Find(XREL_U32(newModule->id));
    if (!mod) {
        return FALSE;
    }
    // the header promises entry points the built-in copy must have
    if ((h->prologSection != 0 && !mod->prolog) || (h->epilogSection != 0 && !mod->epilog) ||
        (h->unresolvedSection != 0 && !mod->unresolved)) {
        return FALSE;
    }
    for (i = 0; i < MAX_LINKED; i++) {
        if (!s_linked[i].info) {
            slot = &s_linked[i];
            break;
        }
    }
    if (!slot) {
        return FALSE;
    }

    ENQUEUE_INFO(&__OSModuleInfoList, newModule, link);
    slot->info = newModule;
    slot->mod = mod;
    slot->prolog = h->prolog;
    slot->epilog = h->epilog;
    slot->unresolved = h->unresolved;
    slot->impSize = h->impSize;

    newModule->sectionInfoOffset += ADDR(h);
    h->relOffset += ADDR(h);
    h->impOffset += ADDR(h);
    if (3 <= newModule->version) {
        h->fixSize += ADDR(h);
    }
    for (i = 1; i < newModule->numSections; i++) {
        si = &PTR(OSSectionInfo*, (newModule)->sectionInfoOffset)[i];
        if (si->offset != 0) {
            si->offset += ADDR(h);
        } else if (si->size != 0) {
            h->bssSection = (u8) i;
            si->offset = ADDR(bss);
        }
    }
    for (imp = PTR(OSImportInfo*, h->impOffset); imp < PTR(OSImportInfo*, h->impOffset + h->impSize); imp++) {
        imp->offset += ADDR(h);
    }

    // the entry points: the built-in functions (0 where the header says the module has none)
    if (h->prologSection != 0) {
        h->prolog = ADDR(mod->prolog);
    }
    if (h->epilogSection != 0) {
        h->epilog = ADDR(mod->epilog);
    }
    if (h->unresolvedSection != 0) {
        h->unresolved = ADDR(mod->unresolved);
    }

    if (__OSStringTable) {
        newModule->nameOffset += ADDR(__OSStringTable);
    }

    if (fixed) {
        for (imp = PTR(OSImportInfo*, h->impOffset); imp < PTR(OSImportInfo*, h->impOffset + h->impSize); imp++) {
            if (imp->id == 0 || imp->id == newModule->id) {
                h->impSize = ADDR((u8*) imp) - h->impOffset;
                break;
            }
        }
    }

    if (bss && h->bssSize) {
        memset(bss, 0, h->bssSize);
    }
    OSNotifyLink(newModule);
    return TRUE;
}

}  // namespace

extern "C" {

void XRel_Register(const XRelModule* table, unsigned int count)
{
    s_table = table;
    s_count = count;
}

const XRelModule* XRel_Find(unsigned int id)
{
    for (unsigned int i = 0; i < s_count; i++) {
        if (s_table[i].id == id) {
            return &s_table[i];
        }
    }
    return 0;
}

unsigned int XRel_LinkedCount(void)
{
    unsigned int n = 0;
    for (int i = 0; i < MAX_LINKED; i++) {
        n += s_linked[i].info != 0;
    }
    return n;
}

void __OSModuleInit(void)
{
    __OSModuleInfoList.head = __OSModuleInfoList.tail = 0;
    __OSStringTable = 0;
    memset(s_linked, 0, sizeof(s_linked));
}

void OSNotifyLink(OSModuleInfo*) {}
void OSNotifyUnlink(OSModuleInfo*) {}

void OSSetStringTable(void* stringTable) { __OSStringTable = stringTable; }

BOOL OSLink(OSModuleInfo* newModule, void* bss) { return Link(newModule, bss, FALSE); }

BOOL OSLinkFixed(OSModuleInfo* newModule, void* bss)
{
    if (OS_MODULE_VERSION < newModule->version || newModule->version < 3) {
        return FALSE;
    }
    return Link(newModule, bss, TRUE);
}

BOOL OSUnlink(OSModuleInfo* oldModule)
{
    OSModuleHeader* h = (OSModuleHeader*) oldModule;
    Linked* slot = FindLinked(oldModule);
    OSSectionInfo* si;
    OSImportInfo* imp;
    u32 i;

    if (!slot) {
        return FALSE;  // not linked: nothing to undo (the SDK would corrupt the list)
    }
    DEQUEUE_INFO(oldModule, &__OSModuleInfoList, link);
    OSNotifyUnlink(oldModule);
    if (__OSStringTable) {
        oldModule->nameOffset -= ADDR(__OSStringTable);
    }
    h->prolog = slot->prolog;
    h->epilog = slot->epilog;
    h->unresolved = slot->unresolved;
    h->impSize = slot->impSize;
    for (imp = PTR(OSImportInfo*, h->impOffset); imp < PTR(OSImportInfo*, h->impOffset + h->impSize); imp++) {
        imp->offset -= ADDR(h);
    }
    for (i = 1; i < oldModule->numSections; i++) {
        si = &PTR(OSSectionInfo*, (oldModule)->sectionInfoOffset)[i];
        if (i == h->bssSection) {
            h->bssSection = 0;
            si->offset = 0;
        } else if (si->offset != 0) {
            si->offset -= ADDR(h);
        }
    }
    h->relOffset -= ADDR(h);
    h->impOffset -= ADDR(h);
    if (3 <= oldModule->version) {
        h->fixSize -= ADDR(h);  // the SDK forgets this one
    }
    oldModule->sectionInfoOffset -= ADDR(h);
    memset(slot, 0, sizeof(*slot));
    return TRUE;
}

OSModuleInfo* OSSearchModule(void* ptr, u32* section, u32* offset)
{
    OSModuleInfo* moduleInfo;
    OSSectionInfo* sectionInfo;
    u32 i, baseSection;

    if (ptr == NULL) {
        return NULL;
    }
    for (moduleInfo = __OSModuleInfoList.head; moduleInfo; moduleInfo = moduleInfo->link.next) {
        sectionInfo = PTR(OSSectionInfo*, moduleInfo->sectionInfoOffset);
        for (i = 0; i < moduleInfo->numSections; i++, sectionInfo++) {
            if (sectionInfo->size != 0) {
                baseSection = sectionInfo->offset & 0xFFFFFFFE;
                if (baseSection <= ADDR(ptr) && ADDR(ptr) < baseSection + sectionInfo->size) {
                    if (section) *section = i;
                    if (offset) *offset = ADDR(ptr) - baseSection;
                    return moduleInfo;
                }
            }
        }
    }
    return NULL;
}

}  // extern "C"

#endif  // RE4_XBOX
