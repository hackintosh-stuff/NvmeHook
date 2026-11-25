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
//      slice's own Mach-O header sits a few hundred kilobytes below it.
//  M1.9: an earlier version read BootKernelExtensions.kc through
//      vnode_lookup/vn_rdwr from its own kernel thread. That path panicked in
//      namei ("Taking non-sleepable RW lock with preemption enabled") when boot
//      timing moved, because a bare kernel thread has no VFS context. The driver
//      no longer touches the file system, and never will.
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
//  M3: the interception points come from the image's own symbol table. A Tahoe
//      IONVMeFamily slice keeps LC_SYMTAB and the three targets are external
//      symbols in it, so they are looked up by exact mangled name, with the
//      slice base recovered from the same live object. Nothing build-specific is
//      compiled in, which is what makes an IONVMeFamily update survivable, and
//      there is no second path on measured offsets: such a table describes one
//      build, cannot tell a rebuilt slice from a misresolution, and a driver that
//      keeps patching on it hides the defect instead of reporting it.
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
#include <libkern/c++/OSBoolean.h>
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

// Diagnostic properties are opt-in, because every one of them is a registry
// write and most of them only matter while the driver is being developed:
//
//   <key>NvmeHookDebug</key> <false/>
//
// Absent means false. What is always published is the minimum needed to know
// what the driver is doing: NvmeHookVersion, NvmeHookPatches, NvmeHookConfig,
// NvmeHookHooks and the counters in NvmeHookTotal.
static constexpr char kDebugKey[] = "NvmeHookDebug";

struct DeviceIdentity {
	char model[kModelLength];
	char serial[kSerialLength];
};

static DeviceIdentity identities[kMaxIdentities] {};
static uint32_t identityCount {0};
static bool configLoaded {false};
static bool debugOutput {false};
static const char *configState = "not read yet";
static const char *workerState = "not started";

static void reportBus();

// Properties IONVMeFamily publishes on each controller.
static const char *keyModel = "Model Number";
static const char *keySerial = "Serial Number";
static const char *keyVendor = "Vendor Identifier";

// ---------------------------------------------------------------------------
// Resolution: the slice's own symbol table.
//
// An IONVMeFamily slice in a kernel collection carries LC_SYMTAB, and the three
// functions are external symbols (N_SECT|N_EXT) in it, so they can be found by
// name. What is missing on Tahoe is a way to get the slice's base, not the table:
// Lilu's solveSymbol resolves through kmod_info->address, which collection-built
// kexts report as zero. The base is recovered from the live object that anchors
// everything else: its vptr points into this slice, so page-aligning down from it
// lands on the Mach-O header within a few hundred kilobytes.
//
// This collection rebases the load commands and the symbol values together, so
// __TEXT.vmaddr already reads as a runtime address and the slide computes to
// zero. Zero and non-zero are both accepted; what decides whether a candidate
// header is real is that the anchor object lies inside the image it declares.
//
// Only memory that is mapped by construction is read: the pages between the
// header and the vptr are the slice's own segments, laid out contiguously. A page
// that starts with the magic but does not parse is skipped, with a bound, because
// a constant inside __TEXT can look like a header.
//
// There is deliberately no fallback to measured offsets and code fingerprints.
// Such a table only ever describes one build of the kext, it cannot tell a
// rebuilt slice from a misresolution, and a driver that keeps patching on it
// hides the resolution defect instead of reporting it. If the names are not
// there, nothing is patched and the registry says why.
// ---------------------------------------------------------------------------
static constexpr uint32_t kMachMagic64 = 0xFEEDFACF;
static constexpr uint32_t kCpuTypeX86_64 = 0x01000007;
static constexpr uint32_t kFileTypeKextBundle = 0xB;
static constexpr uint32_t kLoadCmdSegment64 = 0x19;
static constexpr uint32_t kLoadCmdSymtab = 0x2;
static constexpr uint32_t kMaxSliceSegments = 8;
static constexpr uint32_t kMaxSliceScanPages = 256;
static constexpr uint32_t kMaxLoadCommands = 32;
static constexpr uint32_t kMaxLoadCommandsSize = 0x4000;
static constexpr uint32_t kMaxSymbols = 200000;

