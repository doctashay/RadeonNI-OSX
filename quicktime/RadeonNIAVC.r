/*
 * The registration of the component: what the Component Manager reads from the bundle's
 * Resources/RadeonNIAVC.rsrc (compiled with Rez, into the data fork; see the Makefile).
 *
 * Copyright (c) 2026 kouta-kun and Claude
 * SPDX-License-Identifier: MIT
 */

#define thng_RezTemplateVersion 2
#include <CoreServices/CoreServices.r>
#include <QuickTime/QuickTime.r>

#define kRDNAVCResID		128
#define kRDNAVCVersion		0x00010000
/* thread safe; 32-bit pixels and temporal compression, as codecInfoDoes32 | codecInfoDoesTemporal (the same flags as Apple's decoder) */
#define kRDNAVCFlags		0x10000420

resource 'thng' (kRDNAVCResID, "RadeonNI H.264 Decoder", purgeable) {
	'imdc',				/* a decompressor */
	'avc1',				/* of H.264 */
	'RdNI',				/* RadeonNI */
	0,
	kAnyComponentFlagsMask,
	0, 0,				/* no code resource: the native entry point is in the platform list */
	'STR ', kRDNAVCResID,
	'STR ', kRDNAVCResID + 1,
	0, 0,				/* no icon */
	kRDNAVCVersion,
	componentDoAutoVersion | componentHasMultiplePlatforms,
	0,
	{
		kRDNAVCFlags, 'dlle', kRDNAVCResID, platformPowerPCNativeEntryPoint;
	},
	'thnr', kRDNAVCResID
};

resource 'STR ' (kRDNAVCResID, purgeable) {
	"H.264 (RadeonNI, UVD)"
};

resource 'STR ' (kRDNAVCResID + 1, purgeable) {
	"Decodes H.264 video on the card's video decoder (RadeonNI)."
};

/* the name of the entry point in the executable */
resource 'dlle' (kRDNAVCResID) {
	"RDNAVC_ComponentDispatch"
};

/* the pixel formats it draws: only the first one in the list of Preflight */
resource 'thnr' (kRDNAVCResID) {
	{
		'cpix', 1, 0, 'cpix', kRDNAVCResID, cmpResourceNoFlags
	}
};

data 'cpix' (kRDNAVCResID) {
	"2vuy"
};
