# NvmeHook — handoff notes

Date: 2026-11-09. Driver version: 1.0.0 (deployed and verified on the target
machine). Read `PLAN.md` (what and why), `AGENTS.md` (repo conventions and test
device rules) and `DEBUG.md` (driver shape, IORegistry report, debugging)
before touching anything. This file carries what those three do not: the state
of the deployed driver, the macOS 26 symbol-resolution design with its dead
ends, the operational procedures, and the next milestone.

## 1. Where things live

| Location | Role |
|---|---|
| `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\drivers_src\NvmeHook` | Canonical repo (git). All source edits happen here. |
| `ling@192.168.50.204:~/src/NvmeHook` | Build copy on the macOS machine. `NvmeHook.cpp` and `Scripts/build-noxcode.sh` are synced from the repo by hand (scp) before every build — do not assume they match the repo. |
| `ling@192.168.50.204:~/src/Lilu` | Lilu source tree the build requires. |
| Target machine ESP, `disk2s1` | `EFI/OC/Kexts/NvmeHook.kext` + `EFI/OC/config.plist`. disk2s1 is the internal SATA disk's EFI — the only ESP that may be written; the other EFI partitions belong to USB media. |

SSH access from the Windows side: `ssh -i C:\Users\ling\.ssh\id_rsa
ling@192.168.50.204` (key auth, no password). sudo on that box is passwordless.

The deployed binary is built from the repo sources at version 1.0.0. The repo's
`README.md` and `DEBUG.md` still describe milestone M0 and are out of date on
the driver's shape (see §5 — updating them is part of the next milestone).

## 2. What is deployed and what is verified

The deployed 1.0.0 does **M1 only**: it hooks two IONVMeFamily functions and
counts; no request is altered. Verified on the machine (IORegistry on
`IOResources`, one entry per boot):

```
NvmeHookVersion                 = 1.0.0
NvmeHookPatches                 = counters only, symbol-resolved (M1.8)
NvmeHookHooks                   = armed by symbol: counters + InitializeController
NvmeHookKextBase                = 0xffffff80162bd000   (slide 0x13c00000 this boot)
NvmeHookSymDoUnmap              = 0xffffff80162d0396   (link 0x26d0396 + slide)
NvmeHookSymDoUnmapNVMe          = 0xffffff80162d016c
NvmeHookSymInitializeController = 0xffffff80162c290c
NvmeHookTotal                   = seen=2; dsm=2        (Stage A: rise together)
NvmeHookTarget                  = pass-through controller (model=WD_BLACK AN1500; serial=WUBT21431655)
NvmeHookVptr, NvmeHookProbe, NvmeHookFnProbe, NvmeHookKcStage, NvmeHookInstanceCount
                                = resolution diagnostics (see §4)
```

Multiple cold boots done; every boot re-resolves the slide (it changes per
boot, KASLR is active) and re-arms. No panic, no regression.

The Stage A gate from `PLAN.md` §6 is met: counters move together under real
discard traffic, the machine is stable. `M2` (blocking on the control device)
is the next milestone and is not implemented.

## 3. Device facts (from `AGENTS.md`, restated because they decide every judgement)

- Target (faulty, **not attached**): Intel Optane `INTEL SSDPD21K750GAM`, serial
  `PHKD9344003J750F`, 750 GB. Its firmware drops off the bus on TRIM.
- Control (healthy, attached): `WD_BLACK AN1500`, serial `WUBT21431655`. It is
  the only `IONVMeController` currently published on the machine
  (`NvmeHookInstanceCount = 1`).
- Everything about a Samsung 990 EVO / "DirectLinkSupport" that may linger in
  old notes or agent memories is wrong for this project and has been removed
  from the driver. The identity match is model **and** serial
  (`isTargetController` in `NvmeHook.cpp`).
- Note: `IONVMeController` publishes `Model Number` / `Serial Number` but no
  `Vendor Identifier` (an earlier probe read 0x0). Do not match on vendor.

## 4. The macOS 26 (Tahoe) symbol-resolution design

Read this before changing anything in `workerThread` / `resolveKcSymbols`
(`NvmeHook.cpp`). Four approaches were tried; three are dead, one works.

Dead ends, all verified on this machine:

1. **Lilu `solveSymbol` does not work on Tahoe at all** — neither for kexts
   nor for the kernel. Two independent reasons: kernel-collection kext slices
   are not resolvable by Lilu 1.7.2 (its `kcGetRunningAddresses` looks for a
   literal `__LINKEDIT` segment; Tahoe marks the shared symbol region
   `__LINKEDIT_hidden`), and collection-builtin kexts report
   `kmod_info->address = 0`, so Lilu's kext callback hands the plugin a zero
   base. Do not "fix" this by parsing kmods or trusting the callback's slide.
