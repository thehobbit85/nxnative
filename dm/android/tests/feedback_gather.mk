# Run from the repository root with an NDK/native Vulkan CC and glslc in PATH.
# METAL=--metal builds the production Apple feedback reference for comparison.
B ?= /tmp/nx-feedback-gather
.PHONY: fixture shaders metal-reference
fixture: shaders
	$(CC) -O1 -Wall -Wextra -Werror -Wno-unused-function dm/android/tests/feedback_gather.c -o $(B)/feedback_gather -lvulkan
shaders:
	python3 dm/android/tests/feedback_fixture.py $(B) $(METAL)
metal-reference:
	clang -O1 -Wall -Wextra -Werror -Wno-unused-function -fobjc-arc dm/gfx/tests/feedback_corpus_metal.m -framework Foundation -framework Metal -o $(B)/feedback_corpus_metal
