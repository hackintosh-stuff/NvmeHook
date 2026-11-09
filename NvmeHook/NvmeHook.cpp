//
//  NvmeHook.cpp
//  NvmeHook
//
//  Keeps NVMe TRIM away from devices whose firmware does not survive it.
//
//  M0: loads and reports the NVMe controllers on the bus.
//  M1: counts discard traffic at the point where a storage discard becomes an
//      NVMe Dataset Management command. Counters only: every request still runs
//      through to the original implementation, byte for byte.
//
//  Interception points, both in IONVMeFamily's block storage class:
//
//    IONVMeBlockStorageDevice::doUnmap           storage-layer discard entry
//    IONVMeBlockStorageDevice::doUnmapNVMe       builds the NVMe DSM command
//
//  The first is `seen`, the second is the ground-truth `dsm` counter: if a
//  discard reaches the hardware it must pass through doUnmapNVMe. Stage A of the
//  test plan expects both to rise together with blocking off.
//
//  Locating the functions: IONVMeFamily is fused into the boot kernel collection,
//  so the image mapped into the kext submap carries only headers and data — its
//  code lives in the collection's text region at the link-time address plus the
//  kernel slide. The slide is measured at runtime against `_kmod`, whose link
//  address and runtime address are both known, and every address is verified
//  against the recorded prologue bytes before anything is patched.
//
//  The report is published to the IORegistry, not the kernel log: the ring
//  buffer is overwritten by boot traffic within a minute, while registry
//  properties stay readable for the whole boot.
//

#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <Headers/kern_patcher.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>

static const char *bootargOff[] = { "-nvmehookoff" };
static const char *bootargDebug[] = { "-nvmehookdbg" };
static const char *bootargBeta[] = { "-nvmehookbeta" };

// Properties IONVMeFamily publishes on each controller. The device match is built
// from these, so M0 only reads them back to prove they are visible from a plugin.
static const char *keyModel = "Model Number";
static const char *keySerial = "Serial Number";

static mach_vm_address_t orgDoUnmap {0};
static mach_vm_address_t orgDoUnmapNVMe {0};

// Per-device counters. Fixed capacity: this machine has one NVMe controller, and
// a discard request must never allocate.
struct Counters {
	IOService *device {nullptr};
	uint64_t seen {0};
	uint64_t dsm {0};
};

static constexpr uint32_t maxDevices = 8;
static Counters counters[maxDevices] {};

static const char *stringProperty(IOService *service, const char *key) {
	auto value = OSDynamicCast(OSString, service->getProperty(key));
	return value != nullptr ? value->getCStringNoCopy() : "<absent>";
}

static void publishStatus(const char *key, const char *value) {
	if (auto resources = IOService::getResourceService())
		resources->setProperty(key, value);
}

static void publishAddress(const char *key, mach_vm_address_t address) {
	char value[32];
	snprintf(value, sizeof(value), "0x%llx", address);
	publishStatus(key, value);
}

static Counters *countersFor(IOService *device) {
	for (uint32_t i = 0; i < maxDevices; i++)
		if (counters[i].device == device)
			return &counters[i];

	for (uint32_t i = 0; i < maxDevices; i++) {
		if (counters[i].device == nullptr) {
			counters[i].device = device;
			return &counters[i];
		}
	}

	return nullptr;
}

// Registry writes take locks, so they are kept off the hot path: the first few
// events are published one by one, then every 64th.
static void publishCounters(Counters *c) {
	uint64_t total = c->seen + c->dsm;
	if (total > 16 && (total & 0x3F) != 0)
		return;

	char value[96];
	snprintf(value, sizeof(value), "seen=%llu; dsm=%llu", c->seen, c->dsm);
	c->device->setProperty("NvmeHookCounters", value);
}

static IOReturn doUnmapHook(void *self, IOBlockStorageDeviceExtent *extents,
							uint32_t extentsCount, uint32_t options) {
	auto device = static_cast<IOService *>(self);
	if (auto c = countersFor(device)) {
		c->seen++;
		publishCounters(c);
	}

	auto org = reinterpret_cast<IOReturn (*)(void *, IOBlockStorageDeviceExtent *, uint32_t, uint32_t)>(orgDoUnmap);
	return org(self, extents, extentsCount, options);
}

