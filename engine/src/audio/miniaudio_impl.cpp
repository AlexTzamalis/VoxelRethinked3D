// The one file that compiles miniaudio, with stb_vorbis for ogg files. The
// order is the one miniaudio asks for: the vorbis declarations, then miniaudio,
// then the vorbis code.

#define STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>

#define MINIAUDIO_IMPLEMENTATION
#define MA_NO_ENCODING
#define MA_NO_GENERATION
#include <miniaudio.h>

#undef STB_VORBIS_HEADER_ONLY
#include <stb_vorbis.c>
