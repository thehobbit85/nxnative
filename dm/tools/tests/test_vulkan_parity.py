"""Every record the production NVN backends can receive has a named parity test plan."""
from pathlib import Path
import re
import unittest

ROOT = Path(__file__).resolve().parents[3]
NVN = ROOT / 'dm/gfx/nvn'


def dispatch_cases(path, signature):
    source = path.read_text().split(signature, 1)[1].split('\n}', 1)[0]
    return set(re.findall(r'\bcase (OP_\w+):', source))


class VulkanParityTests(unittest.TestCase):
    def test_inventory_tracks_production_records(self):
        header = (NVN / 'nvn_obj.h').read_text()
        enum = re.search(r'enum\s*\{\s*(OP_END\b.*?OP_COUNT_)\s*\}', header, re.S)
        self.assertIsNotNone(enum)
        names = re.findall(r'\bOP_\w+', re.sub(r'//[^\n]*', '', enum[1]))
        active = set(names) - {'OP_BEGIN', 'OP_COUNT_'}
        matrix = (ROOT / 'docs/nvn-vulkan-parity.md').read_text()
        rows = re.findall(r'^\| `(OP_\w+)` \| (yes|no) \|', matrix, re.M)
        self.assertCountEqual([name for name, _ in rows], active)
        metal = dispatch_cases(NVN / 'nvn_mtl.m', 'void mtl_exec(const uint8_t *p) {')
        vulkan = dispatch_cases(NVN / 'vulkan/nvn_vk.c', 'static void mtl_exec_(const uint8_t *p) {')
        self.assertEqual(active, metal - {'OP_BEGIN'})
        self.assertEqual({name for name, has_case in rows if has_case == 'yes'}, vulkan)


if __name__ == '__main__':
    unittest.main()
