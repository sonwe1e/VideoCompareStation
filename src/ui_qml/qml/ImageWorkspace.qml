pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
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
    signal swapSidesRequested
    signal replacePrimaryRequested
    signal replaceSecondaryRequested

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
    property var hoverPoint: null

    onHasPairChanged: {
        if (!hasPair) {
            singleViewShowSecondary = false;
            hoverPoint = null;
        }
    }
    onCompareModeChanged: {
        if (compareMode !== 0)
            singleViewShowSecondary = false;
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
        if (mode === 0 && compareMode !== 0)
            singleViewShowSecondary = false;
        imageReview.compareMode = mode;
    }

    // View selection mirrors the old chip toggle: picking the active view returns to RGBA,
    // so Alpha Gray (A) and RGB Opaque (O) keep acting as toggles from the dropdown too.
    function setViewMode(mode) {
        if (!imageReview)
            return;
        imageReview.viewMode = (imageReview.viewMode === mode && mode !== 0) ? 0 : mode;
    }

    readonly property string viewModeLabel: viewMode === 1 ? qsTr("Alpha 灰度") : (viewMode === 2 ? qsTr("RGB 忽略") : qsTr("RGBA"))
    readonly property string backgroundModeLabel: backgroundMode === 1 ? qsTr("背景·棋盘格") : (backgroundMode === 2 ? qsTr("背景·黑底") : (backgroundMode === 3 ? qsTr("背景·白底") : qsTr("背景·深色")))
    readonly property string diffModeLabel: compareMode === 2 ? qsTr("差异·绝对差异") : (compareMode === 3 ? qsTr("差异·带符号") : (compareMode === 4 ? qsTr("差异·高亮") : (compareMode === 6 ? qsTr("差异·Alpha") : qsTr("差异·选择"))))

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
    // The suffix names the path the samples travelled: a display-converted source is watched
    // through the RGBA8 conversion, so its code values are a viewing path, not a numeric review
    // of the file's own planes.
    function sourceSummary(bitDepth, format, hasAlpha, displayConverted) {
        let text = "";
        if (bitDepth > 8)
            text = qsTr("%1-bit").arg(bitDepth) + (format ? " " + format : "");
        else
            text = format && format.length > 0 && format !== "rgba" ? format : "RGBA8";
        if (hasAlpha)
            text += " · α";
        if (displayConverted)
            text += " · " + qsTr("观看路径：显示转换（非原始平面）");
        else
            text += " · " + qsTr("数值审查路径：原码值");
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
        property bool dragActive: false
        property point pressStart: Qt.point(0, 0)
        property bool marqueeActive: false
        property bool suppressClickAfterMarquee: false
        property point marqueeStart: Qt.point(0, 0)
        property point marqueeCurrent: Qt.point(0, 0)

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

        Rectangle {
            id: marqueeRect
            visible: viewport.marqueeActive
            z: 25
            x: Math.min(viewport.marqueeStart.x, viewport.marqueeCurrent.x)
            y: Math.min(viewport.marqueeStart.y, viewport.marqueeCurrent.y)
            width: Math.abs(viewport.marqueeCurrent.x - viewport.marqueeStart.x)
            height: Math.abs(viewport.marqueeCurrent.y - viewport.marqueeStart.y)
            color: "#224b8df8"
            border.color: Theme.accent
            border.width: 1
        }

        MouseArea {
            id: viewportMouseArea
            objectName: "imageCanvasMouseArea-" + viewport.slot
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton
            onClicked: mouse => {
                if (viewport.suppressClickAfterMarquee) {
                    viewport.suppressClickAfterMarquee = false;
                    return;
                }
                if (mouse.button === Qt.LeftButton && control.compareMode === 0 && control.hasPair && !viewport.dragActive && !viewport.marqueeActive)
                    control.toggleSinglePairSource();
            }
            onPositionChanged: mouse => {
                if (viewport.marqueeActive) {
                    viewport.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                } else if (pressed && control.imageReview) {
                    if (!viewport.dragActive && Math.hypot(mouse.x - viewport.pressStart.x, mouse.y - viewport.pressStart.y) >= 5)
                        viewport.dragActive = true;
                    if (viewport.dragActive) {
                        const dx = (mouse.x - viewport.dragStart.x) / Math.max(1, width);
                        const dy = (mouse.y - viewport.dragStart.y) / Math.max(1, height);
                        control.imageReview.panBy(dx, dy);
                        viewport.dragStart = Qt.point(mouse.x, mouse.y);
                    }
                }
                control.applyHover(viewport, mouse.x, mouse.y, viewport.slot);
            }
            onExited: {
                if (control.imageReview)
                    control.imageReview.clearCursorPixel();
                control.hoverPoint = null;
            }
            onPressed: mouse => {
                if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier)) {
                    viewport.marqueeActive = true;
                    viewport.marqueeStart = Qt.point(mouse.x, mouse.y);
                    viewport.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                    viewport.dragActive = false;
                } else {
                    viewport.marqueeActive = false;
                    viewport.dragStart = Qt.point(mouse.x, mouse.y);
                    viewport.pressStart = Qt.point(mouse.x, mouse.y);
                    viewport.dragActive = false;
                }
                control.forceActiveFocus();
            }
            onReleased: mouse => {
                if (viewport.marqueeActive) {
                    viewport.marqueeActive = false;
                    viewport.suppressClickAfterMarquee = true;
                    const left = Math.min(viewport.marqueeStart.x, viewport.marqueeCurrent.x);
                    const right = Math.max(viewport.marqueeStart.x, viewport.marqueeCurrent.x);
                    const top = Math.min(viewport.marqueeStart.y, viewport.marqueeCurrent.y);
                    const bottom = Math.max(viewport.marqueeStart.y, viewport.marqueeCurrent.y);
                    if (Math.abs(right - left) >= 8 && Math.abs(bottom - top) >= 8 && previewImage.sourceSize.width > 0 && previewImage.sourceSize.height > 0) {
                        const m0 = control.mapToImage(viewport, left, top);
                        const m1 = control.mapToImage(viewport, right, bottom);
                        if (m0 && m1) {
                            const sw = previewImage.sourceSize.width;
                            const sh = previewImage.sourceSize.height;
                            const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                            const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                            const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                            const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                            if (control.imageReview)
                                control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                        }
                    }
                }
            }
            onDoubleClicked: mouse => {
                // Qt sends a click before the double-click event. Keep the selected A/B
                // source unchanged when the user invokes the zoom gesture.
                if (mouse.button === Qt.LeftButton && control.compareMode === 0 && control.hasPair)
                    control.toggleSinglePairSource();
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
        spacing: 8

        // Row A — file and view commands. Opening variants live behind one dropdown so the
        // top edge never grows past a single row; sidebar toggling only appears with folders.
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
                objectName: "imageOpenPairButton"
                text: qsTr("打开图片对… ▾")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                onClicked: openPairMenu.open()

                VcsMenu {
                    id: openPairMenu
                    menuWidth: 220

                    VcsMenuItem {
                        objectName: "imageOpenPairMenuItem"
                        text: qsTr("打开图片对…")
                        onTriggered: control.openPairRequested()
                    }
                    VcsMenuItem {
                        objectName: "imageCompareFoldersMenuItem"
                        text: qsTr("对比文件夹…")
                        onTriggered: control.compareFoldersRequested()
                    }
                    VcsMenuSeparator {}
                    VcsMenuItem {
                        objectName: "imageAddMenuItem"
                        text: qsTr("添加图片…")
                        enabled: control.hasPrimary && !control.hasSecondary
                        onTriggered: control.addImageRequested()
                    }
                    VcsMenuSeparator {
                        visible: control.hasPrimary
                    }
                    VcsMenuItem {
                        objectName: "imageSwapSidesMenuItem"
                        text: qsTr("对调 A / B")
                        enabled: control.hasPair
                        onTriggered: control.swapSidesRequested()
                    }
                    VcsMenuItem {
                        objectName: "imageReplacePrimaryMenuItem"
                        text: qsTr("替换 A…")
                        enabled: control.hasPrimary
                        onTriggered: control.replacePrimaryRequested()
                    }
                    VcsMenuItem {
                        objectName: "imageReplaceSecondaryMenuItem"
                        text: qsTr("替换 B…")
                        enabled: control.hasPrimary
                        onTriggered: control.replaceSecondaryRequested()
                    }
                }
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
                objectName: "imageToggleSidebarButton"
                checkable: true
                checked: control.sidebarVisible
                text: control.sidebarVisible ? qsTr("隐藏列表") : qsTr("显示列表")
                implicitHeight: 30
                leftPadding: 12
                rightPadding: 12
                visible: control.hasFolders
                onClicked: control.toggleSidebarRequested()
            }

            Rectangle {
                width: 1
                height: 22
                color: Theme.border
                anchors.verticalCenter: parent.verticalCenter
            }

            ReviewActionButton {
                objectName: "imageFitButton"
                checkable: true
                checked: !control.trueSize
                text: qsTr("适应窗口")
                implicitHeight: 30
                leftPadding: 10
                rightPadding: 10
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
                text: qsTr("100%")
                implicitHeight: 30
                leftPadding: 10
                rightPadding: 10
                enabled: control.hasPrimary
                onClicked: {
                    control.trueSize = true;
                    if (control.imageReview)
                        control.imageReview.setZoom(1.0);
                }
            }
            ReviewActionButton {
                objectName: "imageResetViewButton"
                text: qsTr("重置视图")
                implicitHeight: 30
                leftPadding: 10
                rightPadding: 10
                enabled: control.hasPrimary
                onClicked: {
                    control.trueSize = false;
                    if (control.imageReview)
                        control.imageReview.resetView();
                }
            }
        }

        // Row B — comparison layout, difference analysis and observation. Mutually exclusive
        // modes stay grouped; secondary modes collapse into "more" menus like the video bar,
        // so a pair never shows the full control surface at once.
        Row {
            spacing: 8

            ModeChip {
                objectName: "imageModePrimary"
                text: control.hasPair ? qsTr("手动闪烁") : qsTr("查看")
                modeValue: 0
            }
            ModeChip {
                objectName: "imageModeSide"
                text: qsTr("并排")
                modeValue: 1
                visible: control.hasPair
            }
            ModeChip {
                objectName: "imageModeWipe"
                text: qsTr("分割线")
                modeValue: 5
                visible: control.hasPair
            }
            ModeChip {
                objectName: "imageModeFade"
                text: qsTr("淡化")
                modeValue: 7
                visible: control.hasPair
            }
            ReviewActionButton {
                objectName: "imageToggleSourceButton"
                visible: control.hasPair && control.compareMode === 0
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                text: control.compareMode === 0 && control.singleViewShowSecondary ? qsTr("切至 A") : qsTr("切至 B")
                onClicked: control.toggleSinglePairSource()
            }

            ReviewActionButton {
                objectName: "imageSwapSidesButton"
                visible: control.hasPair
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                text: qsTr("对调 A/B")
                helpText: qsTr("交换 A 与 B 的槽位方向。\n不改变文件夹配对身份；带符号差异、分割线与淡化会跟随新的 A/B 方向。")
                onClicked: control.swapSidesRequested()
            }

            ReviewActionButton {
                objectName: "imageReplaceSidesButton"
                text: qsTr("换图… ▾")
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                visible: control.hasPrimary
                enabled: control.hasPrimary
                helpText: qsTr("只替换其中一侧，另一侧保持不变。失败时原图保留。")
                onClicked: replaceSidesMenu.open()

                VcsMenu {
                    id: replaceSidesMenu
                    menuWidth: 180

                    VcsMenuItem {
                        objectName: "imageReplacePrimaryButton"
                        text: qsTr("替换 A…")
                        enabled: control.hasPrimary
                        onTriggered: control.replacePrimaryRequested()
                    }
                    VcsMenuItem {
                        objectName: "imageReplaceSecondaryButton"
                        text: control.hasSecondary ? qsTr("替换 B…") : qsTr("添加 B…")
                        enabled: control.hasPrimary
                        onTriggered: control.replaceSecondaryRequested()
                    }
                }
            }

            Rectangle {
                visible: control.hasPair
                width: 1
                height: 22
                color: Theme.border
                anchors.verticalCenter: parent.verticalCenter
            }

            ReviewActionButton {
                objectName: "imageDiffModeButton"
                text: control.diffModeLabel + " ▾"
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                visible: control.hasPair
                onClicked: diffMenu.open()

                VcsMenu {
                    id: diffMenu
                    menuWidth: 220

                    VcsRadioMenuItem {
                        objectName: "imageModeAbsDiff"
                        text: qsTr("绝对差异")
                        checked: control.compareMode === 2
                        onTriggered: control.modeButton(2)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageModeSignedDiff"
                        text: qsTr("带符号差异")
                        checked: control.compareMode === 3
                        onTriggered: control.modeButton(3)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageModeHighlight"
                        text: qsTr("高亮")
                        checked: control.compareMode === 4
                        onTriggered: control.modeButton(4)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageModeAlphaDiff"
                        text: qsTr("Alpha 差异")
                        enabled: Boolean(control.imageReview && (control.imageReview.primaryHasAlpha || control.imageReview.secondaryHasAlpha))
                        checked: control.compareMode === 6
                        onTriggered: control.modeButton(6)
                    }
                }
            }

            Rectangle {
                width: 1
                height: 22
                color: Theme.border
                anchors.verticalCenter: parent.verticalCenter
            }

            // Difference display gain. It amplifies only the rendered image; the peak/MAE
            // readouts stay raw 8-bit deltas, so two candidates compared at the same gain remain
            // visually comparable and the numbers never follow the display setting.
            ReviewActionButton {
                objectName: "imageDiffGainButton"
                text: qsTr("差异放大 ×%1 ▾").arg(control.imageReview ? control.imageReview.diffGain : 4)
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                visible: control.diffModeActive
                onClicked: diffGainMenu.open()

                VcsMenu {
                    id: diffGainMenu
                    menuWidth: 230

                    VcsRadioMenuItem {
                        objectName: "imageDiffGain1"
                        text: qsTr("×1 · 原始差值（不放大）")
                        checked: control.imageReview && control.imageReview.diffGain === 1
                        onTriggered: {
                            if (control.imageReview)
                                control.imageReview.diffGain = 1;
                        }
                    }
                    VcsRadioMenuItem {
                        objectName: "imageDiffGain2"
                        text: qsTr("×2")
                        checked: control.imageReview && control.imageReview.diffGain === 2
                        onTriggered: {
                            if (control.imageReview)
                                control.imageReview.diffGain = 2;
                        }
                    }
                    VcsRadioMenuItem {
                        objectName: "imageDiffGain4"
                        text: qsTr("×4（默认）")
                        checked: control.imageReview && control.imageReview.diffGain === 4
                        onTriggered: {
                            if (control.imageReview)
                                control.imageReview.diffGain = 4;
                        }
                    }
                    VcsRadioMenuItem {
                        objectName: "imageDiffGain8"
                        text: qsTr("×8")
                        checked: control.imageReview && control.imageReview.diffGain === 8
                        onTriggered: {
                            if (control.imageReview)
                                control.imageReview.diffGain = 8;
                        }
                    }
                    VcsRadioMenuItem {
                        objectName: "imageDiffGain16"
                        text: qsTr("×16")
                        checked: control.imageReview && control.imageReview.diffGain === 16
                        onTriggered: {
                            if (control.imageReview)
                                control.imageReview.diffGain = 16;
                        }
                    }
                }
            }

            ReviewActionButton {
                objectName: "imageViewModeButton"
                text: control.viewModeLabel + " ▾"
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                onClicked: viewMenu.open()

                VcsMenu {
                    id: viewMenu
                    menuWidth: 230

                    VcsRadioMenuItem {
                        objectName: "imageViewRgba"
                        text: qsTr("RGBA")
                        checked: control.viewMode === 0
                        onTriggered: control.setViewMode(0)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageViewAlphaGray"
                        text: qsTr("Alpha 灰度 (A)")
                        checked: control.viewMode === 1
                        onTriggered: control.setViewMode(1)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageViewRgbOpaque"
                        text: qsTr("RGB 忽略透明度 (O)")
                        checked: control.viewMode === 2
                        onTriggered: control.setViewMode(2)
                    }
                }
            }
            ReviewActionButton {
                objectName: "imageBackgroundButton"
                text: control.backgroundModeLabel + " ▾"
                implicitHeight: 30
                leftPadding: 8
                rightPadding: 8
                onClicked: bgMenu.open()

                VcsMenu {
                    id: bgMenu
                    menuWidth: 200

                    VcsRadioMenuItem {
                        objectName: "imageBgDark"
                        text: qsTr("深色")
                        checked: control.backgroundMode === 0
                        onTriggered: control.backgroundMode = 0
                    }
                    VcsRadioMenuItem {
                        objectName: "imageBgChecker"
                        text: qsTr("棋盘格")
                        checked: control.backgroundMode === 1
                        onTriggered: control.backgroundMode = 1
                    }
                    VcsRadioMenuItem {
                        objectName: "imageBgBlack"
                        text: qsTr("黑底")
                        checked: control.backgroundMode === 2
                        onTriggered: control.backgroundMode = 2
                    }
                    VcsRadioMenuItem {
                        objectName: "imageBgWhite"
                        text: qsTr("白底")
                        checked: control.backgroundMode === 3
                        onTriggered: control.backgroundMode = 3
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
    // Manual in-place comparison: each press switches the displayed source.
    Keys.onSpacePressed: event => {
        if (control.hasPair && control.compareMode === 0 && !event.isAutoRepeat) {
            control.toggleSinglePairSource();
            event.accepted = true;
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
        } else if (event.key === Qt.Key_T || event.key === Qt.Key_QuoteLeft) {
            if (control.hasPair && !event.isAutoRepeat) {
                control.toggleSinglePairSource();
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_A) {
            if (control.hasPrimary) {
                control.toggleAlphaView();
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

                // Manual A/B comparison badge stays inside the image stage.
                Rectangle {
                    id: imageInPlaceBadge
                    objectName: "imageInPlaceBadge"
                    visible: control.hasPair && control.compareMode === 0
                    z: 30
                    width: Math.min(stageContent.width - 28, inPlaceBadgeText.implicitWidth + 20)
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

                    Text {
                        id: inPlaceBadgeText
                        anchors.centerIn: parent
                        width: Math.max(0, parent.width - 20)
                        text: control.singleViewShowSecondary ? qsTr("当前 B · 点击画面或按空格切换") : qsTr("当前 A · 点击画面或按空格切换")
                        elide: Text.ElideRight
                        color: "#ffffff"
                        font.pixelSize: 12
                        font.weight: Font.DemiBold
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: control.toggleSinglePairSource()
                    }
                }

                // Alpha & Background observation HUD badge
                Rectangle {
                    id: imageAlphaObservationBadge
                    objectName: "imageAlphaObservationBadge"
                    visible: control.hasPrimary && (control.viewMode !== 0 || control.backgroundMode !== 0)
                    z: 30
                    width: Math.min(stageContent.width - (imageInPlaceBadge.visible ? imageInPlaceBadge.width + 52 : 28), alphaBadgeContent.implicitWidth + 20)
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
                        id: alphaBadgeContent
                        spacing: 8
                        anchors.centerIn: parent

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
                                return qsTr("点击还原深色背景");
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
                                control.backgroundMode = 0;
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
                            text = qsTr("%1 · 峰值 %2 · RGB MAE %3 · 统计 %4").arg(modeName).arg(control.imageReview.maxAbsDifference).arg(control.imageReview.meanAbsDifference.toFixed(2)).arg(control.imageReview.diffScopeText);
                        if (control.imageReview.diffResampled)
                            text += " · " + qsTr("已重采样对齐");
                        if (control.imageReview.alphaDifferenceOnly)
                            text += " · " + qsTr("RGB 相同，alpha 存在差异");
                        // Converted 16-bit statistics: the numbers that stay meaningful when the
                        // RGBA8 display buffer cannot show the difference. The peak uses the
                        // largest RGB channel delta and the MAE averages all three channel deltas,
                        // matching the video metric; the gain sentence lives in diffScopeText and
                        // reports the gain actually in use.
                        if (control.imageReview.nativeStatsText.length > 0)
                            text += " · " + control.imageReview.nativeStatsText;
                        if (control.imageReview.displayEqualButNativeDifferent)
                            text += " · " + qsTr("⚠️ 显示缓冲看不出该差异，只有读数能看出");
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
                    property bool marqueeActive: false
                    property point marqueeStart: Qt.point(0, 0)
                    property point marqueeCurrent: Qt.point(0, 0)

                    Rectangle {
                        id: wipeMarqueeRect
                        visible: wipeOverlay.marqueeActive
                        z: 25
                        x: Math.min(wipeOverlay.marqueeStart.x, wipeOverlay.marqueeCurrent.x)
                        y: Math.min(wipeOverlay.marqueeStart.y, wipeOverlay.marqueeCurrent.y)
                        width: Math.abs(wipeOverlay.marqueeCurrent.x - wipeOverlay.marqueeStart.x)
                        height: Math.abs(wipeOverlay.marqueeCurrent.y - wipeOverlay.marqueeStart.y)
                        color: "#224b8df8"
                        border.color: Theme.accent
                        border.width: 1
                    }

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
                            if (wipeOverlay.marqueeActive) {
                                wipeOverlay.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                            } else if (pressed && control.imageReview) {
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
                            if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier)) {
                                wipeOverlay.marqueeActive = true;
                                wipeOverlay.marqueeStart = Qt.point(mouse.x, mouse.y);
                                wipeOverlay.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                            } else {
                                wipeOverlay.marqueeActive = false;
                                dragStart = Qt.point(mouse.x, mouse.y);
                            }
                            control.forceActiveFocus();
                        }
                        onReleased: mouse => {
                            if (wipeOverlay.marqueeActive) {
                                wipeOverlay.marqueeActive = false;
                                const left = Math.min(wipeOverlay.marqueeStart.x, wipeOverlay.marqueeCurrent.x);
                                const right = Math.max(wipeOverlay.marqueeStart.x, wipeOverlay.marqueeCurrent.x);
                                const top = Math.min(wipeOverlay.marqueeStart.y, wipeOverlay.marqueeCurrent.y);
                                const bottom = Math.max(wipeOverlay.marqueeStart.y, wipeOverlay.marqueeCurrent.y);
                                if (Math.abs(right - left) >= 8 && Math.abs(bottom - top) >= 8 && wipePrimaryImage.sourceSize.width > 0 && wipePrimaryImage.sourceSize.height > 0) {
                                    const m0 = control.mapToImage({
                                        "image": wipePrimaryImage
                                    }, left, top);
                                    const m1 = control.mapToImage({
                                        "image": wipePrimaryImage
                                    }, right, bottom);
                                    if (m0 && m1) {
                                        const sw = wipePrimaryImage.sourceSize.width;
                                        const sh = wipePrimaryImage.sourceSize.height;
                                        const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                                        const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                                        const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                                        const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                                        if (control.imageReview)
                                            control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                                    }
                                }
                            }
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

                // Fade comparison (T-batch 2a): both images stacked in the primary's geometry
                // box; the secondary's opacity is the fade position. Pure presentation — the
                // decoded buffers and the difference pipeline are untouched, and zoom/pan/
                // true-size behave exactly like the wipe layout.
                Item {
                    id: fadeOverlay

                    objectName: "fadeOverlay"
                    visible: control.hasPair && control.compareMode === 7
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
                    property bool marqueeActive: false
                    property point marqueeStart: Qt.point(0, 0)
                    property point marqueeCurrent: Qt.point(0, 0)

                    Rectangle {
                        id: fadeMarqueeRect
                        visible: fadeOverlay.marqueeActive
                        z: 25
                        x: Math.min(fadeOverlay.marqueeStart.x, fadeOverlay.marqueeCurrent.x)
                        y: Math.min(fadeOverlay.marqueeStart.y, fadeOverlay.marqueeCurrent.y)
                        width: Math.abs(fadeOverlay.marqueeCurrent.x - fadeOverlay.marqueeStart.x)
                        height: Math.abs(fadeOverlay.marqueeCurrent.y - fadeOverlay.marqueeStart.y)
                        color: "#224b8df8"
                        border.color: Theme.accent
                        border.width: 1
                    }

                    Image {
                        id: fadePrimaryImage

                        objectName: "fadePrimaryImage"
                        x: fadeOverlay.drawX
                        y: fadeOverlay.drawY
                        width: fadeOverlay.drawWidth
                        height: fadeOverlay.drawHeight
                        source: control.imageReview && control.imageReview.contentGeneration >= 0 && fadeOverlay.visible ? control.imageReview.imageUrl(2) : ""
                        fillMode: Image.Stretch
                        asynchronous: false
                        cache: false
                        smooth: control.zoom <= 2
                    }

                    Image {
                        id: fadeSecondaryImage

                        objectName: "fadeSecondaryImage"
                        x: fadeOverlay.drawX
                        y: fadeOverlay.drawY
                        width: fadeOverlay.drawWidth
                        height: fadeOverlay.drawHeight
                        source: control.imageReview && control.imageReview.contentGeneration >= 0 && fadeOverlay.visible ? control.imageReview.imageUrl(3) : ""
                        // A non-square source over a square primary geometry would stretch;
                        // mirror the wipe layout, which also draws both sides in the primary's
                        // box (the resample toggle governs the diff modes instead).
                        fillMode: Image.Stretch
                        asynchronous: false
                        cache: false
                        smooth: control.zoom <= 2
                        opacity: control.imageReview ? control.imageReview.fadePosition : 0
                    }

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton | Qt.MiddleButton
                        property point dragStart: Qt.point(0, 0)
                        onPositionChanged: mouse => {
                            if (fadeOverlay.marqueeActive) {
                                fadeOverlay.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                            } else if (pressed && control.imageReview) {
                                const dx = (mouse.x - dragStart.x) / Math.max(1, width);
                                const dy = (mouse.y - dragStart.y) / Math.max(1, height);
                                control.imageReview.panBy(dx, dy);
                                dragStart = Qt.point(mouse.x, mouse.y);
                            }
                            // Hover readout samples the primary; the blended pixel value has
                            // no single-source meaning at intermediate positions.
                            control.applyHover({
                                "image": fadePrimaryImage
                            }, mouse.x, mouse.y, 2);
                        }
                        onExited: {
                            if (control.imageReview)
                                control.imageReview.clearCursorPixel();
                        }
                        onPressed: mouse => {
                            if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier)) {
                                fadeOverlay.marqueeActive = true;
                                fadeOverlay.marqueeStart = Qt.point(mouse.x, mouse.y);
                                fadeOverlay.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                            } else {
                                fadeOverlay.marqueeActive = false;
                                dragStart = Qt.point(mouse.x, mouse.y);
                            }
                            control.forceActiveFocus();
                        }
                        onReleased: mouse => {
                            if (fadeOverlay.marqueeActive) {
                                fadeOverlay.marqueeActive = false;
                                const left = Math.min(fadeOverlay.marqueeStart.x, fadeOverlay.marqueeCurrent.x);
                                const right = Math.max(fadeOverlay.marqueeStart.x, fadeOverlay.marqueeCurrent.x);
                                const top = Math.min(fadeOverlay.marqueeStart.y, fadeOverlay.marqueeCurrent.y);
                                const bottom = Math.max(fadeOverlay.marqueeStart.y, fadeOverlay.marqueeCurrent.y);
                                if (Math.abs(right - left) >= 8 && Math.abs(bottom - top) >= 8 && fadePrimaryImage.sourceSize.width > 0 && fadePrimaryImage.sourceSize.height > 0) {
                                    const m0 = control.mapToImage({
                                        "image": fadePrimaryImage
                                    }, left, top);
                                    const m1 = control.mapToImage({
                                        "image": fadePrimaryImage
                                    }, right, bottom);
                                    if (m0 && m1) {
                                        const sw = fadePrimaryImage.sourceSize.width;
                                        const sh = fadePrimaryImage.sourceSize.height;
                                        const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                                        const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                                        const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                                        const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                                        if (control.imageReview)
                                            control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                                    }
                                }
                            }
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
                            const mapped = control.mapToImage({
                                "image": fadePrimaryImage
                            }, wheel.x, wheel.y);
                            const ax = mapped ? Math.max(0, Math.min(1, mapped.x / Math.max(1, fadePrimaryImage.sourceSize.width))) : 0.5;
                            const ay = mapped ? Math.max(0, Math.min(1, mapped.y / Math.max(1, fadePrimaryImage.sourceSize.height))) : 0.5;
                            control.imageReview.zoomBy(wheel.angleDelta.y > 0 ? 1.25 : 0.8, ax, ay);
                            wheel.accepted = true;
                        }
                    }
                }
            }
        }
    }

    // Fade blend control floats over the viewport so it never occupies toolbar space;
    // 0 shows A, 1 shows B, and the readout names both sides so the direction cannot be
    // misread while dragging (semantics unchanged from the previous inline slider).
    Rectangle {
        id: fadeSliderOverlay

        visible: control.hasPair && control.compareMode === 7
        height: 40
        radius: 8
        color: "#d910141b"
        border.color: Theme.menuBorder
        anchors {
            right: stage.right
            bottom: stage.bottom
            margins: 14
        }
        z: 20

        Row {
            spacing: 10
            anchors.centerIn: parent

            Text {
                height: 30
                verticalAlignment: Text.AlignVCenter
                text: qsTr("淡化 · A %1% / B %2%").arg(Math.round((1 - (control.imageReview ? control.imageReview.fadePosition : 0.5)) * 100)).arg(Math.round((control.imageReview ? control.imageReview.fadePosition : 0.5) * 100))
                color: Theme.mutedText
                font.pixelSize: 12
            }
            Slider {
                id: imageFadeSlider

                objectName: "imageFadeSlider"
                width: 170
                height: 30
                from: 0
                to: 1
                value: control.imageReview ? control.imageReview.fadePosition : 0.5
                padding: 6
                onMoved: {
                    if (control.imageReview)
                        control.imageReview.fadePosition = value;
                }

                background: Rectangle {
                    x: imageFadeSlider.leftPadding + (imageFadeSlider.horizontal ? 0 : (imageFadeSlider.availableWidth - width) / 2)
                    y: imageFadeSlider.topPadding + (imageFadeSlider.horizontal ? (imageFadeSlider.availableHeight - height) / 2 : 0)
                    width: imageFadeSlider.horizontal ? imageFadeSlider.availableWidth : implicitWidth
                    height: imageFadeSlider.horizontal ? implicitHeight : imageFadeSlider.availableHeight
                    implicitWidth: 200
                    implicitHeight: 6
                    radius: 3
                    color: Theme.menuBorder

                    Rectangle {
                        y: imageFadeSlider.horizontal ? 0 : imageFadeSlider.visualPosition * parent.height
                        width: imageFadeSlider.horizontal ? imageFadeSlider.position * parent.width : 6
                        height: imageFadeSlider.horizontal ? 6 : imageFadeSlider.position * parent.height
                        radius: 3
                        color: Theme.accent
                    }
                }

                handle: Rectangle {
                    x: imageFadeSlider.leftPadding + imageFadeSlider.visualPosition * (imageFadeSlider.availableWidth - width)
                    y: imageFadeSlider.topPadding + imageFadeSlider.availableHeight / 2 - height / 2
                    implicitWidth: 14
                    implicitHeight: 14
                    radius: 7
                    color: imageFadeSlider.pressed ? Theme.accent : "#d8e2f2"
                    border.color: Theme.border
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

        RowLayout {
            id: statusDetails
            objectName: "imageStatusDetails"
            spacing: 12
            anchors {
                left: parent.left
                leftMargin: 14
                right: statusHints.left
                rightMargin: 12
                verticalCenter: parent.verticalCenter
            }

            Text {
                text: qsTr("缩放 %1%（%2）").arg(control.displayPercent).arg(control.displayPercentMode)
                color: Theme.primaryText
                font.pixelSize: 12
                Layout.maximumWidth: implicitWidth
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
                elide: Text.ElideRight
                Layout.maximumWidth: Math.min(implicitWidth, 160)
            }
            Text {
                id: pixelReadout
                objectName: "imagePixelReadout"
                visible: Boolean(control.cursorPixel && control.cursorPixel.valid)
                text: {
                    if (!control.cursorPixel || !control.cursorPixel.valid)
                        return "";
                    let t = "";
                    if (control.imageReview && (control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted))
                        t = qsTr("像素 (%1, %2) [显示 RGBA8: R %3 G %4 B %5 A %6 %7 α %8%]").arg(control.cursorPixel.x).arg(control.cursorPixel.y).arg(control.cursorPixel.r).arg(control.cursorPixel.g).arg(control.cursorPixel.b).arg(control.cursorPixel.a).arg(control.cursorPixel.hex).arg(control.cursorPixel.alphaPercent);
                    else
                        t = qsTr("像素 (%1, %2)  R %3  G %4  B %5  A %6  %7  α %8%").arg(control.cursorPixel.x).arg(control.cursorPixel.y).arg(control.cursorPixel.r).arg(control.cursorPixel.g).arg(control.cursorPixel.b).arg(control.cursorPixel.a).arg(control.cursorPixel.hex).arg(control.cursorPixel.alphaPercent);
                    if (control.cursorPixel.channelView === "alphaGray")
                        t += " · " + qsTr("Alpha 灰度");
                    else if (control.cursorPixel.channelView === "rgbOpaque")
                        t += " · " + qsTr("RGB 忽略透明度");
                    // I-02: a >8-bit source also reports its original-depth code values so the
                    // readout never implies the 8-bit display numbers are the file's codes.
                    if (control.cursorPixel.nativeBitDepth !== undefined && control.cursorPixel.nativeBitDepth > 8)
                        t += " · " + qsTr("源 %1-bit → 转换后 16-bit：A %2（%3%） R %4 G %5 B %6").arg(control.cursorPixel.nativeBitDepth).arg(control.cursorPixel.a16).arg((Number(control.cursorPixel.a16) / 65535 * 100).toFixed(4)).arg(control.cursorPixel.r16).arg(control.cursorPixel.g16).arg(control.cursorPixel.b16);
                    return t;
                }
                color: Theme.primaryText
                font.pixelSize: 12
                font.family: "Consolas"
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                HoverHandler {
                    id: pixelReadoutHover
                }
                ToolTip.visible: pixelReadoutHover.hovered && pixelReadout.truncated
                ToolTip.text: text
            }
            Text {
                visible: control.errorText.length > 0
                text: control.errorText
                color: Theme.error
                font.pixelSize: 12
                elide: Text.ElideRight
                Layout.fillWidth: true
                Layout.minimumWidth: 0
            }
        }

        Row {
            id: statusHints
            objectName: "imageStatusHints"
            anchors {
                right: parent.right
                rightMargin: 14
                verticalCenter: parent.verticalCenter
            }
            spacing: 12

            Rectangle {
                id: displayConversionBadge
                objectName: "displayConversionBadge"
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted))
                height: 22
                radius: 4
                color: "#2a1e12"
                border.color: "#f59e0b"
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Row {
                    anchors.centerIn: parent
                    leftPadding: 6
                    rightPadding: 6
                    spacing: 4

                    Text {
                        text: "🎨"
                        font.pixelSize: 11
                    }
                    Text {
                        text: qsTr("显示缓冲 RGBA8")
                        color: "#fef3c7"
                        font.pixelSize: 11
                    }
                }

                HoverHandler {
                    id: displayConversionHover
                }

                VcsToolTip {
                    visible: displayConversionHover.hovered
                    text: qsTr("像素格式/位深已转换到 RGBA8 显示缓冲，不是文件平面原始码值。\n未应用 ICC/颜色配置文件；YUV→RGB 使用解码器默认系数，颜色外观不作色彩管理承诺。\n高位深源请参考像素状态栏的转换后 16-bit 码值。")
                }
            }

            Rectangle {
                id: resampleBadge
                objectName: "imageResampleBadge"
                visible: Boolean(control.imageReview && control.hasPair && (control.imageReview.diffResampled || (control.sizesDiffer && control.imageReview.resampleAllowed)))
                height: 22
                radius: 4
                color: "#2a1e12"
                border.color: "#f59e0b"
                border.width: 1
                anchors.verticalCenter: parent.verticalCenter

                Row {
                    anchors.centerIn: parent
                    leftPadding: 6
                    rightPadding: 6
                    spacing: 4

                    Text {
                        text: "📐"
                        font.pixelSize: 11
                    }
                    Text {
                        text: control.imageReview && control.imageReview.diffResampled ? qsTr("空间已重采样") : qsTr("重采样：开")
                        color: "#fef3c7"
                        font.pixelSize: 11
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (control.imageReview)
                            control.imageReview.resampleAllowed = !control.imageReview.resampleAllowed;
                    }
                }

                HoverHandler {
                    id: resampleBadgeHover
                }

                VcsToolTip {
                    visible: resampleBadgeHover.hovered
                    text: control.imageReview && control.imageReview.diffResampled ? qsTr("对比两侧图片尺寸不同，差异计算已对次图像进行双线性空间重采样。\n点击关闭重采样。") : qsTr("已开启重采样对齐（尺寸不同时将 B 缩放到 A 再算差异）。\n点击关闭。")
                }
            }

            Rectangle {
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryHasAlpha || control.imageReview.secondaryHasAlpha))
                height: 22
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

            Text {
                visible: statusBar.width >= 1120
                text: qsTr("滚轮缩放 · 拖动平移 · Shift+拖动框选放大")
                color: Theme.mutedText
                font.pixelSize: 11
            }
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
