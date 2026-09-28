# Packaging

CompareStation ships as a Windows x64 ZIP built from the CMake install tree. Packaging remains
fail-closed unless the pinned FFmpeg runtime, Qt deployment rules, license texts and third-party
notices agree with the reviewed manifests.

The package carries no MSI: the installer cost more to maintain than it returned, and everything it
did that users noticed is either unnecessary for an unpacked build (Start menu entry) or done by the
application itself. On startup CompareStation registers the Explorer "Compare with CompareStation"
command per user under `HKCU\Software\Classes`, needs no administrator rights, and repairs the keys
when the unpacked directory moves, so extracting the ZIP is the whole installation.
`HKCU\Software\CompareStation\ExplorerContextMenu` records the user's own decision: a fresh extract
leaves it absent, which means enabled; `RegisterExplorerCommand.ps1 -Uninstall` sets it to 0 so a
later launch does not put the entry back; `DVS_DISABLE_SHELL_REGISTRATION=1` skips the check for one
run. `RegisterCompareStationContextMenu.cmd` remains in the package for users who never open a
terminal, and `RegisterExplorerCommand.ps1` remains for unattended installs. Keep the `.cmd` ASCII
only, because cmd.exe parses a `.cmd` in the console OEM code page and mangles any other text.

Do not copy DLLs manually into a release directory. Add every runtime component through
`cmake/Install.cmake`, then validate the staged layout and packaged startup check before
publishing a release.
