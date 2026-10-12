#pragma once

#include "dvs/application/FileAssociation.h"

#include <QObject>
#include <QString>

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>

namespace dvs::ui {

// QML face of the Windows file-association settings dialog: the four actions (register as a
// choosable application, jump to system default-apps settings, repair paths after a ZIP move,
// remove this application's registration) plus the read-back status that decides what the
// dialog reports. The controller owns no association state itself; associationStatus() is the
// only authority, and "the settings page opened" is never surfaced as success.
//
// Threading: registry work and the settings launch run on a QThreadPool worker - the GUI thread
// never waits on the registry - and results hop back through queued meta-calls. One action at a
// time; the destructor waits for an in-flight worker, and a queued result that outlives the
// controller is discarded by Qt's posted-event cleanup, so no completion can land on a dead
// object.
class FileAssociationController final : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool busy READ busy NOTIFY stateChanged)
    Q_PROPERTY(QString lastError READ lastError NOTIFY stateChanged)
    // FileAssociationHealth: 0 not registered, 1 current, 2 registered but pointing at another
    // path (a moved or upgraded ZIP - repair rewrites it).
    Q_PROPERTY(int registrationHealth READ registrationHealth NOTIFY stateChanged)
    // DefaultAppOwnership: 0 no UserChoice, 1 every extension of the kind opens here,
    // 2 another application owns part or all of the kind.
    Q_PROPERTY(int videoDefault READ videoDefaultOwnership NOTIFY stateChanged)
    Q_PROPERTY(int imageDefault READ imageDefaultOwnership NOTIFY stateChanged)
public:
    explicit FileAssociationController(
        std::shared_ptr<application::IFileAssociationService> service, QObject* parent = nullptr);
    ~FileAssociationController() override;

    FileAssociationController(const FileAssociationController&) = delete;
    FileAssociationController& operator=(const FileAssociationController&) = delete;

    [[nodiscard]] bool busy() const noexcept;
    [[nodiscard]] QString lastError() const noexcept;
    [[nodiscard]] int registrationHealth() const noexcept;
    [[nodiscard]] int videoDefaultOwnership() const noexcept;
    [[nodiscard]] int imageDefaultOwnership() const noexcept;

    // Each action is refused while another one is running. Completion updates the read-back
    // properties and lastError; the dialog renders them.
    Q_INVOKABLE void registerApplication();
    Q_INVOKABLE void openSystemSettings();
    Q_INVOKABLE void refreshStatus();
    Q_INVOKABLE void repairPaths();
    Q_INVOKABLE void removeRegistration();

signals:
    void stateChanged();

private:
    struct Shutdown final {
        std::mutex mutex;
        std::condition_variable done;
        int inFlight{0};
        bool stopped{false};
    };

    // Runs `action` on the worker pool, then reads the association status on the same worker
    // and applies both to the properties on the GUI thread.
    void runAsync(std::function<application::FileAssociationOutcome()> action);

    std::shared_ptr<application::IFileAssociationService> service_;
    application::FileAssociationStatus status_;
    QString lastError_;
    bool busy_{false};
    Shutdown shutdown_;
};

} // namespace dvs::ui
