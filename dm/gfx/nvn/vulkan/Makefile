# NDK cross-build or headless Linux replay with a local Vulkan driver.
.DEFAULT_GOAL := replay-test
HOST = ../../../host
NVN = ..
B ?= build
$(B)/t_replay: tests/wrapped_buffer.inc
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: wrapped_buffer.inc spv_wrapped_buffer.h block_targets.inc block_targets_finish.inc native_reduction.inc diagnostics.inc diagnostic_targets.inc ../../../third_party/flip/tool/stb_image_write.h
$(B)/t_wrapped_buffer: tests/t_wrapped_buffer.c spv_wrapped_buffer.h spv_texel_bias.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) $< -o $@
fixture: $(B)/shaders/00000000feed9e01.spv
$(B)/shaders/00000000feed9e01.spv: tests/wrapped_buffer.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 1 tex 0 -1 samp_tex 0 view=1 num=2 fetch1d=1\nb 1 ssbo 1 sbuf 0 -1 - -1\nb 2 ssbo 1 sbuf 1 -1 - -1\nb 3 cis 1 tex 1 -1 samp_tex 1\n' > $(@:.spv=.vk.txt)
fixture: $(B)/shaders/00000000feed9e02.spv $(B)/shaders/00000000feed9e03.spv
$(B)/shaders/00000000feed9e02.spv: tests/wrapped_buffer_array.comp Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 3 tex_ubo 7 16 samp_tex 0 view=1 num=2\nb 1 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/00000000feed9e03.spv: tests/wrapped_buffer_array.comp Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 -DSEPARATE $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 tex 3 tex_ubo 7 16 - -1 view=1 num=2\nb 1 ssbo 1 sbuf 0 -1 - -1\nb 2 samp 1 samp 0 -1 - -1\n' > $(@:.spv=.vk.txt)
fixture: $(B)/shaders/00000000feed9e04.spv
$(B)/shaders/00000000feed9e04.spv: tests/wrapped_buffer_table.comp Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 8192 tex_htab 7 0 samp_htab 8 runtime=1 pool_slot=0 soff=0 view=1 num=2\nb 1 ssbo 1 sbuf 0 -1 - -1\nb 2 ssbo 1 htab_map 0 -1 - -1 ro\n' > $(@:.spv=.vk.txt)
$(B)/t_replay: tests/block_targets.inc tests/diagnostics.inc ../../../third_party/flip/tool/stb_image.h
fixture: $(B)/shaders/00000000feed9f01.spv $(B)/shaders/00000000feed9f02.spv
$(B)/shaders/00000000feed9f01.spv: tests/block_target.frag Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 ubo 1 cbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/00000000feed9f02.spv: tests/block_target_sample.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 1 tex 0 -1 samp_tex 0 view=1 num=0\nb 1 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/t_replay: tests/layered_attachments.inc
$(B)/t_replay: tests/geometry_replay.inc
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: geometry_replay.inc ../nvn_geometry.h
.PHONY: geometry-replay-fixture
geometry-replay-fixture: fixture
	GLSLC="$(GLSLC)" python3 tests/geometry_fixture.py "$(B)/shaders"
fixture: $(B)/shaders/00000000feed9b01.spv $(B)/shaders/00000000feed9b02.spv $(B)/shaders/00000000feed9b03.spv
$(B)/shaders/00000000feed9b02.spv: LAYER_FLAGS = -DSINGLE_LAYER
$(B)/shaders/00000000feed9b01.spv $(B)/shaders/00000000feed9b02.spv: tests/layered.vert
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $(LAYER_FLAGS) $< -o $@
	printf 'stage vs\n' > $(@:.spv=.vk.txt)
