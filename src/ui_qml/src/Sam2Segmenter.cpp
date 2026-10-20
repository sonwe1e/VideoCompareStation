#include "dvs/ui/Sam2Segmenter.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define ORT_API_MANUAL_INIT
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QPainter>
#include <QStringList>

#include <algorithm>
#include <array>
#include <cmath>
#include <onnxruntime/onnxruntime_c_api.h>
#include <onnxruntime/onnxruntime_cxx_api.h>
#include <vector>
#include <windows.h>

namespace dvs::ui {
namespace {

using OrtGetApiBaseFn = const OrtApiBase*(ORT_API_CALL*)();

constexpr int kTargetImageSize = 1024;
constexpr int kMaskSize = 256;
constexpr int kMaskScale = kTargetImageSize / kMaskSize;
// Neutral grey used to fill transparent pixels before encoding. Black would be read as a real
// dark object on the second pass over an already cut-out image.
constexpr int kNeutralFill = 128;
// Soft-alpha band. Values outside the band are fully transparent/opaque; inside it a
// smoothstep ramp keeps the edge antialiased without a wide halo.
constexpr int kMaskLow = 89;   // 0.35 * 255
constexpr int kMaskHigh = 166; // 0.65 * 255
constexpr std::array<float, 3> kImageNetMean{0.485f, 0.456f, 0.406f};
constexpr std::array<float, 3> kImageNetStd{0.229f, 0.224f, 0.225f};

// Names reported to the user when a candidate directory is incomplete.
[[nodiscard]] QStringList missingModelFiles(const QDir& dir) {
    QStringList missing;
    if (!dir.exists(QStringLiteral("onnxruntime.dll"))) {
        missing << QStringLiteral("onnxruntime.dll");
    }
    if (!dir.exists(QStringLiteral("sam2_encoder.onnx")) &&
        !dir.exists(QStringLiteral("sam2.1_hiera_tiny.encoder.onnx"))) {
        missing << QStringLiteral("sam2_encoder.onnx");
    }
    if (!dir.exists(QStringLiteral("sam2_decoder.onnx")) &&
        !dir.exists(QStringLiteral("sam2.1_hiera_tiny.decoder.onnx"))) {
        missing << QStringLiteral("sam2_decoder.onnx");
    }
    return missing;
}

// smoothstep(low, high, value) mapped to 0..255, with a single multiply chain per pixel.
[[nodiscard]] uchar softAlphaFromMask(const uchar value) {
    if (value <= kMaskLow) {
        return 0;
    }
    if (value >= kMaskHigh) {
        return 255;
    }
    const float t = static_cast<float>(value - kMaskLow) / static_cast<float>(kMaskHigh - kMaskLow);
    const float smooth = t * t * (3.0f - 2.0f * t);
    return static_cast<uchar>(std::clamp(static_cast<int>(smooth * 255.0f + 0.5f), 0, 255));
}

// One exp per mask cell. 256x256 bounds the cost (about 65k calls), and the true logistic
// curve matches how the decoder logits were trained; a linear ramp would misprice the middle
// of the confidence range.
[[nodiscard]] uchar sigmoidToByte(const float logit) {
    const float value = 1.0f / (1.0f + std::exp(-logit));
    return static_cast<uchar>(std::clamp(static_cast<int>(value * 255.0f + 0.5f), 0, 255));
}

} // namespace

class Sam2Segmenter::Impl final {
public:
    struct OrtState {
        std::unique_ptr<Ort::Env> env;
        std::unique_ptr<Ort::Session> encoderSession;
        std::unique_ptr<Ort::Session> decoderSession;
        std::unique_ptr<Ort::Value> cachedHighRes0;
        std::unique_ptr<Ort::Value> cachedHighRes1;
        std::unique_ptr<Ort::Value> cachedImageEmbed;
    };

    Impl() = default;
    ~Impl() {
        unload();
    }

    void unload() {
        clearEmbeddings();
        ortState_.reset();
        if (ortModule_) {
            FreeLibrary(ortModule_);
            ortModule_ = nullptr;
        }
        loaded_ = false;
    }

