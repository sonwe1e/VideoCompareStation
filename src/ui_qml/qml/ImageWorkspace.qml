pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "VcsTheme.js" as Theme

// Still-image workspace: single-image channel inspector + two-image DiffChecker.
Rectangle {
    id: control

    required property var controller
    required property var pairModel
    required property bool sidebarVisible

    signal openImageRequested
    signal addImageRequested
    signal openPairRequested
    signal compareFoldersRequested
    signal toggleSidebarRequested

    // False = fit-window display; true = true-size display where 1 image pixel maps to
    // 1 physical screen pixel (device-pixel-ratio aware). The two are separate commands
    // (T2); the controller zoom multiplies whichever base is active.
    property bool trueSize: false

    objectName: "imageWorkspace"
    color: "#06080d"
    activeFocusOnTab: true
    onVisibleChanged: {
        if (visible)
            Qt.callLater(() => control.forceActiveFocus());
    }

    readonly property var imageReview: controller
    readonly property bool hasPrimary: Boolean(controller && controller.hasPrimary)
    readonly property bool hasSecondary: Boolean(imageReview && imageReview.hasSecondary)
    readonly property bool hasPair: Boolean(imageReview && imageReview.hasPair)
    readonly property int compareMode: imageReview ? Number(imageReview.compareMode) : 0
    readonly property real wipePosition: imageReview ? Number(imageReview.wipePosition) : 0.5
    readonly property real zoom: imageReview ? Number(imageReview.zoom) : 1
    readonly property var cursorPixel: imageReview ? imageReview.cursorPixel : ({})
    readonly property string errorText: imageReview ? String(imageReview.errorText || "") : ""
    readonly property bool hasFolders: Boolean(pairModel && pairModel.pairCount > 0)
    readonly property bool sizesDiffer: Boolean(imageReview && imageReview.hasPair && imageReview.primaryWidth > 0 && imageReview.primaryWidth !== imageReview.secondaryWidth || imageReview && imageReview.hasPair && imageReview.primaryHeight > 0 && imageReview.primaryHeight !== imageReview.secondaryHeight)
    // Alpha difference (6) is a per-pixel diff over the straight-alpha channel.
    readonly property bool diffModeActive: Boolean(imageReview && imageReview.hasPair && imageReview.compareMode >= 2 && (imageReview.compareMode <= 4 || imageReview.compareMode === 6))
    readonly property int viewMode: imageReview ? Number(imageReview.viewMode) : 0
    // Background under transparent regions: 0 = dark, 1 = checkerboard, 2 = black, 3 = white.
    property int backgroundMode: 0
    property bool singleViewShowSecondary: false
    property bool flickerActive: false
    property int flickerIntervalMs: 400
    property var hoverPoint: null

    onHasPairChanged: {
        if (!hasPair) {
            flickerActive = false;
            singleViewShowSecondary = false;
            hoverPoint = null;
        }
    }
    onCompareModeChanged: {
        if (compareMode !== 0 && flickerActive)
            flickerActive = false;
    }

    function toggleSinglePairSource() {
        if (!hasPair)
            return;
        if (compareMode !== 0) {
            modeButton(0);
            singleViewShowSecondary = true;
        } else {
            singleViewShowSecondary = !singleViewShowSecondary;
        }
    }

    function toggleFlicker() {
        if (!hasPair)
            return;
        if (flickerActive) {
            flickerActive = false;
            singleViewShowSecondary = false;
        } else {
            if (compareMode !== 0)
                modeButton(0);
            flickerActive = true;
            singleViewShowSecondary = true;
            flickerTimer.restart();
        }
    }

    Timer {
        id: flickerTimer
        interval: control.flickerIntervalMs
        repeat: true
        running: control.flickerActive && control.hasPair
        onTriggered: {
            control.singleViewShowSecondary = !control.singleViewShowSecondary;
        }
    }

    function toggleAlphaView() {
        if (!imageReview || !hasPrimary)
            return;
        imageReview.viewMode = (imageReview.viewMode === 1 ? 0 : 1);
    }

    function toggleRgbOpaqueView() {
        if (!imageReview || !hasPrimary)
            return;
        imageReview.viewMode = (imageReview.viewMode === 2 ? 0 : 2);
    }

    function cycleBackgroundMode() {
        backgroundMode = (backgroundMode + 1) % 4;
    }

    // Physical-percent of the active display scale: true-size is 1 image px per physical
    // px (100%), fit shows the actual physical percentage of the fitted image.
    readonly property real displayPercent: {
        if (!imageReview || !imageReview.hasPrimary)
            return 0;
        const dpr = Window.window ? Window.window.devicePixelRatio : 1;
        const base = control.trueSize ? 1 / dpr : (primaryViewport ? primaryViewport.fitScale : 1);
        return Math.round(base * dpr * imageReview.zoom * 100);
    }
    readonly property string displayPercentMode: {
        if (control.trueSize) {
            return (control.imageReview && Math.abs(control.imageReview.zoom - 1.0) < 0.001) ? qsTr("100% 真实尺寸") : qsTr("1:1 像素基准");
        }
        return qsTr("适应窗口");
    }

    function modeButton(mode) {
        if (!imageReview)
            return;
        imageReview.compareMode = mode;
    }

    // File-name part of a controller path ("C:/dir/shot.png" -> "shot.png"); empty for
    // injected test images whose path label carries no separator.
    function imageFileName(pathValue) {
        const text = String(pathValue || "");
        const slash = Math.max(text.lastIndexOf("/"), text.lastIndexOf("\\"));
        return slash >= 0 ? text.substring(slash + 1) : text;
    }

    // Parent-folder part of a controller path, used to disambiguate same-named images.
    function imageParentLabel(pathValue) {
        const text = String(pathValue || "");
        const slash = Math.max(text.lastIndexOf("/"), text.lastIndexOf("\\"));
        if (slash < 0)
            return "";
        const parent = text.substring(0, slash);
        const parentSlash = Math.max(parent.lastIndexOf("/"), parent.lastIndexOf("\\"));
        return parentSlash >= 0 ? parent.substring(parentSlash + 1) : parent;
    }

    // "shot.png" or "shot.png (render_v1)" when both images share the file name.
    function imageTitle(pathValue, otherPathValue) {
        const name = imageFileName(pathValue);
        if (name.length === 0)
            return "";
        if (imageFileName(otherPathValue).localeCompare(name, Qt.CaseInsensitive) === 0) {
            const parent = imageParentLabel(pathValue);
            if (parent.length > 0)
                return "%1 (%2)".arg(name).arg(parent);
        }
        return name;
    }

    // Short source descriptor for the A/B labels, e.g. "16-bit gray16be · 显示转换 · α".
    function sourceSummary(bitDepth, format, hasAlpha, displayConverted) {
        let text = "";
        if (bitDepth > 8)
            text = qsTr("%1-bit").arg(bitDepth) + (format ? " " + format : "");
        else
            text = format && format.length > 0 && format !== "rgba" ? format : "RGBA8";
        if (hasAlpha)
            text += " · α";
        if (displayConverted)
            text += " · " + qsTr("显示转换");
        return text;
    }

    function mapToImage(view, mouseX, mouseY) {
        const img = view.image;
        if (!imageReview || !img || img.sourceSize.width <= 0 || img.sourceSize.height <= 0)
            return null;
        const sourceW = img.sourceSize.width;
        const sourceH = img.sourceSize.height;
        const scale = img.width / sourceW;
        if (!(scale > 0))
            return null;
        return {
            "x": (mouseX - img.x) / scale,
            "y": (mouseY - img.y) / scale,
            "scale": scale
        };
    }

    function applyHover(view, mouseX, mouseY, slot) {
        if (!imageReview)
            return;
        if (slot < 0) {
            imageReview.clearCursorPixel();
            control.hoverPoint = null;
            return;
        }
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped) {
            imageReview.clearCursorPixel();
            control.hoverPoint = null;
            return;
        }
        imageReview.updateCursorPixel(slot, mapped.x, mapped.y);
        const img = view.image;
        if (img && img.sourceSize.width > 0 && img.sourceSize.height > 0) {
            control.hoverPoint = {
                "sourceSlot": slot,
                "normX": Math.max(0, Math.min(1, mapped.x / img.sourceSize.width)),
                "normY": Math.max(0, Math.min(1, mapped.y / img.sourceSize.height)),
                "imgX": Math.round(mapped.x),
                "imgY": Math.round(mapped.y)
            };
        } else {
            control.hoverPoint = null;
        }
    }

    // Deterministic checkerboard behind transparent regions (cell scaled to the viewport),
    // so partial alpha reads as a blend against known colors instead of the dark void.
    component CheckerboardBackground: Canvas {
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            if (!ctx)
                return;
            const cell = Math.max(8, Math.min(16, Math.round(Math.max(width, height) / 56)));
            ctx.fillStyle = "#22262e";
            ctx.fillRect(0, 0, width, height);
            ctx.fillStyle = "#383e4a";
            for (let y = 0; y < height; y += cell) {
                const offset = (y / cell) % 2 === 0 ? 0 : cell;
                for (let x = offset; x < width; x += cell * 2)
                    ctx.fillRect(x, y, cell, cell);
            }
        }
    }

    component ModeChip: ReviewActionButton {
        id: chip

        required property int modeValue
        // 0 = compare mode, 1 = channel observation, 2 = background under alpha.
        property int selectGroup: 0
        property int chipWidth: 108
        checkable: true
        checked: selectGroup === 0 ? control.compareMode === modeValue : selectGroup === 1 ? control.viewMode === modeValue : control.backgroundMode === modeValue
        implicitHeight: 30
        implicitWidth: chipWidth
        leftPadding: 10
        rightPadding: 10
        enabled: selectGroup === 0 ? (control.hasPrimary && (modeValue === 0 || modeValue === 1 || control.hasPair)) : control.hasPrimary
        onClicked: {
            if (selectGroup === 0) {
                control.modeButton(modeValue);
            } else if (selectGroup === 1) {
                if (control.imageReview && control.imageReview.viewMode === modeValue && modeValue !== 0)
                    control.imageReview.viewMode = 0;
                else if (control.imageReview)
                    control.imageReview.viewMode = modeValue;
            } else {
                control.backgroundMode = modeValue;
            }
        }

        contentItem: Text {
            text: chip.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !chip.enabled ? Theme.disabledText : (chip.checked || chip.hovered ? Theme.primaryText : Theme.mutedText)
            font.pixelSize: 12
            font.weight: chip.checked ? Font.DemiBold : Font.Normal
        }

        background: Rectangle {
            radius: 5
            color: !chip.enabled ? Theme.disabledPanel : (chip.checked ? Theme.controlChecked : (chip.down ? Theme.controlPressed : (chip.hovered ? Theme.controlHover : Theme.control)))
            border.width: chip.activeFocus ? 2 : 1
            border.color: chip.activeFocus ? Theme.accent : (chip.checked ? Theme.accent : Theme.controlBorder)
        }
    }

    component ImageViewport: Item {
        id: viewport

        required property int slot
        property string imageUrl: ""
        property string label: ""
        property string title: ""
        property string titlePath: ""
        property point dragStart: Qt.point(0, 0)

        readonly property alias image: previewImage
        // Fit scale of the displayed image (fit-window base), forwarded so the workspace
        // status bar can report the physical display percentage.
        readonly property real fitScale: previewImage ? previewImage.fitScale : 1

        clip: true

        Rectangle {
            anchors.fill: parent
            color: "#090d14"
            visible: control.backgroundMode === 0
        }
        Rectangle {
            anchors.fill: parent
            color: "black"
            visible: control.backgroundMode === 2
        }
        Rectangle {
            anchors.fill: parent
            color: "white"
            visible: control.backgroundMode === 3
        }
        CheckerboardBackground {
            anchors.fill: parent
            visible: control.backgroundMode === 1
        }
        Rectangle {
            anchors.fill: parent
            color: "transparent"
            border.color: Theme.border
            border.width: 1
            radius: 6
        }

        Image {
            id: previewImage

            objectName: "imageViewport-" + viewport.slot
            readonly property real fitScale: {
                const sw = Math.max(1, sourceSize.width);
                const sh = Math.max(1, sourceSize.height);
                return Math.min(viewport.width / sw, viewport.height / sh);
            }
            // True-size: 1 image pixel occupies 1 physical pixel, so the item's
            // device-independent width is sourceSize / DPR (T2).
            readonly property real trueSizeScale: {
                const dpr = Window.window ? Window.window.devicePixelRatio : 1;
                return 1 / dpr;
            }
            readonly property real effectiveBaseScale: control.trueSize ? trueSizeScale : fitScale
            readonly property real drawWidth: sourceSize.width * effectiveBaseScale * control.zoom
            readonly property real drawHeight: sourceSize.height * effectiveBaseScale * control.zoom
            x: (viewport.width - drawWidth) / 2 + (0.5 - (control.imageReview ? control.imageReview.panX : 0.5)) * drawWidth
            y: (viewport.height - drawHeight) / 2 + (0.5 - (control.imageReview ? control.imageReview.panY : 0.5)) * drawHeight
            width: drawWidth
            height: drawHeight
            source: viewport.imageUrl
            fillMode: Image.Stretch
            asynchronous: false
            cache: false
            smooth: control.zoom <= 2
        }

        Column {
            visible: viewport.label.length > 0 || viewport.title.length > 0
            spacing: 2
            anchors {
                top: parent.top
                left: parent.left
                margins: 10
            }

            Text {
                visible: viewport.title.length > 0
                objectName: "imageViewportTitle-" + viewport.slot
                text: viewport.title
                color: Theme.primaryText
                font.pixelSize: 12
                font.weight: Font.DemiBold
                elide: Text.ElideMiddle

                HoverHandler {
                    id: viewportTitleHover
                }

                VcsToolTip {
                    visible: viewportTitleHover.hovered && viewport.titlePath.length > 0
                    text: viewport.titlePath
                }
            }

            Text {
                visible: viewport.label.length > 0
                text: viewport.label
                color: Theme.mutedText
                font.pixelSize: 11
            }
        }

        Item {
            id: syncedCrosshair
            objectName: "syncedCrosshair-" + viewport.slot
            anchors.fill: parent
            visible: Boolean(control.hoverPoint && control.hasPair && control.compareMode === 1 && previewImage.status === Image.Ready)
            z: 15

            readonly property real targetX: control.hoverPoint ? previewImage.x + control.hoverPoint.normX * previewImage.width : 0
            readonly property real targetY: control.hoverPoint ? previewImage.y + control.hoverPoint.normY * previewImage.height : 0
            readonly property bool isHoveredSource: Boolean(control.hoverPoint && control.hoverPoint.sourceSlot === viewport.slot)

            // Vertical hairline
            Rectangle {
                x: Math.round(syncedCrosshair.targetX)
                y: Math.max(0, previewImage.y)
                width: 1
                height: Math.min(viewport.height, previewImage.height)
                color: syncedCrosshair.isHoveredSource ? "#55ffffff" : "#d9facc15"
            }

            // Horizontal hairline
            Rectangle {
                x: Math.max(0, previewImage.x)
                y: Math.round(syncedCrosshair.targetY)
                width: Math.min(viewport.width, previewImage.width)
                height: 1
                color: syncedCrosshair.isHoveredSource ? "#55ffffff" : "#d9facc15"
            }

            // Synced target reticle (shown on the mirror/other viewport)
            Rectangle {
                visible: !syncedCrosshair.isHoveredSource
                x: Math.round(syncedCrosshair.targetX - 9)
                y: Math.round(syncedCrosshair.targetY - 9)
                width: 18
                height: 18
                radius: 9
                color: "transparent"
                border.color: "#facc15"
                border.width: 1.5
            }

            Rectangle {
                visible: !syncedCrosshair.isHoveredSource
                x: Math.round(syncedCrosshair.targetX - 2)
                y: Math.round(syncedCrosshair.targetY - 2)
                width: 4
                height: 4
                radius: 2
                color: "#facc15"
            }

            // Coordinate tag on the synced viewport
            Rectangle {
                visible: !syncedCrosshair.isHoveredSource && control.hoverPoint !== null
                x: Math.min(viewport.width - width - 8, Math.max(8, Math.round(syncedCrosshair.targetX + 12)))
                y: Math.min(viewport.height - height - 8, Math.max(8, Math.round(syncedCrosshair.targetY + 12)))
                width: coordText.implicitWidth + 8
                height: 18
                radius: 3
                color: "#d91e293b"
                border.color: "#facc15"
                border.width: 1

                Text {
                    id: coordText
                    anchors.centerIn: parent
                    text: control.hoverPoint ? "%1, %2".arg(control.hoverPoint.imgX).arg(control.hoverPoint.imgY) : ""
                    color: "#facc15"
                    font.pixelSize: 10
                    font.family: "Consolas"
                    font.weight: Font.Bold
                }
            }

            // Subtle center pip for the hovered viewport
            Rectangle {
                visible: syncedCrosshair.isHoveredSource
                x: Math.round(syncedCrosshair.targetX - 2)
                y: Math.round(syncedCrosshair.targetY - 2)
                width: 4
                height: 4
                radius: 2
                color: "#ffffff"
                opacity: 0.8
            }
        }

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onPositionChanged: mouse => {
                if (pressed && control.imageReview) {
                    const dx = (mouse.x - viewport.dragStart.x) / Math.max(1, width);
                    const dy = (mouse.y - viewport.dragStart.y) / Math.max(1, height);
                    control.imageReview.panBy(dx, dy);
                    viewport.dragStart = Qt.point(mouse.x, mouse.y);
                }
                control.applyHover(viewport, mouse.x, mouse.y, viewport.slot);
            }
            onExited: {
                if (control.imageReview)
                    control.imageReview.clearCursorPixel();
                control.hoverPoint = null;
            }
            onPressed: mouse => {
                viewport.dragStart = Qt.point(mouse.x, mouse.y);
                control.forceActiveFocus();
            }
            onDoubleClicked: mouse => {
                if (control.trueSize) {
                    control.trueSize = false;
                    if (control.imageReview)
                        control.imageReview.resetView();
                } else {
                    control.trueSize = true;
                    if (control.imageReview)
                        control.imageReview.setZoom(1.0);
                }
            }
            onWheel: wheel => {
                if (!control.imageReview)
                    return;
                const mapped = control.mapToImage(viewport, wheel.x, wheel.y);
                const ax = mapped ? Math.max(0, Math.min(1, mapped.x / Math.max(1, previewImage.sourceSize.width))) : 0.5;
                const ay = mapped ? Math.max(0, Math.min(1, mapped.y / Math.max(1, previewImage.sourceSize.height))) : 0.5;
                control.imageReview.zoomBy(wheel.angleDelta.y > 0 ? 1.25 : 0.8, ax, ay);
                wheel.accepted = true;
            }
        }
    }

    Column {
        id: headerColumn

        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: 12
        }
        spacing: 10

        Row {
            spacing: 8

            ReviewActionButton {
                objectName: "imageOpenButton"
                text: qsTr("打开图片…")
                prominent: true
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                onClicked: control.openImageRequested()
            }
            ReviewActionButton {
                objectName: "imageAddButton"
                text: qsTr("添加图片…")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasPrimary && !control.hasSecondary
                onClicked: control.addImageRequested()
            }
            ReviewActionButton {
                objectName: "imageOpenPairButton"
                text: qsTr("打开图片对…")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                onClicked: control.openPairRequested()
            }
            ReviewActionButton {
                objectName: "imageCompareFoldersButton"
                text: qsTr("对比文件夹…")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                onClicked: control.compareFoldersRequested()
            }
            ReviewActionButton {
                objectName: "imageToggleSidebarButton"
                checkable: true
                checked: control.sidebarVisible
                text: control.sidebarVisible ? qsTr("隐藏列表") : qsTr("显示列表")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasFolders
                onClicked: control.toggleSidebarRequested()
            }
            ReviewActionButton {
                objectName: "imageCloseButton"
                text: qsTr("关闭图片")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasPrimary
                onClicked: control.imageReview.closeAll()
            }
            ReviewActionButton {
                objectName: "imageResetViewButton"
                text: qsTr("重置视图")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasPrimary
                onClicked: {
                    control.trueSize = false;
                    if (control.imageReview)
                        control.imageReview.resetView();
                }
            }
            ReviewActionButton {
                objectName: "imageFitButton"
                checkable: true
                checked: !control.trueSize
                text: qsTr("适应窗口")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasPrimary
                onClicked: {
                    control.trueSize = false;
                    if (control.imageReview)
                        control.imageReview.resetView();
                }
            }
            ReviewActionButton {
                objectName: "imageTrueSizeButton"
                checkable: true
                checked: control.trueSize
                text: qsTr("100% 真实尺寸")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                enabled: control.hasPrimary
                onClicked: {
                    control.trueSize = true;
                    if (control.imageReview)
                        control.imageReview.setZoom(1.0);
                }
            }
            ReviewActionButton {
                objectName: "imageResampleToggle"
                checkable: true
                checked: Boolean(control.imageReview && control.imageReview.resampleAllowed)
                text: qsTr("重采样对齐差异")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                visible: control.hasPair && control.sizesDiffer
                onClicked: control.imageReview.resampleAllowed = !control.imageReview.resampleAllowed
            }
        }

        Row {
            spacing: 6

            ModeChip {
                objectName: "imageModePrimary"
                text: control.hasPair ? qsTr("单图/切换") : qsTr("查看")
                modeValue: 0
            }
            ModeChip {
                objectName: "imageModeSide"
                text: qsTr("并排")
                modeValue: 1
            }
            ReviewActionButton {
                objectName: "imageToggleSourceButton"
                visible: control.hasPair
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                text: control.compareMode === 0 && control.singleViewShowSecondary ? qsTr("切至 A") : qsTr("切至 B")
                onClicked: control.toggleSinglePairSource()
            }
            ReviewActionButton {
                objectName: "imageFlickerButton"
                visible: control.hasPair
                checkable: true
                checked: control.flickerActive
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                text: control.flickerActive ? qsTr("停止闪烁") : qsTr("闪烁 (Flicker)")
                onClicked: control.toggleFlicker()
            }
            ModeChip {
                objectName: "imageModeWipe"
                text: qsTr("分割线")
                modeValue: 5
            }
            ModeChip {
                objectName: "imageModeAbsDiff"
                text: qsTr("绝对差异")
                modeValue: 2
            }
            ModeChip {
                objectName: "imageModeSignedDiff"
                text: qsTr("带符号差异")
                modeValue: 3
            }
            ModeChip {
                objectName: "imageModeHighlight"
                text: qsTr("高亮")
                modeValue: 4
            }
            ModeChip {
                objectName: "imageModeAlphaDiff"
                text: qsTr("Alpha 差异")
                modeValue: 6
                chipWidth: 96
                visible: Boolean(control.imageReview && control.hasPair && (control.imageReview.primaryHasAlpha || control.imageReview.secondaryHasAlpha))
            }
        }

        Row {
            spacing: 6

            Text {
                height: 30
                verticalAlignment: Text.AlignVCenter
                text: qsTr("观察")
                color: Theme.mutedText
                font.pixelSize: 12
            }
            ModeChip {
                objectName: "imageViewRgba"
                text: qsTr("RGBA")
                modeValue: 0
                selectGroup: 1
                chipWidth: 76
            }
            ModeChip {
                objectName: "imageViewAlphaGray"
                text: qsTr("Alpha 灰度 (A)")
                modeValue: 1
                selectGroup: 1
                chipWidth: 116
            }
            ModeChip {
                objectName: "imageViewRgbOpaque"
                text: qsTr("RGB 忽略透明度 (O)")
                modeValue: 2
                selectGroup: 1
                chipWidth: 148
            }

            Item {
                width: 10
                height: 1
            }

            Text {
                height: 30
                verticalAlignment: Text.AlignVCenter
                text: qsTr("背景")
                color: Theme.mutedText
                font.pixelSize: 12
            }
            ModeChip {
                objectName: "imageBgDark"
                text: qsTr("深色")
                modeValue: 0
                selectGroup: 2
                chipWidth: 64
            }
            ModeChip {
                objectName: "imageBgChecker"
                text: qsTr("棋盘格")
                modeValue: 1
                selectGroup: 2
                chipWidth: 76
            }
            ModeChip {
                objectName: "imageBgBlack"
                text: qsTr("黑底")
                modeValue: 2
                selectGroup: 2
                chipWidth: 64
            }
            ModeChip {
                objectName: "imageBgWhite"
                text: qsTr("白底")
                modeValue: 3
                selectGroup: 2
                chipWidth: 64
            }
            ReviewActionButton {
                objectName: "imageBgCycleButton"
                text: qsTr("循环背景 (B)")
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                enabled: control.hasPrimary
                onClicked: control.cycleBackgroundMode()
            }

            Rectangle {
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryHasAlpha || control.imageReview.secondaryHasAlpha))
                height: 24
                radius: 4
                color: "#1e293b"
                border.color: "#38bdf8"
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Row {
                    anchors.centerIn: parent
                    leftPadding: 6
                    rightPadding: 6
                    spacing: 4

                    Text {
                        text: "α"
                        color: "#38bdf8"
                        font.pixelSize: 12
                        font.weight: Font.Bold
                    }
                    Text {
                        text: qsTr("直通（未预乘）")
                        color: "#e2e8f0"
                        font.pixelSize: 11
                    }
                }
            }
        }
    }

    // Left/Right/Up/Down walk folder pairs when a comparison list is loaded; Home/End jump
    // to the first/last complete pair.
    Keys.onLeftPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(-1);
            event.accepted = true;
        }
    }
    Keys.onRightPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(1);
            event.accepted = true;
        }
    }
    Keys.onUpPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(-1);
            event.accepted = true;
        }
    }
    Keys.onDownPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(1);
            event.accepted = true;
        }
    }
    // Space hold-to-compare: temporarily shows secondary image (B/Prediction) in single view.
    Keys.onSpacePressed: event => {
        if (control.hasPair && !event.isAutoRepeat && !control.flickerActive) {
            if (control.compareMode === 0) {
                control.singleViewShowSecondary = true;
                event.accepted = true;
            }
        }
    }
    Keys.onReleased: event => {
        if (event.key === Qt.Key_Space) {
            if (control.hasPair && !event.isAutoRepeat && !control.flickerActive) {
                if (control.compareMode === 0) {
                    control.singleViewShowSecondary = false;
                    event.accepted = true;
                }
            }
        }
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            if (control.hasFolders && folderPairSidebar) {
                folderPairSidebar.stepFirst();
                event.accepted = true;
            }
        } else if (event.key === Qt.Key_End) {
            if (control.hasFolders && folderPairSidebar) {
                folderPairSidebar.stepLast();
                event.accepted = true;
            }
        } else if (event.key === Qt.Key_T || event.key === Qt.Key_Tab || event.key === Qt.Key_QuoteLeft) {
            if (control.hasPair) {
                control.toggleSinglePairSource();
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_A) {
            if (control.hasPrimary) {
                control.toggleAlphaView();
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_B) {
            if (control.hasPrimary) {
                control.cycleBackgroundMode();
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_O) {
            if (control.hasPrimary) {
                control.toggleRgbOpaqueView();
                event.accepted = true;
            }
        }
    }

    Item {
        id: stage

        anchors {
            top: headerColumn.bottom
            topMargin: 12
            left: parent.left
            right: parent.right
            bottom: statusBar.top
            bottomMargin: 12
            leftMargin: 12
            rightMargin: 12
        }

        Row {
            id: stageRow

            anchors.fill: parent
            spacing: 12

            FolderPairSidebar {
                id: folderPairSidebar

                objectName: "folderPairSidebarHost"
                visible: control.sidebarVisible && control.hasFolders
                width: 250
                height: parent.height
                pairModel: control.pairModel
                hasFolders: control.hasFolders
                onPairSelected: row => control.forceActiveFocus()
                onChangeFoldersRequested: control.compareFoldersRequested()
            }

            Item {
                id: stageContent

                width: parent.width - (folderPairSidebar.visible ? folderPairSidebar.width + parent.spacing : 0)
                height: parent.height

                Text {
                    visible: !control.hasPrimary
                    anchors.centerIn: parent
                    text: qsTr("打开或拖入一张图片查看通道；打开两张图片可进行差异或分割线对比。")
                    color: Theme.mutedText
                    font.pixelSize: 16
                }

                // In-place A/B compare and Flicker HUD badge
                Rectangle {
                    id: imageInPlaceBadge
                    objectName: "imageInPlaceBadge"
                    visible: control.hasPair && control.compareMode === 0
                    z: 30
                    height: 32
                    radius: 6
                    color: control.singleViewShowSecondary ? "#d90284c7" : "#d916a34a"
                    border.color: "#ffffff"
                    border.width: 1
                    anchors {
                        top: parent.top
                        right: parent.right
                        margins: 14
                    }

                    Row {
                        spacing: 8
                        anchors.centerIn: parent
                        leftPadding: 10
                        rightPadding: 10

                        Text {
                            text: control.flickerActive ? (control.singleViewShowSecondary ? qsTr("⚡ 闪烁对比 · 候选 B") : qsTr("⚡ 闪烁对比 · 基准 A")) : (control.singleViewShowSecondary ? qsTr("原地对比 · 当前：B (候选)") : qsTr("原地对比 · 当前：A (基准)"))
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.weight: Font.Bold
                        }

                        Text {
                            visible: !control.flickerActive
                            text: qsTr("按住空格切换 · 按 T 切换")
                            color: "#e2e8f0"
                            font.pixelSize: 11
                        }

                        Text {
                            visible: control.flickerActive
                            text: qsTr("点击停止")
                            color: "#fef08a"
                            font.pixelSize: 11
                            font.weight: Font.DemiBold
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (control.flickerActive)
                                control.toggleFlicker();
                            else
                                control.toggleSinglePairSource();
                        }
                    }
                }

                // Alpha & Background observation HUD badge
                Rectangle {
                    id: imageAlphaObservationBadge
                    objectName: "imageAlphaObservationBadge"
                    visible: control.hasPrimary && (control.viewMode !== 0 || control.backgroundMode !== 0)
                    z: 30
                    height: 32
                    radius: 6
                    color: control.viewMode === 1 ? "#d92563eb" : (control.viewMode === 2 ? "#d97c3aed" : "#d9334155")
                    border.color: "#ffffff"
                    border.width: 1
                    anchors {
                        top: parent.top
                        right: imageInPlaceBadge.visible ? imageInPlaceBadge.left : parent.right
                        rightMargin: imageInPlaceBadge.visible ? 10 : 14
                        topMargin: 14
                    }

                    Row {
                        spacing: 8
                        anchors.centerIn: parent
                        leftPadding: 10
                        rightPadding: 10

                        Text {
                            id: imageAlphaObservationText
                            objectName: "imageAlphaObservationText"
                            text: {
                                if (control.viewMode === 1)
                                    return qsTr("🔲 观察：Alpha 灰度通道");
                                if (control.viewMode === 2)
                                    return qsTr("👁️ 观察：RGB 忽略透明度");
                                const bgNames = [qsTr("深色底"), qsTr("棋盘格底"), qsTr("黑底"), qsTr("白底")];
                                return qsTr("🎨 背景：%1").arg(bgNames[control.backgroundMode] || "");
                            }
                            color: "#ffffff"
                            font.pixelSize: 12
                            font.weight: Font.Bold
                        }

                        Text {
                            text: {
                                if (control.viewMode === 1)
                                    return qsTr("按 A 还原 RGBA · 点击还原");
                                if (control.viewMode === 2)
                                    return qsTr("按 O 还原 RGBA · 点击还原");
                                return qsTr("按 B 切换 · 点击切换");
                            }
                            color: "#e2e8f0"
                            font.pixelSize: 11
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (control.viewMode !== 0) {
                                if (control.imageReview)
                                    control.imageReview.viewMode = 0;
                            } else {
                                control.cycleBackgroundMode();
                            }
                        }
                    }
                }

                Row {
                    visible: control.hasPrimary && (control.compareMode === 0 || control.compareMode === 1 || !control.hasPair)
                    anchors.fill: parent
                    spacing: 12

                    ImageViewport {
                        id: primaryViewport

                        objectName: "primaryViewport"
                        slot: (control.compareMode === 0 && control.hasPair && control.singleViewShowSecondary) ? 3 : 2
                        width: control.compareMode === 1 && control.hasPair ? parent.width / 2 - 6 : parent.width
                        height: parent.height
                        imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(slot) : ""
                        label: {
                            if (slot === 3)
                                return control.hasSecondary ? qsTr("B · %1×%2 · %3").arg(control.imageReview.secondaryWidth).arg(control.imageReview.secondaryHeight).arg(control.sourceSummary(control.imageReview.secondaryBitDepth, control.imageReview.secondarySourceFormat, control.imageReview.secondaryHasAlpha, control.imageReview.secondaryDisplayConverted)) : "";
                            return control.hasPrimary ? qsTr("A · %1×%2 · %3").arg(control.imageReview.primaryWidth).arg(control.imageReview.primaryHeight).arg(control.sourceSummary(control.imageReview.primaryBitDepth, control.imageReview.primarySourceFormat, control.imageReview.primaryHasAlpha, control.imageReview.primaryDisplayConverted)) : "";
                        }
                        title: {
                            if (slot === 3)
                                return control.hasSecondary ? control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath) : "";
                            return control.hasPrimary ? control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath) : "";
                        }
                        titlePath: {
                            if (slot === 3)
                                return control.hasSecondary ? String(control.imageReview.secondaryPath || "") : "";
                            return control.hasPrimary ? String(control.imageReview.primaryPath || "") : "";
                        }
                    }

                    ImageViewport {
                        objectName: "secondaryViewport"
                        slot: 3
                        visible: control.compareMode === 1 && control.hasPair
                        width: visible ? parent.width / 2 - 6 : 0
                        height: parent.height
                        imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(3) : ""
                        label: control.hasSecondary ? qsTr("B · %1×%2 · %3").arg(control.imageReview.secondaryWidth).arg(control.imageReview.secondaryHeight).arg(control.sourceSummary(control.imageReview.secondaryBitDepth, control.imageReview.secondarySourceFormat, control.imageReview.secondaryHasAlpha, control.imageReview.secondaryDisplayConverted)) : ""
                        title: control.hasSecondary ? control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath) : ""
                        titlePath: control.hasSecondary ? String(control.imageReview.secondaryPath || "") : ""
                    }
                }

                ImageViewport {
                    objectName: "diffViewport"
                    slot: 4
                    visible: control.hasPair && control.imageReview && control.imageReview.hasDiffResult && (control.compareMode >= 2 && control.compareMode <= 4 || control.compareMode === 6)
                    anchors.fill: parent
                    imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(4) : ""
                    label: {
                        if (!control.imageReview || !control.imageReview.hasDiffResult)
                            return "";
                        const alphaMode = control.compareMode === 6;
                        const modeName = control.compareMode === 2 ? qsTr("绝对差异") : (control.compareMode === 3 ? qsTr("带符号差异") : (control.compareMode === 4 ? qsTr("高亮") : qsTr("Alpha 差异")));
                        let text;
                        if (alphaMode)
                            text = qsTr("%1 · 峰值 α %2 · 均值 α %3 · 变化像素 %4 · 统计 %5").arg(modeName).arg(control.imageReview.peakAlphaDifference).arg(control.imageReview.meanAlphaDifference.toFixed(2)).arg(control.imageReview.alphaChangedPixels).arg(control.imageReview.diffScopeText);
                        else
                            text = qsTr("%1 · 峰值 %2 · 均值 %3 · 统计 %4").arg(modeName).arg(control.imageReview.maxAbsDifference).arg(control.imageReview.meanAbsDifference.toFixed(2)).arg(control.imageReview.diffScopeText);
                        if (control.imageReview.diffResampled)
                            text += " · " + qsTr("已重采样对齐");
                        if (control.imageReview.alphaDifferenceOnly)
                            text += " · " + qsTr("RGB 相同，alpha 存在差异");
                        return text;
                    }
                }

                // Diff mode on an unequal-size pair without resampling: no pixels are
                // fabricated and no stats are pretended (T2). The controller errorText
                // carries the exact sizes; the toggle in the toolbar opts into resampling.
                // A retained diff mode on a same-size pair reports the in-flight
                // computation instead of a false size complaint (T6).
                Column {
                    id: diffNoticeColumn
                    objectName: "imageDiffUnavailableNotice"
                    visible: control.diffModeActive && control.imageReview && !control.imageReview.hasDiffResult
                    anchors.centerIn: parent
                    width: parent.width * 0.7
                    spacing: 12

                    Text {
                        width: parent.width
                        horizontalAlignment: Text.AlignHCenter
                        wrapMode: Text.WordWrap
                        text: {
                            if (control.imageReview && control.imageReview.diffPending)
                                return qsTr("正在后台计算差异…");
                            if (control.sizesDiffer)
                                return qsTr("A 与 B 尺寸不同，未计算逐像素差异。可开启“重采样对齐差异”后再比较。");
                            return qsTr("差异暂不可用。");
                        }
                        color: Theme.warning
                        font.pixelSize: 13
                    }

                    ReviewActionButton {
                        objectName: "diffEnableResampleButton"
                        anchors.horizontalCenter: parent.horizontalCenter
                        visible: control.sizesDiffer && control.imageReview && !control.imageReview.resampleAllowed
                        text: qsTr("开启重采样对齐")
                        prominent: true
                        implicitHeight: 28
                        leftPadding: 12
                        rightPadding: 12
                        onClicked: {
                            if (control.imageReview)
                                control.imageReview.resampleAllowed = true;
                        }
                    }
                }

                // Split-line comparison: the secondary image is clipped at the split position and stacked
                // over the primary, both drawn in the primary's geometry box so zoom/pan stay aligned.
                Item {
                    id: wipeOverlay

                    objectName: "wipeOverlay"
                    visible: control.hasPair && control.compareMode === 5
                    anchors.fill: parent
                    clip: true

                    Rectangle {
                        anchors.fill: parent
                        color: "#090d14"
                        visible: control.backgroundMode === 0
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: "black"
                        visible: control.backgroundMode === 2
                    }
                    Rectangle {
                        anchors.fill: parent
                        color: "white"
                        visible: control.backgroundMode === 3
                    }
                    CheckerboardBackground {
                        anchors.fill: parent
                        visible: control.backgroundMode === 1
                    }

                    readonly property real fitScale: {
                        if (!control.imageReview)
                            return 1;
                        const sw = Math.max(1, control.imageReview.primaryWidth);
                        const sh = Math.max(1, control.imageReview.primaryHeight);
                        return Math.min(width / sw, height / sh);
                    }
                    readonly property real effectiveBaseScale: {
                        if (control.trueSize) {
                            const dpr = Window.window ? Window.window.devicePixelRatio : 1;
                            return 1 / dpr;
                        }
                        return fitScale;
                    }
                    readonly property real drawWidth: control.imageReview ? control.imageReview.primaryWidth * effectiveBaseScale * control.zoom : 0
                    readonly property real drawHeight: control.imageReview ? control.imageReview.primaryHeight * effectiveBaseScale * control.zoom : 0
                    readonly property real drawX: (width - drawWidth) / 2 + (0.5 - (control.imageReview ? control.imageReview.panX : 0.5)) * drawWidth
                    readonly property real drawY: (height - drawHeight) / 2 + (0.5 - (control.imageReview ? control.imageReview.panY : 0.5)) * drawHeight
                    readonly property real splitX: width * control.wipePosition

                    Image {
                        id: wipePrimaryImage

                        objectName: "wipePrimaryImage"
                        x: wipeOverlay.drawX
                        y: wipeOverlay.drawY
                        width: wipeOverlay.drawWidth
                        height: wipeOverlay.drawHeight
                        source: control.imageReview && control.imageReview.contentGeneration >= 0 && wipeOverlay.visible ? control.imageReview.imageUrl(2) : ""
                        fillMode: Image.Stretch
                        asynchronous: false
                        cache: false
                        smooth: control.zoom <= 2
                    }

                    Item {
                        id: wipeClip

                        objectName: "wipeClip"
                        width: Math.round(wipeOverlay.splitX)
                        height: wipeOverlay.height
                        clip: true

                        Image {
                            id: wipeSecondaryImage

                            objectName: "wipeSecondaryImage"
                            x: wipeOverlay.drawX
                            y: wipeOverlay.drawY
                            width: wipeOverlay.drawWidth
                            height: wipeOverlay.drawHeight
                            source: control.imageReview && control.imageReview.contentGeneration >= 0 && wipeOverlay.visible ? control.imageReview.imageUrl(3) : ""
                            fillMode: Image.Stretch
                            asynchronous: false
                            cache: false
                            smooth: control.zoom <= 2
                        }
                    }

                    // T6: persistent A/B identity for the wipe halves. The left of the
                    // split shows the secondary image and the right shows the primary, so
                    // the labels name them in that order and can never be misread as
                    // reversed while the split is dragged.
                    Text {
                        id: wipeIdentityLeft

                        objectName: "wipeIdentityLeft"
                        visible: control.hasPair && control.hasSecondary && width > 4
                        text: control.hasSecondary ? qsTr("B · %1").arg(control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath)) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        width: Math.max(0, wipeOverlay.splitX - 20)
                        anchors {
                            top: parent.top
                            left: parent.left
                            margins: 10
                        }
                    }

                    Text {
                        id: wipeIdentityRight

                        objectName: "wipeIdentityRight"
                        visible: control.hasPair && width > 4
                        text: control.hasPrimary ? qsTr("A · %1").arg(control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath)) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                        width: Math.max(0, wipeOverlay.width - wipeOverlay.splitX - 20)
                        anchors {
                            top: parent.top
                            right: parent.right
                            margins: 10
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        property point dragStart: Qt.point(0, 0)
                        onPositionChanged: mouse => {
                            if (pressed && control.imageReview) {
                                const dx = (mouse.x - dragStart.x) / Math.max(1, width);
                                const dy = (mouse.y - dragStart.y) / Math.max(1, height);
                                control.imageReview.panBy(dx, dy);
                                dragStart = Qt.point(mouse.x, mouse.y);
                            }
                            const leftSide = mouse.x < wipeOverlay.splitX;
                            control.applyHover({
                                "image": leftSide ? wipeSecondaryImage : wipePrimaryImage
                            }, mouse.x, mouse.y, leftSide ? 3 : 2);
                        }
                        onExited: {
                            if (control.imageReview)
                                control.imageReview.clearCursorPixel();
                        }
                        onPressed: mouse => {
                            dragStart = Qt.point(mouse.x, mouse.y);
                            control.forceActiveFocus();
                        }
                        onDoubleClicked: mouse => {
                            if (control.trueSize) {
                                control.trueSize = false;
                                if (control.imageReview)
                                    control.imageReview.resetView();
                            } else {
                                control.trueSize = true;
                                if (control.imageReview)
                                    control.imageReview.setZoom(1.0);
                            }
                        }
                        onWheel: wheel => {
                            if (!control.imageReview)
                                return;
                            const img = wheel.x < wipeOverlay.splitX ? wipeSecondaryImage : wipePrimaryImage;
                            const mapped = control.mapToImage({
                                "image": img
                            }, wheel.x, wheel.y);
                            const ax = mapped ? Math.max(0, Math.min(1, mapped.x / Math.max(1, img.sourceSize.width))) : 0.5;
                            const ay = mapped ? Math.max(0, Math.min(1, mapped.y / Math.max(1, img.sourceSize.height))) : 0.5;
                            control.imageReview.zoomBy(wheel.angleDelta.y > 0 ? 1.25 : 0.8, ax, ay);
                            wheel.accepted = true;
                        }
                    }

                    WipeHandle {
                        surfaceItem: wipeOverlay
                        position: control.wipePosition
                        onPositionRequested: position => control.imageReview.wipePosition = position
                    }
                }
            }
        }
    }

    Rectangle {
        id: statusBar

        objectName: "imageStatusBar"
        height: 34
        color: Theme.panel
        border.color: Theme.border
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        Row {
            spacing: 18
            anchors {
                left: parent.left
                leftMargin: 14
                verticalCenter: parent.verticalCenter
            }

            Text {
                text: qsTr("缩放 %1%（%2）").arg(control.displayPercent).arg(control.displayPercentMode)
                color: Theme.primaryText
                font.pixelSize: 12
            }
            // T6: current folder-row context — which row is committed, whether one side
            // is missing, and whether the pairing is ambiguous.
            Text {
                objectName: "imagePairContextLabel"
                visible: Boolean(control.pairModel && control.pairModel.pairCount > 0 && control.pairModel.currentPair >= 0)
                text: {
                    if (!control.pairModel || control.pairModel.currentPair < 0)
                        return "";
                    const info = control.pairModel.pairUrlsAt(Number(control.pairModel.currentPair));
                    if (!info || info.row === undefined)
                        return "";
                    const parts = [qsTr("第 %1/%2 行").arg(Number(info.row) + 1).arg(control.pairModel.pairCount)];
                    if (!info.hasBoth)
                        parts.push(info.hasLeft ? qsTr("仅 A 存在") : qsTr("仅 B 存在"));
                    if (info.caseConflict)
                        parts.push(qsTr("大小写冲突"));
                    return parts.join(" · ");
                }
                color: Theme.mutedText
                font.pixelSize: 12
            }
            Text {
                visible: Boolean(control.cursorPixel && control.cursorPixel.valid)
                text: {
                    if (!control.cursorPixel || !control.cursorPixel.valid)
                        return "";
                    let t = qsTr("像素 (%1, %2)  R %3  G %4  B %5  A %6  %7  α %8%").arg(control.cursorPixel.x).arg(control.cursorPixel.y).arg(control.cursorPixel.r).arg(control.cursorPixel.g).arg(control.cursorPixel.b).arg(control.cursorPixel.a).arg(control.cursorPixel.hex).arg(control.cursorPixel.alphaPercent);
                    if (control.cursorPixel.channelView === "alphaGray")
                        t += " · " + qsTr("Alpha 灰度");
                    else if (control.cursorPixel.channelView === "rgbOpaque")
                        t += " · " + qsTr("RGB 忽略透明度");
                    return t;
                }
                color: Theme.primaryText
                font.pixelSize: 12
                font.family: "Consolas"
            }
            Text {
                visible: control.errorText.length > 0
                text: control.errorText
                color: Theme.error
                font.pixelSize: 12
            }
        }

        Text {
            anchors {
                right: parent.right
                rightMargin: 14
                verticalCenter: parent.verticalCenter
            }
            text: qsTr("滚轮缩放 · 拖动平移")
            color: Theme.mutedText
            font.pixelSize: 11
        }
    }

    // T4/T6: warm the next complete pair in the background after a successful folder
    // commit, so stepping onto it is a cache hit. Bounded to one neighbour; a user
    // request cancels outstanding prefetches and the visible pair never changes.
    Connections {
        target: control.pairModel

        function onPairOpenFinished(row, success, error) {
            if (!success || !control.imageReview || !control.pairModel)
                return;
            const next = control.pairModel.stepCompleteRow(1);
            if (next < 0 || next === Number(row))
                return;
            const urls = control.pairModel.pairUrlsAt(next);
            if (urls.hasLeft && urls.hasRight)
                control.imageReview.prefetchPair(urls.leftUrl, urls.rightUrl);
        }
    }
}
