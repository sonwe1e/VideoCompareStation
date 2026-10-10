#include "dvs/ui/InterpolationProtocolController.h"

#include "dvs/ui/ReviewController.h"

#include <QMetaObject>
#include <QThreadPool>

#include <filesystem>
#include <utility>

namespace dvs::ui {
namespace {

using domain::FrameId;
using domain::GeneratedFrame;
using domain::MissingFrame;
using domain::RetainedFrame;
using domain::UnknownFrame;

[[nodiscard]] quint64 displayedIndex(const std::int64_t internalIndex) {
    // F01: internal indices are zero-based, UI text is one-based; the conversion lives here so
    // no caller can mix the two identity spaces.
    return static_cast<quint64>(internalIndex + 1);
}

} // namespace

InterpolationProtocolController::InterpolationProtocolController(
    application::IInterpolationProtocolRepository* repository, QObject* parent)
    : QObject(parent), repository_(repository) {}

InterpolationProtocolController::~InterpolationProtocolController() {
    // Mark any in-flight worker result dead. The queued delivery may still run afterwards, but
    // Qt drops events whose receiver is gone, and the request id check below discards the rest.
    if (inFlightLoad_) {
        inFlightLoad_->cancelled.store(true, std::memory_order_release);
    }
}

void InterpolationProtocolController::setReviewController(ReviewController* controller) noexcept {
    if (review_ == controller) {
        return;
    }
    if (review_ != nullptr) {
        disconnect(review_, nullptr, this, nullptr);
    }
    review_ = controller;
    if (review_ != nullptr) {
        connect(review_, &ReviewController::frameStateChanged, this, [this] {
            refreshOriginProjection();
        });
    }
    refreshOriginProjection();
}

bool InterpolationProtocolController::protocolLoaded() const noexcept {
    return protocol_.has_value();
}

QString InterpolationProtocolController::lastError() const {
    return lastError_;
}

QString InterpolationProtocolController::currentOriginKind() const {
    return originKind_;
}

QString InterpolationProtocolController::currentOriginBadge() const {
    return originBadge_;
}

bool InterpolationProtocolController::canGoNextGenerated() const {
    if (!protocol_ || review_ == nullptr) {
        return false;
    }
    return protocol_->nextGeneratedFrame(FrameId{review_->currentFrame()}).has_value();
}

bool InterpolationProtocolController::canGoPreviousGenerated() const {
    if (!protocol_ || review_ == nullptr) {
        return false;
    }
    return protocol_->previousGeneratedFrame(FrameId{review_->currentFrame()}).has_value();
}

bool InterpolationProtocolController::loadProtocol(const QUrl& url) {
    if (repository_ == nullptr) {
        lastError_ = QStringLiteral("protocol import adapter missing");
        emit protocolChanged();
        return false;
    }
    const QString localPath = url.isLocalFile() ? url.toLocalFile() : QString{};
    if (localPath.isEmpty()) {
        lastError_ = QStringLiteral("protocol path must be a local file");
        emit protocolChanged();
        return false;
    }
    // Invalidate any earlier worker: only the newest request id may deliver.
    if (inFlightLoad_) {
        inFlightLoad_->cancelled.store(true, std::memory_order_release);
    }
    const quint64 requestId = ++loadRequestId_;
    auto request = std::make_shared<LoadRequest>();
    inFlightLoad_ = request;
    lastError_.clear();
    // The repository is const and stateless, so the worker may call it directly. The path and
    // the raw repository pointer are captured by value; the composition root keeps both alive
    // for the whole application lifetime, matching how every other controller adapter is held.
    const std::filesystem::path path{localPath.toStdWString()};
    application::IInterpolationProtocolRepository* const repository = repository_;
    // Thread entry copies the result and queues it; the repository decodes defensively and Qt
    // terminates on OOM, matching the splash/preferences worker precedent.
    // NOLINTBEGIN(bugprone-exception-escape)
    QThreadPool::globalInstance()->start([this, request, requestId, path, repository] {
        application::InterpolationProtocolIoResult result = repository->load(path);
        if (request->cancelled.load(std::memory_order_acquire)) {
            return;
        }
        QMetaObject::invokeMethod(
            this,
            [this, requestId, result]() { deliverLoadResult(requestId, result); },
            Qt::QueuedConnection);
    });
    // NOLINTEND(bugprone-exception-escape)
    emit protocolChanged();
    return true;
}

void InterpolationProtocolController::deliverLoadResult(
    quint64 requestId, application::InterpolationProtocolIoResult result) {
    if (requestId != loadRequestId_) {
        // Superseded by a newer load or a clearProtocol; the newer owner keeps inFlightLoad_.
        return;
    }
    inFlightLoad_.reset();
    if (!result.ok) {
        // A failed import never clears the protocol already in place.
        lastError_ = QString::fromStdString(result.error);
        emit protocolChanged();
        return;
    }
    protocol_ = std::move(result.protocol).value();
    lastError_.clear();
    emit protocolChanged();
    refreshOriginProjection();
}

void InterpolationProtocolController::clearProtocol() {
    ++loadRequestId_;
    if (inFlightLoad_) {
        inFlightLoad_->cancelled.store(true, std::memory_order_release);
        inFlightLoad_.reset();
    }
    protocol_.reset();
    lastError_.clear();
    emit protocolChanged();
    refreshOriginProjection();
}

bool InterpolationProtocolController::nextGeneratedFrame() {
    if (!protocol_ || review_ == nullptr) {
        return false;
    }
    const auto target = protocol_->nextGeneratedFrame(FrameId{review_->currentFrame()});
    if (!target.has_value()) {
        return false;
    }
    // Explicit user navigation through the existing precise-frame command chain; playback rate
    // and cadence are untouched.
    return review_->seekFrame(target->value());
}

bool InterpolationProtocolController::previousGeneratedFrame() {
    if (!protocol_ || review_ == nullptr) {
        return false;
    }
    const auto target = protocol_->previousGeneratedFrame(FrameId{review_->currentFrame()});
    if (!target.has_value()) {
        return false;
    }
    return review_->seekFrame(target->value());
}

QString InterpolationProtocolController::originKindAtCurrentFrame() const {
    return currentOriginKind();
}

void InterpolationProtocolController::refreshOriginProjection() {
    QString kind;
    QString badge;
    if (protocol_ && review_ != nullptr) {
        const auto origin = protocol_->originAt(FrameId{review_->currentFrame()});
        if (origin) {
            if (const auto* retained = std::get_if<RetainedFrame>(&origin.value())) {
                kind = QStringLiteral("retained");
                badge = QStringLiteral("保留帧 · GT %1").arg(
                    displayedIndex(retained->gtFrame.value()));
            } else if (const auto* generated = std::get_if<GeneratedFrame>(&origin.value())) {
                kind = QStringLiteral("generated");
                badge = QStringLiteral("生成帧 · 输入 GT %1/%2 · 比例 %3/%4")
                            .arg(displayedIndex(generated->input0GtFrame.value()))
                            .arg(displayedIndex(generated->input1GtFrame.value()))
                            .arg(generated->targetRatioNumerator)
                            .arg(generated->targetRatioDenominator);
            } else if (std::holds_alternative<UnknownFrame>(origin.value())) {
                kind = QStringLiteral("unknown");
                badge = QStringLiteral("未知来源");
            } else if (const auto* missing = std::get_if<MissingFrame>(&origin.value())) {
                kind = QStringLiteral("missing");
                badge = missing->expectedGtFrame.has_value()
                            ? QStringLiteral("缺失帧 · 预期 GT %1").arg(
                                  displayedIndex(missing->expectedGtFrame->value()))
                            : QStringLiteral("缺失帧");
            }
        } else if (review_->currentFrame() >= 0) {
            badge = QStringLiteral("当前帧不在协议内");
        }
    }
    if (kind != originKind_ || badge != originBadge_) {
        originKind_ = kind;
        originBadge_ = badge;
        emit currentOriginChanged();
    }
}

QString InterpolationProtocolController::buildStatusText() const {
    if (inFlightLoad_) {
        return QStringLiteral("正在读取协议…");
    }
    if (!protocol_) {
        return QStringLiteral("无协议（协议导航关闭）");
    }
    const qint64 outputPositions = protocol_->frameCount();
    if (protocol_->gtFrameCount() > 0) {
        return QStringLiteral("协议已载入 · %1 个输出位 · GT %2 帧")
            .arg(static_cast<quint64>(outputPositions))
            .arg(static_cast<quint64>(protocol_->gtFrameCount()));
    }
    return QStringLiteral("协议已载入 · %1 个输出位 · GT 帧数未知")
        .arg(static_cast<quint64>(outputPositions));
}

QString InterpolationProtocolController::statusText() const {
    return buildStatusText();
}

} // namespace dvs::ui
