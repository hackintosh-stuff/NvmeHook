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
