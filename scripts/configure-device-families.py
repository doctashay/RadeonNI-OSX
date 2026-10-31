#!/usr/bin/env python3
"""Build family-specific IOKit personalities without installing them."""
import argparse
import copy
import plistlib
import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CAPS_TEXT = (ROOT / "hw/rdn_device_ids.h").read_text()
FAMILY_CAPS = {
    name: dict(engine=engine, pfp_words=int(pfp), me_words=int(me))
    for name, engine, pfp, me in re.findall(
        r"X\((\w+),\s*\d+,\s*\d+,\s*\d+,\s*(\w+),\s*(\d+),\s*(\d+),",
        CAPS_TEXT)
}
CP_BYTES = {name: (caps["pfp_words"] * 4, caps["me_words"] * 4)
            for name, caps in FAMILY_CAPS.items() if caps["engine"] != "NONE"}


def devices():
    text = (ROOT / "hw/rdn_device_ids.h").read_text()
    return [(int(device, 16), family, int(mobile)) for device, family, mobile in
            re.findall(r"X\((0x[0-9a-f]+), RDN_FAMILY_([A-Z]+), ([01])\)", text)]


def configure(template, families, firmware_dir=None):
    result = copy.deepcopy(template)
    original = result["IOKitPersonalities"]["RadeonNI"]
    profiles = {}
    for family in families:
        if family not in FAMILY_CAPS:
            raise ValueError("missing family capabilities: " + family)
        # Inherit interface settings, not a board ROM, power tuning or MC layout.
        common = {"CFBundleIdentifier", "IOClass", "IOMatchCategory", "IOProbeScore",
                  "IOProviderClass", "AccelCaps", "AGPShim", "Surfaces", "SurfaceGate",
                  "GAPlugin", "HWCursor", "BackingSurfaces"}
        profile = {key: copy.deepcopy(value) for key, value in original.items()
                   if key in common}
        ids = [device for device, chip, mobile in devices()
               if chip == family and not mobile]
        if not ids:
            raise ValueError("unsupported family: " + family)
        profile["IOPCIMatch"] = " ".join("0x%04x1002" % device for device in ids)
        profile["FirmwareFamily"] = family
        if family not in CP_BYTES:
            profile.update(Accelerator=False, HWCursor=False, MultiHead=False,
                           ExperimentalFramebuffer=True)
        if firmware_dir is not None and family in CP_BYTES:
            for kind, expected in zip(("PFP", "ME"), CP_BYTES[family]):
                path = firmware_dir / (family + "_" + kind.lower() + ".bin")
                data = path.read_bytes()
                if len(data) != expected:
                    raise ValueError("incorrect microcode size: " + str(path))
                profile["FW_" + kind] = data
            for kind in ("MC", "RLC", "SMC"):
                path = firmware_dir / (family + "_" + kind.lower() + ".bin")
                if path.exists():
                    profile["FW_" + kind] = path.read_bytes()
            profile["Accelerator"] = True
            profile.setdefault("AccelCaps", 3)
            profile.setdefault("Surfaces", True)
            profile.setdefault("SurfaceGate", True)
            profile.setdefault("GAPlugin", True)
        profiles["RadeonNI-" + family] = profile
    others = {key: value for key, value in result["IOKitPersonalities"].items()
              if key != "RadeonNI"}
    others.update(profiles)
    result["IOKitPersonalities"] = others
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--input", type=Path, default=ROOT / "kext/RadeonNI/Info.plist")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--families", nargs="+", choices=sorted({family for _, family, _ in devices()}), required=True)
    parser.add_argument("--firmware-dir", type=Path,
                        help="enable acceleration with this family's microcode")
    args = parser.parse_args()
    with args.input.open("rb") as source:
        result = configure(plistlib.load(source), args.families, args.firmware_dir)
    with args.output.open("wb") as output:
        plistlib.dump(result, output, sort_keys=False)


if __name__ == "__main__":
    main()
