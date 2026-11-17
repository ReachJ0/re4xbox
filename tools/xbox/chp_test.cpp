// Host test of src/xbox/chp_xbox.cpp (DSList, Strcmp/Strcpy/Strlen).
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned char u8; typedef signed char s8; typedef unsigned int u32; typedef char* Ptr;
typedef struct { Ptr Prev; Ptr Next; } DSLink, *DSLinkPtr;
typedef struct { u32 Offset; Ptr Head; Ptr Tail; } DSList, *DSListPtr;
extern "C" { void DSInitList(DSListPtr, Ptr, DSLinkPtr); void DSInsertListObject(DSListPtr, Ptr, Ptr);
 void DSRemoveListObject(DSListPtr, Ptr); void DSAttachList(DSListPtr, DSListPtr); void* DSNextListObj(DSListPtr, Ptr);
 void Strcpy(char*, char*); s8 Strcmp(char*, char*); u32 Strlen(char*); }
static int fails, checks;
#define CHECK(c) do { checks++; if (!(c)) { fails++; printf("FAIL line %d: %s\n", __LINE__, #c); } } while (0)
struct Node { int id; DSLink link; };
int main() {
    Node n[4]; DSList l, m; for (int i = 0; i < 4; i++) n[i].id = i;
    DSInitList(&l, (Ptr) &n[0], (DSLinkPtr) &n[0].link); DSInitList(&m, (Ptr) &n[0], (DSLinkPtr) &n[0].link);
    size_t off = offsetof(Node, link);
    CHECK(l.Offset == off && !l.Head && !l.Tail);
    DSInsertListObject(&l, 0, (Ptr) &n[0]); DSInsertListObject(&l, 0, (Ptr) &n[1]); DSInsertListObject(&l, 0, (Ptr) &n[2]);
    CHECK(l.Head == (Ptr) &n[0] && l.Tail == (Ptr) &n[2]);
    CHECK(DSNextListObj(&l, (Ptr) &n[0]) == &n[1] && DSNextListObj(&l, (Ptr) &n[2]) == 0);
    DSRemoveListObject(&l, (Ptr) &n[1]);
    CHECK(DSNextListObj(&l, (Ptr) &n[0]) == &n[2] && n[2].link.Prev == (Ptr) &n[0]);
    DSInsertListObject(&l, (Ptr) &n[2], (Ptr) &n[1]);                    // before the cursor
    CHECK(DSNextListObj(&l, (Ptr) &n[0]) == &n[1] && DSNextListObj(&l, (Ptr) &n[1]) == &n[2]);
    DSInsertListObject(&l, (Ptr) &n[0], (Ptr) &n[3]);                    // before the head
    CHECK(l.Head == (Ptr) &n[3] && n[0].link.Prev == (Ptr) &n[3]);
    DSRemoveListObject(&l, (Ptr) &n[3]); DSRemoveListObject(&l, (Ptr) &n[2]); DSRemoveListObject(&l, (Ptr) &n[0]);
    CHECK(l.Head == (Ptr) &n[1] && l.Tail == (Ptr) &n[1]);
    DSRemoveListObject(&l, (Ptr) &n[1]); CHECK(!l.Head && !l.Tail);
    DSInsertListObject(&l, 0, (Ptr) &n[0]); DSInsertListObject(&m, 0, (Ptr) &n[1]); DSInsertListObject(&m, 0, (Ptr) &n[2]);
    DSAttachList(&l, &m);
    CHECK(l.Tail == (Ptr) &n[2] && !m.Head && DSNextListObj(&l, (Ptr) &n[0]) == &n[1] && n[1].link.Prev == (Ptr) &n[0]);
    char a[8], b[8]; Strcpy(a, (char*) "abc"); CHECK(!strcmp(a, "abc") && Strlen(a) == 3 && Strlen((char*) "") == 0);
    strcpy(b, "abd"); CHECK(Strcmp(a, a) == 0 && Strcmp(a, b) < 0 && Strcmp(b, a) > 0 && Strcmp((char*) "a", (char*) "ab") < 0);
    printf("chp %d checks, %d failed\n", checks, fails); return fails != 0;
}
