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
         $(B)/shaders/0000000000000003.spv $(B)/shaders/0000000000000004.spv $(B)/shaders/0000000000000021.spv $(B)/shaders/0000000000000022.spv
profile:
	$(MAKE) -C $(HOST) GAME=$(GAME) game_profile
$(B)/t_replay: $(SRC) ../nvn_obj.h ../nvn_mtl.h ../nvn_dirty.h ../nvn_tex.h ../nvn_refl.h ../../../third_party/bcdec/bcdec.h $(GAME_H) | profile
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
