#!/usr/bin/env python3
"""EXTFLASH_HOST=... python3 tools/tests/test_put_wad.py
tools/put_wad.py 对着 extdump v2 的内存假设备 (cs80-extflash/tests/fake_device.py) 跑: 不连设备。
  python3 tools/tests/test_put_wad.py [真 WAD 路径]"""
import os, sys, tempfile, unittest
HERE = os.path.dirname(os.path.abspath(__file__))
EXT = os.environ.get("EXTFLASH_HOST") or sys.exit("要设 EXTFLASH_HOST=<extdump v2 工具>/host (它的 ../tests/fake_device.py 也要在)")
sys.path.insert(0, EXT); sys.path.insert(0, os.path.join(EXT, "..", "tests")); sys.path.insert(0, os.path.join(HERE, ".."))
import extlink as L
from fake_device import FakeDevice
import put_wad

STOCK = {"EFS:/systemparam.txt": b"x" * 100, "EFS:/pay_record.dat": b"y" * 50}
WAD = sys.argv.pop(1) if len(sys.argv) > 1 else None


class T(unittest.TestCase):
    def run_put(self, data, extra=(), total=8 << 20):
        dev = FakeDevice(STOCK, total=total)
        L.open_transport = lambda spec, baud=115200: dev
        with tempfile.NamedTemporaryFile(suffix=".wad", delete=False) as f:
            f.write(data)
        try:
            sys.argv = ["put_wad.py", "--port", "usb", f.name, *extra]
            try:
                put_wad.main(); code = 0
            except SystemExit as e:
                code = e.code or 0
        finally:
            os.remove(f.name)
        return dev, code

    def test_upload_and_verify(self):
        data = open(WAD, "rb").read() if WAD else b"IWAD" + os.urandom(300 * 1024)
        dev, code = self.run_put(data)
        self.assertEqual(code, 0)
        self.assertEqual(dev.fs.get("EFS:/monita/doom1.wad"), data)
        for k, v in STOCK.items():
            self.assertEqual(dev.fs[k], v, "原厂文件不能动")
        self.assertEqual(set(dev.fs) - set(STOCK), {"EFS:/monita/doom1.wad"}, "只多这一个文件 (.part 已改名)")
        for c in dev.calls:
            self.assertTrue(any(isinstance(x, str) and x.startswith("EFS:/monita") for x in c[1:]) or len(c) < 2, c)

    def test_freedoom_name(self):
        dev, code = self.run_put(b"IWAD" + b"\0" * 100 + b"FREEDOOM" + os.urandom(1000))
        self.assertEqual(code, 0)
        self.assertIn("EFS:/monita/freedoom1.wad", dev.fs)

    def test_not_a_wad(self):
        dev, code = self.run_put(b"PK\3\4zip")
        self.assertNotEqual(code, 0)
        self.assertEqual(set(dev.fs), set(STOCK))

    def test_no_space(self):
        dev, code = self.run_put(b"IWAD" + os.urandom(200 * 1024), total=300 * 1024)
        self.assertNotEqual(code, 0)
        self.assertEqual(set(dev.fs), set(STOCK))


if __name__ == "__main__":
    unittest.main()
