# NvmeHook — handoff notes

Date: 2026-11-10. Driver version: 1.3.0 (deployed on the target machine). Read
`PLAN.md` (what and why), `AGENTS.md` (repo conventions and test device rules)
and `DEBUG.md` (driver shape, IORegistry report, debugging) before touching
anything. This file carries what those three do not: the state of the deployed
driver, the macOS 26 hook-resolution design with its dead ends, the operational
procedures, and the next milestone.

## 1. Where things live

| Location | Role |
|---|---|
| `C:\Users\ling\OneDrive\Documents\Agent\Hackintosh\drivers_src\NvmeHook` | Canonical repo (git). All source edits happen here. |
| `ling@192.168.50.204:~/src/NvmeHook` | Build copy on the macOS machine. `NvmeHook/NvmeHook.cpp` and `Scripts/build-noxcode.sh` are synced from the repo by hand (scp) before every build — do not assume they match the repo. |
| `ling@192.168.50.204:~/src/Lilu` | Lilu source tree the build requires. |
| Target machine ESP | `EFI/OC/Kexts/NvmeHook.kext` + `EFI/OC/config.plist`, on the internal SATA disk's EFI — the only ESP that may be written; the other EFI partitions belong to USB media. **Its device identifier moves when NVMe hardware changes** (it was `disk2s1` with the AN1500 attached and `disk1s1` with the Optane alone), so resolve it by identity every time: `diskutil info` must say `Volume Name: EFI` and `Device Location: Internal`. |

SSH access from the Windows side: `ssh -i C:\Users\ling\.ssh\id_rsa
ling@192.168.50.204` (key auth, no password). sudo on that box is passwordless.
The Debian rescue system answers on the same address with a different host key;
reach it with `-o "HostKeyAlias=oc-rescue-usb"` and the rescue `known_hosts`.

The deployed binary is built from the repo sources at version 1.3.0. `README.md`
and `DEBUG.md` describe the current driver; see §5.

The suppressed devices are **not** in the source: they are listed in the kext's
`Info.plist` (`IOKitPersonalities > com.ling.NvmeHook > NvmeHookDevices`), so the
existing bundle on the ESP can be edited and rebooted without a rebuild. Several
rules are allowed, `*` means "any value", and there is no boot argument that
changes matching — loading the plugin applies the rules. `-nvmehookoff` stays as
the Lilu-level switch that prevents loading.

## 2. What is deployed and what is verified

Deployed: **1.3.0**, anchor-resolved (§4), with the rules read from `Info.plist`.
It suppresses discards for every device matching a rule and counts everything.
The target is the only NVMe controller present:

```
NvmeHookVersion   = 1.3.0
NvmeHookPatches   = discard suppression for the configured devices, anchor-resolved (M2)
NvmeHookConfig    = state=ok; identities=1; [0] model=INTEL SSDPD21K750GAM serial=*
NvmeHookHooks     = armed by anchor: doUnmap + counters + InitializeController
NvmeHookAnchor    = i=0 vptr=0xffffff801baeb020 doUnmap=0xffffff801bad0396
                    doUnmapNVMe=0xffffff801bad016c init=0xffffff801bac290c fingerprint=match
NvmeHookDevice0   = model=INTEL SSDPD21K750GAM; serial=PHKD9344003J750F;
                    match=[0] model=INTEL SSDPD21K750GAM serial=*
NvmeHookTotal     = seen=6; blocked=6; dsm=0
```

Stages, in the order they were run:

- **Stage B — control device, `-nvmehookcontrol`**: `blocked == seen` and `dsm`
  flat at zero across a 1.5 GB write-then-delete, unmount/remount, and three
  400 MB write/read-back/delete cycles; `diskutil verifyVolume` exits 0. The
  control device was a `WD_BLACK AN1500` (`WUBT21431655`).
- **Sleep/wake with blocking enabled, control device**: asleep 34 minutes,
  woken by the power button (`Wake from Normal Sleep … due to PWRB`), hooks
  still armed, 16 further discards blocked, `dsm` still zero, read-back and
  `verifyVolume` clean.
- **Stage C — bare target**: control device removed; `NvmeHookTarget` reports
  `matched target controller … discards are suppressed`, the namespace
  enumerates as a 750.2 GB `Microsoft Basic Data` partition, nothing is mounted,
  and every counter stays zero because no filesystem can issue a discard.
- **Stage D — target acceptance**: the Optane was erased to a single APFS volume
  (`NvmeHookTest`, 698 GiB) and soaked for about an hour: 12 cycles of 2 GB of
  random data written, synced, read back with a matching SHA-256 and deleted,
  plus three unmount/remount cycles, a sleep/wake (woken by the power button)
  and another 2 GB write/read-back after the wake. `diskutil verifyVolume` exits
  0 and the drive never dropped off the bus. Counters over the whole run:
  `seen=394; blocked=394; dsm=0` — every discard was swallowed, none became a
  Dataset Management command. Serial logging was not available for this run
  (§7), so the counters were polled from the host every 10 s instead
  (`_scratch/stageD/counters.log`), which also recorded the sleep window as five
  `UNREACHABLE` samples.

