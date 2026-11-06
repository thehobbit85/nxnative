// GLSL buffer coordinates and sizes can be signed. OpUDiv requires an
// unsigned result even when both representations have the same width.
static int texel_udiv(texel_module *m,uint32_t type,uint32_t result,uint32_t a,uint32_t b) {
    if (!m->uint_type) { m->uint_type=m->next++; TEXEL_OP(&m->globals,21,m->uint_type,32,0); }
    if (type==m->uint_type) { TEXEL_OP(&m->body,134,type,result,a,b); }
    else {
        uint32_t ua=m->next++, ub=m->next++, quotient=m->next++;
        TEXEL_OP(&m->body,124,m->uint_type,ua,a);
        TEXEL_OP(&m->body,124,m->uint_type,ub,b);
        TEXEL_OP(&m->body,134,m->uint_type,quotient,ua,ub);
        TEXEL_OP(&m->body,124,type,result,quotient);
    }
    return 1;
fail:
    return 0;
}

// A guest coordinate outside the view must stay out of range even if the
// multiplication by the component count or addition of a prefix wraps.
static uint32_t texel_coordinate(texel_module *m,uint32_t type,uint32_t coord,uint32_t stride,uint32_t extra) {
    uint32_t last=texel_constant(m,type,UINT32_MAX);
    if (!last) return 0;
    if (stride) {
        uint32_t limit=m->next++, wide=m->next++, wrap=m->next++, selected=m->next++;
        if (!texel_udiv(m,type,limit,last,stride)) return 0;
        TEXEL_OP(&m->body,132,type,wide,coord,stride);
        TEXEL_OP(&m->body,172,m->boolean,wrap,coord,limit);
        TEXEL_OP(&m->body,169,type,selected,wrap,last,wide);
        coord=selected;
    }
    if (extra) {
        uint32_t sum=m->next++, wrap=m->next++, selected=m->next++;
        TEXEL_OP(&m->body,128,type,sum,coord,extra);
        TEXEL_OP(&m->body,176,m->boolean,wrap,sum,extra);
        TEXEL_OP(&m->body,169,type,selected,wrap,last,sum);
        coord=selected;
    }
    return coord;
fail:
    return 0;
}

typedef struct { uint32_t value[4], code_type, code; } texel_fetch_parts;

static uint32_t texel_wide_value(texel_module *m,const uint32_t *op,size_t original,uint32_t coord_type,
                                 uint32_t stride,uint32_t vector,uint32_t scalar,uint32_t encoding,texel_fetch_parts *parts) {
    unsigned n=encoding>>TEXEL_WORD_SHIFT;
    if (n>4 || (n>1 && !stride)) return 0;
    if (!m->uint_type) { m->uint_type=m->next++; TEXEL_OP(&m->globals,21,m->uint_type,32,0); }
    if (!m->uint4) { m->uint4=m->next++; TEXEL_OP(&m->globals,23,m->uint4,m->uint_type,4); }
    uint32_t mult=texel_float4(m,vector,scalar,(float[]){65535,65535,65535,65535});
    uint32_t half=texel_float4(m,vector,scalar,(float[]){.5f,.5f,.5f,.5f});
    uint32_t shift=texel_constant(m,m->uint_type,16), lane[4]={0};
    if (!mult || !half || !shift) return 0;
    for (unsigned c=0;c<4;c++) {
        if (c>=n) {
            lane[c]=texel_constant(m,m->uint_type,c==3);
            if (!lane[c]) return 0;
            continue;
        }
        if (!parts->value[c]) {
            // For shorter members of a dynamic descriptor array, repeat
            // word zero instead of speculatively reading beyond its end.
            uint32_t step=texel_constant(m,coord_type,c), zero=texel_constant(m,coord_type,0);
            if (!step || !zero) return 0;
            uint32_t inside=m->next++, delta=m->next++;
            TEXEL_OP(&m->body,172,m->boolean,inside,stride,step);
            TEXEL_OP(&m->body,169,coord_type,delta,inside,step,zero);
            uint32_t coord=texel_coordinate(m,coord_type,m->body.w[original+4],0,delta);
            if (!coord) return 0;
            size_t ins=m->body.n;
            if (!texel_append(&m->body,op,op[0]>>16)) return 0;
            uint32_t fetch=m->next++;
            m->body.w[ins+2]=fetch; m->body.w[ins+4]=coord;
            if (parts->code_type) {
                // Preserve a nonresident status from any word of the guest texel.
                if (!parts->code) {
                    parts->code=m->next++;
                    TEXEL_OP(&m->body,81,parts->code_type,parts->code,m->body.w[original+2],0);
                }
                uint32_t code=m->next++, resident=m->next++, merged=m->next++, data=m->next++;
                TEXEL_OP(&m->body,81,parts->code_type,code,fetch,0);
                TEXEL_OP(&m->body,316,m->boolean,resident,code); // OpImageSparseTexelsResident
                TEXEL_OP(&m->body,169,parts->code_type,merged,resident,parts->code,code);
                TEXEL_OP(&m->body,81,vector,data,fetch,1);
                parts->code=merged; fetch=data;
            }
            parts->value[c]=fetch;
        }
        uint32_t product=m->next++, rounded=m->next++, words=m->next++;
        TEXEL_OP(&m->body,133,vector,product,parts->value[c],mult);
        TEXEL_OP(&m->body,129,vector,rounded,product,half);
        TEXEL_OP(&m->body,109,m->uint4,words,rounded);
        uint32_t low=m->next++, high=m->next++, upper=m->next++;
        TEXEL_OP(&m->body,81,m->uint_type,low,words,0);
        TEXEL_OP(&m->body,81,m->uint_type,high,words,1);
        TEXEL_OP(&m->body,196,m->uint_type,upper,high,shift);
        lane[c]=m->next++;
        TEXEL_OP(&m->body,197,m->uint_type,lane[c],upper,low);
    }
    uint32_t bits=m->next++, value=m->next++;
    TEXEL_OP(&m->body,80,m->uint4,bits,lane[0],lane[1],lane[2],lane[3]);
    if (encoding & (1u<<20)) {
        if (!m->sint_type) { m->sint_type=m->next++; TEXEL_OP(&m->globals,21,m->sint_type,32,1); }
        if (!m->sint4) { m->sint4=m->next++; TEXEL_OP(&m->globals,23,m->sint4,m->sint_type,4); }
        uint32_t signed_bits=m->next++;
        TEXEL_OP(&m->body,124,m->sint4,signed_bits,bits);
        TEXEL_OP(&m->body,111,vector,value,signed_bits);
    } else { TEXEL_OP(&m->body,112,vector,value,bits); }
    return value;
fail:
    return 0;
}
