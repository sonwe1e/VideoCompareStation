#include "dvs/ui/ComparisonSurface.h"

#include <QCoreApplication>
#include <QDir>
#include <QKeyEvent>
#include <QMetaObject>
#include <QMouseEvent>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQmlProperty>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <QUrl>
#include <QVariantList>
#include <QVariantMap>

#include <gtest/gtest.h>
#include <memory>

namespace dvs::ui {
namespace {

class ViewportPixelScaleTests : public ::testing::Test {
protected:
    void SetUp() override {
        // Same import-path contract as MainQmlContractTests: the engine's default import
        // paths do not include the qml/ tree deployed next to the test binary, so without
        // this the QtQuick.Controls import fails with "module is not installed".
        engine_.addImportPath(
            QDir{QCoreApplication::applicationDirPath()}.filePath(QStringLiteral("qml")));
        // Exercise real viewport bindings and surface geometry without renderer services,
        // media, workers or playback. Pointer and keyboard cases add isolated Qt windows.
        QQmlComponent component{&engine_, QUrl{QStringLiteral("qrc:/qml/ComparisonViewport.qml")}};
        ASSERT_EQ(component.status(), QQmlComponent::Ready)
            << component.errorString().toStdString();
        const QVariantMap properties{
            {QStringLiteral("width"), 800},
            {QStringLiteral("height"), 600},
            {QStringLiteral("preferences"), QVariant::fromValue<QObject*>(nullptr)},
            {QStringLiteral("borderColor"), QStringLiteral("#000000")},
            {QStringLiteral("accentColor"), QStringLiteral("#ffffff")},
            {QStringLiteral("primaryTextColor"), QStringLiteral("#ffffff")},
            {QStringLiteral("mutedTextColor"), QStringLiteral("#888888")},
            {QStringLiteral("chromeVisible"), true},
            {QStringLiteral("effectiveViewMode"), ComparisonSurface::Single},
            {QStringLiteral("wipePosition"), 0.5},
            {QStringLiteral("selectedDifferenceExactness"), 0},
            {QStringLiteral("selectedDifferenceEdge"), QVariantMap{}},
            {QStringLiteral("differenceThresholdEnabled"), false},
            {QStringLiteral("differenceThresholdCode"), 0},
            {QStringLiteral("differenceThresholdPolicy"), 0},
            {QStringLiteral("differenceSuppressed"), false},
            {QStringLiteral("referenceSourceIndex"), 0},
            {QStringLiteral("sourceCount"), 1},
            {QStringLiteral("wipeMode"), false},
            {QStringLiteral("differenceMode"), false},
            {QStringLiteral("analysisGridMode"), false},
            {QStringLiteral("immersiveHudVisible"), false},
            {QStringLiteral("immersiveHudText"), QString{}},
            {QStringLiteral("showFramePending"), false},
            {QStringLiteral("currentFrame"), 0},
            {QStringLiteral("differenceUnavailableDetail"), QString{}},
            {QStringLiteral("combinedAlignmentStatus"), QString{}},
            {QStringLiteral("singleMode"), true},
            {QStringLiteral("differenceFirstSlot"), 0},
            {QStringLiteral("effectiveDifferenceEdge"), ComparisonSurface::Edge0And1},
            {QStringLiteral("sourceNames"), QVariantList{}},
            {QStringLiteral("sourceParentLabels"), QVariantList{}},
            {QStringLiteral("sourceFullPaths"), QVariantList{}},
            {QStringLiteral("sourceMediaInfo"), QVariantList{}},
            {QStringLiteral("frameErrorBannerVisible"), false},
            {QStringLiteral("errorDetail"), QString{}},
            {QStringLiteral("overlayVisible"), false},
            {QStringLiteral("hasErrors"), false},
            {QStringLiteral("busy"), false},
            {QStringLiteral("overlayTitle"), QString{}},
            {QStringLiteral("overlayDetail"), QString{}},
        };
        root_.reset(component.createWithInitialProperties(properties));
        ASSERT_NE(root_, nullptr) << component.errorString().toStdString();
        surface_ = root_->findChild<ComparisonSurface*>(QStringLiteral("dualVideoSurface"));
        badge_ = root_->findChild<QQuickItem*>(QStringLiteral("viewportPixelScaleBadge"));
        label_ = root_->findChild<QObject*>(QStringLiteral("viewportPixelScaleLabel"));
        ASSERT_NE(surface_, nullptr);
        ASSERT_NE(badge_, nullptr);
        ASSERT_NE(label_, nullptr);
    }

