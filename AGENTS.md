# Repository conventions

## Documentation: state, not a log

`README.md` is the user-facing overview: purpose, supported capabilities,
configuration, build and installation entry points. Put implementation details,
capacity and memory accounting, synchronization, ABI formats, IORegistry
properties and debugging procedures in `DEBUG.md`, and link to them from the
README instead of duplicating them there.

`README.md` is state documents. They say what is true of this driver now,
not what happened round by round.

- **State only.** State facts that hold at the time of writing.
  Do not record "round N passed / round N failed". Once a defect is fixed
  its failure history is dropped, because the reader needs to know whether
  the combination works now.
- **Attribution decides inclusion.** A failure that follows the driver must be recorded;
  it is a defect in this driver. A failure that follows one physical sample — the same
  hardware error under every driver, every firmware revision and every cold boot — is a
  property of that sample: it is not listed, and it must not be used to excuse a
  driver defect.
- **No version or commit pinning.** The header carries a date and the driver
  version only. Do not write `at commit <hash>` or round-specific report
  filenames into the body or the sources: the hash goes stale immediately, and
  the round names turn a state document back into a log.

## Deployment: the ESP and nothing else

Lilu and this plugin are installed on the ESP only. Neither is ever written to
`/Library/Extensions` or any other system volume, however much shorter the load
path would be — a Lilu plugin kept on the system volume is not an option here.

- The active ESP is the **internal SATA disk's** EFI partition. Confirm it before
  writing: `diskutil info` shows `Device Location: Internal` for the whole disk.
- Never write OpenCore files to a USB disk's EFI. The USB sticks in this machine
  are install and rescue media, not boot configuration.
- The plugin is injected from `EFI/OC/Kexts` through a `Kernel > Add` entry placed
  after `Lilu.kext`. Back up `config.plist` next to itself before editing,
  `plutil -lint` the result, and verify that the only difference from the backup
  is the entry that was added.

## Test devices: only the named sample is faulty

Only the 750 GB Optane (`INTEL SSDPD21K750GAM`, serial `PHKD9344003J750F`) is
known to lose its firmware on TRIM. Do not extend that suspicion to other
hardware in the machine:

- The 32 GB array (`WD_BLACK AN1500`, two 16 GB Optane M10 modules in a set) is
  healthy: formatted APFS, read and written normally, TRIM enabled, no fault. It
  is the control device for testing, not a suspect.
- A failure of the driver on a healthy device is a driver defect. A failure of the
  target sample under any driver is a property of that sample.

