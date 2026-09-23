pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
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
    required property color errorColor
    required property bool chromeVisible
    required property int effectiveViewMode
    required property real wipePosition
    required property int selectedDifferenceExactness
    required property var selectedDifferenceEdge
    required property bool differenceThresholdEnabled
    required property int differenceThresholdCode
    required property int differenceThresholdPolicy
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

    property bool roiSelecting: false
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
    readonly property int droppedFrames: dualVideoSurface.droppedFrames

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

    function comparisonExactnessLabel(exactness) {
        if (exactness === 0)
            return qsTr("逐像素精确");
        if (exactness === 1)
            return qsTr("已做显示空间转换");
        if (exactness === 2)
            return qsTr("已空间重采样");
        if (exactness === 3)
            return qsTr("帧映射或时间戳不同");
        return qsTr("不可用");
    }

    // T6: every applicable inexactness dimension is named side by side. The single
    // exactness enum can only report the highest-priority reason, so a pair that is
    // temporally aligned, spatially resampled and display-space converted at once
    // would otherwise hide two of the three limitations.
    function comparisonDimensionsLabel(edge) {
        if (!edge || Number(edge.dimensionsAvailable) !== 1)
            return qsTr("比较语义不可用");
        const parts = [];
        parts.push(Number(edge.temporalExact) === 1 ? qsTr("时间 索引及时间戳一致") : qsTr("时间 映射或时间戳不同"));
        parts.push(Number(edge.spatialExact) === 1 ? qsTr("空间 原尺寸") : qsTr("空间 已重采样"));
        parts.push(Number(edge.pixelExact) === 1 ? qsTr("像素 原码值") : qsTr("像素 显示空间转换"));
        return parts.join(" · ");
    }

    function comparisonFrameTimesLabel(edge) {
        if (!edge || Number(edge.dimensionsAvailable) !== 1)
            return "";
        const firstFrame = Number(edge.firstSourceFrame);
        const secondFrame = Number(edge.secondSourceFrame);
        if (firstFrame < 0 || secondFrame < 0)
            return "";
        return qsTr("源 %1 第 %2 帧 / %3 ms；源 %4 第 %5 帧 / %6 ms").arg(Number(edge.firstSourceId) + 1).arg(firstFrame + 1).arg((Number(edge.firstPresentationTimeUs) / 1000).toFixed(2)).arg(Number(edge.secondSourceId) + 1).arg(secondFrame + 1).arg((Number(edge.secondPresentationTimeUs) / 1000).toFixed(2));
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
    color: "#06080d"
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
            border.color: "#d8e2f2"
            opacity: 0.75
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

        Rectangle {
            required property var modelData

            x: Math.max(8, Math.min(parent.width - width - 8, Number(modelData.x) + 8))
            y: 8
            width: 30
            height: 26
            radius: 4
            color: "#b3243f68"
            border.color: "#804b8df8"
            z: 10

            Text {
                anchors.centerIn: parent
                text: String.fromCharCode(65 + Number(parent.modelData.slot))
                color: "white"
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
        color: "#dc111923"
        border.color: "#803d4d64"
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
        color: "#dc1d2635"
        border.color: control.borderColor
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
                control.roiSelecting = true;
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
                if (start.insideContent && end.insideContent && start.panel === end.panel && start.sourceX !== undefined && start.sourceY !== undefined && end.sourceX !== undefined && end.sourceY !== undefined)
                    dualVideoSurface.setRoiNormalized(start.sourceX, start.sourceY, end.sourceX, end.sourceY);
            }
            control.roiSelecting = false;
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
        color: "#224b8df8"
        border.color: control.accentColor
        border.width: 1
    }

    Rectangle {
        id: analysisChrome

        objectName: "analysisControlsChrome"
        visible: control.chromeVisible && (control.differenceMode || dualVideoSurface.roiEnabled)
        z: 30
        width: Math.min(Math.max(0, parent.width - 24), analysisStatus.implicitWidth + 18)
        height: 28
        radius: 5
        color: "#dc171e2a"
        border.color: control.borderColor
        anchors {
            right: parent.right
            rightMargin: 12
            bottom: parent.bottom
            bottomMargin: 12
        }

        HoverHandler {
            id: analysisChromeHover
        }
        ToolTip.visible: analysisChromeHover.hovered && control.differenceMode
        ToolTip.text: control.comparisonFrameTimesLabel(control.selectedDifferenceEdge)

        Label {
            id: analysisStatus

            text: {
                const parts = [];
                if (control.differenceMode)
                    parts.push(control.comparisonDimensionsLabel(control.selectedDifferenceEdge));
                if (dualVideoSurface.roiEnabled)
                    parts.push(qsTr("ROI 已启用"));
                return parts.join(" · ");
            }
            color: control.selectedDifferenceExactness === 0 ? "#86efac" : "#facc15"
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
        color: pixelScaleBadge.pixelExact ? "#dc122b1f" : "#dc171e2a"
        border.color: pixelScaleBadge.pixelExact ? "#86efac" : control.borderColor
        height: 28
        width: pixelScaleLabel.implicitWidth + 18
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: parent.bottom
            bottomMargin: 12
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
            color: pixelScaleBadge.pixelExact ? "#86efac" : control.mutedTextColor
            font.pixelSize: 11
            anchors.centerIn: parent
        }

        MouseArea {
            anchors.fill: parent
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

    Rectangle {
        id: differenceUnavailableOverlay

        objectName: "differenceUnavailableOverlay"
        visible: control.differenceUnavailableDetail.length > 0
        width: Math.min(parent.width - 48, 460)
        height: unavailableColumn.implicitHeight + 32
        radius: 8
        color: "#e6121822"
        border.color: control.errorColor
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
                color: "#fca5a5"
                font.pixelSize: 13
            }
        }
    }

    Rectangle {
        id: alignmentStatus

        visible: control.chromeVisible && control.combinedAlignmentStatus.length > 0
        radius: 5
        color: "#d9232c3d"
        border.color: control.errorColor
        height: mappingStatusText.implicitHeight + 14
        width: Math.min(parent.width - 24, mappingStatusText.implicitWidth + 24)
        z: 20
        anchors {
            top: parent.top
            topMargin: 12
            horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: mappingStatusText

            text: control.combinedAlignmentStatus
            color: "#ffd2d2"
            font.pixelSize: 12
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
                width: compactBadge ? 44 : Math.min(280, Math.max(80, panelWidth - 24))
                height: compactBadge ? 36 : 42
                radius: 5
                color: "#d9111721"
                border.color: "#663a4a62"

                Rectangle {
                    width: 26
                    height: 26
                    radius: 4
                    color: control.accentColor
                    anchors {
                        left: parent.left
                        leftMargin: surfaceLabel.compactBadge ? 9 : 8
                        verticalCenter: parent.verticalCenter
                    }

                    Text {
                        anchors.centerIn: parent
                        text: String.fromCharCode(65 + Number(surfaceLabel.modelData.slot))
                        color: "white"
                        font.bold: true
                        font.pixelSize: 13
                    }
                }

                Text {
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
        color: "#e6351f2a"
        border.color: "#b9503f4a"
        z: 40
        Accessible.name: qsTr("帧未变化。%1").arg(control.errorDetail)
        anchors {
            top: parent.top
            topMargin: alignmentStatus.visible ? alignmentStatus.height + 20 : 12
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
                color: "#ffb4b4"
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
        color: control.hasErrors && !control.busy ? "#ee351f2a" : "#ed151d29"
        border.color: control.hasErrors && !control.busy ? "#a9503f4a" : "#a43d4d64"
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
                color: control.hasErrors && !control.busy ? "#ffb4b4" : control.primaryTextColor
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