    static QVariantMap source(const int width,
                              const int height,
                              const int rotation = 0,
                              const int sarNumerator = 1,
                              const int sarDenominator = 1) {
        return {{QStringLiteral("width"), width},
                {QStringLiteral("height"), height},
                {QStringLiteral("rotationDegrees"), rotation},
                {QStringLiteral("sampleAspectNumerator"), sarNumerator},
                {QStringLiteral("sampleAspectDenominator"), sarDenominator}};
    }

    void setSources(const QVariantList& sources) {
        root_->setProperty("sourceMediaInfo", sources);
        root_->setProperty("sourceCount", sources.size());
        QCoreApplication::processEvents();
    }

    [[nodiscard]] double percent() const {
        return badge_->property("effectivePercent").toDouble();
    }

    void clickBadge() {
        ASSERT_TRUE(QMetaObject::invokeMethod(badge_, "clicked"));
        QCoreApplication::processEvents();
    }

    void clickBadgeThroughWindow() {
        ASSERT_NE(window_, nullptr);
        ASSERT_TRUE(badge_->isVisible());
        ASSERT_TRUE(badge_->isEnabled());
        ASSERT_GT(badge_->width(), 0.0);
        ASSERT_GT(badge_->height(), 0.0);
        const QPointF position =
            badge_->mapToScene(QPointF{badge_->width() * 0.5, badge_->height() * 0.5});
        const QPointF globalPosition = window_->mapToGlobal(position);
        QMouseEvent press{QEvent::MouseButtonPress,
                          position,
                          position,
                          globalPosition,
                          Qt::LeftButton,
                          Qt::LeftButton,
                          Qt::NoModifier};
        QCoreApplication::sendEvent(window_.get(), &press);
        QMouseEvent release{QEvent::MouseButtonRelease,
                            position,
                            position,
                            globalPosition,
                            Qt::LeftButton,
                            Qt::NoButton,
                            Qt::NoModifier};
        QCoreApplication::sendEvent(window_.get(), &release);
        QCoreApplication::processEvents();
    }

