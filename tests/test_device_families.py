"""Family classification and offline multi-family firmware profiles."""
from pathlib import Path
import importlib.util
import plistlib
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("profiles", root / "scripts/configure-device-families.py")
profiles = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiles)

assert set(profiles.CP_BYTES) == {"TURKS", "CAYMAN"}
for family in ["REDWOOD", "CEDAR", "JUNIPER", "CYPRESS", "ARUBA"]:
    assert profiles.FAMILY_CAPS[family]["engine"] == "NONE"
    assert not any(chip == family for _, chip, _ in profiles.devices())
template = plistlib.loads((root / "kext/RadeonNI/Info.plist").read_bytes())
template["IOKitPersonalities"]["RadeonNI"].update(FW_PFP=b"wrong", VBIOS=b"other board", RelocateVRAM=True)
with tempfile.TemporaryDirectory(prefix="rdn-families-") as temp:
    temp = Path(temp)
    subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                    str(root / "tests/device_families.c"), "-o", str(temp / "ids")], check=True)
    subprocess.run([str(temp / "ids")], check=True)
    post = (root / "hw/rdn_post.c").read_text()
    init = post.split("int rdn_card_init(", 1)[1].split("\n}\n", 1)[0]
    binding = r'''
#include "rdn_card.h"
#include <assert.h>
static uint32_t pci;
static int parsed;
static uint32_t config(void *cookie, uint32_t off) { (void)cookie; assert(off == 0); return pci; }
static void log_message(void *cookie, enum rdn_log_level level, const char *fmt, va_list ap)
{ (void)cookie; (void)level; (void)fmt; (void)ap; }
int rdn_atom_init(struct rdn_atom *a, struct rdn_os *o, void *bios)
{ (void)a; (void)o; (void)bios; parsed++; return 0; }
'''
    binding += "int rdn_card_init(" + init + "\n}\n"
    binding += r'''
int main(void) {
 struct rdn_card card; struct rdn_os os = {0};
 os.cfg_read32 = config; os.log = log_message;
 pci = 0x67191002; assert(!rdn_card_init(&card, &os, 0));
 assert(card.family == RDN_FAMILY_CAYMAN && parsed == 1);
 pci = 0x68581002; assert(!rdn_card_init(&card, &os, 0));
 assert(card.family == RDN_FAMILY_TURKS && parsed == 2);
 pci = 0x67381002; assert(!rdn_card_init(&card, &os, 0));
 assert(card.family == RDN_FAMILY_BARTS && parsed == 3);
 pci = 0x67791002; assert(!rdn_card_init(&card, &os, 0));
 assert(card.family == RDN_FAMILY_CAICOS && parsed == 4);
 pci = 0x68981002; assert(rdn_card_init(&card, &os, 0) < 0 && parsed == 4);
 pci = 0x671910de; assert(rdn_card_init(&card, &os, 0) < 0 && parsed == 4);
 return 0;
}
'''
    (temp / "binding.c").write_text(binding)
    subprocess.run(["cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror",
                    "-I" + str(root / "hw"), str(temp / "binding.c"),
                    "-o", str(temp / "binding")], check=True)
    subprocess.run([str(temp / "binding")], check=True)
    for family, sizes in profiles.CP_BYTES.items():
        for name, size in zip(("pfp", "me"), sizes):
            (temp / (family + "_" + name + ".bin")).write_bytes(bytes([size % 251]) * size)
    result = profiles.configure(template, ["TURKS", "CAYMAN"], temp)
    all_ids = set()
    for family, expected in [("TURKS", 16), ("CAYMAN", 15)]:
        p = result["IOKitPersonalities"]["RadeonNI-" + family]
        ids = p["IOPCIMatch"].split()
        assert len(ids) == expected and not all_ids.intersection(ids)
        all_ids.update(ids)
        assert "VBIOS" not in p and "RelocateVRAM" not in p and p["FirmwareFamily"] == family
        assert len(p["FW_PFP"]) == profiles.CP_BYTES[family][0]
        assert len(p["FW_ME"]) == profiles.CP_BYTES[family][1]
        assert p["Accelerator"]
    assert "0x67181002" in all_ids and "0x68401002" not in all_ids
    assert "0x67381002" not in all_ids and "0x67791002" not in all_ids
    assert plistlib.loads(plistlib.dumps(result)) == result
    (temp / "CAYMAN_me.bin").write_bytes(b"wrong family")
    try:
        profiles.configure(template, ["CAYMAN"], temp)
    except ValueError:
        pass
    else:
        raise AssertionError("wrong-size microcode accepted")
    p = profiles.configure(template, ["CAYMAN"])["IOKitPersonalities"]["RadeonNI-CAYMAN"]
    assert "FW_PFP" not in p and "Accelerator" not in p
print("PASS distinct family profiles, 31 desktop matches, no inherited ROM/firmware, size rejection")

fb = profiles.configure(template, ["BARTS", "CAICOS"], temp)
for family, count in [("BARTS", 9), ("CAICOS", 11)]:
    p = fb["IOKitPersonalities"]["RadeonNI-" + family]
    assert len(p["IOPCIMatch"].split()) == count
    assert p["Accelerator"] is False and p["ExperimentalFramebuffer"]
    assert p["HWCursor"] is False and p["MultiHead"] is False
    assert not any(key.startswith("FW_") for key in p)
    stock = plistlib.loads((root / "kext/RadeonNI/Info.plist").read_bytes())
    assert stock["IOKitPersonalities"]["RadeonNI-" + family] == p
print("PASS 20 new desktop IDs attached to framebuffer-only profiles with no GPU firmware")
