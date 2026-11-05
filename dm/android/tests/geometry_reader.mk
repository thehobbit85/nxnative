# Run from the repository root. CC is the NDK target compiler; tools need
# glslc, spirv-opt, spirv-val and the locked DMSH_TEST_XLATE translator.
B ?= /tmp/nx-geometry-reader
GLSLC ?= glslc
.PHONY: fixture shaders
fixture: $(B)/geometry_reader shaders
$(B)/geometry_reader: dm/android/tests/geometry_reader.c
	mkdir -p $(B)
	$(CC) -O1 -Wall -Wextra -Werror $< -o $@ -lvulkan
shaders:
	GLSLC="$(GLSLC)" python3 dm/tools/tests/test_android_geometry_compute.py --reader-fixture $(B)
