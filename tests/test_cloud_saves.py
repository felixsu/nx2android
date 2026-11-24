"""Cloud saves (nxnative/dm/android/cloud_android.c): the parts with no network, compiled on the host. The save hash of tests/fixtures/cloud-save-tree must be
the game library's (its apps/server/test/fixtures/save-tree.sha256 holds the same value), and the archives must be plain ustar both ways."""
from pathlib import Path
import io
import shutil
import subprocess
import tarfile
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
AND = ROOT / 'nxnative/dm/android'
FIX = Path(__file__).resolve().parent / 'fixtures/cloud-save-tree'
GOLDEN = (Path(__file__).resolve().parent / 'fixtures/cloud-save-tree.sha256').read_text(encoding='utf-8').strip()

HARNESS = r'''
#include "cloud_android.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(int argc, char **argv) {
    char err[300] = "";
    if (!strcmp(argv[1], "hash")) { char hex[65]; long f; long long b; int r = dm_cloud_hash_tree(argv[2], argv[3], hex, &f, &b); printf("%d %s %ld %lld\n", r, hex[0] ? hex : "-", f, b); return 0; }
    if (!strcmp(argv[1], "tar")) { uint8_t *t; size_t n; if (dm_cloud_tar(argv[2], argv[3], &t, &n, err, sizeof err)) { printf("error %s\n", err); return 1; }
        FILE *f = fopen(argv[4], "wb"); fwrite(t, 1, n, f); fclose(f); free(t); printf("ok\n"); return 0; }
    if (!strcmp(argv[1], "untar")) { FILE *f = fopen(argv[2], "rb"); fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc((size_t)n + 1); fread(b, 1, (size_t)n, f); fclose(f); int dev;
        int r = dm_cloud_untar(b, (size_t)n, argv[3], argv[4], &dev, err, sizeof err); if (r) printf("error %s\n", err); else printf("ok %d\n", dev); free(b); return 0; }
    if (!strcmp(argv[1], "decide")) { const char *t[][3] = {{0,0,0},{"a",0,0},{0,"a","a"},{"a","x","a"},{"b","a","a"},{"a","a","b"},{"b","a","c"},{"b",0,"c"}};
        for (int i = 0; i < 8; i++) printf("%d ", dm_cloud_decide(t[i][0], t[i][1], t[i][2])); printf("\n"); return 0; }
    if (!strcmp(argv[1], "json")) { char out[200]; int r = dm_cloud_json(argv[2], argv[3], out, sizeof out); printf("%d [%s]\n", r, out); return 0; }
    return 2;
}
'''


class CloudSaves(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            raise unittest.SkipTest('a C compiler is needed for the cloud-saves check')
        cls.tmp = tempfile.TemporaryDirectory()
        d = Path(cls.tmp.name)
        (d / 'h.c').write_text(HARNESS, encoding='utf-8')
        cls.exe = d / 'h'
        r = subprocess.run([cc, '-std=gnu11', '-O1', '-DDM_CLOUD_HOST_TEST', f'-I{AND / "compat/include"}', f'-I{AND}', str(d / 'h.c'), str(AND / 'cloud_android.c'),
                            str(AND / 'compat/sha256.c'), '-o', str(cls.exe)], capture_output=True, text=True)
        if r.returncode:
            raise AssertionError(r.stderr)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_h(self, *args):
        return subprocess.run([str(self.exe), *map(str, args)], capture_output=True, text=True, check=True).stdout.strip()

    def test_hash_is_the_librarys(self):
        r, hexd, files, size = self.run_h('hash', FIX / 'save', FIX / 'save_device').split()
        self.assertEqual((r, hexd), ('0', GOLDEN))
        self.assertEqual(int(files), 7)

    def test_no_save_has_no_hash(self):
        with tempfile.TemporaryDirectory() as d:
            self.assertEqual(self.run_h('hash', Path(d) / 'none', Path(d) / 'none_device').split()[:2], ['0', '-'])

    def test_archive_reads_as_ustar_and_back(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            self.assertEqual(self.run_h('tar', FIX / 'save', FIX / 'save_device', d / 'a.tar'), 'ok')
            with tarfile.open(d / 'a.tar') as t:
                self.assertEqual(t.getmember('save/sub/data.bin').size, 3000)
                self.assertTrue(t.getmember('save_device/').isdir() if 'save_device/' in t.getnames() else t.getmember('save_device').isdir())
                self.assertEqual(t.extractfile('save/main').read(), (FIX / 'save/main').read_bytes())
                self.assertIn('save/ポケモン.dat', t.getnames())
            self.assertEqual(self.run_h('untar', d / 'a.tar', d / 'out/save', d / 'out/save_device'), 'ok 1')
            self.assertEqual(self.run_h('hash', d / 'out/save', d / 'out/save_device').split()[1], GOLDEN)

    def test_untar_python_archive_with_long_names(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            long = 'save/' + 'd' * 90 + '/' + 'e' * 60 + '/file.bin'
            with tarfile.open(d / 'p.tar', 'w', format=tarfile.USTAR_FORMAT) as t:
                for name, data in ((long, b'x' * 700), ('save/main', b'main')):
                    i = tarfile.TarInfo(name); i.size = len(data); t.addfile(i, io.BytesIO(data))
            self.assertEqual(self.run_h('untar', d / 'p.tar', d / 'o/save', d / 'o/save_device'), 'ok 0')
            self.assertEqual((d / 'o' / long).read_bytes(), b'x' * 700)

    def test_untar_refuses_paths_outside_the_save(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            for bad in ('save/../escape', 'temp/x', '/abs'):
                with tarfile.open(d / 'b.tar', 'w', format=tarfile.USTAR_FORMAT) as t:
                    i = tarfile.TarInfo(bad); i.size = 1; t.addfile(i, io.BytesIO(b'x'))
                self.assertTrue(self.run_h('untar', d / 'b.tar', d / 'o/save', d / 'o/save_device').startswith('error'), bad)
            self.assertFalse((d / 'escape').exists())

    def test_decide_is_the_librarys_rule(self):
        # both-empty, cloud-empty, local-empty, in-sync, local-newer, cloud-newer, conflict, conflict (no base)
        self.assertEqual(self.run_h('decide'), '0 1 2 3 4 5 6 6')

    def test_json_fields(self):
        j = '{"state":"cloud-newer","latest":{"id":"20261124-1.tar","device":"Felix\\u2019s \\"Mac\\"","size":1234,"uploadedAt":1764000000000}}'
        self.assertEqual(self.run_h('json', j, 'state'), '0 [cloud-newer]')
        self.assertEqual(self.run_h('json', j, 'device'), '0 [Felix’s "Mac"]')
        self.assertEqual(self.run_h('json', j, 'size'), '0 [1234]')
        self.assertEqual(self.run_h('json', j, 'missing'), '-1 []')


if __name__ == '__main__':
    unittest.main()
