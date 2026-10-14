#include "dvs/ui/FileAssociationController.h"

#include <QMetaObject>
#include <QThreadPool>

#include <functional>

namespace dvs::ui {

FileAssociationController::FileAssociationController(
    std::shared_ptr<application::IFileAssociationService> service, QObject* parent)
    : QObject{parent}, service_{std::move(service)} {}

FileAssociationController::~FileAssociationController() {
    // An in-flight worker holds only copies plus the shutdown bookkeeping; waiting here means
    // no queued continuation can touch members after destruction begins. Qt purges the posted
    // event if the controller is gone before delivery.
    std::unique_lock lock(shutdown_.mutex);
    shutdown_.stopped = true;
    shutdown_.done.wait(lock, [this] { return shutdown_.inFlight == 0; });
}

bool FileAssociationController::busy() const noexcept {
    return busy_;
}

QString FileAssociationController::lastError() const noexcept {
    return lastError_;
}

QString FileAssociationController::lastResult() const noexcept {
    return lastResult_;
}

int FileAssociationController::registrationHealth() const noexcept {
    return static_cast<int>(status_.health);
}

int FileAssociationController::videoDefaultOwnership() const noexcept {
    return static_cast<int>(status_.videoDefault);
}

int FileAssociationController::imageDefaultOwnership() const noexcept {
    return static_cast<int>(status_.imageDefault);
}

void FileAssociationController::registerApplication() {
    runAsync([service = service_] { return service->registerAsOptionalApplication(); },
             QStringLiteral("已注册为可选应用；默认打开方式仍需在系统设置中选择本应用。"));
}

void FileAssociationController::openSystemSettings() {
    runAsync([service = service_] { return service->openSystemDefaultAppsSettings(); },
             QStringLiteral("已打开系统默认应用设置；在系统中选择本应用后，回到此处刷新状态。"));
}

void FileAssociationController::refreshStatus() {
    // A read-back is an action with no write: the same one-worker round trip, an empty action,
    // and a status the dialog can trust because it came from UserChoice, not from hope.
    runAsync([] { return application::FileAssociationOutcome{true, {}}; },
             QStringLiteral("状态已刷新。"));
}

void FileAssociationController::repairPaths() {
    runAsync([service = service_] { return service->repairRegistrationPaths(); },
             QStringLiteral("注册路径已修复为当前程序位置。"));
}

void FileAssociationController::removeRegistration() {
    runAsync([service = service_] { return service->removeRegistration(); },
             QStringLiteral("已移除本应用的注册。"));
}

void FileAssociationController::runAsync(
    std::function<application::FileAssociationOutcome()> action, QString successMessage) {
    if (busy_ || service_ == nullptr) {
        return;
    }
    busy_ = true;
    emit stateChanged();
    {
        std::lock_guard lock(shutdown_.mutex);
        if (shutdown_.stopped) {
            busy_ = false;
            emit stateChanged();
            return;
        }
        ++shutdown_.inFlight;
    }
    QThreadPool::globalInstance()->start([this,
                                          service = service_,
                                          action = std::move(action),
                                          successMessage = std::move(successMessage)] {
        const application::FileAssociationOutcome outcome = action();
        // The status read-back rides the same worker round trip, so the properties the dialog
        // shows after an action come from UserChoice and the registry, never from the action's
        // own claim of success.
        const application::FileAssociationStatus status = service->associationStatus();
        QMetaObject::invokeMethod(
            this,
            // The slot copies two small value types and emits; Qt terminates on OOM by design,
            // the same reasoning the automation helpers in DesktopApplication use. The outer
            // lambda's captured copy is const, so the inner one copies - QString is shared.
            // NOLINTBEGIN(bugprone-exception-escape)
            [this, outcome, status, successMessage] {
                status_ = status;
                lastError_ = outcome.ok ? QString{} : QString::fromStdString(outcome.error);
                lastResult_ = outcome.ok ? successMessage : QString{};
                busy_ = false;
                emit stateChanged();
                // NOLINTEND(bugprone-exception-escape)
            },
            Qt::QueuedConnection);
        {
            // Notifying under the lock lets the destructor destroy the condition variable the
            // moment wait() returns, without racing a late notify.
            std::lock_guard lock(shutdown_.mutex);
            --shutdown_.inFlight;
            shutdown_.done.notify_all();
        }
    });
}

} // namespace dvs::ui
