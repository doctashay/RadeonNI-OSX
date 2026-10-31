"""The C++ zero-length SMC tail must preserve the C firmware layout."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = r'''
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include "hw/linux/nislands_smc.h"
int main(void) {
 printf("%zu %zu %zu %zu %zu\n", sizeof(NISLANDS_SMC_SWSTATE),
        offsetof(NISLANDS_SMC_SWSTATE, levels), sizeof(NISLANDS_SMC_STATETABLE),
        offsetof(NISLANDS_SMC_STATETABLE, driverState),
        offsetof(NISLANDS_SMC_STATETABLE, dpmLevels));
 return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="rdn-smc-layout-") as directory:
    temp = Path(directory)
    results = []
    for compiler, extension, standard in [("cc", "c", "gnu99"), ("c++", "cc", "gnu++98")]:
        path = temp / ("layout." + extension)
        path.write_text(source)
        binary = temp / extension
        subprocess.run([compiler, "-std=" + standard, "-I" + str(root), str(path),
                        "-o", str(binary)], check=True)
        results.append(subprocess.check_output([str(binary)], text=True).strip())
    assert results[0] == results[1], results
print("PASS C/C++ SMC firmware layout unchanged:", results[0])
