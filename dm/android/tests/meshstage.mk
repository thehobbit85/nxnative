# From the repository root with glslc and SPIRV-Tools in PATH; CC is NDK/native Vulkan.
B ?= /tmp/nx-meshstage
.PHONY: fixture shaders apple
fixture: shaders
	$(CC) -std=c11 -O1 -Wall -Wextra -Werror dm/android/tests/meshstage.c -o $(B)/meshstage -lvulkan
shaders:
	python3 dm/android/tests/meshstage_fixture.py $(B)
apple: shaders
	python3 dm/android/tests/meshstage_fixture.py $(B) --apple
	for source in $(B)/*.native.metal; do xcrun -sdk macosx metal -std=metal3.1 -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$source -o $${source%.metal}.air && xcrun -sdk iphoneos metal -std=metal3.1 -target air64-apple-ios18.0 -fmodules-cache-path=$(B)/ios-modules -c $$source -o $${source%.metal}.ios.air || exit 1; done
	for source in $(B)/00000000feed001?.metal; do xcrun -sdk macosx metal -std=metal3.1 -target air64-apple-macos15.0 -fmodules-cache-path=$(B)/modules -c $$source -o $${source%.metal}.air && xcrun -sdk macosx metallib $${source%.metal}.air -o $${source%.metal}.metallib || exit 1; done
	clang -std=c11 -O1 -Wall -Wextra -Werror -fobjc-arc dm/android/tests/meshstage_metal.m -framework Foundation -framework Metal -o $(B)/meshstage_metal
