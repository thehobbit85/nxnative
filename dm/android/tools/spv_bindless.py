"""The scalar split-handle words already identified by shared reflection."""


def split_handle_fields(binding):
    # The Apple array path reads handles at off+8*k without a second word.
    if binding['count'] != 1:
        return ()
    sources = [('twin_tex', binding)] if binding['kind'] == 'tex_ubo' else []
    sampler = binding if binding['type'] == 'samp' else binding.get('samp', {})
    if sampler.get('kind') == 'samp_ubo':
        sources.append(('twin_samp', sampler))
    fields = []
    for name, source in sources:
        if 'soff' not in source and 'sreg' not in source:
            continue
        reg, off = source.get('sreg'), source.get('soff')
        if type(reg) is not int or type(off) is not int or not 0 <= reg < 16 or not 0 <= off <= 0x7fffffff:
            raise ValueError('invalid second uniform handle source')
        fields.append(f'{name}={reg}:{off}')
    return tuple(fields)
