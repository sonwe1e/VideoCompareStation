# Third-Party Notices

CompareStation distributes third-party runtime components under their respective
licenses. The package includes the exact license or copyright texts in this directory;
those texts govern the corresponding components.

## Dynamically linked FFmpeg libraries

The application links the vcpkg FFmpeg 8.1.2 libraries built without optional GPL codec
features. Their copyright and license text is installed as `vcpkg/ffmpeg.txt`. The end-user
package deliberately contains neither `ffmpeg.exe` nor `ffprobe.exe`; media probing and decoding
use the linked libraries directly.

## Qt

The application distributes Qt 6.11.1 runtime libraries, plugins, and QML modules. The
corresponding Qt module license and copyright texts are installed under `vcpkg/` from
the pinned vcpkg install tree.

## Supporting libraries

The package also contains runtime components from double-conversion, md4c, PCRE2, and
zlib. Their exact copyright and license texts are installed under `vcpkg/`.

Microsoft Visual C++ runtime files are redistributed under the Microsoft Visual Studio
redistribution terms.