    void clearEmbeddings() {
        if (ortState_) {
            ortState_->cachedHighRes0.reset();
            ortState_->cachedHighRes1.reset();
            ortState_->cachedImageEmbed.reset();
        }
        hasEmbeddings_ = false;
        origWidth_ = 0;
        origHeight_ = 0;
        scale_ = 1.0f;
        scaledWidth_ = 0;
        scaledHeight_ = 0;
    }

    [[nodiscard]] bool probeAndInitialize(const QString& explicitDir) {
        if (loaded_) {
            return true;
        }

        const QString appDir = QCoreApplication::applicationDirPath();
        const std::vector<QString> candidates = {
            explicitDir,
            QDir(appDir).filePath(QStringLiteral("models")),
            QDir(appDir).filePath(QStringLiteral("plugins/sam")),
            QDir(QDir::currentPath()).filePath(QStringLiteral("out/models")),
            QDir(QDir::currentPath()).filePath(QStringLiteral("models")),
        };

        QString chosenDir;
        // Best incomplete candidate, so the failure message can name the files that are
        // actually missing for the directory the user is most likely looking at.
        QString closestDir;
        QStringList closestMissing;
        for (const QString& candidate : candidates) {
            if (candidate.isEmpty()) {
                continue;
            }
            const QDir dir(candidate);
            if (!dir.exists()) {
                continue;
            }
            const QStringList missing = missingModelFiles(dir);
            if (missing.isEmpty()) {
                chosenDir = dir.absolutePath();
                break;
            }
            if (closestDir.isEmpty() || missing.size() < closestMissing.size()) {
                closestDir = dir.absolutePath();
                closestMissing = missing;
            }
        }

        if (chosenDir.isEmpty()) {
            if (closestDir.isEmpty()) {
                status_ = QCoreApplication::translate(
                    "Sam2Segmenter",
                    "未找到模型目录：请将 onnxruntime.dll、sam2_encoder.onnx、sam2_decoder.onnx "
                    "放入 models 目录后点击「检测模型」。");
            } else {
                // File names lead so the status line (which elides) still names what is
                // missing before it runs out of room.
                status_ = QCoreApplication::translate(
                              "Sam2Segmenter", "缺少 %1（目录：%2）。放入后点击「检测模型」。")
                              .arg(closestMissing.join(QStringLiteral("、")),
                                   QDir::toNativeSeparators(closestDir));
            }
            return false;
        }

        const QDir modelDir(chosenDir);
        const QString dllPath = modelDir.filePath(QStringLiteral("onnxruntime.dll"));
        QString encPath = modelDir.filePath(QStringLiteral("sam2_encoder.onnx"));
        if (!QFile::exists(encPath)) {
            encPath = modelDir.filePath(QStringLiteral("sam2.1_hiera_tiny.encoder.onnx"));
        }
        QString decPath = modelDir.filePath(QStringLiteral("sam2_decoder.onnx"));
        if (!QFile::exists(decPath)) {
            decPath = modelDir.filePath(QStringLiteral("sam2.1_hiera_tiny.decoder.onnx"));
        }

        // Dynamically load onnxruntime.dll
        ortModule_ = LoadLibraryW(reinterpret_cast<LPCWSTR>(dllPath.utf16()));
        if (!ortModule_) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "无法载入 %1 (错误码: %2)")
                          .arg(dllPath)
                          .arg(GetLastError());
            return false;
        }

