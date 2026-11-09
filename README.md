# NvmeHook

A Lilu plugin that keeps NVMe TRIM (Dataset Management / deallocate) away from
specific devices whose firmware does not survive it, while every other NVMe
device keeps TRIM unchanged.

Target device: Intel Optane `SSDPD21K750GAM`, serial `PHKD9344003J750F`, 750 GB.
Its firmware drops off the bus when it receives TRIM and takes the machine with
it. Only that sample is faulty; the `WD_BLACK AN1500` in the same machine is the
healthy control device.

## State

Milestone M2. Discards are suppressed for every device that matches a rule in the
kext's `Info.plist`, at the point where a storage discard would become an NVMe
Dataset Management command; every other NVMe device reaches the original
implementation unchanged, byte for byte.

The suppression is measured, not assumed. Three counters are published per
device: `seen` (discards entering the filter), `blocked` (of those, the ones
short-circuited) and `dsm` (Dataset Management commands that left for that
device). On the matched device `blocked` tracks `seen` while `dsm` stays at zero,
and the volume keeps working: written, read back and verified across bulk deletes,
unmount/remount cycles and a sleep/wake cycle.

Verified on the control device first, which shares the target's media family, and
then on the target itself: the Optane was erased to a single APFS volume and put
through an hour of write/read-back/delete cycles, three unmount/remount cycles
and a sleep/wake, with `blocked == seen` and `dsm` at zero throughout — 394
discards swallowed, none reaching the hardware — and `diskutil verifyVolume`
clean at the end. `HANDOFF.md` carries the state of the deployed build and the
operational procedures; `DEBUG.md` carries the implementation, the property table
and the way the hooks are resolved.

The hooks are found from a live `IONVMeController` and fingerprinted before use,
so the driver never reads the file system and a build whose code does not match
makes it inert instead of dangerous.

## Configuration

The devices to suppress are listed in this kext's `Info.plist`, under the
personality, and nowhere else — there are no device names in the source and no
boot argument that changes matching: loading the plugin means its rules apply.

```xml
<key>NvmeHookDevices</key>
<array>
    <dict>
        <key>Model</key>
        <string>INTEL SSDPD21K750GAM</string>
        <key>Serial</key>
        <string>*</string>
    </dict>
</array>
```

- Several dicts may be listed; they are tried in order and the first match wins.
- `*` in either field matches any value. The shipped rule pins the model and
  accepts any serial, so every drive of that model is covered; replace the `*`
  with `PHKD9344003J750F` to pin the single unit instead.
- Matching is on the block storage device the discard belongs to, and the result
  is cached per device.
- Edit the list on the ESP copy of `NvmeHook.kext` and reboot; OpenCore injects
  the bundle, so a change needs a restart.

| boot-arg | effect |
|---|---|
| `-nvmehookoff` | do not load the plugin at all (checked by Lilu) |
| `-nvmehookdbg` | Lilu debug output for this plugin |
| `-nvmehookbeta` | allow loading on beta kernels |

`-nvmehookoff` is the kill switch: with it the machine behaves as if the driver
were not installed. It is also the switch that removes all protection, so it is
for recovery, not for daily use.

## Requirements

- macOS 11 through 26 (Darwin 20 through 25), x86_64.
- Lilu 1.7.x loaded as `as.vit9696.Lilu`.
- A Lilu source tree at build time (`Headers/` and `Library/plugin_start.cpp`);
  a released `Lilu.kext` ships only the headers and is not enough.

## Build

Run on macOS; neither Xcode nor sudo is required, only the Command Line Tools:

```sh
git submodule update --init MacKernelSDK
sh Scripts/build-noxcode.sh            # Lilu tree from $HOME/src/Lilu
```

The script writes one output directory per run and records the newest kext in
`build/latest-noxcode-build.txt`. The version lives in `VERSION=` inside the
script and feeds both the binary and `Info.plist`.

## Install

Lilu and this plugin are installed on the ESP only, never on the system volume:

1. Copy `NvmeHook.kext` to `EFI/OC/Kexts/` on the internal SATA disk's EFI
   partition.
2. Add one `Kernel > Add` entry for it after `Lilu.kext` in `EFI/OC/config.plist`,
   keeping a backup of the plist next to itself and validating the result.
3. Reboot; the kext is injected by OpenCore.

Putting the kext in `/Library/Extensions` does not work: `kmutil` resolves
dependencies against kexts on disk, Lilu is not there, and the collection build
fails.

## Verify

```sh
kextstat | grep -iE 'nvmehook|IONVMeFamily'
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Blocking|Hooks|Total)'
ioreg -r -c IONVMeBlockStorageDevice -l -w0 | grep NvmeHookCounters
```

The kernel log is not a usable record here: it is a ring buffer that ordinary
boot traffic overwrites within a minute. `DEBUG.md` explains the IORegistry
report, the counters and the debugging procedure.