static const char *symDoUnmap =
	"__ZN24IONVMeBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj";
static const char *symDoUnmapNVMe =
	"__ZN24IONVMeBlockStorageDevice11doUnmapNVMeEP16AppleNVMeRequestP26IOBlockStorageDeviceExtentjP17NVMeLBARangeEntry";
static const char *symInitializeController = "__ZN16IONVMeController20InitializeControllerEv";
static const char *symControllerVtable = "__ZTV16IONVMeController";

static inline uint32_t readU32(const uint8_t *p) {
	uint32_t value;
	memcpy(&value, p, sizeof(value));
	return value;
}

static inline uint64_t readU64(const uint8_t *p) {
	uint64_t value;
	memcpy(&value, p, sizeof(value));
	return value;
}

// Bounded compare: the string table is a shared blob and the last string in it
// is not guaranteed to be terminated inside the declared size.
static bool nameEquals(const char *actual, const char *wanted, uint32_t limit) {
	for (uint32_t i = 0; i < limit; i++) {
		if (actual[i] != wanted[i])
			return false;
		if (wanted[i] == '\0')
			return true;
	}
	return false;
}

struct SliceSegment {
	char name[17];
	uint64_t vmaddr {0};
	uint64_t vmsize {0};
	uint64_t fileoff {0};
	uint64_t filesize {0};
};

struct Slice {
	mach_vm_address_t base {0};
	int64_t slide {0};
	SliceSegment segments[kMaxSliceSegments] {};
	uint32_t segmentCount {0};
	uint32_t symoff {0};
	uint32_t symbolCount {0};
	uint32_t stroff {0};
	uint32_t strsize {0};
	bool hasSymtab {false};
};

// Why the symbol path gave up, for the registry report. One string per boot;
// without it a fallback to the constants is indistinguishable from a driver
// that never tried.
static const char *sliceReason = "not attempted";

// Per-page record of the header scan: which page carried the magic and why it
// was rejected. Diagnostic only, published once when the debug switch is on.
static char scanTrace[256];
static uint32_t scanTraceLength {0};

static void traceReject(mach_vm_address_t page, const char *reason) {
	if (scanTraceLength >= sizeof(scanTrace))
		return;
	char entry[80];
	snprintf(entry, sizeof(entry), "0x%llx:%s; ", static_cast<unsigned long long>(page), reason);
	size_t length = strlen(entry);
	if (scanTraceLength + length >= sizeof(scanTrace))
		return;
	memcpy(scanTrace + scanTraceLength, entry, length + 1);
	scanTraceLength += static_cast<uint32_t>(length);
}