**The NanoKVM's USB keyboard does not wake this machine.** A keypress sent
through the KVM leaves it asleep and unreachable; the power button (gpio503,
~0.8 s) wakes it. `pmset -g log` reports the wake reason as `PWRB`. A sleeping
machine is therefore not reachable without KVM power control, and `pmset
sleepnow` takes effect about 30 s later, after `darkwakelinger` and the
bluetoothd assertion expire.

## 3. Device facts (from `AGENTS.md`, restated because they decide every judgement)

- Target (faulty, **not attached**): Intel Optane `INTEL SSDPD21K750GAM`, serial
  `PHKD9344003J750F`, 750 GB. Its firmware drops off the bus on TRIM.
- Control (healthy, attached): `WD_BLACK AN1500`, serial `WUBT21431655`. It is
  the only `IONVMeController` currently published on the machine
  (`NvmeHookInstanceCount = 1`).
- Everything about a Samsung 990 EVO / "DirectLinkSupport" that may linger in
  old notes or agent memories is wrong for this project and has been removed
  from the driver. The identity match is model **and** serial
  (`activeIdentity` in `NvmeHook.cpp`).
- Note: `IONVMeController` publishes `Model Number` / `Serial Number` but no
  `Vendor Identifier` (an earlier probe read 0x0). Do not match on vendor.

## 4. Resolving the hooks on a macOS 26 kernel collection

Read this before changing anything in `workerThread` (`NvmeHook.cpp`).

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
   collection and the OpenCore-injected slices).
3. **`__ZTI16IONVMeController` does not exist** in the collection's symbol
   table (type-info symbols are stripped); `__ZTV16IONVMeController` does.
   Any validation built on the type-info symbol cannot work.
4. **`kextstat`/`kmutil showloaded` report link addresses, not runtime
   addresses.** Comparing them against runtime pointers misleads; KASLR is
   active and the slide was measured at 0x11000000–0x1f600000 (~272–502 MB)
   across boots. Any sanity bound on the slide below ~1 GB silently rejects
   every real boot.
5. **Reading the collection file from the worker thread panics.** Version 1.1.0
   resolved symbol names by reading `BootKernelExtensions.kc` with
   `vnode_lookup` + `vn_rdwr` from its own `kernel_thread_start` thread. Most
   boots worked; one panicked with

   ```
   panic: Taking non-sleepable RW lock with preemption enabled @lock_rw_old.c
   _IORWLockRead <- _namei + 0x7d8 <- _vnode_lookupat <- workerThread + 0x54b
   ```

   A bare kernel thread has no VFS context, and `namei` is not callable from
   one. No file system access from the worker thread, ever.

The working design (`workerThread`):

