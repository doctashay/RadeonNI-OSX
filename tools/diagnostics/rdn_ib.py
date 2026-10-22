#!/usr/bin/env python3
"""Decode saved Evergreen PM4 buffers without accessing the device."""

import argparse
import collections
import json
from pathlib import Path
import struct


REG_BASES = {0x68: 0x8000, 0x69: 0x28000, 0x6B: 0x3A500, 0x6C: 0x3A200,
             0x6D: 0x30000, 0x6E: 0x3C000, 0x6F: 0x3CFF0}
NAMES = {0x10: "NOP", 0x28: "CONTEXT_CONTROL", 0x2A: "INDEX_TYPE",
         0x2B: "DRAW_INDEX", 0x2D: "DRAW_INDEX_AUTO", 0x2F: "NUM_INSTANCES",
         0x32: "INDIRECT_BUFFER", 0x3D: "MEM_WRITE", 0x43: "SURFACE_SYNC",
         0x46: "EVENT_WRITE", 0x47: "EVENT_WRITE_EOP", 0x68: "SET_CONFIG_REG",
         0x69: "SET_CONTEXT_REG", 0x6B: "SET_BOOL_CONST", 0x6C: "SET_LOOP_CONST", 0x6D: "SET_RESOURCE",
         0x6E: "SET_SAMPLER", 0x6F: "SET_CTL_CONST"}


