"""ROM discovery and bounded vendor-range extraction checks."""

import io
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from scripts import romdump


def sparse(total_blocks, chunks):
    header = struct.pack('<IHHHHIIII', romdump.SPARSE_MAGIC, 1, 0, 28, 12,
                         4096, total_blocks, len(chunks), 0)
    result = bytearray(header)
    for kind, count, payload in chunks:
        result.extend(struct.pack('<HHII', kind, 0, count, 12 + len(payload)))
        result.extend(payload)
    return bytes(result)


class RomDumpChecks(unittest.TestCase):
    def test_sparse_overlay_extracts_only_requested_ranges(self):
        first = sparse(8, [(romdump.RAW, 1, b'A' * 4096),
                           (romdump.SKIP, 7, b'')])
        second = sparse(8, [(romdump.SKIP, 2, b''),
                            (romdump.RAW, 2, b'B' * 8192),
                            (romdump.SKIP, 4, b'')])
        target = io.BytesIO(b'\0' * 8192)
        wanted = [(2 * 4096, 4 * 4096, 0)]
        romdump.copy_sparse(io.BytesIO(first), target, wanted)
        romdump.copy_sparse(io.BytesIO(second), target, wanted)
        self.assertEqual(target.getvalue(), b'B' * 8192)

    def test_raw_split_can_cross_chunk_and_vendor_extent_boundaries(self):
        target = io.BytesIO(b'\0' * 12)
        wanted = [(3, 9, 0), (11, 17, 6)]
        romdump.copy_intersection(io.BytesIO(b'abcdefgh'), target, 0, 8, wanted)
        romdump.copy_intersection(io.BytesIO(b'ijklmnop'), target, 8, 8, wanted)
        romdump.copy_intersection(io.BytesIO(b'q'), target, 16, 1, wanted)
        self.assertEqual(target.getvalue(), b'defghilmnopq')

    def test_super_chunks_must_be_complete_and_boot_is_found_inside_zip(self):
        with tempfile.TemporaryDirectory() as temp:
            archive = Path(temp) / 'rom.zip'
            with zipfile.ZipFile(archive, 'w') as out:
                out.writestr('Firmware/boot.img', b'ANDROID!')
                out.writestr('Firmware/super.img_sparsechunk.0', b'zero')
                out.writestr('Firmware/super.img_sparsechunk.2', b'two')
            with romdump.RomDump(archive) as rom:
                self.assertEqual(rom.boot_image(Path(temp) / 'boot').read_bytes(), b'ANDROID!')
                with self.assertRaisesRegex(romdump.RomError, 'noncontiguous'):
                    rom.super_members()

    def test_lpdump_vendor_selection(self):
        output = '''  Name: vendor_a
  Extents:
    0 .. 1 linear super 10
------------------------
  Name: vendor_b
  Extents:
    0 .. 1 linear super 20
------------------------'''
        with self.assertRaisesRegex(romdump.RomError, 'vendor partition'):
            romdump.parse_lpdump(output)
        self.assertEqual(romdump.parse_lpdump(output, 'b'),
                         ('vendor_b', [(20 * 512, 22 * 512, 0)]))


if __name__ == '__main__':
    unittest.main()
