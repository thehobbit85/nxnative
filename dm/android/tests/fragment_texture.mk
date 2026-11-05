B ?= /tmp/nx-fragment-texture
GLSLC ?= glslc
.PHONY: fixture shaders apple
fixture: shaders
	$(GLSLC) dm/android/tests/fragment_texture.vert -o $(B)/fullscreen.spv
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function dm/android/tests/fragment_texture.c -o $(B)/fragment_texture -lvulkan -lm
shaders:
	python3 dm/android/tests/fragment_texture_fixture.py $(B)
apple: shaders
	for source in $(B)/apple/*.metal; do xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$source -o $$source.air && xcrun -sdk macosx metallib $$source.air -o $${source%.metal}.metallib || exit 1; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -Wno-unused-function -fobjc-arc dm/android/tests/fragment_texture_metal.m -framework Foundation -framework Metal -o $(B)/fragment_texture_metal
