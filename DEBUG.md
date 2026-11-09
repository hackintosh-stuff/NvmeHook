# NvmeHook — implementation and debugging

## Shape of the driver

A Lilu plugin with no IOKit personality of its own beyond the shell Lilu
supplies. Three parts:

1. `chefStart` (`PluginConfiguration::pluginStart` in `NvmeHook/NvmeHook.cpp`)
   publishes the version, tries to read the rule list from the plugin's own
   personality, scans the bus for controllers, and registers
   `onPatcherLoad`.
2. `patchersReady` gets Lilu's `KernelPatcher` and starts a kernel thread.
3. `workerThread` waits for an `IONVMeController`, derives the three hook
   addresses from that live object's vtable, and routes them:

| Hooked symbol | Role |
|---|---|
| `IONVMeBlockStorageDevice::doUnmap` | the suppression point: `seen`, `blocked` |
| `IONVMeBlockStorageDevice::doUnmapNVMe` | ground truth: `dsm`, the DSM command that leaves |
| `IONVMeController::InitializeController` | per-controller identity report |

### Why the report is not in the kernel log

`IOLog`/`SYSLOG` output lands in the kernel ring buffer. On this machine ordinary
boot traffic (sandbox reports in particular) overwrites the early buffer within
about 40 seconds, so a banner printed during kext load is gone by the time a
session can be opened. It also never reaches the unified log store for these
early messages, which is why `log show --predicate 'processID == 0'` has no
record of the plugin.

Registry properties have neither problem: they are queryable for the whole boot
and survive whatever the log does.

### Why there is no IOService subclass and no matching notification

`IOResources` properties are write-only from the plugin's point of view, so an
early design replaced Lilu's `IOService` shell (`LILU_CUSTOM_IOKIT_INIT`) and
registered a `gIOFirstMatchNotification` for `IONVMeController` in `start()`, to
report controllers that appear after the plugin loads.

That design hangs the machine during boot. Symptom: the OpenCore picker is
reachable, the selected macOS entry stops before the desktop, the machine
restarts, and the cycle repeats. `Library/Logs/DiagnosticReports` gains nothing —
no `Kernel-*.panic` is written, only the `shutdown_stall` reports that this
machine also produces on ordinary restarts. Registering an IOKit matching
notification from inside `IOService::start()` of a service that is itself being
matched is the suspect: the registry and matching locks are held by the caller.

Do not reintroduce that shape. Late-bound work happens on the worker thread,
which blocks in `IOService::waitForMatchingService` — a blocking wait on a
dedicated kernel thread, which is the shape that works.

## The block decision

`doUnmap` is where a storage discard (APFS, `diskutil`, anything above the block
storage device) turns into an NVMe Dataset Management command. The original does
four things before it submits anything: takes the device's work-loop gate,
allocates and maps a 4 KiB DSM buffer, increments the device's unmap statistic,
and takes a request from the pool. A `kIOReturnSuccess` returned at entry skips
all four, so a suppressed discard leaves no half-built request and nothing to
unwind; the caller sees success, which is what a suppressed discard is.

Blocking one level lower, in `doUnmapNVMe`, would also suppress the command, but
by then the request has been taken from the pool and a memory descriptor has
been attached to it. The upper level is the cleaner cut and is the one
implemented.

`doUnmapNVMe` builds the command and submits it synchronously
(`BuildCommandDeallocate` → `QueueSubmitRequest` → `ProcessSyncNVMeRequest`), so
its counter is the honest measure of "a TRIM left for this device". On a blocked
device it must stay flat; that is what makes suppression a measurement instead
of an inference.

### Other deallocate carriers

TRIM is opcode 0x09, but two other commands can carry a deallocate request. Both
were audited against this collection before the target device was ever attached:

