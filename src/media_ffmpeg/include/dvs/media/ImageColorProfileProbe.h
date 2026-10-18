#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace dvs::media {

// Extracts the embedded ICC profile payload from an encoded still image without decoding any
// pixels. The current FFmpeg build has zlib disabled, so its PNG decoder is absent and PNG
// files are decoded by the Qt fallback; this container-level probe is what keeps the colour
// state meaningful on that real path. Supported containers:
//   PNG  - iCCP chunk (profile is zlib-compressed)
//   JPEG - APP2 "ICC_PROFILE" segments, concatenated by sequence number
//   WebP - ICCP chunk (uncompressed)
//   TIFF - tag 34675 (uncompressed)
//
// Return value: true when an ICC payload was extracted. false means no profile was found OR
// the container declared one that could not be extracted; `profileDeclared` distinguishes the
// two, so a caller can report a corrupt profile instead of "no profile". The bytes are not
// validated as ICC here: classifyImageColorProfile() owns structural classification.
[[nodiscard]] bool extractEmbeddedIccProfile(const std::uint8_t* data,
                                             std::size_t size,
                                             std::vector<std::uint8_t>* profile,
                                             bool* profileDeclared,
                                             std::string* error);

} // namespace dvs::media
