# GPU integration of exported compute, capture and ordered feedback products.
# Run from the repository root with NDK/native Vulkan CC and glslc in PATH.
# METAL=--metal adds the shared Apple compute -> feedback-reader reference.
B ?= /tmp/nx-geometry-feedback
.PHONY: fixture shaders metal-reference
fixture: shaders
	$(CC) -O1 -Wall -Wextra -Werror -Wno-unused-function -DGC_EXPLICIT=1 -I$(B) dm/android/tests/geometry_feedback.c -o $(B)/geometry_feedback -lvulkan
shaders:
	python3 dm/android/tests/geometry_fixture.py $(B) --explicit --feedback $(METAL)
	glslc --target-env=vulkan1.1 dm/gfx/nvn/vulkan/feedback_gather.comp -o $(B)/feedback_gather.spv
	spirv-val --target-env vulkan1.1 $(B)/feedback_gather.spv
metal-reference:
	clang -O1 -Wall -Wextra -Werror -fobjc-arc -DGC_EXPLICIT=1 -I$(B) dm/gfx/tests/geometry_feedback_metal.m -framework Foundation -framework Metal -o $(B)/geometry_feedback_metal
