# From the repository root with glslc and SPIRV-Tools in PATH; CC is NDK/native Vulkan.
B ?= /tmp/nx-meshstage
.PHONY: fixture shaders
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/meshstage.c -o $(B)/meshstage -lvulkan
shaders:
	python3 dm/android/tests/meshstage_fixture.py $(B)
