# NDK cross-build or headless Linux replay with a local Vulkan driver.
.DEFAULT_GOAL := replay-test
HOST = ../../../host
NVN = ..
B ?= build
GAME_H = $(HOST)/gen/$(GAME)/game_profile.h
GLSLC ?= glslc
LIBS ?= -lvulkan -lpthread -lm
CPPFLAGS += -D_GNU_SOURCE -I$(HOST) -I$(HOST)/gen/$(GAME) -I../../../games/$(GAME) -I$(NVN) -I$(NVN)/include
CFLAGS += -O1 -Wall -Werror -Wno-unused-function
REPLAY_CPPFLAGS ?= -DDM_VK_DESCRIPTOR_POOL_DIVISOR=1024
SRC = tests/t_replay.c nvn_vk.c ../nvn_dirty.c ../nvn_tex.c ../../tex/nvn_deswizzle.c
CLEAR_SRC := .
CLEAR_GEN := $(B)/clear-shaders
include clear_shaders.mk
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: $(CLEAR_HEADERS) render_condition.h visibility.h uniform_table.inc primitive_replay.h ../nvn_primitive.h
CPPFLAGS += -I$(CLEAR_GEN)
.PHONY: geometry-topology-fixture
geometry-topology-fixture: $(B)/t_geometry_topology $(B)/topology.spv
$(B)/t_geometry_topology: tests/t_geometry_topology.c
	mkdir -p $(B)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
$(B)/topology.spv: ../../../tools/tests/test_android_geometry_compute.py ../../../android/tools/spv_geometry.py ../../../android/tools/spv_capture.py ../../../android/tools/spv_module.py
	mkdir -p $(B)
	GLSLC="$(GLSLC)" python3 ../../../tools/tests/test_android_geometry_compute.py --fixture $@
.PHONY: warp-width-fixture
warp-width-fixture: $(B)/t_replay
	GLSLC="$(GLSLC)" python3 ../../../tools/tests/test_android_warp_width.py --fixture "$(abspath $(B))/warp-shaders"
.PHONY: vertex-capture-fixture vertex-capture-test
vertex-capture-fixture: $(B)/t_shader_capture $(B)/capture.spv
vertex-capture-test: vertex-capture-fixture
	$(B)/t_shader_capture $(B)/capture.spv
$(B)/t_shader_capture: tests/t_shader_capture.c
	mkdir -p $(B)
	$(CC) $(CFLAGS) $< -o $@ $(LIBS)
$(B)/capture.spv: ../../../tools/tests/test_android_vertex_capture.py ../../../android/tools/spv_capture.py ../../../android/tools/spv_module.py ../dmsh_abi.h ../../tools/dmsh_geometry.py
	mkdir -p $(B)
	GLSLC="$(GLSLC)" python3 ../../../tools/tests/test_android_vertex_capture.py --fixture $@
# Optional Mac reference: compare metal-capture.bin with the Vulkan fixture's
# optional output file. Keep native fixture products apart from NDK binaries.
.PHONY: vertex-capture-metal-test
vertex-capture-metal-test: $(B)/t_shader_capture_metal $(B)/capture.metallib
	$(B)/t_shader_capture_metal $(B)/capture.metallib $(B)/metal-capture.bin
$(B)/t_shader_capture_metal: tests/t_shader_capture_metal.m
	mkdir -p $(B)
	clang -fobjc-arc -O1 -Wall -Wextra -Werror -framework Foundation -framework Metal $< -o $@
$(B)/capture.metal: ../../../tools/tests/test_android_vertex_capture.py ../../tools/dmsh_geometry.py ../../tools/dmsh_build.py
	mkdir -p $(B)
	GLSLC="$(GLSLC)" python3 ../../../tools/tests/test_android_vertex_capture.py --fixture $(B)/metal-input.spv --metal-fixture $@
$(B)/capture.metallib: $(B)/capture.metal
	xcrun -sdk macosx metal -std=macos-metal2.4 -fmodules-cache-path=$(B)/module-cache -c $< -o $(B)/capture.air
	xcrun -sdk macosx metallib $(B)/capture.air -o $@
