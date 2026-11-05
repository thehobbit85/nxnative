# From repository root; CC is an NDK compiler or a native Vulkan compiler.
# Add METAL=--metal to build the optional Apple reference corpus.
# VARIANT=--explicit DEFINES=-DGC_EXPLICIT=1 selects explicit emission/cuts.
B ?= /tmp/nx-geometry-compute
.PHONY: fixture shaders capture-fixture
capture-fixture: shaders
	$(CC) -O1 -Wall -Wextra -Werror dm/android/tests/geometry_capture.c -o $(B)/geometry_capture -lvulkan
fixture: shaders
	$(CC) -O1 -Wall -Wextra -Werror $(DEFINES) -I$(B) dm/android/tests/geometry_compute.c -o $(B)/geometry_compute -lvulkan
shaders:
	python3 dm/android/tests/geometry_fixture.py $(B) $(METAL) $(VARIANT)
.PHONY: metal-reference
metal-reference:
	clang -O1 -Wall -Wextra -Werror $(DEFINES) -fobjc-arc -I$(B) dm/gfx/tests/geometry_corpus_metal.m -framework Foundation -framework Metal -o $(B)/geometry_corpus_metal
