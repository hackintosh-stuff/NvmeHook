# NvmeHook — implementation plan

Date: 2026-11-09. Driver version: 0.1.0 (planned).

A Lilu plugin that suppresses NVMe TRIM (Dataset Management / deallocate) for
specific devices. Target device: an Intel Optane whose firmware drops off the bus
when it receives TRIM, taking the system with it.

## 1. Goal

- No `discard` request from any macOS layer ever reaches the target device as an
  NVMe command.
- Every other NVMe device keeps TRIM exactly as it is today.
- The suppression is provable, not assumed: the driver counts requests it
  swallowed and the commands that actually left for each device.

Out of scope: making the target device fast, fixing its firmware, global TRIM
policy for healthy devices.

## 2. Environment observed on the target machine

| Item | Value |
|---|---|
| macOS | 26.7.2, x86_64 |
| `IONVMeFamily` | 2.1.0 |
| Lilu | 1.7.2, loaded |
| Lilu delivery | OpenCore injection — not present in `/Library/Extensions` |
| Another Lilu plugin already loaded | `org.maz1.AMDMapperHook` 1.0.0 |
| Boot volume | `disk2`, APFS |
| boot-args | `-v keepsyms=1 debug=0x100 msgbuf=1048576 npci=0x2000 …` (no `serial=5`, no `0x108`) |

Devices known to this project:

| Role | Model | Serial | Size | Notes |
|---|---|---|---|---|
| Target (buggy) | `INTEL SSDPD21K750GAM` | `PHKD9344003J750F` | 750 GB | macOS reports `TRIM Support: Yes`. Not attached during development. |
| Control (healthy) | `WD_BLACK AN1500` | `WUBT21431655` | 28.67 GB | Two 16 GB Optane M10 modules in a RAID set on the card. Formatted APFS, read and write verified, TRIM enabled, no fault. |

Only the 750 GB sample is faulty. The control is healthy by direct observation,
not by assumption: it is formatted, written to, and keeps working with TRIM on.
Being the same media family makes it a closer control than an arbitrary drive,
which is why it is the one to use for stages A and B.

## 3. Where to intercept

The request path to cover is `IOStorage::discard` (filesystem / APFS) →
`IOBlockStorageDriver` → NVMe block device → Dataset Management submission.

Candidate interception points, in order of preference:

1. **The `IONVMeFamily` entry point that turns a storage discard into a DSM
   command.** A Lilu code patch routes it to our hook. This is where all
   discard requests converge, so no upper layer can be missed.
2. Alternative: a patch one level lower, on the command submission path, which
   also gives the ground-truth counter for section 5.
3. Fallback if the family patch proves fragile (inlined, per-queue, or
   unpatched on this build): swallow discard above the family, in the block
   storage layer.

Choosing between 1 and 3 is a measurement, not a preference: resolve the symbols
in the installed `IONVMeFamily` and confirm which function runs when a discard
is actually issued on the control device. Blocking at a point that is not on the
real path is the single most expensive mistake available here — it looks like
success until the target device is attached.

## 4. Design constraints

- **Per-device, not global.** The patch is global (one code site serves every
  controller), so the hook reads the owning instance identity on every call and
  short-circuits only for matched devices. Everything else goes to the original
  implementation through a trampoline, unchanged.
- **Match on identity, log the match.** Model + serial from
  `IONVMeController` registry properties. A model-only predicate would hit every
  other drive of the same type, including healthy ones bought later.
- **Bias to over-block.** On the target device, a false positive (blocking
  something that did not need blocking) costs performance; a false negative
  costs the drive. No exceptions, no range heuristics, no "only large requests".
- **Armed before anything can mount.** The hook is installed synchronously in
  plugin init. A late arm is indistinguishable from a missing one.
- **Audit the other deallocate carriers.** TRIM is opcode 0x09, but Write Zeroes
  0x08 carries a deallocate bit and Format NVM 0x80 has a deallocate/secure
  erase setting. Determine empirically which of these macOS can send, then decide
  whether they belong behind the same block.
- **Kill switch.** A boot-arg disables all patching so a broken driver cannot
  make the machine unbootable. Note that the kill switch, used with the target
  attached, means no protection at all.

## 5. Self-verification: two counters

The driver keeps, per controller:

- `seen` — discard requests entering the filter;
- `blocked` — of those, the ones short-circuited;
- `dsm …` — Dataset Management commands that actually left for that device.

The lower counter comes from a read-only instrumentation of the submission path:
it counts, it never changes behaviour. That turns "did the block work" into a
comparison instead of an inference:

- block off: `dsm` rises together with `seen`;
- block on: `seen` rises, `blocked` equals `seen`, `dsm` stays flat.

This is the only way to prove suppression on a healthy device, and it is the
reason the control device matters at all.

Counters are readable as an `IONVMeController` registry property and mirrored to
`IOLog` with the device name and serial on the first event.

