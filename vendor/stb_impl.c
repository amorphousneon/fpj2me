/*
 * fpj2me — vendor/stb_impl.c : the ONE translation unit that emits the
 * stb_image implementation (PNG decoder only). Keep the macro block
 * byte-identical to the one in src/midp_image.c so both TUs agree on
 * the allocator and feature set.
 */
#include "libc_shims.h"

#define STBI_ONLY_PNG
#define STBI_NO_STDIO
#define STBI_NO_HDR
#define STBI_NO_LINEAR
#define STBI_NO_THREAD_LOCALS              /* bare metal, no TLS runtime: GCC16 arm-none-eabi defaults to emutls and libgcc's emutls.o wants malloc/abort */
#define STBI_FAILURE_STRINGS 0            /* keep strings out of .rodata */
#define STBI_MALLOC(s)     fp_alloc(s)
#define STBI_REALLOC(p,s)  fp_realloc(p,s)
#define STBI_FREE(p)       fp_free(p)
#define STBI_ASSERT(x)     ((void)0)
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
