//
//  NvmeHook.cpp
//  NvmeHook
//
//  Keeps NVMe TRIM away from devices whose firmware does not survive it, and
//  reports which attached controller is the known-faulty target.
//
//  M1: counts discard traffic at the point where a storage discard becomes an
//      NVMe Dataset Management command. Counters only: every request still runs
//      through to the original implementation, byte for byte.
//  M1.8: Lilu's own resolution is broken for Tahoe kernel collections (kext and
//      kernel symbols both fail; the kext kmod entries report address 0), and
//      scanning kernel VM for the collection header faulted in the wild.
//      Everything is anchored to a live object instead: the first word of an
//      IONVMeController is its vptr, an address inside IONVMeFamily, and the
//      hooks are a fixed link-time distance away from it, so the KASLR slide
//      cancels out.
//  M1.9: the earlier version resolved those distances by reading
//      BootKernelExtensions.kc through vnode_lookup/vn_rdwr from its own kernel
//      thread. That path panicked in namei ("Taking non-sleepable RW lock with
//      preemption enabled") when boot timing moved, because a bare kernel
//      thread has no VFS context. The driver no longer touches the file
//      system: the distances are constants measured from the same collection,
//      and each candidate is fingerprinted (24 leading code bytes) before
//      anything is routed. A build whose code does not match aborts with a
//      status instead of patching, which is the one thing the symbol approach
//      did better and the reason the fingerprints are checked at all.
//
//  M2: the matched device loses every discard. The decision is taken in
//      doUnmap, before the original acquires the work-loop gate, allocates the
//      DSM buffer or takes a request from the pool, so the short-circuit is a
//      complete no-op that leaves nothing to unwind. The identity is resolved
//      once per device from the block storage device's own registry properties
//      and cached in the counters table; unmatched devices, and devices whose
//      identity is not published yet, pass through untouched.
//      The rules come from the kext's Info.plist (NvmeHookDevices), so several
//      model/serial pairs can be suppressed without touching the source, and
//      "*" is accepted as "any value" in either field. There is no boot-arg
//      switch any more: loading the plugin means its rules apply.
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
#include <IOKit/IOLib.h>
#include <IOKit/storage/IOBlockStorageDevice.h>
#include <IOKit/storage/IOStorage.h>
#include <kern/thread.h>
#include <libkern/c++/OSArray.h>
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/OSAtomic.h>
#include <libkern/libkern.h>

static const char *bootargOff[] = { "-nvmehookoff" };
static const char *bootargDebug[] = { "-nvmehookdbg" };
static const char *bootargBeta[] = { "-nvmehookbeta" };

// Devices to suppress, defined in this kext's Info.plist under the personality:
//
//   <key>NvmeHookDevices</key>
//   <array>
//     <dict>
//       <key>Model</key>  <string>INTEL SSDPD21K750GAM</string>
//       <key>Serial</key> <string>*</string>
//     </dict>
//   </array>
//
// IOKit copies personality keys onto the service instance, so the list is read
// from ADDPR(selfInstance) rather than compiled in. Several entries are allowed;
// "*" in either field matches any value, which is what makes a model-only rule
// possible. An entry whose model and serial both match blocks that device; there
// is no boot argument that changes the behaviour — loading the plugin means the
// rules apply.
static constexpr uint32_t kMaxIdentities = 4;
static constexpr uint32_t kModelLength = 64;
static constexpr uint32_t kSerialLength = 32;
static constexpr char kDevicesKey[] = "NvmeHookDevices";

struct DeviceIdentity {
	char model[kModelLength];
	char serial[kSerialLength];
};

static DeviceIdentity identities[kMaxIdentities] {};
static uint32_t identityCount {0};
static bool configLoaded {false};
static const char *configState = "not read yet";

// Properties IONVMeFamily publishes on each controller.
static const char *keyModel = "Model Number";
static const char *keySerial = "Serial Number";
static const char *keyVendor = "Vendor Identifier";

// Distance from an IONVMeController's vptr to each hook, measured in this
// collection (macOS 26.7.2, 25G332, com.apple.iokit.IONVMeFamily). The vptr is
// __ZTV16IONVMeController + 2 * sizeof(uintptr_t), so
//
//     hook = vptr + (hook link address - (__ZTV link address + 16))
//
// and the KASLR slide cancels out. A different build changes these numbers,
// which is what the fingerprints below exist to catch.
static constexpr int64_t kDeltaDoUnmap = -0x1AC8A;
static constexpr int64_t kDeltaDoUnmapNVMe = -0x1AEB4;
static constexpr int64_t kDeltaInitializeController = -0x28714;

