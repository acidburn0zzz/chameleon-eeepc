/*
 * Copyright (c) 1999-2003 Apple Computer, Inc. All rights reserved.
 *
 * @APPLE_LICENSE_HEADER_START@
 * 
 * Portions Copyright (c) 1999-2003 Apple Computer, Inc.  All Rights
 * Reserved.  This file contains Original Code and/or Modifications of
 * Original Code as defined in and that are subject to the Apple Public
 * Source License Version 2.0 (the "License").	You may not use this file
 * except in compliance with the License.  Please obtain a copy of the
 * License at http://www.apple.com/publicsource and read it before using
 * this file.
 * 
 * The Original Code and all software distributed under the License are
 * distributed on an "AS IS" basis, WITHOUT WARRANTY OF ANY KIND, EITHER
 * EXPRESS OR IMPLIED, AND APPLE HEREBY DISCLAIMS ALL SUCH WARRANTIES,
 * INCLUDING WITHOUT LIMITATION, ANY WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE OR NON- INFRINGEMENT.  Please see the
 * License for the specific language governing rights and limitations
 * under the License.
 * 
 * @APPLE_LICENSE_HEADER_END@
 */

/* 
 * Mach Operating System
 * Copyright (c) 1990 Carnegie-Mellon University
 * Copyright (c) 1989 Carnegie-Mellon University
 * All rights reserved.	 The CMU software License Agreement specifies
 * the terms and conditions for use and redistribution.
 */

/*
 *			INTEL CORPORATION PROPRIETARY INFORMATION
 *
 *	This software is supplied under the terms of a license	agreement or 
 *	nondisclosure agreement with Intel Corporation and may not be copied 
 *	nor disclosed except in accordance with the terms of that agreement.
 *
 *	Copyright 1988, 1989 by Intel Corporation
 */

/*
 * Copyright 1993 NeXT Computer, Inc.
 * All rights reserved.
 */

/*
 * Completely reworked by Sam Streeper (sam_s@NeXT.com)
 * Reworked again by Curtis Galloway (galloway@NeXT.com)
 */

#include "boot.h"
#include "bootstruct.h"
#include "fake_efi.h"
#include "sl.h"
#include "libsa.h"
#include "ramdisk.h"
#include "gui.h"
#include "platform.h"
#include "modules.h"

/*
 * How long to wait (in seconds) to load the
 * kernel after displaying the "boot:" prompt.
 */
#define kBootErrorTimeout 5

bool		gOverrideKernel, gEnableCDROMRescan, gScanSingleDrive, useGUI;
static bool	gUnloadPXEOnExit = false;

static char	gCacheNameAdler[64 + 256];
char		*gPlatformName = gCacheNameAdler;

char		gRootDevice[512];
char		gMKextName[512];
char		gMacOSVersion[8];
int			bvCount = 0, gDeviceCount = 0;
//int		menucount = 0;
long		gBootMode; /* defaults to 0 == kBootModeNormal */
BVRef		bvr, menuBVR, bvChain;

static bool				checkOSVersion(const char * version);
static bool				getOSVersion();
static unsigned long	Adler32(unsigned char *buffer, long length);
//static void			selectBiosDevice(void);

/** options.c **/
extern char* msgbuf;
void showTextBuffer(char *buf, int size);


//==========================================================================
// Zero the BSS.

static void zeroBSS(void)
{
	extern char _DATA__bss__begin, _DATA__bss__end;
	extern char _DATA__common__begin, _DATA__common__end;
	
	bzero(&_DATA__bss__begin, (&_DATA__bss__end - &_DATA__bss__begin));
	bzero(&_DATA__common__begin, (&_DATA__common__end - &_DATA__common__begin));
}

//==========================================================================
// Malloc error function

static void malloc_error(char *addr, size_t size, const char *file, int line)
{
	stop("\nMemory allocation error! Addr: 0x%x, Size: 0x%x, File: %s, Line: %d\n",
									 (unsigned)addr, (unsigned)size, file, line);
}

//==========================================================================
//Initializes the runtime. Right now this means zeroing the BSS and initializing malloc.
//
void initialize_runtime(void)
{
	zeroBSS();
	malloc_init(0, 0, 0, malloc_error);
}

