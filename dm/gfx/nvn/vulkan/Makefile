# NDK cross-build or headless Linux replay with a local Vulkan driver.
HOST = ../../../host
NVN = ..
B ?= build
GAME_H = $(HOST)/gen/$(GAME)/game_profile.h
GLSLC ?= glslc
LIBS ?= -lvulkan -lpthread -lm
CPPFLAGS += -D_GNU_SOURCE -I$(HOST) -I$(HOST)/gen/$(GAME) -I../../../games/$(GAME) -I$(NVN) -I$(NVN)/include
CFLAGS += -O1 -Wall -Werror -Wno-unused-function
SRC = tests/t_replay.c nvn_vk.c ../nvn_dirty.c ../nvn_tex.c ../../tex/nvn_deswizzle.c
.PHONY: replay-test fixture profile
replay-test: fixture
	DM_VK_DIR="$(abspath $(B))/shaders" $(B)/t_replay
fixture: $(B)/t_replay $(B)/shaders/0000000000000001.spv $(B)/shaders/0000000000000002.spv \
         $(B)/shaders/0000000000000003.spv $(B)/shaders/0000000000000004.spv $(B)/shaders/0000000000000021.spv $(B)/shaders/0000000000000022.spv $(B)/shaders/0000000000000031.spv $(B)/shaders/0000000000000032.spv
profile:
	$(MAKE) -C $(HOST) GAME=$(GAME) game_profile
$(B)/t_replay: $(SRC) spv_vertex_fetch.h spv_scaled32.h ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_dirty.h ../nvn_tex.h ../nvn_refl.h ../../../third_party/bcdec/bcdec.h $(GAME_H) | profile
	mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) $(SRC) -o $@ $(LIBS)
$(GAME_H): | profile
$(B)/shaders/0000000000000001.spv: tests/triangle.vert tests/vertex.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/vertex.vk.txt $(B)/shaders/0000000000000001.vk.txt
$(B)/shaders/0000000000000002.spv: tests/triangle.frag tests/fragment.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/fragment.vk.txt $(B)/shaders/0000000000000002.vk.txt

$(B)/shaders/0000000000000003.spv: tests/image.comp tests/compute.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/compute.vk.txt $(B)/shaders/0000000000000003.vk.txt

$(B)/shaders/0000000000000021.spv: tests/numeric.vert tests/numeric.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/numeric.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000022.spv: tests/numeric.frag tests/numeric.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/numeric.frag.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000004.spv: tests/groups.comp tests/groups.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/groups.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000031.spv: tests/bindless.frag tests/bindless.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/bindless.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000032.spv: tests/driver.comp tests/driver.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/driver.vk.txt $(@:.spv=.vk.txt)

# For a Mac build, point VULKAN_SDK at the Mac SDK
# with an offscreen driver installed, or provide RECORDER_CPPFLAGS and LIBS for a local loader.
RECORDER_CPPFLAGS = $(if $(VULKAN_SDK),-I$(VULKAN_SDK)/include)
RECORDER_LDFLAGS = $(if $(VULKAN_SDK),-L$(VULKAN_SDK)/lib -Xlinker -rpath -Xlinker $(VULKAN_SDK)/lib)
RECORDER_SRC = $(filter-out tests/t_replay.c,$(SRC)) tests/recorder_unavailable.c \
               ../../../host/crash.c ../../../host/hle_bigstack.c ../../tests/t_host_stubs.c ../nvn_entry.S ../nvn_thunks.S
.PHONY: recorded-transfer-test recorded-transfer-conformance
recorded-transfer-test: $(B)/t_recorded_transfer
	$(B)/t_recorded_transfer
recorded-transfer-conformance: recorded-transfer-test
	$(MAKE) -C ../../tests GAME=$(GAME) check_t_recorded_transfer
$(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: ../../tests/t_recorded_transfer.c ../hle_nvn.c ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_refl.h spv_vertex_fetch.h spv_scaled32.h $(RECORDER_SRC) ../nvn_asm.h ../../../android/compat/sha256.c ../../../android/compat/CommonCrypto/CommonDigest.h $(GAME_H) | profile
	mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(RECORDER_CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(RECORDER_SRC) $(RECORDER_EXTRA) $(RECORDER_LDFLAGS) -o $@ $(LIBS)

# The Android SHA implementation is the loader's as well. The driver and ANativeWindow
# are linked from the NDK. Use the device's Vulkan loader when executing the fixture.
.PHONY: android-recorder
android-recorder: RECORDER_CPPFLAGS = -I../../../android/compat
android-recorder: RECORDER_EXTRA = ../../../android/compat/sha256.c
android-recorder: CFLAGS += -fPIC -fvisibility=hidden -Wno-format -ffunction-sections -fdata-sections
android-recorder: RECORDER_LDFLAGS = -Wl,--gc-sections -Wl,--no-undefined -Wl,-z,max-page-size=16384
android-recorder: LIBS = -lvulkan -landroid -lm -ldl
android-recorder: $(B)/t_recorded_transfer_android

# Portable capture-file support, shared by the Linux and bionic builds.
LZFSE = ../../../third_party/lzfse
$(B)/t_capture: ../../tests/t_capture.c ../nvn_capture.c ../nvn_capture.h ../nvn_obj.h $(wildcard $(LZFSE)/*.[ch]) | profile
	mkdir -p $(B)
	$(CC) $(CPPFLAGS) -O2 -o $@ ../../tests/t_capture.c ../nvn_capture.c $(wildcard $(LZFSE)/*.c)
.PHONY: portable-capture-test
portable-capture-test: $(B)/t_capture
	$(B)/t_capture