    QQmlEngine engine_;
    std::unique_ptr<QQuickWindow> window_;
    std::unique_ptr<QObject> root_;
    ComparisonSurface* surface_ = nullptr;
    QQuickItem* badge_ = nullptr;
    QObject* label_ = nullptr;
};

TEST_F(ViewportPixelScaleTests, PortraitFitAndClickUseVideoContentInsteadOfLetterbox) {
    setSources({source(1080, 1920)});
    const double fittedPercent = surface_->height() / 1920.0 * 100.0;
    EXPECT_NEAR(percent(), fittedPercent, 0.001);
    EXPECT_TRUE(badge_->isVisible());
    EXPECT_EQ(label_->property("text").toString(), QStringLiteral("画面 31%"));
    const QVariantMap panel = surface_->sourcePanelRects().front().toMap();
    EXPECT_DOUBLE_EQ(panel.value(QStringLiteral("width")).toDouble(), surface_->width());
    EXPECT_NEAR(panel.value(QStringLiteral("contentWidth")).toDouble(), 336.375, 0.001);
    EXPECT_DOUBLE_EQ(panel.value(QStringLiteral("contentHeight")).toDouble(), 598.0);

    clickBadge();
    EXPECT_NEAR(surface_->viewScale(), 1920.0 / surface_->height(), 0.000001);
    EXPECT_TRUE(badge_->property("pixelExact").toBool());
    EXPECT_EQ(label_->property("text").toString(), QStringLiteral("100% 真实尺寸"));
    clickBadge();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_NEAR(surface_->viewScale(), 1920.0 / surface_->height(), 0.000001);
}

TEST_F(ViewportPixelScaleTests, ReferenceAndWipeUseTheDisplayedSourceAndFullComposite) {
    setSources({source(1920, 1080), source(1080, 1920), source(1080, 1920)});
    root_->setProperty("referenceSourceIndex", 1);
    root_->setProperty("effectiveViewMode", ComparisonSurface::ReferenceFocus);
    EXPECT_EQ(badge_->property("sourceSlot").toInt(), 1);
    EXPECT_NEAR(percent(), surface_->height() / 1920.0 * 100.0, 0.001);

    root_->setProperty("effectiveDifferenceEdge", ComparisonSurface::Edge1And2);
    root_->setProperty("effectiveViewMode", ComparisonSurface::Wipe);
    root_->setProperty("wipeMode", true);
    for (const double split : {0.0, 0.25, 0.8, 1.0}) {
        root_->setProperty("wipePosition", split);
        EXPECT_EQ(badge_->property("sourceSlot").toInt(), 1);
        EXPECT_NEAR(percent(), surface_->height() / 1920.0 * 100.0, 0.001);
        EXPECT_TRUE(badge_->isVisible());
    }
    clickBadge();
    EXPECT_NEAR(percent(), 100.0, 0.001);
}

TEST_F(ViewportPixelScaleTests, CropClearAndRestoreRefreshRotatedContentGeometry) {
    setSources({source(1080, 1920, 90)});
    const double fittedPercent = surface_->width() / 1920.0 * 100.0;
    EXPECT_NEAR(percent(), fittedPercent, 0.001);
    surface_->setRoiNormalized(0.25, 0.1, 0.75, 0.9);
    EXPECT_NEAR(percent(), surface_->width() / 1536.0 * 100.0, 0.001);
    clickBadge();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_TRUE(badge_->property("pixelExact").toBool());
    surface_->clearRoi();
    EXPECT_NEAR(percent(), fittedPercent, 0.001);
    surface_->restoreViewport(0.5, 0.5, 2.0, true, 0.25, 0.1, 0.75, 0.9);
    EXPECT_NEAR(percent(), surface_->width() / 1536.0 * 200.0, 0.001);
    EXPECT_TRUE(badge_->property("uniformScale").toBool());
    surface_->restoreViewport(0.5, 0.5, 1.0, false, 0.0, 0.0, 1.0, 1.0);
    EXPECT_NEAR(percent(), fittedPercent, 0.001);
    EXPECT_TRUE(badge_->property("uniformScale").toBool());
}

TEST_F(ViewportPixelScaleTests, NonSquarePixelsReportBothAxesWithoutClaimingTrueSize) {
    setSources({source(1920, 1080, 0, 4, 3)});
    EXPECT_NEAR(percent(), surface_->width() / 2560.0 * 100.0, 0.001);
    EXPECT_FALSE(badge_->property("uniformScale").toBool());
    EXPECT_EQ(label_->property("text").toString(), QStringLiteral("横 42% · 纵 31%"));
    clickBadge();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
    EXPECT_EQ(label_->property("text").toString(), QStringLiteral("横 133% · 纵 100%"));
    clickBadge();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());

    setSources({source(160, 90, 0, 4, 3)});
    surface_->resetViewport();
    ASSERT_GT(percent(), 100.0);
    clickBadge();
    EXPECT_LT(surface_->viewScale(), 1.0);
    EXPECT_NEAR(badge_->property("horizontalPercent").toDouble(), 400.0 / 3.0, 0.001);
    EXPECT_NEAR(badge_->property("verticalPercent").toDouble(), 100.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
    EXPECT_EQ(label_->property("text").toString(), QStringLiteral("横 133% · 纵 100%"));
    const double belowFitScale = surface_->viewScale();
    clickBadge();
    EXPECT_NEAR(surface_->viewScale(), belowFitScale, 0.000001);
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
}

TEST_F(ViewportPixelScaleTests, InvalidNearOneToOneAndClampedScalesStayHonest) {
    EXPECT_FALSE(badge_->isVisible());
    setSources({source(0, 1920)});
    EXPECT_FALSE(badge_->isVisible());
    setSources({source(800, 600)});
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
    EXPECT_NE(label_->property("text").toString(), QStringLiteral("100% 真实尺寸"));
    clickBadge();
    EXPECT_TRUE(badge_->property("pixelExact").toBool());
    setSources({source(100000, 100000)});
    surface_->resetViewport();
    EXPECT_FALSE(badge_->property("targetReachable").toBool());
    EXPECT_FALSE(badge_->isEnabled());
    EXPECT_EQ(badge_->property("text").toString(), QStringLiteral("100% 不可达"));
    EXPECT_TRUE(
        badge_->property("targetLimitText").toString().contains(QStringLiteral("超过 64 倍上限")));
    EXPECT_TRUE(label_->property("text").toString().contains(QStringLiteral("超过 64 倍上限")));
    EXPECT_TRUE(label_->property("text").toString().startsWith(QStringLiteral("画面 <1%")));
    // Even a programmatic signal cannot perform an unreachable 100% action.
    clickBadge();
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    surface_->zoomAt(0.5, 0.5, 1000.0);
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 64.0);
    EXPECT_NEAR(percent(), surface_->height() / 100000.0 * 6400.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());