// The first 24 code bytes of each hook. Twenty-four and not sixteen: the two
// block-device prologues share their first sixteen bytes and only diverge where
// they save their arguments. Checked before routing, so a build that moved
// these functions aborts instead of patching something else.
static constexpr uint32_t kFingerprintLength = 24;
static const uint8_t kFingerprintDoUnmap[kFingerprintLength] {
	0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
	0x53, 0x48, 0x83, 0xec, 0x28, 0x89, 0xcb, 0x41, 0x89, 0xd6, 0x49, 0x89,
};
static const uint8_t kFingerprintDoUnmapNVMe[kFingerprintLength] {
	0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
	0x53, 0x48, 0x83, 0xec, 0x28, 0x48, 0x89, 0x75, 0xc0, 0x48, 0x89, 0x7d,
};
static const uint8_t kFingerprintInitializeController[kFingerprintLength] {
	0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x50, 0x48, 0x89,
	0xfb, 0x48, 0x8b, 0x07, 0xff, 0x90, 0x88, 0x0a, 0x00, 0x00, 0x85, 0xc0,
};

static mach_vm_address_t orgDoUnmap {0};
static mach_vm_address_t orgDoUnmapNVMe {0};
static mach_vm_address_t orgInitializeController {0};
static KernelPatcher *gPatcher {nullptr};

// Per-device counters. Fixed capacity: this machine has one NVMe controller, and
// a discard request must never allocate.
struct Counters {
	IOService *device {nullptr};
	uint64_t seen {0};
	uint64_t blocked {0};
	uint64_t dsm {0};
	uint32_t index {0};
	bool identityKnown {false};
	int32_t identityIndex {-1};
};

static constexpr uint32_t maxDevices = 8;
static Counters counters[maxDevices] {};
static uint32_t counterCount {0};

// Counters are incremented from whatever thread issued the discard, and two
// discards on one device can enter the hook concurrently (the original's
// work-loop gate is taken later), so increments are atomic.
static void bump(uint64_t *counter) {
	OSAddAtomic64(1, reinterpret_cast<volatile SInt64 *>(counter));
}

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

// The block storage device keeps its identity in a "Device Characteristics"
// dictionary; controllers publish the same pair as flat properties. The block
// device is what the hook actually receives, so its own copy is the one the
// block decision reads: no controller pointer arithmetic on the request path.
static const char *identityPropertyValue(IOService *service, const char *key, const char *dictKey) {
	if (auto value = OSDynamicCast(OSString, service->getProperty(key)))
		return value->getCStringNoCopy();

	if (auto dict = OSDynamicCast(OSDictionary, service->getProperty("Device Characteristics")))
		if (auto value = OSDynamicCast(OSString, dict->getObject(dictKey)))
			return value->getCStringNoCopy();

	return nullptr;
}

// NVMe Identify strings are space padded to a fixed field width and the
// registry copy may keep that padding, so trailing blanks are ignored. A rule
// field of "*" matches any value; anything else must be equal, and an empty
// device value only matches "*" — a missing property never becomes a wildcard
// by itself.
static size_t trimmedLength(const char *value) {
	size_t length = strlen(value);
	while (length != 0 && (value[length - 1] == ' ' || value[length - 1] == '\t'))
		length--;
	return length;
}

static bool fieldMatches(const char *actual, const char *wanted) {
	if (wanted[0] == '*' && wanted[1] == '\0')
		return true;

	size_t actualLength = actual != nullptr ? trimmedLength(actual) : 0;
	size_t wantedLength = trimmedLength(wanted);
	return actualLength != 0 && actualLength == wantedLength &&
		   strncmp(actual, wanted, actualLength) == 0;
}

// The configured rules are tried in order; the first match wins and its index is
// what the registry reports.
static int32_t matchIdentity(const char *model, const char *serial) {
	for (uint32_t i = 0; i < identityCount; i++)
		if (fieldMatches(model, identities[i].model) &&
			fieldMatches(serial, identities[i].serial))
			return static_cast<int32_t>(i);
	return -1;
}