def packets(data, endian="big"):
    if len(data) % 4:
        raise ValueError("command buffer length is not a multiple of four")
    words = struct.unpack((">" if endian == "big" else "<") +
                          str(len(data) // 4) + "I", data)
    pos = 0
    while pos < len(words):
        header = words[pos]
        kind = header >> 30
        if kind == 1:
            raise ValueError(f"unsupported type-1 packet at word {pos}")
        size = 1 if kind == 2 else ((header >> 16) & 0x3FFF) + 2
        if pos + size > len(words):
            raise ValueError(f"truncated packet at word {pos}: needs {size} words")
        yield pos, kind, header, words[pos + 1:pos + size]
        pos += size


def decode(data, endian="big"):
    regs, draws, traces, warnings = {}, [], [], []
    counts = collections.Counter()
    index_type, instances = 0, 1
    for pos, kind, header, payload in packets(data, endian):
        if kind == 2:
            counts["TYPE2_NOP"] += 1
            continue
        if kind == 0:
            base = (header & 0xFFFF) * 4
            for i, value in enumerate(payload):
                regs[base + 4 * i] = value
            continue
        op = (header >> 8) & 255
        counts[NAMES.get(op, f"OP_{op:02x}")] += 1
        if op in REG_BASES:
            if len(payload) < 2:
                warnings.append(f"register packet without values at word {pos}")
            base = REG_BASES[op] + payload[0] * 4
            for i, value in enumerate(payload[1:]):
                regs[base + 4 * i] = value
        elif op == 0x2A:
            index_type = payload[0]
        elif op == 0x2F:
            instances = payload[0]
        elif op == 0x3D and len(payload) >= 3:
            traces.append({"word": pos, "address": payload[0] |
                           ((payload[1] & 255) << 32), "value": payload[2]})
        elif op in (0x2B, 0x2D):
            if len(payload) != (4 if op == 0x2B else 2):
                raise ValueError(f"invalid draw length at word {pos}")
            resources = []
            for slot in range(16):
                base = 0x30000 + (992 + slot) * 32
                values = [regs.get(base + 4 * i) for i in range(8)]
                if None not in values and values[7] >> 30 == 3:
                    resources.append({"slot": slot, "address": values[0] |
                                      ((values[2] & 255) << 32),
                                      "size": values[1] + 1,
                                      "stride": (values[2] >> 8) & 0x7FF})
            draw = {"word": pos, "kind": NAMES[op], "instances": instances,
                    "count": payload[2] if op == 0x2B else payload[0],
                    "base_vertex": regs.get(0x28408),
                    "vertex_index_limits": [regs.get(0x28404), regs.get(0x28400)],
                    "restart_enabled": regs.get(0x28A94),
                    "vs": regs.get(0x2885C, 0) << 8,
                    "ps": regs.get(0x28840, 0) << 8,
                    "fetch": regs.get(0x288A4, 0) << 8,
                    "vertex_resources": resources}
            if op == 0x2B:
                draw.update(index_address=payload[0] | ((payload[1] & 255) << 32),
                            index_type=index_type)
            draws.append(draw)
    return {"byte_order": endian, "words": len(data) // 4,
            "packets": dict(counts), "draws": draws, "memory_writes": traces,
            "warnings": warnings}


def check_indices(report, memory, gpu_base):
    """Report descriptor capacity; shader use and base-vertex are not inferred."""
    for draw in report["draws"]:
        if "index_address" not in draw:
            continue
        index_type = draw["index_type"]
        if index_type & 3 not in (0, 1):
            draw["indices"] = {"status": "unsupported index type"}
            continue
        width = 2 if index_type & 3 == 0 else 4
        offset = draw["index_address"] - gpu_base
        size = draw["count"] * width
        if offset < 0 or offset + size > len(memory):
            draw["indices"] = {"status": "not captured"}
            continue
        raw = memory[offset:offset + size]
        swap = (index_type >> 2) & 3
        if swap == 1:
            raw = b"".join(raw[i:i + 2][::-1] for i in range(0, len(raw), 2))
        elif swap == 2:
            raw = b"".join(raw[i:i + 4][::-1] for i in range(0, len(raw), 4))
        elif swap:
            draw["indices"] = {"status": "unsupported swap"}
            continue
        values = struct.unpack("<" + str(draw["count"]) + ("H" if width == 2 else "I"), raw)
        draw["indices"] = {"status": "decoded", "min": min(values, default=0),
                           "max": max(values, default=0)}


def fetch_inputs(memory, gpu_base, address):
    """Read straight-line VTX clauses; reject fetch programs that compute indices."""
    result = []
    for cf in range(128):
        offset = address - gpu_base + cf * 8
        if offset < 0 or offset + 8 > len(memory):
            raise ValueError("fetch program not captured")
        a, b = struct.unpack_from("<2I", memory, offset)
        op = (b >> 22) & 255
        if op == 0x14:
            return result
        if op != 2:
            raise ValueError("fetch program has unsupported control flow")
        start = address - gpu_base + (a & 0xFFFFFF) * 8
        count = ((b >> 10) & 63) + 1
        if start < 0 or start + count * 16 > len(memory):
            raise ValueError("fetch clause not captured")
        for i in range(count):
            w0, w1, w2, _ = struct.unpack_from("<4I", memory, start + i * 16)
            # Only VTX_FETCH from the unmodified vertex index in R0.x.
            if w0 & 0x03FF007F or w2 & ((1 << 18) | (1 << 20)) or w1 & (1 << 21):
                raise ValueError("fetch input uses unsupported indexing or format source")
            result.append({"slot": (w0 >> 8) & 255, "offset": w2 & 65535,
                           "format": (w1 >> 22) & 63})
    raise ValueError("fetch program has no bounded return")


def check_fetch_bounds(report, memory, gpu_base):
    """Check logical attribute bytes, not speculative fetches or memory lifetime."""
    widths = {1: 1, 5: 2, 6: 2, 7: 2, 13: 4, 14: 4, 15: 4, 16: 4,
              25: 4, 26: 4, 27: 4, 29: 8, 30: 8, 31: 8, 32: 8,
              34: 16, 35: 16, 44: 3, 45: 6, 46: 6, 47: 12, 48: 12}
    for draw in report["draws"]:
        checks = []
        draw["fetch_bounds"] = checks
        indices = draw.get("indices", {})
        if not draw["count"]:
            continue
        if indices.get("status") != "decoded" or draw.get("restart_enabled") != 0:
            checks.append({"status": "unchecked indices or primitive restart"})
            continue
        if draw.get("vertex_index_limits") != [0, 0xFFFFFFFF]:
            checks.append({"status": "unsupported or uncaptured index limits"})
            continue
        base = draw.get("base_vertex")
        if base is None:
            checks.append({"status": "base vertex not captured"})
            continue
        if base & 0x80000000:
            base -= 0x100000000
        try:
            inputs = fetch_inputs(memory, gpu_base, draw["fetch"])
        except ValueError as error:
            checks.append({"status": str(error)})
            continue
        resources = {r["slot"]: r for r in draw["vertex_resources"]}
        for attr in inputs:
            row = dict(attr)
            checks.append(row)
            resource = resources.get(attr["slot"])
            width = widths.get(attr["format"])
            if resource is None or width is None:
                row["status"] = "resource or format not decoded"
                continue
            first = (indices["min"] + base) * resource["stride"] + attr["offset"]
            end = (indices["max"] + base) * resource["stride"] + attr["offset"] + width
            row.update(first_byte=first, end_byte=end, descriptor_bytes=resource["size"],
                       status="within descriptor" if 0 <= first <= end <= resource["size"]
                       else "outside descriptor")


def read_capture(data):
    if data[:4] not in (b"RDN1", b"1NDR"):
        raise ValueError("not an RDN submission capture")
    endian = "big" if data[:4] == b"RDN1" else "little"
    fmt = ">" if endian == "big" else "<"
    if len(data) < 64:
        raise ValueError("truncated capture header")
    h = struct.unpack(fmt + "16I", data[:64])
    if h[1:4] != (1, 16, 12):
        raise ValueError("unsupported capture version or record size")
    ib_start = 64 + h[7] * 48
    if len(data) != ib_start + h[8] * 4:
        raise ValueError("capture length does not match its header")
    buffers = []
    for pos in range(64, ib_start, 48):
        r = struct.unpack(fmt + "12I", data[pos:pos + 48])
        buffers.append(dict(id=r[0], region=r[1], foreign=bool(r[2]), usage=r[3],
                            address=(r[4] << 32) | r[5], size=(r[6] << 32) | r[7],
                            last_use=r[8] if r[10] else None,
                            last_write=r[9] if r[11] else None))
    report = decode(data[ib_start:], endian)
    report["submission"] = dict(pid=h[4], sequence=h[5], context=h[6],
                                ib_offset=(h[9] << 32) | h[10],
                                marker_offset=(h[14] << 32) | h[15],
                                result=h[11] if h[11] < 0x80000000 else h[11] - 0x100000000,
                                fence=h[12], returned=bool(h[13]), buffers=buffers)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("ib", type=Path)
    parser.add_argument("--endian", choices=("big", "little"), default="big")
    parser.add_argument("--memory", type=Path)
    parser.add_argument("--gpu-base", type=lambda v: int(v, 0), default=0xF00000000)
    parser.add_argument("--capture", action="store_true", help="read a winsys submission capture")
    args = parser.parse_args()
    try:
        data = args.ib.read_bytes()
        report = read_capture(data) if args.capture else decode(data, args.endian)
        if args.memory:
            memory = args.memory.read_bytes()
            check_indices(report, memory, args.gpu_base)
            check_fetch_bounds(report, memory, args.gpu_base)
    except (ValueError, OSError) as error:
        parser.error(str(error))
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