static bool parseSlice(mach_vm_address_t header, mach_vm_address_t containing, Slice *slice) {
	auto bytes = reinterpret_cast<const uint8_t *>(header);
	uint32_t filetype = readU32(bytes + 12);
	uint32_t ncmds = readU32(bytes + 16);
	uint32_t sizeofcmds = readU32(bytes + 20);

	if (readU32(bytes) != kMachMagic64 || readU32(bytes + 4) != kCpuTypeX86_64 ||
		filetype != kFileTypeKextBundle) {
		sliceReason = "magic without a kext header";
		return false;
	}
	if (ncmds == 0 || ncmds > kMaxLoadCommands || sizeofcmds < 8 ||
		sizeofcmds > kMaxLoadCommandsSize) {
		sliceReason = "load command header out of range";
		return false;
	}

	Slice parsed {};
	parsed.base = header;

	uint32_t offset = 32; // sizeof(mach_header_64)
	uint32_t limit = 32 + sizeofcmds;
	for (uint32_t i = 0; i < ncmds; i++) {
		if (offset + 8 > limit) {
			sliceReason = "load commands overrun their size";
			return false;
		}
		uint32_t cmd = readU32(bytes + offset);
		uint32_t size = readU32(bytes + offset + 4);
		if (size < 8 || offset + size > limit) {
			sliceReason = "malformed load command";
			return false;
		}

		if (cmd == kLoadCmdSegment64) {
			if (size < 72 || parsed.segmentCount >= kMaxSliceSegments) {
				sliceReason = "unusable segment command";
				return false;
			}
			auto &segment = parsed.segments[parsed.segmentCount++];
			memcpy(segment.name, bytes + offset + 8, 16);
			segment.name[16] = '\0';
			segment.vmaddr = readU64(bytes + offset + 24);
			segment.vmsize = readU64(bytes + offset + 32);
			segment.fileoff = readU64(bytes + offset + 40);
			segment.filesize = readU64(bytes + offset + 48);
		} else if (cmd == kLoadCmdSymtab) {
			if (size < 24) {
				sliceReason = "truncated symbol table command";
				return false;
			}
			parsed.symoff = readU32(bytes + offset + 8);
			parsed.symbolCount = readU32(bytes + offset + 12);
			parsed.stroff = readU32(bytes + offset + 16);
			parsed.strsize = readU32(bytes + offset + 20);
			parsed.hasSymtab = true;
		}

		offset += size;
	}

	if (parsed.segmentCount == 0 || strcmp(parsed.segments[0].name, "__TEXT") != 0) {
		sliceReason = "first segment is not __TEXT";
		return false;
	}
	if (!parsed.hasSymtab) {
		sliceReason = "slice has no LC_SYMTAB";
		return false;
	}
	if (parsed.symbolCount == 0 || parsed.symbolCount > kMaxSymbols || parsed.strsize == 0) {
		sliceReason = "empty symbol table";
		return false;
	}

	// The header sits at the start of __TEXT, so the difference is the slide the
	// image was loaded at. Zero is allowed: a collection that has already rebased
	// the load commands reports its runtime address as the link address, and the
	// symbol values are rebased with it, so the same arithmetic holds.
	int64_t slide = static_cast<int64_t>(header) - static_cast<int64_t>(parsed.segments[0].vmaddr);
	if (slide < -0x40000000 || slide > 0x40000000 || (slide & 0xFFF) != 0) {
		sliceReason = "implausible slide";
		return false;
	}

	// The object we started from has to be inside this image under that slide.
	// This is what rejects a page that merely looks like a header.
	bool inside = false;
	for (uint32_t i = 0; i < parsed.segmentCount && !inside; i++) {
		const auto &segment = parsed.segments[i];
		auto start = static_cast<mach_vm_address_t>(
			slide + static_cast<int64_t>(segment.vmaddr));
		inside = containing >= start && containing < start + segment.vmsize;
	}
	if (!inside) {
		sliceReason = "anchor is not inside this image";
		return false;
	}

	parsed.slide = slide;

	*slice = parsed;
	sliceReason = "ok";
	return true;
}

// Scan down from a live pointer inside the slice until its Mach-O header shows
// up. Every page in between belongs to the same image, whose segments are
// contiguous, so nothing outside the mapping is read. A page that starts with
// the magic but does not parse is skipped, not fatal: a constant in __TEXT can
// look like a header, and the real one is always further down.
static bool sliceForAddress(mach_vm_address_t address, Slice *slice) {
	if ((address >> 48) != 0xffff) {
		sliceReason = "vptr is not a kernel address";
		return false;
	}

	scanTraceLength = 0;
	scanTrace[0] = '\0';

	uint32_t rejected = 0;
	mach_vm_address_t page = address & ~static_cast<mach_vm_address_t>(0xFFF);
	for (uint32_t i = 0; i < kMaxSliceScanPages; i++, page -= 0x1000) {
		if (page < 0x100000) {
			sliceReason = "scan left the address space";
			return false;
		}
		if (readU32(reinterpret_cast<const uint8_t *>(page)) != kMachMagic64)
			continue;
		if (parseSlice(page, address, slice)) {
			traceReject(page, "accepted");
			return true;
		}
		traceReject(page, sliceReason);
		// Bounded: after a few rejects the pages below are no longer known to be
		// part of this image, so the scan stops rather than reading them.
		if (++rejected >= 4)
			return false;
	}

	if (rejected == 0)
		sliceReason = "no Mach-O header below the vptr";
	return false;
}