    // A fractional ROI may be smaller than one source pixel. Retain it, but do not
    // offer a native target whose displayed footprint has less than one pixel per axis.
    root_->setProperty("width", 400);
    setSources({source(160, 90)});
    surface_->restoreViewport(0.5, 0.5, 1.0, true, 0.0, 0.0, 0.001, 0.001);
    ASSERT_TRUE(surface_->roiEnabled());
    EXPECT_FALSE(badge_->isEnabled());
    EXPECT_TRUE(label_->property("text").toString().contains(QStringLiteral("显示范围不足")));
    EXPECT_TRUE(
        QQmlProperty::read(badge_, QStringLiteral("Accessible.description"), qmlContext(badge_))
            .toString()
            .contains(QStringLiteral("显示范围不足")));
    EXPECT_LE(badge_->x() + badge_->width(), 400.0);
    clickBadge();
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    EXPECT_DOUBLE_EQ(surface_->roiRight(), 0.001);
    auto* const fit = root_->findChild<QQuickItem*>(QStringLiteral("viewportFitButton"));
    auto* const reset = root_->findChild<QQuickItem*>(QStringLiteral("viewportResetButton"));
    ASSERT_NE(fit, nullptr);
    ASSERT_NE(reset, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(fit, "clicked"));
    EXPECT_TRUE(surface_->roiEnabled());
    EXPECT_FALSE(badge_->isEnabled());
    ASSERT_TRUE(QMetaObject::invokeMethod(reset, "clicked"));
    EXPECT_FALSE(surface_->roiEnabled());
    EXPECT_TRUE(badge_->isEnabled());
}

TEST_F(ViewportPixelScaleTests, FixedActionFromAboveOneToOnePreservesObservation) {
    setSources({source(1920, 1080), source(1080, 1920), source(1080, 1920)});
    root_->setProperty("effectiveViewMode", ComparisonSurface::Wipe);
    root_->setProperty("wipeMode", true);
    root_->setProperty("effectiveDifferenceEdge", ComparisonSurface::Edge1And2);
    root_->setProperty("referenceSourceIndex", 1);
    root_->setProperty("currentFrame", 42);
    root_->setProperty("wipePosition", 0.3);
    surface_->restoreViewport(0.45, 0.55, 8.0, true, 0.1, 0.1, 0.9, 0.9);
    ASSERT_GT(percent(), 100.0);
    for (int repeat = 0; repeat < 2; ++repeat) {
        clickBadge();
        EXPECT_NEAR(percent(), 100.0, 0.001);
        EXPECT_NEAR(surface_->viewCenterX(), 0.45, 0.000001);
        EXPECT_NEAR(surface_->viewCenterY(), 0.55, 0.000001);
        EXPECT_TRUE(surface_->roiEnabled());
        EXPECT_DOUBLE_EQ(surface_->roiLeft(), 0.1);
        EXPECT_DOUBLE_EQ(surface_->roiTop(), 0.1);
        EXPECT_DOUBLE_EQ(surface_->roiRight(), 0.9);
        EXPECT_DOUBLE_EQ(surface_->roiBottom(), 0.9);
        EXPECT_EQ(surface_->differenceEdge(), ComparisonSurface::Edge1And2);
        EXPECT_EQ(surface_->referenceSlot(), 1);
        EXPECT_DOUBLE_EQ(surface_->wipePosition(), 0.3);
        EXPECT_EQ(root_->property("currentFrame").toInt(), 42);
    }
    // A larger visible area must stay in bounds near an edge; use zoomAt's existing clamp.
    surface_->restoreViewport(0.1, 0.9, 8.0, true, 0.1, 0.1, 0.9, 0.9);
    const double targetScale = surface_->viewScale() * 100.0 / percent();
    clickBadge();
    EXPECT_NEAR(surface_->viewCenterX(), 0.5 / targetScale, 0.000001);
    EXPECT_NEAR(surface_->viewCenterY(), 1.0 - 0.5 / targetScale, 0.000001);
}

TEST_F(ViewportPixelScaleTests, FitRetainsRoiAndResetClearsIt) {
    setSources({source(1920, 1080)});
    auto* const fit = root_->findChild<QQuickItem*>(QStringLiteral("viewportFitButton"));
    auto* const reset = root_->findChild<QQuickItem*>(QStringLiteral("viewportResetButton"));
    ASSERT_NE(fit, nullptr);
    ASSERT_NE(reset, nullptr);
    surface_->restoreViewport(0.45, 0.55, 4.0, true, 0.1, 0.2, 0.9, 0.8);
    ASSERT_TRUE(QMetaObject::invokeMethod(fit, "clicked"));
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    EXPECT_DOUBLE_EQ(surface_->viewCenterX(), 0.5);
    EXPECT_DOUBLE_EQ(surface_->viewCenterY(), 0.5);
    EXPECT_TRUE(surface_->roiEnabled());
    EXPECT_DOUBLE_EQ(surface_->roiLeft(), 0.1);
    EXPECT_DOUBLE_EQ(surface_->roiTop(), 0.2);
    EXPECT_DOUBLE_EQ(surface_->roiRight(), 0.9);
    EXPECT_DOUBLE_EQ(surface_->roiBottom(), 0.8);
    surface_->zoomAt(0.5, 0.5, 3.0);
    ASSERT_TRUE(QMetaObject::invokeMethod(reset, "clicked"));
    EXPECT_FALSE(surface_->roiEnabled());
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    EXPECT_DOUBLE_EQ(surface_->viewCenterX(), 0.5);
    EXPECT_DOUBLE_EQ(surface_->viewCenterY(), 0.5);
    // Reset must also fit and recenter a view that has no ROI to clear.
    surface_->restoreViewport(0.4, 0.6, 3.0, false, 0.0, 0.0, 1.0, 1.0);
    ASSERT_TRUE(QMetaObject::invokeMethod(reset, "clicked"));
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    EXPECT_DOUBLE_EQ(surface_->viewCenterX(), 0.5);
    EXPECT_DOUBLE_EQ(surface_->viewCenterY(), 0.5);
}

TEST_F(ViewportPixelScaleTests, PointerActionReachesNativeSizeBelowFitAndPreservesRoi) {
    setSources({source(160, 90)});
    root_->setProperty("currentFrame", 42);
    window_ = std::make_unique<QQuickWindow>();
    window_->resize(801, 601);
    auto* const viewport = qobject_cast<QQuickItem*>(root_.get());
    ASSERT_NE(viewport, nullptr);
    viewport->setSize(QSizeF{801.0, 601.0});
    viewport->setParentItem(window_->contentItem());
    window_->show();
    QCoreApplication::processEvents();
    ASSERT_EQ(window_->width(), 801);
    ASSERT_EQ(window_->height(), 601);
    ASSERT_DOUBLE_EQ(surface_->mapToScene(QPointF{}).x(), 1.0);
    ASSERT_DOUBLE_EQ(surface_->mapToScene(QPointF{}).y(), 1.0);
    const double dpr = window_->effectiveDevicePixelRatio();
    surface_->restoreViewport(0.4, 0.6, 2.0, false, 0.0, 0.0, 1.0, 1.0);
    ASSERT_GT(percent(), 100.0);
    for (int repeat = 0; repeat < 2; ++repeat) {
        clickBadgeThroughWindow();
        EXPECT_NEAR(surface_->viewScale(), 160.0 / (surface_->width() * dpr), 0.000001);
        EXPECT_LT(surface_->viewScale(), 1.0);
        EXPECT_DOUBLE_EQ(surface_->viewCenterX(), 0.5);
        EXPECT_DOUBLE_EQ(surface_->viewCenterY(), 0.5);
        EXPECT_NEAR(percent(), 100.0, 0.001);
        EXPECT_TRUE(badge_->property("pixelExact").toBool());
        EXPECT_EQ(label_->property("text").toString(), QStringLiteral("100% 真实尺寸"));
        EXPECT_FALSE(surface_->roiEnabled());
        EXPECT_EQ(root_->property("currentFrame").toInt(), 42);
    }
    surface_->restoreViewport(0.4, 0.6, 2.0, true, 0.125, 0.25, 0.875, 0.75);
    for (int repeat = 0; repeat < 2; ++repeat) {
        clickBadgeThroughWindow();
        EXPECT_NEAR(surface_->viewScale(), 120.0 / (surface_->width() * dpr), 0.000001);
        EXPECT_LT(surface_->viewScale(), 1.0);
        EXPECT_DOUBLE_EQ(surface_->viewCenterX(), 0.5);
        EXPECT_DOUBLE_EQ(surface_->viewCenterY(), 0.5);
        EXPECT_NEAR(percent(), 100.0, 0.001);
        EXPECT_TRUE(badge_->property("pixelExact").toBool());
        EXPECT_TRUE(surface_->roiEnabled());
        EXPECT_DOUBLE_EQ(surface_->roiLeft(), 0.125);
        EXPECT_DOUBLE_EQ(surface_->roiTop(), 0.25);
        EXPECT_DOUBLE_EQ(surface_->roiRight(), 0.875);
        EXPECT_DOUBLE_EQ(surface_->roiBottom(), 0.75);
        EXPECT_EQ(root_->property("currentFrame").toInt(), 42);
    }
    EXPECT_EQ(badge_->property("text").toString(), QStringLiteral("设为 100%"));
    const auto help = badge_->property("helpText").toString();
    EXPECT_TRUE(help.contains(QStringLiteral("非方形像素")));
    EXPECT_TRUE(help.contains(QStringLiteral("小于适应窗口")));
    EXPECT_TRUE(help.contains(QStringLiteral("完整 ROI 并居中")));
    EXPECT_TRUE(help.contains(QStringLiteral("放大上限")));
    EXPECT_TRUE(help.contains(QStringLiteral("64 倍")));
    EXPECT_TRUE(help.contains(QStringLiteral("实际倍率")));
    EXPECT_EQ(QQmlProperty::read(badge_, QStringLiteral("Accessible.name"), qmlContext(badge_))
                  .toString(),
              badge_->property("text").toString());
    EXPECT_TRUE(
        QQmlProperty::read(badge_, QStringLiteral("Accessible.description"), qmlContext(badge_))
            .toString()
            .contains(label_->property("text").toString()));
    for (const auto* name : {"viewportFitButton", "viewportResetButton"}) {
        auto* const button = root_->findChild<QQuickItem*>(QString::fromLatin1(name));
        ASSERT_NE(button, nullptr);
        EXPECT_FALSE(button->property("helpText").toString().isEmpty());
        EXPECT_EQ(
            QQmlProperty::read(button, QStringLiteral("Accessible.description"), qmlContext(button))
                .toString(),
            button->property("helpText").toString());
    }
}

TEST_F(ViewportPixelScaleTests, KeyboardActionUsesWindowDprAndLeavesTextInputAlone) {
    setSources({source(1920, 1080, 0, 4, 3)});
    window_ = std::make_unique<QQuickWindow>();
    window_->resize(800, 600);
    auto* const item = qobject_cast<QQuickItem*>(root_.get());
    ASSERT_NE(item, nullptr);
    item->setParentItem(window_->contentItem());
    window_->show();
    badge_->forceActiveFocus();
    QCoreApplication::processEvents();
    ASSERT_TRUE(badge_->hasActiveFocus());
    EXPECT_TRUE(badge_->activeFocusOnTab());
    EXPECT_TRUE(badge_->property("blocksGlobalMediaShortcuts").toBool());
    EXPECT_NEAR(percent(), surface_->width() / 2560.0 * window_->devicePixelRatio() * 100.0, 0.001);
    const auto space = [&] {
        QKeyEvent press{QEvent::KeyPress, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")};
        QCoreApplication::sendEvent(window_.get(), &press);
        QKeyEvent release{QEvent::KeyRelease, Qt::Key_Space, Qt::NoModifier, QStringLiteral(" ")};
        QCoreApplication::sendEvent(window_.get(), &release);
        QCoreApplication::processEvents();
    };
    space();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
    space();
    EXPECT_NEAR(percent(), 100.0, 0.001);

    QQmlComponent inputComponent{&engine_};
    inputComponent.setData(
        "import QtQuick; import QtQuick.Controls; Item { width: 800; height: 600; "
        "Dialog { objectName: \"zoomGuardDialog\"; modal: true; width: 240; height: 100; "
        "TextInput { objectName: \"zoomGuardInput\"; width: 100; height: 30 } } }",
        QUrl{});
    std::unique_ptr<QObject> input{inputComponent.create()};
    auto* const inputRoot = qobject_cast<QQuickItem*>(input.get());
    ASSERT_NE(inputRoot, nullptr) << inputComponent.errorString().toStdString();
    inputRoot->setParentItem(window_->contentItem());
    auto* const dialog = input->findChild<QObject*>(QStringLiteral("zoomGuardDialog"));
    auto* const inputItem = input->findChild<QQuickItem*>(QStringLiteral("zoomGuardInput"));
    ASSERT_NE(dialog, nullptr);
    ASSERT_NE(inputItem, nullptr);
    ASSERT_TRUE(QMetaObject::invokeMethod(dialog, "open"));
    QCoreApplication::processEvents();
    ASSERT_TRUE(dialog->property("visible").toBool());
    inputItem->forceActiveFocus();
    ASSERT_TRUE(inputItem->hasActiveFocus());
    surface_->zoomAt(0.5, 0.5, 2.0);
    const auto previousScale = surface_->viewScale();
    space();
    EXPECT_EQ(inputItem->property("text").toString(), QStringLiteral(" "));
    EXPECT_DOUBLE_EQ(surface_->viewScale(), previousScale);
}

TEST_F(ViewportPixelScaleTests, UnavailableControlsLeaveFocusAndTabChainUntilRecovery) {
    window_ = std::make_unique<QQuickWindow>();
    window_->resize(800, 600);
    auto* const viewport = qobject_cast<QQuickItem*>(root_.get());
    ASSERT_NE(viewport, nullptr);
    viewport->setParentItem(window_->contentItem());
    QQmlComponent emptyComponent{&engine_, QUrl{QStringLiteral("qrc:/qml/EmptyReviewView.qml")}};
    std::unique_ptr<QObject> empty{emptyComponent.create()};
    auto* const cover = qobject_cast<QQuickItem*>(empty.get());
    ASSERT_NE(cover, nullptr) << emptyComponent.errorString().toStdString();
    cover->setParentItem(window_->contentItem());
    cover->setSize(QSizeF{800, 600});
    cover->setZ(35);
    QQuickItem* const buttons[]{
        badge_,
        root_->findChild<QQuickItem*>(QStringLiteral("viewportFitButton")),
        root_->findChild<QQuickItem*>(QStringLiteral("viewportResetButton"))};
    for (auto* button : buttons) {
        ASSERT_NE(button, nullptr);
    }
    window_->show();
    window_->requestActivate();
    QCoreApplication::processEvents();
    const auto key = [&](int value) {
        const QString text = value == Qt::Key_Space ? QStringLiteral(" ") : QString{};
        QKeyEvent press{QEvent::KeyPress, value, Qt::NoModifier, text};
        QCoreApplication::sendEvent(window_.get(), &press);
        QKeyEvent release{QEvent::KeyRelease, value, Qt::NoModifier, text};
        QCoreApplication::sendEvent(window_.get(), &release);
        QCoreApplication::processEvents();
    };
    struct State {
        const char* name;
        int sourceKind; // 0: none; 1: valid metadata; 2: invalid metadata.
        int frame;
        bool overlay;
        bool busy;
        bool chrome;
        bool badgeEnabled;
        bool commandsEnabled;
    };
    const State states[]{
        {"empty cover", 0, -1, false, false, true, false, false},
        {"initial loading", 1, -1, true, true, true, false, false},
        {"retained error overlay", 1, 7, true, false, true, false, false},
        {"hidden chrome", 1, 7, false, false, false, false, false},
        {"invalid metadata", 2, 7, false, false, true, false, true},
        {"busy retained frame", 1, 7, false, true, true, true, true},
        {"loaded recovery", 1, 7, false, false, true, true, true},
    };
    const auto apply = [&](const State& state) {
        setSources(state.sourceKind == 0
                       ? QVariantList{}
                       : QVariantList{source(state.sourceKind == 2 ? 0 : 1920, 1080)});
        root_->setProperty("currentFrame", state.frame);
        root_->setProperty("overlayVisible", state.overlay);
        root_->setProperty("busy", state.busy);
        root_->setProperty("chromeVisible", state.chrome);
        cover->setVisible(state.sourceKind == 0);
        QCoreApplication::processEvents();
    };
    for (const auto& state : states) {
        SCOPED_TRACE(state.name);
        for (int index = 0; index < 3; ++index) {
            auto* const button = buttons[index];
            SCOPED_TRACE(button->objectName().toStdString());
            apply(states[6]);
            surface_->restoreViewport(0.4, 0.6, 2.0, true, 0.1, 0.2, 0.9, 0.8);
            button->forceActiveFocus();
            apply(state);
            const bool available = index == 0 ? state.badgeEnabled : state.commandsEnabled;
            EXPECT_EQ(button->isEnabled(), available);
            if (!available) {
                EXPECT_FALSE(button->hasActiveFocus());
                key(Qt::Key_Space);
                EXPECT_DOUBLE_EQ(surface_->viewScale(), 2.0);
                EXPECT_TRUE(surface_->roiEnabled());
            }
        }
        viewport->forceActiveFocus();
        bool visited[3]{false, false, false};
        // Nine steps cover all five real empty-screen actions and the three view controls.
        for (int step = 0; step < 9; ++step) {
            key(Qt::Key_Tab);
            for (int index = 0; index < 3; ++index) {
                visited[index] = visited[index] || window_->activeFocusItem() == buttons[index];
            }
        }
        for (int index = 0; index < 3; ++index) {
            SCOPED_TRACE(buttons[index]->objectName().toStdString());
            EXPECT_EQ(visited[index], index == 0 ? state.badgeEnabled : state.commandsEnabled);
        }
    }
}

// The unified double-click verb and the keyboard view commands share one set of viewport
// functions: toggleNativeFit flips between 100% and fit, zoomViewStep scales around the
// centre, and resetView clears the ROI on its way back to fit.
TEST_F(ViewportPixelScaleTests, ViewCommandFunctionsToggleNativeFitAndReset) {
    setSources({source(1920, 1080)});
    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "fitToWindow"));
    QCoreApplication::processEvents();
    const double fitPercent = percent();
    EXPECT_NE(fitPercent, 100.0);

    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "toggleNativeFit"));
    QCoreApplication::processEvents();
    EXPECT_NEAR(percent(), 100.0, 0.001);

    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "toggleNativeFit"));
    QCoreApplication::processEvents();
    EXPECT_NEAR(percent(), fitPercent, 0.001);

    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "zoomViewStep", Q_ARG(QVariant, 1.25)));
    QCoreApplication::processEvents();
    EXPECT_GT(percent(), fitPercent);
    EXPECT_LT(percent(), 100.0);

    // zoomToNativeSize is the Ctrl+0 path: it lands on 100% and stays there when already
    // native, instead of toggling back to fit.
    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "zoomToNativeSize"));
    QCoreApplication::processEvents();
    EXPECT_NEAR(percent(), 100.0, 0.001);
    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "zoomToNativeSize"));
    QCoreApplication::processEvents();
    EXPECT_NEAR(percent(), 100.0, 0.001);

    // resetView is more than fitToWindow: it drops a comparison ROI as well.
    surface_->setRoiNormalized(0.25, 0.25, 0.75, 0.75);
    QCoreApplication::processEvents();
    EXPECT_TRUE(surface_->roiEnabled());
    ASSERT_TRUE(QMetaObject::invokeMethod(root_.get(), "resetView"));
    QCoreApplication::processEvents();
    EXPECT_FALSE(surface_->roiEnabled());
    EXPECT_NEAR(percent(), fitPercent, 0.001);
}

} // namespace
} // namespace dvs::ui
