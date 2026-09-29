pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import "VcsTheme.js" as Theme
import "MediaLabels.js" as MediaLabels

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

    // Distance from the workspace's bottom edge to the top of the stage's bottom controls;
    // Main floats its notices above this line so they never cover the stage chrome.
    readonly property real bottomControlsInset: fadeSliderOverlay.visible ? control.height - fadeSliderOverlay.y : control.height - (stage.y + stage.height)

    objectName: "imageWorkspace"
    color: Theme.stageWell
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
    // The neutral checkerboard is the default so the content area never tints color judgment
    // of semi-transparent pixels (the dark option keeps its blue-leaning #090d14 as an
    // explicit choice; the app chrome keeps the dark theme).
    property int backgroundMode: 1
    property bool singleViewShowSecondary: false
    property var hoverPoint: null
    // Step-3 image editing: optional controller (absent in isolated harnesses). The session
    // always starts from the committed original, and the workspace only *displays* the
    // working copy — comparison and difference statistics keep using the originals.
    property var imageEdit: null
    readonly property bool editModeActive: Boolean(imageEdit && imageEdit.active)
    readonly property int editSourceSlot: editModeActive ? Number(imageEdit.sourceSlot) : -1
    // Selection in image pixels (null while nothing is selected); shared by crop and mosaic.
    property var cropSelection: null
    // Active editing tool: "crop", "brush", "mosaic", "fill", "clear", "rect", "arrow",
    // "text" or "select". Geometry always travels in image pixels, so switching tool or
    // zooming never drifts a selection, a stroke or an annotation.
    property string editTool: "crop"
    property color brushColor: "#ff3b30"
    property int brushWidth: 4
    property real brushOpacity: 1.0
    property int mosaicBlock: 8
    // Text annotations: typed content and pixel size (the width slider doubles as 字号).
    property string annotationText: ""
    property int annotationTextSize: 28

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

    // A lazily instantiated workspace (video-first sessions build this tree only when the
    // image workspace activates) creates its Flows inside an already-sized parent chain:
    // each Flow lays out once at its width-0 creation state, and the re-layout Qt performs
    // on width changes is frame-driven — it never happens in a window that is not exposed
    // yet. Force the rows to lay out synchronously so the panel never keeps a stale
    // vertical layout: once here, where the widths settle during creation, and on every
    // later width change (chrome margins, window resizes) through each row's handler.
    Component.onCompleted: {
        fileRow.forceLayout();
        modeRow.forceLayout();
        editRow.forceLayout();
        editParamsRow.forceLayout();
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
        return MediaLabels.fileName(pathValue);
    }

    // Parent-folder part of a controller path, used to disambiguate same-named images.
    function imageParentLabel(pathValue) {
        return MediaLabels.parentFolderLabel(pathValue);
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

    // Shared viewport geometry: the slot viewports and the wipe/fade overlays all compute the
    // fit scale, the true-size base and the pan-centered draw origin with the same formulas,
    // so they live here once instead of drifting apart per copy.
    function imageFitScale(containerWidth, containerHeight, imageWidth, imageHeight) {
        const sourceWidth = Math.max(1, imageWidth);
        const sourceHeight = Math.max(1, imageHeight);
        return Math.min(containerWidth / sourceWidth, containerHeight / sourceHeight);
    }

    // True-size: 1 image pixel occupies 1 physical pixel, so the device-independent scale is
    // 1 / DPR (T2).
    function imageTrueSizeScale() {
        const dpr = Window.window ? Window.window.devicePixelRatio : 1;
        return 1 / dpr;
    }

    // Position of one edge of the drawn image rect: centered, then shifted by the pan offset
    // (0.5 = centered).
    function imageDrawEdge(containerSize, drawSize, pan) {
        return (containerSize - drawSize) / 2 + (0.5 - pan) * drawSize;
    }

    // The drag-selection rectangle, shared by the slot viewports and the wipe/fade overlays.
    component ImageMarqueeRect: Rectangle {
        required property bool active
        required property point start
        required property point current

        z: 25
        visible: active
        x: Math.min(start.x, current.x)
        y: Math.min(start.y, current.y)
        width: Math.abs(current.x - start.x)
        height: Math.abs(current.y - start.y)
        color: Theme.selectionFill
        border.color: Theme.accent
        border.width: 1
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

    // ---- Step-3 image editing -------------------------------------------------------
    // The session always starts from the committed original of the side on screen; the
    // original buffer and the source file are never written to, and edits are saved as a
    // copy. Crop geometry is captured in image pixels (mapToImage), so zoom or pan cannot
    // drift the selection.

    function editDisplayedSlot() {
        return (compareMode === 0 && hasPair && singleViewShowSecondary) ? 3 : 2;
    }

    // Label of the tool that is actually selected, so a tool living inside the 「更多工具」
    // dropdown is still readable from the closed row.
    function imageEditToolLabel(tool) {
        switch (String(tool || "")) {
        case "crop":
            return qsTr("裁剪");
        case "brush":
            return qsTr("画笔");
        case "select":
            return qsTr("选择");
        case "mosaic":
            return qsTr("马赛克");
        case "fill":
            return qsTr("填充");
        case "clear":
            return qsTr("清除");
        case "rect":
            return qsTr("矩形");
        case "arrow":
            return qsTr("箭头");
        case "text":
            return qsTr("文字");
        default:
            return qsTr("更多工具");
        }
    }

    // Both dialogs edit the same working copy through the same controller, so the result of
    // each is reported in the shared status line rather than in the dialog, which closes on
    // apply.
    function applyImageScale(width, height, smooth) {
        if (!imageEdit)
            return false;
        const applied = Boolean(imageEdit.resizeImage(width, height, smooth));
        if (applied && imageReview)
            imageReview.resetView();
        return applied;
    }

    function applyImageCanvasFill(width, height, fillColor) {
        if (!imageEdit)
            return false;
        const applied = Boolean(imageEdit.padToCanvas(width, height, fillColor));
        if (applied && imageReview)
            imageReview.resetView();
        return applied;
    }

    function beginImageEdit() {
        if (!imageEdit)
            return false;
        const slot = control.editDisplayedSlot();
        const path = slot === 3 ? control.imageReview.secondaryPath : control.imageReview.primaryPath;
        control.cropSelection = null;
        return Boolean(imageEdit.beginSession(slot, String(path || ""), String(path || "")));
    }

    function finishImageEdit() {
        control.cropSelection = null;
        if (imageEdit)
            imageEdit.endSession();
    }

    function applyCropSelection() {
        if (!imageEdit || !imageEdit.active || !control.cropSelection)
            return false;
        const selection = control.cropSelection;
        let applied = false;
        if (control.editTool === "mosaic") {
            applied = Boolean(imageEdit.mosaicImageRect(selection.x, selection.y, selection.width, selection.height, control.mosaicBlock));
        } else {
            applied = Boolean(imageEdit.cropToImageRect(selection.x, selection.y, selection.width, selection.height));
        }
        if (applied) {
            control.cropSelection = null;
            // Only a crop changes the geometry; a mosaic leaves the view where it was.
            if (control.editTool !== "mosaic" && control.imageReview)
                control.imageReview.resetView();
        }
        return applied;
    }

    // Brush: the stroke is painted live into the working copy, but the whole gesture is
    // committed as one history step on release. Points are mapped to image pixels here, so
    // the controller never sees viewport coordinates.
    function beginBrushStroke(view, mouseX, mouseY) {
        if (!imageEdit || !imageEdit.active)
            return;
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped)
            return;
        if (!imageEdit.beginStroke(control.brushColor, control.brushWidth, control.brushOpacity))
            return;
        imageEdit.strokeTo(Math.round(mapped.x), Math.round(mapped.y));
    }

    function continueBrushStroke(view, mouseX, mouseY) {
        if (!imageEdit || !imageEdit.strokeActive)
            return;
        const mapped = mapToImage(view, mouseX, mouseY);
        if (mapped)
            imageEdit.strokeTo(Math.round(mapped.x), Math.round(mapped.y));
    }

    function endBrushStroke() {
        if (imageEdit && imageEdit.strokeActive)
            imageEdit.endStroke();
    }

    // One-shot rect tools apply on release (undo covers a mistake); crop and mosaic keep the
    // selection until the apply button, because their parameters are worth adjusting first.
    function applyRectTool() {
        if (!imageEdit || !imageEdit.active || !control.cropSelection)
            return false;
        const selection = control.cropSelection;
        const x = selection.x;
        const y = selection.y;
        const right = x + selection.width;
        const bottom = y + selection.height;
        let handled = true;
        switch (control.editTool) {
        case "fill":
            imageEdit.fillImageRect(x, y, selection.width, selection.height, control.brushColor);
            break;
        case "clear":
            imageEdit.clearImageRect(x, y, selection.width, selection.height);
            break;
        case "rect":
            imageEdit.addRectangle(x, y, right, bottom, control.brushColor, control.brushWidth);
            break;
        case "arrow":
            // Arrows keep the drawn direction, so they use the raw endpoints.
            imageEdit.addArrow(selection.fromX, selection.fromY, selection.toX, selection.toY, control.brushColor, control.brushWidth);
            break;
        default:
            handled = false;
            break;
        }
        if (handled)
            control.cropSelection = null;
        return handled;
    }

    function placeTextAt(view, mouseX, mouseY) {
        if (!imageEdit || !imageEdit.active)
            return false;
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped)
            return false;
        return Boolean(imageEdit.addText(Math.round(mapped.x), Math.round(mapped.y), control.annotationText, control.brushColor, control.annotationTextSize));
    }

    function beginAnnotationDrag(view, mouseX, mouseY) {
        if (!imageEdit || !imageEdit.active)
            return false;
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped)
            return false;
        return Boolean(imageEdit.beginAnnotationDrag(Math.round(mapped.x), Math.round(mapped.y)));
    }

    function continueAnnotationDrag(view, mouseX, mouseY) {
        if (!imageEdit || !imageEdit.active)
            return;
        const mapped = mapToImage(view, mouseX, mouseY);
        if (mapped)
            imageEdit.dragAnnotationTo(Math.round(mapped.x), Math.round(mapped.y));
    }

    function endAnnotationDrag() {
        if (imageEdit && imageEdit.active)
            imageEdit.endAnnotationDrag();
    }

    // Converts a drag in one viewport into an image-pixel selection. Rect tools need a real
    // box; an arrow only needs a direction, so a purely horizontal or vertical arrow is
    // valid and the raw drag endpoints are kept for it (normalising would lose the
    // direction the user drew).
    function updateCropSelection(view) {
        if (!imageEdit || !imageEdit.active)
            return;
        const m0 = mapToImage(view, view.cropStart.x, view.cropStart.y);
        const m1 = mapToImage(view, view.cropCurrent.x, view.cropCurrent.y);
        if (!m0 || !m1)
            return;
        const deltaX = m1.x - m0.x;
        const deltaY = m1.y - m0.y;
        const isArrow = control.editTool === "arrow";
        const tooSmall = isArrow ? Math.hypot(deltaX, deltaY) < 3 : (Math.abs(deltaX) < 2 || Math.abs(deltaY) < 2);
        if (tooSmall) {
            control.cropSelection = null;
            return;
        }
        control.cropSelection = {
            "fromX": Math.round(m0.x),
            "fromY": Math.round(m0.y),
            "toX": Math.round(m1.x),
            "toY": Math.round(m1.y),
            "x": Math.max(0, Math.round(Math.min(m0.x, m1.x))),
            "y": Math.max(0, Math.round(Math.min(m0.y, m1.y))),
            "width": Math.max(1, Math.round(Math.abs(deltaX))),
            "height": Math.max(1, Math.round(Math.abs(deltaY)))
        };
    }

    // Deterministic checkerboard behind transparent regions (cell scaled to the viewport),
    // so partial alpha reads as a blend against known colors instead of the dark void. Both
    // tones are true neutral grays at the previous lightness levels; the old #22262e/#383e4a
    // pair leaned blue and could tint color judgment.
    component CheckerboardBackground: Canvas {
        onWidthChanged: requestPaint()
        onHeightChanged: requestPaint()
        onPaint: {
            const ctx = getContext("2d");
            if (!ctx)
                return;
            const cell = Math.max(8, Math.min(16, Math.round(Math.max(width, height) / 56)));
            ctx.fillStyle = "#272727";
            ctx.fillRect(0, 0, width, height);
            ctx.fillStyle = "#404040";
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
        mirrorsState: true
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

        // Same toggle language as the video CompareModeBar: quiet at rest, a light surface on
        // hover, and the accent-tinted fill reserved for the selected option of each group.
        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (!chip.enabled)
                    return "transparent";
                if (chip.checked)
                    return Theme.controlChecked;
                if (chip.down)
                    return Theme.fluentPressed;
                if (chip.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: chip.activeFocus ? 2 : 1
            border.color: {
                if (chip.activeFocus)
                    return Theme.focus;
                if (!chip.enabled)
                    return "transparent";
                if (chip.checked)
                    return Theme.accent;
                if (chip.hovered)
                    return Theme.subtleBorder;
                return "transparent";
            }

            Behavior on color {
                ColorAnimation {
                    duration: 100
                }
            }
        }
    }

    // Hairline between button groups inside a wrapping header row. A positioner hands every
    // child its own y, so a bare 1x22 rectangle would sit against the top edge instead of on
    // the row's centre line; this wrapper carries the row height and centres the line in it.
    component RowSeparator: Item {
        property int hairlineHeight: 22

        width: 1
        height: 30

        Rectangle {
            width: 1
            height: parent.hairlineHeight
            color: Theme.border
            anchors.verticalCenter: parent.verticalCenter
        }
    }

    // Shared status-bar chip: a dark raised plate with a hairline border, where the semantic
    // colour lives in one 6 px dot (or a single glyph) instead of a whole saturated pill. A row
    // of them then reads as one instrument strip rather than a rainbow of one-off badges.
    component StatusBadge: Rectangle {
        id: badge

        property string text: ""
        property string glyph: ""
        property color dotColor: Theme.mutedText
        property color glyphColor: Theme.mutedText
        property bool dotVisible: true

        height: 22
        implicitWidth: badgeContent.implicitWidth + 20
        radius: 6
        color: Theme.raisedPanel
        border.width: 1
        border.color: Theme.border
        anchors.verticalCenter: parent.verticalCenter

        Row {
            id: badgeContent

            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                visible: badge.dotVisible && badge.glyph.length === 0
                width: 6
                height: 6
                radius: 3
                color: badge.dotColor
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                visible: badge.glyph.length > 0
                text: badge.glyph
                color: badge.glyphColor
                font.pixelSize: 12
                font.weight: Font.Bold
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                text: badge.text
                color: Theme.primaryText
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }
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
        // Step-3 crop selection drag (edit mode only; left button, no modifier).
        property bool cropActive: false
        property point cropStart: Qt.point(0, 0)
        property point cropCurrent: Qt.point(0, 0)
        // Annotation drag gesture (select tool) and the text-click guard.
        property bool annotationDragActive: false

        readonly property alias image: previewImage
        // Fit scale of the displayed image (fit-window base), forwarded so the workspace
        // status bar can report the physical display percentage.
        readonly property real fitScale: previewImage ? previewImage.fitScale : 1

        clip: true

        Rectangle {
            anchors.fill: parent
            color: Theme.canvas
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
            readonly property real fitScale: control.imageFitScale(viewport.width, viewport.height, sourceSize.width, sourceSize.height)
            readonly property real trueSizeScale: control.imageTrueSizeScale()
            readonly property real effectiveBaseScale: control.trueSize ? trueSizeScale : fitScale
            readonly property real drawWidth: sourceSize.width * effectiveBaseScale * control.zoom
            readonly property real drawHeight: sourceSize.height * effectiveBaseScale * control.zoom
            x: control.imageDrawEdge(viewport.width, drawWidth, control.imageReview ? control.imageReview.panX : 0.5)
            y: control.imageDrawEdge(viewport.height, drawHeight, control.imageReview ? control.imageReview.panY : 0.5)
            width: drawWidth
            height: drawHeight
            source: viewport.imageUrl
            fillMode: Image.Stretch
            asynchronous: false
            cache: false
            smooth: control.zoom <= 2
        }

        // The title and the size/format readout sit on the image itself, so they get a
        // translucent plate: readable over bright or busy content, and it never takes canvas
        // input away from panning, marquee selection or tool strokes underneath.
        Rectangle {
            visible: viewport.label.length > 0 || viewport.title.length > 0
            width: Math.min(viewportLabelColumn.implicitWidth + 18, Math.max(200, viewport.width - 60))
            height: viewportLabelColumn.implicitHeight + 12
            radius: 8
            color: Theme.stageLabel
            border.width: 1
            border.color: Theme.stageLabelBorder
            anchors {
                top: parent.top
                left: parent.left
                margins: 10
            }

            Column {
                id: viewportLabelColumn

                spacing: 2
                anchors.centerIn: parent

                Text {
                    visible: viewport.title.length > 0
                    objectName: "imageViewportTitle-" + viewport.slot
                    text: viewport.title
                    color: Theme.primaryText
                    font.pixelSize: 12
                    font.weight: Font.DemiBold
                    elide: Text.ElideMiddle
                    width: Math.min(implicitWidth, Math.max(160, viewport.width - 76))

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
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, Math.max(160, viewport.width - 76))
                }
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
                color: syncedCrosshair.isHoveredSource ? Theme.probeLineMuted : Theme.probeLine
            }

            // Horizontal hairline
            Rectangle {
                x: Math.max(0, previewImage.x)
                y: Math.round(syncedCrosshair.targetY)
                width: Math.min(viewport.width, previewImage.width)
                height: 1
                color: syncedCrosshair.isHoveredSource ? Theme.probeLineMuted : Theme.probeLine
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
                border.color: Theme.probe
                border.width: 1.5
            }

            Rectangle {
                visible: !syncedCrosshair.isHoveredSource
                x: Math.round(syncedCrosshair.targetX - 2)
                y: Math.round(syncedCrosshair.targetY - 2)
                width: 4
                height: 4
                radius: 2
                color: Theme.probe
            }

            // Coordinate tag on the synced viewport
            Rectangle {
                visible: !syncedCrosshair.isHoveredSource && control.hoverPoint !== null
                x: Math.min(viewport.width - width - 8, Math.max(8, Math.round(syncedCrosshair.targetX + 12)))
                y: Math.min(viewport.height - height - 8, Math.max(8, Math.round(syncedCrosshair.targetY + 12)))
                width: coordText.implicitWidth + 8
                height: 18
                radius: 3
                color: Theme.oscGlass
                border.color: Theme.probe
                border.width: 1

                Text {
                    id: coordText
                    anchors.centerIn: parent
                    text: control.hoverPoint ? "%1, %2".arg(control.hoverPoint.imgX).arg(control.hoverPoint.imgY) : ""
                    color: Theme.probe
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
                color: Theme.probePip
            }
        }

        ImageMarqueeRect {
            active: viewport.marqueeActive
            start: viewport.marqueeStart
            current: viewport.marqueeCurrent
        }

        // Crop selection while dragging in edit mode; the applied selection is kept as a
        // workspace-level image-pixel rect so it survives further zooming.
        Rectangle {
            id: cropRect
            objectName: "imageCropRect-" + viewport.slot
            visible: viewport.cropActive
            z: 26
            x: Math.min(viewport.cropStart.x, viewport.cropCurrent.x)
            y: Math.min(viewport.cropStart.y, viewport.cropCurrent.y)
            width: Math.abs(viewport.cropCurrent.x - viewport.cropStart.x)
            height: Math.abs(viewport.cropCurrent.y - viewport.cropStart.y)
            color: Theme.cropFill
            border.color: Theme.cropBorder
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
                if (mouse.button === Qt.LeftButton && !control.editModeActive && control.compareMode === 0 && control.hasPair && !viewport.dragActive && !viewport.marqueeActive)
                    control.toggleSinglePairSource();
            }
            onPositionChanged: mouse => {
                if (control.editModeActive && control.imageEdit && control.imageEdit.strokeActive) {
                    control.continueBrushStroke(viewport, mouse.x, mouse.y);
                } else if (viewport.annotationDragActive) {
                    control.continueAnnotationDrag(viewport, mouse.x, mouse.y);
                } else if (viewport.cropActive) {
                    viewport.cropCurrent = Qt.point(mouse.x, mouse.y);
                    control.updateCropSelection(viewport);
                } else if (viewport.marqueeActive) {
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
                // Edit mode: the left button runs the active tool (brush paints, select
                // drags an annotation, text places on click, everything else selects a
                // rect), while the middle button keeps panning below. View mode keeps the
                // old gestures untouched.
                if (control.editModeActive && mouse.button === Qt.LeftButton && !(mouse.modifiers & Qt.ShiftModifier)) {
                    viewport.marqueeActive = false;
                    viewport.dragActive = false;
                    viewport.cropActive = false;
                    viewport.annotationDragActive = false;
                    viewport.pressStart = Qt.point(mouse.x, mouse.y);
                    if (control.editTool === "brush") {
                        control.beginBrushStroke(viewport, mouse.x, mouse.y);
                    } else if (control.editTool === "select") {
                        viewport.annotationDragActive = control.beginAnnotationDrag(viewport, mouse.x, mouse.y);
                    } else if (control.editTool === "text") {
                        // Placed on release, so a press that turns into a drag is ignored.
                    } else {
                        viewport.cropActive = true;
                        viewport.cropStart = Qt.point(mouse.x, mouse.y);
                        viewport.cropCurrent = Qt.point(mouse.x, mouse.y);
                    }
                } else if (mouse.button === Qt.LeftButton && (mouse.modifiers & Qt.ShiftModifier)) {
                    viewport.cropActive = false;
                    viewport.annotationDragActive = false;
                    viewport.marqueeActive = true;
                    viewport.marqueeStart = Qt.point(mouse.x, mouse.y);
                    viewport.marqueeCurrent = Qt.point(mouse.x, mouse.y);
                    viewport.dragActive = false;
                } else {
                    viewport.cropActive = false;
                    viewport.marqueeActive = false;
                    viewport.dragStart = Qt.point(mouse.x, mouse.y);
                    viewport.pressStart = Qt.point(mouse.x, mouse.y);
                    viewport.dragActive = false;
                }
                control.forceActiveFocus();
            }
            onReleased: mouse => {
                control.endBrushStroke();
                if (viewport.annotationDragActive) {
                    viewport.annotationDragActive = false;
                    viewport.suppressClickAfterMarquee = true;
                    control.endAnnotationDrag();
                }
                if (viewport.cropActive) {
                    viewport.cropActive = false;
                    viewport.suppressClickAfterMarquee = true;
                    control.updateCropSelection(viewport);
                    // Fill/clear/rect/arrow are one-shot: they commit on release. Crop and
                    // mosaic keep the selection for the apply button.
                    control.applyRectTool();
                }
                if (control.editModeActive && control.editTool === "text" && Math.hypot(mouse.x - viewport.pressStart.x, mouse.y - viewport.pressStart.y) < 5) {
                    control.placeTextAt(viewport, mouse.x, mouse.y);
                }
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

    // The command surface is one raised panel rather than rows floating on the canvas: file,
    // view, comparison and edit commands belong to a single tool area, and the panel edge gives
    // the wrapping rows a visible boundary at every window width.
    Rectangle {
        id: commandPanel

        objectName: "imageCommandPanel"
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: 12
        }
        height: commandColumn.implicitHeight + 20
        radius: 10
        color: Theme.panelElevated
        border.width: 1
        border.color: Theme.border

        Column {
            id: commandColumn

            anchors {
                top: parent.top
                topMargin: 10
                left: parent.left
                leftMargin: 10
                right: parent.right
                rightMargin: 10
            }
            spacing: 8

            // Row A — file and view commands. Opening variants live behind one dropdown so the
            // top edge never grows past a single row; sidebar toggling only appears with folders.
            Flow {
                id: fileRow

                objectName: "imageFileRow"
                width: parent.width
                spacing: 8
                // See the forceLayout note in Component.onCompleted above.
                onWidthChanged: forceLayout()

                ReviewActionButton {
                    objectName: "imageOpenButton"
                    text: qsTr("打开图片…")
                    // Edit mode has its own primary action (另存副本…); one filled button per view.
                    prominent: !control.editModeActive
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
                    objectName: "imageFitButton"
                    checkable: true
                    checked: !control.trueSize
                    mirrorsState: true
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
                    mirrorsState: true
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

                RowSeparator {
                    visible: control.hasFolders
                }

                ReviewActionButton {
                    objectName: "imageToggleSidebarButton"
                    // The label names the action it performs, so the button carries no checked tint.
                    text: control.sidebarVisible ? qsTr("隐藏列表") : qsTr("显示列表")
                    implicitHeight: 30
                    leftPadding: 10
                    rightPadding: 10
                    visible: control.hasFolders
                    onClicked: control.toggleSidebarRequested()
                }
            }

            // Row B — comparison layout, difference analysis and observation. Mutually exclusive
            // modes stay grouped; secondary modes collapse into "more" menus like the video bar,
            // so a pair never shows the full control surface at once.
            Flow {
                id: modeRow

                objectName: "imageModeRow"
                width: parent.width
                spacing: 8
                // See the forceLayout note in Component.onCompleted above.
                onWidthChanged: forceLayout()

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

                RowSeparator {
                    visible: control.hasPair
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

                RowSeparator {}

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
                    helpText: qsTr("只放大显示，方便看清弱差异；峰值/平均差异读数仍是原始差值。")
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
                    helpText: qsTr("显示通道：正常 RGBA，或只观察 Alpha 灰度、忽略透明度。快捷键 A / O。")
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
                    helpText: qsTr("图片背后的底色，用来判断透明区域；不影响像素读数。")
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

            // Row C — step-3 image editing suite. The original file is never written to: the session
            // edits a working copy and saving always produces a new file. Every tool takes its
            // geometry in image pixels, so zooming or panning cannot drift a selection or a
            // stroke. The tool only changes what the left button does; middle-drag still pans.
            Rectangle {
                id: editSuite

                visible: control.hasPrimary && control.imageEdit !== null
                width: parent.width
                height: editRibbon.implicitHeight + (control.editModeActive ? 16 : 0)
                radius: 6
                color: control.editModeActive ? Theme.raisedPanel : "transparent"

                Column {
                    id: editRibbon

                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: control.editModeActive ? 8 : 0
                    }
                    spacing: 8

                    Flow {
                        id: editRow
                        objectName: "imageEditRow"
                        width: parent.width
                        spacing: 6
                        visible: control.hasPrimary && control.imageEdit !== null
                        // See the forceLayout note in Component.onCompleted above.
                        onWidthChanged: forceLayout()

                        EditButton {
                            objectName: "imageEditStartButton"
                            // An action label, not a state: the filled button while editing is 另存副本….
                            text: control.editModeActive ? qsTr("结束编辑") : qsTr("编辑画面")
                            helpText: qsTr("在原图的副本上编辑；原文件保持不变，编辑结果必须另存为副本。")
                            onClicked: {
                                if (control.editModeActive)
                                    control.finishImageEdit();
                                else
                                    control.beginImageEdit();
                            }
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        EditButton {
                            objectName: "imageEditToolCropButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "crop"
                            mirrorsState: true
                            text: qsTr("裁剪")
                            onClicked: control.editTool = "crop"
                        }
                        EditButton {
                            objectName: "imageEditToolBrushButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "brush"
                            mirrorsState: true
                            text: qsTr("画笔")
                            onClicked: control.editTool = "brush"
                        }
                        EditButton {
                            objectName: "imageEditToolSelectButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "select"
                            mirrorsState: true
                            text: qsTr("选择")
                            helpText: qsTr("点击选中标注后可拖动；Delete 键或「删除标注」可删除。")
                            onClicked: control.editTool = "select"
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        // Whole-image geometry, not a brush: these change the working copy's size rather
                        // than the pixels under the cursor, so they sit next to the tool menu instead of
                        // inside it.
                        EditButton {
                            objectName: "imageEditScaleButton"
                            visible: control.editModeActive
                            text: qsTr("缩放…")
                            helpText: qsTr("把画面重采样到指定的像素尺寸；锁定比例时改一边会自动推算另一边。")
                            onClicked: imageEditDialogs.openScale()
                        }
                        EditButton {
                            objectName: "imageEditFillButton"
                            visible: control.editModeActive
                            text: qsTr("填充画布…")
                            helpText: qsTr("把原图居中放到更大的画布上，四周用指定颜色补齐。")
                            onClicked: imageEditDialogs.openFill()
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        // The remaining tools share one dropdown. Two-thirds of the row used to be tool
                        // buttons, which made the row the widest thing in the header and pushed 撤销/重做
                        // past the window edge at 900 px. The button carries the active tool's name so a
                        // hidden-but-selected tool is never invisible.
                        EditButton {
                            objectName: "imageEditMoreToolsButton"
                            visible: control.editModeActive
                            text: control.imageEditToolLabel(control.editTool) + " ▾"
                            helpText: qsTr("马赛克、填充、清除、矩形、箭头、文字与标注管理。")
                            onClicked: moreToolsMenu.open()

                            VcsMenu {
                                id: moreToolsMenu

                                objectName: "imageEditMoreToolsMenu"
                                menuWidth: 240

                                // Radio rows: the tools are exclusive, and an exclusive item
                                // stays checked when the active tool is chosen again.
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolMosaicButton"
                                    text: qsTr("马赛克")
                                    checked: control.editTool === "mosaic"
                                    onTriggered: control.editTool = "mosaic"
                                }
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolFillButton"
                                    text: qsTr("填充")
                                    checked: control.editTool === "fill"
                                    onTriggered: control.editTool = "fill"
                                }
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolClearButton"
                                    text: qsTr("清除")
                                    checked: control.editTool === "clear"
                                    onTriggered: control.editTool = "clear"
                                }
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolRectButton"
                                    text: qsTr("矩形")
                                    checked: control.editTool === "rect"
                                    onTriggered: control.editTool = "rect"
                                }
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolArrowButton"
                                    text: qsTr("箭头")
                                    checked: control.editTool === "arrow"
                                    onTriggered: control.editTool = "arrow"
                                }
                                VcsRadioMenuItem {
                                    objectName: "imageEditToolTextButton"
                                    text: qsTr("文字")
                                    checked: control.editTool === "text"
                                    onTriggered: control.editTool = "text"
                                }

                                VcsMenuSeparator {}

                                VcsMenuItem {
                                    objectName: "imageEditDeleteAnnotationMenuItem"
                                    text: qsTr("删除选中标注")
                                    enabled: control.imageEdit !== null && control.imageEdit.selectedAnnotation >= 0
                                    onTriggered: {
                                        if (control.imageEdit)
                                            control.imageEdit.deleteSelectedAnnotation();
                                    }
                                }
                                VcsMenuItem {
                                    objectName: "imageEditClearAnnotationsMenuItem"
                                    text: qsTr("清空所有标注")
                                    enabled: control.imageEdit !== null && control.imageEdit.annotationCount > 0
                                    onTriggered: {
                                        if (control.imageEdit)
                                            control.imageEdit.clearAnnotations();
                                    }
                                }
                            }
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        EditButton {
                            objectName: "imageEditUndoButton"
                            visible: control.editModeActive
                            text: qsTr("撤销")
                            enabled: control.imageEdit && control.imageEdit.canUndo
                            onClicked: {
                                if (control.imageEdit)
                                    control.imageEdit.undo();
                            }
                        }
                        EditButton {
                            objectName: "imageEditRedoButton"
                            visible: control.editModeActive
                            text: qsTr("重做")
                            enabled: control.imageEdit && control.imageEdit.canRedo
                            onClicked: {
                                if (control.imageEdit)
                                    control.imageEdit.redo();
                            }
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        EditButton {
                            objectName: "imageEditSaveCopyButton"
                            visible: control.editModeActive
                            prominent: true
                            text: qsTr("另存副本…")
                            onClicked: editSaveDialog.open()
                        }
                        Text {
                            objectName: "imageEditStatusText"
                            visible: control.editModeActive
                            height: 30
                            verticalAlignment: Text.AlignVCenter
                            text: control.imageEdit ? String(control.imageEdit.lastStatus || "") : ""
                            color: Theme.mutedText
                            font.pixelSize: 11
                            elide: Text.ElideRight
                            width: Math.min(240, implicitWidth)
                        }
                    }

                    // Row D — parameters for the active tool. The plate uses the canvas colour so the
                    // strip reads as a recessed well inside the raised edit ribbon, and it wraps
                    // instead of clipping once a tool carries more controls than fit on one line.
                    Rectangle {
                        visible: control.hasPrimary && control.editModeActive
                        width: parent.width
                        height: editParamsRow.implicitHeight + 12
                        radius: 6
                        color: Theme.canvas
                        border.width: 1
                        border.color: Theme.border

                        Flow {
                            id: editParamsRow
                            objectName: "imageEditParamsRow"
                            spacing: 8
                            visible: control.hasPrimary && control.editModeActive
                            // See the forceLayout note in Component.onCompleted above.
                            onWidthChanged: forceLayout()

                            anchors {
                                left: parent.left
                                right: parent.right
                                leftMargin: 10
                                rightMargin: 10
                                verticalCenter: parent.verticalCenter
                            }

                            ReviewActionButton {
                                objectName: "imageEditBrushColorButton"
                                visible: control.editTool === "brush" || control.editTool === "fill" || control.editTool === "rect" || control.editTool === "arrow" || control.editTool === "text"
                                text: qsTr("颜色 ▾")
                                implicitHeight: 30
                                leftPadding: 24
                                rightPadding: 8
                                onClicked: brushColorMenu.open()

                                Rectangle {
                                    width: 10
                                    height: 10
                                    radius: 5
                                    // Full strength: the swatch previews the exact colour the brush paints.
                                    color: control.brushColor
                                    border.width: 1
                                    border.color: Theme.primaryText
                                    anchors {
                                        left: parent.left
                                        leftMargin: 8
                                        verticalCenter: parent.verticalCenter
                                    }
                                }

                                VcsMenu {
                                    id: brushColorMenu
                                    menuWidth: 150

                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorRed"
                                        text: qsTr("红")
                                        checked: control.brushColor === "#ff3b30"
                                        onTriggered: control.brushColor = "#ff3b30"
                                    }
                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorYellow"
                                        text: qsTr("黄")
                                        checked: control.brushColor === "#ffd60a"
                                        onTriggered: control.brushColor = "#ffd60a"
                                    }
                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorGreen"
                                        text: qsTr("绿")
                                        checked: control.brushColor === "#34c759"
                                        onTriggered: control.brushColor = "#34c759"
                                    }
                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorCyan"
                                        text: qsTr("青")
                                        checked: control.brushColor === "#32ade6"
                                        onTriggered: control.brushColor = "#32ade6"
                                    }
                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorWhite"
                                        text: qsTr("白")
                                        checked: control.brushColor === "#ffffff"
                                        onTriggered: control.brushColor = "#ffffff"
                                    }
                                    VcsRadioMenuItem {
                                        objectName: "imageEditBrushColorBlack"
                                        text: qsTr("黑")
                                        checked: control.brushColor === "#000000"
                                        onTriggered: control.brushColor = "#000000"
                                    }
                                }
                            }
                            Text {
                                objectName: "imageEditBrushWidthLabel"
                                visible: control.editTool === "brush" || control.editTool === "rect" || control.editTool === "arrow"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: control.editTool === "brush" ? qsTr("粗细 %1").arg(control.brushWidth) : qsTr("线宽 %1").arg(control.brushWidth)
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                            Slider {
                                objectName: "imageEditBrushWidthSlider"
                                visible: control.editTool === "brush" || control.editTool === "rect" || control.editTool === "arrow"
                                width: 96
                                height: 30
                                from: 1
                                to: 64
                                stepSize: 1
                                value: control.brushWidth
                                onMoved: control.brushWidth = Math.round(value)
                            }
                            Text {
                                objectName: "imageEditBrushOpacityLabel"
                                visible: control.editTool === "brush"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("透明 %1%").arg(Math.round(control.brushOpacity * 100))
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                            Slider {
                                objectName: "imageEditBrushOpacitySlider"
                                visible: control.editTool === "brush"
                                width: 96
                                height: 30
                                from: 0.05
                                to: 1
                                stepSize: 0.05
                                value: control.brushOpacity
                                onMoved: control.brushOpacity = value
                            }
                            Text {
                                objectName: "imageEditMosaicBlockLabel"
                                visible: control.editTool === "mosaic"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("块大小 %1").arg(control.mosaicBlock)
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                            Slider {
                                objectName: "imageEditMosaicBlockSlider"
                                visible: control.editTool === "mosaic"
                                width: 120
                                height: 30
                                from: 2
                                to: 48
                                stepSize: 1
                                value: control.mosaicBlock
                                onMoved: control.mosaicBlock = Math.round(value)
                            }
                            Text {
                                objectName: "imageEditAnnotationTextLabel"
                                visible: control.editTool === "text"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("文字")
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                            TextField {
                                objectName: "imageEditAnnotationTextField"
                                visible: control.editTool === "text"
                                width: 180
                                height: 30
                                placeholderText: qsTr("输入标注文字，再点击画面放置")
                                text: control.annotationText
                                font.pixelSize: 12
                                onTextEdited: control.annotationText = text
                            }
                            Text {
                                objectName: "imageEditAnnotationTextSizeLabel"
                                visible: control.editTool === "text"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("字号 %1").arg(control.annotationTextSize)
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                            Slider {
                                objectName: "imageEditAnnotationTextSizeSlider"
                                visible: control.editTool === "text"
                                width: 110
                                height: 30
                                from: 10
                                to: 160
                                stepSize: 1
                                value: control.annotationTextSize
                                onMoved: control.annotationTextSize = Math.round(value)
                            }
                            ReviewActionButton {
                                objectName: "imageEditApplyCropButton"
                                visible: control.editTool === "crop" || control.editTool === "mosaic"
                                prominent: true
                                text: control.cropSelection ? (control.editTool === "mosaic" ? qsTr("应用马赛克 %1×%2").arg(control.cropSelection.width).arg(control.cropSelection.height) : qsTr("应用裁剪 %1×%2").arg(control.cropSelection.width).arg(control.cropSelection.height)) : (control.editTool === "mosaic" ? qsTr("应用马赛克") : qsTr("应用裁剪"))
                                enabled: control.cropSelection !== null && control.imageEdit && control.imageEdit.active
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                helpText: qsTr("编辑模式下左键框选区域，中键仍可平移；画笔工具下左键直接涂画。")
                                onClicked: control.applyCropSelection()
                            }
                            ReviewActionButton {
                                objectName: "imageEditDeleteAnnotationButton"
                                visible: control.editTool === "select"
                                text: qsTr("删除标注")
                                enabled: control.imageEdit && control.imageEdit.selectedAnnotation >= 0
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                onClicked: {
                                    if (control.imageEdit)
                                        control.imageEdit.deleteSelectedAnnotation();
                                }
                            }
                            ReviewActionButton {
                                objectName: "imageEditClearAnnotationsButton"
                                visible: control.editTool === "select"
                                text: qsTr("清除全部标注")
                                enabled: control.imageEdit && control.imageEdit.annotationCount > 0
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                onClicked: {
                                    if (control.imageEdit)
                                        control.imageEdit.clearAnnotations();
                                }
                            }
                            Text {
                                objectName: "imageEditAnnotationCountText"
                                visible: control.editTool === "select"
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: control.imageEdit ? qsTr("标注 %1 个").arg(control.imageEdit.annotationCount) : ""
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }
                        }
                    }
                }
            }
        }
    }

    // Header buttons size to their label instead of the shared 112 px floor: the row
    // is the widest thing in the header and 重做/另存副本 used to clip past the window
    // edge well above the 960 px minimum width.
    component EditButton: ReviewActionButton {
        implicitHeight: 30
        leftPadding: 10
        rightPadding: 10
        // implicitContentWidth does not notify, so a binding to it keeps the value it had
        // during construction; bind the label's own width instead.
        implicitWidth: Math.max(64, contentItem.implicitWidth + leftPadding + rightPadding + 8)
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
        } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && control.editModeActive && control.imageEdit && control.imageEdit.selectedAnnotation >= 0) {
            control.imageEdit.deleteSelectedAnnotation();
            event.accepted = true;
        }
    }

    Item {
        id: stage

        anchors {
            top: commandPanel.bottom
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

                // Empty state: a headline that names the state and a second line that spells out
                // both workflows, instead of one run-on sentence in flat grey.
                Column {
                    visible: !control.hasPrimary
                    width: Math.min(stageContent.width - 48, 460)
                    spacing: 10
                    anchors.centerIn: parent

                    Text {
                        width: parent.width
                        text: qsTr("还没有打开图片")
                        color: Theme.primaryText
                        font.pixelSize: 17
                        font.weight: Font.DemiBold
                        horizontalAlignment: Text.AlignHCenter
                    }

                    Text {
                        width: parent.width
                        text: qsTr("「打开图片…」查看单张的通道与透明度；\n「打开图片对…」载入 A / B 后可做差异、分割线与淡化对比。")
                        color: Theme.mutedText
                        font.pixelSize: 12
                        lineHeight: 1.4
                        wrapMode: Text.WordWrap
                        horizontalAlignment: Text.AlignHCenter
                    }
                }

                // Manual A/B comparison badge stays inside the image stage. A dark translucent
                // plate plus an identity dot (A = accent blue, B = amber) states the active side
                // the way the rest of the app does, instead of a saturated green/teal pill that
                // competed with the imagery it sits on.
                Rectangle {
                    id: imageInPlaceBadge
                    objectName: "imageInPlaceBadge"
                    visible: control.hasPair && control.compareMode === 0
                    z: 30
                    width: Math.min(stageContent.width - 28, inPlaceBadgeContent.implicitWidth + 20)
                    height: 28
                    radius: 6
                    color: Theme.oscGlass
                    border.width: 1
                    border.color: control.singleViewShowSecondary ? Theme.sourceBBorder : Theme.sourceABorder
                    anchors {
                        top: parent.top
                        right: parent.right
                        margins: 14
                    }

                    Row {
                        id: inPlaceBadgeContent
                        spacing: 7
                        anchors.centerIn: parent

                        Rectangle {
                            width: 7
                            height: 7
                            radius: 3.5
                            color: control.singleViewShowSecondary ? Theme.sourceB : Theme.sourceA
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: control.singleViewShowSecondary ? qsTr("当前 B") : qsTr("当前 A")
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: qsTr("点击画面或按空格切换")
                            color: Theme.mutedText
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: control.toggleSinglePairSource()
                    }
                }

                // Alpha & background observation HUD badge. Same plate as the A/B badge so the
                // two never look like unrelated widgets; the dot carries the state colour and the
                // text names the state in words.
                Rectangle {
                    id: imageAlphaObservationBadge
                    objectName: "imageAlphaObservationBadge"
                    visible: control.hasPrimary && (control.viewMode !== 0 || control.backgroundMode !== 1)
                    z: 30
                    width: Math.min(stageContent.width - (imageInPlaceBadge.visible ? imageInPlaceBadge.width + 52 : 28), alphaBadgeContent.implicitWidth + 20)
                    height: 28
                    radius: 6
                    color: Theme.oscGlass
                    border.width: 1
                    // The dot carries the state; the plate matches the other stage badges.
                    border.color: Theme.oscBorder
                    anchors {
                        top: parent.top
                        right: imageInPlaceBadge.visible ? imageInPlaceBadge.left : parent.right
                        rightMargin: imageInPlaceBadge.visible ? 10 : 14
                        topMargin: 14
                    }

                    Row {
                        id: alphaBadgeContent
                        spacing: 7
                        anchors.centerIn: parent

                        Rectangle {
                            width: 7
                            height: 7
                            radius: 3.5
                            color: {
                                if (control.viewMode === 1)
                                    return Theme.information;
                                if (control.viewMode === 2)
                                    return Theme.warning;
                                return Theme.accent;
                            }
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            id: imageAlphaObservationText
                            objectName: "imageAlphaObservationText"
                            text: {
                                if (control.viewMode === 1)
                                    return qsTr("观察 · Alpha 灰度");
                                if (control.viewMode === 2)
                                    return qsTr("观察 · RGB 忽略透明度");
                                const bgNames = [qsTr("深色底"), qsTr("棋盘格底"), qsTr("黑底"), qsTr("白底")];
                                return qsTr("背景 · %1").arg(bgNames[control.backgroundMode] || "");
                            }
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: {
                                if (control.viewMode === 1)
                                    return qsTr("按 A 或点击还原 RGBA");
                                if (control.viewMode === 2)
                                    return qsTr("按 O 或点击还原 RGBA");
                                return qsTr("点击还原棋盘格背景");
                            }
                            color: Theme.mutedText
                            font.pixelSize: 11
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    MouseArea {
                        anchors.fill: parent
                        onClicked: {
                            if (control.viewMode !== 0) {
                                if (control.imageReview)
                                    control.imageReview.viewMode = 0;
                            } else {
                                control.backgroundMode = 1;
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
                        // While editing, the pane shows the working copy; the compared
                        // originals (and every difference statistic) stay untouched.
                        imageUrl: control.editModeActive && control.editSourceSlot === slot && control.imageEdit ? control.imageEdit.editedImageUrl : (control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(slot) : "")
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
                        color: Theme.canvas
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

                    readonly property real fitScale: control.imageReview ? control.imageFitScale(width, height, control.imageReview.primaryWidth, control.imageReview.primaryHeight) : 1
                    readonly property real effectiveBaseScale: control.trueSize ? control.imageTrueSizeScale() : fitScale
                    readonly property real drawWidth: control.imageReview ? control.imageReview.primaryWidth * effectiveBaseScale * control.zoom : 0
                    readonly property real drawHeight: control.imageReview ? control.imageReview.primaryHeight * effectiveBaseScale * control.zoom : 0
                    readonly property real drawX: control.imageDrawEdge(width, drawWidth, control.imageReview ? control.imageReview.panX : 0.5)
                    readonly property real drawY: control.imageDrawEdge(height, drawHeight, control.imageReview ? control.imageReview.panY : 0.5)
                    readonly property real splitX: width * control.wipePosition
                    property bool marqueeActive: false
                    property point marqueeStart: Qt.point(0, 0)
                    property point marqueeCurrent: Qt.point(0, 0)

                    ImageMarqueeRect {
                        active: wipeOverlay.marqueeActive
                        start: wipeOverlay.marqueeStart
                        current: wipeOverlay.marqueeCurrent
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
                    // reversed while the split is dragged. Both ride on translucent plates
                    // like the viewport labels, so they stay readable over bright imagery.
                    Rectangle {
                        visible: control.hasPair && control.hasSecondary && wipeOverlay.splitX > 44
                        width: wipeIdentityLeft.width + 18
                        height: wipeIdentityLeft.implicitHeight + 12
                        radius: 8
                        color: Theme.stageLabel
                        border.width: 1
                        border.color: Theme.stageLabelBorder
                        anchors {
                            top: parent.top
                            left: parent.left
                            margins: 10
                        }

                        Text {
                            id: wipeIdentityLeft

                            objectName: "wipeIdentityLeft"
                            visible: width > 4
                            text: control.hasSecondary ? qsTr("B · %1").arg(control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath)) : ""
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            width: Math.max(0, wipeOverlay.splitX - 36)
                            anchors.centerIn: parent
                        }
                    }

                    Rectangle {
                        visible: control.hasPair && (wipeOverlay.width - wipeOverlay.splitX) > 44
                        width: wipeIdentityRight.width + 18
                        height: wipeIdentityRight.implicitHeight + 12
                        radius: 8
                        color: Theme.stageLabel
                        border.width: 1
                        border.color: Theme.stageLabelBorder
                        anchors {
                            top: parent.top
                            right: parent.right
                            margins: 10
                        }

                        Text {
                            id: wipeIdentityRight

                            objectName: "wipeIdentityRight"
                            visible: width > 4
                            text: control.hasPrimary ? qsTr("A · %1").arg(control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath)) : ""
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            elide: Text.ElideRight
                            width: Math.max(0, wipeOverlay.width - wipeOverlay.splitX - 36)
                            anchors.centerIn: parent
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
                        color: Theme.canvas
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

                    readonly property real fitScale: control.imageReview ? control.imageFitScale(width, height, control.imageReview.primaryWidth, control.imageReview.primaryHeight) : 1
                    readonly property real effectiveBaseScale: control.trueSize ? control.imageTrueSizeScale() : fitScale
                    readonly property real drawWidth: control.imageReview ? control.imageReview.primaryWidth * effectiveBaseScale * control.zoom : 0
                    readonly property real drawHeight: control.imageReview ? control.imageReview.primaryHeight * effectiveBaseScale * control.zoom : 0
                    readonly property real drawX: control.imageDrawEdge(width, drawWidth, control.imageReview ? control.imageReview.panX : 0.5)
                    readonly property real drawY: control.imageDrawEdge(height, drawHeight, control.imageReview ? control.imageReview.panY : 0.5)
                    property bool marqueeActive: false
                    property point marqueeStart: Qt.point(0, 0)
                    property point marqueeCurrent: Qt.point(0, 0)

                    ImageMarqueeRect {
                        active: fadeOverlay.marqueeActive
                        start: fadeOverlay.marqueeStart
                        current: fadeOverlay.marqueeCurrent
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

                    // Identity for the blended pair: the fade chip reports the mix, this
                    // plate names what A and B actually are — same style as the viewport
                    // labels, and it never intercepts pan or marquee input.
                    Rectangle {
                        visible: control.hasPair
                        width: Math.min(fadeIdentityText.implicitWidth + 16, Math.max(200, fadeOverlay.width - 60))
                        height: fadeIdentityText.implicitHeight + 10
                        radius: 5
                        color: Theme.stageLabel
                        border.width: 1
                        border.color: Theme.stageLabelBorder
                        anchors {
                            top: parent.top
                            left: parent.left
                            margins: 10
                        }

                        Text {
                            id: fadeIdentityText

                            text: control.hasPair ? qsTr("A · %1 ↔ B · %2").arg(control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath)).arg(control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath)) : ""
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            elide: Text.ElideMiddle
                            width: Math.min(implicitWidth, Math.max(160, fadeOverlay.width - 76))
                            anchors.centerIn: parent
                        }
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
        width: fadeRow.implicitWidth + 24
        height: 36
        radius: 8
        color: Theme.oscGlass
        border.width: 1
        border.color: Theme.oscBorder
        anchors {
            right: stage.right
            bottom: stage.bottom
            margins: 14
        }
        z: 20

        Row {
            id: fadeRow

            spacing: 10
            anchors.centerIn: parent

            Text {
                // Fixed width: a readout that resizes with its own digits would make the whole
                // chip jitter under the cursor while the slider is being dragged.
                width: 156
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
                    color: imageFadeSlider.pressed ? Theme.accent : Theme.primaryText
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

            StatusBadge {
                id: displayConversionBadge
                objectName: "displayConversionBadge"
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted))
                text: qsTr("显示缓冲 RGBA8")
                dotColor: Theme.warning

                HoverHandler {
                    id: displayConversionHover
                }

                VcsToolTip {
                    visible: displayConversionHover.hovered
                    text: qsTr("像素格式/位深已转换到 RGBA8 显示缓冲，不是文件平面原始码值。\n未应用 ICC/颜色配置文件；YUV→RGB 使用解码器默认系数，颜色外观不作色彩管理承诺。\n高位深源请参考像素状态栏的转换后 16-bit 码值。")
                }
            }

            StatusBadge {
                id: resampleBadge
                objectName: "imageResampleBadge"
                visible: Boolean(control.imageReview && control.hasPair && (control.imageReview.diffResampled || (control.sizesDiffer && control.imageReview.resampleAllowed)))
                text: control.imageReview && control.imageReview.diffResampled ? qsTr("空间已重采样") : qsTr("重采样：开")
                dotColor: Theme.warning

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

            StatusBadge {
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryHasAlpha || control.imageReview.secondaryHasAlpha))
                text: qsTr("直通（未预乘）")
                glyph: "α"
                glyphColor: Theme.information
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

    // Resize and canvas-fill dialogs. They live here rather than inside the row so that
    // closing the dropdown cannot destroy a half-filled-in size.
    ImageEditDialogs {
        id: imageEditDialogs

        imageEdit: control.imageEdit
        imageWidth: control.imageEdit ? control.imageEdit.imageWidth : 0
        imageHeight: control.imageEdit ? control.imageEdit.imageHeight : 0
        maximumEdge: control.imageEdit ? control.imageEdit.maximumEdge : 16384
        minimumEdge: control.imageEdit ? control.imageEdit.minimumEdge : 1

        onScaleApplied: function (width, height, smooth) {
            return control.applyImageScale(width, height, smooth);
        }
        onFillApplied: function (width, height, fillColor) {
            return control.applyImageCanvasFill(width, height, fillColor);
        }
    }

    // Editing always writes a new file. PNG is the default because it keeps transparency;
    // a JPEG target needs an explicit background and this build reports it honestly when
    // the JPEG encoder is unavailable.
    FileDialog {
        id: editSaveDialog
        objectName: "imageEditSaveDialog"
        title: qsTr("另存编辑副本")
        fileMode: FileDialog.SaveFile
        defaultSuffix: "png"
        nameFilters: [qsTr("PNG 图片 (*.png)"), qsTr("JPEG 图片 (*.jpg *.jpeg)")]
        onAccepted: {
            if (!control.imageEdit)
                return;
            const path = String(selectedFile);
            const jpeg = /\.jpe?g$/i.test(path);
            control.imageEdit.saveCopy(selectedFile, jpeg, jpeg ? "#ffffff" : "transparent");
        }
    }
}