// A file offset from the load commands is valid only inside the segment that
// owns it; the slices of a collection do not share one flat file image.
static bool runtimeFromFileOffset(const Slice &slice, uint32_t fileoff, uint64_t length,
								  mach_vm_address_t *address) {
	for (uint32_t i = 0; i < slice.segmentCount; i++) {
		const auto &segment = slice.segments[i];
		if (segment.filesize == 0)
			continue;
		if (fileoff < segment.fileoff || fileoff + length > segment.fileoff + segment.filesize)
			continue;
		*address = static_cast<mach_vm_address_t>(
			slice.slide + static_cast<int64_t>(segment.vmaddr) +
			static_cast<int64_t>(fileoff - segment.fileoff));
		return true;
	}
	return false;
}

// Exact match on the mangled name. Substring matching would also accept the
// .cold.N clones (which are function fragments, not entry points) and the same
// method of a sibling class, and patching either of those is a panic.
static mach_vm_address_t lookupSymbol(const Slice &slice, const char *wanted) {
	if (!slice.hasSymtab)
		return 0;

	mach_vm_address_t symtab = 0;
	mach_vm_address_t strtab = 0;
	if (!runtimeFromFileOffset(slice, slice.symoff,
							   static_cast<uint64_t>(slice.symbolCount) * 16, &symtab))
		return 0;
	if (!runtimeFromFileOffset(slice, slice.stroff, slice.strsize, &strtab))
		return 0;

	auto strings = reinterpret_cast<const char *>(strtab);
	auto entry = reinterpret_cast<const uint8_t *>(symtab);
	for (uint32_t i = 0; i < slice.symbolCount; i++, entry += 16) {
		uint32_t nameIndex = readU32(entry);
		if (nameIndex == 0 || nameIndex >= slice.strsize)
			continue;
		if ((entry[4] & 0x0e) != 0x0e) // N_SECT only: no undefined, no absolute
			continue;
		if (!nameEquals(strings + nameIndex, wanted, slice.strsize - nameIndex))
			continue;

		auto value = static_cast<mach_vm_address_t>(
			static_cast<int64_t>(readU64(entry + 8)) + slice.slide);

		// Whatever the table says, the result has to be inside this image.
		for (uint32_t s = 0; s < slice.segmentCount; s++) {
			const auto &segment = slice.segments[s];
			auto start = static_cast<mach_vm_address_t>(
				slice.slide + static_cast<int64_t>(segment.vmaddr));
			if (value >= start && value < start + segment.vmsize)
				return value;
		}
		return 0;
	}

	return 0;
}

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

// Diagnostics go through these: nothing is written unless NvmeHookDebug is set.
static void publishDebug(const char *key, const char *value) {
	if (debugOutput)
		publishStatus(key, value);
}

static void publishDebugAddress(const char *key, mach_vm_address_t address) {
	if (debugOutput)
		publishAddress(key, address);
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
	snprintf(report, sizeof(report), "state=%s; debug=%s; identities=%u", configState,
			 debugOutput ? "on" : "off", identityCount);
	for (uint32_t i = 0; i < identityCount; i++) {
		char entry[128];
		snprintf(entry, sizeof(entry), "; [%u] model=%s serial=%s",
				 i, identities[i].model, identities[i].serial);
		appendTo(report, sizeof(report), entry);
	}
	publishStatus("NvmeHookConfig", report);
}