// The request object and the LBA range entry array are opaque here: M1 passes
// them through untouched and only counts the calls.
static IOReturn doUnmapNVMeHook(void *self, void *request, IOBlockStorageDeviceExtent *extents,
								uint32_t extentsCount, void *rangeEntry) {
	auto device = static_cast<IOService *>(self);
	if (auto c = countersFor(device)) {
		c->dsm++;
		publishCounters(c);
	}

	auto org = reinterpret_cast<IOReturn (*)(void *, void *, IOBlockStorageDeviceExtent *, uint32_t, void *)>(orgDoUnmapNVMe);
	return org(self, request, extents, extentsCount, rangeEntry);
}

static void reportBus() {
	uint32_t index = 0;

	auto matching = IOService::serviceMatching("IONVMeController");
	if (matching == nullptr) {
		publishStatus("NvmeHookBus", "cannot build a matching dictionary");
		return;
	}

	if (auto iterator = IOService::getMatchingServices(matching)) {
		while (auto service = OSDynamicCast(IOService, iterator->getNextObject())) {
			char key[40];
			snprintf(key, sizeof(key), "NvmeHookController%u", index);

			char report[256];
			snprintf(report, sizeof(report), "model=%s; serial=%s",
					 stringProperty(service, keyModel), stringProperty(service, keySerial));

			service->setProperty(key, report);
			SYSLOG("nvmehook", "controller %u: %s", index, report);
			index++;
		}
		iterator->release();
	}

	matching->release();

	char status[96];
	snprintf(status, sizeof(status), "%u controller(s) visible at plugin start", index);
	publishStatus("NvmeHookBus", status);
}

// IONVMeFamily is prelinked into the boot kernel collection, so its symbols are
// not visible through the kernel MachInfo (index 0). They are resolved through a
// kext entry instead, and the `Loaded` flag makes Lilu report a kext that is
// already in memory.
static const char *kextIONVMeFamilyPaths[] = { "IONVMeFamily" };
static KernelPatcher::KextInfo kextIONVMeFamily {
	"com.apple.iokit.IONVMeFamily", kextIONVMeFamilyPaths, arrsize(kextIONVMeFamilyPaths),
	{true}, {}
};

// Link-time addresses from the boot kernel collection of the installed build
// (Darwin 25.6.0, kernel UUID 28267FD5-...). They are constants of that build:
// an OS update changes them, and the byte check below then refuses to patch
// instead of writing somewhere random. Refresh them from the collection:
//
//   sudo nm -n /System/Library/KernelCollections/BootKernelExtensions.kc \
//     | grep -E ' T _kmod$|IONVMeBlockStorageDevice(7doUnmap|11doUnmapNVMe)'
static const mach_vm_address_t linkKmod = 0xFFFFFF800139FF58ULL;
static const mach_vm_address_t linkDoUnmap = 0xFFFFFF80026D0396ULL;
static const mach_vm_address_t linkDoUnmapNVMe = 0xFFFFFF80026D016CULL;

// First 21 bytes of each function, read from that same collection. Both open with
// the same frame setup; the argument spills after it distinguish them.
static const uint8_t patternDoUnmap[] {
	0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
	0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x89, 0xCB, 0x41, 0x89
};

static const uint8_t patternDoUnmapNVMe[] {
	0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41,
	0x54, 0x53, 0x48, 0x83, 0xEC, 0x28, 0x48, 0x89, 0x75, 0xC0
};

