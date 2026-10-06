pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "VcsTheme.js" as Theme
// qmllint disable import
import Dvs.Ui 1.0

// qmllint enable import

Rectangle {
    id: control

    required property var preferences
    required property color borderColor
    required property color accentColor
    required property color primaryTextColor
    required property color mutedTextColor
    required property bool chromeVisible
    required property int effectiveViewMode
    required property real wipePosition
    required property int selectedDifferenceExactness
    required property var selectedDifferenceEdge
    required property bool differenceThresholdEnabled
    required property int differenceThresholdCode
    required property int differenceThresholdPolicy
    // Hold-to-peek: while true the difference pass is replaced by the raw first source
    // of the active pair (transient button-hold state, never persisted).
    required property bool differenceSuppressed
    required property int referenceSourceIndex
    required property int sourceCount
    required property bool wipeMode
    required property bool differenceMode
    required property bool analysisGridMode
    required property bool immersiveHudVisible
    required property string immersiveHudText
    required property bool showFramePending
    required property int currentFrame
    required property string differenceUnavailableDetail
    required property string combinedAlignmentStatus
    required property bool singleMode
    required property int differenceFirstSlot
    required property int effectiveDifferenceEdge
    required property var sourceNames
    required property var sourceParentLabels
    required property var sourceFullPaths
    required property var sourceMediaInfo
    required property bool frameErrorBannerVisible
    required property string errorDetail
    required property bool overlayVisible
    required property bool hasErrors
    required property bool busy
    required property string overlayTitle
    required property string overlayDetail

    property string alignmentModeName: ""
    property string inexactReason: ""

    property bool roiSelecting: false
    // Shift marquee zooms to the selection (product "放大"); Alt marquee sets comparison ROI.
    property bool roiModeIsZoom: true
    property int roiPanel: -1
    property real roiStartX: 0
    property real roiStartY: 0
    property real roiCurrentX: 0
    property real roiCurrentY: 0
    property real panLastX: 0
    property real panLastY: 0

    signal wipePositionRequested(real position)
    signal oscRevealRequested
    signal contextMenuRequested
    signal fullScreenToggleRequested
    property alias surface: dualVideoSurface
    property alias videoOutput: surfaceLayer
    readonly property bool roiEnabled: dualVideoSurface.roiEnabled
    // The var-typed `surface` alias defeats qmllint's type resolution from other files, so the
    // drop counter is re-exposed as a plain int computed here where ComparisonSurface resolves.
    readonly property real droppedFrames: dualVideoSurface.droppedFrames
    // How much of the bottom edge the floating transport covers. Main owns the transport geometry,
    // so it measures the overlap and hands the number in; the viewport only needs to know how far
    // to lift its own bottom controls. 0 when the transport is docked or hidden.
    property real bottomOverlayInset: 0
    // Status banners stack under the source label row (12 px inset + 42 px plate + 8 px gap), so a
    // centred banner never covers a panel's identity label.
    readonly property real stageBannerTop: control.chromeVisible ? 62 : 12
    // Distance from the viewport's bottom edge to the top of the fit/reset row, the highest of
    // the bottom stage controls; Main floats its notices above this line.
    readonly property real bottomControlsInset: control.height - viewCommandRow.y

    function clearRoi() {
        dualVideoSurface.clearRoi();
    }

    function panelPoint(x, y) {
        return dualVideoSurface.mapSurfacePoint(x, y);
    }

    function sourceFilename(slot) {
        return slot >= 0 && slot < control.sourceNames.length ? String(control.sourceNames[slot]) : "";
    }

    function sourceParentLabel(slot) {
        return slot >= 0 && slot < control.sourceParentLabels.length ? String(control.sourceParentLabels[slot]) : "";
    }

    function sourceFullPath(slot) {
        return slot >= 0 && slot < control.sourceFullPaths.length ? String(control.sourceFullPaths[slot]) : "";
    }

    // T6: every applicable inexactness dimension is named side by side. The single
    // exactness enum can only report the highest-priority reason, so a pair that is
    // temporally aligned, spatially resampled and display-space converted at once
    // would otherwise hide two of the three limitations. The pixel dimension is phrased as the
    // two questions a reviewer decides between: the viewing path (what the display shows after
    // normalization) and the numeric review path (the source's own plane code values).
    function comparisonDimensionsLabel(edge) {
        if (!edge || Number(edge.dimensionsAvailable) !== 1)
            return qsTr("比较语义不可用");
        const parts = [];
        parts.push(Number(edge.temporalExact) === 1 ? qsTr("时间 索引及时间戳一致") : qsTr("时间 映射或时间戳不同"));
        parts.push(Number(edge.spatialExact) === 1 ? qsTr("空间 原尺寸") : qsTr("空间 尺寸或几何不同"));
        parts.push(Number(edge.pixelExact) === 1 ? qsTr("像素 数值审查路径（原码值）") : qsTr("像素 观看路径（已做显示转换，非原码值）"));
        return parts.join(" · ");
    }

    function comparisonFrameTimesLabel(edge) {
        if (!edge)
            return "";
        const firstFrame = Number(edge.firstSourceFrame);
        const secondFrame = Number(edge.secondSourceFrame);
        const firstId = Number(edge.firstSourceId) + 1;
        const secondId = Number(edge.secondSourceId) + 1;
        const firstText = firstFrame >= 0 ? qsTr("源 %1 第 %2 帧 / %3 ms").arg(firstId).arg(firstFrame + 1).arg((Number(edge.firstPresentationTimeUs) / 1000).toFixed(2)) : qsTr("源 %1 缺失").arg(firstId);
        const secondText = secondFrame >= 0 ? qsTr("源 %2 第 %3 帧 / %4 ms").arg(secondId).arg(secondFrame + 1).arg((Number(edge.secondPresentationTimeUs) / 1000).toFixed(2)) : qsTr("源 %1 缺失").arg(secondId);
        let text = firstText + "；" + secondText;
        if (control.inexactReason.length > 0)
            text += "\n" + qsTr("⚠️ 无法精确对应原因：%1").arg(control.inexactReason);
        return text;
    }

    function surfaceLabelGeometry(index) {
        const label = surfaceLabelRepeater.itemAt(index);
        if (!label)
            return {};
        const panel = dualVideoSurface.sourcePanelRects[index];
        return {
            "sourceSlot": Number(panel.slot),
            "x": label.x,
            "width": label.width,
            "visible": label.visible
        };
    }

    objectName: "mediaViewportFocusTarget"
    focus: true
    color: Theme.stageWell
    border.color: control.borderColor
    border.width: control.chromeVisible ? 1 : 0
    radius: control.chromeVisible ? 7 : 0
    clip: true
    TapHandler {
        onTapped: control.forceActiveFocus()
    }

    Item {
        id: surfaceLayer

        anchors {
            fill: parent
            margins: control.chromeVisible ? 1 : 0
        }

        // The type is runtime-registered; startup smoke coverage verifies the registration.
        // qmllint disable import unqualified unresolved-type
        ComparisonSurface {
            id: dualVideoSurface

            objectName: "dualVideoSurface"
            Accessible.name: qsTr("CompareStation 同步对比画面")
            viewMode: control.effectiveViewMode
            differenceMetric: control.preferences ? control.preferences.differenceMetric : ComparisonSurface.RgbAbsolute
            differenceGain: control.preferences ? control.preferences.differenceGain : ComparisonSurface.Gain1x
            differenceEdge: control.effectiveDifferenceEdge
            differenceFilter: control.preferences ? control.preferences.differenceFilter : ComparisonSurface.Bilinear
            wipePosition: control.wipePosition
            exactPlaneAvailable: control.selectedDifferenceExactness === 0
            thresholdEnabled: control.differenceThresholdEnabled
            threshold: Number(control.differenceThresholdCode) / 255
            thresholdPolicy: control.differenceThresholdPolicy
            differenceSuppressed: control.differenceSuppressed
            referenceSlot: control.referenceSourceIndex >= 0 ? control.referenceSourceIndex : 0
            sourceDisplayInfo: control.sourceMediaInfo
            anchors.fill: parent
        }
        // qmllint enable import unqualified unresolved-type
    }

    Repeater {
        objectName: "panelDividerRepeater"
        model: control.sourceCount > 1 && !control.wipeMode && (!control.differenceMode || control.analysisGridMode) ? dualVideoSurface.sourcePanelRects : []

        Rectangle {
            required property var modelData

            x: Math.round(Number(modelData.x))
            y: Math.round(Number(modelData.y))
            width: Math.round(Number(modelData.width))
            height: Math.round(Number(modelData.height))
            color: "transparent"
            border.width: 1
            border.color: Theme.stageDivider
            z: 8
        }
    }

    WipeHandle {
        visible: control.wipeMode
        z: 50
        surfaceItem: dualVideoSurface
        position: control.wipePosition
        onPositionRequested: position => control.wipePositionRequested(position)
    }

    Repeater {
        model: !control.chromeVisible && control.sourceCount > 1 ? dualVideoSurface.sourcePanelRects : []

        // Immersive letters keep each source's identity colour, like the full labels do.
        Rectangle {
            id: immersiveLetter

            required property var modelData
            readonly property int slot: Number(modelData.slot)

            x: Math.max(8, Math.min(parent.width - width - 8, Number(modelData.x) + 8))
            y: 8
            width: 30
            height: 26
            radius: Theme.radiusSmall
            color: Theme.stageLabel
            border.color: Theme.sourceBorder(immersiveLetter.slot)
            z: 10

            Text {
                anchors.centerIn: parent
                text: String.fromCharCode(65 + immersiveLetter.slot)
                color: Theme.sourceColor(immersiveLetter.slot)
                font.bold: true
                font.pixelSize: 12
            }
        }
    }

    Rectangle {
        objectName: "immersiveReviewHud"
        visible: !control.chromeVisible && control.immersiveHudVisible && control.immersiveHudText.length > 0
        opacity: visible ? 1.0 : 0.0
        z: 45
        width: immersiveHudLabel.implicitWidth + 24
        height: 34
        radius: 6
        color: Theme.oscGlass
        border.color: Theme.oscBorder
        anchors {
            bottom: parent.bottom
            bottomMargin: 18
            horizontalCenter: parent.horizontalCenter
        }

        Behavior on opacity {
            NumberAnimation {
                duration: 120
            }
        }

        Text {
            id: immersiveHudLabel

            text: control.immersiveHudText
            color: control.primaryTextColor
            font.pixelSize: 13
            anchors.centerIn: parent
        }
    }

    Rectangle {
        objectName: "framePendingIndicator"
        visible: control.showFramePending && control.currentFrame >= 0
        z: 40
        radius: 12
        color: Theme.oscGlass
        border.color: Theme.oscBorder
        width: pendingRow.implicitWidth + 22
        height: 30
        anchors {
            top: parent.top
            right: parent.right
            margins: 12
        }

        Row {
            id: pendingRow

            spacing: 7
            anchors.centerIn: parent

            BusyIndicator {
                width: 16
                height: 16
                running: parent.parent.visible
            }
            Text {
                text: qsTr("正在获取最新帧…")
                color: control.mutedTextColor
                font.pixelSize: 11
            }
        }
    }

    MouseArea {
        id: viewportNavigation

        anchors.fill: parent
        anchors.margins: 1
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true

        onWheel: wheel => {
            control.oscRevealRequested();
            const point = control.panelPoint(wheel.x, wheel.y);
            if (!point.insideContent) {
                wheel.accepted = false;
                return;
            }
            dualVideoSurface.zoomAt(point.x, point.y, wheel.angleDelta.y > 0 ? 1.25 : 0.8);
            wheel.accepted = true;
        }
        onPressed: mouse => {
            control.oscRevealRequested();
            if (mouse.button === Qt.RightButton) {
                control.contextMenuRequested();
                return;
            }
            const point = control.panelPoint(mouse.x, mouse.y);
            if (!point.insideContent) {
                control.roiPanel = -1;
                control.forceActiveFocus();
                return;
            }
            control.roiPanel = point.panel;
            control.panLastX = point.x;
            control.panLastY = point.y;
            if ((mouse.modifiers & Qt.ShiftModifier) !== 0) {
                // Unified with the image workspace: Shift+drag zooms to the marquee.
                control.roiSelecting = true;
                control.roiModeIsZoom = true;
                control.roiStartX = mouse.x;
                control.roiStartY = mouse.y;
                control.roiCurrentX = mouse.x;
                control.roiCurrentY = mouse.y;
            } else if ((mouse.modifiers & Qt.AltModifier) !== 0) {
                control.roiSelecting = true;
                control.roiModeIsZoom = false;
                control.roiStartX = mouse.x;
                control.roiStartY = mouse.y;
                control.roiCurrentX = mouse.x;
                control.roiCurrentY = mouse.y;
            }
            control.forceActiveFocus();
        }
        onPositionChanged: mouse => {
            control.oscRevealRequested();
            const point = control.panelPoint(mouse.x, mouse.y);
            if (control.roiSelecting) {
                control.roiCurrentX = mouse.x;
                control.roiCurrentY = mouse.y;
            } else if (pressed && point.insideContent && point.panel === control.roiPanel) {
                dualVideoSurface.panBy(point.x - control.panLastX, point.y - control.panLastY);
                control.panLastX = point.x;
                control.panLastY = point.y;
            }
        }
        onReleased: mouse => {
            if (control.roiSelecting) {
                const start = control.panelPoint(control.roiStartX, control.roiStartY);
                const end = control.panelPoint(mouse.x, mouse.y);
                if (start.insideContent && end.insideContent && start.panel === end.panel && start.sourceX !== undefined && start.sourceY !== undefined && end.sourceX !== undefined && end.sourceY !== undefined) {
                    if (control.roiModeIsZoom)
                        dualVideoSurface.zoomToNormalizedRect(start.sourceX, start.sourceY, end.sourceX, end.sourceY);
                    else
                        dualVideoSurface.setRoiNormalized(start.sourceX, start.sourceY, end.sourceX, end.sourceY);
                }
            }
            control.roiSelecting = false;
            control.roiModeIsZoom = true;
            control.roiPanel = -1;
        }
        onDoubleClicked: {
            if (control.roiEnabled)
                control.clearRoi();
            else
                control.fullScreenToggleRequested();
        }
    }

    Rectangle {
        visible: control.roiSelecting
        x: Math.min(control.roiStartX, control.roiCurrentX)
        y: Math.min(control.roiStartY, control.roiCurrentY)
        width: Math.abs(control.roiCurrentX - control.roiStartX)
        height: Math.abs(control.roiCurrentY - control.roiStartY)
        color: Theme.selectionFill
        border.color: control.accentColor
        border.width: 1
    }

    Rectangle {
        id: analysisChrome

        objectName: "analysisControlsChrome"
        visible: control.chromeVisible && (control.differenceMode || dualVideoSurface.roiEnabled || control.alignmentModeName.length > 0 || control.differenceThresholdEnabled)
        z: 30
        width: Math.min(Math.max(0, parent.width - 24), analysisStatus.implicitWidth + 18)
        height: 28
        radius: 5
        color: Theme.oscGlass
        border.color: Theme.oscBorder
        anchors {
            right: parent.right
            rightMargin: 12
            bottom: parent.bottom
            bottomMargin: 12
        }

        HoverHandler {
            id: analysisChromeHover
        }
        ToolTip.visible: analysisChromeHover.hovered && (control.differenceMode || control.alignmentModeName.length > 0)
        ToolTip.text: control.comparisonFrameTimesLabel(control.selectedDifferenceEdge)

        Label {
            id: analysisStatus

            text: {
                const parts = [];
                if (control.alignmentModeName.length > 0)
                    parts.push(qsTr("对齐：%1").arg(control.alignmentModeName));
                if (control.differenceMode)
                    parts.push(control.comparisonDimensionsLabel(control.selectedDifferenceEdge));
                if (control.differenceThresholdEnabled)
                    parts.push(qsTr("阈值已启用 (%1)").arg(control.differenceThresholdCode));
                if (dualVideoSurface.roiEnabled)
                    parts.push(qsTr("ROI 已启用"));
                return parts.join(" · ");
            }
            // Exactness describes the pixel comparison (format, colour space, geometry), so only
            // difference mode is tinted; plain side-by-side viewing stays neutral.
            color: !control.differenceMode ? Theme.secondaryText : (control.inexactReason.length === 0 && control.selectedDifferenceExactness === 0 ? Theme.success : Theme.warning)
            font.pixelSize: 11
            width: Math.max(0, parent.width - 18)
            elide: Text.ElideRight
            anchors.centerIn: parent
        }
    }

    // Effective pixel-scale badge (parity with the image workspace's percent readout). The
    // surface zoom is relative to the fitted layout, so 1:1 needs the panel rect, the source's
    // pixel extent, and the window DPR. "100% 真实尺寸" marks exact physical 1:1; clicking
    // either jumps to 1:1 (zoom in) or back to the fitted viewport.
    Rectangle {
        id: pixelScaleBadge

        objectName: "viewportPixelScaleBadge"
        visible: control.chromeVisible && pixelScaleBadge.sourceExtent > 0 && pixelScaleBadge.panelWidth > 0
        z: 30
        radius: 5
        // Same glass as the other stage plates; exact 1:1 is told by the green edge and label.
        color: pixelScaleMouse.containsMouse ? Theme.oscGlassHover : Theme.oscGlass
        border.color: pixelScaleBadge.pixelExact ? Theme.success : Theme.oscBorder
        height: 28
        width: pixelScaleLabel.implicitWidth + 18
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: parent.bottom
            bottomMargin: 12 + control.bottomOverlayInset
        }

        readonly property var firstPanel: dualVideoSurface.sourcePanelRects.length > 0 ? dualVideoSurface.sourcePanelRects[0] : null
        readonly property real panelWidth: firstPanel ? Number(firstPanel.width) : 0
        // Rotation swaps the effective extent along the panel width; sample aspect ratio is
        // not compensated (rare, and the badge is an orientation hint, not a measurement).
        readonly property real sourceExtent: {
            if (control.sourceMediaInfo.length === 0)
                return 0;
            const info = control.sourceMediaInfo[0];
            const rotation = Number(info.rotationDegrees);
            return (rotation === 90 || rotation === 270) ? Number(info.height) : Number(info.width);
        }
        readonly property real devicePixelRatio: Window.window ? Window.window.devicePixelRatio : 1
        readonly property real effectivePercent: sourceExtent > 0 && panelWidth > 0 ? (panelWidth / sourceExtent) * dualVideoSurface.viewScale * devicePixelRatio * 100 : 0
        readonly property bool pixelExact: Math.abs(effectivePercent - 100) < 2.5

        Label {
            id: pixelScaleLabel

            text: pixelScaleBadge.pixelExact ? qsTr("100% 真实尺寸") : qsTr("画面 %1%").arg(Math.round(pixelScaleBadge.effectivePercent))
            color: pixelScaleBadge.pixelExact ? Theme.success : control.mutedTextColor
            font.pixelSize: 11
            anchors.centerIn: parent
        }

        MouseArea {
            id: pixelScaleMouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                if (pixelScaleBadge.pixelExact || pixelScaleBadge.effectivePercent > 100) {
                    dualVideoSurface.resetViewport();
                    return;
                }
                // Zoom toward 1:1 physical pixels; zoomAt clamps to the surface scale range.
                const factor = 100 / Math.max(1, pixelScaleBadge.effectivePercent);
                dualVideoSurface.zoomAt(0.5, 0.5, factor);
            }
        }
    }

    // Fit / reset commands, same product verbs as the image workspace toolbar.
    Row {
        id: viewCommandRow

        objectName: "viewportViewCommands"
        visible: control.chromeVisible
        z: 30
        spacing: 6
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: pixelScaleBadge.top
            bottomMargin: 8
        }

        Rectangle {
            objectName: "viewportFitButton"
            width: fitLabel.implicitWidth + 16
            height: 24
            radius: 4
            color: fitMouse.containsMouse ? Theme.oscGlassHover : Theme.oscGlass
            border.color: fitMouse.containsMouse ? Theme.borderHover : Theme.oscBorder

            Label {
                id: fitLabel

                text: qsTr("适应窗口")
                color: fitMouse.containsMouse ? control.primaryTextColor : control.mutedTextColor
                font.pixelSize: 11
                anchors.centerIn: parent
            }

            MouseArea {
                id: fitMouse

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: dualVideoSurface.resetViewport()
            }
        }

        Rectangle {
            objectName: "viewportResetButton"
            width: resetLabel.implicitWidth + 16
            height: 24
            radius: 4
            color: resetMouse.containsMouse ? Theme.oscGlassHover : Theme.oscGlass
            border.color: resetMouse.containsMouse ? Theme.borderHover : Theme.oscBorder

            Label {
                id: resetLabel

                text: qsTr("重置视图")
                color: resetMouse.containsMouse ? control.primaryTextColor : control.mutedTextColor
                font.pixelSize: 11
                anchors.centerIn: parent
            }

            MouseArea {
                id: resetMouse

                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                // Full observation reset: fit the view AND drop the ROI, so 差异/指标 return
                // to the whole-frame baseline (适应窗口 keeps the ROI).
                onClicked: {
                    dualVideoSurface.clearRoi();
                    dualVideoSurface.resetViewport();
                }
            }
        }
    }

    Rectangle {
        id: differenceUnavailableOverlay

        objectName: "differenceUnavailableOverlay"
        visible: control.differenceUnavailableDetail.length > 0
        width: Math.min(parent.width - 48, 460)
        height: unavailableColumn.implicitHeight + 32
        radius: 8
        // A missing frame or an inexact pairing is a limit of the material, not a failure.
        color: Theme.warningPanel
        border.color: Theme.warningBorder
        border.width: 1
        z: 40
        anchors.centerIn: parent

        Column {
            id: unavailableColumn

            width: parent.width - 32
            spacing: 6
            anchors.centerIn: parent

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: qsTr("差异不可用")
                color: control.primaryTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
                text: control.differenceUnavailableDetail
                color: Theme.warningText
                font.pixelSize: 13
            }
        }
    }

    Rectangle {
        id: alignmentStatus

        visible: control.chromeVisible && control.combinedAlignmentStatus.length > 0
        // The mapping readout is on for every aligned session, so it uses the neutral stage plate;
        // red stays reserved for the real failure banners.
        radius: Theme.radiusMedium
        color: Theme.stageLabel
        border.color: Theme.stageLabelBorder
        height: mappingStatusText.implicitHeight + 14
        width: Math.min(parent.width - 24, mappingStatusText.implicitWidth + 24)
        z: 20
        anchors {
            top: parent.top
            topMargin: control.stageBannerTop
            horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: mappingStatusText

            width: Math.min(implicitWidth, alignmentStatus.width - 24)
            text: control.combinedAlignmentStatus
            color: Theme.secondaryText
            font.pixelSize: 12
            font.weight: Font.Medium
            elide: Text.ElideRight
            anchors.centerIn: parent
        }
    }

    Rectangle {
        id: inexactDifferenceBanner

        objectName: "inexactDifferenceBanner"
        visible: control.chromeVisible && control.differenceMode && control.inexactReason.length > 0
        radius: 5
        color: Theme.warningPanel
        border.color: Theme.warningBorder
        border.width: 1
        height: inexactBannerText.implicitHeight + 10
        width: Math.min(parent.width - 24, inexactBannerText.implicitWidth + 20)
        z: 20
        anchors {
            top: alignmentStatus.visible ? alignmentStatus.bottom : parent.top
            topMargin: alignmentStatus.visible ? 8 : control.stageBannerTop
            horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: inexactBannerText

            text: qsTr("⚠️ 对比非精确对应：%1").arg(control.inexactReason)
            color: Theme.warningText
            font.pixelSize: 11
            font.weight: Font.DemiBold
            anchors.centerIn: parent
        }
    }

    Item {
        id: surfaceLabels

        visible: control.chromeVisible
        z: 10
        anchors {
            fill: parent
            margins: control.chromeVisible ? 1 : 0
        }

        Repeater {
            id: surfaceLabelRepeater

            objectName: "surfaceLabelRepeater"
            model: dualVideoSurface.sourcePanelRects

            delegate: Rectangle {
                id: surfaceLabel

                required property int index
                required property var modelData
                objectName: "surfaceLabel"
                property int sourceSlot: Number(modelData.slot)

                readonly property real panelWidth: Number(modelData.width)
                readonly property bool wipeBadge: control.wipeMode
                readonly property bool showFilename: !control.singleMode && !wipeBadge && panelWidth >= 160
                readonly property bool compactBadge: control.singleMode || wipeBadge || panelWidth < 160

                visible: wipeBadge || control.singleMode || panelWidth >= 80
                x: wipeBadge ? (Number(modelData.slot) === control.differenceFirstSlot ? 12 : parent.width - width - 12) : Number(modelData.x) + 12
                y: Number(modelData.y) + 12
                // The plate hugs the file name (46 px letter column + 10 px end inset) and only
                // elides once the name outgrows the panel or the 280 px cap.
                width: compactBadge ? 44 : Math.min(280, Math.max(80, panelWidth - 24), Math.max(80, surfaceLabelName.implicitWidth + 56))
                height: compactBadge ? 36 : 42
                radius: 8
                color: Theme.oscGlass
                border.width: 1
                border.color: Theme.sourceBorder(surfaceLabel.sourceSlot)

                Rectangle {
                    width: 26
                    height: 26
                    radius: 5
                    color: Theme.sourceColor(surfaceLabel.sourceSlot)
                    anchors {
                        left: parent.left
                        leftMargin: surfaceLabel.compactBadge ? 9 : 8
                        verticalCenter: parent.verticalCenter
                    }

                    Text {
                        anchors.centerIn: parent
                        text: String.fromCharCode(65 + Number(surfaceLabel.modelData.slot))
                        color: Theme.sourceInk
                        font.bold: true
                        font.pixelSize: 13
                    }
                }

                Text {
                    id: surfaceLabelName

                    visible: surfaceLabel.showFilename
                    text: {
                        const parent = control.sourceParentLabel(surfaceLabel.sourceSlot);
                        const name = control.sourceFilename(surfaceLabel.sourceSlot);
                        return parent.length > 0 ? "%1 (%2)".arg(name).arg(parent) : name;
                    }
                    color: control.primaryTextColor
                    font.pixelSize: 12
                    elide: Text.ElideMiddle
                    anchors {
                        left: parent.left
                        leftMargin: 46
                        right: parent.right
                        rightMargin: 10
                        verticalCenter: parent.verticalCenter
                    }

                    HoverHandler {
                        id: surfaceLabelNameHover
                    }

                    VcsToolTip {
                        visible: surfaceLabelNameHover.hovered && control.sourceFullPath(surfaceLabel.sourceSlot).length > 0
                        text: control.sourceFullPath(surfaceLabel.sourceSlot)
                    }
                }
            }
        }
    }

    Rectangle {
        id: frameErrorBanner

        objectName: "frameErrorBanner"
        width: Math.min(parent.width - 48, 720)
        height: frameErrorBannerColumn.implicitHeight + 20
        radius: 6
        visible: control.frameErrorBannerVisible
        color: Theme.errorPanel
        border.color: Theme.errorBorder
        z: 40
        Accessible.name: qsTr("帧未变化。%1").arg(control.errorDetail)
        anchors {
            top: parent.top
            topMargin: alignmentStatus.visible ? control.stageBannerTop + alignmentStatus.height + 8 : control.stageBannerTop
            horizontalCenter: parent.horizontalCenter
        }

        Column {
            id: frameErrorBannerColumn

            width: parent.width - 28
            spacing: 3
            anchors.centerIn: parent

            Text {
                width: parent.width
                text: qsTr("帧未变化")
                color: Theme.errorText
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "frameErrorBannerDetail"
                width: parent.width
                text: control.errorDetail
                color: control.primaryTextColor
                font.pixelSize: 11
                wrapMode: Text.Wrap
            }
        }
    }

    Rectangle {
        id: statusOverlay

        objectName: "statusOverlay"
        width: Math.min(parent.width - 48, 560)
        height: overlayColumn.implicitHeight + 32
        radius: 7
        visible: control.overlayVisible
        color: control.hasErrors && !control.busy ? Theme.errorPanel : Theme.oscGlass
        border.color: control.hasErrors && !control.busy ? Theme.errorBorder : Theme.oscBorder
        anchors.centerIn: parent

        Column {
            id: overlayColumn

            width: parent.width - 36
            spacing: 8
            anchors.centerIn: parent

            BusyIndicator {
                width: 34
                height: 34
                running: control.busy && control.currentFrame < 0
                visible: running
                anchors.horizontalCenter: parent.horizontalCenter
                Accessible.name: qsTr("加载中")
            }

            Text {
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: control.overlayTitle
                color: control.hasErrors && !control.busy ? Theme.errorText : control.primaryTextColor
                font.pixelSize: 17
                font.weight: Font.DemiBold
                wrapMode: Text.Wrap
            }

            Text {
                objectName: "statusDetail"
                width: parent.width
                horizontalAlignment: Text.AlignHCenter
                text: control.overlayDetail
                color: control.mutedTextColor
                font.pixelSize: 12
                lineHeight: 1.25
                wrapMode: Text.Wrap
            }
        }
    }
}
