# Packaging

CompareStation ships as a Windows x64 ZIP built from the CMake install tree. Packaging remains
fail-closed unless the pinned FFmpeg runtime, Qt deployment rules, license texts and third-party
notices agree with the reviewed manifests.

The package carries no MSI: the installer cost more to maintain than it returned, and everything it
did that users noticed is either unnecessary for an unpacked build (Start menu entry) or handled by
`tools/shell/RegisterExplorerCommand.ps1`, which registers the Explorer "Compare with
CompareStation" command per user under `HKCU\Software\Classes` and needs no administrator rights.

Do not copy DLLs manually into a release directory. Add every runtime component through
`cmake/Install.cmake`, then validate the staged layout and packaged startup check before
publishing a release.