| Carrier | Evidence | Reachable from macOS |
|---|---|---|
| Write Zeroes, 0x08 | no builder and no method of that name anywhere in the IONVMeFamily slice | no |
| Format NVM, 0x80 (`BuildCommandFormatNVME`, `0x26cd7de`) | the builder has zero cross-references, and `IONVMeBlockStorageDevice::doFormatMedia` (`0x26cfe32`) is a two-instruction stub returning `kIOReturnUnsupported` (`0xE00002C7`) | no |
| Dataset Management, 0x09 (`BuildCommandDeallocate`, `0x26cdcb0`) | exactly one call site, inside `doUnmapNVMe` | yes, and the block sits above it |

`BuildCommandFormatNVME` does put its argument into cdw10, which is where the
SES/deallocate bits live — the command would be able to deallocate if anything
built it. Nothing does. The audit is closed: 0x09 is the only deallocate carrier
this build can emit, and no second block is needed.

### Identity, and where it is read

The decision needs the identity of the device that owns the discard. The hook
receives the `IONVMeBlockStorageDevice`, and that object publishes the same pair
the controller reports, inside its `Device Characteristics` dictionary:

```
"Device Characteristics" = {"Serial Number"="…", "Product Name"="…", …}
```

`SetNVMeProperties` fills that in during device start, i.e. before any media
exists, so by the time a discard can arrive it is there. Reading it from the
device the hook already has avoids the alternative — following the object's
controller pointer at `+0x90`, which is real (`doUnmap` itself uses it) but is
raw offset arithmetic on the request path.

The answer is resolved once per device and cached in the counters table;
`resolveIdentity` leaves it open and retries on the next event if the properties
are not published yet. It is never guessed: a device whose identity is unknown
passes through.

### The rule list comes from Info.plist

There is no device name in the source. The rules live in this kext's own
`Info.plist`, inside the personality dictionary:

```xml
<key>IOKitPersonalities</key>
<dict>
  <key>com.ling.NvmeHook</key>
  <dict>
    ...
    <key>NvmeHookDevices</key>
    <array>
      <dict>
        <key>Model</key>  <string>INTEL SSDPD21K750GAM</string>
        <key>Serial</key> <string>*</string>
      </dict>
    </array>
  </dict>
</dict>
```

IOKit copies personality keys onto the service instance, so `loadIdentities`
reads the array from `ADDPR(selfInstance)` and copies up to `kMaxIdentities` (4)
entries into fixed buffers. Nothing allocates and no file is opened: the values
arrive through the registry, which is the one channel available to a kext at
that point in boot. If the instance does not exist yet when the first discard
arrives, the identity stays unresolved and is retried on the next event.

Several rules are allowed and they are tried in order; the first match wins and
its index is what the registry reports. A field of `*` matches any value, which
is how a model-only rule is expressed — the shipped rule is
`INTEL SSDPD21K750GAM` / `*`, so every drive of that model is covered. To pin a
unit instead, put its serial in place of the `*`; to cover several devices, add
dicts to the array.

Comparison drops trailing blanks from both sides, because NVMe Identify strings
are space padded to a fixed width. An empty or absent *device* value only
matches `*`; it never satisfies a literal rule, so a missing property cannot
turn into a wildcard by accident.

Loading the plugin is the only switch: there is no boot argument that changes
matching or blocking. `-nvmehookoff` remains the Lilu-level switch that stops
the plugin from loading at all, for recovery.

### Costs on the request path

Per discard: one linear scan of at most 8 table entries, one atomic increment,
one comparison of two cached booleans. Nothing allocates; the table is fixed
size. Registry writes are the expensive part and are throttled: the first 17
events are published individually, then every 64th, plus the aggregate
`NvmeHookTotal` on every event (a single `setProperty` on `IOResources`).

Increments are atomic (`OSAddAtomic64`) because two discards on one device can
enter the hook concurrently — the original's work-loop gate is taken inside the
function, not before the hook. Slot claiming uses `OSCompareAndSwap`; if two
threads do claim slots for the same device in the same instant, the counters
split across two entries but the block decision is identical, so no gate reading
is affected.

## IORegistry properties

Published on `IOResources` unless stated otherwise:

