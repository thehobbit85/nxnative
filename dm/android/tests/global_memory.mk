B ?= /tmp/nx-global-memory
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/global_memory.c -o $(B)/global_memory -lvulkan
shaders:
	python3 dm/android/tests/global_memory_fixture.py $(B)
apple: shaders
	for source in $(B)/*.metal; do xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$source -o $${source%.metal}.air && xcrun -sdk macosx metallib $${source%.metal}.air -o $${source%.metal}.metallib || exit 1; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/global_memory_metal.m -framework Foundation -framework Metal -o $(B)/global_memory_metal