// Read the device list and the diagnostics switch from the plugin's own
// personality once. The instance may not exist yet when the first discard
// arrives, in which case nothing is decided and the read is retried on the next
// event: the rules are never guessed.
static void loadConfiguration() {
	if (configLoaded)
		return;

	auto instance = ADDPR(selfInstance);
	if (instance == nullptr) {
		configState = "plugin service not started";
		return;
	}

	configLoaded = true;
	if (auto flag = OSDynamicCast(OSBoolean, instance->getProperty(kDebugKey)))
		debugOutput = flag->getValue();

	auto array = OSDynamicCast(OSArray, instance->getProperty(kDevicesKey));
	if (array == nullptr) {
		configState = "NvmeHookDevices missing from Info.plist";
	} else {
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
	}

	publishConfig();
	SYSLOG("nvmehook", "config: %s, %u identity(ies), debug %s", configState, identityCount,
		   debugOutput ? "on" : "off");

	// The diagnostics that were produced before the switch was known are
	// published now, once, if it is on.
	if (debugOutput) {
		publishStatus("NvmeHookWorker", workerState);
		reportBus();
	}
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
	loadConfiguration();
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
	publishDebug(key, value);
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
	if (debugOutput)
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
	loadConfiguration();
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

	if (debugOutput)
		controller->setProperty("NvmeHookTarget", report);
	publishDebug("NvmeHookTarget", report);
	SYSLOG("nvmehook", "identity: %s", report);
}

