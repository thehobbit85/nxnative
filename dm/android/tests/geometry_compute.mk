# From repository root; CC is an NDK compiler or a native Vulkan compiler.
# Add METAL=--metal to build the optional Apple reference corpus.
B ?= /tmp/nx-geometry-compute
.PHONY: fixture shaders
fixture: shaders
	$(CC) -O1 -Wall -Wextra -Werror -I$(B) dm/android/tests/geometry_compute.c -o $(B)/geometry_compute -lvulkan
shaders:
	python3 dm/android/tests/geometry_fixture.py $(B) $(METAL)
.PHONY: metal-reference
metal-reference:
	clang -O1 -Wall -Wextra -Werror -fobjc-arc -I$(B) dm/gfx/tests/geometry_corpus_metal.m -framework Foundation -framework Metal -o $(B)/geometry_corpus_metal
