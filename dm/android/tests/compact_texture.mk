B ?= /tmp/nx-compact-texture
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/compact_texture.c -o $(B)/compact_texture -lvulkan -lm
shaders:
	python3 dm/android/tests/compact_texture_fixture.py $(B)
apple: shaders
	@for v in $(B)/*.metal; do \
	  xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$v -o $${v%.metal}.air && \
	  xcrun -sdk macosx metallib $${v%.metal}.air -o $${v%.metal}.metallib || exit; \
	done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/compact_texture_metal.m -framework Foundation -framework Metal -o $(B)/compact_texture_metal
