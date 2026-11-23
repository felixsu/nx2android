"""Compile the production vs_layer_gs rewrite (gfx_vk_vs_layer.c) against a synthetic vertex shader; spirv-val checks its modules when installed."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CC = shutil.which(os.environ.get('CC', 'cc')) or shutil.which('clang') or shutil.which('gcc')
SPIRV_VAL = shutil.which('spirv-val')


@unittest.skipUnless(CC, 'the vs_layer_gs harness needs a host C compiler')
class VertexLayerGeometryShader(unittest.TestCase):
    def test_layer_moves_to_a_location_and_a_geometry_shader_writes_it(self):
        with tempfile.TemporaryDirectory(prefix='nx2a_vs_layer_') as tmp:
            exe = Path(tmp) / ('vs_layer.exe' if os.name == 'nt' else 'vs_layer')
            build = subprocess.run([CC, '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                                    str(ROOT / 'tests/vk_vs_layer_harness.c'),
                                    str(ROOT / 'nxnative/dm/android/gfx_vk_vs_layer.c'), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stdout + build.stderr)
            out = Path(tmp) / 'spv'
            out.mkdir()
            result = subprocess.run([str(exe), str(out)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn('100 cases passed', result.stdout)
            modules = sorted(out.glob('*.spv'))
            self.assertEqual(len(modules), 7)   # the vertex shader, and the geometry shaders for 3 primitives with and without point size
            if SPIRV_VAL:
                for m in modules:
                    v = subprocess.run([SPIRV_VAL, '--target-env', 'vulkan1.2', str(m)], capture_output=True, text=True)
                    self.assertEqual(v.returncode, 0, f'{m.name}: {v.stdout}{v.stderr}')


if __name__ == '__main__':
    unittest.main()
