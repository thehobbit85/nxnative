# Embed the renderer's clear shaders in the host. Games need no external
# shader pack or runtime compiler to clear an attachment.
CLEAR_COLOR_HEADERS := $(addprefix $(CLEAR_GEN)/clear_color.,vec4.inc ivec4.inc uvec4.inc)
CLEAR_HEADERS := $(addprefix $(CLEAR_GEN)/,clear.vert.inc clear_stencil.frag.inc resolve_depth.frag.inc) $(CLEAR_COLOR_HEADERS)
$(CLEAR_COLOR_HEADERS): $(CLEAR_GEN)/clear_color.%.inc: $(CLEAR_SRC)/clear_color.frag
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c -DCOLOR_TYPE=$* "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
$(CLEAR_GEN)/%.inc: $(CLEAR_SRC)/%
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
