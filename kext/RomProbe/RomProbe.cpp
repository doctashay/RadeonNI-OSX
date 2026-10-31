/*
 * Explicitly loaded ROM probe for one selected Radeon PCI device.
 * Reads through machine-check-safe word probes and restores PCI decoding.
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IODeviceMemory.h>
#include <IOKit/pci/IOPCIDevice.h>

extern "C" {
int rdn_probe_bringup(IOService *owner, IOPCIDevice *dev, void *bios);
/* osfmk/ppc/machine_routines.h; exported, but not in Kernel.framework. */
boolean_t ml_probe_read(vm_offset_t paddr, unsigned int *val);
}

class RadeonNIRomProbe : public IOService
{
	OSDeclareDefaultStructors(RadeonNIRomProbe)

public:
	virtual bool start(IOService *provider);

private:
	IOPCIDevice *findCard();
	void probe(IOPCIDevice *dev);
};

#define super IOService
OSDefineMetaClassAndStructors(RadeonNIRomProbe, IOService)

static bool isTurks(UInt16 d)
{
	return d == 0x675d || d == 0x6758 || d == 0x6759 || d == 0x675b ||
	       d == 0x675f;
}

IOPCIDevice *RadeonNIRomProbe::findCard()
{
	OSDictionary *match = serviceMatching("IOPCIDevice");
	OSIterator *iter = match ? getMatchingServices(match) : 0;
	IOPCIDevice *found = 0;
	OSObject *obj;
	OSNumber *target = OSDynamicCast(OSNumber, getProperty("TargetDeviceID"));

	if (match)
		match->release();
	if (!iter)
		return 0;
	while (!found && (obj = iter->getNextObject())) {
		IOPCIDevice *dev = OSDynamicCast(IOPCIDevice, obj);

		if (dev && dev->configRead16(kIOPCIConfigVendorID) == 0x1002 &&
		    (target ? dev->configRead16(kIOPCIConfigDeviceID) == target->unsigned16BitValue() :
		     isTurks(dev->configRead16(kIOPCIConfigDeviceID)))) {
			dev->retain();
			found = dev;
		}
	}
	iter->release();
	return found;
}

void RadeonNIRomProbe::probe(IOPCIDevice *dev)
{
	IODeviceMemory *mem;
	UInt8 *copy = 0;
	UInt32 bar, phys, length = 0, image, i;
	UInt16 command;
	unsigned int word = 0;
	UInt8 sum = 0;

	if (!dev->lockForArbitration(false))
		return;
	if (!dev->open(this)) {
		dev->unlockForArbitration();
		return;
	}
	command = dev->configRead16(kIOPCIConfigCommand);
	bar = dev->configRead32(kIOPCIConfigExpansionROMBase);
	setProperty("DeviceID", dev->configRead16(kIOPCIConfigDeviceID), 16);
	setProperty("Subsystem", dev->configRead32(0x2c), 32);
	setProperty("CommandBefore", command, 16);
	setProperty("BAR", bar, 32);
	setProperty("ApertureBAR", dev->configRead32(0x10), 32);
	setProperty("RegisterBAR", dev->configRead32(0x18), 32);
	mem = dev->getDeviceMemoryWithRegister(kIOPCIConfigExpansionROMBase);
	if (!(bar & 0xfffff800) || !mem)
		goto out;
	phys = mem->getPhysicalAddress();
	length = mem->getLength();
	if (length < 512 || length > 0x200000 || phys + length < phys)
		goto out;
	copy = (UInt8 *)IOMalloc(length);
	if (!copy)
		goto out;
	dev->configWrite16(kIOPCIConfigCommand, command | 2);
	dev->configWrite32(kIOPCIConfigExpansionROMBase, bar | 1);
	IODelay(1000);
	// Word probes avoid machine checks and unsupported byte ROM reads.
	for (i = 0; i + 4 <= length; i += 4) {
		if (!ml_probe_read(phys + i, &word))
			break;
		copy[i] = (UInt8)(word >> 24);
		copy[i + 1] = (UInt8)(word >> 16);
		copy[i + 2] = (UInt8)(word >> 8);
		copy[i + 3] = (UInt8)word;
	}
	setProperty("BytesRead", i, 32);
	if (i != length || copy[0] != 0x55 || copy[1] != 0xaa)
		goto out;
	image = (UInt32)copy[2] * 512;
	if (image < 512 || image > length)
		goto out;
	for (i = 0; i < image; i++)
		sum += copy[i];
	setProperty("Checksum", sum, 8);
	if (sum)
		goto out;
	setProperty("ROM", copy, image);
	if (getProperty("RunBringup") == kOSBooleanTrue)
		setProperty("BringupResult", (UInt64)(SInt64)rdn_probe_bringup(this, dev, copy), 32);
	IOLog("RomProbe: device 1002:%04x, valid ROM %lu bytes\n",
	      dev->configRead16(kIOPCIConfigDeviceID), (unsigned long)image);
out:
	dev->configWrite32(kIOPCIConfigExpansionROMBase, bar);
	dev->configWrite16(kIOPCIConfigCommand, command);
	setProperty("ConfigurationRestored",
		dev->configRead32(kIOPCIConfigExpansionROMBase) == bar &&
		dev->configRead16(kIOPCIConfigCommand) == command);
	if (copy)
		IOFree(copy, length);
	dev->close(this);
	dev->unlockForArbitration();
}

bool RadeonNIRomProbe::start(IOService *provider)
{
	IOPCIDevice *dev;

	if (!super::start(provider))
		return false;
	dev = findCard();
	if (!dev) {
		IOLog("RomProbe: no matching Radeon device\n");
	} else {
		probe(dev);
		dev->release();
	}
	registerService();
	return true;
}
