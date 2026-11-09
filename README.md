# NvmeHook

A Lilu plugin that keeps NVMe TRIM (Dataset Management / deallocate) away from
specific devices whose firmware does not survive it, while every other NVMe
device keeps TRIM unchanged.

Target device: Intel Optane `SSDPD21K750GAM`, serial `PHKD9344003J750F`, 750 GB.
Its firmware drops off the bus when it receives TRIM and takes the machine with
it. Only that sample is faulty; the `WD_BLACK AN1500` in the same machine is the
healthy control device.

## State

Milestone M0. The plugin loads, reports the NVMe controllers it can see to the
IORegistry, and patches nothing: no request is inspected, altered or blocked yet.
`NvmeHookVersion`, `NvmeHookPatches` and `NvmeHookBus` are published on
`IOResources`; see `DEBUG.md` for the property table and for how to read it.
Interception is the next milestone.

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
`build/latest-noxcode-build.txt`.

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
kextstat | grep -i nvmehook
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Patches|Bus)'
```

The kernel log is not a usable record here: it is a ring buffer that ordinary
boot traffic overwrites within a minute. `DEBUG.md` explains the IORegistry
report and the debugging procedure.
