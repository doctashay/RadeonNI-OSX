/* Inspect a captured board ROM without executing its command tables. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../hw/rdn_card.h"
#include "../hw/rdn_mode.h"
#include "../hw/rdn_pm.h"
#include "../hw/rdn_cayman_dpm.h"
#include "../hw/atom/atom.h"
#include "../hw/atom/atombios.h"

static void *alloc_mem(void *cookie, size_t size) { return calloc(1, size); }
static void free_mem(void *cookie, void *p) { free(p); }
static void log_msg(void *cookie, enum rdn_log_level level,
		    const char *fmt, va_list ap)
{
	vprintf(fmt, ap);
	putchar('\n');
}
static unsigned writes;
static uint32_t read_reg(void *cookie,uint32_t reg){return 0;}
static void write_reg(void *cookie,uint32_t reg,uint32_t value){writes++;printf("WRITE %x=%x\n",reg,value);}
static uint64_t timer(void *cookie){static uint64_t ticks;return ticks++;}
static void delay(void *cookie,uint32_t us){}

int __real_atom_execute_table(struct atom_context *,int,uint32_t *,int);
int __wrap_atom_execute_table(struct atom_context *ctx,int index,uint32_t *params,int size)
{
 if(index==GetIndexIntoMasterTable(COMMAND,GetEngineClock)){params[0]=cpu_to_le32(25000);return 0;}
 if(index==GetIndexIntoMasterTable(COMMAND,GetMemoryClock)){params[0]=cpu_to_le32(125000);return 0;}
 return __real_atom_execute_table(ctx,index,params,size);
}
int main(int argc, char **argv)
{
	struct rdn_os os;
	struct rdn_card card;
	struct rdn_pm_state boot, performance;
	unsigned char *bios;
	FILE *f;
	long size;
	unsigned i, sum = 0, pcir;
	if (argc < 2 || !(f = fopen(argv[1], "rb")))
		return 1;
	fseek(f, 0, SEEK_END);
	size = ftell(f);
	rewind(f);
	if (size < 512 || size > 0x200000)
		return 1;
	bios = malloc((size_t)size);
	assert(bios && fread(bios, 1, (size_t)size, f) == (size_t)size);
	fclose(f);
	assert(bios[0] == 0x55 && bios[1] == 0xaa);
	assert((unsigned)bios[2] * 512 == (unsigned)size);
	for (i = 0; i < (unsigned)size; i++)
		sum += bios[i];
	assert(!(sum & 255));
	pcir = bios[0x18] | ((unsigned)bios[0x19] << 8);
	assert(pcir + 8 <= (unsigned)size && !memcmp(bios + pcir, "PCIR", 4));
	printf("PCI %04x:%04x\n", bios[pcir + 4] | bios[pcir + 5] << 8,
	       bios[pcir + 6] | bios[pcir + 7] << 8);
	memset(&os, 0, sizeof(os));
	os.alloc = alloc_mem;
	os.free = free_mem;
	os.log = log_msg;
	os.delay_us=delay;os.time_ms=timer;
 os.mmio_read32 = read_reg;
	os.mmio_write32 = write_reg;
	assert(!rdn_card_init(&card, &os, bios));
	assert(!rdn_pm_boot_state(&card, &boot));
	assert(!rdn_pm_performance_state(&card, &performance));
	printf("Boot %u/%u MHz; performance %u/%u MHz\n",
	       boot.sclk / 100, boot.mclk / 100,
	       performance.sclk / 100, performance.mclk / 100);
 card.family=RDN_FAMILY_CAYMAN;
 struct rdn_cayman_calibration cal;int err=rdn_cayman_calibration(&card,&cal);
 printf("calibration result %d writes %u\n",err,writes);if(err)return 2;
 printf("TDP=%u near=%u reference=%u voltages=%u leakage=%u\n",cal.tdp,cal.near_tdp,cal.reference_clock,cal.voltage_count,cal.leakage_count);
 for(unsigned j=0;j<cal.voltage_count;j++)printf("VDDC %u GPIO %x mask %x std %u leakage %u\n",cal.voltage[j],cal.gpio[j],cal.gpio_mask,cal.std_voltage[j],cal.leakage[j]);
 for(unsigned j=0;j<cal.vddci_count;j++)printf("VDDCI %u GPIO %x mask %x\n",cal.vddci[j],cal.vddci_gpio[j],cal.vddci_mask);
 assert(!writes);
 for(unsigned i=0;i<2;i++){COMPUTE_MEMORY_ENGINE_PLL_PARAMETERS_V3 args;memset(&args,0,sizeof(args));args.ulClockParams=cpu_to_le32((2u<<24)|(i?72500:25000));
 int e=__real_atom_execute_table(card.atom.ctx,GetIndexIntoMasterTable(COMMAND,ComputeMemoryEnginePLL),(uint32_t *)&args,sizeof(args));
 printf("PLL %u err=%d whole=%u frac=%u ref=%u post=%u flags=%u\n",i?72500:25000,e,le16_to_cpu(args.ulFbDiv.usFbDiv),le16_to_cpu(args.ulFbDiv.usFbDivFrac),args.ucRefDiv,args.ucPostDiv,args.ucCntlFlag);}

 if(argc>2){struct rdn_cayman_dpm *pm=calloc(1,sizeof(*pm));err=rdn_cayman_dpm_prepare(&card,pm);
 printf("prepare result %d writes %u size %u idle %u/%u performance %u/%u\n",err,writes,(unsigned)sizeof(pm->table),pm->idle.sclk,pm->idle.vddc,pm->performance.sclk,pm->performance.vddc);
 if(err||writes)return 3;free(pm);}

	rdn_card_fini(&card);
	free(bios);
	puts("PASS ROM metadata parsed without register access");
	return 0;
}
