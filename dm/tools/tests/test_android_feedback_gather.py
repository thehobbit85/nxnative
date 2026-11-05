"""Consume exported feedback layouts without losing order or raw component bits."""
import ctypes
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[3]


@unittest.skipUnless(shutil.which('cc'), 'requires C compiler')
class FeedbackLayout(unittest.TestCase):
    def test_native_sidecar_maps_multiple_interleaved_buffers(self):
        with tempfile.TemporaryDirectory() as tmp:
            work = Path(tmp)
            source = work / 'check.c'
            source.write_text('''#include "dm/gfx/nvn/vulkan/feedback_gather.h"
int pack(const char *text, unsigned buffer, uint32_t *words) {
    dm_vk_feedback layout = {0};
    const char *line = text;
    while (*line) {
        const char *end = strchr(line, '\\n');
        size_t n = end ? (size_t)(end - line) : strlen(line);
        if (dm_vk_feedback_line(line, n, &layout)) return -1;
        line += n + (end != NULL);
    }
    dm_vk_feedback_gather control = {0};
    if (!dm_vk_feedback_gather_layout(&layout, buffer, &control)) return 0;
    memcpy(words, &control, sizeof control);
    return 1;
}
''')
            lib = work / 'check.so'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                            '-shared', '-fPIC', '-I', ROOT, source, '-o', lib], check=True, capture_output=True)
            pack = ctypes.CDLL(str(lib)).pack
            pack.argtypes = [ctypes.c_char_p, ctypes.c_uint, ctypes.POINTER(ctypes.c_uint32)]
            pack.restype = ctypes.c_int
            text = b'tf 0 36 7 2 0 3 7 4\ntf 1 4 31 1\ntf 3 20 0 1 31 4\n'
            for buffer, expected in ((0, [44, 45, 16, 17, 18, 44, 45, 46, 47]),
                                      (1, [140]), (3, [16, 140, 141, 142, 143])):
                words = (ctypes.c_uint32 * 520)()
                self.assertEqual(pack(text, buffer, words), 1)
                self.assertEqual(words[5], len(expected))
                self.assertEqual(list(words)[8:8 + len(expected)], expected)
            self.assertEqual(pack(text, 2, words), 0)
            self.assertEqual(pack(text, 4, words), 0)
            maximum = ('tf 0 2048' + ' 31 4' * 128 + '\n').encode()
            self.assertEqual(pack(maximum, 0, words), 1)
            self.assertEqual(words[5], 512)
            self.assertEqual(list(words)[8:], [140, 141, 142, 143] * 128)
            for bad in (b'tf 0 4 32 1\n', b'tf 0 4 0 2\n', b'tf 0 4 0 0\n'):
                self.assertEqual(pack(bad, 0, words), -1)


@unittest.skipUnless(all(shutil.which(t) for t in ('glslc', 'spirv-val')), 'requires shader tools')
class FeedbackShader(unittest.TestCase):
    def test_helper_validates_for_baseline_vulkan(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'gather.spv'
            subprocess.run(['glslc', '--target-env=vulkan1.1', ROOT / 'dm/gfx/nvn/vulkan/feedback_gather.comp',
                            '-o', binary], check=True, capture_output=True)
            subprocess.run(['spirv-val', '--target-env', 'vulkan1.1', binary], check=True, capture_output=True)


if __name__ == '__main__': unittest.main()
