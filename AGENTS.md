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