## 6. Test plan

The failure is already characterised at device level: any TRIM to this Optane
kills it. No protocol-level reproduction on another OS is planned. What remains
untested is the driver, so the order below spends the irreplaceable sample last.

### Stage A — control device, pass-through

Hook installed, blocking disabled. Generate real discard traffic (large deletes,
unmount/remount, `diskutil` operations). Expect `seen` and `dsm` to rise
together.

Gate: counters move; no regression in normal use; survives sleep/wake; the
volume is intact.

### Stage B — control device, blocking enabled

Temporarily match the control device's own serial. Expect `blocked == seen` and
`dsm` flat while the filesystem keeps working.

Gate: proof that suppression actually reaches the hardware, plus sleep/wake and
a soak with deletes. This is the gate that allows the target to be plugged in.

### Stage C — target device, bare

Attach the Optane with no filesystem on it and do not mount it. Verify the
identity match fires, the namespace enumerates, and the log records the match
with the right serial, while `seen` stays 0 because nothing can issue a discard.

Gate: match is correct for the target and does not fire for the control device.

### Stage D — target device, acceptance

Create a volume, mount it, and run the workload that would previously have
killed it: bulk delete, unmount/remount, sleep/wake, one to two hours of soak.
APFS issues discard for freed extents in the background, not only at mount, so
an immediate "still alive" is not a pass.

Serial logging (`debug=0x108` plus `serial=5`) is enabled for this run only,
because a firmware drop is a hard hang and `IOLog` will not reach disk. Turned
off immediately afterwards, per the workspace rule.

Gate: `dsm` stays 0 for the target over the whole soak, no firmware drop, data
written before the soak reads back intact.

## 7. Build and deploy

- Build: `clang` from the Command Line Tools plus the `MacKernelSDK` submodule
  and a Lilu source tree, driven by `Scripts/build-noxcode.sh`. No Xcode project;
  the build machine has only the CLT. The script also compiles Lilu's
  `Library/plugin_start.cpp`, which a plugin cannot do without: a released
  Lilu.kext ships only the headers.
- **Lilu and this plugin live on the ESP only.** Nothing of either is written to
  `/Library/Extensions` or any other system volume, no matter how convenient the
  load path would be. Both are injected by OpenCore from `EFI/OC/Kexts` through a
  `Kernel > Add` entry, which is how Lilu and `AMDMapperHook` already arrive on
  this machine.
- This is also the only route that works. `/Library/Extensions` fails at
  collection build time because `kmutil` resolves dependencies against kexts on
  disk and Lilu is not one of them:
  `Failed to bind '_lilu' as could not find a kext with 'as.vit9696.Lilu' bundle-id`.
- Deploying therefore costs one reboot per iteration: build, copy the kext into
  the ESP, edit `Kernel > Add`, back up `config.plist` first, validate the plist,
  unmount, reboot. Changes are batched for that reason.
- The usual workspace pitfall still applies: the `Info.plist` must be LF-only or
  the kext is silently dropped.
- Every deploy keeps a known-good config backup and a bootable rescue USB within
  reach; a bad patch on a machine that cannot boot is the realistic worst case.

## 8. Milestones

| # | Deliverable | Done when |
|---|---|---|
| M0 | Repo skeleton, build script, empty plugin | Loads on the control machine, patches nothing, logs its version |
| M1 | Interception point identified, counters only | `seen`/`dsm` move together under real discard traffic |
| M2 | Blocking works on the control device | `dsm` flat while `blocked == seen`, system stable |
| M3 | Device matching verified on the bare target | Match fires for target, not for control |
| M4 | Target acceptance | Stage D gate met |
| M5 | Docs | README states purpose/build/install; DEBUG carries interception details, ABI, counters, debugging |

## 9. Risks

| Risk | Mitigation |
|---|---|
| Patch is off the real path → silent pass, then a dead drive | Stage A/B gates measure the hardware-side counter before the target is ever attached |
| False negative from a too-narrow predicate | Serial+model match, match logged at attach, no range heuristics |
| Global patch regresses healthy NVMe | Unmatched calls go through the trampoline; control device is the regression test |
| Hang destroys the evidence | Serial console on for the acceptance run only |
| Firmware drop bricks the sample | Sample is treated as sacrificial; acceptance is the last step |
| Deploy mistake makes the machine unbootable | Kill-switch boot-arg, config backup, rescue USB |

## 10. Open questions

- What is actually inside the `WD_BLACK AN1500`? The volume is named `Optane_32G`,
  which suggests an Optane module behind the card's RAID controller, i.e. not a
  plain healthy NVMe. Confirm before treating it as the control device, because
  a control device that shares the target's flaw proves nothing.
- Is the 750 GB sample removable at will, and is there a second one?
- Deploy path for iteration: `/Library/Extensions` or OpenCore injection?
