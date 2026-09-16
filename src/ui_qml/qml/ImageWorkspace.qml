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
    readonly property bool diffModeActive: Boolean(imageReview && imageReview.hasPair && imageReview.compareMode >= 2 && imageReview.compareMode <= 4)

    // Physical-percent of the active display scale: true-size is 1 image px per physical
    // px (100%), fit shows the actual physical percentage of the fitted image.
    readonly property real displayPercent: {
        if (!imageReview || !imageReview.hasPrimary)
            return 0;
        const dpr = Window.window ? Window.window.devicePixelRatio : 1;
        const base = control.trueSize ? 1 / dpr : (primaryViewport ? primaryViewport.fitScale : 1);
        return Math.round(base * dpr * imageReview.zoom * 100);
    }
    readonly property string displayPercentMode: control.trueSize ? qsTr("真实尺寸") : qsTr("适应窗口")

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
            return;
        }
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped) {
            imageReview.clearCursorPixel();
            return;
        }
        imageReview.updateCursorPixel(slot, mapped.x, mapped.y);
    }

    component ModeChip: ReviewActionButton {
        id: chip

        required property int modeValue
        checkable: true
        checked: control.compareMode === modeValue
        implicitHeight: 30
        implicitWidth: 108
        leftPadding: 10
        rightPadding: 10
        enabled: control.hasPrimary && (modeValue === 0 || modeValue === 1 || control.hasPair)
        onClicked: control.modeButton(modeValue)

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

        MouseArea {
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton
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
            }
            onPressed: mouse => {
                viewport.dragStart = Qt.point(mouse.x, mouse.y);
                control.forceActiveFocus();
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
                onClicked: control.imageReview.resetView()
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
                onClicked: control.trueSize = false
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
                onClicked: control.trueSize = true
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
                text: qsTr("查看")
                modeValue: 0
            }
            ModeChip {
                objectName: "imageModeSide"
                text: qsTr("并排")
                modeValue: 1
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
                objectName: "imageModeWipe"
                text: qsTr("分割线")
                modeValue: 5
            }
        }
    }

    // Left/Right walk folder pairs when a comparison list is loaded; otherwise they are
    // unused in this workspace (no frame stepping) and stay free for the shell.
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

                Row {
                    visible: control.hasPrimary && (control.compareMode === 0 || control.compareMode === 1 || !control.hasPair)
                    anchors.fill: parent
                    spacing: 12

                    ImageViewport {
                        id: primaryViewport

                        objectName: "primaryViewport"
                        slot: 2
                        width: control.compareMode === 1 && control.hasPair ? parent.width / 2 - 6 : parent.width
                        height: parent.height
                        imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(2) : ""
                        label: control.hasPrimary ? qsTr("A · %1×%2").arg(control.imageReview.primaryWidth).arg(control.imageReview.primaryHeight) : ""
                        title: control.hasPrimary ? control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath) : ""
                        titlePath: control.hasPrimary ? String(control.imageReview.primaryPath || "") : ""
                    }

                    ImageViewport {
                        objectName: "secondaryViewport"
                        slot: 3
                        visible: control.compareMode === 1 && control.hasPair
                        width: visible ? parent.width / 2 - 6 : 0
                        height: parent.height
                        imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(3) : ""
                        label: control.hasSecondary ? qsTr("B · %1×%2").arg(control.imageReview.secondaryWidth).arg(control.imageReview.secondaryHeight) : ""
                        title: control.hasSecondary ? control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath) : ""
                        titlePath: control.hasSecondary ? String(control.imageReview.secondaryPath || "") : ""
                    }
                }

                ImageViewport {
                    objectName: "diffViewport"
                    slot: 4
                    visible: control.hasPair && control.compareMode >= 2 && control.compareMode <= 4 && control.imageReview && control.imageReview.hasDiffResult
                    anchors.fill: parent
                    imageUrl: control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(4) : ""
                    label: {
                        if (!control.imageReview || !control.imageReview.hasDiffResult)
                            return "";
                        const modeName = control.compareMode === 2 ? qsTr("绝对差异") : (control.compareMode === 3 ? qsTr("带符号差异") : qsTr("高亮"));
                        let text = qsTr("%1 · 峰值 %2 · 均值 %3 · 统计 %4").arg(modeName).arg(control.imageReview.maxAbsDifference).arg(control.imageReview.meanAbsDifference.toFixed(2)).arg(control.imageReview.diffScopeText);
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
                Text {
                    objectName: "imageDiffUnavailableNotice"
                    visible: control.diffModeActive && control.imageReview && !control.imageReview.hasDiffResult
                    anchors.centerIn: parent
                    width: parent.width * 0.7
                    horizontalAlignment: Text.AlignHCenter
                    wrapMode: Text.WordWrap
                    text: qsTr("A 与 B 尺寸不同，未计算逐像素差异。可开启“重采样对齐差异”后再比较。")
                    color: Theme.warning
                    font.pixelSize: 13
                }

                // Split-line comparison: the secondary image is clipped at the split position and stacked
                // over the primary, both drawn in the primary's geometry box so zoom/pan stay aligned.
                Item {
                    id: wipeOverlay

                    objectName: "wipeOverlay"
                    visible: control.hasPair && control.compareMode === 5
                    anchors.fill: parent
                    clip: true

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

                    MouseArea {
                        anchors.fill: parent
                        hoverEnabled: true
                        acceptedButtons: Qt.LeftButton
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
            Text {
                visible: Boolean(control.cursorPixel && control.cursorPixel.valid)
                text: control.cursorPixel && control.cursorPixel.valid ? qsTr("像素 (%1, %2)  R %3  G %4  B %5  A %6  %7").arg(control.cursorPixel.x).arg(control.cursorPixel.y).arg(control.cursorPixel.r).arg(control.cursorPixel.g).arg(control.cursorPixel.b).arg(control.cursorPixel.a).arg(control.cursorPixel.hex) : ""
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
}
