# Repository Tools

PowerShell automation is grouped by responsibility:

- `ci/`: wrappers used by continuous integration jobs.
- `quality/`: repository policy, resource-limit, and coverage checks.
- `testing/`: fixture generation and playback/performance gate entry points. The playback gates
  share `PlaybackTraceGate.psm1`; its trace check validates schema-v1 structure, numeric types, and
  ranges, but is not a semantic invariant analyzer. `CommandLineArgument.psm1` owns the single
  CRT/`CommandLineToArgvW` command-line quoting implementation used by the playback gates, the
  performance gate, and the popup-pixel smoke script, so quoting fixes land in one place. See
  [`docs/engineering/maintenance-and-performance.md`](../docs/engineering/maintenance-and-performance.md).
  `generate-performance-diff-fixture.ps1` derives the external 1080p60 Diff B fixture from A with
  a deterministic visible patch and writes a same-basename `.contract.json` hash contract. Its
  FFmpeg operations are timeout-bounded, and it publishes the video/contract pair with rollback so
  a failed replacement preserves the previous usable pair. The hardware gate consumes the default
  `gate-1080p60-diff-b` pair.
- `release/`: release identity and validation operations. Releases are currently unsigned
  by policy; signing tooling stays out of the tree until a signed release is actually planned.

Run scripts from the repository root using their full path. Component-local compiled utilities,
such as the HLSL header compiler, remain beside their owning component under `src/`.