        auto* getApiBase =
            reinterpret_cast<OrtGetApiBaseFn>(GetProcAddress(ortModule_, "OrtGetApiBase"));
        if (!getApiBase) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "%1 未导出 OrtGetApiBase 符号。")
                          .arg(dllPath);
            unload();
            return false;
        }

        const OrtApiBase* apiBase = getApiBase();
        if (!apiBase) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "无法获取 OrtApiBase。");
            unload();
            return false;
        }

        const OrtApi* api = apiBase->GetApi(ORT_API_VERSION);
        if (!api) {
            status_ =
                QCoreApplication::translate("Sam2Segmenter", "无法初始化 ORT_API_VERSION %1。")
                    .arg(ORT_API_VERSION);
            unload();
            return false;
        }

        Ort::InitApi(api);

        try {
            auto env = std::make_unique<Ort::Env>(ORT_LOGGING_LEVEL_WARNING, "dvs_sam2");
            Ort::SessionOptions sessionOptions;
            sessionOptions.SetIntraOpNumThreads(4);
            sessionOptions.SetInterOpNumThreads(4);

            auto encoder = std::make_unique<Ort::Session>(
                *env, reinterpret_cast<const wchar_t*>(encPath.utf16()), sessionOptions);
            auto decoder = std::make_unique<Ort::Session>(
                *env, reinterpret_cast<const wchar_t*>(decPath.utf16()), sessionOptions);

            ortState_ = std::make_unique<OrtState>();
            ortState_->env = std::move(env);
            ortState_->encoderSession = std::move(encoder);
            ortState_->decoderSession = std::move(decoder);
        } catch (const std::exception& ex) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "初始化 ONNX 引擎失败: %1")
                          .arg(QString::fromUtf8(ex.what()));
            unload();
            return false;
        }

        loaded_ = true;
        status_ = QCoreApplication::translate("Sam2Segmenter",
                                              "智能抠图引擎已就绪 (SAM 2.1 纯 CPU 模式)。");
        return true;
    }

    bool encode(const QImage& image) {
        if (!loaded_ || image.isNull()) {
            return false;
        }

        origWidth_ = image.width();
        origHeight_ = image.height();
        scale_ = static_cast<float>(kTargetImageSize) /
                 static_cast<float>(std::max(origWidth_, origHeight_));
        // A very thin crop can round one edge to zero, which would produce a null scaled image
        // and an empty tensor row; clamp so a 1-pixel-wide prompt still encodes.
        scaledWidth_ =
            std::max(1, static_cast<int>(std::round(static_cast<float>(origWidth_) * scale_)));
        scaledHeight_ =
            std::max(1, static_cast<int>(std::round(static_cast<float>(origHeight_) * scale_)));

        // Scale first, composite second: the temporary stays at the 1024 encoder size, so a
        // 16k source never allocates a full-size RGB copy. Transparent pixels are blended onto
        // neutral grey, not black, so a second cutout of an already cut image does not feed
        // the old background to the encoder as a real dark object.
        const QImage scaled =
            image
                .scaled(
                    scaledWidth_, scaledHeight_, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)
                .convertToFormat(QImage::Format_ARGB32);

        std::vector<float> inputTensor(
            static_cast<std::size_t>(3) * kTargetImageSize * kTargetImageSize, 0.0f);
        for (int y = 0; y < scaledHeight_; ++y) {
            const auto* scan = reinterpret_cast<const QRgb*>(scaled.constScanLine(y));
            for (int x = 0; x < scaledWidth_; ++x) {
                const QRgb pixel = scan[x];
                const int alpha = qAlpha(pixel);
                const int inverse = 255 - alpha;
                const float r = static_cast<float>(qRed(pixel) * alpha + kNeutralFill * inverse) /
                                (255.0f * 255.0f);
                const float g = static_cast<float>(qGreen(pixel) * alpha + kNeutralFill * inverse) /
                                (255.0f * 255.0f);
                const float b = static_cast<float>(qBlue(pixel) * alpha + kNeutralFill * inverse) /
                                (255.0f * 255.0f);

                const int idxR =
                    0 * (kTargetImageSize * kTargetImageSize) + y * kTargetImageSize + x;
                const int idxG =
                    1 * (kTargetImageSize * kTargetImageSize) + y * kTargetImageSize + x;
                const int idxB =
                    2 * (kTargetImageSize * kTargetImageSize) + y * kTargetImageSize + x;

                inputTensor[idxR] = (r - kImageNetMean[0]) / kImageNetStd[0];
                inputTensor[idxG] = (g - kImageNetMean[1]) / kImageNetStd[1];
                inputTensor[idxB] = (b - kImageNetMean[2]) / kImageNetStd[2];
            }
        }

        try {
            Ort::MemoryInfo memoryInfo =
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
            const std::array<int64_t, 4> inputShape{1, 3, kTargetImageSize, kTargetImageSize};
            Ort::Value inValue = Ort::Value::CreateTensor<float>(memoryInfo,
                                                                 inputTensor.data(),
                                                                 inputTensor.size(),
                                                                 inputShape.data(),
                                                                 inputShape.size());

            const char* inNames[] = {"image"};
            const char* outNames[] = {"high_res_feats_0", "high_res_feats_1", "image_embed"};

            auto outs = ortState_->encoderSession->Run(
                Ort::RunOptions{nullptr}, inNames, &inValue, 1, outNames, 3);
            ortState_->cachedHighRes0 = std::make_unique<Ort::Value>(std::move(outs[0]));
            ortState_->cachedHighRes1 = std::make_unique<Ort::Value>(std::move(outs[1]));
            ortState_->cachedImageEmbed = std::make_unique<Ort::Value>(std::move(outs[2]));
            hasEmbeddings_ = true;
            return true;
        } catch (const std::exception& ex) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "编码图像失败: %1")
                          .arg(QString::fromUtf8(ex.what()));
            return false;
        }
    }

    [[nodiscard]] QImage runDecoder(const std::vector<float>& coords,
                                    const std::vector<float>& labels) {
        if (!hasEmbeddings_ || !ortState_ || coords.empty() || labels.empty()) {
            return {};
        }

        const int numPoints = static_cast<int>(labels.size());
        const std::array<int64_t, 3> coordsShape{1, numPoints, 2};
        const std::array<int64_t, 2> labelsShape{1, numPoints};
        const std::array<int64_t, 4> maskInputShape{1, 1, kMaskSize, kMaskSize};
        const std::array<int64_t, 1> hasMaskInputShape{1};

        std::vector<float> maskInput(static_cast<std::size_t>(kMaskSize) * kMaskSize, 0.0f);
        std::vector<float> hasMaskInput{0.0f};

        try {
            Ort::MemoryInfo memoryInfo =
                Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

            Ort::Value coordsValue =
                Ort::Value::CreateTensor<float>(memoryInfo,
                                                const_cast<float*>(coords.data()),
                                                coords.size(),
                                                coordsShape.data(),
                                                coordsShape.size());

            Ort::Value labelsValue =
                Ort::Value::CreateTensor<float>(memoryInfo,
                                                const_cast<float*>(labels.data()),
                                                labels.size(),
                                                labelsShape.data(),
                                                labelsShape.size());

            Ort::Value maskInputValue = Ort::Value::CreateTensor<float>(memoryInfo,
                                                                        maskInput.data(),
                                                                        maskInput.size(),
                                                                        maskInputShape.data(),
                                                                        maskInputShape.size());

            Ort::Value hasMaskInputValue =
                Ort::Value::CreateTensor<float>(memoryInfo,
                                                hasMaskInput.data(),
                                                hasMaskInput.size(),
                                                hasMaskInputShape.data(),
                                                hasMaskInputShape.size());

            const char* inNames[] = {"image_embed",
                                     "high_res_feats_0",
                                     "high_res_feats_1",
                                     "point_coords",
                                     "point_labels",
                                     "mask_input",
                                     "has_mask_input"};

            const Ort::Value inValues[] = {std::move(*ortState_->cachedImageEmbed),
                                           std::move(*ortState_->cachedHighRes0),
                                           std::move(*ortState_->cachedHighRes1),
                                           std::move(coordsValue),
                                           std::move(labelsValue),
                                           std::move(maskInputValue),
                                           std::move(hasMaskInputValue)};

            const char* outNames[] = {"masks", "iou_predictions"};

            auto outs = ortState_->decoderSession->Run(
                Ort::RunOptions{nullptr}, inNames, inValues, 7, outNames, 2);

            // Re-store embeddings for future decodes
            *ortState_->cachedImageEmbed = std::move(const_cast<Ort::Value&>(inValues[0]));
            *ortState_->cachedHighRes0 = std::move(const_cast<Ort::Value&>(inValues[1]));
            *ortState_->cachedHighRes1 = std::move(const_cast<Ort::Value&>(inValues[2]));

            const float* maskData = outs[0].GetTensorData<float>();
            const float* iouData = outs[1].GetTensorData<float>();

            // Find best mask from the 3 multi-mask outputs
            int bestIdx = 0;
            float maxIou = -1.0f;
            for (int i = 0; i < 3; ++i) {
                if (iouData[i] > maxIou) {
                    maxIou = iouData[i];
                    bestIdx = i;
                }
            }

            // Keep the decoder's logits as a soft alpha ramp. A hard 0/255 split at zero is
            // what produced the visible staircase after upsampling.
            QImage mask256(kMaskSize, kMaskSize, QImage::Format_Grayscale8);
            const float* bestMaskSlice =
                maskData + static_cast<std::ptrdiff_t>(bestIdx) *
                               static_cast<std::ptrdiff_t>(kMaskSize * kMaskSize);
            for (int y = 0; y < kMaskSize; ++y) {
                uchar* scan = mask256.scanLine(y);
                for (int x = 0; x < kMaskSize; ++x) {
                    scan[x] = sigmoidToByte(bestMaskSlice[y * kMaskSize + x]);
                }
            }

            // Drop the zero-padded letterbox region in mask space, then go straight to the
            // original size with a smooth (bilinear) filter. One interpolation step instead of
            // "nearest to original", so the soft band survives instead of snapping back to a
            // hard edge.
            const int validWidth = std::clamp(
                static_cast<int>(std::lround(static_cast<double>(scaledWidth_) / kMaskScale)),
                1,
                kMaskSize);
            const int validHeight = std::clamp(
                static_cast<int>(std::lround(static_cast<double>(scaledHeight_) / kMaskScale)),
                1,
                kMaskSize);
            const QImage maskValid = mask256.copy(0, 0, validWidth, validHeight);
            return maskValid.scaled(
                origWidth_, origHeight_, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        } catch (const std::exception& ex) {
            status_ = QCoreApplication::translate("Sam2Segmenter", "解码掩膜失败: %1")
                          .arg(QString::fromUtf8(ex.what()));
            return {};
        }
    }

    HMODULE ortModule_ = nullptr;
    std::unique_ptr<OrtState> ortState_;

    bool loaded_ = false;
    bool hasEmbeddings_ = false;
    QString status_;

    int origWidth_ = 0;
    int origHeight_ = 0;
    float scale_ = 1.0f;
    int scaledWidth_ = 0;
    int scaledHeight_ = 0;
};

