# Handoff

Date: 2026-11-09. Written to move this work to another agent without re-deriving
what has already been established. `PLAN.md` holds the design and the test plan;
this file holds state, access, and the mistakes already paid for.

## 1. Objective

A Lilu plugin that stops NVMe TRIM from ever reaching one specific device: an
Intel Optane `SSDPD21K750GAM`, serial `PHKD9344003J750F`, 750 GB, whose firmware
drops off the bus when it receives TRIM, hanging the machine. Every other NVMe
device must keep TRIM unchanged.

That device is **not attached** to the machine at the time of writing. The only
NVMe present is the control device (section 4).

## 2. Constraints that are not negotiable

- **Lilu and this plugin live on the ESP only.** Nothing of either is written to
  `/Library/Extensions` or any other system volume. See `AGENTS.md`.
- The active ESP is the **internal SATA disk's** EFI partition. Never write
  OpenCore files to a USB disk's EFI. The USB sticks are install and rescue media.
- **Only the 750 GB sample is faulty.** The 32 GB array is healthy and verified
  working; do not treat it as a suspect, and do not use "the array is also Optane"
  to explain away a driver defect.
- The goal is per-device suppression. A global TRIM disable is not the answer,
  even though OpenCore's `Kernel > Quirks > SetApfsTrimTimeout = -1` can do it.
- Documentation conventions are in `AGENTS.md`: `README.md` states current truth,
  `DEBUG.md` carries implementation and debugging detail, and neither is a log.

## 3. Repository

Windows path: `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\drivers_src\NvmeHook`
(branch `master`, `origin/master` present).

Working tree state at handoff — **all of this is uncommitted**:

| Path | State |
|---|---|
| `AGENTS.md` | modified (deployment and test-device constraints added) |
| `PLAN.md` | new, untracked |
| `NvmeHook/NvmeHook.cpp` | new, untracked |
| `NvmeHook/Info.plist` | new, untracked |
| `Scripts/build-noxcode.sh` | new, untracked |
| `README.md`, `DEBUG.md` | modified: they state the M0 result |
| `HANDOFF.md` | untracked: this file. It is not in git; keep a copy if it matters. |
| `MacKernelSDK` | submodule, pinned at `3f75008` |
| `_scratch/` | ignored: Lilu source clone, config plists, helper scripts |

Committing is safe and nothing else is in flight. `_scratch/` is gitignored and
holds a full Lilu 1.7.2 checkout plus the OpenCore config copies described below.

## 4. Machine

| Item | Value |
|---|---|
| macOS | 26.7.2 (Darwin 25), x86_64 |
| SSH | `ling@192.168.50.204`, key auth, `sudo` NOPASSWD |
| Boot disk | `disk2`, SanDisk SD8SNAT-128G, internal SATA, APFS |
| Active ESP | `disk2s1`, mounts at `/Volumes/EFI` |
| OpenCore | in `EFI/OC`, loads Lilu, `AMDMapperHook`, AMD kexts |
| `IONVMeFamily` | 2.1.0 |
| Lilu | 1.7.2, injected by OpenCore, **not** on disk |
| Control NVMe | `disk0` `WD_BLACK AN1500`, serial `WUBT21431655`, two 16 GB Optane M10 in a set, APFS volume `Optane_32G` mounted, TRIM on, healthy |
| Target NVMe | absent |
| USB `disk4` | install media + `OCRESCUE`, do not modify |
| NanoKVM | `192.168.50.155`, root, known_hosts at `MSI_X570/tools/ssh/nanokvm_known_hosts`; power is the recovery path |

SSH must use `-o BatchMode=yes -o StrictHostKeyChecking=yes`. The Mac's clock is
in US Pacific time, so its timestamps run 16 hours behind Asia/Shanghai; do not
mistake that for a stale log.

## 5. Build

The Mac has only the Command Line Tools, no Xcode. Build happens on the Mac from
a mirror of the repository:

| Path on the Mac | What it is |
|---|---|
| `~/src/NvmeHook` | mirror of the repo (`NvmeHook/`, `Scripts/`, `MacKernelSDK/`) |
| `~/src/Lilu` | Lilu 1.7.2 **repository root**; the source dir is `~/src/Lilu/Lilu` |

```sh
ssh ling@192.168.50.204 'cd ~/src/NvmeHook && sh Scripts/build-noxcode.sh'
```

The newest kext path is written to `build/latest-noxcode-build.txt`.

Three traps, all already hit:

1. **Lilu's `Headers/hde64.h` is a symlink to `../../hde/hde64.h`.** A tree that
   keeps only `Lilu/Headers` and `Lilu/Library` will not compile. Keep the
   repository layout and point the build at `~/src/Lilu/Lilu`.
2. **Copying files from Windows with `tar` preserves the read-only attribute on
   directories** (mode 555), which makes `rm -rf` fail with "Permission denied".
   Run `chmod -R u+rwX` on the extracted tree.
3. The Mac sometimes cannot reach github.com. Clone on Windows instead, through
   the proxy in the MCP PowerShell console, then copy the tarball over.

## 6. Deploy

```sh
# kext
sudo cp -R <build>/NvmeHook.kext /Volumes/EFI/EFI/OC/Kexts/
# config: Kernel > Add gains one entry after Lilu, then
sudo cp /Volumes/EFI/EFI/OC/config.plist /Volumes/EFI/EFI/OC/config.plist.bak-<reason>-<timestamp>
plutil -lint /Volumes/EFI/EFI/OC/config.plist
sync
```