static IOReturn initializeControllerHook(void *self) {
	auto org = reinterpret_cast<IOReturn (*)(void *)>(orgInitializeController);
	IOReturn result = org(self);
	reportControllerIdentity(static_cast<IOService *>(self));
	return result;
}
// Resolve every interception point and route the hooks. Runs on a worker
// thread once an IONVMeController exists, because the slice is found from that
// live object's vtable pointer. Instances are probed in turn: the first
// published controller is not necessarily driven by IONVMeFamily (this machine
// has also carried a RAID card), and a subclass would carry a different vtable,
// so the symbol table decides which instance is the anchor, not the enumeration
// order.
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

	// The plugin's own service exists by now in practice, so this is where the
	// diagnostics switch usually becomes known; if it is on, the bus report that
	// was skipped at load time is published from here.
	loadConfiguration();

	for (uint32_t i = 0; i < instanceCount; i++) {
		char key[32];
		snprintf(key, sizeof(key), "NvmeHookVptr%u", i);
		publishDebugAddress(key, *reinterpret_cast<uintptr_t *>(instances[i]));
	}
	char count[16];
	snprintf(count, sizeof(count), "%u", instanceCount);
	publishDebug("NvmeHookInstanceCount", count);

	// The identity report covers every controller: the matched device may sit
	// behind any of them, and the registry publishes model and serial per
	// controller, not per family.
	for (uint32_t i = 0; i < instanceCount; i++)
		reportControllerIdentity(instances[i]);

	mach_vm_address_t anchor {0};
	mach_vm_address_t anchorSlice {0};
	uint32_t anchorSymbols {0};
	int64_t anchorSlide {0};
	mach_vm_address_t addressDoUnmap {0};
	mach_vm_address_t addressDoUnmapNVMe {0};
	mach_vm_address_t addressInitializeController {0};

	// The slice's own symbol table is the only source. Nothing here depends on a
	// build constant, so an IONVMeFamily update that keeps these symbols keeps the
	// driver armed without touching the source. The instance vptr is used only to
	// locate the slice.
	char symbolProbe[320];
	symbolProbe[0] = '\0';
	for (uint32_t i = 0; i < instanceCount && anchor == 0; i++) {
		auto vptr = *reinterpret_cast<uintptr_t *>(instances[i]);
		Slice slice {};
		if (!sliceForAddress(vptr, &slice))
			continue;

		auto resolvedDoUnmap = lookupSymbol(slice, symDoUnmap);
		auto resolvedDoUnmapNVMe = lookupSymbol(slice, symDoUnmapNVMe);
		auto resolvedInitializeController = lookupSymbol(slice, symInitializeController);
		auto resolvedVtable = lookupSymbol(slice, symControllerVtable);

		snprintf(symbolProbe, sizeof(symbolProbe),
				 "i=%u vptr=0x%llx slice=0x%llx slide=0x%llx syms=%u doUnmap=0x%llx doUnmapNVMe=0x%llx init=0x%llx ztv=0x%llx",
				 i, static_cast<unsigned long long>(vptr),
				 static_cast<unsigned long long>(slice.base),
				 static_cast<unsigned long long>(slice.slide), slice.symbolCount,
				 static_cast<unsigned long long>(resolvedDoUnmap),
				 static_cast<unsigned long long>(resolvedDoUnmapNVMe),
				 static_cast<unsigned long long>(resolvedInitializeController),
				 static_cast<unsigned long long>(resolvedVtable));
		publishDebug("NvmeHookSymbols", symbolProbe);

		if (resolvedDoUnmap == 0 || resolvedDoUnmapNVMe == 0 ||
			resolvedInitializeController == 0 || resolvedVtable == 0) {
			sliceReason = "symbol table parsed but the names are missing";
			continue;
		}

		// The vtable symbol points at offset-to-top, so the vptr is two words in.
		anchor = resolvedVtable + 2 * sizeof(uintptr_t);
		anchorSlice = slice.base;
		anchorSymbols = slice.symbolCount;
		anchorSlide = slice.slide;
		addressDoUnmap = resolvedDoUnmap;
		addressDoUnmapNVMe = resolvedDoUnmapNVMe;
		addressInitializeController = resolvedInitializeController;
	}

	if (anchor == 0 && scanTrace[0] != '\0')
		publishDebug("NvmeHookScan", scanTrace);

	if (anchor == 0) {
		char report[256];
		snprintf(report, sizeof(report),
				 "symbol resolution failed across %u controller(s): %s; nothing patched",
				 instanceCount, sliceReason);
		publishStatus("NvmeHookResolve", report);
		publishStatus("NvmeHookHooks", "no interception point resolved, nothing patched");
		goto releaseInstances;
	}

	{
		// Published even when resolution succeeded, so the reason a slice had to be
		// skipped on another instance is not lost.
		char report[256];
		snprintf(report, sizeof(report), "resolved by symbols across %u controller(s); symbol path: %s",
				 instanceCount, sliceReason);
		publishStatus("NvmeHookResolve", report);
		if (symbolProbe[0] != '\0')
			publishDebug("NvmeHookSymbolsResult", symbolProbe);

		publishDebugAddress("NvmeHookVptr", anchor);
		publishDebugAddress("NvmeHookSymDoUnmap", addressDoUnmap);
		publishDebugAddress("NvmeHookSymDoUnmapNVMe", addressDoUnmapNVMe);
		publishDebugAddress("NvmeHookSymInitializeController", addressInitializeController);

		orgDoUnmap = gPatcher->routeFunction(addressDoUnmap,
											 reinterpret_cast<mach_vm_address_t>(doUnmapHook), true);
		orgDoUnmapNVMe = gPatcher->routeFunction(addressDoUnmapNVMe,
												 reinterpret_cast<mach_vm_address_t>(doUnmapNVMeHook), true);
		orgInitializeController = gPatcher->routeFunction(addressInitializeController,
														  reinterpret_cast<mach_vm_address_t>(initializeControllerHook), true);

		// The distances are published for the record, not compared against anything:
		// with no stored baseline in the binary there is nothing to drift from, and
		// whoever wants to check them against a hand-measured build has the numbers
		// in the registry and the slice they came from beside them.
		int64_t deltaDoUnmap = static_cast<int64_t>(addressDoUnmap) - static_cast<int64_t>(anchor);
		int64_t deltaDoUnmapNVMe = static_cast<int64_t>(addressDoUnmapNVMe) - static_cast<int64_t>(anchor);
		int64_t deltaInitializeController = static_cast<int64_t>(addressInitializeController) -
										   static_cast<int64_t>(anchor);

		char layout[256];
		snprintf(layout, sizeof(layout),
				 "source=symbols; doUnmap=%s%llx; doUnmapNVMe=%s%llx; init=%s%llx; slice=0x%llx; slide=0x%llx; syms=%u",
				 deltaDoUnmap < 0 ? "-0x" : "0x",
				 static_cast<unsigned long long>(deltaDoUnmap < 0 ? -deltaDoUnmap : deltaDoUnmap),
				 deltaDoUnmapNVMe < 0 ? "-0x" : "0x",
				 static_cast<unsigned long long>(deltaDoUnmapNVMe < 0 ? -deltaDoUnmapNVMe : deltaDoUnmapNVMe),
				 deltaInitializeController < 0 ? "-0x" : "0x",
				 static_cast<unsigned long long>(deltaInitializeController < 0 ? -deltaInitializeController
																		  : deltaInitializeController),
				 static_cast<unsigned long long>(anchorSlice),
				 static_cast<unsigned long long>(anchorSlide), anchorSymbols);
		publishStatus("NvmeHookLayout", layout);

		// doUnmap is the hook that can drop a request, so it is named first and
		// called out separately: if it failed to route while the counter hook
		// succeeded, the driver is reporting traffic it cannot suppress.
		char state[224];
		if (orgDoUnmap != 0 && orgDoUnmapNVMe != 0 && orgInitializeController != 0) {
			snprintf(state, sizeof(state), "armed: doUnmap + counters + InitializeController");
		} else if (orgDoUnmapNVMe != 0 && orgInitializeController != 0) {
			snprintf(state, sizeof(state), "counters armed but doUnmap unresolved, nothing is blocked");
		} else if (orgDoUnmapNVMe != 0) {
			snprintf(state, sizeof(state), "counters armed; doUnmap and InitializeController unresolved");
		} else {
			snprintf(state, sizeof(state), "routing failed, nothing patched");
		}

		publishStatus("NvmeHookHooks", state);
		SYSLOG("nvmehook", "hooks: %s; layout: %s", state, layout);
	}