2. **Scanning kernel VM is banned.** A version that walked VM pages to find the
   outer collection header panicked in the wild (unmapped gap between the base
   collection and the OpenCore-injected slices). Kernel file reads via
   `vn_rdwr` return errors instead of faulting — that is the only sanctioned
   memory access that is not backed by a validated live object.
3. **`__ZTI16IONVMeController` does not exist** in the collection's symbol
   table (type-info symbols are stripped); `__ZTV16IONVMeController` does.
   Any validation built on the type-info symbol cannot work.
4. **`kextstat`/`kmutil showloaded` report link addresses, not runtime
   addresses.** Comparing them against runtime pointers misleads; KASLR is
   active and the slide was measured at 0x13c00000–0x1f600000 (~315–530 MB)
   across boots. Any sanity bound on the slide below ~1 GB silently rejects
   every real boot.

The working design (`workerThread`):

1. A worker thread (started from Lilu's `patchersReady` callback) waits for an
   `IONVMeController` instance via `IOService::waitForMatchingService`. This
   must stay a **blocking wait on a dedicated kernel thread** — `DEBUG.md`
   documents that the alternative (an IOKit matching notification registered in
   `start()`) hangs the machine at boot. Do not reintroduce it.
2. The instance's first word is its vptr — an address inside IONVMeFamily,
   whatever the slide is.
3. The on-disk collection
   `/System/Library/KernelCollections/BootKernelExtensions.kc` is read with
   `vn_rdwr` (safe). The IONVMeFamily slice is found through its
   `LC_FILESET_ENTRY` (cmd `0x80000035`), and the slice's own `LC_SYMTAB`
   resolves the hook symbols **plus `__ZTV16IONVMeController`** by mangled
   name. Symbol link values give offsets relative to the slice, which is why
   the aux collection (OpenCore-injected, different layout) is irrelevant as
   long as the base kext binary is unchanged.
4. `slide = vptr - (__ZTV link value + 2 * sizeof(uintptr_t))`. The vptr points
   at the first virtual function slot; the vtable symbol points two slots
   earlier, and the vtable lives in `__DATA` — **not** in `__TEXT` (an earlier
   validation required the vptr itself to be in `__TEXT` and rejected every
   real boot).
5. Validation before routing: slide page-aligned and bounded
   (`< 0x1000000000` — a 256 MB bound was the last bug and rejected every real
   boot), `vptr - slide` inside the slice's full segment span, and the function
   pointers stored in the first vtable slots minus slide inside the slice's
   `__TEXT`. All checks are visible in the registry diagnostics
   (`NvmeHookProbe`, `NvmeHookFnProbe`, `NvmeHookKcStage`) — keep them when
   editing.
6. Only after everything validates does `routeFunction` run. Any failure
   publishes a status and leaves the system unpatched instead of guessing.

Design constraints from `PLAN.md` §4 that are not implemented yet and must not
be forgotten: per-device short-circuit in the hook (match on the owning
instance), bias to over-block, kill-switch boot-arg (`-nvmehookoff` already
gates loading via Lilu), and the Write Zeroes (0x08) / Format NVM (0x80)
deallocate audit.

## 5. Known doc debt

- `README.md` "State" still says milestone M0.
- `DEBUG.md` "Shape of the driver" describes the M0 `chefStart`; the real shape
  now includes the worker thread, the collection parser and the identity
  report, and the IORegistry property table has grown (`NvmeHookTarget`,
  `NvmeHookDevice*`, `NvmeHookCounters`, `NvmeHookTotal`, the resolution
  diagnostics listed in §2).
- Both must be brought in line when M2 lands, per `AGENTS.md`: state documents,
  no round-by-round history, no commit pinning.

## 6. Build, deploy, verify

Build (on the Mac, in `~/src/NvmeHook`, after syncing the two files from the
repo):

```sh
sh Scripts/build-noxcode.sh
# binary: build/noxcode.*/NvmeHook.kext/Contents/MacOS/NvmeHook
```

The version lives in `VERSION=` inside `Scripts/build-noxcode.sh` (1.0.0).
`Info.plist` must stay LF-only or the kext is silently dropped.

Deploy (from Windows, all over SSH):

```sh
scp <built binary> ling@192.168.50.204:~/NvmeHook-new
ssh ling@192.168.50.204
sudo diskutil mount disk2s1
sudo cp ~/NvmeHook-new /Volumes/EFI/EFI/OC/Kexts/NvmeHook.kext/Contents/MacOS/NvmeHook
sudo diskutil unmount disk2s1
sudo reboot
```

The `Kernel > Add` entry for `NvmeHook.kext` is present and `Enabled`. To
disable it (panic recovery), toggle `Enabled` with `python3` + `plistlib`
against `/Volumes/EFI/EFI/OC/config.plist` — backups exist next to it
(`config.plist.bak-m1-probe`, `bak-m15-deploy`, `bak-m15-panic`).

Verify after boot:

```sh
kextstat | grep -iE 'nvmehook|IONVMeFamily'
ioreg -lw0 | grep -oE '"NvmeHook[A-Za-z0-9]*" = "[^"]*"' | sort -u
```

`kextstat` shows the **link** address; `NvmeHookKextBase` shows the runtime
base. Sanity check: `NvmeHookSymDoUnmap - NvmeHookKextBase` must equal `0x13396`
(the link offset of doUnmap in the 25G332 collection — the same offset the old
hard-coded driver used; the new driver re-derives it from the symbol table on
every boot instead).

## 7. Panic recovery (proven, keep within reach)

A panicking boot loops as long as the ESP entry is enabled, so recovery is a
routine, not an emergency:

1. Power off: on the KVM host `root@192.168.50.155`,
   `echo 1 > /sys/class/gpio/gpio503/value; sleep 6; echo 0 > ...`, then check
   that `gpio504` reads `1` (LED off). gpio503 is the power button (1 =
   pressed), gpio504 the LED (0 = machine on).
2. Power on with F11 held through POST:
   `nohup python3 /root/nanokvm-boot-helper.py --stop /tmp/<fresh>.stop > /tmp/log 2>&1 &`.
   Early `write /dev/hidg0: i/o timeout` lines are normal.
3. When the MSI boot menu appears: `touch` the stop marker, wait ~5 s. Menu
   rules: pressing Enter while no grey bar is visible boots the orange
   (last-boot) entry; pressing a direction key makes the grey bar appear; the
   grey bar is the selection. Target entry: `KingstonDataTraveler ...
   Partition 1` (GRUB → Debian rescue). The rescue machine's IP is printed on
   its tty1 banner — confirm it there instead of trusting notes.
4. In the rescue environment: `sudo mount /dev/sda1 /mnt/esp`, then
   `sudo python3 /tmp/oc_disable_kext.py /mnt/esp/EFI/OC/config.plist
   NvmeHook.kext off` (re-upload the script if `/tmp` is empty), `sync`,
   unmount, reboot back into macOS.
5. Panic evidence, from the running system afterwards:
   `/Library/Logs/DiagnosticReports/Kernel-*.panic` and
   `sudo nvram aapl,panic-info`. An early panic may leave neither.

NanoKVM screenshots lag one frame: after any keypress take a second screenshot
before concluding anything. If remote control returns "preempted by manual
input", the web UI holds manual control — stop and ask.

## 8. Next milestone: M2 (blocking on the control device)

Implementation sketch, per `PLAN.md` §3–§5:

- In `doUnmapNVMeHook`: if the owning instance matches the configured identity,
  return success **without** calling the original — the DSM command is never
  built. Everything else calls the trampoline unchanged.
- `doUnmapHook` counts `seen` and `blocked`; `dsm` keeps counting at the
  current instrumentation point. Stage A gate: `dsm` rises with `seen`
  (already proven). Stage B gate: `blocked == seen`, `dsm` flat.
- For Stage B the matched identity is temporarily the **control device's**
  serial (`WUBT21431655`); the real target identity
  (`INTEL SSDPD21K750GAM` / `PHKD9344003J750F`) goes in only for Stage C/D.
  Make the identity a single obvious constant (or a boot-arg override), never
  a scattered string compare.
- The owning instance for a discard is the `IONVMeBlockStorageDevice`
  (`self` in the hook); its controller is reachable the same way
  `IONVMeFamily` reaches it, or the identity can be cached per instance in the
  existing `Counters` table keyed on `self` — the registry identity is already
  read once there (`NvmeHookDevice*`).
- Respect the plan's bias to over-block: no range heuristics, no "only large
  requests". The kill switch (`-nvmehookoff`) must keep working; consider a
  second boot-arg that gates only the blocking stage.
- Afterwards: update `README.md`/`DEBUG.md` (§5) and only then move to
  Stage C (bare target attach) and Stage D (acceptance) — the gates and the
  serial-logging requirement are in `PLAN.md` §6.