Sam2Segmenter::Sam2Segmenter() : impl_(std::make_unique<Impl>()) {}
Sam2Segmenter::~Sam2Segmenter() = default;
Sam2Segmenter::Sam2Segmenter(Sam2Segmenter&&) noexcept = default;
Sam2Segmenter& Sam2Segmenter::operator=(Sam2Segmenter&&) noexcept = default;

bool Sam2Segmenter::probe(const QString& explicitDir) {
    return impl_->probeAndInitialize(explicitDir);
}

bool Sam2Segmenter::isAvailable() const noexcept {
    return impl_->loaded_;
}

QString Sam2Segmenter::statusMessage() const {
    return impl_->status_;
}

bool Sam2Segmenter::encodeImage(const QImage& image) {
    return impl_->encode(image);
}

void Sam2Segmenter::clearCache() {
    impl_->clearEmbeddings();
}

QImage Sam2Segmenter::segmentBox(const QRect& box) {
    if (!impl_->hasEmbeddings_ || box.isEmpty()) {
        return {};
    }
    const float x0 = static_cast<float>(box.left()) * impl_->scale_;
    const float y0 = static_cast<float>(box.top()) * impl_->scale_;
    const float x1 = static_cast<float>(box.right()) * impl_->scale_;
    const float y1 = static_cast<float>(box.bottom()) * impl_->scale_;

    const std::vector<float> coords = {x0, y0, x1, y1};
    const std::vector<float> labels = {2.0f, 3.0f}; // 2: box top-left, 3: box bottom-right
    return impl_->runDecoder(coords, labels);
}

