#!/usr/bin/env python3
"""Validate the legacy UVD fixture without submitting GPU commands."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess
import tempfile

COMMIT = "3933a57341a729a424fd6823af0c14fd104b95d1"
SOURCE_URL = (
    "https://android.googlesource.com/platform/external/libdrm/+/"
    + COMMIT + "/tests/amdgpu/uvd_messages.h"
)
HASHES = {
    "create": "aece999f84c9b15bcf165aadbd9552d41dbbd6a73d6f870284e918affad6d9bc",
    "decode": "6d751db1fc9108e6289ac379c37f420bcc0c551fec790222ddefe831f31d74ea",
    "destroy": "54b54a8a08422c68a724677c54d23514d327ae48dd19ba8349e9c3e2d728684e",
    "bitstream": "00e5e28eeecb74c4ba6252b298391d19de3626c8ea0e8610892a61b0a9f10f22",
}
REFERENCE_SHA256 = "f8128078aebc6e5dcf4f80afecd8ff1f01ce3f82ae6d543bb9dcb828f496250e"
WIDTH, HEIGHT, PITCH = 864, 480, 1024
TARGET_BYTES, NV12_BYTES = 737280, 622080


def require(condition, description):
    if not condition:
        raise AssertionError(description)


def load_arrays():
    source = Path(__file__).with_name("uvd_decode_fixture.c").read_text()
    arrays = {}
    for name, expected_hash in HASHES.items():
        match = re.search(
            r"\brdn_uvd_fixture_" + name + r"\[\d+\]\s*=\s*\{(.*?)\};",
            source, re.S)
        require(match is not None, "missing array " + name)
        data = bytes(int(v, 16) for v in re.findall(r"0x([0-9a-fA-F]+)", match[1]))
        require(hashlib.sha256(data).hexdigest() == expected_hash,
                "upstream bytes changed: " + name)
        arrays[name] = data
    return arrays


def le32(data, offset):
    return int.from_bytes(data[offset:offset + 4], "little")


class Bits:
    def __init__(self):
        self.bits = ""

    def u(self, value, count):
        require(0 <= value < (1 << count), "bit field out of range")
        self.bits += format(value, "0%db" % count)
        return self

    def ue(self, value):
        require(value >= 0, "negative unsigned Exp-Golomb value")
        encoded = bin(value + 1)[2:]
        self.bits += "0" * (len(encoded) - 1) + encoded
        return self

    def se(self, value):
        return self.ue(2 * abs(value) - (1 if value > 0 else 0))

    def finish(self, nal_header):
        self.bits += "1"
        self.bits += "0" * (-len(self.bits) % 8)
        rbsp = bytes(int(self.bits[i:i + 8], 2) for i in range(0, len(self.bits), 8))
        output, zeros = bytearray([0, 0, 0, 1, nal_header]), 0
        for value in rbsp:
            if zeros >= 2 and value <= 3:
                output.append(3)
                zeros = 0
            output.append(value)
            zeros = zeros + 1 if value == 0 else 0
        return bytes(output)


def parameter_sets(message):
    # This fixture omits SPS/PPS NALs. Its message supplies the needed fields.
    # Parameter-set IDs are zero; crop/VUI are absent from this reconstructed stream.
    codec = 0xE0
    require(le32(message, codec) == 2, "fixture must use High profile")
    require(le32(message, codec + 8) == 5, "unexpected SPS flags")
    require(le32(message, codec + 12) == 0x88, "unexpected PPS flags")
    require(message[codec + 16:codec + 24] == bytes([1, 0, 0, 1, 0, 3, 2, 0]),
            "mixed byte fields changed")
    require(message[codec + 24:codec + 36] == bytes([2] + [0] * 11),
            "unsupported PPS or slice-group parameters")
    require(message[codec + 36:codec + 260] == bytes([16]) * 224,
            "non-flat scaling lists require explicit SPS/PPS matrices")
    sps = Bits().u(100, 8).u(0, 8).u(le32(message, codec + 4), 8).ue(0)
    sps.ue(message[codec + 16]).ue(message[codec + 17]).ue(message[codec + 18])
    sps.u(0, 1).u(0, 1).ue(message[codec + 19]).ue(message[codec + 20])
    sps.ue(message[codec + 21]).ue(message[codec + 22]).u(0, 1)
    sps.ue(le32(message, 0x18) // 16 - 1).ue(le32(message, 0x1C) // 16 - 1)
    sps.u(1, 1).u(1, 1).u(0, 1).u(0, 1)
    pps = Bits().ue(0).ue(0).u(0, 1).u(1, 1).ue(0).ue(0).ue(0)
    pps.u(0, 1).u(0, 2).se(message[codec + 24]).se(0).se(0)
    pps.u(1, 1).u(0, 1).u(0, 1)
    return sps.finish(0x67) + pps.finish(0x68)


def unpack_nv12(target):
    require(len(target) == TARGET_BYTES, "expected a 737280-byte field-separated target")
    rows = []
    for height, offset, field_span in [(HEIGHT, 0, 0x3C000),
                                        (HEIGHT // 2, 0x78000, 0x1E000)]:
        for row in range(height):
            start = offset + (row & 1) * field_span + (row >> 1) * PITCH
            rows.append(target[start:start + WIDTH])
    return b"".join(rows)


def validate_layout(arrays):
    for kind, name in enumerate(["create", "decode", "destroy"]):
        data = arrays[name]
        require(len(data) == 3568, "padded message byte count")
        require(le32(data, 0) == 3556 and le32(data, 4) == kind, "LE message header")
        require(le32(data, 8) == 0x40440003 and le32(data, 0x10) == 0,
                "legacy H264 session/codec")
        require(data[3556:] == bytes(12), "trailing message padding")
    fields = {0x18: WIDTH, 0x1C: HEIGHT, 0x24: 15923584, 0x58: 1408,
              0x70: PITCH, 0x74: 0, 0x78: 0, 0x7C: 1, 0x80: 0,
              0x84: 0x3C000, 0x88: 0x78000, 0x8C: 0x96000}
    for offset, value in fields.items():
        require(le32(arrays["decode"], offset) == value, "decode field %x" % offset)
    starts = [m.end() for m in re.finditer(b"\x00\x00\x01", arrays["bitstream"])]
    require(len(starts) == 5 and all(arrays["bitstream"][p] & 31 == 5 for p in starts),
            "expected five IDR slices, without SPS/PPS")
    params = parameter_sets(arrays["decode"])
    require(params.hex() == "000000016764001eac5230360f640000000168de09c8",
            "reconstructed parameter-set bytes")
    return params


C_TEST = r'''
#include "uvd_decode_fixture.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void) {
    unsigned kind, row, column;
    unsigned char msg[RDN_UVD_FIXTURE_MESSAGE_BYTES + 2];
    unsigned char *target = malloc(RDN_UVD_FIXTURE_TARGET_BYTES + 2);
    unsigned char *output = malloc(RDN_UVD_FIXTURE_NV12_BYTES + 2);
    assert(target && output);
    memset(msg, 0xA5, sizeof(msg));
    assert(rdn_uvd_fixture_message_copy(0, 0, msg + 1, 3568) == -1);
    assert(rdn_uvd_fixture_message_copy(3, 1, msg + 1, 3568) == -1);
    assert(rdn_uvd_fixture_message_copy(0, 1, msg + 1, 3567) == -1);
    assert(rdn_uvd_fixture_message_copy(0, 1, NULL, 3568) == -1);
    for (kind = 0; kind < sizeof(msg); ++kind) assert(msg[kind] == 0xA5);
    for (kind = 0; kind < 3; ++kind) {
        assert(!rdn_uvd_fixture_message_copy(kind, 0x12345678, msg + 1, 3568));
        assert(msg[0] == 0xA5 && msg[3569] == 0xA5);
        assert(msg[9] == 0x78 && msg[10] == 0x56 && msg[11] == 0x34 && msg[12] == 0x12);
        assert(fwrite(msg + 1, 1, 3568, stdout) == 3568);
    }
    memset(target, 0xA5, RDN_UVD_FIXTURE_TARGET_BYTES + 2);
    memset(output, 0xA5, RDN_UVD_FIXTURE_NV12_BYTES + 2);
    for (row = 0; row < 480; ++row)
        for (column = 0; column < 864; ++column)
            target[1 + (row & 1) * 0x3C000 + (row >> 1) * 1024 + column] =
                (unsigned char)(row * 7 + column * 3);
    for (row = 0; row < 240; ++row)
        for (column = 0; column < 864; ++column)
            target[1 + 0x78000 + (row & 1) * 0x1E000 + (row >> 1) * 1024 + column] =
                (unsigned char)(row * 11 + column * 5 + 37);
    assert(rdn_uvd_fixture_unpack_nv12(NULL, 737280, output + 1, 622080) == -1);
    assert(rdn_uvd_fixture_unpack_nv12(target + 1, 737279, output + 1, 622080) == -1);
    assert(rdn_uvd_fixture_unpack_nv12(target + 1, 737280, output + 1, 622079) == -1);
    for (row = 0; row < 622082; ++row) assert(output[row] == 0xA5);
    assert(!rdn_uvd_fixture_unpack_nv12(target + 1, 737280, output + 1, 622080));
    assert(target[0] == 0xA5 && target[737281] == 0xA5);
    assert(output[0] == 0xA5 && output[622081] == 0xA5);
    assert(fwrite(output + 1, 1, 622080, stdout) == 622080);
    free(target); free(output);
    return 0;
}
'''


def validate_c_helper(cc, sanitize, arrays):
    root = Path(__file__).resolve().parent
    with tempfile.TemporaryDirectory(prefix="rdn-uvd-fixture-") as directory:
        work = Path(directory)
        (work / "test.c").write_text(C_TEST)
        command = [cc, "-std=c99", "-Wall", "-Wextra", "-Werror", "-O2"]
        if sanitize:
            command += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer"]
        command += ["-I", str(root), str(root / "uvd_decode_fixture.c"),
                    str(work / "test.c"), "-o", str(work / "probe")]
        subprocess.run(command, check=True, capture_output=True, timeout=60)
        result = subprocess.run([str(work / "probe")], check=True, capture_output=True, timeout=30)
        require(not result.stderr, "C helper emitted sanitizer/other diagnostics")
    expected = bytearray()
    for name in ["create", "decode", "destroy"]:
        message = bytearray(arrays[name])
        message[8:12] = bytes([0x78, 0x56, 0x34, 0x12])
        expected += message
    expected += bytes((row * 7 + column * 3) & 255
                      for row in range(480) for column in range(864))
    expected += bytes((row * 11 + column * 5 + 37) & 255
                      for row in range(240) for column in range(864))
    require(result.stdout == expected, "C LE message or field-weave mismatch")
    return {"passed": True, "compiler": cc, "sanitizers": sanitize,
            "compared_bytes": len(expected), "unaligned_buffers": True,
            "invalid_arguments_and_canaries": True}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", help="Optional C compiler for the portable helper test")
    parser.add_argument("--sanitize", action="store_true")
    parser.add_argument("--ffmpeg", help="Optional FFmpeg executable for independent CPU decode")
    parser.add_argument("--decoded", type=Path, help="Raw field-separated native decode target")
    parser.add_argument("--emit-annexb", type=Path)
    parser.add_argument("--emit-reference", type=Path)
    args = parser.parse_args()
    arrays = load_arrays()
    params = validate_layout(arrays)
    stream = params + arrays["bitstream"]
    result = {"upstream": SOURCE_URL, "arrays_sha256": HASHES,
              "fixture_layout_passed": True, "hardware_decode_executed": False,
              "width": WIDTH, "height": HEIGHT, "pitch": PITCH,
              "reference_sha256": REFERENCE_SHA256,
              "scope": "Legacy H264 fixture. Turks/UVD3 execution remains unverified.",
              "coverage": "One IDR picture; no inter-picture DPB, seek, or crop coverage."}
    if args.cc:
        result["c_helper"] = validate_c_helper(args.cc, args.sanitize, arrays)
    if args.emit_annexb:
        args.emit_annexb.write_bytes(stream)
    if args.emit_reference and not args.ffmpeg:
        parser.error("--emit-reference requires --ffmpeg")
    if args.ffmpeg:
        version = subprocess.run([args.ffmpeg, "-version"], check=True,
                                 capture_output=True, timeout=20).stdout.splitlines()[0].decode()
        command = [args.ffmpeg, "-v", "warning", "-hwaccel", "none", "-c:v", "h264",
                   "-threads", "1", "-f", "h264", "-i", "pipe:0", "-frames:v", "1",
                   "-f", "rawvideo", "-pix_fmt", "nv12", "pipe:1"]
        decoded = subprocess.run(command, input=stream, check=True,
                                 capture_output=True, timeout=30)
        require(not decoded.stderr, "FFmpeg emitted warnings: " + decoded.stderr.decode())
        require(len(decoded.stdout) == NV12_BYTES, "CPU reference byte count")
        require(hashlib.sha256(decoded.stdout).hexdigest() == REFERENCE_SHA256,
                "CPU reference hash mismatch")
        require(sum(decoded.stdout) == 0x20345D8, "AMD upstream pixel sum mismatch")
        result["cpu_reference"] = {"passed": True, "decoder": version,
                                   "bytes": len(decoded.stdout), "sum": "0x20345d8",
                                   "sha256": REFERENCE_SHA256, "warnings": []}
        if args.emit_reference:
            args.emit_reference.write_bytes(decoded.stdout)
    if args.decoded:
        actual = unpack_nv12(args.decoded.read_bytes())
        digest = hashlib.sha256(actual).hexdigest()
        result["provided_target"] = {"path": str(args.decoded), "sha256": digest,
                                     "passed": digest == REFERENCE_SHA256}
        require(digest == REFERENCE_SHA256, "native target pixel mismatch")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
