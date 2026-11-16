// Host test for the statically linked REL layer (src/xbox/link_xbox.cpp, rel_registry.cpp).
// The module images are linked by the real SDK code (src/lib/OSLink.c, built under other names) and by the
// layer, and everything the SDK does to a header (offsets turned into addresses, the module list, the bss)
// must come out the same; only prolog / epilog / unresolved differ, on purpose.
#include <dolphin/os.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

#include "xbox/xrel.h"

extern "C" {
BOOL sdk_OSLink(OSModuleInfo*, void*);
BOOL sdk_OSLinkFixed(OSModuleInfo*, void*);
BOOL sdk_OSUnlink(OSModuleInfo*);
OSModuleInfo* sdk_OSSearchModule(void*, u32*, u32*);
void sdk_OSSetStringTable(void*);
extern OSModuleQueue sdk_list;
extern OSModuleQueue __OSModuleInfoList;
}

extern "C" {
// what the SDK linker calls after patching code (cache maintenance) and when it meets an unknown relocation
void DCFlushRange(void*, u32) {}
void ICInvalidateRange(void*, u32) {}
void OSReport(const char*, ...) {}
}

static int g_n, g_bad;
#define CHECK(c) do { g_n++; if (!(c)) { g_bad++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)

// ---- entry points the "modules" have
static int g_calls[8];
extern "C" {
void ma_pro(void) { g_calls[0]++; }
void ma_epi(void) { g_calls[1]++; }
void ma_unr(void) { g_calls[2]++; }
void mb_pro(void) { g_calls[3]++; }
void mb_epi(void) { g_calls[4]++; }
}
static XRelModule g_mods[140] = {
    {11, "ma", ma_pro, ma_epi, ma_unr},
    {13, "mb", mb_pro, mb_epi, ma_unr},
    {15, "mc_nopro", NULL, ma_epi, NULL},
};

