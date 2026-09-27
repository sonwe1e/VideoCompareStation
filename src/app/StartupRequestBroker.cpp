#include "StartupRequestBroker.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QThread>

#include <cstddef>
#include <deque>
#include <utility>

namespace dvs::app {
namespace {

constexpr qsizetype kMaximumFrameBytes = 64 * 1024;
constexpr int kConnectAttemptMilliseconds = 50;
constexpr int kConnectWindowMilliseconds = 1000;
constexpr int kAcknowledgementTimeoutMilliseconds = 1000;
constexpr int kElectionWindowMilliseconds = 1000;
constexpr int kElectionAttemptMilliseconds = 10;
constexpr std::size_t kMaximumPendingRequests = 8U;

[[nodiscard]] QString serverName() {
    const QByteArray userIdentity =
        QCryptographicHash::hash(QDir::homePath().toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .first(16);
    return QStringLiteral("CompareStation.StartupRequest.v1.%1")
        .arg(QString::fromLatin1(userIdentity));
}

[[nodiscard]] QString lockFilePath(const QString& endpointName) {
    const QByteArray endpointHash =
        QCryptographicHash::hash(endpointName.toUtf8(), QCryptographicHash::Sha256)
            .toHex()
            .first(24);
    return QDir::temp().filePath(QStringLiteral("CompareStation.StartupRequest.%1.lock")
                                     .arg(QString::fromLatin1(endpointHash)));
}

} // namespace

class StartupRequestBroker::Impl final {
public:
    explicit Impl(StartupRequestBroker& owner, QString endpointName)
        : owner_(owner), endpointName_(std::move(endpointName)),
          electionLock_(lockFilePath(endpointName_)) {
        QObject::connect(
            &server_, &QLocalServer::newConnection, &owner_, [this] { acceptConnections(); });
    }

    [[nodiscard]] StartResult startOrForward(const StartupRequest& request) {
        if (server_.isListening()) {
            return StartResult::Primary;
        }
        if (electionLock_.tryLock(0)) {
            if (tryBecomePrimary()) {
                return StartResult::Primary;
            }
            electionLock_.unlock();
            return StartResult::Failed;
        }

        QLocalSocket socket;
        QElapsedTimer connectWindow;
        connectWindow.start();
        do {
            socket.abort();
            socket.connectToServer(endpointName_, QIODevice::ReadWrite);
            if (socket.waitForConnected(kConnectAttemptMilliseconds)) {
                break;
            }
            QThread::msleep(10U);
        } while (connectWindow.elapsed() < kConnectWindowMilliseconds);
        if (socket.state() == QLocalSocket::ConnectedState) {
            QByteArray payload = encodeStartupRequest(request);
            if (payload.isEmpty()) {
                return StartResult::Failed;
            }
            payload.push_back('\n');
            if (socket.write(payload) != payload.size()) {
                return StartResult::Failed;
            }
            static_cast<void>(socket.flush());
            QByteArray acknowledgement;
            QElapsedTimer acknowledgementWindow;
            acknowledgementWindow.start();
            do {
                if (socket.bytesAvailable() == 0) {
                    static_cast<void>(socket.waitForReadyRead(kConnectAttemptMilliseconds));
                }
                acknowledgement.append(socket.readAll());
            } while (!acknowledgement.contains('\n') &&
                     socket.state() != QLocalSocket::UnconnectedState &&
                     acknowledgementWindow.elapsed() < kAcknowledgementTimeoutMilliseconds);
            socket.disconnectFromServer();
            return acknowledgement == QByteArrayLiteral("OK\n") ? StartResult::Forwarded
                                                                : StartResult::Failed;
        }

        // Nobody answered on the endpoint, but the election lock is held. Its owner may be a
        // process that has taken the lock and has not started listening yet, or one that is going
        // away; wait out the same window the connect path tolerates before declaring the start
        // failed.
        QElapsedTimer electionWindow;
        electionWindow.start();
        do {
            if (electionLock_.tryLock(0)) {
                if (tryBecomePrimary()) {
                    return StartResult::Primary;
                }
                electionLock_.unlock();
                return StartResult::Failed;
            }
            QThread::msleep(kElectionAttemptMilliseconds);
        } while (electionWindow.elapsed() < kElectionWindowMilliseconds);
        return StartResult::Failed;
    }

