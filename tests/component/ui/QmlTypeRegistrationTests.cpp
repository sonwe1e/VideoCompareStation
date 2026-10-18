#include "dvs/ui/ColorClipboardController.h"
#include "dvs/ui/ComparisonSurface.h"
#include "dvs/ui/GraphicsBackend.h"
#include "dvs/ui/HoldKeyHandler.h"
#include "dvs/ui/ImageReviewController.h"
#include "dvs/ui/JogKeyHandler.h"
#include "dvs/ui/QmlTypeRegistration.h"

#include <QCoreApplication>
#include <QFile>
#include <QGuiApplication>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QString>
#include <QStringList>
#include <QUrl>
#include <QVariant>
#include <QVariantMap>
#include <QtQml/qqml.h>

#include <gtest/gtest.h>
#include <memory>

namespace dvs::ui {
namespace {

[[nodiscard]] QString componentErrors(const QQmlComponent& component) {
    QStringList messages;
    for (const QQmlError& error : component.errors()) {
        messages.push_back(error.toString());
    }
    return messages.join(QStringLiteral("\n"));
}

// Collects Qt messages while it is alive. The registration contract is also about the
// warning a *failed* typed assignment produces, not only about the resulting null.
class MessageCapture final {
public:
    MessageCapture() {
        previous_ = qInstallMessageHandler(&MessageCapture::handle);
        active_ = this;
    }
    ~MessageCapture() {
        qInstallMessageHandler(previous_);
        active_ = nullptr;
    }
    MessageCapture(const MessageCapture&) = delete;
    MessageCapture& operator=(const MessageCapture&) = delete;

    [[nodiscard]] bool contains(const QString& needle) const {
        for (const QString& message : messages_) {
            if (message.contains(needle)) {
                return true;
            }
        }
        return false;
    }
    [[nodiscard]] QString joined() const {
        return messages_.join(QStringLiteral("\n"));
    }

private:
    static void handle(QtMsgType, const QMessageLogContext&, const QString& message) {
        if (active_ != nullptr) {
            active_->messages_.push_back(message);
        }
    }

    static inline MessageCapture* active_ = nullptr;
    QtMessageHandler previous_ = nullptr;
    QStringList messages_;
};

// The snippet intentionally uses only the probe surface of the pilot feature: a typed
// controller property, a numeric-format read, a formatting invokable and one enum.
constexpr auto kTypedControllerSnippet = R"qml(
import Dvs.Ui 1.0
import QtQml
QtObject {
    required property ImageReviewController controller
    readonly property string channel: controller.channelText(128)
    readonly property int formatValue: controller.numericFormat
    readonly property int probeRgb: ImageReviewController.ProbeRgb
}
)qml";

} // namespace

// The single registration entry is the only place the runtime surface is defined; calling
// it twice (composition root plus an embedding) must stay idempotent.
TEST(QmlTypeRegistrationTests, EveryDvsUiTypeIsRegisteredByTheSingleEntry) {
    registerDvsUiTypes();
    registerDvsUiTypes();
    for (const char* const name : {"ComparisonSurface",
                                   "JogKeyHandler",
                                   "HoldKeyHandler",
                                   "ImageReviewController",
                                   "ColorClipboardController"}) {
        EXPECT_GE(qmlTypeId("Dvs.Ui", 1, 0, name), 0) << name;
    }
}

// The pilot contract: a typed property naming the composition-root controller resolves,
// the invokable runs against the real controller, and the enum is addressable on the type.
TEST(QmlTypeRegistrationTests, TypedControllerPropertyResolvesAndRunsTheProbeInvokables) {
    registerDvsUiTypes();
    QQmlEngine engine;
    ImageReviewController controller;
    controller.setNumericFormat(ImageReviewController::NormalizedFloat);

    QQmlComponent component(&engine);
    component.setData(kTypedControllerSnippet, QUrl{QStringLiteral("qrc:/typed-controller.qml")});
    ASSERT_FALSE(component.isError()) << componentErrors(component).toStdString();

    std::unique_ptr<QObject> object(component.createWithInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(&controller)}}));
    ASSERT_NE(object, nullptr) << componentErrors(component).toStdString();
    EXPECT_EQ(object->property("channel").toString(), QStringLiteral("0.501961"));
    EXPECT_EQ(object->property("formatValue").toInt(),
              static_cast<int>(ImageReviewController::NormalizedFloat));
    EXPECT_EQ(object->property("probeRgb").toInt(),
              static_cast<int>(ImageReviewController::ProbeRgb));
}

