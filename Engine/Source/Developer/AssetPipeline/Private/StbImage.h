#pragma once

// Every translation unit must see the same configuration because it changes stb_image's declarations.
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_HDR
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#include <stb_image.h>
