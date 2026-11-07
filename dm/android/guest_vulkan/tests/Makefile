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
.DEFAULT_GOAL := $(B)/t_dispatch
$(B)/t_dispatch: t_dispatch.c $(SOURCES) $(wildcard ../*.h)
	@mkdir -p $(B)
	$(CC) $(CPPFLAGS) $(CFLAGS) $< $(SOURCES) -lvulkan -o $@
