// Single translation unit that instantiates the vendored stb single-header libs.
// Compiled with warnings off (see CMakeLists) so upstream style never gates our
// -Wall -Wextra build. Our own code includes the headers WITHOUT the *_IMPLEMENTATION
// macro and links these symbols.
#define STB_TRUETYPE_IMPLEMENTATION
#include "stb_truetype.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
