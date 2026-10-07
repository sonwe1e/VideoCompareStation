# Clip export: preserve the destination when publication fails

Date: 2026-10-06. Baseline: draft PR #34 head
`d44a682c8aff0ea93ab4d44070e1bd686271d7dd`.
Status: focused cloud protocol verification complete; native Windows acceptance pending.

## Reproduction and scope

The baseline tries `rename(work, target)` and, if it fails, removes the destination before
retrying the rename. An independent FFmpeg 7.1.5 run with both rename calls returning EACCES
reports failure after deleting the existing successful clip. The old AVIO destructor also
discards close errors, so buffered-write failures can be published as successful output.

The correction uses the existing `platform::AtomicFilePublisher`; it does not introduce a
second replacement algorithm or a production POSIX implementation. Export range planning,
keyframe discovery, packet/timestamp handling, faststart and the UI are unchanged.

## Lifecycle and ownership

1. Capture whether the target exists before beginning the transaction. A target created later
   by somebody else must make `publishNew()` fail; it is never silently replaced
2. The existing publisher allocates a unique, same-directory `CREATE_NEW` temporary. Its name
   includes the operation, request revision, process and sequence, so repeated request IDs do
   not share staging files or remove unrelated legacy `.partial.mp4` files
3. The new narrow `prepareForExternalWrite()` closes the publisher's exclusive handle. The
   caller exclusively owns the transaction and must close its external writer before `flush()`.
   The publisher retains cleanup ownership throughout; this is not an adversarial-path guarantee
4. FFmpeg writes that temporary. The container is selected from the requested destination's
   extension, while the muxer's actual URL remains the temporary for MP4 faststart reopening
5. Check trailer and AVIO close results. The close clears `pb`, preventing a second hidden close.
   Reopen the existing temporary exclusively, flush it and close it through the publisher.
   `OPEN_EXISTING` cannot recreate a missing output as an empty successful file
6. Check cancellation after AVIO close and again after flush. Publication is the commit point:
   cancellation before it preserves the old target; cancellation after success cannot undo it
7. Call the existing new-file move or replacement/backup logic. There is no delete-target
   fallback. Ordinary failure/cancellation cleans owned staging files when filesystem cleanup
   is available. Existing helper callers retain their original write/flush behavior

The canonical helper already handles Windows error 1177 by restoring the backup without
replacing a concurrently recreated destination. If restoration fails, both original backup
and replacement are intentionally retained and their full paths are returned in the diagnostic.
These are recovery copies, not successful output. Recovery diagnostics now use UTF-8 so a
non-ASCII path does not depend on the Windows narrow code page.

This is failure recovery, not a guarantee against power loss. Windows ReplaceFile does not
support its nominal write-through flag; OS/storage durability is not established by these tests.

## Verification evidence

- Baseline data loss reproduced with real FFmpeg and failed rename calls
- 33 focused GoogleTests pass: the previous 13 planner/origin/final-keyframe tests, existing
  publisher regressions, and new publication/handoff cases
- New cases exercise new and existing destinations; handoff close, AVIO close, flush and final
  close failures; failed replacement/new publication; cancellation before work, during copy,
  after AVIO close and after flush; a concurrently created target; partial-replacement
  restoration; retained recovery copies; repeated request IDs and unrelated-file preservation
- An independent driver passes 21 scenarios and 139 checked conditions, including actual
  buffered AVIO error injection, trailer failure, reopen failure, nested identical request IDs
  and a concurrently recreated destination during rollback. Successful exports retain identical
  compressed packets and a faststart `moov` before `mdat`
- 28/28 compiled production mutations are detected by runtime assertions, covering all 52
  executable new assertion sites, including setup/state guards. Compile errors are not counted.
  The mutation runner checks its expected mutation count; evidence is under
  `out/verification/publication/`. Final source bytes are kept separate from all mutants

These are Linux/GCC14 builds with real FFmpeg 7.1.5 and GoogleTest 1.17.0. The production writer
is unmodified for the build. The actual canonical publisher implementation is compiled with
only its unsupported-platform guard removed in a copied source, against a test-only Win32 API
model implemented using POSIX calls. The model substitutes `WindowsPaths::absolutePath` and
native path character types. It executes the real state transitions and recovery decisions;
it does **not** validate Win32 ABI, file sharing, Unicode paths, atomic filesystem semantics,
NTFS behavior or storage durability. The model is not shipped or added to the production build.

Four permanent Windows registrations are compiled but not run here: two Unicode variants,
an externally held temporary, and a locked existing destination. Their assertions are not
counted as cloud runtime or mutation coverage. The ASCII recovery/name variants do run.

## I/O cost and remaining acceptance

There is no extra full-clip copy. The correction adds a temporary-handle handoff, reopen,
explicit flush/close and the existing backup/replacement operation. Flush can add latency,
especially on slow disks. Cloud timing is only a bounded sanity check, not Windows performance
acceptance; native filesystem behavior remains the key outstanding check. Seven runs of full
exports from small synthetic 5/10-minute MP4s (1.48/2.95 MB; 9,000/18,000 packets) give median
baseline → corrected times of 11.36 → 11.67 ms and 21.83 → 22.03 ms. The corrected run uses
real POSIX fsync through the model; it cannot predict Windows/NTFS or high-bitrate disk costs.

Run the supported Windows wrapper against the exact final source:

```powershell
pwsh tools/build/build.ps1 -Preset dev -Test -TestRegex '^(platform.AtomicFilePublisherTests|media.(ClipExportPublicationTests|NewAndExisting/ClipExportPublicationTests|AsciiAndUnicode/ClipExportPathTests|ClipExportWriterTests|FirstMiddleLast/ClipExportOriginTests)|application.ClipExportPlannerTests)'
pwsh tools/build/build.ps1 -Preset dev -Target format-check
pwsh tools/build/build.ps1 -Preset dev -Target lint
```

Verify the selected test count is nonzero and includes both Unicode cases and both real lock
cases. On Windows/NTFS, keep an old destination locked while exporting, confirm failure with
unchanged bytes and no ordinary staging leak, then release the lock and repeat successfully.
Check failure diagnostics can identify retained Unicode recovery paths.

Windows/MSVC, pinned FFmpeg 8.1.2/Qt 6.11.1, full CTest, format/lint targets, real GUI,
GPU/performance gates and packaging were **not run**. No workflow changes/reruns, force push,
merge or deployment are included. The PR stays draft; skipped CI is not passed CI.