//==========================================================================
// execKernel - Load the kernel image (mach-o) and jump to its entry point.

static int ExecKernel(void *binary)
{
	int			ret;
	entry_t		kernelEntry;
	
	bootArgs->kaddr = bootArgs->ksize = 0;
	execute_hook("ExecKernel", (void*)binary, NULL, NULL, NULL);
	
	ret = DecodeKernel(binary,
					   &kernelEntry,
					   (char **) &bootArgs->kaddr,
					   (int *)&bootArgs->ksize );
	
	if ( ret != 0 )
		return ret;
	
	// Reserve space for boot args
	reserveKernBootStruct();
	
	// Notify modules that the kernel has been decoded
	execute_hook("DecodedKernel", (void*)binary, NULL, NULL, NULL);
	
	setupFakeEfi();
	
	// Load boot drivers from the specifed root path.
	//if (!gHaveKernelCache)
	LoadDrivers("/");
	
	execute_hook("DriversLoaded", (void*)binary, NULL, NULL, NULL);
	
	clearActivityIndicator();
	
	if (gErrors) {
		printf("Errors encountered while starting up the computer.\n");
		printf("Pausing %d seconds...\n", kBootErrorTimeout);
		sleep(kBootErrorTimeout);
	}
	
	md0Ramdisk();
	
	verbose("Starting Darwin %s\n",( archCpuType == CPU_TYPE_I386 ) ? "x86" : "x86_64");
	
	// Cleanup the PXE base code.
	
	if ( (gBootFileType == kNetworkDeviceType) && gUnloadPXEOnExit ) {
		if ( (ret = nbpUnloadBaseCode()) != nbpStatusSuccess )
		{
			printf("nbpUnloadBaseCode error %d\n", (int) ret);
			sleep(2);
		}
	}
	
	bool dummyVal;
	if (getBoolForKey(kWaitForKeypressKey, &dummyVal, &bootInfo->chameleonConfig) && dummyVal) {
		showTextBuffer(msgbuf, strlen(msgbuf));
	}
	
	usb_loop();
	
	// Notify modules that the kernel is about to be started
	if (checkOSVersion("10.7"))
	{
		execute_hook("Kernel Start", (void*)kernelEntry, (void*)bootArgs, NULL, NULL);
	}
	else
	{
		execute_hook("Kernel Start", (void*)kernelEntry, (void*)bootArgsPreLion, NULL, NULL);
	}
	
	// If we were in text mode, switch to graphics mode.
	// This will draw the boot graphics unless we are in
	// verbose mode.
	if (gVerboseMode)
		setVideoMode( GRAPHICS_MODE, 0 );
	else
		drawBootGraphics();
	
	setupBooterLog();
	
	finalizeBootStruct();
	
	// Jump to kernel's entry point. There's no going back now.
	if (checkOSVersion("10.7")) {
		
		// Masking out so that Lion doesn't doublefault
		outb(0x21, 0xff);	/* Maskout all interrupts Pic1 */
		outb(0xa1, 0xff);	/* Maskout all interrupts Pic2 */
		
		startprog( kernelEntry, bootArgs );
	}
	else {
		startprog( kernelEntry, bootArgsPreLion );
	}
	
	// Not reached
	return 0;
}