// Removing the required dependency (here: not providing the controller at all) must fail
// component creation with the explicit Qt contract error instead of silently degrading.
TEST(QmlTypeRegistrationTests, MissingRequiredControllerFailsComponentCreation) {
    registerDvsUiTypes();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(kTypedControllerSnippet, QUrl{QStringLiteral("qrc:/typed-controller.qml")});
    ASSERT_FALSE(component.isError()) << componentErrors(component).toStdString();

    const std::unique_ptr<QObject> object(component.createWithInitialProperties({}));
    EXPECT_EQ(object, nullptr);
    const QString errors = componentErrors(component);
    EXPECT_TRUE(errors.contains(QStringLiteral("Required property controller was not "
                                               "initialized")))
        << errors.toStdString();
}

// Wiring the wrong object into the typed boundary is reported by Qt ("Could not set
// initial property") and leaves the property null; it must never silently become the
// controller. The warning is the observable signal the contract pins.
TEST(QmlTypeRegistrationTests, WrongTypedObjectCannotBecomeTheTypedController) {
    registerDvsUiTypes();
    QQmlEngine engine;
    QQmlEngine wrongObject; // A QObject, but not an ImageReviewController.
    QQmlComponent component(&engine);
    component.setData(kTypedControllerSnippet, QUrl{QStringLiteral("qrc:/typed-controller.qml")});
    ASSERT_FALSE(component.isError()) << componentErrors(component).toStdString();

    std::unique_ptr<QObject> object;
    {
        const MessageCapture capture;
        object.reset(component.createWithInitialProperties(
            {{QStringLiteral("controller"), QVariant::fromValue(&wrongObject)}}));
        EXPECT_TRUE(capture.contains(QStringLiteral("Could not set initial property")))
            << capture.joined().toStdString();
    }
    ASSERT_NE(object, nullptr); // Creation continues; the typed property is simply null.
    EXPECT_EQ(object->property("controller").value<QObject*>(), nullptr);
}

// The composition root owns the controller instances: QML can name the types but must not
// instantiate them. Both controllers are covered; each is rejected with the registration's
// uncreatable explanation.
TEST(QmlTypeRegistrationTests, ControllersAreNotCreatableFromQml) {
    registerDvsUiTypes();
    for (const char* const name : {"ImageReviewController", "ColorClipboardController"}) {
        QQmlEngine engine;
        QQmlComponent component(&engine);
        const QString qml = QStringLiteral("import Dvs.Ui 1.0\n"
                                           "import QtQuick\n"
                                           "Item {\n"
                                           "    %1 {\n"
                                           "    }\n"
                                           "}\n")
                                .arg(QString::fromLatin1(name));
        component.setData(qml.toUtf8(), QUrl{QStringLiteral("qrc:/uncreatable-controller.qml")});
        const std::unique_ptr<QObject> object(component.create());
        EXPECT_EQ(object, nullptr) << name;
        EXPECT_TRUE(componentErrors(component).contains(
            QStringLiteral("provided by the CompareStation composition root")))
            << name << ": " << componentErrors(component).toStdString();
    }
}

// ... and the types QML genuinely instantiates (the comparison surface, the jog and hold
// key handlers) stay creatable through the same entry.
TEST(QmlTypeRegistrationTests, CreatableTypesStayCreatable) {
    registerDvsUiTypes();
    QQmlEngine engine;
    QQmlComponent component(&engine);
    component.setData(R"qml(
import Dvs.Ui 1.0
import QtQuick
Item {
    ComparisonSurface {
    }
    JogKeyHandler {
    }
    HoldKeyHandler {
    }
}
)qml",
                      QUrl{QStringLiteral("qrc:/creatable-types.qml")});
    const std::unique_ptr<QObject> object(component.create());
    ASSERT_NE(object, nullptr) << componentErrors(component).toStdString();
    EXPECT_NE(object->findChild<ComparisonSurface*>(), nullptr);
    EXPECT_NE(object->findChild<JogKeyHandler*>(), nullptr);
    EXPECT_NE(object->findChild<HoldKeyHandler*>(), nullptr);
}

