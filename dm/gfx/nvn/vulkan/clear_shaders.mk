# Embed the renderer's clear shaders in the host. Games need no external
# shader pack or runtime compiler to clear an attachment.
CLEAR_HEADERS := $(addprefix $(CLEAR_GEN)/,clear.vert.inc clear_stencil.frag.inc)
$(CLEAR_GEN)/%.inc: $(CLEAR_SRC)/%
	@mkdir -p "$(@D)"
	"$(GLSLC)" --target-env=vulkan1.1 -O -mfmt=c "$<" -o "$@.tmp"
	@mv "$@.tmp" "$@"