//==========================================================================
// LoadKernelCache - Try to load Kernel Cache.
// return the length of the loaded cache file or -1 on error
long LoadKernelCache(void **binary) {
	char		kernelCacheFile[512];
	char		kernelCachePath[512];
	const char	*val;
	int		    len;
	long		flags, time, cachetime, kerneltime, exttime, ret=-1;
    unsigned long adler32;

	// Determine the name of the Kernel Cache
	if (getValueForKey(kKernelCacheKey, &val, &len, &bootInfo->bootConfig)) {
		if (val[0] == '\\')
		{
			len--;
			val++;
		}
		strlcpy(kernelCacheFile, val, len + 1);
	}
	else {
		// Lion prelink kernel cache file
		if (checkOSVersion("10.7")) {
			sprintf(kernelCacheFile, "%skernelcache", kDefaultCachePathSnow);
		}
		// Snow Leopard prelink kernel cache file
		else if (checkOSVersion("10.6")) {
			sprintf(kernelCacheFile, "kernelcache_%s", (archCpuType == CPU_TYPE_I386)
					? "i386" : "x86_64");
			int lnam = strlen(kernelCacheFile) + 9; //with adler32

			char* name;
			long prev_time = 0;

			struct dirstuff* cacheDir = opendir(kDefaultCachePathSnow);

			while(readdir(cacheDir, (const char**)&name, &flags, &time) >= 0)
			{
				if (((flags & kFileTypeMask) != kFileTypeDirectory) && time > prev_time
					&& strstr(name, kernelCacheFile) && (name[lnam] != '.'))
				{
					sprintf(kernelCacheFile, "%s%s", kDefaultCachePathSnow, name);
					prev_time = time;
				}
			}
		}
		else {
			// Reset cache name.
			bzero(gCacheNameAdler + 64, sizeof(gCacheNameAdler) - 64);
			sprintf(gCacheNameAdler + 64, "%s,%s", gRootDevice, bootInfo->bootFile);
			adler32 = Adler32((unsigned char *)gCacheNameAdler, sizeof(gCacheNameAdler));
			sprintf(kernelCacheFile, "%s.%08lX", kDefaultCachePathLeo, adler32);
		}
	}

	// kernelCacheFile must start with a /
	if (kernelCacheFile[0] != '/') {
		char *str = strdup(kernelCacheFile);
		if (str == NULL)
			return -1;
		sprintf(kernelCacheFile, "/%s", str);
		free(str);
	}

	// Check if the kernel cache file exists
	ret = -1;

	// If boot from a boot helper partition check the kernel cache file on it
	if (gBootVolume->flags & kBVFlagBooter) {
		sprintf(kernelCachePath, "com.apple.boot.P%s", kernelCacheFile);
		ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);
		if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
		{
			sprintf(kernelCachePath, "com.apple.boot.R%s", kernelCacheFile);
			ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);
			if ((ret == -1) || ((flags & kFileTypeMask) != kFileTypeFlat))
			{
				sprintf(kernelCachePath, "com.apple.boot.S%s", kernelCacheFile);
				ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);
				if ((flags & kFileTypeMask) != kFileTypeFlat)
					ret = -1;
			}
		}
	}
	// If not found, use the original kernel cache path.
	if (ret == -1) {
		strcpy(kernelCachePath, kernelCacheFile);
		ret = GetFileInfo(NULL, kernelCachePath, &flags, &cachetime);
		if ((flags & kFileTypeMask) != kFileTypeFlat)
			ret = -1;
	}

	// Exit if kernel cache file wasn't found
	if (ret == -1) {
		verbose("No Kernel Cache File '%s' found\n", kernelCacheFile);
		return -1;
	}

	// Check if the kernel cache file is more recent (mtime)
	// than the kernel file or the S/L/E directory
	ret = GetFileInfo(NULL, bootInfo->bootFile, &flags, &kerneltime);
	// Check if the kernel file is more recent than the cache file
	if ((ret == 0) && ((flags & kFileTypeMask) == kFileTypeFlat)
		&& (kerneltime > cachetime)) {
		verbose("Kernel file (%s) is more recent than KernelCache (%s), ignoring KernelCache\n",
				bootInfo->bootFile, kernelCacheFile);
		return -1;
	}

	ret = GetFileInfo("/System/Library/", "Extensions", &flags, &exttime);
	// Check if the S/L/E directory time is more recent than the cache file
	if ((ret == 0) && ((flags & kFileTypeMask) == kFileTypeDirectory)
		&& (exttime > cachetime)) {
		verbose("/System/Library/Extensions is more recent than KernelCache (%s), ignoring KernelCache\n",
				kernelCacheFile);
		return -1;
	}

	// Since the kernel cache file exists and is the most recent try to load it
	verbose("Loading kernel cache %s\n", kernelCachePath);

	if (checkOSVersion("10.7")) {
		ret = LoadThinFatFile(kernelCachePath, binary);
	} else {
		ret = LoadFile(kernelCachePath);
		*binary = (void *)kLoadAddr;
	}
	return ret; // ret contain the length of the binary
}

