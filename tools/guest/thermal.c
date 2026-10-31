/* Read-only Cayman temperature, clocks and fan-register sampling. */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <unistd.h>
#include <time.h>
#include <mach/mach.h>
#include <IOKit/IOKitLib.h>
#include "rdn_user.h"

static int read_reg(io_connect_t conn, uint32_t offset, uint32_t *value)
{
 return IOConnectMethodScalarIScalarO(conn, RDN_UC_REG_READ, 1, 1,
                                    (int)offset, value);
}

int main(int argc, char **argv)
{
 const uint32_t fan_regs[] = {0x76c,0x770,0x774,0x778,0x77c,0x780,0x784,0x788};
 io_service_t service;
 io_connect_t conn = 0;
 struct rdn_user_info info;
 IOByteCount size = sizeof(info);
 unsigned count = argc == 2 ? strtoul(argv[1], NULL, 10) : 1, sample, i;
 int status = 1;
 if (argc > 2 || !count || count > 3600) {
  fprintf(stderr, "usage: thermal [samples: 1..3600]\n");
  return 2;
 }
 service = IOServiceGetMatchingService(kIOMasterPortDefault,
                                      IOServiceMatching(RDN_UC_SERVICE_CLASS));
 if (!service) return 1;
 if (IOServiceOpen(service, mach_task_self(), RDN_UC_TYPE, &conn)) goto out;
 if (IOConnectMethodStructureIStructureO(conn, RDN_UC_GET_INFO, 0,
                                        &size, NULL, &info)) goto out;
 if (info.pci_device_id != 0x6704) {
  fprintf(stderr, "Cayman V7900 required; found %04x\n", info.pci_device_id);
  goto out;
 }
 puts("time,sclk_MHz,mclk_MHz,temp_valid,temp_C,thermal_raw,power_raw,dpm_enabled,thermal_protection_disabled,fan_76c,fan_770,fan_774,fan_778,fan_77c,fan_780,fan_784,fan_788");
 for (sample = 0; sample < count; sample++) {
  uint32_t sclk = 0, mclk = 0, biased = 0, raw, power, fan[8], temp;
  int valid, actual;
  if (IOConnectMethodScalarIScalarO(conn, RDN_UC_POWER, 2, 3,
      RDN_UC_POWER_QUERY, 0, &sclk, &mclk, &biased)) goto out;
  if (read_reg(conn, 0x740, &raw) || read_reg(conn, 0x63c, &power)) goto out;
  for (i = 0; i < 8; i++) if (read_reg(conn, fan_regs[i], &fan[i])) goto out;
  temp = (raw >> 16) & 0x7ff;
  valid = !(temp & 0x600);
  actual = (temp & 0x100) ? (int)(temp & 0x1ff) - 0x200 : (int)(temp & 0xff);
  printf("%lu,%.2f,%.2f,%d,%.1f,%08x,%08x,%u,%u", (unsigned long)time(NULL),
         sclk / 100.0, mclk / 100.0, valid, actual / 2.0,
         raw, power, power & 1, (power >> 2) & 1);
  for (i = 0; i < 8; i++) printf(",%08x", fan[i]);
  putchar('\n');fflush(stdout);
  if (sample + 1 < count) sleep(1);
 }
 status = 0;
out:
 if (conn) IOServiceClose(conn);
 IOObjectRelease(service);
 return status;
}
