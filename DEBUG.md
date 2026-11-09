# NvmeHook — implementation and debugging

## Shape of the driver

A Lilu plugin with no IOKit personality of its own beyond the shell Lilu
supplies. All M0 work happens in the `PluginConfiguration::pluginStart` callback
(`chefStart` in `NvmeHook/NvmeHook.cpp`), which Lilu calls once, after it has
accepted the plugin.

`chefStart`:

1. publishes the version and a "patches: none" marker on `IOResources`;
2. enumerates `IONVMeController` through `IOService::serviceMatching` and
   `getMatchingServices`, and writes what it finds back into the registry;
3. records how many controllers were visible, verbatim, in `NvmeHookBus`.

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

`IOResources` properties are write-only from the plugin's point of view, so the
first design replaced Lilu's `IOService` shell (`LILU_CUSTOM_IOKIT_INIT`) and
registered a `gIOFirstMatchNotification` for `IONVMeController` in `start()`, to
report controllers that appear after the plugin loads.

That design hangs the machine during boot. Symptom: the OpenCore picker is
reachable, the selected macOS entry stops before the desktop, the machine
restarts, and the cycle repeats. `Library/Logs/DiagnosticReports` gains nothing —
no `Kernel-*.panic` is written, only the `shutdown_stall` reports that this
machine also produces on ordinary restarts. Registering an IOKit matching
notification from inside `IOService::start()` of a service that is itself being
matched is the suspect: the registry and matching locks are held by the caller.

Do not reintroduce that shape. When late-bound reporting is needed (M1 must see
each controller as it starts), hook `IONVMeController::start` through Lilu's
routing instead, and publish from the hooked call, not from a notification
registered in `start()`.

## IORegistry properties

Published on `IOResources`:

| Property | Meaning |
|---|---|
| `NvmeHookVersion` | `MODULE_VERSION` from `Scripts/build-noxcode.sh`, currently `0.1.0` |
| `NvmeHookPatches` | always `none (M0 reports the bus only)` in M0 |
| `NvmeHookBus` | result of the scan, e.g. `0 controller(s) visible at plugin start` |
| `NvmeHookController<n>` | written on each `IONVMeController` found, `model=…; serial=…` |

Setting properties on `IOResources` also appends their names to the
`IOResourceMatched` array, because that array is how IOResources advertises what
it can match. This is expected, not a leak.

On this machine `NvmeHookBus` reads `0 controller(s) visible at plugin start`:
the injected kext loads before `IONVMeFamily` publishes its controllers, so the
scan legitimately sees none. The property proves the code ran and says so instead
of leaving the reader to guess. Controller visibility arrives with the M1 hook,
which runs when a controller actually starts.

## Reading the report

```sh
kextstat | grep -i nvmehook
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Patches|Bus)'
ioreg -r -c IONVMeController -l -w0 | grep -E 'Model Number|Serial Number|NvmeHookController'
```

`ioreg -n IOResources` prints a large blob; the grep is required, not optional.

## Deploy and recovery

Deployment goes to the ESP only and costs one reboot per iteration.

Editing `config.plist` with `plutil`: **`plutil -extract` writes its result back
into the input file when `-o` is omitted.** A read-only-looking query such as
`plutil -extract Kernel.Add json config.plist` replaces the whole configuration
with the extracted fragment. Always pass `-o -` when reading, or use `plutil
-replace` for edits, or edit through `plistlib` after making a backup.

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

## M1: locating the discard entry points

Targets, from the boot kernel collection:

```
IONVMeBlockStorageDevice::doUnmap       storage-layer discard entry   (`seen`)
IONVMeBlockStorageDevice::doUnmapNVMe   builds the NVMe DSM command   (`dsm`)
```

```
sudo nm -n /System/Library/KernelCollections/BootKernelExtensions.kc \
  | grep IONVMeBlockStorageDevice | grep -i unmap
ffffff80026d016c T __ZN24IONVMeBlockStorageDevice11doUnmapNVMeEP16AppleNVMeRequestP26IOBlockStorageDeviceExtentjP17NVMeLBARangeEntry
ffffff80026d0396 T __ZN24IONVMeBlockStorageDevice7doUnmapEP26IOBlockStorageDeviceExtentjj
```