QImage Sam2Segmenter::segmentPoints(const std::vector<QPoint>& points,
                                    const std::vector<int>& labels) {
    if (!impl_->hasEmbeddings_ || points.empty() || points.size() != labels.size()) {
        return {};
    }
    std::vector<float> coords;
    std::vector<float> floatLabels;
    coords.reserve(points.size() * 2);
    floatLabels.reserve(labels.size());

    for (std::size_t i = 0; i < points.size(); ++i) {
        coords.push_back(static_cast<float>(points[i].x()) * impl_->scale_);
        coords.push_back(static_cast<float>(points[i].y()) * impl_->scale_);
        floatLabels.push_back(labels[i] > 0 ? 1.0f : 0.0f);
    }
    return impl_->runDecoder(coords, floatLabels);
}

QImage Sam2Segmenter::applyCutout(const QImage& original,
                                  const QImage& mask,
                                  const bool cropToBoundingBox,
                                  QRect* outBoundingBox) {
    if (outBoundingBox) {
        *outBoundingBox = QRect();
    }
    if (original.isNull() || mask.isNull() || original.size() != mask.size()) {
        return {};
    }

    QImage cutout = original.convertToFormat(QImage::Format_ARGB32);
    const QImage monoMask = mask.convertToFormat(QImage::Format_Grayscale8);

    int minX = cutout.width();
    int minY = cutout.height();
    int maxX = -1;
    int maxY = -1;
    bool solidFound = false;

    for (int y = 0; y < cutout.height(); ++y) {
        auto* scanPixels = reinterpret_cast<QRgb*>(cutout.scanLine(y));
        const uchar* scanMask = monoMask.constScanLine(y);
        for (int x = 0; x < cutout.width(); ++x) {
            const uchar maskValue = scanMask[x];
            const uchar edge = softAlphaFromMask(maskValue);
            // The bounding box includes the soft ramp, not just the solid core: a tight crop
            // that stopped at the core would slice the feather off and hand back a hard edge,
            // which is exactly the artefact this ramp exists to remove. The ramp is only one or
            // two source pixels wide, so the grown box stays honest.
            if (edge > 0) {
                minX = std::min(minX, x);
                minY = std::min(minY, y);
                maxX = std::max(maxX, x);
                maxY = std::max(maxY, y);
            }
            solidFound = solidFound || maskValue > kMaskHigh;
            if (edge == 0) {
                scanPixels[x] = qRgba(0, 0, 0, 0);
                continue;
            }
            // Preserve the source RGB and modulate only its alpha, so the foreground is not
            // darkened by the edge ramp. Round to the nearest byte to avoid a systematic
            // one-step darkening on fully opaque pixels.
            const QRgb source = scanPixels[x];
            const int alpha = (qAlpha(source) * static_cast<int>(edge) + 127) / 255;
            scanPixels[x] = qRgba(qRed(source), qGreen(source), qBlue(source), alpha);
        }
    }

    if (solidFound && minX <= maxX && minY <= maxY) {
        const QRect bbox(QPoint(minX, minY), QPoint(maxX, maxY));
        if (outBoundingBox) {
            *outBoundingBox = bbox;
        }
        if (cropToBoundingBox) {
            return cutout.copy(bbox);
        }
        return cutout;
    }
    // No solid pixel: report failure instead of handing back a fully transparent canvas that
    // a caller could mistake for a successful cutout.
    return {};
}

qreal Sam2Segmenter::solidCoverage(const QImage& mask) {
    if (mask.isNull()) {
        return 0.0;
    }
    const QImage monoMask = mask.convertToFormat(QImage::Format_Grayscale8);
    qint64 solid = 0;
    for (int y = 0; y < monoMask.height(); ++y) {
        const uchar* scan = monoMask.constScanLine(y);
        for (int x = 0; x < monoMask.width(); ++x) {
            if (scan[x] > kMaskHigh) {
                ++solid;
            }
        }
    }
    const qint64 total = static_cast<qint64>(monoMask.width()) * monoMask.height();
    return total > 0 ? static_cast<qreal>(solid) / static_cast<qreal>(total) : 0.0;
}

} // namespace dvs::ui
