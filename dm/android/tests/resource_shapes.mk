B ?= /tmp/nx-resource-shapes
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/resource_shapes.c -o $(B)/resource_shapes -lvulkan
shaders:
	python3 dm/android/tests/resource_shapes_fixture.py $(B)/wrapped wrapped
	python3 dm/android/tests/resource_shapes_fixture.py $(B)/size size
	python3 dm/android/tests/resource_shapes_fixture.py $(B)/slots slots
	python3 dm/android/tests/resource_shapes_fixture.py $(B)/many many
apple: shaders
	@for v in wrapped size slots many; do \
	  xcrun -sdk macosx metal -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $(B)/$$v/00000000feed6100.metal -o $(B)/$$v/shader.air && \
	  xcrun -sdk macosx metallib $(B)/$$v/shader.air -o $(B)/$$v/00000000feed6100.metallib || exit; \
	done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/resource_shapes_metal.m -framework Foundation -framework Metal -o $(B)/resource_shapes_metal