namespace {

// One QML export line, e.g. exports: ["Dvs.Ui/ColorClipboardController 1.0"]. This is the
// identity qmllint and the QML engine actually resolve. It is independent of the C++ class
// name in the "name:" field and can be renamed on its own through QML_NAMED_ELEMENT.
struct QmlExport {
    QString uri;
    QString typeName;
    int majorVersion = 0;
    int minorVersion = 0;
};

struct DescribedComponent {
    QString cppName;          // from name: "dvs::ui::<cppName>"
    QList<QmlExport> exports; // from exports: [...]
    bool creatable = true;
};

// Parses a single "URI/TypeName major.minor" export entry. Returns an export with an empty
// typeName when the entry is malformed, so the caller can reject it rather than guess.
[[nodiscard]] QmlExport parseQmlExport(const QString& entry) {
    QmlExport parsed;
    const int slash = entry.indexOf(QLatin1Char('/'));
    if (slash < 0) {
        return parsed;
    }
    parsed.uri = entry.left(slash).trimmed();
    QString rest = entry.mid(slash + 1).trimmed();
    QString version;
    const int space = rest.indexOf(QLatin1Char(' '));
    if (space >= 0) {
        version = rest.mid(space + 1).trimmed();
        rest = rest.left(space).trimmed();
    }
    parsed.typeName = rest;
    bool okMajor = false;
    bool okMinor = true;
    const int dot = version.indexOf(QLatin1Char('.'));
    if (dot >= 0) {
        parsed.majorVersion = version.left(dot).toInt(&okMajor);
        parsed.minorVersion = version.mid(dot + 1).toInt(&okMinor);
    } else {
        parsed.majorVersion = version.toInt(&okMajor);
    }
    if (parsed.typeName.isEmpty() || parsed.uri.isEmpty() || !okMajor || !okMinor) {
        return {};
    }
    return parsed;
}

// The generated description and the runtime registration are two declarations of the same
// module. This reads the artifact qmllint consumes and cross-checks it against the runtime
// registry. The QML identity is taken from each component's exports line (URI, type name,
// major/minor version), never derived from the C++ class name; the C++ name is kept as
// independent information so a rename of only the export, a dropped type, a shifted
// URI/version or a flipped creatability on either side fails instead of shipping a
// description that no longer matches what QML loads.
[[nodiscard]] QList<DescribedComponent> parseDescribedComponents(const QString& content) {
    QList<DescribedComponent> components;
    for (const QString& rawLine : content.split(QLatin1Char('\n'))) {
        const QString line = rawLine.trimmed();
        const QString namePrefix = QStringLiteral("name: \"dvs::ui::");
        if (line.startsWith(namePrefix)) {
            DescribedComponent component;
            component.cppName = line.mid(namePrefix.size(), line.size() - namePrefix.size() - 1);
            components.push_back(component);
            continue;
        }
        if (components.isEmpty()) {
            continue;
        }
        if (line == QLatin1String("isCreatable: false")) {
            components.last().creatable = false;
            continue;
        }
        const QString exportsPrefix = QStringLiteral("exports: [");
        if (line.startsWith(exportsPrefix)) {
            const int open = line.indexOf(QLatin1Char('['));
            const int close = line.lastIndexOf(QLatin1Char(']'));
            if (open >= 0 && close > open) {
                const QString inner = line.mid(open + 1, close - open - 1);
                for (const QString& rawEntry : inner.split(QLatin1Char(','))) {
                    QString entry = rawEntry.trimmed();
                    if (entry.startsWith(QLatin1Char('"'))) {
                        entry.remove(0, 1);
                    }
                    if (entry.endsWith(QLatin1Char('"'))) {
                        entry.chop(1);
                    }
                    const QmlExport parsed = parseQmlExport(entry);
                    if (!parsed.typeName.isEmpty()) {
                        components.last().exports.push_back(parsed);
                    }
                }
            }
        }
    }
    return components;
}

// The runtime registry id for a described export, queried by the export's own
// URI/version/name - the identity the engine resolves, not the C++ class name.
[[nodiscard]] int describedExportTypeId(const QmlExport& described) {
    return qmlTypeId(described.uri.toUtf8().constData(),
                     described.majorVersion,
                     described.minorVersion,
                     described.typeName.toUtf8().constData());
}

[[nodiscard]] bool instantiationSucceeds(const QString& typeName) {
    QQmlEngine engine;
    QQmlComponent component(&engine);
    const QString qml = QStringLiteral("import Dvs.Ui 1.0\n"
                                       "import QtQuick\n"
                                       "Item {\n"
                                       "    %1 {\n"
                                       "    }\n"
                                       "}\n")
                            .arg(typeName);
    component.setData(qml.toUtf8(), QUrl{QStringLiteral("qrc:/described-type.qml")});
    const std::unique_ptr<QObject> object(component.create());
    return object != nullptr;
}

} // namespace

