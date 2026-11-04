# Android host: the shared host sources (dm/host, dm/gfx) built with the NDK into libdmhost.so, the native library of the Android app (make GAME=<base game title ID>, as dm/host).
# Uses shared portable host implementations; compat/ supplies SHA-256 and limited headers, never global Mach failure shims.
# NDK: NDK=<path> or ANDROID_NDK_HOME. API level 30 (Android 11: bionic leaves TLS_SLOT_APP to apps from Android 10, renameat2 and sem_clockwait arrive in 11)
NDK ?= $(ANDROID_NDK_HOME)
API ?= 30
NDK_HOST ?= $(if $(filter Darwin,$(shell uname -s)),darwin-x86_64,linux-x86_64)
TC := $(NDK)/toolchains/llvm/prebuilt/$(NDK_HOST)/bin
CC := $(TC)/aarch64-linux-android$(API)-clang
AR := $(TC)/llvm-ar
ifeq ($(strip $(GAME)),)
$(error GAME is not set: make GAME=<base game title ID>)
endif
HOST := ../host
BUILD_ROOT ?= $(CURDIR)/gen
OUT := $(BUILD_ROOT)/$(GAME)
# The game profile header comes from the host Makefile (game.py, DM_GAMES_DIR), which also links an outside profile at host/gen/<id>/profile
GAME_GEN := $(HOST)/gen/$(GAME)
GAME_DIR := $(if $(wildcard ../games/$(GAME)/game.tsv),../games/$(GAME),$(GAME_GEN)/profile)
CFLAGS := -MMD -MP -O1 -g -fPIC -fvisibility=hidden -D_GNU_SOURCE -Wall -Wno-unused-function -Wno-format -Wno-deprecated-declarations -Wno-incompatible-pointer-types-discards-qualifiers \
          -Icompat -I$(HOST) -I../gfx/nvn -I../gfx/nvn/include -I../third_party/vulkan-headers/include -I$(GAME_GEN) -I$(GAME_DIR) \
          -I../third_party/libvpx -I../third_party/dav1d/include -I../third_party/stb_vorbis -I../third_party/lzfse
CONFIG_FLAGS := $(CFLAGS)
SRC := $(HOST)/main.c $(HOST)/loader.c $(HOST)/hle.c $(HOST)/crash.c $(HOST)/probe.c $(HOST)/probe_text.c $(HOST)/thread.c $(HOST)/save_sync.c $(HOST)/save_fs_linux.c $(sort $(wildcard $(HOST)/hle_*.c)) $(HOST)/entry.S \
       ../gfx/hle_nv.c ../gfx/hle_vi.c $(sort $(wildcard ../gfx/nvn/*.c ../gfx/nvn/*.S ../gfx/tex/*.c))
# The donor Vulkan implementation lives beside the shared NVN recorder.
SRC += ../gfx/nvn/vulkan/nvn_vk.c
LOCAL := compat/sha256.c $(sort $(wildcard src/*.c))
SRC += $(sort $(wildcard ../third_party/lzfse/*.c))
OBJ := $(patsubst ../%,$(OUT)/obj/%.o,$(SRC)) $(patsubst %,$(OUT)/obj/android/%.o,$(LOCAL))
CODEC_DIR := $(BUILD_ROOT)/codecs
CODEC_LIBS := $(CODEC_DIR)/vpx/libvpx.a $(CODEC_DIR)/dav1d/libdav1d.a
$(CODEC_LIBS): | codecs
.PHONY: codecs
codecs:
	$(MAKE) -C $(HOST) GAME=$(GAME) CODEC_TARGET=android CODEC_CC="$(CC)" CODEC_AR="$(AR)" CODEC_GEN="$(CODEC_DIR)" codec-libs
# dmhost: the same host as a command-line program for adb (cli/front_cli.c: no window), run from /data/local/tmp as on the Mac: dmhost <module dir> [modules]
CLI_OBJ := $(patsubst %,$(OUT)/obj/android/%.o,cli/front_cli.c)
$(CLI_OBJ): CFLAGS += -DDM_ANDROID_CLI
.DEFAULT_GOAL := $(OUT)/libdmhost.so
.PHONY: profile clean FORCE
profile: $(GAME_GEN)/game_profile.h
$(GAME_GEN)/game_profile.h: FORCE
	$(MAKE) -C $(HOST) GAME=$(GAME) game_profile
# Recompile when the compiler or flags change, while leaving identical stamps alone.
$(OUT)/compile.flags: FORCE
	@mkdir -p $(OUT)
	@test -x "$(CC)" || { echo '[RED] Set NDK or ANDROID_NDK_HOME to an installed Android NDK'; exit 1; }
	@printf '%s\n' '$(CC)' '$(CONFIG_FLAGS)' > $@.tmp
	@cmp -s $@.tmp $@ && rm $@.tmp || mv $@.tmp $@
$(OBJ) $(CLI_OBJ) $(OUT)/obj/android/app/front_android.c.o: Makefile $(OUT)/compile.flags $(GAME_GEN)/game_profile.h
$(OUT)/obj/android/%.o: % | profile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
$(OUT)/obj/host/main.c.o: CFLAGS += -Dmain=dm_main   # the app's front end calls it (app/front_android.c); the adb program's main is in cli/front_cli.c
$(OUT)/obj/%.o: ../% | profile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c $< -o $@
APP_OBJ := $(patsubst %,$(OUT)/obj/android/%.o,app/front_android.c)
$(OUT)/android_contract.h: FORCE
	python3 tools/target_contract.py $(GAME) $@
$(APP_OBJ): $(OUT)/android_contract.h
$(APP_OBJ): CFLAGS += -I$(OUT)
$(OUT)/libdmhost.so: $(OBJ) $(APP_OBJ) $(CODEC_LIBS)
	$(CC) -shared -o $@ $(OBJ) $(APP_OBJ) $(CODEC_LIBS) -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-z,max-page-size=16384 -landroid -llog -laaudio -lvulkan -lmediandk -lm -ldl
$(OUT)/dmhost: $(OBJ) $(CLI_OBJ) $(CODEC_LIBS)
	$(CC) -pie -o $@ $(OBJ) $(CLI_OBJ) $(CODEC_LIBS) -Wl,--no-undefined -Wl,--error-limit=0 -Wl,-z,max-page-size=16384 -landroid -llog -laaudio -lvulkan -lmediandk -lm -ldl
dmhost: $(OUT)/dmhost
.PHONY: dmhost
-include $(OBJ:.o=.d) $(APP_OBJ:.o=.d) $(CLI_OBJ:.o=.d)
clean:
	rm -rf "$(OUT)"
