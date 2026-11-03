// arm64 bionic uses ELF symbols and getauxval() for optional instruction sets.
#include "../config.h"
#undef PREFIX
#undef HAVE_GETAUXVAL
#define HAVE_GETAUXVAL 1
