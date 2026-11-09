//
//  NvmeHook.cpp
//  NvmeHook
//
//  Keeps NVMe TRIM away from devices whose firmware does not survive it.
//
//  Milestone M0: the plugin loads and reports the NVMe controllers on the bus.
//  Nothing is patched and no request is altered; interception lands in M1.
//
//  The report is published to the IORegistry, not only to the kernel log: the
//  log is a ring buffer that ordinary boot traffic overwrites within a minute,
//  while registry properties stay readable with `ioreg` for the whole boot.
//  Each controller found gets an `NvmeHookController<n>` property, and
//  `IOResources` carries the version and the outcome of the scan.
//
//  Everything runs in the plugin start callback, which is the context a Lilu
//  plugin already runs in. There is deliberately no custom IOService and no
//  IOKit matching notification: registering one from inside IOService::start()
//  hangs this machine during boot.
//

#include <Headers/kern_api.hpp>
#include <Headers/plugin_start.hpp>
#include <Headers/kern_util.hpp>

#include <IOKit/IOService.h>
#include <IOKit/IOKitKeys.h>
#include <libkern/c++/OSString.h>
#include <libkern/libkern.h>

static const char *bootargOff[] = { "-nvmehookoff" };
static const char *bootargDebug[] = { "-nvmehookdbg" };
static const char *bootargBeta[] = { "-nvmehookbeta" };

// Properties IONVMeFamily publishes on each controller. The device match is built
// from these, so M0 only reads them back to prove they are visible from a plugin.
static const char *keyModel = "Model Number";
static const char *keySerial = "Serial Number";

static const char *stringProperty(IOService *service, const char *key) {
	auto value = OSDynamicCast(OSString, service->getProperty(key));
	return value != nullptr ? value->getCStringNoCopy() : "<absent>";
}

static void publishStatus(const char *key, const char *value) {
	if (auto resources = IOService::getResourceService())
		resources->setProperty(key, value);
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

static void chefStart() {
	publishStatus("NvmeHookVersion", xStringify(MODULE_VERSION));
	publishStatus("NvmeHookPatches", "none (M0 reports the bus only)");
	reportBus();

	SYSLOG("nvmehook", "%s %s loaded; M0 reports the bus and patches nothing",
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