static void appendTo(char *buffer, size_t size, const char *text) {
	size_t used = strlen(buffer);
	if (used + 1 >= size)
		return;
	strlcpy(buffer + used, text, size - used);
}

static void publishConfig() {
	char report[480];
	snprintf(report, sizeof(report), "state=%s; identities=%u", configState, identityCount);
	for (uint32_t i = 0; i < identityCount; i++) {
		char entry[128];
		snprintf(entry, sizeof(entry), "; [%u] model=%s serial=%s",
				 i, identities[i].model, identities[i].serial);
		appendTo(report, sizeof(report), entry);
	}
	publishStatus("NvmeHookConfig", report);
}

// Read the device list from the plugin's own personality once. The instance may
// not exist yet when the first discard arrives, in which case the identity stays
// unresolved and is retried on the next event: the rules are never guessed.
static void loadIdentities() {
	if (configLoaded)
		return;

	auto instance = ADDPR(selfInstance);
	if (instance == nullptr) {
		configState = "plugin service not started";
		return;
	}

	configLoaded = true;
	auto array = OSDynamicCast(OSArray, instance->getProperty(kDevicesKey));
	if (array == nullptr) {
		configState = "NvmeHookDevices missing from Info.plist";
		publishConfig();
		return;
	}

	uint32_t entries = array->getCount();
	for (uint32_t i = 0; i < entries && identityCount < kMaxIdentities; i++) {
		auto entry = OSDynamicCast(OSDictionary, array->getObject(i));
		if (entry == nullptr)
			continue;
		auto model = OSDynamicCast(OSString, entry->getObject("Model"));
		auto serial = OSDynamicCast(OSString, entry->getObject("Serial"));
		if (model == nullptr || serial == nullptr)
			continue;
		strlcpy(identities[identityCount].model, model->getCStringNoCopy(), kModelLength);
		strlcpy(identities[identityCount].serial, serial->getCStringNoCopy(), kSerialLength);
		identityCount++;
	}

	configState = identityCount != 0 ? "ok" : "no usable entry in NvmeHookDevices";
	publishConfig();
	SYSLOG("nvmehook", "config: %s, %u identity(ies) from Info.plist", configState, identityCount);
}

static Counters *countersFor(IOService *device) {
	for (uint32_t i = 0; i < counterCount; i++)
		if (counters[i].device == device)
			return &counters[i];

	// A slot is claimed with a compare-and-swap so that two concurrent first
	// discards cannot hand out the same index. A racing thread that reads the
	// raised count before the slot is filled only sees a null device pointer,
	// which the scan above skips.
	uint32_t index = counterCount;
	while (true) {
		if (index >= maxDevices)
			return nullptr;
		if (OSCompareAndSwap(index, index + 1, &counterCount))
			break;
		index = counterCount;
	}

	auto c = &counters[index];
	c->index = index;
	c->device = device;
	return c;
}

// Resolve and cache the block decision once per device. Device Characteristics
// is published by SetNVMeProperties during device start, i.e. before any media
// exists, so by the time a discard can arrive the answer is normally available
// on the first call; if it is not, the decision stays open and is retried on
// the next event instead of being guessed.
static void resolveIdentity(Counters *c) {
	loadIdentities();
	if (!configLoaded)
		return;

	const char *model = identityPropertyValue(c->device, keyModel, "Product Name");
	const char *serial = identityPropertyValue(c->device, keySerial, keySerial);
	if (model == nullptr || serial == nullptr)
		return;

	c->identityKnown = true;
	c->identityIndex = matchIdentity(model, serial);

	char key[40];
	snprintf(key, sizeof(key), "NvmeHookDevice%u", c->index);
	char value[384];
	if (c->identityIndex >= 0)
		snprintf(value, sizeof(value), "model=%s; serial=%s; match=[%d] model=%s serial=%s",
				 model, serial, c->identityIndex,
				 identities[c->identityIndex].model, identities[c->identityIndex].serial);
	else
		snprintf(value, sizeof(value), "model=%s; serial=%s; match=none", model, serial);
	publishStatus(key, value);
	SYSLOG("nvmehook", "device %u: %s", c->index, value);
}

// Registry writes take locks, so they are kept off the hot path: the first few
// events are published one by one, then every 64th.
static void publishCounters(Counters *c) {
	uint64_t total = c->seen + c->dsm;
	if (total > 16 && (total & 0x3F) != 0)
		return;

	char value[128];
	snprintf(value, sizeof(value), "seen=%llu; blocked=%llu; dsm=%llu",
			 c->seen, c->blocked, c->dsm);
	c->device->setProperty("NvmeHookCounters", value);
}

