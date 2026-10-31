import importlib.util
import json
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "package_gld", Path(__file__).resolve().parents[1] / "scripts/package-leopard-gld.py")
gld = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gld)


def slice_for(cpu, exports=None):
    exports = gld.EXPORTS if exports is None else exports
    header = 28 if cpu == 18 else 32
    entry = 12 if cpu == 18 else 16
    strings, symbols = bytearray(b"\0"), bytearray()
    for name in sorted(exports):
        symbols.extend(struct.pack(">IBBHI" if cpu == 18 else ">IBBHQ",
                                   len(strings), 0x0f, 1, 0, 1))
        strings.extend(name.encode() + b"\0")
    symoff = header + 24
    fields = [0xfeedface if cpu == 18 else 0xfeedfacf, cpu, 0, 8, 1, 24, 0]
    if cpu != 18:
        fields.append(0)
    return (struct.pack(">" + "I" * len(fields), *fields) +
            struct.pack(">6I", 2, 24, symoff, len(symbols) // entry,
                        symoff + len(symbols), len(strings)) + symbols + strings)


class LeopardPackageTest(unittest.TestCase):
    def test_thin_rejected_as_installable(self):
        for cpu in gld.CPUS:
            with self.assertRaises(ValueError):
                gld.universal(slice_for(cpu))

    def test_wrong_architecture(self):
        with self.assertRaises(ValueError):
            gld.thin(slice_for(18), gld.CPUS[1])

    def test_missing_video_export(self):
        with self.assertRaises(ValueError):
            gld.thin(slice_for(18, {"_RDNGLMakeCurrent"}), 18)

    def test_truncated_symbol_table(self):
        with self.assertRaises(ValueError):
            gld.thin(slice_for(18)[:-1], 18)

    def test_all_modules_and_slice_hashes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dirs = [root / "32", root / "64"]
            for path, cpu in zip(dirs, gld.CPUS):
                path.mkdir()
                for name in gld.MODULES:
                    (path / (name + ".dylib")).write_bytes(slice_for(cpu))
            output = root / "universal"
            manifest = gld.package(*dirs, output)
            self.assertEqual(manifest, json.loads((output / "manifest.json").read_text()))
            for name in gld.MODULES:
                data = bytearray((output / name).read_bytes())
                self.assertEqual(len(gld.universal(data)), 2)
                struct.pack_into(">I", data, 28, 18)
                with self.assertRaises(ValueError):
                    gld.universal(data)

    def test_validation_precedes_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            dirs = [root / "32", root / "64"]
            for path, cpu in zip(dirs, gld.CPUS):
                path.mkdir()
                for name in gld.MODULES:
                    (path / (name + ".dylib")).write_bytes(slice_for(cpu))
            (dirs[1] / "RadeonNIGLDriver3.dylib").write_bytes(slice_for(18))
            with self.assertRaises(ValueError):
                gld.package(*dirs, root / "output")
            self.assertFalse((root / "output").exists())


if __name__ == "__main__":
    unittest.main()
