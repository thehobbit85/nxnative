# Embed the renderer's internal shaders in the host. Games need no external
# shader pack or runtime compiler to clear an attachment.
CLEAR_COLOR_HEADERS := $(addprefix $(CLEAR_GEN)/clear_color.,vec4.inc ivec4.inc uvec4.inc)
GEOMETRY_PACK_HEADERS := $(addprefix $(CLEAR_GEN)/geometry_pack.,0.inc 1.inc 2.inc)
CLEAR_HEADERS := $(addprefix $(CLEAR_GEN)/,clear.vert.inc clear_stencil.frag.inc resolve_depth.frag.inc blit_color.frag.inc draw_texture.frag.inc temporal_prepare.frag.inc temporal_resolve.frag.inc packed_depth.comp.inc feedback_prepare.comp.inc feedback_gather.comp.inc) $(CLEAR_COLOR_HEADERS)
$(CLEAR_COLOR_HEADERS): $(CLEAR_GEN)/clear_color.%.inc: $(CLEAR_SRC)/clear_color.frag
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c -DCOLOR_TYPE=$* "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
CLEAR_HEADERS += $(GEOMETRY_PACK_HEADERS)
$(GEOMETRY_PACK_HEADERS): $(CLEAR_GEN)/geometry_pack.%.inc: $(CLEAR_SRC)/geometry_pack.comp
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c -DPASS=$* "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
$(CLEAR_GEN)/%.inc: $(CLEAR_SRC)/%
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
