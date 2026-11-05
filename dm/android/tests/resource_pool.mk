B ?= /tmp/nx-resource-pool
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/resource_pool.c -o $(B)/resource_pool -lvulkan
shaders:
	python3 dm/android/tests/resource_pool_fixture.py $(B)
apple: shaders
	xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $(B)/00000000feed6000.metal -o $(B)/pool.air
	xcrun -sdk macosx metallib $(B)/pool.air -o $(B)/00000000feed6000.metallib
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/resource_pool_metal.m -framework Foundation -framework Metal -o $(B)/resource_pool_metal
