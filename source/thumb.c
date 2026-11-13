/*
 * JPEG thumbnail decoding for Tama3DS.
 * P's download items start with a small baseline JPEG preview
 * (24x24 for food, ~20x30 for accessories, 128x72 for rooms...).
 * Decoded with stb_image (public domain / MIT, see stb_image.h).
 */
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_HDR
#define STBI_NO_SIMD
#define STBI_NO_THREAD_LOCALS
#include "stb_image.h"

unsigned char *thumb_decode(const unsigned char *buf, int len, int *w, int *h)
{
    int n = 0;
    return stbi_load_from_memory(buf, len, w, h, &n, 4);
}

void thumb_free(unsigned char *px)
{
    stbi_image_free(px);
}