$(B)/shaders/00000000feed9b03.spv: tests/layered.frag
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\n' > $(@:.spv=.vk.txt)
GAME_H = $(HOST)/gen/$(GAME)/game_profile.h
GLSLC ?= glslc
LIBS ?= -lvulkan -lpthread -lm
CPPFLAGS += -D_GNU_SOURCE -I$(HOST) -I$(HOST)/gen/$(GAME) -I../../../games/$(GAME) -I$(NVN) -I$(NVN)/include
CFLAGS += -O1 -Wall -Werror -Wno-unused-function
REPLAY_CPPFLAGS ?= -DDM_VK_DESCRIPTOR_POOL_DIVISOR=1024
SRC = tests/t_replay.c nvn_vk.c ../nvn_dirty.c ../nvn_tex.c ../../tex/nvn_deswizzle.c
$(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: ../../tests/recorded_draw_texture.h ../../tests/recorded_sampler_reduction.h ../../tests/recorded_sparse_texture.h ../../tests/recorded_query_lifetime.h ../../tests/recorded_feedback_control.h
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: spv_storage_texels.inc spv_sampler_pairs.inc spv_one_row.h ../nvn_border.h spv_alpha.h spv_integer_outputs.h spv_fragment_outputs.h
$(B)/t_replay: ../../tests/packed_texel_corpus.h ../../tests/dlss_spatial_corpus.h ../../tests/dlss_temporal_corpus.h ../../tests/recorded_list.h ../../tests/polygon_offset_corpus.h ../../tests/polygon_fill_corpus.h ../../tests/alpha_coverage_corpus.h tests/alpha.inc tests/depth_views.inc tests/cube_ranges.inc tests/logic.inc tests/feedback_capture.inc
CLEAR_SRC := .
CLEAR_GEN := $(B)/clear-shaders
include clear_shaders.mk
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: $(CLEAR_HEADERS) transform_feedback.inc feedback_gather.h render_condition.h visibility.h uniform_table.inc attachment_feedback.inc buffer_image_transfer.inc stored_transfer.inc window_draw.inc image_copy_draw.inc temporal_upscale.inc draw_texture.inc packed_depth.inc public_texels.inc sparse_upload.inc primitive_replay.h ../nvn_primitive.h
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
         $(B)/shaders/0000000000000056.spv $(B)/shaders/0000000000000057.spv $(B)/shaders/0000000000000058.spv $(B)/shaders/0000000000000059.spv $(B)/shaders/000000000000005a.spv $(B)/shaders/000000000000005b.spv \
         $(B)/shaders/be5c489b134ec370.spv $(B)/shaders/9954f8894fadd956.spv $(B)/shaders/ee208c00f07f83b4.spv $(B)/shaders/7fe604cfab645fc7.spv \
         $(B)/shaders/98076dcbe001b8d9.spv $(B)/shaders/0f505d6a0231e82b.spv $(B)/shaders/ea957dc56076e186.spv $(B)/shaders/000000000000005c.spv $(B)/shaders/000000000000005d.spv \
         $(B)/shaders/000000000000005e.spv $(B)/shaders/000000000000005f.spv $(B)/shaders/c7bfdf71f80f7dd4.spv $(B)/shaders/0000000000000060.spv \
         $(B)/shaders/0000000000000061.spv $(B)/shaders/0000000000000062.spv $(B)/shaders/0000000000000063.spv \
         $(B)/shaders/0000000000000064.spv $(B)/shaders/0000000000000065.spv $(B)/shaders/0000000000000066.spv $(B)/shaders/0000000000000067.spv $(B)/shaders/0000000000000068.spv $(B)/shaders/0000000000000069.spv $(B)/shaders/000000000000006a.spv
	python3 tests/stamp_shader_contract.py $(B)/shaders
# Public recorder keys hash a 0x880-byte guest control section and one code byte per fixture stage.
fixture: $(B)/shaders/de216783c4f70605.vc.spv
$(B)/shaders/de216783c4f70605.spv: tests/feedback_capture.vert
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage vs\nb 0 ssbo 1 sbuf 3 -1 - -1\ntf 0 20 0 3 3 1 0 1\ntf 2 8 0 2\n' > $(@:.spv=.vk.txt)
$(B)/shaders/de216783c4f70605.vc.spv: $(B)/shaders/de216783c4f70605.spv tests/feedback_capture_shader.py ../../../android/tools/spv_capture.py
	python3 tests/feedback_capture_shader.py $< $@
$(B)/shaders/be5c489b134ec370.spv: $(B)/shaders/0000000000000035.spv
	cp $< $@
	cp $(<:.spv=.vk.txt) $(@:.spv=.vk.txt)
$(B)/shaders/c7bfdf71f80f7dd4.spv: tests/public_bindings.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 cis 1 tex_ubo 7 0 samp_ubo 8 soff=4 view=1 num=0\n' > $(@:.spv=.vk.txt)
$(B)/shaders/9954f8894fadd956.spv: tests/public_bindings.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 cis 1 tex 3 -1 samp_tex 3\n' > $(@:.spv=.vk.txt)
$(B)/shaders/ee208c00f07f83b4.spv: tests/public_bindings.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DSEPARATE $< -o $@
	printf 'stage ps\nb 0 tex 1 tex 131 -1 - -1 view=1 num=0\nb 1 samp 1 samp 7 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/7fe604cfab645fc7.spv: tests/public_cube.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 cis 1 tex 3 -1 samp_tex 3 view=3 num=0\nb 1 cis 1 tex 4 -1 samp_tex 4 view=3 num=0\n' > $(@:.spv=.vk.txt)
$(B)/shaders/98076dcbe001b8d9.spv: tests/public_buffers.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage vs\nb 0 ubo 1 cbuf 7 -1 - -1\nb 1 ssbo 1 sbuf 3 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/0f505d6a0231e82b.spv: tests/public_buffers.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 ubo 1 cbuf 7 -1 - -1\nb 1 ssbo 1 sbuf 3 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/ea957dc56076e186.spv: tests/public_buffers.comp
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 ubo 1 cbuf 7 -1 - -1\nb 1 ssbo 1 sbuf 3 -1 - -1\nb 2 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
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
$(B)/shaders/0000000000000055.spv $(B)/shaders/0000000000000066.spv: tests/blend_mrt.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DSECOND_LOCATION=$(if $(filter %66.spv,$@),2,1) $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000054.spv: tests/one_sample.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/000000000000005e.spv: tests/image_order.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 simg 1 img 3 -1 - -1 view=1 num=1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/000000000000005f.spv: tests/image_order.comp
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 simg 1 img 3 -1 - -1 view=1 num=1\nb 1 simg 1 img 4 -1 - -1 ro view=1 num=1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/000000000000005d.spv: tests/feedback.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage ps\nb 0 cis 1 tex 0 -1 samp_tex 0 view=1 num=0\nb 1 cis 1 tex 1 -1 samp_tex 1 view=1 num=0\n' > $(@:.spv=.vk.txt)
$(B)/shaders/000000000000005c.spv: tests/depth_clamp.vert tests/origin.vert.vk.txt Makefile
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/origin.vert.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000052.spv: tests/depth_range.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage vs' > $(@:.spv=.vk.txt)
fixture: $(B)/shaders/000000000000006d.spv
fixture: $(B)/shaders/000000000000006e.spv $(B)/shaders/000000000000006f.spv
$(B)/t_replay: tests/storage_views.inc tests/bc_views.inc
$(B)/t_replay: tests/empty_images.inc
$(B)/t_replay: tests/empty_ms.inc
$(B)/t_replay: tests/subresource_views.inc
$(B)/t_replay: tests/sampler_reduction.inc
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: spv_sample_reduction.h
$(B)/shaders/0000000000000075.spv: REDUCTION_FLAGS =
$(B)/shaders/0000000000000076.spv: REDUCTION_FLAGS = -DSEPARATE
$(B)/shaders/0000000000000077.spv: REDUCTION_FLAGS = -DARRAY
$(B)/shaders/0000000000000078.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE
$(B)/shaders/0000000000000079.spv: REDUCTION_FLAGS = -DGRADIENTS
$(B)/shaders/000000000000007a.spv: REDUCTION_FLAGS = -DSEPARATE -DGRADIENTS
$(B)/shaders/000000000000007b.spv: REDUCTION_FLAGS = -DARRAY -DGRADIENTS
$(B)/shaders/000000000000007c.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DGRADIENTS
$(B)/shaders/000000000000007d.spv: REDUCTION_FLAGS = -DOFFSET
$(B)/shaders/000000000000007e.spv: REDUCTION_FLAGS = -DSEPARATE -DOFFSET
$(B)/shaders/000000000000007f.spv: REDUCTION_FLAGS = -DARRAY -DOFFSET
$(B)/shaders/0000000000000080.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DOFFSET
$(B)/shaders/0000000000000081.spv: REDUCTION_FLAGS = -DGRADIENTS -DOFFSET
$(B)/shaders/0000000000000082.spv: REDUCTION_FLAGS = -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/0000000000000083.spv: REDUCTION_FLAGS = -DARRAY -DGRADIENTS -DOFFSET
$(B)/shaders/0000000000000084.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/0000000000000085.spv: REDUCTION_FLAGS = -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/0000000000000086.spv: REDUCTION_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/0000000000000087.spv: REDUCTION_FLAGS = -DARRAY -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/0000000000000088.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/0000000000000089.spv: REDUCTION_FLAGS = -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/000000000000008a.spv: REDUCTION_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/000000000000008b.spv: REDUCTION_FLAGS = -DARRAY -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/000000000000008c.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/000000000000008d.spv: REDUCTION_FLAGS = -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/000000000000008e.spv: REDUCTION_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/000000000000008f.spv: REDUCTION_FLAGS = -DARRAY -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/0000000000000090.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/0000000000000091.spv: REDUCTION_FLAGS = -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/0000000000000092.spv: REDUCTION_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/0000000000000093.spv: REDUCTION_FLAGS = -DARRAY -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/0000000000000094.spv: REDUCTION_FLAGS = -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/0000000000000095.spv: REDUCTION_FLAGS = -DPROJECTED
$(B)/shaders/0000000000000096.spv: REDUCTION_FLAGS = -DPROJECTED -DSEPARATE
$(B)/shaders/0000000000000099.spv: REDUCTION_FLAGS = -DPROJECTED -DGRADIENTS
$(B)/shaders/000000000000009a.spv: REDUCTION_FLAGS = -DPROJECTED -DSEPARATE -DGRADIENTS
$(B)/shaders/000000000000009d.spv: REDUCTION_FLAGS = -DPROJECTED -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/000000000000009e.spv: REDUCTION_FLAGS = -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000000000a1.spv: REDUCTION_FLAGS = -DPROJECTED -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000000000a2.spv: REDUCTION_FLAGS = -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000000000a5.spv: REDUCTION_FLAGS = -DQUERY -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000000000a6.spv: REDUCTION_FLAGS = -DQUERY -DFRAGMENT -fshader-stage=fragment -DSEPARATE
$(B)/shaders/00000000000000a7.spv: REDUCTION_FLAGS = -DQUERY -DFRAGMENT -fshader-stage=fragment -DARRAY
$(B)/shaders/00000000000000a8.spv: REDUCTION_FLAGS = -DQUERY -DFRAGMENT -fshader-stage=fragment -DARRAY -DSEPARATE
REDUCTION_SHADERS = $(B)/shaders/0000000000000075.spv $(B)/shaders/0000000000000076.spv $(B)/shaders/0000000000000077.spv $(B)/shaders/0000000000000078.spv \
    $(B)/shaders/0000000000000079.spv $(B)/shaders/000000000000007a.spv $(B)/shaders/000000000000007b.spv $(B)/shaders/000000000000007c.spv \
    $(B)/shaders/000000000000007d.spv $(B)/shaders/000000000000007e.spv $(B)/shaders/000000000000007f.spv $(B)/shaders/0000000000000080.spv \
    $(B)/shaders/0000000000000081.spv $(B)/shaders/0000000000000082.spv $(B)/shaders/0000000000000083.spv $(B)/shaders/0000000000000084.spv \
    $(B)/shaders/0000000000000085.spv $(B)/shaders/0000000000000086.spv $(B)/shaders/0000000000000087.spv $(B)/shaders/0000000000000088.spv \
    $(B)/shaders/0000000000000089.spv $(B)/shaders/000000000000008a.spv $(B)/shaders/000000000000008b.spv $(B)/shaders/000000000000008c.spv \
    $(B)/shaders/000000000000008d.spv $(B)/shaders/000000000000008e.spv $(B)/shaders/000000000000008f.spv $(B)/shaders/0000000000000090.spv \
    $(B)/shaders/0000000000000091.spv $(B)/shaders/0000000000000092.spv $(B)/shaders/0000000000000093.spv $(B)/shaders/0000000000000094.spv
REDUCTION_SHADERS += $(B)/shaders/0000000000000095.spv $(B)/shaders/0000000000000096.spv $(B)/shaders/0000000000000099.spv $(B)/shaders/000000000000009a.spv $(B)/shaders/000000000000009d.spv $(B)/shaders/000000000000009e.spv $(B)/shaders/00000000000000a1.spv $(B)/shaders/00000000000000a2.spv
REDUCTION_SHADERS += $(B)/shaders/00000000000000a5.spv $(B)/shaders/00000000000000a6.spv $(B)/shaders/00000000000000a7.spv $(B)/shaders/00000000000000a8.spv
$(B)/shaders/00000000feed97b1.spv: REDUCTION_FLAGS = -DVOLUME
$(B)/shaders/00000000feed97b2.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE
$(B)/shaders/00000000feed97b3.spv: REDUCTION_FLAGS = -DVOLUME -DGRADIENTS
$(B)/shaders/00000000feed97b4.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed97b5.spv: REDUCTION_FLAGS = -DVOLUME -DOFFSET
$(B)/shaders/00000000feed97b6.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DOFFSET
$(B)/shaders/00000000feed97b7.spv: REDUCTION_FLAGS = -DVOLUME -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed97b8.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed97b9.spv: REDUCTION_FLAGS = -DVOLUME -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed97ba.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed97bb.spv: REDUCTION_FLAGS = -DVOLUME -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed97bc.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed97bd.spv: REDUCTION_FLAGS = -DVOLUME -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed97be.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed97bf.spv: REDUCTION_FLAGS = -DVOLUME -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed97c0.spv: REDUCTION_FLAGS = -DVOLUME -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed97c1.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED
$(B)/shaders/00000000feed97c2.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DSEPARATE
$(B)/shaders/00000000feed97c3.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DGRADIENTS
$(B)/shaders/00000000feed97c4.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed97c5.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed97c6.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed97c7.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed97c8.spv: REDUCTION_FLAGS = -DVOLUME -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed97c9.spv: REDUCTION_FLAGS = -DVOLUME -DQUERY -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed97ca.spv: REDUCTION_FLAGS = -DVOLUME -DQUERY -DFRAGMENT -fshader-stage=fragment -DSEPARATE
REDUCTION_SHADERS += $(B)/shaders/00000000feed97b1.spv $(B)/shaders/00000000feed97b2.spv $(B)/shaders/00000000feed97b3.spv $(B)/shaders/00000000feed97b4.spv $(B)/shaders/00000000feed97b5.spv $(B)/shaders/00000000feed97b6.spv $(B)/shaders/00000000feed97b7.spv $(B)/shaders/00000000feed97b8.spv $(B)/shaders/00000000feed97b9.spv $(B)/shaders/00000000feed97ba.spv $(B)/shaders/00000000feed97bb.spv $(B)/shaders/00000000feed97bc.spv $(B)/shaders/00000000feed97bd.spv $(B)/shaders/00000000feed97be.spv $(B)/shaders/00000000feed97bf.spv $(B)/shaders/00000000feed97c0.spv $(B)/shaders/00000000feed97c1.spv $(B)/shaders/00000000feed97c2.spv $(B)/shaders/00000000feed97c3.spv $(B)/shaders/00000000feed97c4.spv $(B)/shaders/00000000feed97c5.spv $(B)/shaders/00000000feed97c6.spv $(B)/shaders/00000000feed97c7.spv $(B)/shaders/00000000feed97c8.spv $(B)/shaders/00000000feed97c9.spv $(B)/shaders/00000000feed97ca.spv
$(B)/shaders/00000000feed9875.spv: REDUCTION_FLAGS = -DONE_ROW
$(B)/shaders/00000000feed9876.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE
$(B)/shaders/00000000feed9877.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY
$(B)/shaders/00000000feed9878.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE
$(B)/shaders/00000000feed9879.spv: REDUCTION_FLAGS = -DONE_ROW -DGRADIENTS
$(B)/shaders/00000000feed987a.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed987b.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DGRADIENTS
$(B)/shaders/00000000feed987c.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed987d.spv: REDUCTION_FLAGS = -DONE_ROW -DOFFSET
$(B)/shaders/00000000feed987e.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DOFFSET
$(B)/shaders/00000000feed987f.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DOFFSET
$(B)/shaders/00000000feed9880.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DOFFSET
$(B)/shaders/00000000feed9881.spv: REDUCTION_FLAGS = -DONE_ROW -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9882.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9883.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9884.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9885.spv: REDUCTION_FLAGS = -DONE_ROW -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9886.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9887.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9888.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9889.spv: REDUCTION_FLAGS = -DONE_ROW -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed988a.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed988b.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed988c.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed988d.spv: REDUCTION_FLAGS = -DONE_ROW -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed988e.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed988f.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed9890.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed9891.spv: REDUCTION_FLAGS = -DONE_ROW -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed9892.spv: REDUCTION_FLAGS = -DONE_ROW -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed9893.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed9894.spv: REDUCTION_FLAGS = -DONE_ROW -DARRAY -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed9895.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED
$(B)/shaders/00000000feed9896.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DSEPARATE
$(B)/shaders/00000000feed9899.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DGRADIENTS
$(B)/shaders/00000000feed989a.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed989d.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed989e.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed98a1.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed98a2.spv: REDUCTION_FLAGS = -DONE_ROW -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed98a5.spv: REDUCTION_FLAGS = -DONE_ROW -DQUERY -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed98a6.spv: REDUCTION_FLAGS = -DONE_ROW -DQUERY -DFRAGMENT -fshader-stage=fragment -DSEPARATE
$(B)/shaders/00000000feed98a7.spv: REDUCTION_FLAGS = -DONE_ROW -DQUERY -DFRAGMENT -fshader-stage=fragment -DARRAY
$(B)/shaders/00000000feed98a8.spv: REDUCTION_FLAGS = -DONE_ROW -DQUERY -DFRAGMENT -fshader-stage=fragment -DARRAY -DSEPARATE
REDUCTION_SHADERS += $(B)/shaders/00000000feed9875.spv $(B)/shaders/00000000feed9876.spv $(B)/shaders/00000000feed9877.spv $(B)/shaders/00000000feed9878.spv $(B)/shaders/00000000feed9879.spv $(B)/shaders/00000000feed987a.spv $(B)/shaders/00000000feed987b.spv $(B)/shaders/00000000feed987c.spv $(B)/shaders/00000000feed987d.spv $(B)/shaders/00000000feed987e.spv $(B)/shaders/00000000feed987f.spv $(B)/shaders/00000000feed9880.spv $(B)/shaders/00000000feed9881.spv $(B)/shaders/00000000feed9882.spv $(B)/shaders/00000000feed9883.spv $(B)/shaders/00000000feed9884.spv $(B)/shaders/00000000feed9885.spv $(B)/shaders/00000000feed9886.spv $(B)/shaders/00000000feed9887.spv $(B)/shaders/00000000feed9888.spv $(B)/shaders/00000000feed9889.spv $(B)/shaders/00000000feed988a.spv $(B)/shaders/00000000feed988b.spv $(B)/shaders/00000000feed988c.spv $(B)/shaders/00000000feed988d.spv $(B)/shaders/00000000feed988e.spv $(B)/shaders/00000000feed988f.spv $(B)/shaders/00000000feed9890.spv $(B)/shaders/00000000feed9891.spv $(B)/shaders/00000000feed9892.spv $(B)/shaders/00000000feed9893.spv $(B)/shaders/00000000feed9894.spv $(B)/shaders/00000000feed9895.spv $(B)/shaders/00000000feed9896.spv $(B)/shaders/00000000feed9899.spv $(B)/shaders/00000000feed989a.spv $(B)/shaders/00000000feed989d.spv $(B)/shaders/00000000feed989e.spv $(B)/shaders/00000000feed98a1.spv $(B)/shaders/00000000feed98a2.spv $(B)/shaders/00000000feed98a5.spv $(B)/shaders/00000000feed98a6.spv $(B)/shaders/00000000feed98a7.spv $(B)/shaders/00000000feed98a8.spv
fixture: $(REDUCTION_SHADERS)
$(REDUCTION_SHADERS): tests/sampler_reduction.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $(REDUCTION_FLAGS) $< -o $@
	$(if $(findstring -DFRAGMENT,$(REDUCTION_FLAGS)),printf 'stage ps\n',printf 'stage cs\ntg 1 1 1\nb 2 ssbo 1 sbuf 0 -1 - -1\n') > $(@:.spv=.vk.txt)
	printf 'b 1 ubo 1 cbuf 0 -1 - -1\n' >> $(@:.spv=.vk.txt)
	$(if $(findstring -DSEPARATE,$(REDUCTION_FLAGS)),printf 'b 0 tex 1 tex 0 -1 - -1\nb 3 samp 1 samp 1 -1 - -1\n',printf 'b 0 cis 1 tex 0 -1 samp_tex 0\n') >> $(@:.spv=.vk.txt)
$(B)/t_replay: tests/one_row_guest.inc ../../../android/tests/texture_coordinates_cases.h
REDUCTION_ARRAY_SHADERS = $(B)/shaders/00000000feed99b1.spv $(B)/shaders/00000000feed99b2.spv $(B)/shaders/00000000feed99b3.spv $(B)/shaders/00000000feed99b4.spv
$(B)/shaders/00000000feed99b9.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed99ba.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DSEPARATE
$(B)/shaders/00000000feed99bb.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DCONSTANT_INDEX
$(B)/shaders/00000000feed99bc.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DCONSTANT_INDEX -DSEPARATE
$(B)/shaders/00000000feed99c1.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DQUERY
$(B)/shaders/00000000feed99c2.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DQUERY -DSEPARATE
$(B)/shaders/00000000feed99c3.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DQUERY -DCONSTANT_INDEX
$(B)/shaders/00000000feed99c4.spv: ARRAY_FLAGS = -DFRAGMENT -fshader-stage=fragment -DQUERY -DCONSTANT_INDEX -DSEPARATE
REDUCTION_ARRAY_SHADERS += $(B)/shaders/00000000feed99b9.spv $(B)/shaders/00000000feed99ba.spv $(B)/shaders/00000000feed99bb.spv $(B)/shaders/00000000feed99bc.spv $(B)/shaders/00000000feed99c1.spv $(B)/shaders/00000000feed99c2.spv $(B)/shaders/00000000feed99c3.spv $(B)/shaders/00000000feed99c4.spv
$(B)/shaders/00000000feed9ab1.spv: COMPARISON_FLAGS =
$(B)/shaders/00000000feed9ab2.spv: COMPARISON_FLAGS = -DSEPARATE
$(B)/shaders/00000000feed9ab3.spv: COMPARISON_FLAGS = -DGRADIENTS
$(B)/shaders/00000000feed9ab4.spv: COMPARISON_FLAGS = -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed9ab5.spv: COMPARISON_FLAGS = -DOFFSET
$(B)/shaders/00000000feed9ab6.spv: COMPARISON_FLAGS = -DSEPARATE -DOFFSET
$(B)/shaders/00000000feed9ab7.spv: COMPARISON_FLAGS = -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9ab8.spv: COMPARISON_FLAGS = -DSEPARATE -DGRADIENTS -DOFFSET
$(B)/shaders/00000000feed9ab9.spv: COMPARISON_FLAGS = -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9aba.spv: COMPARISON_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9abb.spv: COMPARISON_FLAGS = -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed9abc.spv: COMPARISON_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed9abd.spv: COMPARISON_FLAGS = -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed9abe.spv: COMPARISON_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DOFFSET
$(B)/shaders/00000000feed9abf.spv: COMPARISON_FLAGS = -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
$(B)/shaders/00000000feed9ac0.spv: COMPARISON_FLAGS = -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS -DOFFSET
REDUCTION_COMPARISON_SHADERS = $(B)/shaders/00000000feed9ab1.spv $(B)/shaders/00000000feed9ab2.spv $(B)/shaders/00000000feed9ab3.spv $(B)/shaders/00000000feed9ab4.spv $(B)/shaders/00000000feed9ab5.spv $(B)/shaders/00000000feed9ab6.spv $(B)/shaders/00000000feed9ab7.spv $(B)/shaders/00000000feed9ab8.spv $(B)/shaders/00000000feed9ab9.spv $(B)/shaders/00000000feed9aba.spv $(B)/shaders/00000000feed9abb.spv $(B)/shaders/00000000feed9abc.spv $(B)/shaders/00000000feed9abd.spv $(B)/shaders/00000000feed9abe.spv $(B)/shaders/00000000feed9abf.spv $(B)/shaders/00000000feed9ac0.spv
$(B)/shaders/00000000feed9ac1.spv: COMPARISON_FLAGS = -DPROJECTED
$(B)/shaders/00000000feed9ac2.spv: COMPARISON_FLAGS = -DPROJECTED -DSEPARATE
$(B)/shaders/00000000feed9ac3.spv: COMPARISON_FLAGS = -DPROJECTED -DGRADIENTS
$(B)/shaders/00000000feed9ac4.spv: COMPARISON_FLAGS = -DPROJECTED -DSEPARATE -DGRADIENTS
$(B)/shaders/00000000feed9ac5.spv: COMPARISON_FLAGS = -DPROJECTED -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9ac6.spv: COMPARISON_FLAGS = -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment
$(B)/shaders/00000000feed9ac7.spv: COMPARISON_FLAGS = -DPROJECTED -DFRAGMENT -fshader-stage=fragment -DBIAS
$(B)/shaders/00000000feed9ac8.spv: COMPARISON_FLAGS = -DPROJECTED -DSEPARATE -DFRAGMENT -fshader-stage=fragment -DBIAS
REDUCTION_COMPARISON_SHADERS += $(B)/shaders/00000000feed9ac1.spv $(B)/shaders/00000000feed9ac2.spv $(B)/shaders/00000000feed9ac3.spv $(B)/shaders/00000000feed9ac4.spv $(B)/shaders/00000000feed9ac5.spv $(B)/shaders/00000000feed9ac6.spv $(B)/shaders/00000000feed9ac7.spv $(B)/shaders/00000000feed9ac8.spv
fixture: $(REDUCTION_COMPARISON_SHADERS)
$(REDUCTION_COMPARISON_SHADERS): tests/sampler_reduction_compare.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $(COMPARISON_FLAGS) $< -o $@
	$(if $(findstring -DFRAGMENT,$(COMPARISON_FLAGS)),printf 'stage ps\n',printf 'stage cs\ntg 1 1 1\nb 2 ssbo 1 sbuf 0 -1 - -1\n') > $(@:.spv=.vk.txt)
	printf 'b 1 ubo 1 cbuf 0 -1 - -1\n' >> $(@:.spv=.vk.txt)
	$(if $(findstring -DSEPARATE,$(COMPARISON_FLAGS)),printf 'b 0 tex 1 tex 0 -1 - -1\nb 3 samp 1 samp 1 -1 - -1\n',printf 'b 0 cis 1 tex 0 -1 samp_tex 0\n') >> $(@:.spv=.vk.txt)
fixture: $(REDUCTION_ARRAY_SHADERS)
$(B)/shaders/00000000feed99b1.spv: ARRAY_FLAGS =
$(B)/shaders/00000000feed99b2.spv: ARRAY_FLAGS = -DSEPARATE
$(B)/shaders/00000000feed99b3.spv: ARRAY_FLAGS = -DCONSTANT_INDEX
$(B)/shaders/00000000feed99b4.spv: ARRAY_FLAGS = -DCONSTANT_INDEX -DSEPARATE
$(REDUCTION_ARRAY_SHADERS): tests/sampler_reduction_array.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $(ARRAY_FLAGS) $< -o $@
	$(if $(findstring -DFRAGMENT,$(ARRAY_FLAGS)),printf 'stage ps\n',printf 'stage cs\ntg 1 1 1\nb 2 ssbo 1 sbuf 0 -1 - -1\n') > $(@:.spv=.vk.txt)
	printf 'b 1 ubo 1 cbuf 0 -1 - -1\n' >> $(@:.spv=.vk.txt)
	$(if $(findstring -DSEPARATE,$(ARRAY_FLAGS)),printf 'b 0 tex 32 tex 0 -1 - -1\nb 3 samp 32 samp 1 -1 - -1\n',printf 'b 0 cis 32 tex 0 -1 samp_tex 0\n') >> $(@:.spv=.vk.txt)
fixture: $(B)/shaders/0000000000000072.spv $(B)/shaders/0000000000000073.spv $(B)/shaders/0000000000000074.spv
$(B)/shaders/0000000000000074.spv: tests/subresource_views.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 -DONE_ROW $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 1 tex 0 -1 samp_tex 0\nb 1 cis 1 tex 1 -1 samp_tex 1\nb 2 simg 1 img 0 -1 - -1\nb 3 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000072.spv: tests/subresource_views.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 1 tex 0 -1 samp_tex 0\nb 1 cis 1 tex 1 -1 samp_tex 1\nb 2 simg 1 img 0 -1 - -1\nb 3 ssbo 1 sbuf 0 -1 - -1\nb 4 simg 1 img 0 -1 - -1\nb 5 cis 1 tex 0 -1 samp_tex 0\n' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000073.spv: tests/subresource_views.comp
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 -DVOLUME $< -o $@
	printf 'stage cs\ntg 1 1 1\nb 0 cis 1 tex 0 -1 samp_tex 0\nb 1 simg 1 img 0 -1 - -1\nb 2 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
fixture: $(B)/shaders/0000000000000071.spv
$(B)/shaders/0000000000000071.spv: tests/empty_ms.comp tests/empty_ms.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/empty_ms.vk.txt $(@:.spv=.vk.txt)
fixture: $(B)/shaders/0000000000000070.spv
$(B)/shaders/0000000000000070.spv: tests/empty_images.comp tests/empty_images.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/empty_images.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000006e.spv: tests/storage_views.comp tests/storage_views.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/storage_views.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000006f.spv: tests/storage_views.comp tests/storage_views.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 -DFORMATLESS $< -o $@
	cp tests/storage_views.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000006d.spv: tests/cube_ranges.comp tests/cube_ranges.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/cube_ranges.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000006a.spv: tests/typed_border.comp tests/typed_border_separate.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DSEPARATE $< -o $@
	cp tests/typed_border_separate.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000069.spv: tests/typed_border.comp tests/typed_border.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/typed_border.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000068.spv: tests/polygon_fill.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage vs' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000060.spv: tests/polygon_offset.vert
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	echo 'stage vs' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000061.spv: tests/alpha.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DBOUND=0 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000062.spv: tests/alpha.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DBOUND=1 $< -o $@
	printf 'stage ps\nb 0 ubo 1 cbuf 0 -1 - -1\nb 1 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000063.spv: tests/alpha.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DINTEGER_TARGET=1 $< -o $@
	echo 'stage ps' > $(@:.spv=.vk.txt)
$(B)/shaders/0000000000000064.spv $(B)/shaders/0000000000000065.spv $(B)/shaders/0000000000000067.spv: tests/logic.frag
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 -DUINT_TARGET=$(if $(filter %65.spv,$@),1,0) -DMIXED_TARGET=$(if $(filter %67.spv,$@),1,0) $< -o $@
	printf 'stage ps\nb 0 ubo 1 cbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
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
$(B)/shaders/000000000000ad72.spv: tests/typed_format.comp tests/typed_format.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/typed_format.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000ad73.spv: tests/empty_texels.comp tests/empty_texels.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/empty_texels.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000ad74.spv $(B)/shaders/000000000000ad75.spv: tests/scaled_texel.comp tests/scaled_texel.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $(if $(filter %75.spv,$@),-DDYNAMIC_SLOT) $< -o $@
	cp tests/scaled_texel.vk.txt $(@:.spv=.vk.txt)
fixture: $(B)/shaders/000000000000ad74.spv $(B)/shaders/000000000000ad75.spv $(B)/shaders/000000000000ad73.spv $(B)/shaders/000000000000ad72.spv $(B)/shaders/000000000000ad70.spv $(B)/shaders/000000000000ad71.spv
$(B)/shaders/000000000000ad71.spv: $(B)/texel-dynamic.spv tests/texel_alias.vk.txt
	mkdir -p $(B)/shaders
	cp $< $@
	cp tests/texel_alias.vk.txt $(@:.spv=.vk.txt)
$(B)/shaders/000000000000ad70.spv: tests/texel_alias.comp tests/texel_alias.vk.txt
	mkdir -p $(B)/shaders
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	cp tests/texel_alias.vk.txt $(@:.spv=.vk.txt)
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
$(B)/t_replay: $(SRC) tests/resource_shapes.inc ../../../android/tests/resource_shapes_cases.h tests/driver_c2.inc tests/split_handle.inc tests/uniform_storage.inc spv_vertex_fetch.h spv_scaled32.h spv_output_defaults.h spv_draw_parameters.h spv_buffer_bias.h spv_texel_bias.h spv_texel_wide.inc shader_contract.h warp_width.h geometry_contract.h meshstage_contract.h uniform_address.h present_rect.h split_handle.h driver_c2.h driver_c0.h resource_contract.h ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_dirty.h ../nvn_tex.h ../nvn_refl.h ../../../third_party/bcdec/bcdec.h $(GAME_H) | profile
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
recorded-transfer-test: $(B)/t_recorded_transfer fixture
	DM_VK_DIR="$(abspath $(B)/shaders)" $(B)/t_recorded_transfer
recorded-transfer-conformance: recorded-transfer-test
	$(MAKE) -C ../../tests GAME=$(GAME) check_t_recorded_transfer
$(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: ../../tests/t_recorded_transfer.c tests/public_bindings.inc tests/public_buffers.inc tests/public_feedback.inc ../hle_nvn.c ../nvn_obj.h ../nvn_mtl.h ../nvn_timestamp.h ../dmsh_abi.h ../nvn_refl.h spv_vertex_fetch.h spv_scaled32.h spv_output_defaults.h spv_draw_parameters.h spv_buffer_bias.h spv_texel_bias.h spv_texel_wide.inc driver_c0.h resource_contract.h $(RECORDER_SRC) ../nvn_asm.h ../../../android/compat/sha256.c ../../../android/compat/CommonCrypto/CommonDigest.h $(GAME_H) | profile
	mkdir -p $(B)
	$(CC) $(CPPFLAGS) -DDM_NVN_VULKAN -DDM_VK_PUBLIC_BINDINGS $(RECORDER_CPPFLAGS) $(CFLAGS) -UNDEBUG $< $(RECORDER_SRC) $(RECORDER_EXTRA) $(RECORDER_LDFLAGS) -o $@ $(LIBS)

# The Android SHA implementation is the loader's as well. The driver and ANativeWindow
# are linked from the NDK. Use the device's Vulkan loader when executing the fixture.
SURFACE_WRAPS = vkGetSwapchainImagesKHR vkAcquireNextImageKHR vkQueuePresentKHR vkGetPhysicalDeviceSurfaceCapabilitiesKHR vkCreateSwapchainKHR
$(B)/t_present_surface_android: $(B)/t_replay tests/present_surface.inc
	$(CC) $(CPPFLAGS) $(REPLAY_CPPFLAGS) $(CFLAGS) -DDM_VK_TEST_ANDROID_SURFACE $(SRC) -o $@ $(LIBS) -lmediandk $(foreach f,$(SURFACE_WRAPS),-Wl,--wrap=$(f))
$(B)/t_sampler_limits: $(B)/t_replay tests/sampler_limits.inc
	$(CC) $(CPPFLAGS) $(REPLAY_CPPFLAGS) $(CFLAGS) -DDM_VK_TEST_SAMPLER_LIMITS $(SRC) -o $@ $(LIBS) $(foreach f,vkGetPhysicalDeviceProperties vkGetPhysicalDeviceFeatures2 vkCreateSampler,-Wl,--wrap=$(f))
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

fixture: $(B)/shaders/000000000000006c.spv
$(B)/shaders/000000000000006c.spv: tests/storage.vert tests/storage.vert.vk.txt
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 -DCHECK_DISCARD_IMAGE $< -o $@
	cat tests/storage.vert.vk.txt > $(@:.spv=.vk.txt)
	echo 'b 1 simg 1 img 2 -1 - -1 view=1 num=1' >> $(@:.spv=.vk.txt)
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

$(B)/t_texel_bias: tests/t_texel_bias.c spv_texel_bias.h spv_texel_wide.inc
	mkdir -p $(B)
	$(BUILD_CC) -O1 -g -Wall -Werror $(BUILD_CFLAGS) $< -o $@
$(B)/texel-dynamic.spv: tests/texel_alias.comp
	mkdir -p $(B)
	$(GLSLC) --target-env=vulkan1.1 -DDYNAMIC_SLOT $< -o $@
$(B)/texel-sparse.spv: tests/sparse_texel.spvasm
	mkdir -p $(B)
	$(GLSLC) --target-env=vulkan1.1 -c $< -o $@
.PHONY: texel-bias-check
texel-bias-check: $(B)/shaders/000000000000ad75.spv $(B)/t_texel_bias $(B)/shaders/000000000000ad70.spv $(B)/texel-dynamic.spv $(B)/texel-sparse.spv
	$(B)/t_texel_bias $(B)/shaders/000000000000ad70.spv $(B)/texel-biased.spv
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/texel-biased.spv
	$(B)/t_texel_bias $(B)/texel-dynamic.spv $(B)/texel-dynamic-biased.spv
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/texel-dynamic-biased.spv
	$(B)/t_texel_bias $(B)/shaders/000000000000ad75.spv $(B)/texel-scaled.spv scaled
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/texel-scaled.spv
	$(B)/t_texel_bias $(B)/shaders/000000000000ad75.spv $(B)/texel-wide.spv wide
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/texel-wide.spv
	$(B)/t_texel_bias $(B)/texel-sparse.spv $(B)/texel-wide-sparse.spv wide
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/texel-wide-sparse.spv

$(B)/t_integer_outputs: tests/t_integer_outputs.c spv_integer_outputs.h spv_fragment_outputs.h spv_alpha.h
	mkdir -p $(B)
	$(BUILD_CC) -O1 -g -Wall -Werror $(BUILD_CFLAGS) $< -o $@
.PHONY: integer-outputs-check
integer-outputs-check: $(B)/t_integer_outputs $(B)/shaders/0000000000000067.spv
	$(B)/t_integer_outputs $(B)/shaders/0000000000000067.spv $(B)/integer-mixed.spv 2 0 1
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/integer-mixed.spv
	$(B)/t_integer_outputs $(B)/shaders/0000000000000067.spv $(B)/integer-signed.spv 1 2 0
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/integer-signed.spv

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

# Runtime 1D compatibility also applies when loading older shader libraries.
$(B)/t_one_row: tests/t_one_row.c spv_one_row.h
	mkdir -p $(B)
	$(BUILD_CC) -O1 -g -Wall -Wextra -Werror $(BUILD_CFLAGS) $< -o $@
$(B)/one-row-fragment.spv: tests/one_row.frag
	mkdir -p $(B)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
.PHONY: one-row-check
one-row-check: $(B)/t_one_row $(B)/one-row-fragment.spv $(B)/shaders/0000000000000072.spv $(B)/shaders/0000000000000074.spv
	$(B)/t_one_row $(B)/shaders/0000000000000072.spv $(B)/one-row-compute.spv
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/one-row-compute.spv
	$(B)/t_one_row $(B)/one-row-fragment.spv $(B)/one-row-fragment-adapted.spv
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/one-row-fragment-adapted.spv
	$(B)/t_one_row $(B)/shaders/0000000000000074.spv $(B)/one-row-unchanged.spv
	cmp $(B)/shaders/0000000000000074.spv $(B)/one-row-unchanged.spv

.PHONY: one-row-guest-fixture
one-row-guest-fixture: fixture
	python3 ../../../android/tests/texture_coordinates_fixture.py $(abspath $(B))/one-row-samples
	python3 ../../../android/tests/texture_query_shapes_fixture.py $(abspath $(B))/one-row-queries
	cp $(B)/one-row-samples/export/*.spv $(B)/one-row-samples/export/*.vk.txt $(B)/shaders/
	cp $(B)/one-row-queries/export/*.spv $(B)/one-row-queries/export/*.vk.txt $(B)/shaders/

$(B)/t_sample_reduction: tests/t_sample_reduction.c spv_sample_reduction.h spv_one_row.h spv_texel_bias.h spv_texel_wide.inc
	mkdir -p $(B)
	$(BUILD_CC) -O1 -g -Wall -Wextra -Werror -Wno-unused-function $(BUILD_CFLAGS) $< -o $@
.PHONY: sampler-reduction-check
sampler-reduction-check: $(B)/t_sample_reduction $(REDUCTION_SHADERS) $(REDUCTION_ARRAY_SHADERS) $(REDUCTION_COMPARISON_SHADERS)
	@set -e; for input in $(REDUCTION_SHADERS) $(REDUCTION_ARRAY_SHADERS) $(REDUCTION_COMPARISON_SHADERS); do \
	  key=$${input##*/}; value=$${key%.spv}; binding=$$(( (0x$$value - 0x75) % 2 * 3 )); \
	  case $$key in *feed99*) option=array ;; *) option= ;; esac; \
	  $(B)/t_sample_reduction "$$input" "$(B)/reduction-$$key" "$$binding" $$option; \
	  $(SPIRV_VAL) --target-env vulkan1.1 "$(B)/reduction-$$key"; \
	done
	$(B)/t_sample_reduction $(B)/shaders/0000000000000075.spv $(B)/reduction-no-import.spv 0 no-import
	$(SPIRV_VAL) --target-env vulkan1.1 $(B)/reduction-no-import.spv