Their prologues, read from that collection, are the two signatures the driver
verifies before patching anything:

```
doUnmap     55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 89 cb 41 89
doUnmapNVMe 55 48 89 e5 41 57 41 56 41 55 41 54 53 48 83 ec 28 48 89 75 c0
```

### Why addressing it from the callback does not work directly

The `onKextLoad` callback reports the kext image's address and size, and for this
kext they are **0xffffff7f…** and **0x19000**: header, `__DATA`, `__DATA_CONST`
and a shared `__LINKEDIT`, with no code. The image in the kext submap keeps only
data; the code is placed in the kernel's executable region, and that placement
does not match the on-disk collection:

- the on-disk image's `__TEXT` is 0x2b000 and the two functions sit at +0x13396
  and +0x1316c inside it;
- at runtime those offsets read as zeros, and the whole image is only 0x19000;
- scanning every segment of the runtime image for the prologues finds nothing.

`KernelPatcher::solveSymbol` does not help here either: the kext's own MachInfo
carries no symbol table (`solveSymbol(index, …)` returns 0), and the kernel
symbol table does not hold the kext's symbols (`solveSymbol(KernelID, …)` also
returns 0), even though `_kmod` resolves fine through that same call. Computing
the slide from `_kmod` and adding it to the collection's link-time addresses lands
outside the real code as well, which is why the prologue check rejects it.

The current build therefore stops at "prologue mismatch, nothing patched": the
counters are written but never armed, and nothing is redirected.

### Next options, in order of preference

1. Make Lilu read the runtime collection: call `updateRunningInfo(id, slide,
   size, true)` for the kext before solving symbols, so that `solveSymbol(id, …)`
   operates on the running image rather than the on-disk one.
2. Hook at the IOKit level instead of in code: find the `IONVMeBlockStorageDevice`
   instance and replace its vtable entry for `doUnmap`, which needs no code
   address arithmetic but does need the object to exist.
3. Locate the code by scanning the kernel's executable region for the prologues,
   bounded to the region the running collection reports. Only attempt this with a
   mapped-range source: an unbounded scan of kernel space is exactly what produced
   the page-fault panics described below.

### Two panics that came out of this, and what they cost

Both were page faults inside the plugin's own kext-load callback, both from
addressing arithmetic, not from the hooks:

- a slide/absolute mix-up produced `CR2=0xffffff001e12d000`, a non-canonical
  address derived from a segment `vmaddr` treated as a slide;
- a `segment_command_64` parse that read `segname` where `vmaddr` belongs (the
  fields start at `offset + 24`, not `offset + 8`) produced segment ranges that
  were never mapped.

Neither wrote to kernel memory; the plugin crashed while reading. That is why
every scan is now bounded by data that comes from the image itself, and why the
prologue comparison happens before any patch.

## Recovery flow for a boot loop

Given by the user and confirmed on this machine:

1. Start sending repeated F11 presses immediately after the machine starts
   rebooting and keep them going through POST.
2. When the MSI boot menu appears, stop F11, press ArrowDown once to move to the
   Kingston entry (`UEFI: KingstonDataTraveler 3.00000, Partition 1`), press
   Enter.
3. Wait for Debian to come up, then `rescue@192.168.50.204` with
   `MSI_X570/installer_media_creation/debian-rescue/rescue-known_hosts`.

Long key holds repeat in the firmware menus and skip entries; single short
presses are the reliable choice once a menu is on screen. In the rescue system
the internal ESP is `/dev/sda1` and the macOS volume `/dev/sda3` (read-only via
`sudo apfs-mount /dev/sda3 /mnt/mac`); disabling the `Kernel > Add` entry there
restores a bootable machine. `/tmp` on the rescue medium is RAM and does not
survive a reboot.