static void publishTotal() {
	uint64_t seen = 0;
	uint64_t blocked = 0;
	uint64_t dsm = 0;
	for (uint32_t i = 0; i < counterCount; i++) {
		seen += counters[i].seen;
		blocked += counters[i].blocked;
		dsm += counters[i].dsm;
	}

	char value[96];
	snprintf(value, sizeof(value), "seen=%llu; blocked=%llu; dsm=%llu", seen, blocked, dsm);
	publishStatus("NvmeHookTotal", value);
}

// The suppression point. doUnmap is where a storage discard becomes a DSM
// command, and the original does four things before it submits anything: take
// the work-loop gate, allocate and map the 4 KiB DSM buffer, increment the
// device's unmap statistic, and take a request from the pool. Short-circuiting
// at entry skips all four, so there is no half-built request and no state to
// unwind; the caller sees a success, which is what a suppressed discard is.
// Unmatched devices, and devices whose identity is not published yet, reach the
// trampoline unchanged.
static IOReturn doUnmapHook(void *self, IOBlockStorageDeviceExtent *extents,
							uint32_t extentsCount, uint32_t options) {
	auto device = static_cast<IOService *>(self);
	if (auto c = countersFor(device)) {
		bump(&c->seen);
		if (!c->identityKnown)
			resolveIdentity(c);

		bool block = c->identityKnown && c->identityIndex >= 0;
		if (block)
			bump(&c->blocked);

		publishCounters(c);
		publishTotal();

		if (block)
			return kIOReturnSuccess;
	}

	auto org = reinterpret_cast<IOReturn (*)(void *, IOBlockStorageDeviceExtent *, uint32_t, uint32_t)>(orgDoUnmap);
	return org(self, extents, extentsCount, options);
}

// The command that actually leaves for the hardware, and the ground truth for
// the suppression gate: on a blocked device this counter must stay flat while
// `blocked` tracks `seen`. The request object and the LBA range entry array are
// opaque here; this hook counts and never alters behaviour.
static IOReturn doUnmapNVMeHook(void *self, void *request, IOBlockStorageDeviceExtent *extents,
								uint32_t extentsCount, void *rangeEntry) {
	auto device = static_cast<IOService *>(self);
	if (auto c = countersFor(device)) {
		bump(&c->dsm);
		publishCounters(c);
		publishTotal();
	}

	auto org = reinterpret_cast<IOReturn (*)(void *, void *, IOBlockStorageDeviceExtent *, uint32_t, void *)>(orgDoUnmapNVMe);
	return org(self, request, extents, extentsCount, rangeEntry);
}

// Controller-level identity report, from the controller's own properties. The
// hook above decides per block storage device; this one says, once per start,
// which configured rule (if any) this controller falls under.
static void reportControllerIdentity(IOService *controller) {
	const char *model = stringProperty(controller, keyModel);
	const char *serial = stringProperty(controller, keySerial);
	loadIdentities();
	int32_t matched = configLoaded ? matchIdentity(model, serial) : -1;

	char report[320];
	if (matched >= 0) {
		snprintf(report, sizeof(report),
				 "matched rule [%d] (model=%s; serial=%s) on controller model=%s serial=%s; discards are suppressed",
				 matched, identities[matched].model, identities[matched].serial, model, serial);
	} else {
		snprintf(report, sizeof(report),
				 "pass-through controller (model=%s; serial=%s)", model, serial);
	}

	controller->setProperty("NvmeHookTarget", report);
	publishStatus("NvmeHookTarget", report);
	SYSLOG("nvmehook", "identity: %s", report);
}

static IOReturn initializeControllerHook(void *self) {
	auto org = reinterpret_cast<IOReturn (*)(void *)>(orgInitializeController);
	IOReturn result = org(self);
	reportControllerIdentity(static_cast<IOService *>(self));
	return result;
}
// Read the leading code bytes of a resolved hook and compare them with the
// fingerprint measured on the build these distances came from. Reading the
// candidate is safe by construction: it is a small constant offset from a live
// vptr, so it lands in the same image that pointer belongs to.
static bool fingerprintMatches(mach_vm_address_t address, const uint8_t *expected) {
	auto bytes = reinterpret_cast<const uint8_t *>(address);
	for (uint32_t i = 0; i < kFingerprintLength; i++)
		if (bytes[i] != expected[i])
			return false;
	return true;
}