    void setRequestHandler(std::function<bool(StartupRequest)> handler) {
        handler_ = std::move(handler);
        if (!handler_) {
            return;
        }
        while (!pendingRequests_.empty()) {
            StartupRequest request = std::move(pendingRequests_.front());
            pendingRequests_.pop_front();
            static_cast<void>(handler_(std::move(request)));
        }
    }

private:
    // A previous instance that crashed, or that was killed, leaves its endpoint name behind for a
    // moment while the operating system tears the local server down, so `listen` can fail even
    // though the election lock is ours. Retry inside the window the connect path already tolerates:
    // refusing on the first attempt turned "relaunch right after a crash" into a fatal startup
    // error.
    [[nodiscard]] bool tryBecomePrimary() {
        QElapsedTimer listenWindow;
        listenWindow.start();
        do {
            static_cast<void>(QLocalServer::removeServer(endpointName_));
            if (server_.listen(endpointName_)) {
                return true;
            }
            QThread::msleep(kElectionAttemptMilliseconds);
        } while (listenWindow.elapsed() < kElectionWindowMilliseconds);
        return false;
    }

    void acceptConnections() {
        while (QLocalSocket* socket = server_.nextPendingConnection()) {
            buffers_.insert(socket, {});
            QObject::connect(
                socket, &QLocalSocket::readyRead, &owner_, [this, socket] { readFrom(*socket); });
            QObject::connect(socket, &QLocalSocket::disconnected, &owner_, [this, socket] {
                buffers_.remove(socket);
                socket->deleteLater();
            });
            readFrom(*socket);
        }
    }

    void readFrom(QLocalSocket& socket) {
        auto iterator = buffers_.find(&socket);
        if (iterator == buffers_.end()) {
            return;
        }
        iterator.value().append(socket.readAll());
        if (iterator.value().size() > kMaximumFrameBytes + 1) {
            socket.disconnectFromServer();
            return;
        }
        const qsizetype terminator = iterator.value().indexOf('\n');
        if (terminator < 0) {
            return;
        }
        const QByteArray payload = iterator.value().first(terminator);
        iterator.value().remove(0, terminator + 1);
        const StartupRequestParseResult decoded = decodeStartupRequest(payload);
        if (decoded && (handler_ || pendingRequests_.size() < kMaximumPendingRequests)) {
            bool accepted = true;
            if (handler_) {
                accepted = handler_(std::move(*decoded.request));
            } else {
                pendingRequests_.push_back(std::move(*decoded.request));
            }
            if (accepted) {
                static_cast<void>(socket.write(QByteArrayLiteral("OK\n")));
                static_cast<void>(socket.flush());
            }
        }
        socket.disconnectFromServer();
    }

    StartupRequestBroker& owner_;
    QString endpointName_;
    QLockFile electionLock_;
    QLocalServer server_;
    QHash<QLocalSocket*, QByteArray> buffers_;
    std::function<bool(StartupRequest)> handler_;
    std::deque<StartupRequest> pendingRequests_;
};

StartupRequestBroker::StartupRequestBroker(QObject* const parent)
    : StartupRequestBroker(serverName(), parent) {}

StartupRequestBroker::StartupRequestBroker(QString endpointName, QObject* const parent)
    : QObject(parent), impl_(std::make_unique<Impl>(*this, std::move(endpointName))) {}

StartupRequestBroker::~StartupRequestBroker() = default;

StartupRequestBroker::StartResult
StartupRequestBroker::startOrForward(const StartupRequest& request) {
    return impl_->startOrForward(request);
}

void StartupRequestBroker::setRequestHandler(std::function<bool(StartupRequest)> handler) {
    impl_->setRequestHandler(std::move(handler));
}

} // namespace dvs::app