// ---- a module image in memory below 4 GB (the SDK code keeps addresses in 32 bit u32)
// (OSModuleInfo holds two pointers: on a 64 bit host the header is bigger than the console's 0x4C bytes)
enum { IMG = 0x400, SEC = 0x80, IMP = 0xC0, REL = 0xD0, NAME = 0xE0 };
struct Image { uint8_t* p; };
static uint8_t* AllocImg()
{
    void* m = mmap(NULL, 0x1000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    return m == MAP_FAILED ? NULL : (uint8_t*) m;
}
static void Fill(uint8_t* img, u32 id, int version, bool withImports, bool noEntry)
{
    memset(img, 0, IMG);
    OSModuleHeader* h = (OSModuleHeader*) img;
    h->info.id = id;
    h->info.numSections = 5;
    h->info.sectionInfoOffset = SEC;
    h->info.nameOffset = NAME;
    h->info.nameSize = 5;
    h->info.version = version;
    h->bssSize = 0x30;
    h->relOffset = REL;
    h->impOffset = IMP;
    h->impSize = withImports ? 16 : 0;
    h->prologSection = noEntry ? 0 : 1; h->epilogSection = noEntry ? 0 : 1; h->unresolvedSection = noEntry ? 0 : 1;
    h->prolog = noEntry ? 0 : 0x10; h->epilog = noEntry ? 0 : 0x20; h->unresolved = noEntry ? 0 : 0x30;
    h->align = 8; h->bssAlign = 4; h->fixSize = 0x40;
    OSSectionInfo* si = (OSSectionInfo*) (img + SEC);
    si[1].offset = 0x100 | 1; si[1].size = 0x40;   // text (exec bit)
    si[2].offset = 0x140;     si[2].size = 0x20;
    si[3].offset = 0x180;     si[3].size = 0x20;
    si[4].offset = 0;         si[4].size = 0x30;    // bss: filled in by the link
    OSImportInfo* imp = (OSImportInfo*) (img + IMP);
    imp[0].id = 0;  imp[0].offset = REL;            // the DOL: just an end marker
    imp[1].id = id; imp[1].offset = REL;
    OSRel* r = (OSRel*) (img + REL);
    r->offset = 0; r->type = R_DOLPHIN_END; r->section = 0; r->addend = 0;
    memcpy(img + NAME, "mod_x", 6);
}
static u32 g_impOrig;   // import table size before the link
static bool SameExceptEntries(uint8_t* a, uint8_t* b, bool linked, u32 strtabA, u32 strtabB)
{
    u32 ba = (u32) (uintptr_t) a, bb = (u32) (uintptr_t) b;
    OSModuleHeader *ha = (OSModuleHeader*) a, *hb = (OSModuleHeader*) b;
    bool ok = ha->info.id == hb->info.id && ha->info.numSections == hb->info.numSections && ha->info.version == hb->info.version &&
              ha->info.nameSize == hb->info.nameSize && ha->bssSize == hb->bssSize && ha->impSize == hb->impSize &&
              ha->prologSection == hb->prologSection && ha->epilogSection == hb->epilogSection &&
              ha->unresolvedSection == hb->unresolvedSection && ha->bssSection == hb->bssSection &&
              ha->align == hb->align && ha->bssAlign == hb->bssAlign;
    u32 adj = linked ? 1 : 0;
    ok = ok && ha->info.sectionInfoOffset - (adj ? ba : 0) == hb->info.sectionInfoOffset - (adj ? bb : 0);
    ok = ok && ha->relOffset - (adj ? ba : 0) == hb->relOffset - (adj ? bb : 0);
    ok = ok && ha->impOffset - (adj ? ba : 0) == hb->impOffset - (adj ? bb : 0);
    ok = ok && ha->fixSize - (adj && ha->info.version >= 3 ? ba : 0) == hb->fixSize - (adj && hb->info.version >= 3 ? bb : 0);
    ok = ok && ha->info.nameOffset - (strtabA ? strtabA : 0) == hb->info.nameOffset - (strtabB ? strtabB : 0);
    OSSectionInfo *sa = (OSSectionInfo*) (a + SEC), *sb = (OSSectionInfo*) (b + SEC);
    if (linked) { sa = (OSSectionInfo*) (uintptr_t) ha->info.sectionInfoOffset; sb = (OSSectionInfo*) (uintptr_t) hb->info.sectionInfoOffset; }
    for (u32 i = 0; i < 5; i++) {
        ok = ok && sa[i].size == sb[i].size;
        // bss sections point at the bss given, text/data at the image
        if (sa[i].offset == 0) ok = ok && sb[i].offset == 0;
        else if (linked && i != ha->bssSection) ok = ok && sa[i].offset - ba == sb[i].offset - bb;
        else if (!linked) ok = ok && sa[i].offset == sb[i].offset;
    }
    OSImportInfo *ia = (OSImportInfo*) (linked ? (uintptr_t) ha->impOffset : (uintptr_t) (a + IMP));
    OSImportInfo *ib = (OSImportInfo*) (linked ? (uintptr_t) hb->impOffset : (uintptr_t) (b + IMP));
    for (int i = 0; i < 2; i++) { bool moved = adj && g_impOrig != 0; ok = ok && ia[i].id == ib[i].id && ia[i].offset - (moved ? ba : 0) == ib[i].offset - (moved ? bb : 0); }
    if (!ok) printf("  bss %d/%d bssSec %d/%d impSize %d/%d fix %x/%x name %x/%x\n", ha->bssSize, hb->bssSize, ha->bssSection, hb->bssSection, ha->impSize, hb->impSize, ha->fixSize, hb->fixSize, ha->info.nameOffset, hb->info.nameOffset);
    if (!ok) printf("  headers differ: ids %d sec %d imp %d relo %d\n", (int) (ha->info.id == hb->info.id), (int) (ha->info.sectionInfoOffset - (adj ? ba : 0) == hb->info.sectionInfoOffset - (adj ? bb : 0)), (int) (ha->impOffset - (adj ? ba : 0) == hb->impOffset - (adj ? bb : 0)), (int) (ha->relOffset - (adj ? ba : 0) == hb->relOffset - (adj ? bb : 0)));
    return ok;
}

static void TestLinkAgainstSdk(int version, bool imports, bool noEntry, bool fixed)
{
    int before = g_bad; struct Rep { int* b; int s; int v, i, n, f; ~Rep() { if (*b != s) printf("  (version %d imports %d noEntry %d fixed %d)\n", v, i, n, f); } } rep = {&g_bad, before, version, imports, noEntry, fixed};
    uint8_t *a = AllocImg(), *b = AllocImg(), *orig = (uint8_t*) malloc(IMG);
    uint8_t *bssA = AllocImg(), *bssB = AllocImg();
    memset(bssA, 0xAA, 0x100); memset(bssB, 0xAA, 0x100);
    Fill(a, 11, version, imports, noEntry); Fill(b, 11, version, imports, noEntry);
    if (imports && fixed) { ((OSImportInfo*) (a + IMP))[0].id = 5; ((OSImportInfo*) (b + IMP))[0].id = 5; }   // the cut lands on the second entry (own id)
    memcpy(orig, a, IMG);
    g_impOrig = imports ? 16 : 0;
    BOOL ra = fixed ? sdk_OSLinkFixed((OSModuleInfo*) a, bssA) : sdk_OSLink((OSModuleInfo*) a, bssA);
    BOOL rb = fixed ? OSLinkFixed((OSModuleInfo*) b, bssB) : OSLink((OSModuleInfo*) b, bssB);
    CHECK(ra == TRUE && rb == TRUE);
    CHECK(SameExceptEntries(a, b, true, 0, 0));
    // the bss: cleared over bssSize only, address recorded in its section
    CHECK(memcmp(bssA, bssB, 0x100) == 0 && bssB[0x2F] == 0 && bssB[0x30] == 0xAA);
    CHECK(((OSModuleHeader*) b)->bssSection == 4 && ((OSSectionInfo*) (uintptr_t) ((OSModuleInfo*) b)->sectionInfoOffset)[4].offset == (u32) (uintptr_t) bssB);
    // entry points
    OSModuleHeader* hb = (OSModuleHeader*) b;
    if (imports && fixed) CHECK(hb->impSize == 8);
    if (noEntry) CHECK(hb->prolog == 0 && hb->epilog == 0 && hb->unresolved == 0);
    else {
        CHECK(hb->prolog == (u32) (uintptr_t) ma_pro && hb->epilog == (u32) (uintptr_t) ma_epi && hb->unresolved == (u32) (uintptr_t) ma_unr);
        int c0 = g_calls[0], c1 = g_calls[1]; ((void (*)()) (uintptr_t) hb->prolog)(); ((void (*)()) (uintptr_t) hb->epilog)();
        CHECK(g_calls[0] == c0 + 1 && g_calls[1] == c1 + 1);
    }
    // the module list
    CHECK(__OSModuleInfoList.head == (OSModuleInfo*) b && __OSModuleInfoList.tail == (OSModuleInfo*) b && XRel_LinkedCount() == 1);
    CHECK(sdk_list.head == (OSModuleInfo*) a && ((OSModuleInfo*) b)->link.next == NULL && ((OSModuleInfo*) b)->link.prev == NULL);
    // search: every address of the image and some outside
    int agree = 0, total = 0;
    for (u32 off = 0; off < 0x300; off += 4) {
        u32 s1 = 99, o1 = 99, s2 = 99, o2 = 99;
        OSModuleInfo* ma = sdk_OSSearchModule(a + off, &s1, &o1);
        OSModuleInfo* mb = OSSearchModule(b + off, &s2, &o2);
        total++; agree += ((ma != NULL) == (mb != NULL)) && (!ma || (s1 == s2 && o1 == o2));
    }
    CHECK(agree == total);
    // unlink
    CHECK(sdk_OSUnlink((OSModuleInfo*) a) == TRUE && OSUnlink((OSModuleInfo*) b) == TRUE);
    ((OSModuleHeader*) a)->fixSize = ((OSModuleHeader*) orig)->fixSize;   // the SDK leaves fixSize shifted
    ((OSModuleHeader*) a)->impSize = ((OSModuleHeader*) orig)->impSize;   // and OSLinkFixed's cut of the import table
    CHECK(memcmp(b, orig, IMG) == 0 && (memcmp(a, orig, IMG) == 0 || (fixed && imports)));      // the image is back as it was, byte for byte
    CHECK(__OSModuleInfoList.head == NULL && __OSModuleInfoList.tail == NULL && XRel_LinkedCount() == 0);
    CHECK(OSUnlink((OSModuleInfo*) b) == FALSE);                         // twice
    // and it can be linked again
    CHECK(OSLink((OSModuleInfo*) b, bssB) == (BOOL) (version <= 3));
    if (version <= 3) CHECK(OSUnlink((OSModuleInfo*) b) == TRUE && memcmp(b, orig, IMG) == 0);
    free(orig);
}

static void TestFailures()
{
    uint8_t *a = AllocImg(), *bss = AllocImg();
    Fill(a, 99, 3, false, false);
    CHECK(OSLink((OSModuleInfo*) a, bss) == FALSE && XRel_LinkedCount() == 0 && __OSModuleInfoList.head == NULL);   // not built in
    Fill(a, 11, 4, false, false);
    CHECK(OSLink((OSModuleInfo*) a, bss) == FALSE);                       // newer than the layer knows
    Fill(a, 11, 3, false, false); ((OSModuleHeader*) a)->align = 0x40;     // image not aligned that much? a is page aligned
    CHECK(OSLink((OSModuleInfo*) a, bss) == TRUE); OSUnlink((OSModuleInfo*) a);
    Fill(a, 11, 3, false, false); ((OSModuleHeader*) a)->bssAlign = 16;
    CHECK(OSLink((OSModuleInfo*) a, bss + 4) == FALSE);                    // bss misaligned
    CHECK(OSLink((OSModuleInfo*) a, bss + 16) == TRUE); OSUnlink((OSModuleInfo*) a);
    {   // an image that is not aligned as its header asks (0x2000): the odd page of two
        uint8_t* two = (uint8_t*) mmap(NULL, 0x3000, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        uint8_t* odd = ((uintptr_t) two & 0x1FFF) ? two : two + 0x1000;
        Fill(odd, 11, 3, false, false); ((OSModuleHeader*) odd)->align = 0x2000;
        CHECK(OSLink((OSModuleInfo*) odd, bss) == FALSE && XRel_LinkedCount() == 0);
        uint8_t* even = ((uintptr_t) two & 0x1FFF) ? two + 0x1000 : two;
        Fill(even, 11, 3, false, false); ((OSModuleHeader*) even)->align = 0x2000;
        CHECK(OSLink((OSModuleInfo*) even, bss) == TRUE && OSUnlink((OSModuleInfo*) even) == TRUE);
    }
    // the header promises an epilog the built-in module does not have
    Fill(a, 15, 3, false, false); ((OSModuleHeader*) a)->prologSection = 0; ((OSModuleHeader*) a)->unresolvedSection = 0;
    CHECK(OSLink((OSModuleInfo*) a, bss) == TRUE); OSUnlink((OSModuleInfo*) a);   // mc_nopro: epilog only
    Fill(a, 15, 3, false, false);
    CHECK(OSLink((OSModuleInfo*) a, bss) == FALSE && __OSModuleInfoList.head == NULL && XRel_LinkedCount() == 0);   // wants a prolog
    Fill(a, 15, 3, false, false); ((OSModuleHeader*) a)->epilogSection = 0; ((OSModuleHeader*) a)->unresolvedSection = 0;
    CHECK(OSLink((OSModuleInfo*) a, bss) == FALSE && XRel_LinkedCount() == 0);   // wants only the prolog
    // fixed linking needs version 3
    Fill(a, 11, 2, false, false);
    CHECK(OSLinkFixed((OSModuleInfo*) a, bss) == FALSE);
    // already linked: same header, same id in another image
    uint8_t* b = AllocImg();
    Fill(a, 11, 3, false, false); Fill(b, 11, 3, false, false);
    CHECK(OSLink((OSModuleInfo*) a, bss) == TRUE && OSLink((OSModuleInfo*) a, bss) == FALSE && OSLink((OSModuleInfo*) b, bss) == FALSE);
    CHECK(XRel_LinkedCount() == 1 && __OSModuleInfoList.head == (OSModuleInfo*) a && ((OSModuleInfo*) a)->link.next == NULL);
    CHECK(OSUnlink((OSModuleInfo*) b) == FALSE);                           // never linked
    CHECK(OSUnlink((OSModuleInfo*) a) == TRUE);
    // no bss given for a module without bss
    Fill(a, 11, 3, false, false); ((OSModuleHeader*) a)->bssSize = 0;
    CHECK(OSLink((OSModuleInfo*) a, NULL) == TRUE && OSUnlink((OSModuleInfo*) a) == TRUE);
}

static void TestListOrderAndCapacity()
{
    uint8_t *a = AllocImg(), *b = AllocImg(), *bss = AllocImg();
    Fill(a, 11, 3, false, false); Fill(b, 13, 3, false, false);
    CHECK(OSLink((OSModuleInfo*) a, bss) && OSLink((OSModuleInfo*) b, bss + 0x40));
    CHECK(__OSModuleInfoList.head == (OSModuleInfo*) a && __OSModuleInfoList.tail == (OSModuleInfo*) b);
    CHECK(((OSModuleInfo*) a)->link.next == (OSModuleInfo*) b && ((OSModuleInfo*) b)->link.prev == (OSModuleInfo*) a);
    // a pointer inside either image finds its module
    CHECK(OSSearchModule(a + 0x140, NULL, NULL) == (OSModuleInfo*) a || OSSearchModule(a + 0x140, NULL, NULL) == NULL);
    u32 sec = 0, off = 0;
    CHECK(OSSearchModule(a + 0x150, &sec, &off) == (OSModuleInfo*) a && sec == 2 && off == 0x10);
    CHECK(OSSearchModule(b + 0x104, &sec, &off) == (OSModuleInfo*) b && sec == 1 && off == 4);
    CHECK(OSSearchModule(NULL, &sec, &off) == NULL && OSSearchModule(b + 0x3F0, &sec, &off) == NULL);
    CHECK(OSUnlink((OSModuleInfo*) a) && __OSModuleInfoList.head == (OSModuleInfo*) b && __OSModuleInfoList.tail == (OSModuleInfo*) b);
    CHECK(OSUnlink((OSModuleInfo*) b) && __OSModuleInfoList.head == NULL);
    // 128 modules linked at once, the 129th is refused, and a freed slot is reusable
    uint8_t* imgs[130]; uint8_t* bss2 = AllocImg();
    for (int i = 0; i < 130; i++) { g_mods[3 + i].id = 1000 + i; g_mods[3 + i].name = "x"; g_mods[3 + i].prolog = ma_pro; g_mods[3 + i].epilog = ma_epi; g_mods[3 + i].unresolved = ma_unr; }
    XRel_Register(g_mods, 3 + 130);
    int ok = 0;
    for (int i = 0; i < 129; i++) { imgs[i] = AllocImg(); Fill(imgs[i], 1000 + i, 3, false, false); ok += OSLink((OSModuleInfo*) imgs[i], bss2) == TRUE; }
    CHECK(ok == 128 && XRel_LinkedCount() == 128 && OSLink((OSModuleInfo*) imgs[128], bss2) == FALSE);
    CHECK(OSUnlink((OSModuleInfo*) imgs[5]) && OSLink((OSModuleInfo*) imgs[128], bss2) == TRUE);
    int n = 0; for (OSModuleInfo* m = __OSModuleInfoList.head; m; m = m->link.next) n++;
    CHECK(n == 128);
    for (int i = 0; i < 129; i++) if (i != 5) OSUnlink((OSModuleInfo*) imgs[i]);
    CHECK(XRel_LinkedCount() == 0 && __OSModuleInfoList.head == NULL);
    XRel_Register(g_mods, 3);
}

static void TestStringTable()
{
    uint8_t *a = AllocImg(), *b = AllocImg(), *bss = AllocImg(), *bss2 = AllocImg();
    Fill(a, 11, 3, false, false); Fill(b, 11, 3, false, false);
    u32 tab = 0x1000;
    sdk_OSSetStringTable((void*) (uintptr_t) tab); OSSetStringTable((void*) (uintptr_t) tab);
    CHECK(sdk_OSLink((OSModuleInfo*) a, bss) && OSLink((OSModuleInfo*) b, bss2));
    CHECK(((OSModuleInfo*) a)->nameOffset == NAME + tab && ((OSModuleInfo*) b)->nameOffset == NAME + tab);
    CHECK(sdk_OSUnlink((OSModuleInfo*) a) && OSUnlink((OSModuleInfo*) b));
    CHECK(((OSModuleInfo*) b)->nameOffset == NAME);
    sdk_OSSetStringTable(NULL); OSSetStringTable(NULL);
}

static void TestRegistry()
{
    XRel_Register(NULL, 0);
    CHECK(XRel_Find(11) == NULL);
    XRel_Init();
    int n = 0; unsigned seen = 0;
    for (unsigned id = 0; id < 300; id++) { const XRelModule* m = XRel_Find(id); if (m) { n++; CHECK(m->id == id && m->name && m->prolog && m->epilog && m->unresolved); } }
    (void) seen;
    CHECK(n == 114);
    CHECK(XRel_Find(11) && strcmp(XRel_Find(11)->name, "em10") == 0);
    CHECK(XRel_Find(71) && strcmp(XRel_Find(71)->name, "Sscrn") == 0);
    CHECK(XRel_Find(1) && strcmp(XRel_Find(1)->name, "Tools") == 0);
    CHECK(XRel_Find(74) && strcmp(XRel_Find(74)->name, "st1_0") == 0);
    // all 114 can be linked and unlinked through the game's own entry points
    uint8_t* bss = AllocImg(); int linked = 0;
    uint8_t* img[300]; memset(img, 0, sizeof img);
    for (unsigned id = 0; id < 300; id++) if (XRel_Find(id)) {
        // 128 at a time at most: link 100, unlink, then the rest
        img[id] = AllocImg(); Fill(img[id], id, 3, false, false);
    }
    int cnt = 0;
    for (unsigned id = 0; id < 300; id++) if (img[id]) { linked += OSLink((OSModuleInfo*) img[id], bss) == TRUE; cnt++; }
    CHECK(linked == 114 && XRel_LinkedCount() == 114);
    for (unsigned id = 0; id < 300; id++) if (img[id]) { CHECK(((void (*)()) (uintptr_t) ((OSModuleHeader*) img[id])->prolog) == (void (*)()) XRel_Find(id)->prolog); CHECK(OSUnlink((OSModuleInfo*) img[id]) == TRUE); }
    CHECK(XRel_LinkedCount() == 0);
    XRel_Register(g_mods, 3);
}

int main()
{
    XRel_Register(g_mods, 3);
    for (int v = 1; v <= 3; v++)
        for (int imp = 0; imp < 2; imp++)
            for (int noEntry = 0; noEntry < 2; noEntry++)
                for (int fixed = 0; fixed < 2; fixed++) {
                    if (fixed && v < 3) continue;
                    TestLinkAgainstSdk(v, imp, noEntry, fixed);
                }
    TestFailures();
    TestListOrderAndCapacity();
    TestStringTable();
    TestRegistry();
    printf("%d checks, %d failures\n", g_n, g_bad);
    return g_bad != 0;
}