1. A worker thread (started from Lilu's `patchersReady` callback) blocks in
   `IOService::waitForMatchingService` for an `IONVMeController`. This must stay
   a **blocking wait on a dedicated kernel thread** — `DEBUG.md` documents that
   the alternative (an IOKit matching notification registered in `start()`)
   hangs the machine at boot. Do not reintroduce it.
2. The instance's first word is its vptr, i.e. `__ZTV16IONVMeController + 16`
   plus the slide. Every hook is a fixed link-time distance from that slot:
   `hook = vptr + (hook link address - (__ZTV link address + 16))`, so the slide
   is never computed and nothing is read from disk. The distances are `kDelta*`
   in `NvmeHook.cpp` (−0x1AC8A, −0x1AEB4, −0x28714).
3. Each candidate is fingerprinted before routing: the first 24 code bytes must
   equal `kFingerprint*`, measured on this build. Twenty-four and not sixteen,
   because the two block-device prologues share their first sixteen bytes.
4. Before the fingerprint read, each candidate must be within 4 MB of the
   vtable's first slot, which is a pointer into the same image, so a foreign
   subclass vtable is skipped instead of read into an unmapped gap.
5. The first instance that fingerprints cleanly becomes the anchor; only then
   does `routeFunction` run. A mismatch publishes
   `NvmeHookAnchor … fingerprint=mismatch` and patches nothing.

The constants belong to 25G332 (macOS 26.7.2). Re-measure them with
`vt_slots.py` / `fn_bytes.py` in the `ida/` analysis directory after any
IONVMeFamily change; a stale set fails the fingerprints, which is visible and
safe, but it also means no protection.

`PLAN.md` §4's deallocate-carrier audit is closed on static evidence from this
collection (`DEBUG.md` has the table): Write Zeroes 0x08 has no builder and no
method at all, Format NVM 0x80 has a builder with zero cross-references and
`doFormatMedia` is a stub returning `kIOReturnUnsupported`, so Dataset Management
0x09 is the only carrier macOS can emit — and the block sits above its only
builder. No second block is needed. Everything else in §4 is in place: the
short-circuit is per device (identity read from the owning block storage
device), the bias to over-block holds (a matched device loses every discard,
whatever its size), and `-nvmehookoff` still gates loading through Lilu, with
`-nvmehooknoblock` as the armed-but-pass-through configuration.

## 5. Documentation state

`README.md` states the M2 capability, the boot-args and the counters; `DEBUG.md`
carries the current shape (three hooks on the worker thread, the block decision
at `doUnmap` and why it is there, where the identity is read, the full property
table, the symbol-resolution design, deploy and recovery). Both were brought in
line with M2. Keep them state documents per `AGENTS.md`: no round-by-round
history, no commit pinning.

## 6. Build, deploy, verify

Build (on the Mac, in `~/src/NvmeHook`, after syncing the two files from the
repo):

```sh
sh Scripts/build-noxcode.sh
# binary: build/noxcode.*/NvmeHook.kext/Contents/MacOS/NvmeHook
```

The version lives in `VERSION=` inside `Scripts/build-noxcode.sh` (1.1.0).
`Info.plist` must stay LF-only or the kext is silently dropped.

Deploying only the Mach-O leaves the ESP bundle's own `Contents/Info.plist` at
whatever version it was built with, which is the version `kextstat` prints. Keep
its `CFBundleVersion`, `CFBundleShortVersionString` and
`OSBundleCompatibleVersion` in step with `VERSION=`, and back the plist up
before touching it.

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
(`config.plist.bak-m1-probe`, `bak-m15-deploy`, `bak-m15-panic`,
`bak-m2-stageB-*`).

Boot-args live in `NVRAM > Add > 7C436110-AB2A-4BBB-A880-FE41995C9F82 >
boot-args`. The M2 verification run carried `-nvmehookcontrol`; the token has
since been removed for Stage C (backup `config.plist.bak-stageC-*`), so the
deployed configuration matches the target identity again and no token needs to be
edited before the bare-target boot.

Verify after boot:

```sh
kextstat | grep -iE 'nvmehook|IONVMeFamily'
ioreg -n IOResources -l -w0 | grep -E 'NvmeHook(Version|Blocking|Hooks|Total)'
ioreg -r -c IONVMeBlockStorageDevice -l -w0 | grep NvmeHookCounters
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
input", the web UI holds manual control — stop and ask. A sleeping machine has
no video signal at all, so screenshot capture fails with `retCode -4`; that is
sleep, not a fault (§2).

**Serial debugging is not usable on this machine at present.** With
`debug=0x108` plus `serial=5`, two consecutive boots stalled for more than ten
minutes in early kext loading and never reached the desktop; the capture stops
around `IOKitCrashQuiz1::init` with no panic, while the driver had already
reported `hooks: armed by anchor`. The serial link is therefore left off
(`debug=0x100`, no `serial=5`), and acceptance evidence comes from the host
polling the counters every 10 s into `_scratch/stageD/counters.log`, which keeps
the last values and a timestamp if the machine dies.

## 8. Where the test plan stands

Stages A, B, C and D are done (§2). The driver suppresses TRIM on the target and
the target survived the workload that used to kill it: an hour of write/verify/
delete cycles, unmount/remount, sleep/wake, and `verifyVolume` clean at the end.
Two deviations from the original sketch are worth remembering:

- The block is taken in `doUnmap`, not in `doUnmapNVMe`. At `doUnmapNVMe` the
  request has already been taken from the pool and a memory descriptor attached;
  at entry to `doUnmap` nothing has been acquired, so the short-circuit is a
  complete no-op. `DEBUG.md` has the reasoning; `doUnmapNVMe` remains the
  ground-truth counter.
- The acceptance run had no serial logging, because enabling it now makes the
  machine hang during boot (§7). The counters were polled from the host instead.

What is left is upkeep rather than testing:

1. `kDelta*` / `kFingerprint*` are tied to 25G332. After any IONVMeFamily update,
   re-measure them (§4) and confirm `NvmeHookAnchor` still reads
   `fingerprint=match`; a mismatch means the driver is inert, not that it is
   working.
2. The shipped rule is `INTEL SSDPD21K750GAM` / `*`, i.e. **model only**: any
   further drive of that model would be suppressed too, healthy or not. Put the
   serial in place of the `*` to pin the one sample, and add dicts to the array
   for more devices. After editing `NvmeHookDevices` on the ESP, confirm
   `NvmeHookConfig` lists what was intended — a malformed list leaves the driver
   loaded but matching nothing.
3. The target now carries an empty APFS volume `NvmeHookTest` (750 GB container).
   It can be kept, renamed or erased as ordinary storage.
4. `-nvmehookoff` remains the kill switch and must stay reachable; the recovery
   procedure in §7 works whether or not the driver is at fault.
