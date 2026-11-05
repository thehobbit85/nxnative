"""The scalar split-handle words already identified by shared reflection."""


def driver_array_fields(binding):
    sampler = binding if binding['type'] == 'samp' else binding.get('samp', {})
    for source in (binding, sampler):
        if source.get('kind') not in ('tex_c2a', 'samp_c2a'):
            continue
        offset = source.get('off')
        if type(offset) is not int or not 0 <= offset <= 0x7fffffff:
            raise ValueError('invalid driver array offset')
    if sampler.get('kind') != 'samp_c2a' or 'soff' not in sampler:
        return ()
    offset = sampler['soff']
    if type(offset) is not int or not 0 <= offset <= 0x7fffffff:
        raise ValueError('invalid fixed driver sampler offset')
    return (f'c2_or={offset}',)


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