TEST(QmlTypeRegistrationTests, GeneratedDescriptionMatchesTheRuntimeRegistration) {
    registerDvsUiTypes();
    QFile description{DVS_QMLTYPES_PATH};
    ASSERT_TRUE(description.open(QIODevice::ReadOnly)) << DVS_QMLTYPES_PATH;
    const QString content = QString::fromUtf8(description.readAll());
    const QList<DescribedComponent> described = parseDescribedComponents(content);

    // The expected module surface, spelled once. The generated description must describe
    // exactly these types (dropping one, renaming one or adding a stray one fails), and
    // every one of them must answer at runtime under the identity its exports line declares.
    const QStringList expectedNames{QStringLiteral("ColorClipboardController"),
                                    QStringLiteral("ComparisonSurface"),
                                    QStringLiteral("HoldKeyHandler"),
                                    QStringLiteral("ImageReviewController"),
                                    QStringLiteral("JogKeyHandler")};
    QStringList sortedExpected = expectedNames;
    sortedExpected.sort();

    // The C++ class names stay independent information: exactly the five expected.
    QStringList describedCppNames;
    for (const DescribedComponent& component : described) {
        describedCppNames.push_back(component.cppName);
    }
    describedCppNames.sort();
    EXPECT_EQ(describedCppNames, sortedExpected) << content.toStdString();

    // The QML export identity is what the engine and qmllint resolve. Each described
    // component must export into Dvs.Ui at the registered 1.0 version; that exact export
    // must be registered at runtime (queried by URI/version/name, never by the C++ name)
    // and must instantiate exactly per its described creatability.
    QStringList describedQmlNames;
    for (const DescribedComponent& component : described) {
        const QmlExport* dvsExport = nullptr;
        for (const QmlExport& candidate : component.exports) {
            if (candidate.uri == QLatin1String("Dvs.Ui") && candidate.majorVersion == 1 &&
                candidate.minorVersion == 0) {
                dvsExport = &candidate;
                break;
            }
        }
        ASSERT_TRUE(dvsExport != nullptr)
            << component.cppName.toStdString()
            << " must export into Dvs.Ui 1.0; its exports line no longer matches the module";
        EXPECT_GE(describedExportTypeId(*dvsExport), 0)
            << component.cppName.toStdString() << " exports Dvs.Ui/"
            << dvsExport->typeName.toStdString() << " but no such type is registered";
        EXPECT_EQ(instantiationSucceeds(dvsExport->typeName), component.creatable)
            << dvsExport->typeName.toStdString();
        describedQmlNames.push_back(dvsExport->typeName);
    }

    // The set of QML export names must be exactly the five expected, distinct names.
    describedQmlNames.sort();
    EXPECT_EQ(describedQmlNames, sortedExpected) << content.toStdString();
}

// Counterexample for the consistency check: a description whose QML export was renamed (or
// whose URI/version shifted) while its C++ class name stayed the same must be seen as drift.
// The check keys on the export identity, so the moved export resolves to nothing at runtime
// instead of silently passing on the unchanged C++ name. The very query the real-artifact
// check runs returns < 0 here, which is what turns this into a failing consistency check.
TEST(QmlTypeRegistrationTests, RenamedOrVersionShiftedExportIsNotRegisteredAndIsDetected) {
    registerDvsUiTypes();
    // Only the exports lines move; every "name:" C++ class name is unchanged.
    const QString drifted = QStringLiteral("Module {\n"
                                           "    Component {\n"
                                           "        name: \"dvs::ui::ColorClipboardController\"\n"
                                           "        exports: [\"Dvs.Ui/ClipboardRenamed 1.0\"]\n"
                                           "        isCreatable: false\n"
                                           "    }\n"
                                           "    Component {\n"
                                           "        name: \"dvs::ui::ComparisonSurface\"\n"
                                           "        exports: [\"Dvs.Ui/ComparisonSurface 2.0\"]\n"
                                           "    }\n"
                                           "}\n");
    const QList<DescribedComponent> described = parseDescribedComponents(drifted);
    ASSERT_EQ(described.size(), 2);

    // The parse kept the unchanged C++ class names ...
    EXPECT_EQ(described[0].cppName, QStringLiteral("ColorClipboardController"));
    EXPECT_EQ(described[1].cppName, QStringLiteral("ComparisonSurface"));

    // ... but read the moved QML identity from the exports line.
    ASSERT_EQ(described[0].exports.size(), 1);
    EXPECT_EQ(described[0].exports.first().typeName, QStringLiteral("ClipboardRenamed"));
    ASSERT_EQ(described[1].exports.size(), 1);
    EXPECT_EQ(described[1].exports.first().majorVersion, 2);

    // Neither drifted export is registered at runtime: the same query the consistency test
    // runs on the real artifact returns < 0, so this drift is detected, not passed through.
    EXPECT_LT(describedExportTypeId(described[0].exports.first()), 0)
        << "a renamed export with no matching registration must not resolve";
    EXPECT_LT(describedExportTypeId(described[1].exports.first()), 0)
        << "a version-shifted export with no matching registration must not resolve";
}

} // namespace dvs::ui

int main(int argumentCount, char* arguments[]) {
    dvs::ui::configureGraphicsBackend();
    QGuiApplication application{argumentCount, arguments};
    testing::InitGoogleTest(&argumentCount, arguments);
    return RUN_ALL_TESTS();
}