$(B)/t_replay: tests/logical_slots.inc
fixture: $(B)/shaders/00000000feedaa02.spv
$(B)/shaders/00000000feedaa02.spv: tests/logical_slots.comp Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 32 1 1\nb 0 tex 32 tex_slots -1 -1 - -1 view=1 num=0\nb 1 samp 32 samp_slots -1 -1 - -1\nb 2 ssbo 1 sbuf 0 -1 - -1\nb 3 ubo 1 drvcb -1 2 - -1\n' > $(@:.spv=.vk.txt)
.PHONY: logical-slots-fixture
logical-slots-fixture: fixture
	PATH="$(dir $(GLSLC)):$$PATH" python3 ../../../android/tests/resource_shapes_fixture.py $(B)/logical-slot-fixture slots
	cp $(B)/logical-slot-fixture/export/00000000feed6100.spv $(B)/shaders/00000000feedaa01.spv
	cp $(B)/logical-slot-fixture/export/00000000feed6100.vk.txt $(B)/shaders/00000000feedaa01.vk.txt

$(B)/t_replay: tests/c2_arrays.inc ../../../android/tests/c2_array_cases.h
.PHONY: c2-arrays-fixture
c2-arrays-fixture: fixture
	PATH="$(dir $(GLSLC)):$$PATH" python3 ../../../android/tests/c2_array_fixture.py $(B)/c2-array-fixture
	cp $(B)/c2-array-fixture/export/*.spv $(B)/c2-array-fixture/export/*.vk.txt $(B)/shaders/
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: descriptor_tables.inc spv_descriptor_table.h
$(B)/t_descriptor_table: tests/t_descriptor_table.c spv_descriptor_table.h spv_texel_bias.h
	mkdir -p $(B)
	$(CC) $(CFLAGS) $< -o $@
fixture: $(B)/shaders/00000000feed4002.spv
$(B)/shaders/00000000feed4002.spv: tests/c2_separate.comp Makefile
	mkdir -p $(@D)
	$(GLSLC) --target-env=vulkan1.1 $< -o $@
	printf 'stage cs\ntg 32 1 1\nb 0 tex 301 tex_c2a -1 0 - -1 view=1 num=0\nb 1 samp 301 samp_c2a -1 0 - -1 c2_or=1392\nb 2 ssbo 1 sbuf 0 -1 - -1\n' > $(@:.spv=.vk.txt)
$(B)/t_replay: tests/resource_pools.inc
.PHONY: resource-pools-fixture
resource-pools-fixture: fixture
	PATH="$(dir $(GLSLC)):$$PATH" python3 ../../../android/tests/resource_pool_fixture.py $(B)/resource-pool-fixture
	cp $(B)/resource-pool-fixture/export/*.spv $(B)/resource-pool-fixture/export/*.vk.txt $(B)/shaders/
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: resource_pools.inc
$(B)/t_replay: tests/global_memory.inc
.PHONY: global-memory-fixture
global-memory-fixture: fixture
	PATH="$(dir $(GLSLC)):$$PATH" python3 ../../../android/tests/global_memory_fixture.py $(B)/global-memory-fixture
	cp $(B)/global-memory-fixture/export/*.spv $(B)/global-memory-fixture/export/*.vk.txt $(B)/shaders/
$(B)/t_replay $(B)/t_recorded_transfer $(B)/t_recorded_transfer_android: global_memory_read.inc
