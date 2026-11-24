"""Compile the production BC1-to-ASTC rewrite and RGBA-to-ASTC encoder (gfx_vk_astc.c) and check their blocks against ones an independent ASTC decoder was run on."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('clang') or shutil.which('gcc')


@unittest.skipUnless(CC, 'the BC1-to-ASTC harness needs a host C compiler')
class BC1AsASTC(unittest.TestCase):
    def test_bc1_blocks_become_the_expected_astc_blocks(self):
        with tempfile.TemporaryDirectory(prefix='nx2a_astc_') as tmp:
            exe = Path(tmp) / ('astc.exe' if os.name == 'nt' else 'astc')
            build = subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT / 'tests/vk_astc_harness.c'),
                                    str(ROOT / 'nxnative/dm/android/gfx_vk_astc.c'), '-pthread', '-lm', '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            result = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('40 cases passed', result.stdout)


if __name__ == '__main__':
    unittest.main()
