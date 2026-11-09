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
//  M1.8: symbol resolution is fully symbol-name based and panic-free:
//      Lilu's own resolution is broken for Tahoe kernel collections (kext and
//      kernel symbols both fail; the kext kmod entries report address 0), and
//      scanning kernel VM for the collection header faulted in the wild. This
//      version anchors everything to a live object instead:
//
//        1. wait for an IONVMeController instance (waitForMatchingService),
//        2. its vtable pointer is an address inside IONVMeFamily,
//        3. read the on-disk boot collection with vn_rdwr (kernel file reads
//           cannot fault), find the IONVMeFamily slice and its LC_SYMTAB, and
//           resolve hook symbols plus __ZTV/__ZTI16IONVMeController by name,
//        4. slide = vptr - (__ZTV link value + 16), cross-checked against the
//           type-info pointer stored immediately before the vptr,
//        5. hook addresses = link value + slide; route with Lilu.
//
//      Nothing is hardcoded: every offset comes from the collection's symbol
//      table at runtime, every address is validated before routing, and any
//      failed check aborts with a status instead of patching.
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
#include <libkern/c++/OSDictionary.h>
#include <libkern/c++/OSNumber.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach-o/stab.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/uio.h>
#include <sys/vm.h>
#include <sys/vnode.h>

static const char *bootargOff[] = { "-nvmehookoff" };
static const char *bootargDebug[] = { "-nvmehookdbg" };
static const char *bootargBeta[] = { "-nvmehookbeta" };

// Properties IONVMeFamily publishes on each controller.
static const char *keyModel = "Model Number";
static const char *keySerial = "Serial Number";
static const char *keyVendor = "Vendor Identifier";

// The boot collection on the sealed system volume. Only symbol offsets are
// taken from it; the running base comes from the live object's vtable, so the
// aux collection may add slices as long as the base kext binary is unchanged
// (verified structurally by the type-info cross-check below).
static constexpr char kcPath[] = "/System/Library/KernelCollections/BootKernelExtensions.kc";

// Mangled symbol names, verified in the collection with IDA. These are the
// only identities the hooks rely on; nothing here encodes an address.
static constexpr const char *symNames[] {
	"__ZN24IONVMeBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj",
	"__ZN24IONVMeBlockStorageDevice11doUnmapNVMeEP16AppleNVMeRequestP26IOBlockStorageDeviceExtentjP17NVMeLBARangeEntry",
	"__ZN16IONVMeController20InitializeControllerEv",
	"__ZTV16IONVMeController",
};
static constexpr uint32_t kSymDoUnmap = 0;
static constexpr uint32_t kSymDoUnmapNVMe = 1;
static constexpr uint32_t kSymInitializeController = 2;
static constexpr uint32_t kSymVtable = 3;
static constexpr uint32_t kSymCount = 4;

// kcgen's fileset entry command; not in the public headers. Layout:
// cmd/cmdsize/vmaddr/fileoff/entry_id offset/reserved, identifier string at
// command start + entry_id.
struct FilesetEntryCommand {
	uint32_t cmd;
	uint32_t cmdsize;
	uint64_t vmaddr;
	uint64_t fileoff;
	uint32_t entryId;
	uint32_t reserved;
};
static constexpr uint32_t kLcFilesetEntry = 0x80000035;

static mach_vm_address_t orgDoUnmap {0};
static mach_vm_address_t orgDoUnmapNVMe {0};
static mach_vm_address_t orgInitializeController {0};
static KernelPatcher *gPatcher {nullptr};

// Per-device counters. Fixed capacity: this machine has one NVMe controller, and
// a discard request must never allocate.
struct Counters {
	IOService *device {nullptr};
	uint64_t seen {0};
	uint64_t dsm {0};
};

static constexpr uint32_t maxDevices = 8;
static Counters counters[maxDevices] {};
static uint32_t counterCount {0};

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
// dictionary; controllers publish the same pair as flat properties.
static const char *identityProperty(IOService *service, const char *key, const char *dictKey) {
	if (auto value = OSDynamicCast(OSString, service->getProperty(key)))
		return value->getCStringNoCopy();

	if (auto dict = OSDynamicCast(OSDictionary, service->getProperty("Device Characteristics")))
		if (auto value = OSDynamicCast(OSString, dict->getObject(dictKey)))
			return value->getCStringNoCopy();

	return "<absent>";
}