| Property | Meaning |
|---|---|
| `NvmeHookVersion` | `MODULE_VERSION` from `Scripts/build-noxcode.sh` |
| `NvmeHookPatches` | `discard suppression for the configured devices, anchor-resolved (M2)` |
| `NvmeHookConfig` | the rule list as read from `Info.plist`: `state=ok; identities=1; [0] model=INTEL SSDPD21K750GAM serial=*`, or the reason it could not be read |
| `NvmeHookTotal` | aggregate `seen=…; blocked=…; dsm=…` over all devices, republished on every event |
| `NvmeHookBus` | result of the plugin-start scan, e.g. `0 controller(s) visible at plugin start` |
| `NvmeHookWorker` | `started` when the worker thread exists |
| `NvmeHookDevice<n>` | one per block storage device seen on the request path: `model=…; serial=…; match=[i] model=… serial=…` or `match=none` |
| `NvmeHookTarget` | on each controller and on `IOResources`: which rule matched (`matched rule [i] …`) or `pass-through controller …` |
| `NvmeHookController<n>` | written during the plugin-start scan if a controller is already published |
| `NvmeHookCounters` | **on the block storage device itself**: `seen=…; blocked=…; dsm=…`, throttled |

Resolution diagnostics, one set per boot:

| Property | Meaning |
|---|---|
| `NvmeHookInstanceCount` | `IONVMeController` instances found by the worker |
| `NvmeHookVptr<n>` | first word of instance `<n>` — its vtable pointer |
| `NvmeHookAnchor` | per instance: the vptr, the three candidates it implies, and `fingerprint=match` / `mismatch` / `skipped (foreign image)` |
| `NvmeHookVptr` | the vptr that became the anchor |
| `NvmeHookSymDoUnmap` / `…NVMe` / `…InitializeController` | runtime addresses of the three hooks, derived from the anchor |
| `NvmeHookHooks` | which of the three hooks routed |

Setting properties on `IOResources` also appends their names to the
`IOResourceMatched` array, because that array is how IOResources advertises what
it can match. This is expected, not a leak.

## Reading the report

```sh
kextstat | grep -iE 'nvmehook|IONVMeFamily'
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Patches|Config|Hooks|Total|Anchor)'
ioreg -r -c IONVMeBlockStorageDevice -l -w0 | grep -E 'NvmeHookCounters'
```

`ioreg -n IOResources` prints a large blob; the grep is required, not optional.

`NvmeHookConfig` must read `state=ok; identities=…` with the rules that were
intended: if it says the key is missing, the `Info.plist` edit did not reach the
injected bundle, and nothing will be blocked.

The gate the counters are read against: `seen` rises, `blocked` equals `seen`,
`dsm` stays at zero. (`dsm` rising together with `seen` means the device is not
matched at all — check `NvmeHookDevice<n>`.)

`NvmeHookAnchor` must read `fingerprint=match`, and
`NvmeHookVptr - NvmeHookSymDoUnmap` must be `0x1AC8A`. Both are independent of
the slide, so they catch a wrong anchor even when the addresses look plausible.

Two invariants make a bad reading obvious: `blocked` never exceeds `seen`, and
`dsm` on a blocked device stays flat across unmount/remount, bulk deletes and a
soak.

## Resolving the hooks on a macOS 26 kernel collection

`workerThread` is the fragile part; read this before changing it.

Dead ends, all verified on this machine:

1. **Lilu `solveSymbol` does not work on Tahoe at all** — neither for kexts nor
   for the kernel. Kext slices in the collection are not resolvable by Lilu
   1.7.2 (its `kcGetRunningAddresses` looks for a literal `__LINKEDIT` segment;
   Tahoe names the shared symbol region `__LINKEDIT_hidden`), and
   collection-builtin kexts report `kmod_info->address = 0`, so the kext
   callback hands the plugin a zero base.
2. **Scanning kernel VM is banned.** A version that walked VM pages to find the
   outer collection header panicked in the wild (unmapped gap between the base
   collection and the OpenCore-injected slices).
3. **`__ZTI16IONVMeController` does not exist** in the slice's symbol table
   (type-info symbols are stripped); `__ZTV16IONVMeController` does.