//==========================================================================
// This is the entrypoint from real-mode which functions exactly as it did
// before. Multiboot does its own runtime initialization, does some of its
// own things, and then calls common_boot.
void boot(int biosdev)
{
	initialize_runtime();
	// Enable A20 gate before accessing memory above 1Mb.
	enableA20();
	common_boot(biosdev);
}

//==========================================================================
// The 'main' function for the booter. Called by boot0 when booting
// from a block device, or by the network booter.
//
// arguments:
//	 biosdev - Value passed from boot1/NBP to specify the device
//			   that the booter was loaded from.
//
// If biosdev is kBIOSDevNetwork, then this function will return if
// booting was unsuccessful. This allows the PXE firmware to try the
// next boot device on its list.
void common_boot(int biosdev)
{
	bool	 		quiet;
	bool	 		firstRun = true;
	bool	 		instantMenu;
	bool	 		rescanPrompt;
	int				status;
	unsigned int	allowBVFlags = kBVFlagSystemVolume | kBVFlagForeignBoot;
	unsigned int	denyBVFlags = kBVFlagEFISystem;
	
	// Set reminder to unload the PXE base code. Neglect to unload
	// the base code will result in a hang or kernel panic.
	gUnloadPXEOnExit = true;
	
	// Record the device that the booter was loaded from.
	gBIOSDev = biosdev & kBIOSDevMask;
	
	// Initialize boot info structure.
	initKernBootStruct();
	
	initBooterLog();
	
	// Setup VGA text mode.
	// Not sure if it is safe to call setVideoMode() before the
	// config table has been loaded. Call video_mode() instead.
#if DEBUG
	printf("before video_mode\n");
#endif
	video_mode( 2 );  // 80x25 mono text mode.
#if DEBUG
	printf("after video_mode\n");
#endif
	
	// Scan and record the system's hardware information.
	scan_platform();
	
	// First get info for boot volume.
	scanBootVolumes(gBIOSDev, 0);
	bvChain = getBVChainForBIOSDev(gBIOSDev);
	setBootGlobals(bvChain);
	
	// Load boot.plist config file
	status = loadChameleonConfig(&bootInfo->chameleonConfig);
	
	if (getBoolForKey(kQuietBootKey, &quiet, &bootInfo->chameleonConfig) && quiet) {
		gBootMode |= kBootModeQuiet;
	}
	
	// Override firstRun to get to the boot menu instantly by setting "Instant Menu"=y in system config
	if (getBoolForKey(kInstantMenuKey, &instantMenu, &bootInfo->chameleonConfig) && instantMenu) {
		firstRun = false;
	}
	
	// Loading preboot ramdisk if exists.
	loadPrebootRAMDisk();
	
	// Disable rescan option by default
	gEnableCDROMRescan = false;
	
	// Enable it with Rescan=y in system config
	if (getBoolForKey(kRescanKey, &gEnableCDROMRescan, &bootInfo->chameleonConfig)
		&& gEnableCDROMRescan) {
		gEnableCDROMRescan = true;
	}
	
	// Ask the user for Rescan option by setting "Rescan Prompt"=y in system config.
	rescanPrompt = false;
	if (getBoolForKey(kRescanPromptKey, &rescanPrompt , &bootInfo->chameleonConfig)
		&& rescanPrompt && biosDevIsCDROM(gBIOSDev))
	{
		gEnableCDROMRescan = promptForRescanOption();
	}
	
	// Enable touching a single BIOS device only if "Scan Single Drive"=y is set in system config.
	if (getBoolForKey(kScanSingleDriveKey, &gScanSingleDrive, &bootInfo->chameleonConfig)
		&& gScanSingleDrive) {
		gScanSingleDrive = true;
	}
	
	// Create a list of partitions on device(s).
	if (gScanSingleDrive) {
		scanBootVolumes(gBIOSDev, &bvCount);
	} else {
		scanDisks(gBIOSDev, &bvCount);
	}
	
	// Create a separated bvr chain using the specified filters.
	bvChain = newFilteredBVChain(0x80, 0xFF, allowBVFlags, denyBVFlags, &gDeviceCount);
	
	gBootVolume = selectBootVolume(bvChain);
	
	// Intialize module system 
	init_module_system();
		
#if DEBUG
	printf(" Default: %d, ->biosdev: %d, ->part_no: %d ->flags: %d\n",
			 gBootVolume, gBootVolume->biosdev, gBootVolume->part_no, gBootVolume->flags);
	printf(" bt(0,0): %d, ->biosdev: %d, ->part_no: %d ->flags: %d\n",
			 gBIOSBootVolume, gBIOSBootVolume->biosdev, gBIOSBootVolume->part_no, gBIOSBootVolume->flags);
	getchar();
#endif
	
	useGUI = true;
	// Override useGUI default
	getBoolForKey(kGUIKey, &useGUI, &bootInfo->chameleonConfig);
	if (useGUI && initGUI())
	{
		// initGUI() returned with an error, disabling GUI.
		useGUI = false;
	}
	
	setBootGlobals(bvChain);
	
	// Parse args, load and start kernel.
	while (1)
	{
		bool		tryresume, tryresumedefault, forceresume;
		bool		useKernelCache = false; // by default don't use prelink kernel cache
		const char	*val;
		int			len, ret = -1;
		long		flags, sleeptime, time;
		void		*binary = (void *)kLoadAddr;
		
		// additional variable for testing alternate kernel image locations on boot helper partitions.
		char        bootFile[sizeof(bootInfo->bootFile)];
		char		bootFilePath[512];
		
		// Initialize globals.
		sysConfigValid = false;
		gErrors		   = false;
		
		status = getBootOptions(firstRun);
		firstRun = false;
		if (status == -1) continue;
		 
		status = processBootOptions();
		// Status == 1 means to chainboot
		if ( status ==	1 ) break;
		// Status == -1 means that the config file couldn't be loaded or that gBootVolume is NULL
		if ( status == -1 )
		{
			// gBootVolume == NULL usually means the user hit escape.
			if (gBootVolume == NULL)
			{
				freeFilteredBVChain(bvChain);
				
				if (gEnableCDROMRescan)
					rescanBIOSDevice(gBIOSDev);
				
				bvChain = newFilteredBVChain(0x80, 0xFF, allowBVFlags, denyBVFlags, &gDeviceCount);
				setBootGlobals(bvChain);
				setupDeviceList(&bootInfo->themeConfig);
			}
			continue;
		}
		
		// Other status (e.g. 0) means that we should proceed with boot.
		
		// Turn off any GUI elements
		if ( bootArgs->Video.v_display == GRAPHICS_MODE )
		{
			gui.devicelist.draw = false;
			gui.bootprompt.draw = false;
			gui.menu.draw = false;
			gui.infobox.draw = false;
			gui.logo.draw = false;
			drawBackground();
			updateVRAM();
		}
		
		// Find out which version mac os we're booting.
		getOSVersion();
		
		if (platformCPUFeature(CPU_FEATURE_EM64T)) {
			archCpuType = CPU_TYPE_X86_64;
		} else {
			archCpuType = CPU_TYPE_I386;
		}
		
		if (getValueForKey(karch, &val, &len, &bootInfo->chameleonConfig)) {
			if (strncmp(val, "i386", 4) == 0) {
				archCpuType = CPU_TYPE_I386;
			}
		}
		
		if (getValueForKey(kKernelArchKey, &val, &len, &bootInfo->chameleonConfig)) {
			if (strncmp(val, "i386", 4) == 0) {
				archCpuType = CPU_TYPE_I386;
			}
		}
		
		// Notify modules that we are attempting to boot
		execute_hook("PreBoot", NULL, NULL, NULL, NULL);
		
		if (!getBoolForKey (kWake, &tryresume, &bootInfo->chameleonConfig)) {
			tryresume = true;
			tryresumedefault = true;
		} else {
			tryresumedefault = false;
		}
		
		if (!getBoolForKey (kForceWake, &forceresume, &bootInfo->chameleonConfig)) {
			forceresume = false;
		}
		
		if (forceresume) {
			tryresume = true;
			tryresumedefault = false;
		}
		
		while (tryresume) {
			const char *tmp;
			BVRef bvr;
			if (!getValueForKey(kWakeImage, &val, &len, &bootInfo->chameleonConfig))
				val = "/private/var/vm/sleepimage";
			
			// Do this first to be sure that root volume is mounted
			ret = GetFileInfo(0, val, &flags, &sleeptime);
			
			if ((bvr = getBootVolumeRef(val, &tmp)) == NULL)
				break;
			
			// Can't check if it was hibernation Wake=y is required
			if (bvr->modTime == 0 && tryresumedefault)
				break;
			
			if ((ret != 0) || ((flags & kFileTypeMask) != kFileTypeFlat))
				break;
			
			if (!forceresume && ((sleeptime+3)<bvr->modTime)) {
#if DEBUG	
				printf ("Hibernate image is too old by %d seconds. Use ForceWake=y to override\n",
						bvr->modTime-sleeptime);
#endif				  
				break;
			}
			
			HibernateBoot((char *)val);
			break;
		}
		
		verbose("Loading Darwin %s\n", gMacOSVersion);

		useKernelCache = false; // by default don't use prelink kernel cache
		getBoolForKey(kUseKernelCache, &useKernelCache, &bootInfo->chameleonConfig);

		if (useKernelCache) do {

			// If boot from boot helper partitions and OS is Lion use prelink kernel.
			// We need to find a solution to load extra mkext with a prelink kernel.
			if (gBootVolume->flags & kBVFlagBooter && checkOSVersion("10.7")) {
				verbose("Booting from Lion RAID volume so forcing to use KernelCache\n");
				useKernelCache = true;
				break;
			}

			if ((gBootMode & kBootModeSafe) == 1) {
				verbose("Booting in 'Boot Safe' mode, KernelCache will not be used\n");
				useKernelCache = false;
				break;
			}
			if (gOverrideKernel) {
				verbose("Using a non default kernel (%s), KernelCache will not be used\n",
						bootInfo->bootFile);
				useKernelCache = false;
				break;
			}
			if (gMKextName[0] != 0) {
				verbose("Using a specific MKext Cache (%s), KernelCache will not be used\n",
						gMKextName);
				useKernelCache = false;
				break;
			}
			if (gBootFileType != kBlockDeviceType)
				useKernelCache = false;

		} while(0);

		do {
			if (useKernelCache) {
				ret = LoadKernelCache(&binary);
				if (ret >= 0)
					break;
			}

			bool bootFileWithDevice = false;
			// Check if bootFile start with a device ex: bt(0,0)/Extra/mach_kernel
			if (strncmp(bootInfo->bootFile,"bt(",3) == 0 ||
				strncmp(bootInfo->bootFile,"hd(",3) == 0 ||
				strncmp(bootInfo->bootFile,"rd(",3) == 0)
				bootFileWithDevice = true;

			// bootFile must start with a / if it not start with a device name
			if (!bootFileWithDevice && (bootInfo->bootFile)[0] != '/')
				sprintf(bootFile, "/%s", bootInfo->bootFile); // append a leading /
			else
				strlcpy(bootFile, bootInfo->bootFile, sizeof(bootFile));

			// Try to load kernel image from alternate locations on boot helper partitions.
			ret = -1;
			if ((gBootVolume->flags & kBVFlagBooter) && !bootFileWithDevice) {
				sprintf(bootFilePath, "com.apple.boot.P%s", bootFile);
				ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
				if (ret == -1)
				{
					sprintf(bootFilePath, "com.apple.boot.R%s", bootFile);
					ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
					if (ret == -1)
					{
						sprintf(bootFilePath, "com.apple.boot.S%s", bootFile);
						ret = GetFileInfo(NULL, bootFilePath, &flags, &time);
					}
				}
			}
			if (ret == -1) {
				// No alternate location found, using the original kernel image path.
				strlcpy(bootFilePath, bootFile,sizeof(bootFilePath));
			}
			
			verbose("Loading kernel %s\n", bootFilePath);
			ret = LoadThinFatFile(bootFilePath, &binary);
			if (ret <= 0 && archCpuType == CPU_TYPE_X86_64)
			{
				archCpuType = CPU_TYPE_I386;
				ret = LoadThinFatFile(bootFilePath, &binary);
			}
		} while (0);
		
		clearActivityIndicator();
		
#if DEBUG
		printf("Pausing...");
		sleep(8);
#endif
		
		if (ret <= 0) {
			printf("Can't find %s\n", bootFile);
			sleep(1);
			
			if (gBootFileType == kNetworkDeviceType) {
				// Return control back to PXE. Don't unload PXE base code.
				gUnloadPXEOnExit = false;
				break;
			}
			pause();

		} else {
			/* Won't return if successful. */
			ret = ExecKernel(binary);
		}
	}
	
	// chainboot
	if (status == 1) {
		// if we are already in graphics-mode,
		if (getVideoMode() == GRAPHICS_MODE) {
			setVideoMode(VGA_TEXT_MODE, 0); // switch back to text mode.
		}
	}
	
	if ((gBootFileType == kNetworkDeviceType) && gUnloadPXEOnExit) {
		nbpUnloadBaseCode();
	}
}