static void processKext(KernelPatcher &patcher, size_t, mach_vm_address_t slide, size_t size) {
	publishAddress("NvmeHookSlide", slide);
	publishAddress("NvmeHookArgSize", size);

	auto kmod = patcher.solveSymbol(KernelPatcher::KernelID, "_kmod");
	publishAddress("NvmeHookKmodRuntime", kmod);
	if (kmod == 0) {
		publishStatus("NvmeHookHooks", "no _kmod symbol, nothing patched");
		return;
	}

	auto kernelSlide = kmod - linkKmod;
	publishAddress("NvmeHookKernelSlide", kernelSlide);

	auto addressDoUnmap = linkDoUnmap + kernelSlide;
	auto addressDoUnmapNVMe = linkDoUnmapNVMe + kernelSlide;

	publishAddress("NvmeHookAddrDoUnmap", addressDoUnmap);
	publishAddress("NvmeHookAddrDoUnmapNVMe", addressDoUnmapNVMe);

	// Everything is verified before it is written: a stale link-time address must
	// fail here rather than redirect a live code path.
	bool matchesDoUnmap = memcmp(reinterpret_cast<const void *>(addressDoUnmap),
								 patternDoUnmap, sizeof(patternDoUnmap)) == 0;
	bool matchesDoUnmapNVMe = memcmp(reinterpret_cast<const void *>(addressDoUnmapNVMe),
									 patternDoUnmapNVMe, sizeof(patternDoUnmapNVMe)) == 0;

	if (!matchesDoUnmap && !matchesDoUnmapNVMe) {
		publishStatus("NvmeHookHooks", "prologue mismatch, nothing patched");
		return;
	}

	if (matchesDoUnmap)
		orgDoUnmap = patcher.routeFunction(addressDoUnmap, reinterpret_cast<mach_vm_address_t>(doUnmapHook), true);
	if (matchesDoUnmapNVMe)
		orgDoUnmapNVMe = patcher.routeFunction(addressDoUnmapNVMe, reinterpret_cast<mach_vm_address_t>(doUnmapNVMeHook), true);

	const char *state = "routing failed";
	if (orgDoUnmap != 0 && orgDoUnmapNVMe != 0)
		state = "counters armed (no blocking)";
	else if (orgDoUnmap != 0)
		state = "doUnmap only";
	else if (orgDoUnmapNVMe != 0)
		state = "doUnmapNVMe only";
	else if (!matchesDoUnmap)
		state = "doUnmap prologue mismatch";
	else if (!matchesDoUnmapNVMe)
		state = "doUnmapNVMe prologue mismatch";

	publishStatus("NvmeHookHooks", state);
	SYSLOG("nvmehook", "counter hooks: %s", state);
}

static void kextLoaded(void *, KernelPatcher &patcher, size_t index, mach_vm_address_t slide, size_t size) {
	processKext(patcher, index, slide, size);
}

// Lilu hands its patcher over here. It initialises that patcher itself on one
// boot path and not on another, and without `init()` there is no prelink buffer,
// which is what kernel-collection kext symbols are read through. Establish which
// case this is before the kext callback needs it.
static void patchersReady(void *, KernelPatcher &patcher) {
	auto kmod = patcher.solveSymbol(KernelPatcher::KernelID, "_kmod");
	if (kmod == 0) {
		patcher.init();
		publishStatus("NvmeHookInit", "initialised by the plugin");
		kmod = patcher.solveSymbol(KernelPatcher::KernelID, "_kmod");
	} else {
		publishStatus("NvmeHookInit", "already initialised by Lilu");
	}
	publishAddress("NvmeHookSymKmod", kmod);
}

static void chefStart() {
	publishStatus("NvmeHookVersion", xStringify(MODULE_VERSION));
	publishStatus("NvmeHookPatches", "counters only (M1)");
	reportBus();
	lilu.onPatcherLoad(patchersReady);
	lilu.onKextLoad(&kextIONVMeFamily, 1, kextLoaded);

	SYSLOG("nvmehook", "%s %s loaded; counters only, no request is altered",
		   xStringify(PRODUCT_NAME), xStringify(MODULE_VERSION));
}

PluginConfiguration ADDPR(config) = {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal | LiluAPI::AllowSafeMode,
	bootargOff, arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta, arrsize(bootargBeta),
	KernelVersion::BigSur,
	KernelVersion::Tahoe,
	chefStart
};
