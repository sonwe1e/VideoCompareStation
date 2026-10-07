#include "dvs/ui/ComparisonSurface.h"

#include <QCoreApplication>
#include <QMetaObject>
#include <QObject>
#include <QQmlComponent>
#include <QQmlEngine>
#include <QQuickItem>
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
        // No window, renderer services, media, filesystem worker or playback coordinator is
        // needed: exercise the real viewport bindings and surface geometry in isolation.
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
        mouse_ = root_->findChild<QObject*>(QStringLiteral("viewportPixelScaleMouse"));
        ASSERT_NE(surface_, nullptr);
        ASSERT_NE(badge_, nullptr);
        ASSERT_NE(label_, nullptr);
        ASSERT_NE(mouse_, nullptr);
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
        // MouseArea.clicked has a private event type, but this handler does not consume it.
        // Invoke its real signal with a null event rather than duplicate the production action.
        void* event = nullptr;
        ASSERT_TRUE(QMetaObject::invokeMethod(
            mouse_, "clicked", QGenericArgument("QQuickMouseEvent*", &event)));
        QCoreApplication::processEvents();
    }

    QQmlEngine engine_;
    std::unique_ptr<QObject> root_;
    ComparisonSurface* surface_ = nullptr;
    QQuickItem* badge_ = nullptr;
    QObject* label_ = nullptr;
    QObject* mouse_ = nullptr;
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
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
    EXPECT_NEAR(percent(), fittedPercent, 0.001);
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
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 1.0);
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
    clickBadge();
    EXPECT_DOUBLE_EQ(surface_->viewScale(), 64.0);
    EXPECT_NEAR(percent(), surface_->height() / 100000.0 * 6400.0, 0.001);
    EXPECT_FALSE(badge_->property("pixelExact").toBool());
}

} // namespace
} // namespace dvs::ui