/*!
	Selects a new BIOS device, taking care to update the global state appropriately.
 */
/*
static void selectBiosDevice(void)
{
	struct DiskBVMap *oldMap = diskResetBootVolumes(gBIOSDev);
	CacheReset();
	diskFreeMap(oldMap);
	oldMap = NULL;
	
	int dev = selectAlternateBootDevice(gBIOSDev);
	
	BVRef bvchain = scanBootVolumes(dev, 0);
	BVRef bootVol = selectBootVolume(bvchain);
	gBootVolume = bootVol;
	setRootVolume(bootVol);
	gBIOSDev = dev;
}
*/

bool checkOSVersion(const char * version) 
{
	return ((gMacOSVersion[0] == version[0]) && (gMacOSVersion[1] == version[1])
			&& (gMacOSVersion[2] == version[2]) && (gMacOSVersion[3] == version[3]));
}

bool getOSVersion()
{
	bool			valid = false;
	const char		*val;
	int				len;
	config_file_t	systemVersion;
	
	if (!loadConfigFile("System/Library/CoreServices/SystemVersion.plist", &systemVersion))
	{
		valid = true;
	}
	else if (!loadConfigFile("System/Library/CoreServices/ServerVersion.plist", &systemVersion))
	{
		valid = true;
	}
	
	if (valid)
	{
		if	(getValueForKey(kProductVersion, &val, &len, &systemVersion))
		{
			// getValueForKey uses const char for val
			// so copy it and trim
			*gMacOSVersion = '\0';
			strncat(gMacOSVersion, val, MIN(len, 4));
		}
		else
			valid = false;
	}
	
	return valid;
}

