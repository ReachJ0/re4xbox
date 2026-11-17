// CharPipeline helpers the SDK source tree does not carry (structures/List, dolphinString): written from their
// documented behaviour for src/lib/fileCache.c and src/lib/texPalette.c.
#ifdef RE4_XBOX
#include <charPipeline/structures/List.h>
#include <charPipeline/structures/dolphinString.h>
#include <stddef.h>

#define LINK(list, obj) ((DSLinkPtr) ((char*) (obj) + (list)->Offset))

extern "C" {

void DSInitList(DSListPtr list, Ptr obj, DSLinkPtr link)
{
    list->Offset = (u32) ((char*) link - (char*) obj);
    list->Head = NULL;
    list->Tail = NULL;
}

// Inserts `obj` before `cursor`; a null cursor appends at the tail.
void DSInsertListObject(DSListPtr list, Ptr cursor, Ptr obj)
{
    DSLinkPtr l = LINK(list, obj);

    if (cursor == NULL) {
        l->Next = NULL;
        l->Prev = list->Tail;
        if (list->Tail) {
            LINK(list, list->Tail)->Next = obj;
        } else {
            list->Head = obj;
        }
        list->Tail = obj;
        return;
    }
    {
        DSLinkPtr c = LINK(list, cursor);
        l->Next = cursor;
        l->Prev = c->Prev;
        if (c->Prev) {
            LINK(list, c->Prev)->Next = obj;
        } else {
            list->Head = obj;
        }
        c->Prev = obj;
    }
}

void DSRemoveListObject(DSListPtr list, Ptr obj)
{
    DSLinkPtr l = LINK(list, obj);

    if (l->Prev) {
        LINK(list, l->Prev)->Next = l->Next;
    } else {
        list->Head = l->Next;
    }
    if (l->Next) {
        LINK(list, l->Next)->Prev = l->Prev;
    } else {
        list->Tail = l->Prev;
    }
    l->Prev = NULL;
    l->Next = NULL;
}

// Appends every object of `attachList` to `baseList` (both lists use the same link offset).
void DSAttachList(DSListPtr baseList, DSListPtr attachList)
{
    if (!attachList->Head) {
        return;
    }
    if (baseList->Tail) {
        LINK(baseList, baseList->Tail)->Next = attachList->Head;
        LINK(baseList, attachList->Head)->Prev = baseList->Tail;
    } else {
        baseList->Head = attachList->Head;
    }
    baseList->Tail = attachList->Tail;
    attachList->Head = NULL;
    attachList->Tail = NULL;
}

void* DSNextListObj(DSListPtr list, Ptr obj)
{
    return LINK(list, obj)->Next;
}

void Strcpy(char* dst, char* src)
{
    while ((*dst++ = *src++) != 0) {
    }
}

s8 Strcmp(char* a, char* b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char) *a == (unsigned char) *b ? 0 : ((unsigned char) *a < (unsigned char) *b ? -1 : 1);
}

u32 Strlen(char* s)
{
    u32 n = 0;
    while (*s++) {
        n++;
    }
    return n;
}

}
#endif