The ESP currently holds the M0 configuration: `NvmeHook.kext` enabled after Lilu,
and `Misc > Debug > Target = 103` (`0x67`, OpenCore writes its own log file to the
ESP). Backups of both earlier states sit next to it (`config.plist.bak-nvmehook-*`).

**`plutil -extract` writes its result back into the input file when `-o` is
omitted.** `plutil -extract Kernel.Add json config.plist` replaces the entire
configuration with the extracted fragment; that happened once and cost a restore
from backup. Always pass `-o -` to read, `plutil -replace` to edit, or edit with
Python `plistlib` after backing up. `_scratch/add_oc_entry.py` and
`_scratch/set_oc_log_target.py` are the safe helpers for the two recurring edits.

Why not `/Library/Extensions`: tried, and it cannot work. `kmutil` builds the
auxiliary collection from kexts on disk and Lilu is not there, so it fails with
`Failed to bind '_lilu' as could not find a kext with 'as.vit9696.Lilu' bundle-id`.

Side effect still present: that experiment put `com.ling.NvmeHook` into the
auxiliary collection's history, so `kernelmanagerd` logs
`Couldn't load bundle 'com.ling.NvmeHook' from history: ... No bundle at
/Library/Extensions/NvmeHook.kext` on every boot. Harmless, and it disappears
after `sudo kmutil install --update-all` or the next macOS update rebuilds the
collection. Do not "fix" it by putting files back in `/Library/Extensions`.

## 7. State of the work

Milestone **M0 passes**: the plugin loads (`com.ling.NvmeHook 0.1.0` in
`kextstat`), patches nothing, and reports itself to the IORegistry instead of the
kernel log:

```sh
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Patches|Bus)'
```

`NvmeHookBus` currently reads `0 controller(s) visible at plugin start`. That is
correct rather than a fault: the injected kext loads before `IONVMeFamily`
publishes its controllers, so the scan in the plugin start callback sees none.
`DEBUG.md` holds the property table, the reasoning and the reading commands.

Two things were established the hard way, both recorded in `DEBUG.md`:

- Kernel logging is unusable as evidence here. The ring buffer is overwritten by
  boot traffic within about 40 seconds and the early messages never reach the
  unified log, so neither `dmesg` nor `log show` shows the plugin at all. Hence
  the IORegistry report.
- A first attempt published the report from a custom `IOService` subclass
  (`LILU_CUSTOM_IOKIT_INIT`) that registered a `gIOFirstMatchNotification` for
  `IONVMeController` inside `start()`. It hangs the machine during boot: the
  OpenCore picker stays reachable, macOS never reaches the desktop, the machine
  restarts, and the cycle repeats with no `Kernel-*.panic` written. That design
  is gone; do not reintroduce it.

## 8. Immediate next step

M1: find the exact `IONVMeFamily` entry point that turns a storage discard into a
Dataset Management command, and instrument it with counters only.

- `PLAN.md` sections 3, 5 and 6 define the acceptance gates. Do not skip stage A
  or B on the control device before the Optane is ever mounted.
- The per-controller identity report that M0 cannot produce belongs here: hook
  `IONVMeController::start` through Lilu's routing and publish model and serial
  from the hooked call, where the controller provably exists.
- Publish counters the same way, as IORegistry properties on the controller or on
  `IOResources`. Keep the kernel log out of the loop.

## 9. Things that will cost time if rediscovered

- **Reboots are the deploy loop.** Batch changes; there is no way to load a
  Lilu plugin on this machine without OpenCore injection.
- Mounting the ESP is per boot: `sudo diskutil mount disk2s1`. USB EFI, if ever
  needed for reading only, is `sudo diskutil mount readOnly disk4s1` and must
  not be written.
- `kextload`/`kextutil` are shims over `kmutil` here and cannot install a plugin
  that depends on Lilu; do not spend time on them.
- A kext copied out of a user home fails validation on ownership (501:20 vs 0:0);
  irrelevant now that deploy goes to the ESP, where FAT stores no ownership.
- macOS on this machine sometimes stalls on restart and leaves
  `Library/Logs/DiagnosticReports/shutdown_stall_*.shutdownStall`; that is not a
  driver panic and does not by itself indicate a bad deploy.
- **Boot-loop recovery**: press and hold F11 from before POST until the MSI boot
  menu appears, choose the Kingston USB, and in the Debian rescue system mount
  `/dev/sda1` (the internal ESP), set `Enabled = false` for `NvmeHook.kext` in
  `EFI/OC/config.plist` after backing it up, `sync`, unmount, reboot. The macOS
  volume there is `/dev/sda3`; `sudo apfs-mount /dev/sda3 /mnt/mac` mounts it
  read-only for evidence such as panic reports. Rescue SSH is
  `rescue@192.168.50.204` with `MSI_X570/installer_media_creation/debian-rescue/rescue-known_hosts`.
- NanoKVM can be driven over SSH without the web UI:
  `MSI_X570/tools/ssh/kvm-screenshot.py` (screenshot) and the same MCP endpoint
  for `cube_press_keys` / `cube_type_text`. Screenshots may lag a frame; take a
  second one before acting on what it shows.
- Before any change that carries a panic risk, confirm the NanoKVM channel works
  (hostname `kvm-2c33`, `hw=pcie`, `mode=mcp`) — it is the only way back if the
  machine stops booting.
- Serial console debugging is off by default on this machine and is expensive
  when on; turn it on only for the acceptance run, and off immediately after.
