import pathlib
import struct
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1] / "tools" / "diagnostics"))
import rdn_ib


class SavedSubmissionTests(unittest.TestCase):
    def test_index_swap_and_draw(self):
        words = [0xC0002A00, 4, 0xC0032B00, 0x100, 0xF, 3, 0, 0x80000000]
        report = rdn_ib.decode(struct.pack(">8I", *words))
        memory = bytes(256) + struct.pack(">3H", 16, 17, 459)
        rdn_ib.check_indices(report, memory, 0xF00000000)
        self.assertEqual(report["draws"][0]["indices"],
                         {"status": "decoded", "min": 16, "max": 459})
        self.assertEqual(report["packets"]["TYPE2_NOP"], 1)

    def test_truncated_packet(self):
        with self.assertRaisesRegex(ValueError, "truncated packet"):
            rdn_ib.decode(struct.pack(">2I", 0xC0032B00, 0))
        with self.assertRaisesRegex(ValueError, "multiple of four"):
            rdn_ib.decode(b"abc")

    def test_uncaptured_memory_is_not_bad_indices(self):
        report = rdn_ib.decode(struct.pack(">5I", 0xC0032B00, 0, 0xF, 3, 0))
        rdn_ib.check_indices(report, b"", 0xF00000000)
        self.assertEqual(report["draws"][0]["indices"]["status"], "not captured")

    def test_capture_byte_orders_and_failure_status(self):
        header = [0x52444E31, 1, 16, 12, 356, 42, 0x1000, 1, 1,
                  0, 0x25C1000, 0xFFFFFFF4, 0, 1, 0, 0]
        buffer = [7, 0, 0, 2, 0xF, 0x3000000, 0, 4096, 100, 90, 1, 1]
        for endian in (">", "<"):
            data = struct.pack(endian + "29I", *(header + buffer + [0x80000000]))
            report = rdn_ib.read_capture(data)
            self.assertEqual(report["submission"]["result"], -12)
            self.assertEqual(report["submission"]["buffers"][0]["address"], 0xF03000000)
            self.assertEqual(report["submission"]["buffers"][0]["last_use"], 100)
            with self.assertRaisesRegex(ValueError, "capture length"):
                rdn_ib.read_capture(data[:-4])

    def test_empty_register_write_is_reported(self):
        report = rdn_ib.decode(struct.pack(">2I", 0xC0006900, 0x191))
        self.assertEqual(len(report["warnings"]), 1)

    def test_fetch_bounds_use_active_inputs_and_base_vertex(self):
        memory = struct.pack("<8I", 2, 0x80800000, 0, 0x85000000,
                             0x7C000000, 48 << 22, 0xA0000, 0)
        draw = dict(count=3, indices=dict(status="decoded", min=5, max=7),
                    base_vertex=0xFFFFFFFC, restart_enabled=0,
                    vertex_index_limits=[0, 0xFFFFFFFF], fetch=0x100,
                    vertex_resources=[dict(slot=0, stride=16, size=60),
                                      dict(slot=1, stride=16, size=1)])
        report = dict(draws=[draw])
        rdn_ib.check_fetch_bounds(report, memory, 0x100)
        self.assertEqual(len(draw["fetch_bounds"]), 1)
        self.assertEqual(draw["fetch_bounds"][0]["end_byte"], 60)
        self.assertEqual(draw["fetch_bounds"][0]["status"], "within descriptor")
        draw["vertex_resources"][0]["size"] = 59
        rdn_ib.check_fetch_bounds(report, memory, 0x100)
        self.assertEqual(draw["fetch_bounds"][0]["status"], "outside descriptor")

    def test_fetch_parser_rejects_missing_memory_and_computed_indices(self):
        with self.assertRaisesRegex(ValueError, "not captured"):
            rdn_ib.fetch_inputs(b"", 0, 0)
        memory = struct.pack("<8I", 2, 0x80800000, 0, 0x85000000,
                             0x7C010000, 48 << 22, 0xA0000, 0)
        with self.assertRaisesRegex(ValueError, "unsupported indexing"):
            rdn_ib.fetch_inputs(memory, 0, 0)


if __name__ == "__main__":
    unittest.main()