.PHONY: replay-test fixture profile
.PHONY: resource-interface-fixture
resource-interface-fixture: fixture
	python3 ../../../android/tests/resource_shapes_fixture.py $(abspath $(B))/resource-many many
	cp $(B)/resource-many/export/00000000feed6100.spv $(B)/resource-many/export/00000000feed6100.vk.txt $(B)/shaders/
	python3 tests/stamp_shader_contract.py $(B)/shaders
$(B)/t_present_rect: tests/t_present_rect.c present_rect.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) $< -lm -o $@
replay-test: fixture $(B)/t_present_rect
	$(B)/t_present_rect
	DM_VK_DIR="$(abspath $(B))/shaders" $(B)/t_replay
fixture: $(B)/t_replay $(B)/shaders/0000000000000001.spv $(B)/shaders/0000000000000002.spv \
         $(B)/shaders/0000000000000003.spv $(B)/shaders/0000000000000004.spv $(B)/shaders/0000000000000021.spv $(B)/shaders/0000000000000022.spv $(B)/shaders/0000000000000031.spv $(B)/shaders/0000000000000032.spv \
         $(B)/shaders/0000000000000033.spv $(B)/shaders/0000000000000034.spv $(B)/shaders/0000000000000035.spv $(B)/shaders/0000000000000036.spv \
         $(B)/shaders/0000000000000037.spv $(B)/shaders/0000000000000038.spv \
         $(B)/shaders/0000000000000039.spv $(B)/shaders/000000000000003a.spv $(B)/shaders/0000000000000040.spv \
         $(B)/shaders/0000000000000041.spv $(B)/shaders/0000000000000042.spv $(B)/shaders/0000000000000043.spv \
         $(B)/shaders/0000000000000044.spv $(B)/shaders/0000000000000045.spv $(B)/shaders/0000000000000046.spv $(B)/shaders/0000000000000047.spv $(B)/shaders/0000000000000048.spv $(B)/shaders/0000000000000049.spv $(B)/shaders/000000000000004a.spv \
         $(B)/shaders/000000000000004b.spv $(B)/shaders/000000000000004c.spv \
         $(B)/shaders/000000000000004d.spv $(B)/shaders/000000000000004e.spv $(B)/shaders/000000000000004f.spv $(B)/shaders/0000000000000050.spv $(B)/shaders/0000000000000051.spv \
         $(B)/shaders/0000000000000052.spv $(B)/shaders/0000000000000053.spv $(B)/shaders/0000000000000054.spv $(B)/shaders/0000000000000055.spv \
         $(B)/shaders/0000000000000056.spv $(B)/shaders/0000000000000057.spv $(B)/shaders/0000000000000058.spv $(B)/shaders/0000000000000059.spv $(B)/shaders/000000000000005a.spv $(B)/shaders/000000000000005b.spv
	python3 tests/stamp_shader_contract.py $(B)/shaders
$(B)/shaders/000000000000005b.spv: tests/packed_streams.vert tests/packed_streams.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/packed_streams.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000005a.spv: tests/restart.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage vs' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000059.spv: $(B)/shaders/0000000000000058.spv tests/handle_table_driver.vk.txt
	cp $< $@
	cp tests/handle_table_driver.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000058.spv: tests/bindless.frag tests/handle_table.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DHANDLE_TABLE $< -o $@
	cp tests/handle_table.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000056.spv: tests/instance_divisor.vert tests/instance_divisor.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/instance_divisor.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000057.spv: tests/instance_divisor.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000055.spv: tests/blend_mrt.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000054.spv: tests/one_sample.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000052.spv: tests/depth_range.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage vs' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000053.spv: tests/depth_range.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/000000000000004a.spv: tests/texel.vert tests/texel.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/texel.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000049.spv: tests/texel.frag tests/texel.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/texel.frag.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000048.spv: tests/texel.comp tests/texel.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/texel.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000044.spv: tests/separate.frag tests/separate.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/separate.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000045.spv: $(B)/shaders/0000000000000044.spv tests/separate_ubo.vk.txt
	cp $< $@
	cp tests/separate_ubo.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000046.spv: $(B)/shaders/0000000000000044.spv tests/separate_tex.vk.txt
	cp $< $@
	cp tests/separate_tex.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000047.spv: $(B)/shaders/0000000000000044.spv tests/separate_id.vk.txt
	cp $< $@
	cp tests/separate_id.vk.txt $(@:.spv=.vk.txt)
profile:
	$(MAKE) -C $(HOST) GAME=$(GAME) game_profile
