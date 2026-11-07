NDK ?= $(ANDROID_NDK_HOME)
NDK_HOST ?= $(if $(filter Darwin,$(shell uname -s)),darwin-x86_64,linux-x86_64)
CC := $(NDK)/toolchains/llvm/prebuilt/$(NDK_HOST)/bin/aarch64-linux-android30-clang
B ?= build
ifeq ($(strip $(GAME)),)
$(error GAME is not set)
endif
HOST := ../../../host
CPPFLAGS := -D_GNU_SOURCE -I$(HOST) -I$(HOST)/gen/$(GAME) -I../../../third_party/vulkan-headers/include
CFLAGS := -O1 -g -Wall -Wextra -Werror -Wno-unused-parameter -Wno-format
SOURCES := $(wildcard ../*.c) $(HOST)/hle_bigstack.c
GLSLC := $(NDK)/shader-tools/$(NDK_HOST)/glslc
.DEFAULT_GOAL := $(B)/t_dispatch
$(B)/t_dispatch: t_dispatch.c $(SOURCES) $(wildcard ../*.h)
	@mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(SOURCES) -lvulkan -o $@
$(B)/triangle.%.h: triangle.%
	@mkdir -p $(B)
	$(GLSLC) --target-env=vulkan1.1 -mfmt=c $< -o $@
$(B)/t_resources: t_dispatch.c resource_checks.h $(SOURCES) $(wildcard ../*.h) $(B)/triangle.vert.h $(B)/triangle.frag.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTEST_RESOURCES -I$(B) $< $(SOURCES) -lvulkan -o $@
.PHONY: resources
resources: $(B)/t_resources
$(B)/t_commands: t_dispatch.c resource_checks.h command_checks.h $(SOURCES) $(wildcard ../*.h) $(B)/triangle.vert.h $(B)/triangle.frag.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -DTEST_RESOURCES -DTEST_COMMANDS -I$(B) $< $(SOURCES) -lvulkan -o $@
.PHONY: commands
commands: $(B)/t_commands
$(B)/t_template_layout: t_template_layout.c ../descriptor_templates.c ../guest_vk.h $(HOST)/hle_bigstack.c
	@mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(HOST)/hle_bigstack.c -o $@
.PHONY: templates
templates: $(B)/t_template_layout