4. **`kextstat`/`kmutil showloaded` report link addresses, not runtime
   addresses.** KASLR is active; the slide measured 0x11000000–0x1f600000
   (272–502 MB) across boots. Any sanity bound below that rejects real boots.
5. **No file system access from the worker thread.** The 1.1.0 build resolved
   symbol names by reading `BootKernelExtensions.kc` with `vnode_lookup` +
   `vn_rdwr` from its own `kernel_thread_start` thread. It worked on most boots
   and on one boot panicked:

   ```
   panic: Taking non-sleepable RW lock with preemption enabled @lock_rw_old.c
   _IORWLockRead  ←  _namei + 0x7d8  ←  _vnode_lookupat  ←  workerThread + 0x54b
   ```

   A bare kernel thread has no VFS context, and `namei` is not callable from
   one; the serial console only changed the timing enough to expose it. Do not
   reintroduce `vnode_lookup`, `vn_rdwr` or any path lookup here.

The working design:

1. Block in `waitForMatchingService` on the worker thread until an
   `IONVMeController` exists.
2. The instance's first word is its vptr, i.e. `__ZTV16IONVMeController + 16`
   plus the slide. Every hook is a fixed link-time distance from that slot:
   `hook = vptr + (hook link address - (__ZTV link address + 16))`, so the slide
   never has to be computed and no address is read from disk. The distances are
   `kDelta*` at the top of `NvmeHook.cpp`.
3. Each candidate is read for 24 bytes and compared with `kFingerprint*`,
   measured on the same build. Twenty-four and not sixteen: the two block-device
   prologues share their first sixteen bytes and only diverge where they save
   their arguments.
4. Before that read, each candidate must be within 4 MB of the vtable's first
   slot, which is a pointer into the same image. A foreign subclass vtable is
   skipped instead of being read, so a fingerprint read can never land in an
   unmapped gap.
5. The first instance that fingerprints cleanly becomes the anchor; only then
   does `routeFunction` run. A mismatch publishes
   `NvmeHookAnchor … fingerprint=mismatch` and patches nothing.

The constants belong to macOS 26.7.2 / 25G332. An IONVMeFamily update that moves
or recompiles these functions makes the fingerprints fail: visible and safe, but
it also means no protection, so re-measure the `kDelta*`/`kFingerprint*` values
(`vt_slots.py` and `fn_bytes.py` in the `ida/` analysis directory do exactly
that) after any change to that kext.

## Deploy and recovery

Deployment goes to the ESP only and costs one reboot per iteration.

Editing `config.plist` with `plutil`: **`plutil -extract` writes its result back
into the input file when `-o` is omitted.** A read-only-looking query such as
`plutil -extract Kernel.Add json config.plist` replaces the whole configuration
with the extracted fragment. Always pass `-o -` when reading, or use
`plutil -replace` for edits, or edit through `plistlib` after making a backup.

The injected bundle carries its own `Contents/Info.plist`: replacing only the
Mach-O leaves the old version string, which is what `kextstat` prints. Keep the
bundle's three version keys in step with the binary.

If the machine stops reaching the desktop after a deploy:

1. Do not keep rebooting it. Boot the Debian rescue stick: power on and hold
   F11 from before POST until the MSI boot menu appears, then choose `UEFI:
   KingstonDataTraveler …`.
2. In the rescue system the internal ESP is `/dev/sda1` and the macOS volume is
   `/dev/sda3` (read-only via `sudo apfs-mount /dev/sda3 /mnt/mac`).
3. Mount the ESP, set `Enabled` to false for `NvmeHook.kext` in
   `EFI/OC/config.plist` (back up the plist first, keep the kext file), `sync`,
   unmount, reboot.
4. Read the evidence after the desktop is back: `kextstat`, the IORegistry
   properties above, and `Library/Logs/DiagnosticReports` for a
   `Kernel-*.panic`. A boot that leaves no panic entry stopped by hanging, not
   by crashing.

`_scratch/` in the Windows working copy holds the Lilu clone, config copies and
the helper scripts used to add the `Kernel > Add` entry and to set
`Misc > Debug > Target`.