$(B)/shaders/0000000000000040.spv: tests/buffer.comp tests/buffer.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/buffer.vk.txt $(@:.spv=.vk.txt)
$(B)/t_replay: $(SRC) tests/resource_shapes.inc ../../../android/tests/resource_shapes_cases.h tests/driver_c2.inc tests/split_handle.inc tests/uniform_storage.inc spv_vertex_fetch.h spv_scaled32.h spv_output_defaults.h spv_draw_parameters.h spv_buffer_bias.h shader_contract.h warp_width.h geometry_contract.h meshstage_contract.h uniform_address.h present_rect.h split_handle.h driver_c2.h driver_c0.h resource_contract.h ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_dirty.h ../nvn_tex.h ../nvn_refl.h ../../../third_party/bcdec/bcdec.h $(GAME_H) | profile
	mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(REPLAY_CPPFLAGS) $(CFLAGS) $(SRC) -o $@ $(LIBS)
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

$(B)/shaders/0000000000000033.spv: tests/output_defaults.vert tests/output_defaults.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/output_defaults.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000034.spv: tests/output_defaults.frag tests/output_defaults.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/output_defaults.frag.vk.txt $(@:.spv=.vk.txt)

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
$(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: ../../tests/t_recorded_transfer.c ../hle_nvn.c ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_refl.h spv_vertex_fetch.h spv_scaled32.h spv_output_defaults.h spv_draw_parameters.h spv_buffer_bias.h driver_c0.h resource_contract.h $(RECORDER_SRC) ../nvn_asm.h ../../../android/compat/sha256.c ../../../android/compat/CommonCrypto/CommonDigest.h $(GAME_H) | profile
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

$(B)/shaders/0000000000000035.spv: tests/depth_copy.vert tests/depth_copy.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/depth_copy.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000036.spv: tests/depth_copy.frag tests/depth_copy.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/depth_copy.frag.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000037.spv: tests/origin.vert tests/origin.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/origin.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000038.spv: tests/origin.frag tests/origin.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/origin.frag.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000039.spv: tests/draw_parameters.vert tests/draw_parameters.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/draw_parameters.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000003a.spv: tests/draw_parameters.frag tests/draw_parameters.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/draw_parameters.frag.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000041.spv: tests/storage.vert tests/storage.vert.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/storage.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000042.spv: tests/storage.frag tests/storage.frag.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/storage.frag.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000043.spv: tests/alias.comp tests/alias.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/alias.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/000000000000004b.spv: $(B)/shaders/0000000000000041.spv tests/uniform_storage.vert.vk.txt
	cp $< $@
	cp tests/uniform_storage.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000004c.spv: $(B)/shaders/0000000000000042.spv tests/uniform_storage.frag.vk.txt
	cp $< $@
	cp tests/uniform_storage.frag.vk.txt $(@:.spv=.vk.txt)

# This small host tool validates buffer-block rebasing without an Android device.
BUILD_CC ?= cc
SPIRV_VAL ?= spirv-val
$(B)/t_buffer_bias: tests/t_buffer_bias.c spv_buffer_bias.h
	mkdir -p $(B)
	$(BUILD_CC) -O1 -g -Wall -Werror $(BUILD_CFLAGS) $< -o $@
.PHONY: buffer-bias-check
buffer-bias-check: $(B)/t_buffer_bias $(B)/shaders/0000000000000043.spv
	$(B)/t_buffer_bias $(B)/shaders/0000000000000043.spv $(B)/buffer-biased.spv
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/buffer-biased.spv

$(B)/shaders/000000000000004d.spv: tests/split_handle.frag tests/split_combined.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DSEPARATE=0 $< -o $@
	cp tests/split_combined.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000004e.spv: tests/split_handle.frag tests/split_separate.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DSEPARATE=1 $< -o $@
	cp tests/split_separate.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/000000000000004f.spv: $(B)/shaders/000000000000004d.spv tests/split_fixed.vk.txt
	cp $< $@
	cp tests/split_fixed.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000050.spv: tests/driver_c2.comp tests/driver_c2.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/driver_c2.vk.txt $(@:.spv=.vk.txt)

$(B)/shaders/0000000000000051.spv: tests/c2_sample.frag tests/c2_sample.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/c2_sample.vk.txt $(@:.spv=.vk.txt)