#define BASE 65521L /* largest prime smaller than 65536 */
#define NMAX 5000
// NMAX (was 5521) the largest n such that 255n(n+1)/2 + (n+1)(BASE-1) <= 2^32-1

#define DO1(buf, i)	{s1 += buf[i]; s2 += s1;}
#define DO2(buf, i)	DO1(buf, i); DO1(buf, i + 1);
#define DO4(buf, i)	DO2(buf, i); DO2(buf, i + 2);
#define DO8(buf, i)	DO4(buf, i); DO4(buf, i + 4);
#define DO16(buf)	DO8(buf, 0); DO8(buf, 8);

unsigned long Adler32(unsigned char *buf, long len)
{
	unsigned long s1 = 1; // adler & 0xffff;
	unsigned long s2 = 0; // (adler >> 16) & 0xffff;
	unsigned long result;
	int k;
	
	while (len > 0) {
		k = len < NMAX ? len : NMAX;
		len -= k;
		while (k >= 16) {
			DO16(buf);
			buf += 16;
			k -= 16;
		}
		if (k != 0) do {
			s1 += *buf++;
			s2 += s1;
		} while (--k);
		s1 %= BASE;
		s2 %= BASE;
	}
	result = (s2 << 16) | s1;
	return OSSwapHostToBigInt32(result);
}