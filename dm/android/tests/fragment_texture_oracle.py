"""Independently check saved fragment_texture outputs, including filtered modes.

The standard result is always the pass criterion. A separate SwiftShader model
only diagnoses the known accumulated-offset bug; matching it never grants a pass.
"""
import argparse
import json
import math
from pathlib import Path
import struct


def address(index, size, repeat):
    return index % size if repeat else max(0, min(size - 1, index))


def sample(u, v, width, height, mip, repeat, linear):
    def value(x, y):
        return 10000 * mip + 100 * address(y, height, repeat) + address(x, width, repeat) + 1
    if not linear: return value(math.floor(u * width), math.floor(v * height))
    x, y = u * width - .5, v * height - .5
    ix, iy = math.floor(x), math.floor(y); fx, fy = x - ix, y - iy
    return ((value(ix, iy) * (1 - fx) + value(ix + 1, iy) * fx) * (1 - fy) +
            (value(ix, iy + 1) * (1 - fx) + value(ix + 1, iy + 1) * fx) * fy)


def expected(variant, mode, case, x, y, accumulated_offset=False):
    width, height, levels, base = (16, 8, 5, 1) if mode == 5 else (8, 8, 4, 0)
    minimum, maximum = (1.25, 2.5) if mode == 4 else (0, 3)
    min_linear, mag_linear, mip_linear = mode not in (0, 4), mode not in (0, 3), mode > 1
    scale = (.03125, .0625, .125, .25, .5)[case // 8]
    bias = (-1.25, 0, .375, 1.375)[(case // 2) % 4] if variant else 0
    lod = max(minimum, min(maximum, math.log2(max(width >> base, height >> base) * scale) + bias))
    linear = mag_linear if lod <= 0 else min_linear
    lo = min(levels - base - 1, math.floor(lod) if mip_linear else math.floor(lod + .5))
    hi = min(levels - base - 1, lo + 1); fraction = lod - math.floor(lod) if mip_linear else 0
    lw, lh = max(1, width >> (base + lo)), max(1, height >> (base + lo))
    hw, hh = max(1, width >> (base + hi)), max(1, height >> (base + hi))
    ox, oy = x if x < 8 else x - 16, y if y < 8 else y - 16
    u, v = (x + .5) * scale, (y + .5) * scale
    low = sample(u + ox / lw, v + oy / lh, lw, lh, base + lo, case % 2, linear)
    high = sample(u + ox / hw + (ox / lw if accumulated_offset else 0),
                  v + oy / hh + (oy / lh if accumulated_offset else 0), hw, hh, base + hi, case % 2, linear)
    return low * (1 - fraction) + high * fraction


def check(data, variant, mode):
    if variant not in (0, 1) or mode not in range(6): raise ValueError('invalid fixture variant or mode')
    if len(data) != 40 * 16 * 16 * 16: raise ValueError('incomplete fragment output')
    words = struct.unpack('<40960f', data); errors = driver_errors = 0; first = None
    for case in range(40):
        for y in range(16):
            for x in range(16):
                standard = expected(variant, mode, case, x, y)
                driver = expected(variant, mode, case, x, y, True)
                for component in range(4):
                    actual = words[(case * 256 + y * 16 + x) * 4 + component]
                    want = standard + 1000 * component
                    if actual != want:
                        errors += 1
                        if first is None: first = dict(case=case, x=x, y=y, component=component, actual=actual, expected=want)
                    driver_errors += actual != driver + 1000 * component
    return dict(standard_match=errors == 0, differences=errors, first=first,
                matches_accumulated_offset_model=driver_errors == 0, driver_model_differences=driver_errors)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__); parser.add_argument('file', type=Path)
    parser.add_argument('variant', type=int, choices=(0, 1)); parser.add_argument('mode', type=int, choices=range(6))
    args = parser.parse_args(); result = check(args.file.read_bytes(), args.variant, args.mode)
    print(json.dumps(result, indent=2)); raise SystemExit(0 if result['standard_match'] else 1)
