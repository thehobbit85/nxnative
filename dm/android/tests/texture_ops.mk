B ?= /tmp/nx-texture-ops
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/texture_ops.c -o $(B)/texture_ops -lvulkan -lm
shaders:
	python3 dm/android/tests/texture_ops_fixture.py $(B)
apple: shaders
	for source in $(B)/*.metal; do xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$source -o $$source.air && xcrun -sdk macosx metallib $$source.air -o $${source%.metal}.metallib || exit 1; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/texture_ops_metal.m -framework Foundation -framework Metal -o $(B)/texture_ops_metal