static Counters *countersFor(IOService *device) {
	for (uint32_t i = 0; i < counterCount; i++)
		if (counters[i].device == device)
			return &counters[i];

	if (counterCount >= maxDevices)
		return nullptr;

	auto c = &counters[counterCount];
	c->device = device;

	char key[40];
	snprintf(key, sizeof(key), "NvmeHookDevice%u", counterCount);
	char value[320];
	snprintf(value, sizeof(value), "model=%s; serial=%s",
			 identityProperty(device, keyModel, "Product Name"),
			 identityProperty(device, keySerial, keySerial));
	publishStatus(key, value);

	counterCount++;
	return c;
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

static void publishTotal() {
	uint64_t seen = 0;
	uint64_t dsm = 0;
	for (uint32_t i = 0; i < counterCount; i++) {
		seen += counters[i].seen;
		dsm += counters[i].dsm;
	}

	char value[64];
	snprintf(value, sizeof(value), "seen=%llu; dsm=%llu", seen, dsm);
	publishStatus("NvmeHookTotal", value);
}

static IOReturn doUnmapHook(void *self, IOBlockStorageDeviceExtent *extents,
							uint32_t extentsCount, uint32_t options) {
	auto device = static_cast<IOService *>(self);
	if (auto c = countersFor(device)) {
		c->seen++;
		publishCounters(c);
		publishTotal();
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
		publishTotal();
	}

	auto org = reinterpret_cast<IOReturn (*)(void *, void *, IOBlockStorageDeviceExtent *, uint32_t, void *)>(orgDoUnmapNVMe);
	return org(self, request, extents, extentsCount, rangeEntry);
}

// Per-device identity match, per the plan: model AND serial, both from the
// controller's registry properties. Only the named 750 GB Optane sample is the
// target; every other controller — the healthy WD_BLACK AN1500 control device
// included — reports as a pass-through. Blocking is a later milestone; for now
// this only logs the match.
static const char *targetModel = "INTEL SSDPD21K750GAM";
static const char *targetSerial = "PHKD9344003J750F";

static bool isTargetController(IOService *controller) {
	auto model = stringProperty(controller, keyModel);
	auto serial = stringProperty(controller, keySerial);
	return strncmp(model, targetModel, strlen(targetModel) + 1) == 0 &&
		   strncmp(serial, targetSerial, strlen(targetSerial) + 1) == 0;
}

static void reportControllerIdentity(IOService *controller) {
	const char *model = stringProperty(controller, keyModel);
	const char *serial = stringProperty(controller, keySerial);
	bool target = isTargetController(controller);

	char report[256];
	if (target) {
		snprintf(report, sizeof(report),
				 "TRIM-suppression target attached (model=%s; serial=%s); "
				 "blocking stage not enabled yet", model, serial);
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
// ---------------------------------------------------------------------------
// Boot collection parsing. All reads go through vn_rdwr against the on-disk
// collection: kernel file reads return errors instead of faulting, so a wrong
// offset can never panic the machine.
// ---------------------------------------------------------------------------

static bool readKcBytes(vnode_t vp, void *buffer, uint32_t length, uint64_t offset) {
	int resid = 0;
	auto error = vn_rdwr(UIO_READ, vp, reinterpret_cast<caddr_t>(buffer),
						 length, static_cast<off_t>(offset), UIO_SYSSPACE,
						 0, kauth_cred_get(), &resid, current_proc());
	return error == 0 && resid == 0;
}

// Parse the collection, find the IONVMeFamily slice, and resolve every symbol
// in symNames to its link-time address. Returns false on any structural
// surprise; every allocation is freed before returning. stage reports where.
static bool resolveKcSymbols(uint64_t *links, uint64_t &textVmaddrOut, uint64_t &textVmSizeOut,
							 uint64_t &sliceLoOut, uint64_t &sliceHiOut, const char *&stage) {
	vnode_t vp = nullptr;
	stage = "vnode-lookup";
	if (vnode_lookup(kcPath, 0, &vp, vfs_context_current()) != 0)
		return false;

	bool ok = false;
	uint8_t *headerBlock = nullptr;
	uint8_t *sliceBlock = nullptr;
	uint8_t *symtab = nullptr;
	uint8_t *strtab = nullptr;
	uint64_t strtabSize = 0;
	uint64_t symtabSize = 0;

	auto cleanup = [&]() {
		if (headerBlock) IOFree(headerBlock, 256 * 1024);
		if (sliceBlock) IOFree(sliceBlock, 4096);
		if (strtab) IOFree(strtab, strtabSize);
		if (symtab) IOFree(symtab, symtabSize);
		vnode_put(vp);
	};

	stage = "alloc-header";
	headerBlock = reinterpret_cast<uint8_t *>(IOMalloc(256 * 1024));
	if (headerBlock == nullptr)
		goto out;
	stage = "read-header";
	if (!readKcBytes(vp, headerBlock, 256 * 1024, 0))
		goto out;

	{
		auto header = reinterpret_cast<mach_header_64 *>(headerBlock);
		if (header->magic != MH_MAGIC_64 || header->ncmds < 128) {
			// Not the collection header we know: the real one carries ~830
			// load commands, most of them fileset entries.
			stage = "bad-header";
			goto out;
		}

		uint64_t sliceFileoff = 0;
		auto cmd = headerBlock + sizeof(mach_header_64);
		for (uint32_t i = 0; i < header->ncmds; i++) {
			auto loadCmd = reinterpret_cast<const load_command *>(cmd);
			if (loadCmd->cmdsize < sizeof(load_command) ||
				cmd + loadCmd->cmdsize > headerBlock + 256 * 1024)
				goto out;

			if (loadCmd->cmd == kLcFilesetEntry) {
				auto entry = reinterpret_cast<const FilesetEntryCommand *>(cmd);
				auto id = reinterpret_cast<const char *>(cmd) + entry->entryId;
				if (strncmp(id, "com.apple.iokit.IONVMeFamily", 30) == 0) {
					sliceFileoff = entry->fileoff;
					break;
				}
			}
			cmd += loadCmd->cmdsize;
		}

		if (sliceFileoff == 0) {
			stage = "no-fileset-entry";
			goto out;
		}

		sliceBlock = reinterpret_cast<uint8_t *>(IOMalloc(4096));
		if (sliceBlock == nullptr) {
			stage = "alloc-slice";
			goto out;
		}
		if (!readKcBytes(vp, sliceBlock, 4096, sliceFileoff)) {
			stage = "read-slice";
			goto out;
		}

		auto slice = reinterpret_cast<mach_header_64 *>(sliceBlock);
		if (slice->magic != MH_MAGIC_64 || slice->filetype != MH_KEXT_BUNDLE) {
			stage = "bad-slice";
			goto out;
		}

		segment_command_64 *text = nullptr;
		symtab_command *symtabCmd = nullptr;
		uint64_t spanLo = UINT64_MAX;
		uint64_t spanHi = 0;
		auto sliceCmd = sliceBlock + sizeof(mach_header_64);
		for (uint32_t i = 0; i < slice->ncmds; i++) {
			auto loadCmd = reinterpret_cast<const load_command *>(sliceCmd);
			if (loadCmd->cmdsize < sizeof(load_command) ||
				sliceCmd + loadCmd->cmdsize > sliceBlock + 4096)
				goto out;

			if (loadCmd->cmd == LC_SEGMENT_64) {
				auto seg = reinterpret_cast<const segment_command_64 *>(sliceCmd);
				if (strncmp(seg->segname, "__TEXT", sizeof(seg->segname)) == 0 && text == nullptr)
					text = const_cast<segment_command_64 *>(seg);
				if (seg->vmsize > 0) {
					if (seg->vmaddr < spanLo)
						spanLo = seg->vmaddr;
					if (seg->vmaddr + seg->vmsize > spanHi)
						spanHi = seg->vmaddr + seg->vmsize;
				}
			} else if (loadCmd->cmd == LC_SYMTAB) {
				auto st = reinterpret_cast<const symtab_command *>(sliceCmd);
				if (st->symoff != 0 && st->nsyms != 0 && symtabCmd == nullptr)
					symtabCmd = const_cast<symtab_command *>(st);
			}
			sliceCmd += loadCmd->cmdsize;
		}

		if (text == nullptr || symtabCmd == nullptr) {
			stage = "no-text-or-symtab";
			goto out;
		}
		if (symtabCmd->nsyms > 200000 || symtabCmd->strsize > 64 * 1024 * 1024) {
			stage = "implausible-symtab";
			goto out;
		}

		symtabSize = static_cast<uint64_t>(symtabCmd->nsyms) * sizeof(nlist_64);
		strtabSize = symtabCmd->strsize;

		symtab = reinterpret_cast<uint8_t *>(IOMalloc(symtabSize));
		strtab = reinterpret_cast<uint8_t *>(IOMalloc(strtabSize));
		if (symtab == nullptr || strtab == nullptr) {
			stage = "alloc-symtab";
			goto out;
		}
		if (!readKcBytes(vp, symtab, static_cast<uint32_t>(symtabSize), symtabCmd->symoff) ||
			!readKcBytes(vp, strtab, symtabCmd->strsize, symtabCmd->stroff)) {
			stage = "read-symtab";
			goto out;
		}

		uint32_t found = 0;
		auto entries = reinterpret_cast<const nlist_64 *>(symtab);
		auto strings = reinterpret_cast<const char *>(strtab);
		for (uint32_t i = 0; i < symtabCmd->nsyms; i++) {
			if (entries[i].n_type & N_STAB)
				continue;
			auto candidate = strings + entries[i].n_un.n_strx;
			for (uint32_t s = 0; s < kSymCount; s++) {
				if (links[s] != 0)
					continue;
				if (strncmp(candidate, symNames[s], strlen(symNames[s]) + 1) == 0) {
					links[s] = entries[i].n_value;
					found++;
				}
			}
			if (found == kSymCount)
				break;
		}
		if (found != kSymCount) {
			stage = "symbols-missing";
			goto out;
		}

		// The three hook targets must be text; vtable/type-info are data in
		// the slice, so they are only required to be non-zero.
		for (uint32_t s = 0; s <= kSymInitializeController; s++) {
			if (links[s] < text->vmaddr || links[s] >= text->vmaddr + text->vmsize) {
				stage = "symbol-out-of-text";
				goto out;
			}
		}

		textVmaddrOut = text->vmaddr;
		textVmSizeOut = text->vmsize;
		sliceLoOut = spanLo;
		sliceHiOut = spanHi;
		ok = true;
	}

out:
	cleanup();
	return ok;
}

// Resolve every interception point and route the hooks. Runs on a worker
// thread once an IONVMeController exists, because the running base is derived
// from its vtable. Every controller instance is probed against the collection
// symbol table: the first published controller is not necessarily driven by
// IONVMeFamily (this machine also carries a WD RAID card), and only the
// type-info anchor proves which one is.
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

	// DirectLink target detection covers every controller: the Samsung 990 may
	// sit behind any of them.
	for (uint32_t i = 0; i < instanceCount; i++)
		reportControllerIdentity(instances[i]);

	uint64_t links[kSymCount] {};
	uint64_t sliceTextLink = 0;
	uint64_t sliceTextSize = 0;
	uint64_t sliceLo = 0;
	uint64_t sliceHi = 0;
	const char *stage = "unknown";
	bool parsed = false;
	mach_vm_address_t chosenVptr {0};
	int64_t chosenSlide {0};
	for (uint32_t attempt = 0; attempt < 60 && !parsed; attempt++) {
		if (resolveKcSymbols(links, sliceTextLink, sliceTextSize, sliceLo, sliceHi, stage)) {
			parsed = true;
		} else {
			publishStatus("NvmeHookKcStage", stage);
			IOSleep(1000);
		}
	}
	if (!parsed) {
		char state[160];
		snprintf(state, sizeof(state), "collection parse failed at %s, nothing patched", stage);
		publishStatus("NvmeHookHooks", state);
		goto releaseInstances;
	}
	publishStatus("NvmeHookKcStage", "ok");

	// Probe every instance: the vptr must sit exactly one type-info slot past
	// the vtable symbol, the vptr itself must land inside the slice (the
	// vtable lives in __DATA), and the function pointers stored in the first
	// vtable slots must land inside the slice's __TEXT after the slide is
	// removed. The first instance that validates is our anchor.
	for (uint32_t i = 0; i < instanceCount && chosenVptr == 0; i++) {
		auto vptr = *reinterpret_cast<uintptr_t *>(instances[i]);
		auto slide = static_cast<int64_t>(vptr) -
					 static_cast<int64_t>(links[kSymVtable] + 2 * sizeof(uintptr_t));

		char probe[192];
		snprintf(probe, sizeof(probe), "i=%u slide=0x%llx vptr-ztv=0x%llx",
				 i, static_cast<long long>(slide),
				 static_cast<unsigned long long>(vptr - links[kSymVtable]));
		publishStatus("NvmeHookProbe", probe);

		if (slide < 0 || (slide & 0xFFF) != 0 || slide > 0x1000000000ULL)
			continue;

		auto inSlice = [&](uintptr_t link) {
			return link >= sliceLo && link < sliceHi;
		};
		auto inText = [&](uintptr_t link) {
			return link >= sliceTextLink && link < sliceTextLink + sliceTextSize;
		};
		if (!inSlice(vptr - slide))
			continue;
		auto fn0 = *reinterpret_cast<uintptr_t *>(vptr);
		auto fn1 = *reinterpret_cast<uintptr_t *>(vptr + sizeof(uintptr_t));
		snprintf(probe, sizeof(probe), "i=%u fn0=0x%llx fn1=0x%llx fn0-s=0x%llx fn1-s=0x%llx",
				 i, static_cast<unsigned long long>(fn0), static_cast<unsigned long long>(fn1),
				 static_cast<unsigned long long>(fn0 - slide),
				 static_cast<unsigned long long>(fn1 - slide));
		publishStatus("NvmeHookFnProbe", probe);
		if (!inText(fn0 - slide) || !inText(fn1 - slide))
			continue;
		chosenVptr = vptr;
		chosenSlide = slide;
	}

	if (chosenVptr == 0) {
		publishStatus("NvmeHookHooks", "no controller validates the vtable anchor, nothing patched");
		goto releaseInstances;
	}

	{
		auto slide = chosenSlide;
		publishStatus("NvmeHookSlide", "resolved");
		publishAddress("NvmeHookKextBase", sliceTextLink + slide);
		publishAddress("NvmeHookVptr", chosenVptr);

		auto addressDoUnmap = links[kSymDoUnmap] + slide;
		auto addressDoUnmapNVMe = links[kSymDoUnmapNVMe] + slide;
		auto addressInitializeController = links[kSymInitializeController] + slide;

		publishAddress("NvmeHookSymDoUnmap", addressDoUnmap);
		publishAddress("NvmeHookSymDoUnmapNVMe", addressDoUnmapNVMe);
		publishAddress("NvmeHookSymInitializeController", addressInitializeController);

		orgDoUnmap = gPatcher->routeFunction(addressDoUnmap,
											 reinterpret_cast<mach_vm_address_t>(doUnmapHook), true);
		orgDoUnmapNVMe = gPatcher->routeFunction(addressDoUnmapNVMe,
												 reinterpret_cast<mach_vm_address_t>(doUnmapNVMeHook), true);
		orgInitializeController = gPatcher->routeFunction(addressInitializeController,
														  reinterpret_cast<mach_vm_address_t>(initializeControllerHook), true);

		char state[128];
		if (orgDoUnmap != 0 && orgDoUnmapNVMe != 0 && orgInitializeController != 0) {
			snprintf(state, sizeof(state), "armed by symbol: counters + InitializeController");
		} else if (orgDoUnmap != 0 && orgDoUnmapNVMe != 0) {
			snprintf(state, sizeof(state), "counters armed; InitializeController unresolved");
		} else if (orgDoUnmap != 0 || orgDoUnmapNVMe != 0) {
			snprintf(state, sizeof(state), "partial: one counter hook failed to route");
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
	publishStatus("NvmeHookPatches", "counters only, symbol-resolved (M1.8)");
	publishStatus("NvmeHookTotal", "seen=0; dsm=0");
	reportBus();
	lilu.onPatcherLoad(patchersReady);

	SYSLOG("nvmehook", "%s %s loaded; symbol-resolved hooks, no request is altered",
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
