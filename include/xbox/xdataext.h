// Big-endian archive header access for GetDataExt (see src/xbox/xdataext.cpp).
#ifndef XBOX_XDATAEXT_H
#define XBOX_XDATAEXT_H
#ifdef __cplusplus
extern "C" {
#endif
unsigned XDataExt_Num(const void* arc);                                                       // number of blocks (0 if implausible)
void* XDataExt_Block(void* arc, unsigned i, const char* tag, void (*trace)(const char*));     // block i, NULL while its tag has no converter
#ifdef __cplusplus
}
#endif
#endif
