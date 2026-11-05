B ?= /tmp/nx-c2-array
.PHONY: fixture shaders
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/c2_array.c -o $(B)/c2_array -lvulkan
shaders:
	python3 dm/android/tests/c2_array_fixture.py $(B)
.PHONY: apple
apple: shaders
	for variant in 0 1; do xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $(B)/00000000feed400$$variant.metal -o $(B)/c2_$$variant.air && xcrun -sdk macosx metallib $(B)/c2_$$variant.air -o $(B)/00000000feed400$$variant.metallib || exit 1; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc -I$(B) dm/android/tests/c2_array_metal.m -framework Foundation -framework Metal -o $(B)/c2_array_metal
