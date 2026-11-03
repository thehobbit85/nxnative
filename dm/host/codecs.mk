# Shared by the native host and the Android cross build. Outputs from different ABIs are kept apart.
CODEC_CC ?= clang
CODEC_AR ?= ar
CODEC_GEN ?= gen
ifeq ($(CODEC_TARGET),android)
CODEC_GEN = gen/android-arm64
CODEC_CFLAGS = -fPIC -D_GNU_SOURCE
DAV1D_CONFIG_FLAGS = -I$(DAV1D)/config/android
CODEC_PLATFORM_DEP = ../third_party/dav1d/config/android/config.h
endif
# Third party: libvpx ships the VP8 and VP9 decoder sources (movie decoding, hle_movie.c), with its configuration headers in config/ (arm64, shared by Mac and iOS). Built separately into a static library, file by file, so host changes do not recompile it
VPX=../third_party/libvpx
VPX_SRC=$(sort $(shell find $(VPX) -name '*.c'))
VPX_OBJ=$(patsubst $(VPX)/%.c,$(CODEC_GEN)/vpx/%.o,$(VPX_SRC))
VPX_LIB=$(CODEC_GEN)/vpx/libvpx.a
$(CODEC_GEN)/vpx/%.o: $(VPX)/%.c $(wildcard $(VPX)/config/*.h) $(CODEC_PLATFORM_DEP) codecs.mk
	@mkdir -p $(dir $@)
	$(CODEC_CC) -O2 -g $(CODEC_CFLAGS) -I$(VPX) -I$(VPX)/config -c $< -o $@
$(VPX_LIB): $(VPX_OBJ)
	rm -f $@; $(CODEC_AR) rcs $@ $^
# Third party: dav1d, the AV1 decoder where VideoToolbox has none (hle_movie_av1.c), with its arm64 NEON assembly and its configuration in config/. Its *_tmpl.c files are built
# through the per-bit-depth wrappers in config/tmpl, and the assembly files other assembly files include (*_tmpl.S, util.S) are not built on their own: the list meson builds
DAV1D=../third_party/dav1d
DAV1D_SRC=$(sort $(filter-out %_tmpl.c,$(wildcard $(DAV1D)/src/*.c)) $(DAV1D)/src/arm/cpu.c $(wildcard $(DAV1D)/config/tmpl/*.c) \
          $(filter-out %_tmpl.S %/util.S,$(wildcard $(DAV1D)/src/arm/64/*.S)))
DAV1D_OBJ=$(patsubst $(DAV1D)/%,$(CODEC_GEN)/dav1d/%.o,$(DAV1D_SRC))
DAV1D_LIB=$(CODEC_GEN)/dav1d/libdav1d.a
DAV1D_H=$(wildcard $(DAV1D)/config/*.h $(DAV1D)/src/*.h $(DAV1D)/src/*_tmpl.c $(DAV1D)/src/arm/*.h $(DAV1D)/src/arm/asm.S $(DAV1D)/src/arm/64/*_tmpl.S $(DAV1D)/src/arm/64/util.S $(DAV1D)/include/*/*.h)
$(CODEC_GEN)/dav1d/%.o: $(DAV1D)/% $(DAV1D_H) $(CODEC_PLATFORM_DEP) codecs.mk
	@mkdir -p $(dir $@)
	$(CODEC_CC) -O2 -g $(CODEC_CFLAGS) -fvisibility=hidden -fomit-frame-pointer -fno-align-functions $(DAV1D_CONFIG_FLAGS) -I$(DAV1D) -I$(DAV1D)/include -I$(DAV1D)/config -c $< -o $@
$(DAV1D_LIB): $(DAV1D_OBJ)
	rm -f $@; $(CODEC_AR) rcs $@ $^

.PHONY: codec-libs
codec-libs: $(VPX_LIB) $(DAV1D_LIB)
