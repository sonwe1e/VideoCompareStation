#pragma once

#include <QImage>
#include <QPoint>
#include <QRect>
#include <QString>

#include <memory>
#include <vector>

namespace dvs::ui {

// Optional drop-in SAM 2.1 AI segmentation engine.
// Completely decouples onnxruntime from compile-time / link-time dependencies.
// If onnxruntime.dll and model files are present in the models directory,
// the engine dynamically resolves symbols via LoadLibrary and enables AI cutout.
// If absent, it gracefully reports unavailable without any errors or overhead.
class Sam2Segmenter final {
public:
    Sam2Segmenter();
    ~Sam2Segmenter();

    Sam2Segmenter(const Sam2Segmenter&) = delete;
    Sam2Segmenter& operator=(const Sam2Segmenter&) = delete;
    Sam2Segmenter(Sam2Segmenter&&) noexcept;
    Sam2Segmenter& operator=(Sam2Segmenter&&) noexcept;

    // Checks candidate directories for onnxruntime.dll, sam2_encoder.onnx, and
    // sam2_decoder.onnx. Loads and initializes sessions on CPU if all found.
    [[nodiscard]] bool probe(const QString& explicitDir = QString());

    // Whether the engine is initialized and ready for inference.
    [[nodiscard]] bool isAvailable() const noexcept;

    // Human-readable status message for user feedback.
    [[nodiscard]] QString statusMessage() const;

    // Encodes the working image using the SAM 2.1 Image Encoder.
    // Caches the 1024-dim and high-resolution features for instant decoder calls.
    bool encodeImage(const QImage& image);

    // Clears the cached image embeddings.
    void clearCache();

    // Runs the Mask Decoder using an image-pixel bounding box prompt.
    // Returns a soft-alpha QImage (Format_Grayscale8, 255 = solid foreground, 0 = background)
    // with dimensions identical to the encoded image. Edge pixels carry intermediate values
    // so the cutout can fade over roughly one source pixel instead of stepping.
    [[nodiscard]] QImage segmentBox(const QRect& box);

    // Runs the Mask Decoder using image-pixel points and positive/negative labels.
    [[nodiscard]] QImage segmentPoints(const std::vector<QPoint>& points,
                                       const std::vector<int>& labels);

    // Applies the soft mask to the original image. Output alpha is
    // round(srcAlpha * smoothstep(low, high, mask)), so the foreground keeps its original RGB
    // and true alpha while the background becomes fully transparent. If cropToBoundingBox is
    // true, crops tightly to the solid mask bounds. If outBoundingBox is non-null, writes the
    // bounding box of solid foreground pixels. Returns a null image when no solid foreground
    // pixel exists, which lets callers reject an empty result instead of committing a
    // fully transparent frame.
    [[nodiscard]] static QImage applyCutout(const QImage& original,
                                            const QImage& mask,
                                            bool cropToBoundingBox = false,
                                            QRect* outBoundingBox = nullptr);

    // Fraction (0..1) of mask pixels above the solid-foreground threshold. The controller
    // rejects degenerate masks (nothing found, or the whole frame selected) before pushing a
    // no-op history step.
    [[nodiscard]] static qreal solidCoverage(const QImage& mask);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace dvs::ui