// Resolve every interception point and route the hooks. Runs on a worker
// thread once an IONVMeController exists, because every address is expressed
// relative to that live object's vtable pointer. Instances are probed in turn:
// the first published controller is not necessarily driven by IONVMeFamily
// (this machine has also carried a RAID card), and a subclass would have a
// different vtable at a different distance, so the fingerprints decide which
// instance is the anchor, not the enumeration order.
static void workerThread(void *, wait_result_t) {
	auto matching = IOService::serviceMatching("IONVMeController");
	if (matching == nullptr) {
		publishStatus("NvmeHookHooks", "cannot match IONVMeController");
		return;
	}

	auto first = IOService::waitForMatchingService(matching, 120ULL * 1000ULL * 1000ULL * 1000ULL);
	if (first == nullptr) {
		matching->release();
		publishStatus("NvmeHookHooks", "no IONVMeController within 120s");
		return;
	}
	first->release();

	// Collect the instances (bounded; this machine has a handful at most).
	IOService *instances[8] {};
	uint32_t instanceCount = 0;
	if (auto iterator = IOService::getMatchingServices(matching)) {
		while (instanceCount < 8) {
			auto service = OSDynamicCast(IOService, iterator->getNextObject());
			if (service == nullptr)
				break;
			service->retain();
			instances[instanceCount++] = service;
		}
		iterator->release();
	}
	matching->release();

	if (instanceCount == 0) {
		publishStatus("NvmeHookHooks", "controller vanished before probe");
		return;
	}

	for (uint32_t i = 0; i < instanceCount; i++) {
		char key[32];
		snprintf(key, sizeof(key), "NvmeHookVptr%u", i);
		publishAddress(key, *reinterpret_cast<uintptr_t *>(instances[i]));
	}
	char count[16];
	snprintf(count, sizeof(count), "%u", instanceCount);
	publishStatus("NvmeHookInstanceCount", count);

	// The identity report covers every controller: the matched device may sit
	// behind any of them, and the registry publishes model and serial per
	// controller, not per family.
	for (uint32_t i = 0; i < instanceCount; i++)
		reportControllerIdentity(instances[i]);

	mach_vm_address_t anchor {0};
	mach_vm_address_t addressDoUnmap {0};
	mach_vm_address_t addressDoUnmapNVMe {0};
	mach_vm_address_t addressInitializeController {0};

	// Probe every instance. The candidates are constant distances from the
	// vptr, so they are inside the same image whatever the slide is; the
	// fingerprints then say whether this image is the build the distances were
	// measured on. The first instance that matches is the anchor.
	for (uint32_t i = 0; i < instanceCount && anchor == 0; i++) {
		auto vptr = *reinterpret_cast<uintptr_t *>(instances[i]);
		auto candidateDoUnmap = static_cast<mach_vm_address_t>(
			static_cast<int64_t>(vptr) + kDeltaDoUnmap);
		auto candidateDoUnmapNVMe = static_cast<mach_vm_address_t>(
			static_cast<int64_t>(vptr) + kDeltaDoUnmapNVMe);
		auto candidateInitializeController = static_cast<mach_vm_address_t>(
			static_cast<int64_t>(vptr) + kDeltaInitializeController);

		// The vtable is mapped by definition, and its first entry is a method
		// in the same image. Anything further than 4 MB from that method is not
		// this image, so it is skipped instead of being read: a fingerprint
		// read is only safe inside a mapping we know exists.
		auto firstMethod = static_cast<int64_t>(reinterpret_cast<const uintptr_t *>(vptr)[0]);
		auto nearAnchorImage = [&](mach_vm_address_t candidate) {
			auto delta = static_cast<int64_t>(candidate) - firstMethod;
			return delta > -0x400000 && delta < 0x400000;
		};

		bool near = nearAnchorImage(candidateDoUnmap) &&
					nearAnchorImage(candidateDoUnmapNVMe) &&
					nearAnchorImage(candidateInitializeController);
		bool matched = near &&
					   fingerprintMatches(candidateDoUnmap, kFingerprintDoUnmap) &&
					   fingerprintMatches(candidateDoUnmapNVMe, kFingerprintDoUnmapNVMe) &&
					   fingerprintMatches(candidateInitializeController, kFingerprintInitializeController);

		char probe[224];
		snprintf(probe, sizeof(probe),
				 "i=%u vptr=0x%llx doUnmap=0x%llx doUnmapNVMe=0x%llx init=0x%llx fingerprint=%s",
				 i, static_cast<unsigned long long>(vptr),
				 static_cast<unsigned long long>(candidateDoUnmap),
				 static_cast<unsigned long long>(candidateDoUnmapNVMe),
				 static_cast<unsigned long long>(candidateInitializeController),
				 !near ? "skipped (foreign image)" : (matched ? "match" : "mismatch"));
		publishStatus("NvmeHookAnchor", probe);

		if (!matched)
			continue;

		anchor = static_cast<mach_vm_address_t>(vptr);
		addressDoUnmap = candidateDoUnmap;
		addressDoUnmapNVMe = candidateDoUnmapNVMe;
		addressInitializeController = candidateInitializeController;
	}

	if (anchor == 0) {
		publishStatus("NvmeHookHooks", "no controller matches the measured layout, nothing patched");
		goto releaseInstances;
	}

	{
		publishAddress("NvmeHookVptr", anchor);
		publishAddress("NvmeHookSymDoUnmap", addressDoUnmap);
		publishAddress("NvmeHookSymDoUnmapNVMe", addressDoUnmapNVMe);
		publishAddress("NvmeHookSymInitializeController", addressInitializeController);

		orgDoUnmap = gPatcher->routeFunction(addressDoUnmap,
											 reinterpret_cast<mach_vm_address_t>(doUnmapHook), true);
		orgDoUnmapNVMe = gPatcher->routeFunction(addressDoUnmapNVMe,
												 reinterpret_cast<mach_vm_address_t>(doUnmapNVMeHook), true);
		orgInitializeController = gPatcher->routeFunction(addressInitializeController,
														  reinterpret_cast<mach_vm_address_t>(initializeControllerHook), true);

		// doUnmap is the hook that can drop a request, so it is named first and
		// called out separately: if it failed to route while the counter hook
		// succeeded, the driver is reporting traffic it cannot suppress.
		char state[192];
		if (orgDoUnmap != 0 && orgDoUnmapNVMe != 0 && orgInitializeController != 0) {
			snprintf(state, sizeof(state), "armed by anchor: doUnmap + counters + InitializeController");
		} else if (orgDoUnmapNVMe != 0 && orgInitializeController != 0) {
			snprintf(state, sizeof(state), "counters armed but doUnmap unresolved, nothing is blocked");
		} else if (orgDoUnmapNVMe != 0) {
			snprintf(state, sizeof(state), "counters armed; doUnmap and InitializeController unresolved");
		} else {
			snprintf(state, sizeof(state), "routing failed, nothing patched");
		}

		publishStatus("NvmeHookHooks", state);
		SYSLOG("nvmehook", "hooks: %s", state);
	}

releaseInstances:
	for (uint32_t i = 0; i < instanceCount; i++)
		instances[i]->release();
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

// Lilu hands its patcher over here once kernel patching is possible; from this
// point a worker thread does the deferred, panic-free resolution.
static void patchersReady(void *, KernelPatcher &patcher) {
	gPatcher = &patcher;

	thread_t thread = nullptr;
	if (kernel_thread_start(reinterpret_cast<thread_continue_t>(workerThread), nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		publishStatus("NvmeHookWorker", "started");
	} else {
		publishStatus("NvmeHookWorker", "failed to start");
	}
}

static void chefStart() {
	publishStatus("NvmeHookVersion", xStringify(MODULE_VERSION));
	publishStatus("NvmeHookPatches", "discard suppression for the configured devices, anchor-resolved (M2)");
	publishStatus("NvmeHookTotal", "seen=0; blocked=0; dsm=0");

	// The list usually is not readable yet (the plugin's IOService has not been
	// instantiated when the kext starts); whoever reads it later publishes it.
	loadIdentities();
	publishConfig();

	reportBus();
	lilu.onPatcherLoad(patchersReady);

	SYSLOG("nvmehook", "%s %s loaded; %u identity(ies) configured", xStringify(PRODUCT_NAME),
		   xStringify(MODULE_VERSION), identityCount);
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
