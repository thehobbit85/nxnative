B ?= /tmp/nx-constant-banks
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/constant_banks.c -o $(B)/constant_banks -lvulkan
shaders:
	python3 dm/android/tests/constant_banks_fixture.py $(B)
apple: shaders
	@for v in $(B)/*.metal; do \
	  xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$v -o $${v%.metal}.air && \
	  xcrun -sdk macosx metallib $${v%.metal}.air -o $${v%.metal}.metallib || exit; \
	done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/constant_banks_metal.m -framework Foundation -framework Metal -o $(B)/constant_banks_metal