releaseInstances:
	for (uint32_t i = 0; i < instanceCount; i++)
		instances[i]->release();
}

static void reportBus() {
	uint32_t index = 0;

	auto matching = IOService::serviceMatching("IONVMeController");
	if (matching == nullptr) {
		publishDebug("NvmeHookBus", "cannot build a matching dictionary");
		return;
	}

	if (auto iterator = IOService::getMatchingServices(matching)) {
		while (auto service = OSDynamicCast(IOService, iterator->getNextObject())) {
			char key[40];
			snprintf(key, sizeof(key), "NvmeHookController%u", index);

			char report[256];
			snprintf(report, sizeof(report), "model=%s; serial=%s",
					 stringProperty(service, keyModel), stringProperty(service, keySerial));

			if (debugOutput)
				service->setProperty(key, report);
			SYSLOG("nvmehook", "controller %u: %s", index, report);
			index++;
		}
		iterator->release();
	}

	matching->release();

	char status[96];
	snprintf(status, sizeof(status), "%u controller(s) visible at plugin start", index);
	publishDebug("NvmeHookBus", status);
}

// Lilu hands its patcher over here once kernel patching is possible; from this
// point a worker thread does the deferred, panic-free resolution.
static void patchersReady(void *, KernelPatcher &patcher) {
	gPatcher = &patcher;

	thread_t thread = nullptr;
	if (kernel_thread_start(reinterpret_cast<thread_continue_t>(workerThread), nullptr, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		workerState = "started";
	} else {
		workerState = "failed to start";
	}
	publishDebug("NvmeHookWorker", workerState);
}

static void chefStart() {
	publishStatus("NvmeHookVersion", xStringify(MODULE_VERSION));
	publishStatus("NvmeHookPatches", "discard suppression for the configured devices, symbol-resolved (M3)");
	publishStatus("NvmeHookTotal", "seen=0; blocked=0; dsm=0");

	// The rules usually are not readable yet (the plugin's IOService has not been
	// instantiated when the kext starts); whoever reads them later publishes the
	// configuration, and the diagnostics if NvmeHookDebug asks for them.
	loadConfiguration();
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
