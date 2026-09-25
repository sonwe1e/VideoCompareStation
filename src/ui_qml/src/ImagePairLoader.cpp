#include "dvs/ui/ImagePairLoader.h"

#include "ByteLruCache.h"
#include "ImageHeaderProbe.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRunnable>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <exception>
#include <map>
#include <memory>
#include <optional>
#include <utility>
namespace dvs::ui {
namespace {
constexpr int kMaximumImageEdge = 8192;
constexpr qint64 kMaximumDecodedBytes = 128LL * 1024LL * 1024LL;
constexpr qint64 kMaximumEncodedBytes = 512LL * 1024LL * 1024LL;
class FunctionRunnable final : public QRunnable {
public:
    explicit FunctionRunnable(std::function<void()> function) : function_(std::move(function)) {
        setAutoDelete(true);
    }
    void run() override {
        function_();
    }

private:
    std::function<void()> function_;
};
[[nodiscard]] QString sourceLabel(const QUrl& url) {
    return url.isLocalFile() ? url.toLocalFile() : url.toString();
}
[[nodiscard]] QString fileIdentity(const QUrl& url, const std::uint64_t policyRevision) {
    const QString label = sourceLabel(url);
    if (label.isEmpty()) {
        return {};
    }
    const QFileInfo info{label};
    return QStringLiteral("file:%1|size:%2|mtime:%3|policy:%4")
        .arg(QDir::cleanPath(info.absoluteFilePath()))
        .arg(info.size())
        .arg(info.lastModified().toMSecsSinceEpoch())
        .arg(policyRevision);
}
[[nodiscard]] bool dimensionsWithinBudget(const int width, const int height, QString* error) {
    if (width <= 0 || height <= 0) {
        if (error) {
            *error = QObject::tr("Image dimensions are empty.");
        }
        return false;
    }
    if (width > kMaximumImageEdge || height > kMaximumImageEdge) {
        if (error) {
            *error = QObject::tr("Image is larger than %1 px on a side.").arg(kMaximumImageEdge);
        }
        return false;
    }
    const qint64 decodedBytes = static_cast<qint64>(width) * static_cast<qint64>(height) * 4LL;
    if (decodedBytes > kMaximumDecodedBytes) {
        if (error) {
            *error = QObject::tr("Image exceeds the decoded-image memory budget.");
        }
        return false;
    }
    return true;
}
} // namespace

ImageWorkingSetEstimate estimateSingleImageWorkingSet(const int width,
                                                      const int height,
                                                      const bool nativeSidecar) noexcept {
    ImageWorkingSetEstimate estimate;
    if (width <= 0 || height <= 0) {
        return estimate;
    }
    const qint64 pixels = static_cast<qint64>(width) * static_cast<qint64>(height);
    estimate.displayBytes = pixels * 4LL;
    estimate.nativeBytes = nativeSidecar ? pixels * 8LL : 0LL;
    estimate.totalBytes = estimate.displayBytes + estimate.nativeBytes;
    return estimate;
}

ImageWorkingSetEstimate estimatePairWorkingSet(const QSize primary,
                                               const QSize secondary,
                                               const bool nativeSidecars,
                                               const bool resample) noexcept {
    ImageWorkingSetEstimate estimate;
    if (primary.isEmpty() || secondary.isEmpty()) {
        return estimate;
    }
    const qint64 primaryPixels = static_cast<qint64>(primary.width()) * primary.height();
    const qint64 secondaryPixels = static_cast<qint64>(secondary.width()) * secondary.height();
    // The comparison target (the primary extent) carries the analysis field and the derived
    // difference image; the secondary contributes its own decoded buffer and sidecar.
    estimate.displayBytes = (primaryPixels + secondaryPixels) * 4LL;
    estimate.nativeBytes = nativeSidecars ? (primaryPixels + secondaryPixels) * 8LL : 0LL;
    estimate.analysisBytes = primaryPixels * 5LL; // delta field (4) + sign map (1)
    estimate.derivedBytes = primaryPixels * 4LL;
    estimate.resampleBytes = resample && primary != secondary ? primaryPixels * 4LL : 0LL;
    estimate.totalBytes = estimate.displayBytes + estimate.nativeBytes + estimate.analysisBytes +
                          estimate.derivedBytes + estimate.resampleBytes;
    return estimate;
}

namespace {
[[nodiscard]] bool loadImageFromDisk(const QUrl& url,
                                     const ImagePairLoader::DecodePolicy& policy,
                                     QImage* image,
                                     QImage* nativeImage,
                                     StillImageSourceInfo* info,
                                     QString* identity,
                                     QString* error,
                                     const std::atomic_bool& cancelled) {
    if (image == nullptr) {
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    const QString label = sourceLabel(url);
    if (label.isEmpty()) {
        if (error) {
            *error = QObject::tr("Invalid image path.");
        }
        return false;
    }
    if (identity != nullptr) {
        *identity = fileIdentity(url, policy.revision);
    }
    QFile file(label);
    if (!file.open(QIODevice::ReadOnly)) {
        if (error) {
            *error = QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName());
        }
        return false;
    }
    if (file.size() > kMaximumEncodedBytes) {
        if (error) {
            *error = QObject::tr("Image file is too large to read.");
        }
        return false;
    }
    const QByteArray bytes = file.readAll();
    if (bytes.isEmpty()) {
        if (error) {
            *error = QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName());
        }
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    QSize headerSize;
    bool haveHeader = false;
    if (policy.probe) {
        haveHeader = policy.probe(bytes, &headerSize);
    }
    if (!haveHeader) {
        haveHeader = probeImageHeader(bytes, &headerSize);
    }
    if (haveHeader) {
        QString dimensionError;
        if (!dimensionsWithinBudget(headerSize.width(), headerSize.height(), &dimensionError)) {
            if (error) {
                *error = dimensionError;
            }
            return false;
        }
    }
    if (cancelled.load()) {
        return false;
    }
    QImage decoded;
    QImage nativeDecoded;
    bool decodedOk = false;
    std::string decoderError;
    if (policy.loader) {
        try {
            decodedOk = policy.loader(bytes, &decoded, &nativeDecoded, info, &decoderError);
        } catch (const std::exception& exception) {
            decoderError = exception.what();
            decodedOk = false;
        } catch (...) {
            decoderError = "Still-image loader threw an unknown exception.";
            decodedOk = false;
        }
    }
    if (cancelled.load()) {
        return false;
    }
    if (!decodedOk) {
        decoded = QImage{};
        if (!decoded.loadFromData(bytes) || decoded.isNull()) {
            if (error) {
                *error =
                    decoderError.empty()
                        ? QObject::tr("Could not open image: %1").arg(QFileInfo(label).fileName())
                        : QString::fromStdString(decoderError);
            }
            return false;
        }
        // Qt's own loader gives no source provenance beyond the decoded format.
        if (info != nullptr) {
            info->sourceFormat = QStringLiteral("rgba8 (qt)");
            info->channels = 4;
            info->hasAlpha = decoded.hasAlphaChannel();
        }
    }
    if (decoded.isNull()) {
        if (error) {
            *error = QObject::tr("Decoded image is empty.");
        }
        return false;
    }
    QString dimensionError;
    if (!dimensionsWithinBudget(decoded.width(), decoded.height(), &dimensionError)) {
        if (error) {
            *error = dimensionError;
        }
        return false;
    }
    if (cancelled.load()) {
        return false;
    }
    if (decoded.format() != QImage::Format_RGBA8888) {
        decoded = decoded.convertToFormat(QImage::Format_RGBA8888);
    }
    if (decoded.isNull()) {
        if (error) {
            *error = QObject::tr("Decoded image conversion failed.");
        }
        return false;
    }
    if (info != nullptr) {
        // Only fall back to the buffer when the source provenance is unknown: a loader-
        // reported gray source (hasAlpha=false) must not be re-labeled alpha just because
        // the display buffer is RGBA8.
        if (info->sourceFormat.isEmpty()) {
            info->hasAlpha = decoded.hasAlphaChannel();
            info->channels = 4;
        }
        // Display conversion happened whenever the source bit depth, channel layout or
        // format differs from plain 8-bit RGBA; the provider fills the real source fields.
        info->displayConverted =
            info->bitDepth != 8 || info->channels != 4 ||
            (!info->sourceFormat.isEmpty() && info->sourceFormat != QStringLiteral("rgba"));
    }
    *image = std::move(decoded);
    if (nativeImage != nullptr) {
        *nativeImage = std::move(nativeDecoded);
    }
    return true;
}
[[nodiscard]] int clampChannel(const int value) noexcept {
    return std::clamp(value, 0, 255);
}
// Difference display helpers. They read the canonical per-pair analysis (exact per-channel
// magnitudes plus the delta sign map), so a mode or gain change never touches the sources again.
[[nodiscard]] QRgb
absDiffPixel(const int red, const int green, const int blue, const int gain) noexcept {
    return qRgb(clampChannel(red * gain), clampChannel(green * gain), clampChannel(blue * gain));
}
[[nodiscard]] QRgb signedDiffPixel(
    const int red, const int green, const int blue, const int signBits, const int gain) noexcept {
    // signBits bit0/bit1/bit2 record "secondary is brighter" for R/G/B, so the signed view keeps
    // showing which side is larger instead of only how far apart they are.
    const int redDelta = (signBits & 0x1) != 0 ? -red : red;
    const int greenDelta = (signBits & 0x2) != 0 ? -green : green;
    const int blueDelta = (signBits & 0x4) != 0 ? -blue : blue;
    return qRgb(clampChannel(128 + redDelta * gain),
                clampChannel(128 + greenDelta * gain),
                clampChannel(128 + blueDelta * gain));
}
[[nodiscard]] QRgb highlightPixel(const QRgb base, const int delta, const int gain) noexcept {
    if (delta <= 0) {
        return base | 0xff000000;
    }
    const int strength = clampChannel(delta * gain);
    const int red = clampChannel(qRed(base) + (255 - qRed(base)) * strength / 255);
    const int green = clampChannel(qGreen(base) + (40 - qGreen(base)) * strength / 255);
    const int blue = clampChannel(qBlue(base) + (90 - qBlue(base)) * strength / 255);
    return qRgb(red, green, blue);
}
// Alpha difference map: brightness encodes |alphaA - alphaB| × gain as a grayscale value.
// Both buffers carry straight (unassociated) alpha, so the subtraction is representation-safe.
[[nodiscard]] QRgb alphaDiffPixel(const int alphaMagnitude, const int gain) noexcept {
    const int delta = clampChannel(alphaMagnitude * gain);
    return qRgb(delta, delta, delta);
}
// CompareMode values of ImageReviewController, repeated here so the loader never includes the
// controller header (that header includes this one).
constexpr int kAbsDifferenceMode = 2;
constexpr int kSignedDifferenceMode = 3;
constexpr int kHighlightMode = 4;
constexpr int kAlphaDifferenceMode = 6;

// Canonical per-pair difference analysis. It is the expensive part, a full pass over both
// decoded display buffers and their high-depth sidecars; every display variant and every
// statistic is derived from it, so switching mode or gain never re-reads the sources.
struct DifferenceAnalysis final {
    QString key;
    QString error;
    // R/G/B = |delta| of the decoded RGBA8 samples, A = |alpha delta|. The magnitudes keep their
    // full 0-255 range; amplification happens only when a variant is rendered.
    QImage magnitudes;
    // One byte per pixel: bit0/bit1/bit2 say whether the secondary sample is brighter than the
    // primary one in R/G/B.
    QImage signs;
    // Primary display buffer (shared, not copied). The highlight variant blends over it.
    QImage base;
    bool resampled = false;
    int maxAbsDifference = 0;
    double meanAbsDifference = 0.0;
    int peakAlphaDifference = 0;
    double meanAlphaDifference = 0.0;
    qint64 alphaChangedPixels = 0;
    bool hasAlpha = false;
    bool nativeAvailable = false;
    int nativeMaxAbsDifference = 0;
    double nativeMeanAbsDifference = 0.0;
    int nativePeakAlphaDifference = 0;
    qint64 nativeChangedPixels = 0;
    qint64 nativeBeyondDisplayPixels = 0;
    qint64 bytes = 0;
};

struct DifferenceComputation final {
    ImagePairLoader::DifferenceResult result;
    // Set only when the analysis was newly computed; a reused analysis stays in the owner's
    // single-slot cache untouched.
    std::optional<DifferenceAnalysis> analysis;
};

[[nodiscard]] QString analysisKey(const QImage& primary,
                                  const QImage& secondary,
                                  const QImage& primaryNative,
                                  const QImage& secondaryNative,
                                  const bool resample) {
    // Buffer identity, not path: a re-decoded or replaced image must never reuse the previous
    // pair's field even when it is filed under the same name.
    return QStringLiteral("analysis-v1|%1|%2|%3|%4|resample:%5|%6x%7")
        .arg(primary.cacheKey())
        .arg(secondary.cacheKey())
        .arg(primaryNative.isNull() ? 0 : primaryNative.cacheKey())
        .arg(secondaryNative.isNull() ? 0 : secondaryNative.cacheKey())
        .arg(resample ? 1 : 0)
        .arg(primary.width())
        .arg(primary.height());
}

[[nodiscard]] DifferenceAnalysis analyzeDifference(QImage primary,
                                                   QImage secondary,
                                                   QImage primaryNative,
                                                   QImage secondaryNative,
                                                   const QString& key,
                                                   const bool resampleAllowed,
                                                   const std::atomic_bool& cancelled,
                                                   const std::function<void(int)>& rowObserver) {
    DifferenceAnalysis analysis;
    analysis.key = key;
    if (primary.isNull() || secondary.isNull()) {
        analysis.error = QObject::tr("Image pair is empty.");
        return analysis;
    }
    if (primary.width() <= 0 || primary.height() <= 0 || primary.width() > kMaximumImageEdge ||
        primary.height() > kMaximumImageEdge || secondary.width() <= 0 || secondary.height() <= 0 ||
        secondary.width() > kMaximumImageEdge || secondary.height() > kMaximumImageEdge) {
        analysis.error = QObject::tr("Image pair is empty or too large to diff.");
        return analysis;
    }
    if (cancelled.load()) {
        return analysis;
    }
    // Decoded buffers already arrive as RGBA8888, and convertToFormat returns the same buffer
    // when the format matches, so the common path neither copies nor reorders channels. Samples
    // are read as explicit RGBA8888 bytes: QRgb accessors would swap red and blue here.
    const QImage left = primary.format() == QImage::Format_RGBA8888
                            ? primary
                            : primary.convertToFormat(QImage::Format_RGBA8888);
    if (left.isNull()) {
        analysis.error = QObject::tr("Could not normalize the primary image buffer.");
        return analysis;
    }
    QImage right = secondary.format() == QImage::Format_RGBA8888
                       ? secondary
                       : secondary.convertToFormat(QImage::Format_RGBA8888);
    if (right.isNull()) {
        analysis.error = QObject::tr("Could not normalize the secondary image buffer.");
        return analysis;
    }
    if (right.size() != left.size()) {
        if (!resampleAllowed) {
            analysis.error =
                QObject::tr("A 与 B 尺寸不同（%1×%2 与 %3×%4），未启用重采样时不计算逐像素差异。")
                    .arg(primary.width())
                    .arg(primary.height())
                    .arg(secondary.width())
                    .arg(secondary.height());
            return analysis;
        }
        right = right.scaled(left.size(), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        if (right.format() != QImage::Format_RGBA8888) {
            right = right.convertToFormat(QImage::Format_RGBA8888);
        }
        if (right.isNull()) {
            analysis.error = QObject::tr("Could not resample the secondary image buffer.");
            return analysis;
        }
        analysis.resampled = true;
    }
    if (cancelled.load()) {
        return analysis;
    }
    // High-depth statistics need pixel-aligned sidecars. A resampled comparison no longer
    // corresponds to the sidecar samples, so it reports display-buffer statistics only.
    const bool native = !analysis.resampled && !primaryNative.isNull() &&
                        !secondaryNative.isNull() && primaryNative.size() == left.size() &&
                        secondaryNative.size() == left.size() &&
                        primaryNative.format() == QImage::Format_RGBA64 &&
                        secondaryNative.format() == QImage::Format_RGBA64;
    QImage magnitudes(left.width(), left.height(), QImage::Format_RGBA8888);
    QImage signs(left.width(), left.height(), QImage::Format_Grayscale8);
    if (magnitudes.isNull() || signs.isNull()) {
        analysis.error = QObject::tr("Could not allocate difference image.");
        return analysis;
    }
    const auto nativeSampleDelta =
        [](const uchar* leftLine, const uchar* rightLine, const std::size_t channel) {
            const auto* leftSamples = reinterpret_cast<const quint16*>(leftLine);
            const auto* rightSamples = reinterpret_cast<const quint16*>(rightLine);
            return std::abs(static_cast<int>(leftSamples[channel]) -
                            static_cast<int>(rightSamples[channel]));
        };
    qint64 rgbAbsSum = 0;
    qint64 alphaSum = 0;
    qint64 alphaChanged = 0;
    int peak = 0;
    int peakAlpha = 0;
    bool hasAlpha = false;
    qint64 nativeRgbSum = 0;
    qint64 nativeChanged = 0;
    qint64 nativeBeyondDisplay = 0;
    int nativePeak = 0;
    int nativePeakAlpha = 0;
    for (int y = 0; y < left.height(); ++y) {
        if (rowObserver) {
            rowObserver(y);
        }
        if (cancelled.load()) {
            return analysis;
        }
        const uchar* const leftLine = left.constScanLine(y);
        const uchar* const rightLine = right.constScanLine(y);
        uchar* const magnitudeLine = magnitudes.scanLine(y);
        uchar* const signLine = signs.scanLine(y);
        const uchar* const nativeLeftLine = native ? primaryNative.constScanLine(y) : nullptr;
        const uchar* const nativeRightLine = native ? secondaryNative.constScanLine(y) : nullptr;
        for (int x = 0; x < left.width(); ++x) {
            const auto offset = static_cast<std::size_t>(x) * 4U;
            const int redLeft = leftLine[offset];
            const int greenLeft = leftLine[offset + 1U];
            const int blueLeft = leftLine[offset + 2U];
            const int alphaLeft = leftLine[offset + 3U];
            const int redRight = rightLine[offset];
            const int greenRight = rightLine[offset + 1U];
            const int blueRight = rightLine[offset + 2U];
            const int alphaRight = rightLine[offset + 3U];
            const int redDelta = std::abs(redLeft - redRight);
            const int greenDelta = std::abs(greenLeft - greenRight);
            const int blueDelta = std::abs(blueLeft - blueRight);
            const int alphaDelta = std::abs(alphaLeft - alphaRight);
            const int delta = std::max({redDelta, greenDelta, blueDelta});
            rgbAbsSum += redDelta + greenDelta + blueDelta;
            peak = std::max(peak, delta);
            peakAlpha = std::max(peakAlpha, alphaDelta);
            // Alpha statistics cover the straight (unassociated) alpha of both decoded
            // buffers, so a transparency regression is visible even when RGB matches.
            alphaSum += alphaDelta;
            if (alphaDelta > 0) {
                ++alphaChanged;
            }
            if (alphaLeft < 255 || alphaRight < 255) {
                hasAlpha = true;
            }
            magnitudeLine[offset] = static_cast<uchar>(redDelta);
            magnitudeLine[offset + 1U] = static_cast<uchar>(greenDelta);
            magnitudeLine[offset + 2U] = static_cast<uchar>(blueDelta);
            magnitudeLine[offset + 3U] = static_cast<uchar>(alphaDelta);
            int signBits = 0;
            if (redLeft < redRight) {
                signBits |= 0x1;
            }
            if (greenLeft < greenRight) {
                signBits |= 0x2;
            }
            if (blueLeft < blueRight) {
                signBits |= 0x4;
            }
            signLine[x] = static_cast<uchar>(signBits);
            if (nativeLeftLine == nullptr) {
                continue;
            }
            const int nativeRedDelta = nativeSampleDelta(nativeLeftLine, nativeRightLine, offset);
            const int nativeGreenDelta =
                nativeSampleDelta(nativeLeftLine, nativeRightLine, offset + 1U);
            const int nativeBlueDelta =
                nativeSampleDelta(nativeLeftLine, nativeRightLine, offset + 2U);
            const int nativeAlphaDelta =
                nativeSampleDelta(nativeLeftLine, nativeRightLine, offset + 3U);
            const int nativeDelta = std::max({nativeRedDelta, nativeGreenDelta, nativeBlueDelta});
            nativeRgbSum += nativeRedDelta + nativeGreenDelta + nativeBlueDelta;
            nativePeak = std::max(nativePeak, nativeDelta);
            nativePeakAlpha = std::max(nativePeakAlpha, nativeAlphaDelta);
            if (nativeDelta > 0 || nativeAlphaDelta > 0) {
                ++nativeChanged;
            }
            if (delta == 0 && alphaDelta == 0 && (nativeDelta > 0 || nativeAlphaDelta > 0)) {
                // The RGBA8 display buffers call these pixels identical while the converted
                // 16-bit code values still differ: exactly the case the screen cannot show and
                // the numbers must not hide.
                ++nativeBeyondDisplay;
            }
        }
    }
    const qint64 pixelCount =
        static_cast<qint64>(left.width()) * static_cast<qint64>(left.height());
    analysis.magnitudes = std::move(magnitudes);
    analysis.signs = std::move(signs);
    analysis.base = left;
    analysis.maxAbsDifference = peak;
    analysis.meanAbsDifference =
        pixelCount > 0 ? static_cast<double>(rgbAbsSum) / (pixelCount * 3) : 0.0;
    analysis.peakAlphaDifference = peakAlpha;
    analysis.meanAlphaDifference =
        pixelCount > 0 ? static_cast<double>(alphaSum) / pixelCount : 0.0;
    analysis.alphaChangedPixels = alphaChanged;
    analysis.hasAlpha = hasAlpha;
    analysis.nativeAvailable = native;
    analysis.nativeMaxAbsDifference = nativePeak;
    analysis.nativeMeanAbsDifference =
        native && pixelCount > 0 ? static_cast<double>(nativeRgbSum) / (pixelCount * 3) : 0.0;
    analysis.nativePeakAlphaDifference = nativePeakAlpha;
    analysis.nativeChangedPixels = nativeChanged;
    analysis.nativeBeyondDisplayPixels = nativeBeyondDisplay;
    analysis.bytes =
        static_cast<qint64>(analysis.magnitudes.sizeInBytes()) + analysis.signs.sizeInBytes();
    return analysis;
}

// Renders one display variant from a finished analysis. This pass reads the canonical field
// only, so its cost does not depend on the source formats or on the statistics.
[[nodiscard]] QImage renderDifference(const DifferenceAnalysis& analysis,
                                      const int compareMode,
                                      const int gain,
                                      const std::atomic_bool& cancelled) {
    const QImage& magnitudes = analysis.magnitudes;
    const QImage& signs = analysis.signs;
    const QImage& base = analysis.base;
    QImage output(magnitudes.width(), magnitudes.height(), QImage::Format_ARGB32);
    if (output.isNull()) {
        return {};
    }
    const int safeGain = std::clamp(gain, kMinimumDifferenceGain, kMaximumDifferenceGain);
    for (int y = 0; y < magnitudes.height(); ++y) {
        if (cancelled.load()) {
            return {};
        }
        const uchar* const magnitudeLine = magnitudes.constScanLine(y);
        const uchar* const signLine = signs.constScanLine(y);
        const uchar* const baseLine = base.constScanLine(y);
        auto* const outLine = reinterpret_cast<QRgb*>(output.scanLine(y));
        for (int x = 0; x < magnitudes.width(); ++x) {
            const auto offset = static_cast<std::size_t>(x) * 4U;
            const int red = magnitudeLine[offset];
            const int green = magnitudeLine[offset + 1U];
            const int blue = magnitudeLine[offset + 2U];
            const int alpha = magnitudeLine[offset + 3U];
            switch (compareMode) {
            case kSignedDifferenceMode:
                outLine[x] = signedDiffPixel(red, green, blue, signLine[x], safeGain);
                break;
            case kHighlightMode:
                outLine[x] = highlightPixel(
                    qRgb(baseLine[offset], baseLine[offset + 1U], baseLine[offset + 2U]),
                    std::max({red, green, blue}),
                    safeGain);
                break;
            case kAlphaDifferenceMode:
                outLine[x] = alphaDiffPixel(alpha, safeGain);
                break;
            case kAbsDifferenceMode:
            default:
                outLine[x] = absDiffPixel(red, green, blue, safeGain);
                break;
            }
        }
    }
    return output;
}

void applyAnalysisStatistics(const DifferenceAnalysis& analysis,
                             ImagePairLoader::DifferenceResult* const result) {
    result->maxAbsDifference = analysis.maxAbsDifference;
    result->meanAbsDifference = analysis.meanAbsDifference;
    result->peakAlphaDifference = analysis.peakAlphaDifference;
    result->meanAlphaDifference = analysis.meanAlphaDifference;
    result->alphaChangedPixels = analysis.alphaChangedPixels;
    result->hasAlpha = analysis.hasAlpha;
    result->resampled = analysis.resampled;
    result->alphaDifferenceOnly =
        analysis.maxAbsDifference == 0 && analysis.peakAlphaDifference > 0;
    result->nativeAvailable = analysis.nativeAvailable;
    result->nativeMaxAbsDifference = analysis.nativeMaxAbsDifference;
    result->nativeMeanAbsDifference = analysis.nativeMeanAbsDifference;
    result->nativePeakAlphaDifference = analysis.nativePeakAlphaDifference;
    result->nativeChangedPixels = analysis.nativeChangedPixels;
    result->nativeBeyondDisplayPixels = analysis.nativeBeyondDisplayPixels;
    result->displayEqualButNativeDifferent = analysis.nativeBeyondDisplayPixels > 0;
}
struct RequestState final {
    quint64 requestId = 0;
    bool prefetch = false;
    bool difference = false;
    std::atomic_bool cancelled{false};
    ImagePairLoader::ResultHandler resultHandler;
    ImagePairLoader::DifferenceHandler differenceHandler;
};
// Decoded image plus its source provenance, cached together so a cache hit never loses the
// metadata that separates display-converted samples from original code values. The native
// RGBA64 sidecar rides along for high-bit-depth sources.
struct DecodedCacheEntry final {
    QImage image;
    QImage native;
    StillImageSourceInfo info;
};
struct LoadJob final {
    enum class Kind {
        Primary,
        Secondary,
        Pair,
    };
    quint64 requestId = 0;
    Kind kind = Kind::Primary;
    bool prefetch = false;
    QUrl primaryUrl;
    QUrl secondaryUrl;
    int pairId = -1;
    QString primaryIdentity;
    QString secondaryIdentity;
    std::optional<DecodedCacheEntry> cachedPrimary;
    std::optional<DecodedCacheEntry> cachedSecondary;
    ImagePairLoader::DecodePolicy policy;
};
struct DifferenceJob final {
    quint64 requestId = 0;
    QImage primary;
    QImage secondary;
    QImage primaryNative;
    QImage secondaryNative;
    ImagePairLoader::DifferenceOptions options;
    QString analysisKey;
    // Non-empty when the pair cannot run inside the working-set budget: published as the job's
    // failure without allocating anything.
    QString budgetError;
};

// Runs the analysis when it is not reused, renders one display variant, and reports the freshly
// computed analysis back so the owner can keep it for the next mode or gain.
[[nodiscard]] DifferenceComputation computeDifference(const DifferenceJob& job,
                                                      std::optional<DifferenceAnalysis> reused,
                                                      const std::atomic_bool& cancelled,
                                                      const std::function<void(int)>& rowObserver) {
    DifferenceComputation computation;
    ImagePairLoader::DifferenceResult& result = computation.result;
    if (!job.budgetError.isEmpty()) {
        result.error = job.budgetError;
        return computation;
    }
    if (reused.has_value()) {
        result.analysisReused = true;
    } else {
        DifferenceAnalysis analysis = analyzeDifference(job.primary,
                                                        job.secondary,
                                                        job.primaryNative,
                                                        job.secondaryNative,
                                                        job.analysisKey,
                                                        job.options.resample,
                                                        cancelled,
                                                        rowObserver);
        if (!analysis.error.isEmpty()) {
            result.error = analysis.error;
            return computation;
        }
        if (analysis.magnitudes.isNull() || analysis.signs.isNull()) {
            // Cancelled mid-pass: there is no field and no statistics to publish or cache.
            return computation;
        }
        reused = std::move(analysis);
        computation.analysis = reused;
    }
    if (reused->magnitudes.isNull() || reused->signs.isNull()) {
        return computation;
    }
    applyAnalysisStatistics(*reused, &result);
    result.gain = std::clamp(job.options.gain, kMinimumDifferenceGain, kMaximumDifferenceGain);
    QImage rendered = renderDifference(*reused, job.options.compareMode, result.gain, cancelled);
    if (rendered.isNull()) {
        if (!cancelled.load()) {
            result.error = QObject::tr("Could not allocate difference image.");
        }
        return computation;
    }
    result.image = std::move(rendered);
    return computation;
}
} // namespace
class ImagePairLoader::Impl final {
public:
    explicit Impl(ImagePairLoader* owner) : owner_(owner) {
        pool_.setMaxThreadCount(2);
        pool_.setExpiryTimeout(3000);
        cache_.setMaximumBytes(128LL * 1024LL * 1024LL);
    }
    ~Impl() {
        shutdown();
    }
    void shutdown() {
        cancelAll();
        pool_.waitForDone();
        active_.clear();
    }
    quint64 requestPrimary(const QUrl& primary,
                           const int pairId,
                           const DecodePolicy& policy,
                           ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Primary;
        job.primaryUrl = primary;
        job.pairId = pairId;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 requestSecondary(const QUrl& secondary,
                             const int pairId,
                             const DecodePolicy& policy,
                             ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Secondary;
        job.secondaryUrl = secondary;
        job.pairId = pairId;
        job.policy = policy;
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 requestPair(const QUrl& primary,
                        const QUrl& secondary,
                        const int pairId,
                        const DecodePolicy& policy,
                        ResultHandler handler) {
        LoadJob job;
        job.kind = LoadJob::Kind::Pair;
        job.primaryUrl = primary;
        job.secondaryUrl = secondary;
        job.pairId = pairId;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), std::move(handler), /*prefetch=*/false);
    }
    quint64 prefetchPair(const QUrl& primary, const QUrl& secondary, const DecodePolicy& policy) {
        LoadJob job;
        job.kind = LoadJob::Kind::Pair;
        job.prefetch = true;
        job.primaryUrl = primary;
        job.secondaryUrl = secondary;
        job.policy = policy;
        job.primaryIdentity = identityFor(primary, policy.revision);
        job.secondaryIdentity = identityFor(secondary, policy.revision);
        job.cachedPrimary = lookup(job.primaryIdentity);
        job.cachedSecondary = lookup(job.secondaryIdentity);
        return enqueueLoad(std::move(job), {}, /*prefetch=*/true);
    }
    quint64 requestDifference(QImage primary,
                              QImage secondary,
                              QImage primaryNative,
                              QImage secondaryNative,
                              const DifferenceOptions& options,
                              DifferenceHandler handler) {
        DifferenceJob job;
        job.primary = std::move(primary);
        job.secondary = std::move(secondary);
        job.primaryNative = std::move(primaryNative);
        job.secondaryNative = std::move(secondaryNative);
        job.options = options;
        job.analysisKey = analysisKey(
            job.primary, job.secondary, job.primaryNative, job.secondaryNative, options.resample);
        const bool nativeSidecars = !job.primaryNative.isNull() && !job.secondaryNative.isNull();
        workingSet_ = estimatePairWorkingSet(
            job.primary.size(), job.secondary.size(), nativeSidecars, options.resample);
        if (workingSet_.totalBytes > workingSetBudgetBytes_) {
            job.budgetError =
                QObject::tr("该配对的工作集约 %1 MiB，超过 %2 MiB 预算，未开始差异计算。")
                    .arg((workingSet_.totalBytes + 1048575LL) / 1048576LL)
                    .arg((workingSetBudgetBytes_ + 1048575LL) / 1048576LL);
        }
        // The reuse decision is taken here, on the owner's thread, so the worker never touches
        // the cache. QImage is implicitly shared, so handing the analysis to the job only bumps
        // reference counts.
        std::optional<DifferenceAnalysis> reused;
        if (job.budgetError.isEmpty() && analysis_.has_value()) {
            if (analysis_->key == job.analysisKey && analysis_->error.isEmpty()) {
                reused = analysis_;
            } else {
                // Another pair is being inspected without a load request (a caller switched
                // buffers directly), so drop the stale field instead of pinning its sources.
                analysis_.reset();
            }
        }
        const quint64 requestId = nextRequestId_++;
        job.requestId = requestId;
        auto state = std::make_shared<RequestState>();
        state->requestId = requestId;
        state->difference = true;
        state->differenceHandler = std::move(handler);
        active_.emplace(requestId, state);
        schedule(state,
                 [this,
                  requestId,
                  job = std::move(job),
                  reused = std::move(reused),
                  state,
                  observer = differenceRowObserver_]() mutable {
                     DifferenceComputation computation;
                     computation.result.requestId = requestId;
                     if (!state->cancelled.load()) {
                         computation =
                             computeDifference(job, std::move(reused), state->cancelled, observer);
                         computation.result.requestId = requestId;
                     }
                     postDifferenceResult(std::move(computation), std::move(state));
                 });
        return requestId;
    }
    void clearAnalysisCache() {
        analysis_.reset();
    }
    void cancel(const quint64 requestId) {
        const auto iterator = active_.find(requestId);
        if (iterator != active_.end()) {
            iterator->second->cancelled.store(true);
            if (pending_.erase(requestId) != 0) {
                active_.erase(iterator);
            }
        }
    }
    void cancelAll() {
        for (auto& [id, state] : active_) {
            static_cast<void>(id);
            state->cancelled.store(true);
        }
        for (const auto& [id, job] : pending_) {
            static_cast<void>(job);
            active_.erase(id);
        }
        pending_.clear();
        // Every request was cancelled, so a cached pair analysis has no consumer left.
        analysis_.reset();
    }
    void cancelPrefetches() {
        for (auto iterator = active_.begin(); iterator != active_.end();) {
            const auto state = iterator++->second;
            if (state->prefetch) {
                cancel(state->requestId);
            }
        }
    }
    void setDifferenceRowObserverForTesting(std::function<void(int)> observer) {
        differenceRowObserver_ = std::move(observer);
    }
    void setCacheBudgetBytes(const qint64 bytes) {
        cache_.setMaximumBytes(bytes);
    }
    [[nodiscard]] qint64 cacheBudgetBytes() const noexcept {
        return cache_.maximumBytes();
    }
    void setWorkingSetBudgetBytes(const qint64 bytes) {
        workingSetBudgetBytes_ = bytes > 0 ? bytes : kImagePairWorkingSetBudgetBytes;
    }
    [[nodiscard]] qint64 workingSetBudgetBytes() const noexcept {
        return workingSetBudgetBytes_;
    }
    void clearCache() {
        cache_.clear();
        cache_.resetStats();
        analysis_.reset();
    }
    [[nodiscard]] Stats stats() const noexcept {
        Stats result;
        result.cacheBytes = cache_.currentBytes();
        result.cacheBudgetBytes = cache_.maximumBytes();
        result.cacheEntries = cache_.size();
        result.cacheHits = cache_.hits();
        result.cacheMisses = cache_.misses();
        result.activeRequests = static_cast<int>(active_.size());
        result.maxThreadCount = pool_.maxThreadCount();
        result.pendingRequests = static_cast<int>(pending_.size());
        result.analysisRuns = analysisRuns_;
        result.analysisReuses = analysisReuses_;
        result.renderRuns = renderRuns_;
        result.workingSet = workingSet_;
        result.workingSetBudgetBytes = workingSetBudgetBytes_;
        return result;
    }

private:
    void schedule(const std::shared_ptr<RequestState>& state, std::function<void()> job) {
        // At most two running jobs and one pending job per class. Replacing a queued
        // candidate releases its images immediately rather than feeding QThreadPool a backlog.
        for (auto iterator = pending_.begin(); iterator != pending_.end();) {
            const auto old = active_.at(iterator->first);
            if (old->prefetch == state->prefetch && old->difference == state->difference) {
                old->cancelled.store(true);
                active_.erase(iterator->first);
                iterator = pending_.erase(iterator);
            } else {
                ++iterator;
            }
        }
        pending_.emplace(state->requestId, std::move(job));
        dispatch();
    }
    void dispatch() {
        while (running_ < pool_.maxThreadCount() && !pending_.empty()) {
            auto next = pending_.begin();
            for (auto iterator = pending_.begin(); iterator != pending_.end(); ++iterator) {
                if (!active_.at(iterator->first)->prefetch) {
                    next = iterator;
                    break;
                }
            }
            auto job = std::move(next->second);
            pending_.erase(next);
            ++running_;
            pool_.start(new FunctionRunnable(std::move(job)));
        }
    }
    [[nodiscard]] QString cacheKey(const QString& identity) const {
        return identity + QStringLiteral("|decoded-rgba8");
    }
    [[nodiscard]] QString identityFor(const QUrl& url, const std::uint64_t revision) const {
        if (!url.isValid()) {
            return {};
        }
        const QString identity = fileIdentity(url, revision);
        return identity.isEmpty()
                   ? QStringLiteral("url:%1|policy:%2").arg(url.toString()).arg(revision)
                   : identity;
    }
    [[nodiscard]] std::optional<DecodedCacheEntry> lookup(const QString& identity) {
        if (identity.isEmpty()) {
            return std::nullopt;
        }
        DecodedCacheEntry entry;
        if (cache_.get(cacheKey(identity), &entry) && !entry.image.isNull()) {
            return entry;
        }
        return std::nullopt;
    }
    [[nodiscard]] quint64 enqueueLoad(LoadJob job, ResultHandler handler, const bool prefetch) {
        cancelPrefetches();
        if (!prefetch) {
            // A user-visible load changes the committed pair, so the previous pair's analysis
            // field (and the source buffers it pins) must not survive it.
            analysis_.reset();
        }
        job.requestId = nextRequestId_++;
        auto state = std::make_shared<RequestState>();
        state->requestId = job.requestId;
        state->prefetch = prefetch;
        state->resultHandler = std::move(handler);
        const quint64 requestId = job.requestId;
        active_.emplace(requestId, state);
        schedule(state, [this, job = std::move(job), state]() mutable {
            executeLoad(std::move(job), std::move(state));
        });
        return requestId;
    }
    void executeLoad(LoadJob job, std::shared_ptr<RequestState> state) {
        Result result;
        result.requestId = job.requestId;
        result.pairId = job.pairId;
        result.pair = job.kind == LoadJob::Kind::Pair;
        if (!state->cancelled.load()) {
            if (job.kind != LoadJob::Kind::Secondary && !job.cachedPrimary.has_value()) {
                QString error;
                QString identity;
                if (!loadImageFromDisk(job.primaryUrl,
                                       job.policy,
                                       &result.primary,
                                       &result.primaryNative,
                                       &result.primaryInfo,
                                       &identity,
                                       &error,
                                       state->cancelled)) {
                    result.failedSide = 0;
                    result.error = error;
                }
                if (job.primaryIdentity.isEmpty()) {
                    job.primaryIdentity = identity;
                }
            } else if (job.cachedPrimary.has_value()) {
                result.primary = job.cachedPrimary->image;
                result.primaryNative = job.cachedPrimary->native;
                result.primaryInfo = job.cachedPrimary->info;
            }
            if (!state->cancelled.load() && result.error.isEmpty() &&
                job.kind != LoadJob::Kind::Primary && !job.cachedSecondary.has_value()) {
                QString error;
                QString identity;
                if (!loadImageFromDisk(job.secondaryUrl,
                                       job.policy,
                                       &result.secondary,
                                       &result.secondaryNative,
                                       &result.secondaryInfo,
                                       &identity,
                                       &error,
                                       state->cancelled)) {
                    result.failedSide = 1;
                    result.error = error;
                }
                if (job.secondaryIdentity.isEmpty()) {
                    job.secondaryIdentity = identity;
                }
            } else if (job.cachedSecondary.has_value()) {
                result.secondary = job.cachedSecondary->image;
                result.secondaryNative = job.cachedSecondary->native;
                result.secondaryInfo = job.cachedSecondary->info;
            }
        }
        if (job.kind == LoadJob::Kind::Primary) {
            result.secondaryOnly = false;
        } else if (job.kind == LoadJob::Kind::Secondary) {
            result.secondaryOnly = true;
        }
        result.primaryLabel =
            job.kind == LoadJob::Kind::Secondary ? QString{} : sourceLabel(job.primaryUrl);
        result.secondaryLabel =
            job.kind == LoadJob::Kind::Primary ? QString{} : sourceLabel(job.secondaryUrl);
        result.primaryIdentity = job.primaryIdentity;
        result.secondaryIdentity = job.secondaryIdentity;
        postLoadResult(std::move(result), std::move(state));
    }
    void postLoadResult(Result result, std::shared_ptr<RequestState> state) {
        QMetaObject::invokeMethod(
            owner_,
            [this, result = std::move(result), state = std::move(state)]() mutable {
                finishLoad(std::move(result), std::move(state));
            },
            Qt::QueuedConnection);
    }
    void finishLoad(Result result, const std::shared_ptr<RequestState>& state) {
        active_.erase(result.requestId);
        --running_;
        dispatch();
        if (!state || state->cancelled.load()) {
            return;
        }
        if (result.succeeded()) {
            if (!result.primary.isNull() && !result.primaryIdentity.isEmpty()) {
                cache_.put(
                    cacheKey(result.primaryIdentity),
                    DecodedCacheEntry{result.primary, result.primaryNative, result.primaryInfo});
            }
            if (!result.secondary.isNull() && !result.secondaryIdentity.isEmpty()) {
                cache_.put(cacheKey(result.secondaryIdentity),
                           DecodedCacheEntry{
                               result.secondary, result.secondaryNative, result.secondaryInfo});
            }
        }
        if (!state->prefetch && state->resultHandler) {
            state->resultHandler(std::move(result));
        }
    }
    void postDifferenceResult(DifferenceComputation computation,
                              std::shared_ptr<RequestState> state) {
        QMetaObject::invokeMethod(
            owner_,
            [this, computation = std::move(computation), state = std::move(state)]() mutable {
                finishDifference(std::move(computation), std::move(state));
            },
            Qt::QueuedConnection);
    }
    void finishDifference(DifferenceComputation computation,
                          const std::shared_ptr<RequestState>& state) {
        DifferenceResult result = std::move(computation.result);
        active_.erase(result.requestId);
        --running_;
        dispatch();
        if (!state || state->cancelled.load()) {
            return;
        }
        if (computation.analysis.has_value() && computation.analysis->error.isEmpty()) {
            analysis_ = std::move(computation.analysis);
            ++analysisRuns_;
        } else if (result.analysisReused) {
            ++analysisReuses_;
        }
        if (result.succeeded()) {
            ++renderRuns_;
        }
        if (state->differenceHandler) {
            state->differenceHandler(std::move(result));
        }
    }
    ImagePairLoader* owner_ = nullptr;
    QThreadPool pool_;
    ByteLruCache<DecodedCacheEntry> cache_{
        128LL * 1024LL * 1024LL, [](const DecodedCacheEntry& entry) {
            // The RGBA64 sidecar is part of the entry's footprint, not an extra cache: a
            // high-bit-depth image that no longer fits evicts itself as one unit.
            return static_cast<qint64>(entry.image.sizeInBytes()) +
                   static_cast<qint64>(entry.native.sizeInBytes());
        }};
    std::map<quint64, std::shared_ptr<RequestState>> active_;
    std::map<quint64, std::function<void()>> pending_;
    std::function<void(int)> differenceRowObserver_;
    // Single-slot per-pair analysis: the delta field, the sign map and every statistic of the
    // pair currently under inspection. Owned by this object's thread; a job receives a shared
    // copy so a worker never mutates it.
    std::optional<DifferenceAnalysis> analysis_;
    ImageWorkingSetEstimate workingSet_;
    qint64 workingSetBudgetBytes_ = kImagePairWorkingSetBudgetBytes;
    quint64 analysisRuns_ = 0;
    quint64 analysisReuses_ = 0;
    quint64 renderRuns_ = 0;
    int running_ = 0;
    quint64 nextRequestId_ = 1;
};
ImagePairLoader::ImagePairLoader(QObject* parent)
    : QObject(parent), impl_(std::make_unique<Impl>(this)) {}
ImagePairLoader::~ImagePairLoader() {
    impl_->shutdown();
}
quint64 ImagePairLoader::requestPrimary(const QUrl& primary,
                                        const int pairId,
                                        const DecodePolicy& policy,
                                        ResultHandler handler) {
    return impl_->requestPrimary(primary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::requestSecondary(const QUrl& secondary,
                                          const int pairId,
                                          const DecodePolicy& policy,
                                          ResultHandler handler) {
    return impl_->requestSecondary(secondary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::requestPair(const QUrl& primary,
                                     const QUrl& secondary,
                                     const int pairId,
                                     const DecodePolicy& policy,
                                     ResultHandler handler) {
    return impl_->requestPair(primary, secondary, pairId, policy, std::move(handler));
}
quint64 ImagePairLoader::prefetchPair(const QUrl& primary,
                                      const QUrl& secondary,
                                      const DecodePolicy& policy) {
    return impl_->prefetchPair(primary, secondary, policy);
}
quint64 ImagePairLoader::requestDifference(QImage primary,
                                           QImage secondary,
                                           QImage primaryNative,
                                           QImage secondaryNative,
                                           const DifferenceOptions& options,
                                           DifferenceHandler handler) {
    return impl_->requestDifference(std::move(primary),
                                    std::move(secondary),
                                    std::move(primaryNative),
                                    std::move(secondaryNative),
                                    options,
                                    std::move(handler));
}
quint64 ImagePairLoader::requestDifference(QImage primary,
                                           QImage secondary,
                                           const int compareMode,
                                           const bool resample,
                                           DifferenceHandler handler) {
    DifferenceOptions options;
    options.compareMode = compareMode;
    options.resample = resample;
    return impl_->requestDifference(
        std::move(primary), std::move(secondary), QImage{}, QImage{}, options, std::move(handler));
}
void ImagePairLoader::clearAnalysisCache() {
    impl_->clearAnalysisCache();
}
void ImagePairLoader::setDifferenceRowObserverForTesting(std::function<void(int)> observer) {
    impl_->setDifferenceRowObserverForTesting(std::move(observer));
}
void ImagePairLoader::cancel(const quint64 requestId) {
    impl_->cancel(requestId);
}
void ImagePairLoader::cancelAll() {
    impl_->cancelAll();
}
void ImagePairLoader::cancelPrefetches() {
    impl_->cancelPrefetches();
}
void ImagePairLoader::setCacheBudgetBytes(const qint64 bytes) {
    impl_->setCacheBudgetBytes(bytes);
}
qint64 ImagePairLoader::cacheBudgetBytes() const noexcept {
    return impl_->cacheBudgetBytes();
}
void ImagePairLoader::setWorkingSetBudgetBytes(const qint64 bytes) {
    impl_->setWorkingSetBudgetBytes(bytes);
}
qint64 ImagePairLoader::workingSetBudgetBytes() const noexcept {
    return impl_->workingSetBudgetBytes();
}
void ImagePairLoader::clearCache() {
    impl_->clearCache();
}
ImagePairLoader::Stats ImagePairLoader::stats() const noexcept {
    return impl_->stats();
}
} // namespace dvs::ui
