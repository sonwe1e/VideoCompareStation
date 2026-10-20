pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import QtQuick.Window
import Dvs.Ui 1.0
import "VcsTheme.js" as Theme
import "MediaLabels.js" as MediaLabels

// Still-image workspace: single-image channel inspector + two-image DiffChecker.
Rectangle {
    id: control

    // The composition root owns both adapters and passes them in: the controller is a real
    // ImageReviewController, the clipboard a real ColorClipboardController (typed properties,
    // so member typos fail qmllint and a wrong object cannot become the controller).
    required property ImageReviewController controller
    required property ColorClipboardController colorClipboard
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
    // Drop-to-replace on the A/B source chips carries the dropped URL directly, so Main can
    // stage the in-place slot replace (requestReplacePrimary/Secondary) without a file
    // dialog. Mirrors the video strip's replaceDropped(identity, url) contract.
    signal replacePrimaryDropped(url url)
    signal replaceSecondaryDropped(url url)
    // I0x "右键关闭此图片": the chip's right-click menu names the side it belongs to, so
    // this handler closes exactly that side through the controller's single-side close.
    signal closeSideRequested(int sourceSlot)

    // False = fit-window display; true = true-size display where 1 image pixel maps to
    // 1 physical screen pixel (device-pixel-ratio aware). The two are separate commands
    // (T2); the controller zoom multiplies whichever base is active.
    property bool trueSize: false
    property bool pixelGridEnabled: false

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

    // Typed, non-nullable in every real session (the main window hands in the composition
    // root's controller); the concrete type is kept here so every member access below is
    // checked by qmllint instead of degrading to a dynamic lookup.
    readonly property ImageReviewController imageReview: controller
    readonly property bool hasPrimary: Boolean(controller && controller.hasPrimary)
    readonly property bool hasSecondary: Boolean(imageReview && imageReview.hasSecondary)
    readonly property bool hasPair: Boolean(imageReview && imageReview.hasPair)
    readonly property bool canSwitchSingleSource: hasPair && !editModeActive
    readonly property int compareMode: editModeActive ? 0 : (imageReview ? Number(imageReview.compareMode) : 0)
    readonly property real wipePosition: imageReview ? Number(imageReview.wipePosition) : 0.5
    readonly property real zoom: imageReview ? Number(imageReview.zoom) : 1
    readonly property var cursorPixel: imageReview ? imageReview.cursorPixel : ({})
    readonly property string errorText: imageReview ? String(imageReview.errorText || "") : ""
    readonly property bool hasFolders: Boolean(pairModel && pairModel.pairCount > 0)
    // FLOW3 single-directory browsing. The host owns the cursor and the open request; this
    // component only reports the key gesture and shows the position, so a failed decode is
    // decided in exactly one place.
    property bool folderBrowseActive: false
    property string folderBrowseText: ""
    signal folderStepRequested(int delta)
    signal folderJumpRequested(bool toLast)
    readonly property bool sizesDiffer: Boolean(imageReview && imageReview.hasPair && imageReview.primaryWidth > 0 && imageReview.primaryWidth !== imageReview.secondaryWidth || imageReview && imageReview.hasPair && imageReview.primaryHeight > 0 && imageReview.primaryHeight !== imageReview.secondaryHeight)
    // Alpha difference (6) is a per-pixel diff over the straight-alpha channel.
    readonly property bool diffModeActive: Boolean(imageReview && imageReview.hasPair && compareMode >= 2 && (compareMode <= 4 || compareMode === 6))
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
    // The scale/fill dialogs are modal popups that own the keyboard while open; Main.qml reads
    // this to keep workspace shortcuts inert behind them (the dialogs themselves carry the
    // popupInputContext marker for the focus walk).
    readonly property bool anyEditDialogOpen: imageEditDialogs.anyVisible
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
    property bool smartSegmentTightCrop: true

    // ---- Hold-to-peek (look at an original while a difference/fade view is active) ----
    // Peek is a temporary state: entering it snapshots {compareMode, viewMode, diffGain,
    // fadePosition, singleViewShowSecondary}, shows the named original as a plain single
    // view, and every release path (key up, focus loss, window deactivation, image/task
    // change, edit-mode change) restores that snapshot exactly. A press while a peek is
    // already active never re-snapshots, so the snapshot always describes the view from
    // before the peek.
    property bool peekActive: false
    property int peekSource: 0
    property var peekSnapshot: null

    // Floating crosshair coordinate card verbosity (P): false = concise "(x, y)",
    // true = detailed swatch + coordinates + original-pixel R G B A + α%.
    property bool crosshairVerbose: false

    // Same-normalized-coordinate A/B sampling of the two originals (refreshPairSample).
    // Never derived from the difference image: a/b come from samplePixel(2/3), so the
    // status-bar pair readout always names original pixels even in diff modes.
    property var pairSample: null

    // Crop aspect lock: 0 = free, otherwise width / height (1, 16/9, 4/3). The crop tool's
    // drag handles and the direct W/H inputs both honour it inside the image bounds.
    property real cropRatioLock: 0

    // Lightweight marquee feedback: a Shift+drag released outside the image used to vanish
    // silently; the status bar now names the reason for a couple of seconds.
    property string marqueeNoticeText: ""

    readonly property int contentGeneration: imageReview ? Number(imageReview.contentGeneration) : 0

    // Explicit relayout discipline for the whole command-panel chain, mirroring
    // CompareModeBar: hosts that pump the event loop without a render pass (the drag
    // harness) never run the positioners' polish, so the rows keep their creation
    // layout and the Column keeps a zero implicitHeight — the panel collapses to
    // zero height and Qt's drag traversal prunes the entire chip subtree at it.
    // Every state flip that adds or removes rows or chips re-runs the chain; the
    // deferred call settles after the current binding cycle, and Qt.callLater
    // coalesces repeated requests of the same function into one pass.
    function relayoutCommandPanel() {
        fileRow.forceLayout();
        modeRow.forceLayout();
        editRow.forceLayout();
        editParamsRow.forceLayout();
        // The Column re-sums only on its own layout pass; without it the rows above
        // settle while commandPanel.height (bound to commandColumn.implicitHeight)
        // still reads zero.
        commandColumn.forceLayout();
    }

    onHasPairChanged: {
        endPeek();
        if (!hasPair) {
            singleViewShowSecondary = false;
            hoverPoint = null;
        }
        Qt.callLater(relayoutCommandPanel);
    }
    onCompareModeChanged: {
        if (compareMode !== 0)
            singleViewShowSecondary = false;
        clearImageHover();
        Qt.callLater(relayoutCommandPanel);
    }
    onEditModeActiveChanged: {
        endPeek();
        clearImageHover();
        Qt.callLater(relayoutCommandPanel);
    }
    onHasPrimaryChanged: Qt.callLater(relayoutCommandPanel)
    onHasSecondaryChanged: Qt.callLater(relayoutCommandPanel)
    onActiveFocusChanged: {
        // Losing the keyboard (a TextField grabbed it, another pane took focus) must not
        // strand the workspace in a peek state the user can no longer release.
        if (!activeFocus)
            endPeek();
    }
    // A new image generation replaces both buffers under a stationary pointer; the pair
    // sample must not outlive the pixels it names. compareMode changes clear the hover
    // wholesale (existing behaviour), and the observation view is irrelevant to the
    // originals but refresh anyway so a stale sample can never survive a mode flip.
    onContentGenerationChanged: {
        endPeek();
        refreshPairSample();
    }
    onViewModeChanged: refreshPairSample()

    // A lazily instantiated workspace (video-first sessions build this tree only when the
    // image workspace activates) creates its Flows inside an already-sized parent chain:
    // each Flow lays out once at its width-0 creation state, and the re-layout Qt performs
    // on width changes is frame-driven — it never happens in a window that is not exposed
    // yet. Force the rows to lay out synchronously so the panel never keeps a stale
    // vertical layout: once here, where the widths settle during creation, on every
    // later width change (chrome margins, window resizes) through each row's handler,
    // and on content flips (rows, chips appearing and disappearing) through the
    // relayout hooks above — the Column itself must re-sum in every one of those
    // passes, or commandPanel.height stays at its zero creation value.
    Component.onCompleted: relayoutCommandPanel()

    function toggleSinglePairSource() {
        if (!canSwitchSingleSource)
            return;
        if (compareMode !== 0) {
            modeButton(0);
            singleViewShowSecondary = true;
        } else {
            singleViewShowSecondary = !singleViewShowSecondary;
        }
    }

    // ---- Single-side close -----------------------------------------------------------------
    //
    // The reason a close is currently refused, or "" when it is allowed. An edit session
    // with unsaved changes is the only refusal: the working copy lives in memory only and
    // the original file is never written, so dropping it here would lose the work without
    // saying so. The user is told to save a copy (or end the edit) instead.
    readonly property string closeSideBlockedReason: {
        if (!editModeActive)
            return "";
        if (!imageEdit || !imageEdit.dirty)
            return "";
        return qsTr("编辑副本有未保存修改：请先「另存副本…」或「结束编辑」，再关闭此图片。");
    }

    // Closes one side of the pair. Closing B keeps A; closing A keeps B (the controller
    // promotes the surviving side into the primary slot so its file identity, decoded
    // content and provenance stay correct); closing the last image reaches the empty state.
    function closeSide(sourceSlot) {
        if (!imageReview)
            return false;
        if (closeSideBlockedReason.length > 0) {
            showMarqueeNotice(closeSideBlockedReason);
            return false;
        }
        // A clean edit session has nothing to lose, so it is ended before the side goes.
        if (editModeActive)
            control.finishImageEdit();
        const isSecondary = Number(sourceSlot) === 3;
        const closed = isSecondary ? Boolean(imageReview.closeSecondary()) : Boolean(imageReview.closePrimary());
        if (!closed)
            return false;
        // A single-side close leaves a single image (or nothing). The pair-only modes and
        // the A/B side switch are already invalid, and the workspace-local side flag is
        // reset here so a stale "showing B" can never survive into the next pair.
        if (!hasPair) {
            singleViewShowSecondary = false;
            hoverPoint = null;
            pairSample = null;
        }
        endPeek();
        Qt.callLater(relayoutCommandPanel);
        control.closeSideRequested(isSecondary ? 3 : 2);
        return true;
    }

    function toggleAlphaView() {
        if (!imageReview || !hasPrimary)
            return;
        imageReview.viewMode = (imageReview.viewMode === 1 ? 0 : 1);
    }

    // COLOR4: one switch for the display-only colour conversion, used by the status-strip
    // badge. It never touches the analysis buffers or the source values.
    function toggleColorDisplay() {
        if (!imageReview)
            return;
        imageReview.colorDisplayEnabled = !imageReview.colorDisplayEnabled;
    }

    function toggleRgbOpaqueView() {
        if (!imageReview || !hasPrimary)
            return;
        imageReview.viewMode = (imageReview.viewMode === 2 ? 0 : 2);
    }

    // ---- Hold-to-peek -------------------------------------------------------------

    // Peek is offered exactly where the original plan needs it: the difference modes
    // (2/3/4/6) and fade (7). Everywhere else the plain in-place toggle (Space/T) already
    // shows originals.
    readonly property bool peekModeAvailable: Boolean(imageReview && hasPair && !editModeActive && (diffModeActive || compareMode === 7))

    function beginPeek(showSecondary) {
        if (peekActive || !peekModeAvailable)
            return;
        peekSnapshot = {
            "compareMode": Number(imageReview.compareMode),
            "singleViewShowSecondary": singleViewShowSecondary,
            "viewMode": Number(imageReview.viewMode),
            "diffGain": Number(imageReview.diffGain),
            "fadePosition": Number(imageReview.fadePosition)
        };
        peekSource = showSecondary ? 1 : 0;
        peekActive = true;
        singleViewShowSecondary = showSecondary;
        // compareMode is restored before singleViewShowSecondary below, and the
        // onCompareModeChanged handler only clears the flag for non-zero modes.
        imageReview.compareMode = 0;
    }

    function endPeek() {
        if (!peekActive || !peekSnapshot || !imageReview)
            return;
        const snapshot = peekSnapshot;
        peekSnapshot = null;
        peekActive = false;
        // A peek that ended because the pair went away cannot restore a pair-only mode;
        // fall back to the neutral single view for that case.
        imageReview.compareMode = hasPair ? snapshot.compareMode : 0;
        singleViewShowSecondary = hasPair ? snapshot.singleViewShowSecondary : false;
        imageReview.viewMode = snapshot.viewMode;
        imageReview.diffGain = snapshot.diffGain;
        imageReview.fadePosition = snapshot.fadePosition;
    }

    // Z and double-click share one verb: flip between fit-window and true 1:1 pixels.
    function toggleTrueSize() {
        if (trueSize) {
            trueSize = false;
            if (imageReview)
                imageReview.resetView();
        } else {
            trueSize = true;
            if (imageReview)
                imageReview.setZoom(1.0);
        }
    }

    // ---- Same-coordinate pair sampling ----------------------------------------------

    // Samples both committed decode buffers at the hovered normalized coordinate through
    // samplePixelOriginal, so the active channel observation view never repaints the pair
    // numbers and the difference image can never leak into an A/B readout. "Same
    // coordinate" is only claimed when both sides share one pixel grid: unequal sizes keep
    // their own grids, and a resampled difference does not relocate B's pixels, so the
    // conservative honest readout names the state instead of inventing a correspondence.
    function refreshPairSample() {
        if (!imageReview || !hasPair || editModeActive || !hoverPoint) {
            pairSample = null;
            return;
        }
        const pw = Number(imageReview.primaryWidth);
        const ph = Number(imageReview.primaryHeight);
        const sw = Number(imageReview.secondaryWidth);
        const sh = Number(imageReview.secondaryHeight);
        if (!(pw > 0) || !(ph > 0) || !(sw > 0) || !(sh > 0)) {
            pairSample = null;
            return;
        }
        const a = imageReview.samplePixelOriginal(2, hoverPoint.normX * pw, hoverPoint.normY * ph);
        let b = null;
        if (!sizesDiffer)
            b = imageReview.samplePixelOriginal(3, hoverPoint.normX * sw, hoverPoint.normY * sh);
        else
            b = {
                "unaligned": true
            };
        pairSample = {
            "a": a,
            "b": b
        };
    }

    // The sample the floating coordinate card names in verbose mode. Each slot gets its own
    // side's value from the pair sample, so the two side-by-side cards show two different
    // (and individually correct) numbers on an asymmetric pair. Without a pair, the hovered
    // single image's own original readout is the only honest value.
    function crosshairSampleForSlot(slot) {
        if (editModeActive)
            return null;
        if (pairSample) {
            if (slot === 3)
                return (pairSample.b && pairSample.b.valid) ? pairSample.b : null;
            return (pairSample.a && pairSample.a.valid) ? pairSample.a : null;
        }
        if (slot === 4)
            return null;
        if (cursorPixel && cursorPixel.valid && cursorPixel.source === "original")
            return cursorPixel;
        return null;
    }

    // Identity label for the detailed floating card. Original samples name their side;
    // a sample that passed a derived channel view is labelled as an observation and never
    // presented as original RGBA (the source/channelView semantics from the controller).
    function crosshairCardLabel(slot, sample) {
        if (!sample)
            return "";
        if (sample.channelView === "alphaGray")
            return qsTr("观察值 · Alpha 灰度");
        if (sample.channelView === "rgbOpaque")
            return qsTr("观察值 · RGB 忽略透明");
        if (sample.source === "diff")
            return qsTr("差异像素");
        return slot === 3 ? qsTr("B 原图") : qsTr("A 原图");
    }

    // ---- Numeric presentation ------------------------------------------------------------
    //
    // One switch, two meanings, zero effect on the pixels. Integer mode prints the code
    // value the buffer actually holds; normalized float mode prints it against that
    // buffer's own full scale (channel/255 for the 8-bit display buffer, channel16/65535
    // for the converted 16-bit sidecar). Neither form is linear light, HDR or colour
    // managed, and Hex stays hexadecimal in both modes. The controller owns the exact
    // text so the readout, the tooltip and the clipboard can never disagree.
    //
    // Binding dependency: the typed invokables below read numericFormat inside C++, and
    // invokable state reads are invisible to QML's dependency tracking. Every textual
    // helper therefore reads `control.numericFloat` first (the const name documents why
    // it exists); a binding that shows formatted text then re-evaluates when the format
    // flips even though the sample itself is unchanged.
    readonly property bool numericFloat: imageReview.numericFormat === 1

    function setNumericFormat(format) {
        imageReview.numericFormat = Number(format);
    }

    function channelText(value) {
        // Read, not consumed: this is the caller binding's subscription to the format
        // switch (see the section comment). The controller formats the value itself.
        const bindingDependency = control.numericFloat;
        return imageReview.channelText(Number(value));
    }

    function deltaText(low, high) {
        const bindingDependency = control.numericFloat;
        return imageReview.deltaText(Number(low), Number(high));
    }

    // Fixed-width monospace channel text: spaces pad to three columns in integer mode and
    // to eight in float mode (a normalized value is always "0.047059"), so a value 1 -> 255
    // or a coordinate gaining a digit reshapes nothing - the following columns keep their
    // start and the row never jitters while the pointer moves.
    function fmtChannel(value) {
        const text = channelText(value);
        const columns = control.numericFloat ? 8 : 3;
        return text.length >= columns ? text : " ".repeat(columns - text.length) + text;
    }

    // Signed delta text. The sign is part of the meaning (Δ = B − A), so a negative delta
    // is never clipped to zero in either mode and a zero delta never reads "-0.000000".
    function fmtSigned(low, high) {
        return deltaText(low, high);
    }

    readonly property int channelColumnWidth: control.numericFloat ? 62 : 30
    readonly property int deltaColumnWidth: control.numericFloat ? 72 : 38
    // Give the readout the whole available row; wrap only when the window actually fills.

    // A channel delta is highlighted when it exceeds the display quantization floor
    // (1 step of the RGBA8 buffer, or 1/65535 when the pair is read through the converted
    // 16-bit sidecar). The image pipeline has no other threshold: statistics are raw
    // 8-bit deltas and the gain is display-only, so this constant is the readout's
    // own documented limit, not a replay of some pipeline setting.
    readonly property int pairDeltaWarnThreshold: 1

    // The A|B|Δ(B−A) status-bar sentence, plain variant (tests, tooltips).
    function pairReadoutText() {
        const sample = readoutPairSample;
        if (!sample)
            return "";
        const aOk = Boolean(sample.a && sample.a.valid);
        let text = "";
        if (aOk)
            text = qsTr("A (%1, %2): R %3 G %4 B %5 A %6").arg(sample.a.x).arg(sample.a.y).arg(fmtChannel(sample.a.r)).arg(fmtChannel(sample.a.g)).arg(fmtChannel(sample.a.b)).arg(fmtChannel(sample.a.a));
        else
            text = qsTr("A: 无有效值");
        if (sample.b && sample.b.unaligned) {
            text += qsTr("  |  B: 未对齐无有效值（尺寸不同）");
            return text;
        }
        const bOk = Boolean(sample.b && sample.b.valid);
        if (!bOk) {
            text += qsTr("  |  B: 无有效值");
            return text;
        }
        text += qsTr("  |  B (%1, %2): R %3 G %4 B %5 A %6").arg(sample.b.x).arg(sample.b.y).arg(fmtChannel(sample.b.r)).arg(fmtChannel(sample.b.g)).arg(fmtChannel(sample.b.b)).arg(fmtChannel(sample.b.a));
        if (aOk) {
            // Δ = B − A, computed from the same code values the columns show, so switching
            // to the normalized format moves the decimal point of both sides and of the
            // difference without changing what the difference means.
            text += qsTr("  |  Δ(B−A): R %1 G %2 B %3 A %4").arg(fmtSigned(sample.a.r, sample.b.r)).arg(fmtSigned(sample.a.g, sample.b.g)).arg(fmtSigned(sample.a.b, sample.b.b)).arg(fmtSigned(sample.a.a, sample.b.a));
        }
        return text;
    }

    // ---- Frozen probe sample & colour copy ------------------------------------------------
    //
    // The pointer leaves the pixel long before the user reaches the button, so the sample
    // that gets copied is the last valid one of the current material generation, held by
    // the controller. It is not re-sampled at click time (the pointer is elsewhere by
    // then), it is not the screen colour under the button, and it dies with the generation
    // that produced it - a closed, replaced or re-opened side invalidates it.

    readonly property var probeSample: imageReview.frozenProbeSample
    readonly property bool probeSampleReady: Boolean(probeSample && probeSample.valid === true)
    readonly property var statusPixel: cursorPixel && cursorPixel.valid ? cursorPixel : (probeSampleReady ? probeSample : ({}))
    // The crosshair follows the live pointer. The status bar keeps the last valid coordinate
    // of this material generation so moving to its controls does not remove the numbers.
    readonly property var readoutPairSample: {
        if (pairSample)
            return pairSample;
        if (!hasPair || editModeActive || !probeSampleReady)
            return null;
        const sample = probeSample;
        return {
            "a": imageReview.samplePixelOriginal(2, sample.x, sample.y),
            "b": sizesDiffer ? {
                "unaligned": true
            } : imageReview.samplePixelOriginal(3, sample.x, sample.y)
        };
    }

    // Names the material, the coordinate and the value the copy will carry, so the user can
    // see what they are about to copy before they choose a format. A pair always names A or
    // B; nothing is guessed from "the main side".
    readonly property string probeSampleLabel: {
        const sample = control.probeSample;
        if (!control.probeSampleReady)
            return qsTr("无有效样本：把指针移到图片像素上");
        const side = String(sample.side || "A");
        const text = qsTr("%1 原图 · 像素 (%2, %3) · %4").arg(side).arg(sample.x).arg(sample.y).arg(control.rgbText(sample.r, sample.g, sample.b));
        return sample.alphaHidden === true ? text + qsTr(" · 已被当前 Alpha 阈值隐藏") : text;
    }

    function rgbText(r, g, b) {
        const bindingDependency = control.numericFloat;
        return imageReview.rgbText(r, g, b);
    }

    function captureProbeSample(slot, x, y) {
        imageReview.captureProbeSample(slot, x, y);
    }

    function copyProbeColor(format) {
        // Business conditions only: the typed dependencies are non-null in every real
        // session (the composition root owns them); "no sample yet" is a user state.
        if (!control.probeSampleReady) {
            showMarqueeNotice(qsTr("没有可复制的颜色样本：请先把指针移到图片像素上。"));
            return false;
        }
        const text = String(imageReview.probeColorText(Number(format)) || "");
        if (text.length === 0) {
            showMarqueeNotice(qsTr("没有可复制的颜色样本。"));
            return false;
        }
        // The adapter verifies its own write by reading the value back, so a failure can
        // never be reported as a success here (and an absent GUI clipboard is reported
        // through its status text below).
        const copied = Boolean(colorClipboard.copyText(text));
        showMarqueeNotice(copied ? qsTr("已复制 %1").arg(text) : qsTr("复制失败：%1").arg(String(colorClipboard.statusText || "")));
        return copied;
    }

    // ---- Alpha threshold observation filter -----------------------------------------------

    readonly property bool alphaFilterEnabled: Boolean(imageReview && imageReview.alphaFilterEnabled === true)
    readonly property int alphaThreshold: imageReview ? Number(imageReview.alphaThreshold) : 0
    // The filter only changes the picture in the "RGB ignoring alpha" observation. Every
    // other mode (alpha gray, the difference images, the edit working copy) keeps its own
    // explicit rule, and the status badge says so instead of applying something undefined.
    readonly property bool alphaFilterApplies: Boolean(control.alphaFilterEnabled && control.viewMode === 2)
    readonly property string alphaFilterBadgeText: {
        if (!control.alphaFilterEnabled)
            return "";
        if (!control.alphaFilterApplies)
            return qsTr("Alpha 过滤未应用（当前不是 RGB 忽略透明度）");
        if (control.alphaThreshold >= 255)
            return qsTr("Alpha 过滤 A > 255：无像素通过（有效过滤结果）");
        return qsTr("Alpha 过滤 A > %1").arg(control.alphaThreshold);
    }

    function setAlphaFilterEnabled(enabled) {
        if (!imageReview)
            return;
        if (enabled) {
            imageReview.viewMode = 2;
            if (diffModeActive || compareMode === 7)
                modeButton(hasPair ? 1 : 0);
        }
        imageReview.alphaFilterEnabled = Boolean(enabled);
    }

    function setAlphaThreshold(threshold) {
        alphaThresholdCommit.stop();
        if (imageReview)
            imageReview.alphaThreshold = Math.round(Number(threshold));
    }

    function resetAlphaThreshold() {
        control.setAlphaThreshold(0);
        control.setAlphaFilterEnabled(false);
    }

    // ---- Marquee feedback -------------------------------------------------------------

    function showMarqueeNotice(text) {
        marqueeNoticeText = text;
        marqueeNoticeTimer.restart();
    }

    // ---- Drag-in image filter ---------------------------------------------------------

    // One dragged URL counts as a replaceable image only when its extension matches the
    // open dialogs' still-image filter. Multi-file and non-image drags are rejected by the
    // chips so the workspace-level drop semantics keep them.
    readonly property var imageFileExtensions: ["png", "jpg", "jpeg", "bmp", "gif", "webp", "tif", "tiff", "pnm", "ppm", "pgm", "pbm", "pam"]

    function isImageFileUrl(fileUrl) {
        const path = String(fileUrl);
        const query = Math.max(path.indexOf("?"), path.indexOf("#"));
        const stem = query >= 0 ? path.slice(0, query) : path;
        const dot = stem.lastIndexOf(".");
        if (dot < 0)
            return false;
        const ext = stem.slice(dot + 1).toLowerCase();
        return imageFileExtensions.indexOf(ext) >= 0;
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
    // (0.5 = centered). Pan is normalised to the drawn image, so at fit zoom the image is
    // smaller than the viewport and any offset would slide it off-centre with empty canvas
    // behind it. Clamp the origin: a smaller image stays centred, a larger one may only move
    // as far as its edges still cover the viewport.
    function imageDrawEdge(containerSize, drawSize, pan) {
        const centered = (containerSize - drawSize) / 2;
        if (drawSize <= containerSize)
            return centered;
        return Math.min(0, Math.max(containerSize - drawSize, centered + (0.5 - pan) * drawSize));
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

    function clearImageHover() {
        if (imageReview)
            imageReview.clearCursorPixel();
        control.hoverPoint = null;
        control.pairSample = null;
    }

    function applyHover(view, mouseX, mouseY, slot) {
        if (!imageReview)
            return;
        const img = view.image;
        if (slot < 0 || !img || img.status !== Image.Ready) {
            clearImageHover();
            return;
        }
        const mapped = mapToImage(view, mouseX, mouseY);
        if (!mapped || mapped.x < 0 || mapped.y < 0 || mapped.x >= img.sourceSize.width || mapped.y >= img.sourceSize.height) {
            clearImageHover();
            return;
        }
        // Editing changes the image's origin and pixels. Never label a sample from the
        // immutable review original as a value from the displayed working copy.
        if (control.editModeActive) {
            imageReview.clearCursorPixel();
            imageReview.clearFrozenProbeSample();
        } else {
            imageReview.updateCursorPixel(slot, mapped.x, mapped.y);
            // Freeze the same coordinate as the copy source: the controller keeps it after
            // the pointer leaves, and drops it when the material generation changes.
            control.captureProbeSample(slot, mapped.x, mapped.y);
        }
        control.hoverPoint = {
            "sourceSlot": slot,
            "normX": mapped.x / img.sourceSize.width,
            "normY": mapped.y / img.sourceSize.height,
            "imgX": Math.floor(mapped.x),
            "imgY": Math.floor(mapped.y)
        };
        control.refreshPairSample();
    }

    // ---- Step-3 image editing -------------------------------------------------------
    // The session always starts from the committed original of the side on screen; the
    // original buffer and the source file are never written to, and edits are saved as a
    // copy. Crop geometry is captured in image pixels (mapToImage), so zoom or pan cannot
    // drift the selection.

    function editDisplayedSlot() {
        return (compareMode === 0 && hasPair && singleViewShowSecondary) ? 3 : 2;
    }

    // Label of the 「更多工具」dropdown. It names the active tool only when that tool lives
    // inside the dropdown — a tool that already has its own button keeps the plain label, so
    // the row never shows two buttons with the same name.
    function imageEditToolLabel(tool) {
        switch (String(tool || "")) {
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

    function applyImageMirror(vertical) {
        if (!imageEdit)
            return false;
        const applied = Boolean(imageEdit.mirrorImage(vertical));
        if (applied && imageReview)
            imageReview.resetView();
        return applied;
    }

    function applyImageRotation(turns) {
        if (!imageEdit)
            return false;
        const applied = Boolean(imageEdit.rotateImage(turns));
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
        } else if (control.editTool === "smart_select") {
            applied = Boolean(imageEdit.smartSegmentRect(selection.x, selection.y, selection.width, selection.height, control.smartSegmentTightCrop));
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

    // ---- Crop ratio lock & direct size entry -----------------------------------------

    // Reshape one selection rectangle to the locked aspect ratio inside the image bounds.
    // leadByWidth names the dimension the drag/input drives; the other follows. The mutual
    // clamp (a height cap that trims the width that then trims the height again) converges
    // within a couple of passes; the loop guard keeps a pathological bound from spinning.
    function cropSelectionWithRatio(x, y, w, h, leadByWidth) {
        const free = !(cropRatioLock > 0) || editTool !== "crop";
        const imgW = imageEdit ? Number(imageEdit.imageWidth) : 0;
        const imgH = imageEdit ? Number(imageEdit.imageHeight) : 0;
        if (free || !(imgW >= 2) || !(imgH >= 2)) {
            return {
                "x": x,
                "y": y,
                "width": Math.max(1, w),
                "height": Math.max(1, h)
            };
        }
        x = Math.max(0, Math.min(Math.round(x), imgW - 2));
        y = Math.max(0, Math.min(Math.round(y), imgH - 2));
        let width = Math.max(2, Math.min(Math.round(w), imgW - x));
        let height = Math.max(2, Math.min(Math.round(h), imgH - y));
        for (let guard = 0; guard < 4; ++guard) {
            if (leadByWidth) {
                const want = Math.max(2, Math.round(width / cropRatioLock));
                if (want <= imgH - y) {
                    height = want;
                    break;
                }
                height = Math.max(2, imgH - y);
                const nextWidth = Math.max(2, Math.min(imgW - x, Math.round(height * cropRatioLock)));
                if (nextWidth === width)
                    break;
                width = nextWidth;
            } else {
                const want = Math.max(2, Math.round(height * cropRatioLock));
                if (want <= imgW - x) {
                    width = want;
                    break;
                }
                width = Math.max(2, imgW - x);
                const nextHeight = Math.max(2, Math.min(imgH - y, Math.round(width / cropRatioLock)));
                if (nextHeight === height)
                    break;
                height = nextHeight;
            }
        }
        return {
            "x": x,
            "y": y,
            "width": width,
            "height": height
        };
    }

    // A newly picked ratio reshapes the existing selection in place (width leads, anchored
    // at the current origin) so the chips are one-click, not "pick then drag again".
    function applyCropRatioToSelection() {
        if (!cropSelection || !imageEdit || !(cropRatioLock > 0))
            return;
        const shaped = cropSelectionWithRatio(cropSelection.x, cropSelection.y, cropSelection.width, cropSelection.height, true);
        cropSelection = {
            "x": shaped.x,
            "y": shaped.y,
            "width": shaped.width,
            "height": shaped.height,
            "fromX": shaped.x,
            "fromY": shaped.y,
            "toX": shaped.x + shaped.width,
            "toY": shaped.y + shaped.height
        };
    }

    // Direct W/H entry: the edited side leads; a locked ratio pushes the other side, and
    // both clamp inside the image (keeping the selection origin).
    function applyCropSizeFromField(which, value) {
        if (!cropSelection || !imageEdit)
            return;
        const parsed = Math.round(Number(value));
        if (!(parsed >= 1))
            return;
        const shaped = cropSelectionWithRatio(cropSelection.x, cropSelection.y, which === "width" ? parsed : cropSelection.width, which === "height" ? parsed : cropSelection.height, which === "width");
        cropSelection = {
            "x": shaped.x,
            "y": shaped.y,
            "width": shaped.width,
            "height": shaped.height,
            "fromX": shaped.x,
            "fromY": shaped.y,
            "toX": shaped.x + shaped.width,
            "toY": shaped.y + shaped.height
        };
        control.forceActiveFocus();
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
        let m0 = mapToImage(view, view.cropStart.x, view.cropStart.y);
        let m1 = mapToImage(view, view.cropCurrent.x, view.cropCurrent.y);
        if (!m0 || !m1)
            return;
        if (control.editTool !== "arrow") {
            // Clip both endpoints before computing the extent. Clipping only x/y counts
            // letterbox pixels in the width/height, especially after a first crop.
            const width = imageEdit.imageWidth;
            const height = imageEdit.imageHeight;
            m0 = {
                "x": Math.max(0, Math.min(width, m0.x)),
                "y": Math.max(0, Math.min(height, m0.y))
            };
            m1 = {
                "x": Math.max(0, Math.min(width, m1.x)),
                "y": Math.max(0, Math.min(height, m1.y))
            };
        }
        const deltaX = m1.x - m0.x;
        const deltaY = m1.y - m0.y;
        const isArrow = control.editTool === "arrow";
        const tooSmall = isArrow ? Math.hypot(deltaX, deltaY) < 3 : (Math.abs(deltaX) < 2 || Math.abs(deltaY) < 2);
        if (tooSmall) {
            control.cropSelection = null;
            return;
        }
        let box = {
            "x": Math.max(0, Math.round(Math.min(m0.x, m1.x))),
            "y": Math.max(0, Math.round(Math.min(m0.y, m1.y))),
            "width": Math.max(1, Math.round(Math.abs(deltaX))),
            "height": Math.max(1, Math.round(Math.abs(deltaY)))
        };
        if (control.editTool === "crop" && control.cropRatioLock > 0)
            box = control.cropSelectionWithRatio(box.x, box.y, box.width, box.height, Math.abs(deltaX) >= Math.abs(deltaY));
        control.cropSelection = {
            "fromX": Math.round(m0.x),
            "fromY": Math.round(m0.y),
            "toX": Math.round(m1.x),
            "toY": Math.round(m1.y),
            "x": box.x,
            "y": box.y,
            "width": box.width,
            "height": box.height
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
        property int chipWidth: Math.max(64, contentItem.implicitWidth + 24)
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
            border.width: chip.visualFocus ? 2 : 0
            border.color: Theme.focus

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationFast
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

    // A/B identity chip for the source row. A plain click is the direct look of that side
    // (single view switched to A or B); one dropped still image replaces exactly this slot
    // through the URL-carrying signal, mirroring the video strip's drop contract. Built on
    // ReviewActionButton so the keyboard focus ring follows the visualFocus convention.
    component ImageSourceChip: ReviewActionButton {
        id: sourceChip

        required property int sourceSlot
        property string chipTitle: ""
        property string chipPath: ""
        readonly property bool isSecondary: sourceSlot === 3
        // The chip highlights itself while the stage shows exactly this side.
        readonly property bool isDisplayed: control.hasPair && control.compareMode === 0 && (isSecondary === control.singleViewShowSecondary)
        readonly property color sourceAccent: isSecondary ? Theme.sourceB : Theme.sourceA
        readonly property color sourceBorder: isSecondary ? Theme.sourceBBorder : Theme.sourceABorder
        readonly property bool dropHovered: chipDropArea.containsDrag

        signal directLookRequested
        signal replaceDropped(url url)
        // I0x "右键关闭此图片": the chip's right-click menu asks the workspace to close
        // exactly this side. The signal carries no index and no filename - the chip is the
        // side's own component, so the target can never drift to the other slot.
        signal closeRequested

        implicitHeight: 30
        implicitWidth: Math.min(220, Math.max(120, sourceChipLabel.implicitWidth + 34))
        leftPadding: Theme.spaceMain
        rightPadding: Theme.spaceMain
        focusTarget: control
        enabled: !control.editModeActive

        onClicked: directLookRequested()

        // Right-click on the filename label opens this side's close menu. The handler only
        // accepts the right button, so the left-button activation above (the direct look)
        // never fires first: no view switch, no A/B flip and no drag can happen before the
        // menu appears. It is also invisible to the chip's own Button handling, which only
        // reacts to the left button.
        TapHandler {
            id: chipContextHandler

            acceptedButtons: Qt.RightButton
            gesturePolicy: TapHandler.DragThreshold

            onTapped: function (eventPoint, button) {
                sourceChip.openCloseMenu(eventPoint.position);
            }
        }

        function openCloseMenu(position) {
            closeMenu.popup(sourceChip, position);
            return true;
        }

        contentItem: Row {
            id: sourceChipLabel

            spacing: Theme.spaceMicro
            leftPadding: 0

            Rectangle {
                width: 18
                height: 18
                radius: Theme.radiusMicro
                color: sourceChip.isDisplayed ? sourceChip.sourceAccent : Theme.fluentHover
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    anchors.centerIn: parent
                    text: sourceChip.isSecondary ? "B" : "A"
                    color: sourceChip.isDisplayed ? Theme.sourceInk : sourceChip.sourceAccent
                    font.pixelSize: Theme.fontSecondary
                    font.weight: Font.Bold
                }
            }

            Text {
                text: sourceChip.chipTitle
                color: sourceChip.enabled ? (sourceChip.isDisplayed ? Theme.primaryText : Theme.secondaryText) : Theme.disabledText
                font.pixelSize: Theme.fontSecondary
                elide: Text.ElideMiddle
                width: Math.min(implicitWidth, 150)
                anchors.verticalCenter: parent.verticalCenter

                HoverHandler {
                    id: chipTitleHover
                }

                VcsToolTip {
                    visible: chipTitleHover.hovered && sourceChip.chipPath.length > 0
                    text: sourceChip.chipPath
                }
            }
        }

        background: Rectangle {
            radius: Theme.radiusLarge
            color: {
                if (!sourceChip.enabled)
                    return "transparent";
                if (sourceChip.isDisplayed)
                    return sourceChip.isSecondary ? Theme.sourceBBackground : Theme.sourceABackground;
                if (sourceChip.down)
                    return Theme.fluentPressed;
                if (sourceChip.hovered)
                    return Theme.fluentHover;
                return Theme.raisedPanel;
            }
            border.width: 1
            border.color: sourceChip.isDisplayed || sourceChip.hovered ? sourceChip.sourceBorder : Theme.controlBorder
        }

        // One still image replaces this side. Multi-URL and non-image drags are rejected so
        // they keep the workspace-level open/append drop semantics.
        DropArea {
            id: chipDropArea

            objectName: (sourceChip.isSecondary ? "imageSourceChipDropAreaB" : "imageSourceChipDropAreaA")
            anchors.fill: parent
            onEntered: drag => {
                if (!drag.hasUrls || drag.urls.length !== 1 || !control.isImageFileUrl(drag.urls[0]))
                    drag.accepted = false;
            }
            onDropped: drop => {
                drop.acceptProposedAction();
                sourceChip.replaceDropped(drop.urls[0]);
            }
        }

        // I0x: this side's close menu. The same VcsMenu component the rest of the shell
        // uses, so the popup, focus and keyboard behaviour stay one contract. Every row is
        // addressed through the chip that owns the side, never through a display position.
        VcsMenu {
            id: closeMenu

            objectName: (sourceChip.isSecondary ? "imageSourceChipCloseMenuB" : "imageSourceChipCloseMenuA")
            // Closing is only meaningful while this side actually holds an image.
            enabled: sourceChip.isSecondary ? control.hasSecondary : control.hasPrimary

            onClosed: {
                // The menu took the keyboard while it was open; hand it back to the
                // workspace so Space/T and the other review shortcuts work immediately.
                Qt.callLater(() => control.forceActiveFocus());
            }

            VcsMenuItem {
                objectName: (sourceChip.isSecondary ? "imageCloseSecondaryAction" : "imageClosePrimaryAction")
                text: sourceChip.isSecondary ? qsTr("关闭此图片（B）") : qsTr("关闭此图片（A）")
                enabled: closeMenu.enabled
                onTriggered: {
                    closeMenu.close();
                    sourceChip.closeRequested();
                }
            }
            VcsMenuItem {
                objectName: (sourceChip.isSecondary ? "imageCloseSecondaryReason" : "imageClosePrimaryReason")
                // Never leave the user without a reason: an edit session with unsaved
                // changes refuses the close instead of dropping the work silently.
                visible: control.closeSideBlockedReason.length > 0
                enabled: false
                text: control.closeSideBlockedReason
                contentItem: Text {
                    text: control.closeSideBlockedReason
                    wrapMode: Text.Wrap
                    color: Theme.mutedText
                    font.pixelSize: Theme.fontSecondary
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Rectangle {
            objectName: (sourceChip.isSecondary ? "imageSourceChipDropHintB" : "imageSourceChipDropHintA")
            visible: sourceChip.dropHovered
            anchors.fill: parent
            radius: Theme.radiusLarge
            color: Theme.dropScrim
            border.color: sourceChip.sourceBorder
            border.width: 2

            Text {
                anchors.centerIn: parent
                text: qsTr("松开替换 %1").arg(sourceChip.isSecondary ? "B" : "A")
                color: Theme.primaryText
                font.pixelSize: Theme.fontSecondary
                font.weight: Font.DemiBold
            }
        }
    }

    // Glass chip for the bottom-left view-command HUD: the same material and metrics as the
    // video viewport's viewCommandRow so both workspaces keep one muscle memory.
    component HudCommandButton: ReviewActionButton {
        id: hudButton

        Accessible.name: text
        Accessible.description: helpText
        width: hudLabel.implicitWidth + 16
        // The chip itself is the receiver, so it carries the full hit-area minimum
        // (HUD view commands and the gain steps are frequent pointer targets).
        height: Theme.hitAreaMinimum
        leftPadding: 8
        rightPadding: 8
        topPadding: 0
        bottomPadding: 0
        focusTarget: control

        contentItem: Text {
            id: hudLabel

            text: hudButton.text
            color: !hudButton.enabled ? Theme.disabledText : (hudButton.checked || hudButton.hovered ? Theme.primaryText : Theme.mutedText)
            font.pixelSize: Theme.fontCaption
            font.weight: hudButton.checked ? Font.DemiBold : Font.Normal
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }

        background: Rectangle {
            radius: Theme.radiusMicro
            color: hudButton.checked ? Theme.controlChecked : (hudButton.hovered ? Theme.oscGlassHover : Theme.oscGlass)
            border.width: hudButton.visualFocus ? 2 : 1
            border.color: hudButton.visualFocus ? Theme.focus : Theme.oscBorder
        }
    }

    // Crosshair layer for the single-surface compare overlays (wipe/fade). The pointer
    // position is stored as a normalized coordinate over the shared primary-geometry box,
    // so the crosshair follows the same mapping on both overlays and the diff viewport.
    component OverlayCrosshair: Item {
        id: overlayCross

        required property Image imageItem
        required property string key

        anchors.fill: parent
        visible: Boolean(control.hoverPoint && imageItem.status === Image.Ready)
        z: 16

        readonly property real targetX: control.hoverPoint ? imageItem.x + control.hoverPoint.normX * imageItem.width : 0
        readonly property real targetY: control.hoverPoint ? imageItem.y + control.hoverPoint.normY * imageItem.height : 0
        readonly property real imageLeft: Math.max(0, imageItem.x)
        readonly property real imageTop: Math.max(0, imageItem.y)
        readonly property real imageRight: Math.min(width, imageItem.x + imageItem.width)
        readonly property real imageBottom: Math.min(height, imageItem.y + imageItem.height)
        readonly property var cardSample: control.crosshairSampleForSlot(control.hoverPoint ? control.hoverPoint.sourceSlot : 2)

        // Vertical hairline
        Rectangle {
            objectName: "imageCrosshairVertical-" + overlayCross.key
            x: Math.round(overlayCross.targetX)
            y: overlayCross.imageTop
            width: 1
            height: Math.max(0, overlayCross.imageBottom - overlayCross.imageTop)
            color: Theme.probeLineMuted

            Rectangle {
                x: -1
                width: parent.width + 2
                height: parent.height
                z: -1
                color: "#99000000"
            }
        }

        // Horizontal hairline
        Rectangle {
            objectName: "imageCrosshairHorizontal-" + overlayCross.key
            x: overlayCross.imageLeft
            y: Math.round(overlayCross.targetY)
            width: Math.max(0, overlayCross.imageRight - overlayCross.imageLeft)
            height: 1
            color: Theme.probeLineMuted

            Rectangle {
                y: -1
                width: parent.width
                height: parent.height + 2
                z: -1
                color: "#99000000"
            }
        }

        // Center pip at the probe point
        Rectangle {
            x: Math.round(overlayCross.targetX - 2)
            y: Math.round(overlayCross.targetY - 2)
            width: 4
            height: 4
            radius: 2
            color: Theme.probePip
        }

        // Coordinate card: concise "(x, y)", or detailed original-pixel readout (P).
        Rectangle {
            objectName: "imageCrosshairCoordinates-" + overlayCross.key
            x: Math.min(overlayCross.width - width - 8, Math.max(8, Math.round(overlayCross.targetX + 12)))
            y: Math.min(overlayCross.height - height - 8, Math.max(8, Math.round(overlayCross.targetY + 12)))
            width: overlayCrossCardRow.implicitWidth + 10
            height: control.crosshairVerbose ? 24 : 18
            radius: Theme.radiusMicro
            color: Theme.oscGlass
            border.color: Theme.probe
            border.width: 1

            Row {
                id: overlayCrossCardRow

                spacing: Theme.spaceMicro
                anchors.centerIn: parent

                Rectangle {
                    visible: control.crosshairVerbose && overlayCross.cardSample !== null
                    width: 14
                    height: 14
                    radius: 2
                    color: overlayCross.cardSample ? Qt.rgba(overlayCross.cardSample.r / 255, overlayCross.cardSample.g / 255, overlayCross.cardSample.b / 255, 1) : "transparent"
                    border.width: 1
                    border.color: Theme.probe
                    anchors.verticalCenter: parent.verticalCenter
                }

                Text {
                    objectName: "imageCrosshairCoordinateText-" + overlayCross.key
                    text: {
                        if (!control.hoverPoint)
                            return "";
                        const coords = "(%1, %2)".arg(Math.floor(control.hoverPoint.normX * (control.imageReview ? control.imageReview.primaryWidth : 0))).arg(Math.floor(control.hoverPoint.normY * (control.imageReview ? control.imageReview.primaryHeight : 0)));
                        if (!control.crosshairVerbose || !overlayCross.cardSample)
                            return coords;
                        const s = overlayCross.cardSample;
                        const hoverSlot = control.hoverPoint.sourceSlot;
                        // The card follows the same integer/float switch as the status bar, so
                        // the two can never disagree about what a number means.
                        return "%1 · %2 · R %3 G %4 B %5 A %6 · α %7%".arg(control.crosshairCardLabel(hoverSlot, s)).arg(coords).arg(control.channelText(s.r)).arg(control.channelText(s.g)).arg(control.channelText(s.b)).arg(control.channelText(s.a)).arg(s.alphaPercent);
                    }
                    color: Theme.probe
                    font.pixelSize: 10
                    font.family: Theme.fontFamilyMono
                    font.weight: Font.Bold
                    anchors.verticalCenter: parent.verticalCenter
                }
            }
        }
    }

    // UX-15: this workspace's split line was drag-only, while the video surface has had
    // Alt+arrows since E1. These mirror ReviewShortcuts one-for-one - 1 % steps, Shift for 5 %,
    // axis-gated - so the same chord moves the same control in both workspaces.
    //
    // They are application Shortcuts, not extra branches in the workspace key handler: arrow
    // keys never reach that handler because a focused child (canvas, list) consumes them for
    // its own navigation, which is exactly why the video side uses Shortcuts too. `W` stays in
    // the key handler - a bare letter there is what keeps text fields safe; an Alt chord has
    // no such competition, and edit mode (the only place this workspace has text entry) turns
    // these off wholesale.
    readonly property bool wipeKeysActive: control.visible && !control.editModeActive && !control.anyEditDialogOpen && control.compareMode === 5 && control.hasPair
    readonly property bool wipeHorizontal: control.imageReview ? control.imageReview.wipeOrientation === 1 : false

    function nudgeWipe(delta) {
        if (!control.imageReview)
            return;
        const current = Number(control.imageReview.wipePosition);
        control.imageReview.wipePosition = Math.max(0, Math.min(1, current + delta));
    }

    Shortcut {
        sequence: "Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && !control.wipeHorizontal
        onActivated: control.nudgeWipe(-0.01)
    }
    Shortcut {
        sequence: "Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && !control.wipeHorizontal
        onActivated: control.nudgeWipe(0.01)
    }
    Shortcut {
        sequence: "Shift+Alt+Left"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && !control.wipeHorizontal
        onActivated: control.nudgeWipe(-0.05)
    }
    Shortcut {
        sequence: "Shift+Alt+Right"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && !control.wipeHorizontal
        onActivated: control.nudgeWipe(0.05)
    }
    Shortcut {
        sequence: "Alt+Up"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && control.wipeHorizontal
        onActivated: control.nudgeWipe(-0.01)
    }
    Shortcut {
        sequence: "Alt+Down"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && control.wipeHorizontal
        onActivated: control.nudgeWipe(0.01)
    }
    Shortcut {
        sequence: "Shift+Alt+Up"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && control.wipeHorizontal
        onActivated: control.nudgeWipe(-0.05)
    }
    Shortcut {
        sequence: "Shift+Alt+Down"
        context: Qt.ApplicationShortcut
        enabled: control.wipeKeysActive && control.wipeHorizontal
        onActivated: control.nudgeWipe(0.05)
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
        // Hollow ring for "no reading yet". A filled dot is a claim; a ring is the absence of
        // one, so an unprobed/unapplied state stays distinguishable from an applied state at
        // 6 px without inventing a fourth hue next to the source-identity colours.
        property bool dotHollow: false

        height: Theme.hitAreaMinimum
        implicitWidth: badgeContent.implicitWidth + 20
        radius: Theme.radiusSubtle
        color: Theme.raisedPanel
        border.width: 1
        border.color: Theme.border

        Row {
            id: badgeContent

            anchors.centerIn: parent
            spacing: 6

            Rectangle {
                visible: badge.dotVisible && badge.glyph.length === 0
                width: 6
                height: 6
                radius: 3
                color: badge.dotHollow ? "transparent" : badge.dotColor
                border.width: badge.dotHollow ? 1 : 0
                border.color: badge.dotColor
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
                font.pixelSize: Theme.fontCaption
                anchors.verticalCenter: parent.verticalCenter
            }
        }
    }

    component CropHandle: Item {
        id: handleArea

        required property string handleMode
        required property Item targetViewport
        required property var overlayParent

        width: 22
        height: 22
        z: 32

        // Corner L-bracket decoration
        Item {
            anchors.fill: parent
            visible: handleArea.handleMode === "tl" || handleArea.handleMode === "tr" || handleArea.handleMode === "bl" || handleArea.handleMode === "br"

            Rectangle {
                width: 14
                height: 3
                color: "#ffffff"
                x: handleArea.handleMode.indexOf("l") !== -1 ? 10 : (parent.width - 10 - width)
                y: handleArea.handleMode.indexOf("t") !== -1 ? 10 : (parent.height - 10 - height)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -1
                    z: -1
                    color: "transparent"
                    border.color: "#80000000"
                    border.width: 1
                }
            }
            Rectangle {
                width: 3
                height: 14
                color: "#ffffff"
                x: handleArea.handleMode.indexOf("l") !== -1 ? 10 : (parent.width - 10 - width)
                y: handleArea.handleMode.indexOf("t") !== -1 ? 10 : (parent.height - 10 - height)

                Rectangle {
                    anchors.fill: parent
                    anchors.margins: -1
                    z: -1
                    color: "transparent"
                    border.color: "#80000000"
                    border.width: 1
                }
            }
        }

        // Edge horizontal pill (t, b)
        Rectangle {
            visible: handleArea.handleMode === "t" || handleArea.handleMode === "b"
            anchors.centerIn: parent
            width: 18
            height: 4
            radius: 2
            color: "#ffffff"

            Rectangle {
                anchors.fill: parent
                anchors.margins: -1
                radius: 2
                z: -1
                color: "transparent"
                border.color: "#80000000"
                border.width: 1
            }
        }

        // Edge vertical pill (l, r)
        Rectangle {
            visible: handleArea.handleMode === "l" || handleArea.handleMode === "r"
            anchors.centerIn: parent
            width: 4
            height: 18
            radius: 2
            color: "#ffffff"

            Rectangle {
                anchors.fill: parent
                anchors.margins: -1
                radius: 2
                z: -1
                color: "transparent"
                border.color: "#80000000"
                border.width: 1
            }
        }
    }

    component ImagePixelGrid: PixelGrid {
        required property Image imageItem
        required property Item viewportItem

        x: Math.max(0, imageItem.x)
        y: Math.max(0, imageItem.y)
        width: Math.max(0, Math.min(viewportItem.width, imageItem.x + imageItem.width) - x)
        height: Math.max(0, Math.min(viewportItem.height, imageItem.y + imageItem.height) - y)
        originX: imageItem.x - x
        originY: imageItem.y - y
        stepX: imageItem.sourceSize.width > 0 ? imageItem.width / imageItem.sourceSize.width : 0
        stepY: imageItem.sourceSize.height > 0 ? imageItem.height / imageItem.sourceSize.height : 0
        requested: control.pixelGridEnabled && imageItem.status === Image.Ready
        nearest: !imageItem.smooth
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
        readonly property bool isCropTarget: control.editModeActive && (viewport.slot === (control.editSourceSlot >= 0 ? control.editSourceSlot : control.editDisplayedSlot())) && (control.editTool === "crop" || control.editTool === "mosaic" || control.editTool === "smart_select")
        // Annotation drag gesture (select tool) and the text-click guard.
        property bool annotationDragActive: false

        readonly property alias image: previewImage
        // Fit scale of the displayed image (fit-window base), forwarded so the workspace
        // status bar can report the physical display percentage.
        readonly property real fitScale: previewImage ? previewImage.fitScale : 1

        function refreshHover() {
            if (viewport.visible && viewportMouseArea.containsMouse)
                control.applyHover(viewport, viewportMouseArea.mouseX, viewportMouseArea.mouseY, viewport.slot);
        }

        onVisibleChanged: Qt.callLater(viewport.refreshHover)
        onSlotChanged: Qt.callLater(viewport.refreshHover)

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
            radius: Theme.radiusSubtle
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
            // Coalesce geometry updates so a stationary pointer still describes the pixel
            // currently underneath it after zoom, pan, crop, rotate or undo.
            onXChanged: Qt.callLater(viewport.refreshHover)
            onYChanged: Qt.callLater(viewport.refreshHover)
            onWidthChanged: Qt.callLater(viewport.refreshHover)
            onHeightChanged: Qt.callLater(viewport.refreshHover)
            onSourceSizeChanged: Qt.callLater(viewport.refreshHover)
            onStatusChanged: Qt.callLater(viewport.refreshHover)
            onSourceChanged: {
                if (control.hoverPoint && control.hoverPoint.sourceSlot === viewport.slot)
                    control.clearImageHover();
                Qt.callLater(viewport.refreshHover);
            }
        }

        ImagePixelGrid {
            objectName: "imagePixelGrid-" + viewport.slot
            imageItem: previewImage
            viewportItem: viewport
        }

        // The title and the size/format readout sit on the image itself, so they get a
        // translucent plate: readable over bright or busy content, and it never takes canvas
        // input away from panning, marquee selection or tool strokes underneath.
        Rectangle {
            visible: viewport.label.length > 0 || viewport.title.length > 0
            width: Math.min(viewportLabelColumn.implicitWidth + 18, Math.max(200, viewport.width - 60))
            height: viewportLabelColumn.implicitHeight + 12
            radius: Theme.radiusSmall
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
                    font.pixelSize: Theme.fontCaption
                    elide: Text.ElideRight
                    width: Math.min(implicitWidth, Math.max(160, viewport.width - 76))
                }
            }
        }

        Item {
            id: syncedCrosshair
            objectName: "syncedCrosshair-" + viewport.slot
            anchors.fill: parent
            // Side-by-side keeps the mirrored reticle pair; mode 0 shows the hovered pane's
            // own crosshair; the difference viewport (slot 4) gets the same crosshair in the
            // diff modes so the probe coordinate stays visible while reading the difference.
            visible: Boolean(control.hoverPoint && previewImage.status === Image.Ready && ((control.compareMode === 1 && control.hasPair) || (control.compareMode === 0 && isHoveredSource) || (viewport.slot === 4 && control.diffModeActive && isHoveredSource)))
            z: 15

            readonly property real targetX: control.hoverPoint ? previewImage.x + control.hoverPoint.normX * previewImage.width : 0
            readonly property real targetY: control.hoverPoint ? previewImage.y + control.hoverPoint.normY * previewImage.height : 0
            readonly property bool isHoveredSource: Boolean(control.hoverPoint && control.hoverPoint.sourceSlot === viewport.slot)
            readonly property real imageLeft: Math.max(0, previewImage.x)
            readonly property real imageTop: Math.max(0, previewImage.y)
            readonly property real imageRight: Math.min(viewport.width, previewImage.x + previewImage.width)
            readonly property real imageBottom: Math.min(viewport.height, previewImage.y + previewImage.height)
            readonly property var cardSample: control.crosshairSampleForSlot(viewport.slot)

            // Vertical hairline
            Rectangle {
                objectName: "imageCrosshairVertical-" + viewport.slot
                x: Math.round(syncedCrosshair.targetX)
                y: syncedCrosshair.imageTop
                width: 1
                height: Math.max(0, syncedCrosshair.imageBottom - syncedCrosshair.imageTop)
                color: syncedCrosshair.isHoveredSource ? Theme.probeLineMuted : Theme.probeLine

                Rectangle {
                    x: -1
                    width: parent.width + 2
                    height: parent.height
                    z: -1
                    color: "#99000000"
                }
            }

            // Horizontal hairline
            Rectangle {
                objectName: "imageCrosshairHorizontal-" + viewport.slot
                x: syncedCrosshair.imageLeft
                y: Math.round(syncedCrosshair.targetY)
                width: Math.max(0, syncedCrosshair.imageRight - syncedCrosshair.imageLeft)
                height: 1
                color: syncedCrosshair.isHoveredSource ? Theme.probeLineMuted : Theme.probeLine

                Rectangle {
                    y: -1
                    width: parent.width
                    height: parent.height + 2
                    z: -1
                    color: "#99000000"
                }
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

            // The active single-image pane (and the difference viewport) names the pixel
            // under the probe. Side-by-side shows BOTH panes' cards: each names its own
            // side's original, so an asymmetric pair reads as two different (individually
            // correct) numbers instead of one pane staying unlabelled. P flips the card
            // between "(x, y)" and the detailed original-pixel readout with a swatch.
            Rectangle {
                objectName: "imageCrosshairCoordinates-" + viewport.slot
                visible: control.hoverPoint !== null && (control.compareMode === 1 || !syncedCrosshair.isHoveredSource || control.compareMode === 0 || viewport.slot === 4)
                x: Math.min(viewport.width - width - 8, Math.max(8, Math.round(syncedCrosshair.targetX + 12)))
                y: Math.min(viewport.height - height - 8, Math.max(8, Math.round(syncedCrosshair.targetY + 12)))
                width: coordCardRow.implicitWidth + 10
                height: control.crosshairVerbose ? 24 : 18
                radius: 3
                color: Theme.oscGlass
                border.color: Theme.probe
                border.width: 1

                Row {
                    id: coordCardRow

                    spacing: Theme.spaceMicro
                    anchors.centerIn: parent

                    Rectangle {
                        visible: control.crosshairVerbose && syncedCrosshair.cardSample !== null
                        width: 14
                        height: 14
                        radius: 2
                        color: syncedCrosshair.cardSample ? Qt.rgba(syncedCrosshair.cardSample.r / 255, syncedCrosshair.cardSample.g / 255, syncedCrosshair.cardSample.b / 255, 1) : "transparent"
                        border.width: 1
                        border.color: Theme.probe
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Text {
                        id: coordText
                        objectName: "imageCrosshairCoordinateText-" + viewport.slot
                        text: {
                            if (!control.hoverPoint)
                                return "";
                            const coords = "%1, %2".arg(Math.floor(control.hoverPoint.normX * previewImage.sourceSize.width)).arg(Math.floor(control.hoverPoint.normY * previewImage.sourceSize.height));
                            if (!control.crosshairVerbose || !syncedCrosshair.cardSample)
                                return coords;
                            // Detailed values name the sample's identity: the side's own
                            // original, or an observation value that a channel view converted.
                            // The numbers follow the status bar's integer/float switch, and a
                            // pixel the alpha threshold hides says so instead of looking like
                            // a value the filter changed.
                            const s = syncedCrosshair.cardSample;
                            return "%1 · %2 · R %3 G %4 B %5 A %6 · α %7%%8".arg(control.crosshairCardLabel(viewport.slot, s)).arg(coords).arg(control.channelText(s.r)).arg(control.channelText(s.g)).arg(control.channelText(s.b)).arg(control.channelText(s.a)).arg(s.alphaPercent).arg(s.alphaHidden === true ? qsTr(" · 已被当前 Alpha 阈值隐藏") : "");
                        }
                        color: Theme.probe
                        font.pixelSize: 10
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.Bold
                        anchors.verticalCenter: parent.verticalCenter
                    }
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

        // Generic rect drag for one-shot tools (fill, clear, rect, arrow)
        Rectangle {
            id: genericRectDrag
            objectName: "imageGenericRect-" + viewport.slot
            visible: viewport.cropActive && !viewport.isCropTarget
            z: 26
            x: Math.min(viewport.cropStart.x, viewport.cropCurrent.x)
            y: Math.min(viewport.cropStart.y, viewport.cropCurrent.y)
            width: Math.abs(viewport.cropCurrent.x - viewport.cropStart.x)
            height: Math.abs(viewport.cropCurrent.y - viewport.cropStart.y)
            color: Theme.cropFill
            border.color: Theme.cropBorder
            border.width: 1
        }

        // Comprehensive crop overlay with dark scrim, 8 drag handles, move pan, and mini action bar
        Item {
            id: cropOverlay
            objectName: "imageCropOverlay-" + viewport.slot
            anchors.fill: parent
            z: 28
            visible: viewport.isCropTarget && (viewport.cropActive || control.cropSelection !== null)

            property string activeDragMode: ""
            property point dragStartViewPoint: Qt.point(0, 0)
            property var initialCropSelection: null

            readonly property bool hasSelection: control.cropSelection !== null
            readonly property bool isDraggingNew: viewport.cropActive
            readonly property real imgScale: (previewImage && previewImage.sourceSize.width > 0) ? (previewImage.width / previewImage.sourceSize.width) : 1
            readonly property real imgLeft: previewImage ? previewImage.x : 0
            readonly property real imgTop: previewImage ? previewImage.y : 0
            readonly property real imgRight: previewImage ? (previewImage.x + previewImage.width) : 0
            readonly property real imgBottom: previewImage ? (previewImage.y + previewImage.height) : 0

            readonly property real boxLeft: isDraggingNew ? Math.min(viewport.cropStart.x, viewport.cropCurrent.x) : (hasSelection ? (previewImage.x + control.cropSelection.x * imgScale) : 0)
            readonly property real boxTop: isDraggingNew ? Math.min(viewport.cropStart.y, viewport.cropCurrent.y) : (hasSelection ? (previewImage.y + control.cropSelection.y * imgScale) : 0)
            readonly property real boxWidth: isDraggingNew ? Math.abs(viewport.cropCurrent.x - viewport.cropStart.x) : (hasSelection ? (control.cropSelection.width * imgScale) : 0)
            readonly property real boxHeight: isDraggingNew ? Math.abs(viewport.cropCurrent.y - viewport.cropStart.y) : (hasSelection ? (control.cropSelection.height * imgScale) : 0)

            function beginCropDrag(mode, mouseViewX, mouseViewY) {
                if (!control.cropSelection || !previewImage)
                    return;
                activeDragMode = mode;
                dragStartViewPoint = Qt.point(mouseViewX, mouseViewY);
                initialCropSelection = {
                    "x": Number(control.cropSelection.x),
                    "y": Number(control.cropSelection.y),
                    "width": Number(control.cropSelection.width),
                    "height": Number(control.cropSelection.height)
                };
            }

            function applyCropDrag(mode, mouseViewX, mouseViewY) {
                if (!initialCropSelection || !previewImage || !control.imageEdit)
                    return;
                const mapped = control.mapToImage(viewport, mouseViewX, mouseViewY);
                const startMapped = control.mapToImage(viewport, dragStartViewPoint.x, dragStartViewPoint.y);
                if (!mapped || !startMapped)
                    return;
                const imgW = Number(control.imageEdit.imageWidth);
                const imgH = Number(control.imageEdit.imageHeight);
                const deltaX = Math.round(mapped.x - startMapped.x);
                const deltaY = Math.round(mapped.y - startMapped.y);

                if (mode === "move") {
                    const newX = Math.max(0, Math.min(imgW - initialCropSelection.width, initialCropSelection.x + deltaX));
                    const newY = Math.max(0, Math.min(imgH - initialCropSelection.height, initialCropSelection.y + deltaY));
                    control.cropSelection = {
                        "x": newX,
                        "y": newY,
                        "width": initialCropSelection.width,
                        "height": initialCropSelection.height,
                        "fromX": newX,
                        "fromY": newY,
                        "toX": newX + initialCropSelection.width,
                        "toY": newY + initialCropSelection.height
                    };
                    return;
                }

                let x0 = initialCropSelection.x;
                let y0 = initialCropSelection.y;
                let x1 = initialCropSelection.x + initialCropSelection.width;
                let y1 = initialCropSelection.y + initialCropSelection.height;

                if (mode.indexOf("l") !== -1)
                    x0 = Math.max(0, Math.min(initialCropSelection.x + initialCropSelection.width - 2, initialCropSelection.x + deltaX));
                if (mode.indexOf("r") !== -1)
                    x1 = Math.min(imgW, Math.max(initialCropSelection.x + 2, initialCropSelection.x + initialCropSelection.width + deltaX));
                if (mode.indexOf("t") !== -1)
                    y0 = Math.max(0, Math.min(initialCropSelection.y + initialCropSelection.height - 2, initialCropSelection.y + deltaY));
                if (mode.indexOf("b") !== -1)
                    y1 = Math.min(imgH, Math.max(initialCropSelection.y + 2, initialCropSelection.y + initialCropSelection.height + deltaY));

                // A locked ratio follows the dragged edge pair: left/right drags lead with
                // the width, top/bottom and corners with the larger intent (width).
                const ratioShaped = control.cropSelectionWithRatio(x0, y0, Math.max(2, x1 - x0), Math.max(2, y1 - y0), mode.indexOf("l") !== -1 || mode.indexOf("r") !== -1 || Math.abs(x1 - x0) >= Math.abs(y1 - y0));
                control.cropSelection = {
                    "x": ratioShaped.x,
                    "y": ratioShaped.y,
                    "width": ratioShaped.width,
                    "height": ratioShaped.height,
                    "fromX": ratioShaped.x,
                    "fromY": ratioShaped.y,
                    "toX": ratioShaped.x + ratioShaped.width,
                    "toY": ratioShaped.y + ratioShaped.height
                };
            }

            function endCropDrag() {
                activeDragMode = "";
                initialCropSelection = null;
            }

            // Top Scrim: strictly bounded within the image above the crop box
            Rectangle {
                objectName: "imageCropScrimTop-" + viewport.slot
                visible: cropOverlay.boxWidth > 2 && cropOverlay.boxHeight > 2
                x: cropOverlay.imgLeft
                y: cropOverlay.imgTop
                width: Math.max(0, cropOverlay.imgRight - cropOverlay.imgLeft)
                height: Math.max(0, cropOverlay.boxTop - cropOverlay.imgTop)
                color: "#73000000"
            }
            // Bottom Scrim: strictly bounded within the image below the crop box
            Rectangle {
                objectName: "imageCropScrimBottom-" + viewport.slot
                visible: cropOverlay.boxWidth > 2 && cropOverlay.boxHeight > 2
                x: cropOverlay.imgLeft
                y: cropOverlay.boxTop + cropOverlay.boxHeight
                width: Math.max(0, cropOverlay.imgRight - cropOverlay.imgLeft)
                height: Math.max(0, cropOverlay.imgBottom - (cropOverlay.boxTop + cropOverlay.boxHeight))
                color: "#73000000"
            }
            // Left Scrim: strictly bounded within the image on the left side of the crop box
            Rectangle {
                objectName: "imageCropScrimLeft-" + viewport.slot
                visible: cropOverlay.boxWidth > 2 && cropOverlay.boxHeight > 2
                x: cropOverlay.imgLeft
                y: cropOverlay.boxTop
                width: Math.max(0, cropOverlay.boxLeft - cropOverlay.imgLeft)
                height: Math.max(0, cropOverlay.boxHeight)
                color: "#73000000"
            }
            // Right Scrim: strictly bounded within the image on the right side of the crop box
            Rectangle {
                objectName: "imageCropScrimRight-" + viewport.slot
                visible: cropOverlay.boxWidth > 2 && cropOverlay.boxHeight > 2
                x: cropOverlay.boxLeft + cropOverlay.boxWidth
                y: cropOverlay.boxTop
                width: Math.max(0, cropOverlay.imgRight - (cropOverlay.boxLeft + cropOverlay.boxWidth))
                height: Math.max(0, cropOverlay.boxHeight)
                color: "#73000000"
            }

            // The Crop Bounding Box Item
            Item {
                id: cropBoxItem
                x: Math.round(cropOverlay.boxLeft)
                y: Math.round(cropOverlay.boxTop)
                width: Math.round(cropOverlay.boxWidth)
                height: Math.round(cropOverlay.boxHeight)
                visible: cropOverlay.boxWidth > 2 && cropOverlay.boxHeight > 2

                // Preserved objectName for backwards compatibility
                Rectangle {
                    id: cropRect
                    objectName: "imageCropRect-" + viewport.slot
                    anchors.fill: parent
                    color: cropOverlay.isDraggingNew ? Theme.cropFill : "transparent"
                    border.color: "#ffffff"
                    border.width: 1.5

                    // Outer dark border to ensure contrast on white pictures
                    Rectangle {
                        anchors.fill: parent
                        anchors.margins: -1
                        z: -1
                        color: "transparent"
                        border.color: "#66000000"
                        border.width: 1
                    }
                }

                // Rule-of-Thirds Grid
                Item {
                    id: ruleOfThirdsGrid
                    anchors.fill: parent
                    visible: parent.width > 36 && parent.height > 36

                    Rectangle {
                        x: 0
                        y: Math.round(parent.height / 3)
                        width: parent.width
                        height: 1
                        color: "#40ffffff"
                    }
                    Rectangle {
                        x: 0
                        y: Math.round(parent.height * 2 / 3)
                        width: parent.width
                        height: 1
                        color: "#40ffffff"
                    }
                    Rectangle {
                        x: Math.round(parent.width / 3)
                        y: 0
                        width: 1
                        height: parent.height
                        color: "#40ffffff"
                    }
                    Rectangle {
                        x: Math.round(parent.width * 2 / 3)
                        y: 0
                        width: 1
                        height: parent.height
                        color: "#40ffffff"
                    }
                }

                // Center Move Area (visual anchor & contract compatibility)
                Item {
                    id: cropMoveArea
                    objectName: "imageCropMoveArea-" + viewport.slot
                    anchors.fill: parent
                    anchors.margins: 10
                }

                // 8 Drag Handles (only in established selection mode)
                CropHandle {
                    objectName: "imageCropHandle-tl-" + viewport.slot
                    handleMode: "tl"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: -width / 2
                    y: -height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew
                }
                CropHandle {
                    objectName: "imageCropHandle-tr-" + viewport.slot
                    handleMode: "tr"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: parent.width - width / 2
                    y: -height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew
                }
                CropHandle {
                    objectName: "imageCropHandle-bl-" + viewport.slot
                    handleMode: "bl"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: -width / 2
                    y: parent.height - height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew
                }
                CropHandle {
                    objectName: "imageCropHandle-br-" + viewport.slot
                    handleMode: "br"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: parent.width - width / 2
                    y: parent.height - height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew
                }
                CropHandle {
                    objectName: "imageCropHandle-t-" + viewport.slot
                    handleMode: "t"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: -height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew && parent.width > 44
                }
                CropHandle {
                    objectName: "imageCropHandle-b-" + viewport.slot
                    handleMode: "b"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    anchors.horizontalCenter: parent.horizontalCenter
                    y: parent.height - height / 2
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew && parent.width > 44
                }
                CropHandle {
                    objectName: "imageCropHandle-l-" + viewport.slot
                    handleMode: "l"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: -width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew && parent.height > 44
                }
                CropHandle {
                    objectName: "imageCropHandle-r-" + viewport.slot
                    handleMode: "r"
                    targetViewport: viewport
                    overlayParent: cropOverlay
                    x: parent.width - width / 2
                    anchors.verticalCenter: parent.verticalCenter
                    visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew && parent.height > 44
                }
            }

            // Master Crop Interaction Area: non-moving viewport-level pointer capture
            // completely eliminates recursive feedback and jitter.
            MouseArea {
                id: cropInteractionArea
                objectName: "cropInteractionArea-" + viewport.slot
                anchors.fill: parent
                z: 35
                enabled: cropOverlay.visible && (cropOverlay.hasSelection || viewport.cropActive)
                hoverEnabled: true
                acceptedButtons: Qt.LeftButton | Qt.RightButton

                property string currentMode: ""
                property point dragStartPoint: Qt.point(0, 0)
                property var initialBox: null

                function hitTest(mx, my) {
                    if (!cropOverlay.hasSelection)
                        return "new";
                    const bx = cropOverlay.boxLeft;
                    const by = cropOverlay.boxTop;
                    const bw = cropOverlay.boxWidth;
                    const bh = cropOverlay.boxHeight;
                    const brx = bx + bw;
                    const bby = by + bh;
                    const tol = 16;

                    // 4 corners
                    if (Math.hypot(mx - bx, my - by) <= tol)
                        return "tl";
                    if (Math.hypot(mx - brx, my - by) <= tol)
                        return "tr";
                    if (Math.hypot(mx - bx, my - bby) <= tol)
                        return "bl";
                    if (Math.hypot(mx - brx, my - bby) <= tol)
                        return "br";

                    // 4 edges
                    if (Math.abs(my - by) <= 8 && mx >= bx - 4 && mx <= brx + 4)
                        return "t";
                    if (Math.abs(my - bby) <= 8 && mx >= bx - 4 && mx <= brx + 4)
                        return "b";
                    if (Math.abs(mx - bx) <= 8 && my >= by - 4 && my <= bby + 4)
                        return "l";
                    if (Math.abs(mx - brx) <= 8 && my >= by - 4 && my <= bby + 4)
                        return "r";

                    // Inside box
                    if (mx >= bx && mx <= brx && my >= by && my <= bby)
                        return "move";

                    return "new";
                }

                cursorShape: {
                    if (pressed) {
                        if (currentMode === "move")
                            return Qt.ClosedHandCursor;
                        if (currentMode === "pan")
                            return Qt.ClosedHandCursor;
                    }
                    const mode = pressed ? currentMode : hitTest(mouseX, mouseY);
                    switch (mode) {
                    case "tl":
                    case "br":
                        return Qt.SizeFDiagCursor;
                    case "tr":
                    case "bl":
                        return Qt.SizeBDiagCursor;
                    case "t":
                    case "b":
                        return Qt.SizeVerCursor;
                    case "l":
                    case "r":
                        return Qt.SizeHorCursor;
                    case "move":
                        return Qt.SizeAllCursor;
                    default:
                        return Qt.CrossCursor;
                    }
                }

                onPressed: mouse => {
                    if (mouse.button === Qt.RightButton) {
                        currentMode = "pan";
                        viewport.dragStart = Qt.point(mouse.x, mouse.y);
                        viewport.pressStart = Qt.point(mouse.x, mouse.y);
                        viewport.dragActive = false;
                        return;
                    }
                    const mode = hitTest(mouse.x, mouse.y);
                    currentMode = mode;
                    dragStartPoint = Qt.point(mouse.x, mouse.y);
                    if (mode !== "new" && cropOverlay.hasSelection) {
                        initialBox = {
                            "x": Number(control.cropSelection.x),
                            "y": Number(control.cropSelection.y),
                            "width": Number(control.cropSelection.width),
                            "height": Number(control.cropSelection.height)
                        };
                    } else {
                        initialBox = null;
                        viewport.pressStart = Qt.point(mouse.x, mouse.y);
                    }
                }

                onPositionChanged: mouse => {
                    if (pressed) {
                        if (currentMode === "pan") {
                            const baseW = Math.max(1, previewImage.drawWidth / Math.max(0.001, control.zoom));
                            const baseH = Math.max(1, previewImage.drawHeight / Math.max(0.001, control.zoom));
                            const dx = (mouse.x - viewport.dragStart.x) / baseW;
                            const dy = (mouse.y - viewport.dragStart.y) / baseH;
                            control.imageReview.panBy(dx, dy);
                            viewport.dragStart = Qt.point(mouse.x, mouse.y);
                            return;
                        }

                        if (currentMode === "new") {
                            if (!viewport.cropActive && Math.hypot(mouse.x - dragStartPoint.x, mouse.y - dragStartPoint.y) >= 6) {
                                viewport.cropActive = true;
                                viewport.cropStart = dragStartPoint;
                            }
                            if (viewport.cropActive) {
                                viewport.cropCurrent = Qt.point(mouse.x, mouse.y);
                                control.updateCropSelection(viewport);
                            }
                            return;
                        }

                        if (!initialBox || !control.imageEdit)
                            return;

                        const scale = cropOverlay.imgScale;
                        if (!(scale > 0))
                            return;

                        const deltaX = Math.round((mouse.x - dragStartPoint.x) / scale);
                        const deltaY = Math.round((mouse.y - dragStartPoint.y) / scale);
                        const imgW = Number(control.imageEdit.imageWidth);
                        const imgH = Number(control.imageEdit.imageHeight);

                        if (currentMode === "move") {
                            const newX = Math.max(0, Math.min(imgW - initialBox.width, initialBox.x + deltaX));
                            const newY = Math.max(0, Math.min(imgH - initialBox.height, initialBox.y + deltaY));
                            control.cropSelection = {
                                "x": newX,
                                "y": newY,
                                "width": initialBox.width,
                                "height": initialBox.height,
                                "fromX": newX,
                                "fromY": newY,
                                "toX": newX + initialBox.width,
                                "toY": newY + initialBox.height
                            };
                            return;
                        }

                        let x0 = initialBox.x;
                        let y0 = initialBox.y;
                        let x1 = initialBox.x + initialBox.width;
                        let y1 = initialBox.y + initialBox.height;

                        if (currentMode.indexOf("l") !== -1)
                            x0 = Math.max(0, Math.min(initialBox.x + initialBox.width - 2, initialBox.x + deltaX));
                        if (currentMode.indexOf("r") !== -1)
                            x1 = Math.min(imgW, Math.max(initialBox.x + 2, initialBox.x + initialBox.width + deltaX));
                        if (currentMode.indexOf("t") !== -1)
                            y0 = Math.max(0, Math.min(initialBox.y + initialBox.height - 2, initialBox.y + deltaY));
                        if (currentMode.indexOf("b") !== -1)
                            y1 = Math.min(imgH, Math.max(initialBox.y + 2, initialBox.y + initialBox.height + deltaY));

                        // Ratio-locked handles follow the dragged edge pair (see the shared
                        // helper); a free ratio keeps the previous clamp-only behaviour.
                        const ratioShaped = control.cropSelectionWithRatio(x0, y0, Math.max(2, x1 - x0), Math.max(2, y1 - y0), currentMode.indexOf("l") !== -1 || currentMode.indexOf("r") !== -1 || Math.abs(x1 - x0) >= Math.abs(y1 - y0));
                        control.cropSelection = {
                            "x": ratioShaped.x,
                            "y": ratioShaped.y,
                            "width": ratioShaped.width,
                            "height": ratioShaped.height,
                            "fromX": ratioShaped.x,
                            "fromY": ratioShaped.y,
                            "toX": ratioShaped.x + ratioShaped.width,
                            "toY": ratioShaped.y + ratioShaped.height
                        };
                    }
                }

                onReleased: {
                    currentMode = "";
                    initialBox = null;
                    viewport.cropActive = false;
                }

                onCanceled: {
                    currentMode = "";
                    initialBox = null;
                    viewport.cropActive = false;
                }

                onDoubleClicked: mouse => {
                    if (hitTest(mouse.x, mouse.y) === "move") {
                        control.applyCropSelection();
                    }
                }
            }

            // Floating Mini Action Bar
            Rectangle {
                id: cropActionBar
                objectName: "imageCropActionBar-" + viewport.slot
                visible: cropOverlay.hasSelection && !cropOverlay.isDraggingNew && cropBoxItem.visible
                z: 40
                // The bar's inner controls carry the 32 px hit-area minimum; the plate
                // keeps a 2 px breathing margin above and below them.
                implicitHeight: 36
                implicitWidth: cropActionRow.implicitWidth + 16
                radius: Theme.radiusMedium
                color: Theme.raisedPanel
                border.width: 1
                border.color: Theme.controlBorder

                x: Math.max(8, Math.min(viewport.width - width - 8, cropBoxItem.x + cropBoxItem.width - width))
                // Prefer the space below the box, fall back to the box's bottom edge,
                // then clamp inside the viewport AND above the bottom-left view-command
                // HUD. The bar and the HUD float on the canvas floor in the same
                // vertical frame, and an overlap puts the HUD's buttons (a later stage
                // sibling with higher z) on top of the bar's left chips, where clicks
                // can never reach them; a zoomed or panned crop can additionally put
                // the box itself beyond the visible canvas, so an unclamped fallback
                // would strand the bar outside the clipped viewport entirely.
                y: {
                    const belowY = cropBoxItem.y + cropBoxItem.height + 8;
                    const aboveY = cropBoxItem.y + cropBoxItem.height - height - 8;
                    const preferredY = (belowY + height <= viewport.height - 8) ? belowY : aboveY;
                    const floorY = viewport.height - height - 8;
                    const maxY = imageViewCommandHud.visible ? Math.min(floorY, imageViewCommandHud.y - height - 8) : floorY;
                    return Math.max(8, Math.min(maxY, preferredY));
                }

                Row {
                    id: cropActionRow
                    anchors.centerIn: parent
                    spacing: Theme.spaceMain

                    Text {
                        id: cropSizeLabel
                        objectName: "imageCropDimensionText-" + viewport.slot
                        text: control.cropSelection ? qsTr("%1 × %2 px").arg(control.cropSelection.width).arg(control.cropSelection.height) : ""
                        color: Theme.secondaryText
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle {
                        width: 1
                        height: 14
                        color: Theme.border
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    // Aspect chips: picking a ratio reshapes the current selection in place
                    // and constrains handle drags; 自由 releases the lock.
                    ReviewActionButton {
                        objectName: "imageCropRatioFree"
                        visible: control.editTool === "crop"
                        checkable: true
                        checked: !(control.cropRatioLock > 0)
                        mirrorsState: true
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("自由")
                        onClicked: control.cropRatioLock = 0
                    }
                    ReviewActionButton {
                        objectName: "imageCropRatioOneOne"
                        visible: control.editTool === "crop"
                        checkable: true
                        checked: control.cropRatioLock > 0 && Math.abs(control.cropRatioLock - 1) < 0.0001
                        mirrorsState: true
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("1:1")
                        onClicked: {
                            control.cropRatioLock = 1;
                            control.applyCropRatioToSelection();
                        }
                    }
                    ReviewActionButton {
                        objectName: "imageCropRatioSixteenNine"
                        visible: control.editTool === "crop"
                        checkable: true
                        checked: control.cropRatioLock > 0 && Math.abs(control.cropRatioLock - 16 / 9) < 0.0001
                        mirrorsState: true
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("16:9")
                        onClicked: {
                            control.cropRatioLock = 16 / 9;
                            control.applyCropRatioToSelection();
                        }
                    }
                    ReviewActionButton {
                        objectName: "imageCropRatioFourThree"
                        visible: control.editTool === "crop"
                        checkable: true
                        checked: control.cropRatioLock > 0 && Math.abs(control.cropRatioLock - 4 / 3) < 0.0001
                        mirrorsState: true
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("4:3")
                        onClicked: {
                            control.cropRatioLock = 4 / 3;
                            control.applyCropRatioToSelection();
                        }
                    }

                    // Direct size entry: the edited side leads, the ratio pushes the other
                    // side, and both clamp inside the image bounds.
                    Text {
                        visible: control.editTool === "crop"
                        text: qsTr("宽")
                        color: Theme.mutedText
                        font.pixelSize: Theme.fontCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TextField {
                        id: cropWidthField

                        objectName: "imageCropWidthField"
                        visible: control.editTool === "crop"
                        width: 54
                        height: 24
                        font.pixelSize: Theme.fontCaption
                        horizontalAlignment: TextInput.AlignHCenter
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator {
                            bottom: 1
                            top: control.imageEdit ? control.imageEdit.imageWidth : 1
                        }
                        text: control.cropSelection ? control.cropSelection.width : ""
                        onEditingFinished: {
                            control.applyCropSizeFromField("width", text);
                            text = Qt.binding(() => control.cropSelection ? control.cropSelection.width : "");
                        }
                        anchors.verticalCenter: parent.verticalCenter

                        background: Rectangle {
                            radius: Theme.radiusMicro
                            color: Theme.control
                            border.width: cropWidthField.activeFocus ? 2 : 1
                            border.color: cropWidthField.activeFocus ? Theme.focus : Theme.controlBorder
                        }
                    }
                    Text {
                        visible: control.editTool === "crop"
                        text: "×"
                        color: Theme.mutedText
                        font.pixelSize: Theme.fontCaption
                        anchors.verticalCenter: parent.verticalCenter
                    }
                    TextField {
                        id: cropHeightField

                        objectName: "imageCropHeightField"
                        visible: control.editTool === "crop"
                        width: 54
                        height: 24
                        font.pixelSize: Theme.fontCaption
                        horizontalAlignment: TextInput.AlignHCenter
                        inputMethodHints: Qt.ImhDigitsOnly
                        validator: IntValidator {
                            bottom: 1
                            top: control.imageEdit ? control.imageEdit.imageHeight : 1
                        }
                        text: control.cropSelection ? control.cropSelection.height : ""
                        onEditingFinished: {
                            control.applyCropSizeFromField("height", text);
                            text = Qt.binding(() => control.cropSelection ? control.cropSelection.height : "");
                        }
                        anchors.verticalCenter: parent.verticalCenter

                        background: Rectangle {
                            radius: Theme.radiusMicro
                            color: Theme.control
                            border.width: cropHeightField.activeFocus ? 2 : 1
                            border.color: cropHeightField.activeFocus ? Theme.focus : Theme.controlBorder
                        }
                    }

                    Rectangle {
                        width: 1
                        height: 14
                        color: Theme.border
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    Rectangle {
                        id: cancelBtn
                        objectName: "imageCropCancelButton-" + viewport.slot
                        signal clicked
                        onClicked: {
                            control.cropSelection = null;
                            control.forceActiveFocus();
                        }
                        implicitWidth: cancelText.implicitWidth + 16
                        implicitHeight: Theme.hitAreaMinimum
                        radius: Theme.radiusSmall
                        color: cancelMouse.pressed ? Theme.fluentPressed : (cancelMouse.containsMouse ? Theme.fluentHover : "transparent")
                        border.color: cancelMouse.containsMouse ? Theme.borderHover : Theme.border
                        border.width: 1
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            id: cancelText
                            anchors.centerIn: parent
                            text: qsTr("取消")
                            color: cancelMouse.containsMouse ? Theme.primaryText : Theme.secondaryText
                            font.pixelSize: Theme.fontCaption
                        }
                        MouseArea {
                            id: cancelMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: cancelBtn.clicked()
                        }
                    }

                    Rectangle {
                        id: applyBtn
                        objectName: "imageCropApplyButton-" + viewport.slot
                        signal clicked
                        onClicked: {
                            control.applyCropSelection();
                            control.forceActiveFocus();
                        }
                        implicitWidth: applyText.implicitWidth + 18
                        implicitHeight: Theme.hitAreaMinimum
                        radius: Theme.radiusSmall
                        color: applyMouse.pressed ? Theme.accentPressed : (applyMouse.containsMouse ? Theme.accentHover : Theme.accentFill)
                        border.color: Theme.accent
                        border.width: 1
                        anchors.verticalCenter: parent.verticalCenter

                        Text {
                            id: applyText
                            anchors.centerIn: parent
                            text: control.editTool === "mosaic" ? qsTr("应用马赛克") : (control.editTool === "smart_select" ? qsTr("智能抠出目标") : qsTr("应用裁剪"))
                            color: Theme.inverseText
                            font.pixelSize: Theme.fontCaption
                            font.weight: Font.DemiBold
                        }
                        MouseArea {
                            id: applyMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: applyBtn.clicked()
                        }
                    }
                }
            }
        }

        MouseArea {
            id: viewportMouseArea
            objectName: "imageCanvasMouseArea-" + viewport.slot
            anchors.fill: parent
            hoverEnabled: true
            acceptedButtons: Qt.LeftButton | Qt.MiddleButton | Qt.RightButton
            cursorShape: {
                if (viewport.dragActive)
                    return Qt.ClosedHandCursor;
                if (control.editModeActive) {
                    if (control.editTool === "brush" || control.editTool === "crop" || control.editTool === "smart_select")
                        return Qt.CrossCursor;
                }
                return Qt.ArrowCursor;
            }
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
                    const isPanButton = (mouse.buttons & Qt.RightButton) || (mouse.buttons & Qt.MiddleButton) || (!control.editModeActive && (mouse.buttons & Qt.LeftButton));
                    if (isPanButton) {
                        if (!viewport.dragActive && Math.hypot(mouse.x - viewport.pressStart.x, mouse.y - viewport.pressStart.y) >= 3)
                            viewport.dragActive = true;
                        if (viewport.dragActive) {
                            // 1:1 exact pixel pan tracking: mouse delta maps directly to image screen delta
                            const baseW = Math.max(1, previewImage.drawWidth / Math.max(0.001, control.zoom));
                            const baseH = Math.max(1, previewImage.drawHeight / Math.max(0.001, control.zoom));
                            const dx = (mouse.x - viewport.dragStart.x) / baseW;
                            const dy = (mouse.y - viewport.dragStart.y) / baseH;
                            control.imageReview.panBy(dx, dy);
                            viewport.dragStart = Qt.point(mouse.x, mouse.y);
                        }
                    } else if (control.editModeActive && (mouse.buttons & Qt.LeftButton) && !(mouse.modifiers & Qt.ShiftModifier) && !viewport.cropActive && (control.editTool === "crop" || control.editTool === "mosaic" || control.editTool === "smart_select")) {
                        // Start a new crop rectangle only after dragging past 6 pixels, avoiding accidental destruction of an existing selection
                        if (Math.hypot(mouse.x - viewport.pressStart.x, mouse.y - viewport.pressStart.y) >= 6) {
                            viewport.cropActive = true;
                            viewport.cropStart = viewport.pressStart;
                            viewport.cropCurrent = Qt.point(mouse.x, mouse.y);
                            control.updateCropSelection(viewport);
                        }
                    }
                }
                control.applyHover(viewport, mouse.x, mouse.y, viewport.slot);
            }
            onExited: {
                control.clearImageHover();
            }
            onCanceled: viewport.dragActive = false
            onPressed: mouse => {
                control.applyHover(viewport, mouse.x, mouse.y, viewport.slot);
                // RightButton or MiddleButton always pans the canvas, both in review and edit mode
                if (mouse.button === Qt.RightButton || mouse.button === Qt.MiddleButton) {
                    viewport.cropActive = false;
                    viewport.marqueeActive = false;
                    viewport.annotationDragActive = false;
                    viewport.dragStart = Qt.point(mouse.x, mouse.y);
                    viewport.pressStart = Qt.point(mouse.x, mouse.y);
                    viewport.dragActive = false;
                    control.forceActiveFocus();
                    return;
                }

                // Edit mode: the left button runs the active tool (brush paints, select
                // drags an annotation, text places on click, crop/rect selects a rect).
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
                        // If no crop selection exists yet, start dragging immediately.
                        // If a crop selection already exists, wait for drag threshold in onPositionChanged
                        // so an accidental click doesn't instantly wipe the established crop selection!
                        if (!control.cropSelection) {
                            viewport.cropActive = true;
                            viewport.cropStart = Qt.point(mouse.x, mouse.y);
                            viewport.cropCurrent = Qt.point(mouse.x, mouse.y);
                        }
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
                // Every release ends the pan grab: a left-drag past the 3px threshold must
                // clear dragActive too, or the closed-hand cursor and the onClicked drag gate
                // stay stuck for the next click. That release is still a drag, not a click, so
                // suppress it from toggling the source - the same suppression the annotation,
                // crop and marquee gestures already use.
                if (mouse.button === Qt.LeftButton && viewport.dragActive)
                    viewport.suppressClickAfterMarquee = true;
                viewport.dragActive = false;
                if (mouse.button === Qt.RightButton || mouse.button === Qt.MiddleButton) {
                    return;
                }
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
                        // A release whose rectangle misses the picture entirely used to fall
                        // through silently; the status bar now says why nothing zoomed.
                        if (m0 && m1 && !(m0.x >= previewImage.sourceSize.width || m0.y >= previewImage.sourceSize.height || m1.x <= 0 || m1.y <= 0)) {
                            const sw = previewImage.sourceSize.width;
                            const sh = previewImage.sourceSize.height;
                            const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                            const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                            const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                            const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                            if (control.imageReview)
                                control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                        } else {
                            control.showMarqueeNotice(qsTr("框选超出画面范围，未放大"));
                        }
                    } else if (Math.abs(right - left) >= 8 || Math.abs(bottom - top) >= 8) {
                        control.showMarqueeNotice(qsTr("框选区域太小（至少 8×8 像素），未放大"));
                    }
                }
            }
            onDoubleClicked: mouse => {
                // Qt sends a click before the double-click event. Keep the selected A/B
                // source unchanged when the user invokes the zoom gesture.
                if (mouse.button === Qt.LeftButton && control.compareMode === 0 && control.hasPair)
                    control.toggleSinglePairSource();
                control.toggleTrueSize();
            }
            onWheel: wheel => {
                if (!control.imageReview)
                    return;
                const mapped = control.mapToImage(viewport, wheel.x, wheel.y);
                const ax = mapped ? Math.max(0, Math.min(1, mapped.x / Math.max(1, previewImage.sourceSize.width))) : 0.5;
                const ay = mapped ? Math.max(0, Math.min(1, mapped.y / Math.max(1, previewImage.sourceSize.height))) : 0.5;
                const steps = wheel.angleDelta.y / 120.0;
                const factor = Math.pow(1.15, steps);
                control.imageReview.zoomBy(factor, ax, ay);
                wheel.accepted = true;
            }
        }
    }

    // The command surface is a flat two-row strip aligned with the video workspace: row 1
    // names the sources and the session commands, row 2 carries the comparison and
    // observation modes and is replaced wholesale by the edit ribbon while editing. The
    // stage keeps its anchor contract: stage.top stays commandPanel.bottom + 12.
    Rectangle {
        id: commandPanel

        objectName: "imageCommandPanel"
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
        }
        height: commandColumn.implicitHeight
        color: Theme.panel

        Column {
            id: commandColumn

            anchors {
                left: parent.left
                right: parent.right
                top: parent.top
            }
            // Bands butt exactly: a Column's default spacing is 0, no literal needed.

            // Row 1 — sources & session. The A/B chips give the pair a visible identity
            // (click = direct look, drop = replace that slot); the session commands sit in
            // the same flow so a narrow window wraps instead of overlapping. Opening
            // variants live behind one dropdown so the strip never grows past one band.
            Item {
                id: sourceRow

                objectName: "imageSourceRow"
                width: parent.width
                height: Math.max(Theme.barRowHeight, fileRow.implicitHeight + 8)

                Flow {
                    id: fileRow

                    objectName: "imageFileRow"
                    spacing: Theme.spaceMain
                    anchors {
                        left: parent.left
                        leftMargin: 12
                        right: parent.right
                        rightMargin: 12
                        verticalCenter: parent.verticalCenter
                    }
                    // See the forceLayout note in Component.onCompleted above.
                    onWidthChanged: {
                        forceLayout();
                        Qt.callLater(commandColumn.forceLayout);
                    }

                    ImageSourceChip {
                        objectName: "imageSourceChipA"
                        sourceSlot: 2
                        visible: control.hasPrimary
                        chipTitle: control.imageTitle(control.imageReview.primaryPath, control.imageReview.secondaryPath)
                        chipPath: String((control.imageReview && control.imageReview.primaryPath) || "")
                        onDirectLookRequested: {
                            if (!control.canSwitchSingleSource)
                                return;
                            if (control.compareMode !== 0)
                                control.modeButton(0);
                            control.singleViewShowSecondary = false;
                        }
                        onCloseRequested: control.closeSide(2)
                        onReplaceDropped: url => control.replacePrimaryDropped(url)
                    }
                    ImageSourceChip {
                        objectName: "imageSourceChipB"
                        sourceSlot: 3
                        visible: control.hasSecondary
                        chipTitle: control.imageTitle(control.imageReview.secondaryPath, control.imageReview.primaryPath)
                        chipPath: String((control.imageReview && control.imageReview.secondaryPath) || "")
                        onDirectLookRequested: {
                            if (!control.canSwitchSingleSource)
                                return;
                            if (control.compareMode !== 0)
                                control.modeButton(0);
                            control.singleViewShowSecondary = true;
                        }
                        onCloseRequested: control.closeSide(3)
                        onReplaceDropped: url => control.replaceSecondaryDropped(url)
                    }

                    RowSeparator {
                        visible: control.hasPrimary
                    }

                    ImageCommandButton {
                        objectName: "imageOpenButton"
                        // With nothing open this is the entry point; once an image is open
                        // the same commands live behind 打开/添加 ▾ below.
                        visible: !control.hasPrimary
                        text: qsTr("打开图片…")
                        // Edit mode has its own primary action (另存副本…); one filled button per view.
                        prominent: !control.hasPrimary && !control.editModeActive
                        implicitHeight: 30
                        leftPadding: 12
                        rightPadding: 12
                        onClicked: control.openImageRequested()
                    }
                    ImageCommandButton {
                        objectName: "imageOpenPairButton"
                        text: qsTr("打开/添加 ▾")
                        implicitHeight: 30
                        leftPadding: 8
                        rightPadding: 8
                        onClicked: openPairMenu.open()

                        VcsMenu {
                            id: openPairMenu
                            menuWidth: 220

                            VcsMenuItem {
                                objectName: "imageOpenSingleMenuItem"
                                text: qsTr("打开图片…")
                                onTriggered: control.openImageRequested()
                            }
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
                    ImageCommandButton {
                        objectName: "imageSwapSidesButton"
                        visible: control.hasPair
                        implicitHeight: 30
                        leftPadding: 8
                        rightPadding: 8
                        text: qsTr("对调 A/B")
                        helpText: qsTr("交换 A 与 B 的槽位方向。\n不改变文件夹配对身份；带符号差异、分割线与淡化会跟随新的 A/B 方向。")
                        onClicked: control.swapSidesRequested()
                    }

                    ImageCommandButton {
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
                            VcsMenuSeparator {}
                            VcsMenuItem {
                                objectName: "imageCloseMenuItem"
                                text: qsTr("关闭图片")
                                // Same guard as the chip's right-click menu: an edit session
                                // with unsaved work refuses and explains, instead of dropping
                                // the working copy silently.
                                enabled: control.hasPrimary && control.closeSideBlockedReason.length === 0
                                onTriggered: {
                                    if (control.closeSideBlockedReason.length > 0) {
                                        control.showMarqueeNotice(control.closeSideBlockedReason);
                                        return;
                                    }
                                    // The surviving side is closed last: closing B keeps A,
                                    // then closing A reaches the empty state through the same
                                    // single-side path - a closeAll is never used to stand in
                                    // for a single-side close.
                                    control.closeSide(3);
                                    control.closeSide(2);
                                }
                            }
                        }
                    }

                    ImageCommandButton {
                        objectName: "imageEditStartButton"
                        visible: control.hasPrimary && control.imageEdit !== null
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

                    // Preserved objectName: the close action moved into the 换图 dropdown
                    // and the A/B chips' right-click menu; the standalone button stays in the
                    // tree (hidden) for compatibility and routes through the same guarded
                    // single-side close.
                    ImageCommandButton {
                        objectName: "imageCloseButton"
                        visible: false
                        text: qsTr("关闭图片")
                        enabled: control.hasPrimary
                        onClicked: {
                            control.closeSide(3);
                            control.closeSide(2);
                        }
                    }

                    ImageCommandButton {
                        objectName: "imageToggleSidebarButton"
                        // The label names the action it performs, so the button carries no checked tint.
                        text: control.sidebarVisible ? qsTr("隐藏列表") : qsTr("显示列表")
                        implicitHeight: 30
                        leftPadding: 10
                        rightPadding: 10
                        visible: control.hasFolders
                        onClicked: control.toggleSidebarRequested()
                    }

                    // FLOW3: where the arrow keys are in the current directory. The count is not
                    // decoration - it says the image belongs to a sequence and how far it runs,
                    // which is what tells the user a neighbour exists before pressing a key.
                    Text {
                        objectName: "imageFolderPositionLabel"
                        visible: control.folderBrowseText.length > 0
                        text: control.folderBrowseText
                        color: Theme.secondaryText
                        font.pixelSize: Theme.fontCaption
                        verticalAlignment: Text.AlignVCenter
                    }
                }
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
            }

            // Row 2 — comparison & observation. Mutually exclusive modes stay grouped; the
            // difference modes collapse behind one dropdown, the channel/background
            // observations are one-click chips. While editing, this whole band is replaced
            // by the edit ribbon below it.
            Item {
                id: modeRowHost

                width: parent.width
                visible: !control.editModeActive
                height: visible ? Math.max(Theme.barRowHeight, modeRow.implicitHeight + 8) : 0

                Flow {
                    id: modeRow

                    objectName: "imageModeRow"
                    spacing: Theme.spaceMain
                    anchors {
                        left: parent.left
                        leftMargin: 12
                        right: parent.right
                        rightMargin: 12
                        verticalCenter: parent.verticalCenter
                    }
                    // See the forceLayout note in Component.onCompleted above.
                    onWidthChanged: {
                        forceLayout();
                        Qt.callLater(commandColumn.forceLayout);
                    }

                    // C-01: the single-image A/B view. The name says what it is - one canvas
                    // showing A or B - instead of the old "手动闪烁", which described the
                    // switching gesture and read as a separate feature next to the fade mode.
                    // Which side is on screen stays visible in the in-canvas badge, and the
                    // A/B file chips plus Space/T are the entries that change it.
                    ModeChip {
                        objectName: "imageModePrimary"
                        text: control.hasPair ? qsTr("单图 A/B") : qsTr("查看")
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
                    // E1: wipe split axis toggle - the discoverable entry for the image
                    // workspace, shown only while the wipe comparison is active. The noun is
                    // 分割线, the same one the mode bar, the video HUD and the shortcut table
                    // use (UX-10); the qualifier names the halves (左右/上下) instead of the
                    // axis jargon. Switching keeps the ratio, the pair and the decoded buffers
                    // untouched.
                    ImageCommandButton {
                        objectName: "imageWipeOrientationButton"
                        visible: control.hasPair && control.compareMode === 5
                        enabled: visible && control.imageReview !== null
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: control.imageReview && control.imageReview.wipeOrientation === 1 ? qsTr("分割线：上下") : qsTr("分割线：左右")
                        onClicked: {
                            if (control.imageReview)
                                control.imageReview.wipeOrientation = control.imageReview.wipeOrientation === 1 ? 0 : 1;
                        }
                    }
                    ModeChip {
                        objectName: "imageModeFade"
                        text: qsTr("淡化")
                        modeValue: 7
                        visible: control.hasPair
                    }
                    // C-01: the standalone "切至 A / 切至 B" toolbar button is gone. It
                    // duplicated the A/B file chips (a click on the chip selects that side),
                    // the Space/T shortcut and the in-canvas badge, and a permanent button
                    // whose label flipped on every press was the least discoverable of the
                    // three. The remaining entries are kept and tested; the video playback
                    // shortcuts are untouched.
                    ImageCommandButton {
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

                    // Hold-to-peek the originals while reading a difference or fade view.
                    // The button mirrors the keyboard: hold = look, release = restore.
                    // visible/enabled stay on while a peek is active (beginPeek switches the
                    // mode away from the availability condition; hiding or disabling the
                    // button mid-hold would cancel the press and end the peek), and the
                    // press must not move the focus either — a focus change cancels a held
                    // AbstractButton, so only tab navigation can focus it.
                    ImageCommandButton {
                        objectName: "imagePeekButton"
                        visible: control.peekModeAvailable || control.peekActive
                        enabled: control.peekModeAvailable || control.peekActive
                        focusPolicy: Qt.TabFocus
                        implicitHeight: 30
                        leftPadding: 8
                        rightPadding: 8
                        text: control.peekActive ? qsTr("松开恢复 %1").arg(control.diffModeLabel) : qsTr("按住看 A/B 原图")
                        helpText: qsTr("按住时临时直看原图：` 看 A，Shift+` 看 B；松开恢复差异/淡化视图。也可按住此按钮。")
                        onPressed: control.beginPeek(false)
                        onReleased: control.endPeek()
                        onCanceled: control.endPeek()
                    }

                    // Difference display gain, one click per step. Wide windows show the
                    // inline steps; narrow ones fall back to the dropdown below. The gain
                    // amplifies only the rendered image; the peak/MAE readouts stay raw
                    // 8-bit deltas, so two candidates compared at the same gain remain
                    // visually comparable and the numbers never follow the display setting.
                    Row {
                        id: imageDiffGainChipRow

                        objectName: "imageDiffGainChipRow"
                        visible: control.diffModeActive && commandPanel.width >= 1180
                        spacing: 4
                        height: Theme.hitAreaMinimum

                        Text {
                            text: qsTr("增益")
                            color: Theme.mutedText
                            font.pixelSize: Theme.fontCaption
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        HudCommandButton {
                            objectName: "imageDiffGainChip1"
                            checkable: true
                            checked: control.imageReview && control.imageReview.diffGain === 1
                            mirrorsState: true
                            text: qsTr("×1")
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.diffGain = 1;
                            }
                        }
                        HudCommandButton {
                            objectName: "imageDiffGainChip2"
                            checkable: true
                            checked: control.imageReview && control.imageReview.diffGain === 2
                            mirrorsState: true
                            text: qsTr("×2")
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.diffGain = 2;
                            }
                        }
                        HudCommandButton {
                            objectName: "imageDiffGainChip4"
                            checkable: true
                            checked: control.imageReview && control.imageReview.diffGain === 4
                            mirrorsState: true
                            text: qsTr("×4")
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.diffGain = 4;
                            }
                        }
                        HudCommandButton {
                            objectName: "imageDiffGainChip8"
                            checkable: true
                            checked: control.imageReview && control.imageReview.diffGain === 8
                            mirrorsState: true
                            text: qsTr("×8")
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.diffGain = 8;
                            }
                        }
                        HudCommandButton {
                            objectName: "imageDiffGainChip16"
                            checkable: true
                            checked: control.imageReview && control.imageReview.diffGain === 16
                            mirrorsState: true
                            text: qsTr("×16")
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.diffGain = 16;
                            }
                        }
                    }

                    ImageCommandButton {
                        objectName: "imageDiffGainButton"
                        text: qsTr("差异放大 ×%1 ▾").arg(control.imageReview ? control.imageReview.diffGain : 4)
                        implicitHeight: 30
                        leftPadding: 8
                        rightPadding: 8
                        visible: control.diffModeActive && !imageDiffGainChipRow.visible
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

                    RowSeparator {}

                    // Channel observation quick chips (A / O keep their toggle semantics:
                    // clicking the active chip returns to plain RGBA).
                    ModeChip {
                        objectName: "imageViewChipRgba"
                        text: qsTr("RGBA")
                        selectGroup: 1
                        modeValue: 0
                    }
                    ModeChip {
                        objectName: "imageViewChipAlphaGray"
                        text: qsTr("Alpha (A)")
                        selectGroup: 1
                        modeValue: 1
                    }
                    ModeChip {
                        objectName: "imageViewChipRgbOpaque"
                        text: qsTr("忽略透明 (O)")
                        selectGroup: 1
                        modeValue: 2
                    }

                    // ---- Alpha threshold observation filter -------------------------------------
                    // Sits next to the "RGB ignoring alpha" entry it belongs to, because that
                    // is the only observation it applies to. Default off; "off" and "on with
                    // T = 0" are deliberately different states (T = 0 still hides A = 0).
                    ImageCommandButton {
                        objectName: "imageAlphaFilterButton"
                        visible: control.hasPrimary
                        implicitHeight: Theme.hitAreaMinimum
                        leftPadding: 8
                        rightPadding: 8
                        text: control.alphaFilterEnabled ? qsTr("Alpha 过滤：A > %1").arg(control.alphaThreshold) : qsTr("Alpha 过滤：关")
                        helpText: qsTr("仅显示 A > T（严格大于）：A=20/T=25 与 A=25/T=25 隐藏，A=26/T=25 显示。\n未通过区域显示中性棋盘格，不会被误认为原像素是黑色。\n不修改素材、编辑副本、原始读数、差异或统计；探针仍读原始 RGBA。")
                        onClicked: control.setAlphaFilterEnabled(!control.alphaFilterEnabled)
                    }

                    // Slider + exact entry + reset. The slider writes through a debounce so a
                    // drag cannot queue one full view rebuild per pixel of travel; the spin
                    // box and the reset commit immediately.
                    Row {
                        objectName: "imageAlphaFilterRow"
                        visible: control.alphaFilterEnabled && control.hasPrimary
                        spacing: Theme.spaceMicro
                        height: Theme.hitAreaMinimum

                        Text {
                            text: qsTr("仅显示 A > T")
                            color: Theme.mutedText
                            font.pixelSize: Theme.fontCaption
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Slider {
                            id: alphaThresholdSlider

                            objectName: "imageAlphaFilterSlider"
                            from: 0
                            to: 255
                            stepSize: 1
                            implicitWidth: 148
                            anchors.verticalCenter: parent.verticalCenter
                            // Dragging writes `value` directly, which would break a plain
                            // binding on the first drag; restoreMode puts it back so the
                            // controller stays the single source of truth (the reset button
                            // and the spin box both write through it).
                            Binding {
                                target: alphaThresholdSlider
                                property: "value"
                                value: control.alphaThreshold
                                restoreMode: Binding.RestoreBinding
                            }
                            onMoved: {
                                alphaThresholdCommit.pendingValue = value;
                                alphaThresholdCommit.restart();
                            }
                            onPressedChanged: {
                                if (!pressed) {
                                    alphaThresholdCommit.stop();
                                    control.setAlphaThreshold(value);
                                }
                            }
                        }

                        ReviewOffsetSpinBox {
                            id: alphaThresholdSpin

                            objectName: "imageAlphaFilterSpin"
                            from: 0
                            to: 255
                            implicitWidth: 104
                            anchors.verticalCenter: parent.verticalCenter
                            Binding {
                                target: alphaThresholdSpin
                                property: "value"
                                value: control.alphaThreshold
                                restoreMode: Binding.RestoreBinding
                            }
                            onValueModified: control.setAlphaThreshold(value)
                        }

                        ImageCommandButton {
                            objectName: "imageAlphaFilterResetButton"
                            text: qsTr("重置")
                            implicitHeight: Theme.hitAreaMinimum
                            leftPadding: 8
                            rightPadding: 8
                            anchors.verticalCenter: parent.verticalCenter
                            onClicked: control.resetAlphaThreshold()
                        }
                    }

                    // Background quick chips under transparent regions.
                    ModeChip {
                        objectName: "imageBgChipDark"
                        text: qsTr("深色")
                        selectGroup: 2
                        modeValue: 0
                    }
                    ModeChip {
                        objectName: "imageBgChipChecker"
                        text: qsTr("棋盘")
                        selectGroup: 2
                        modeValue: 1
                    }
                    ModeChip {
                        objectName: "imageBgChipBlack"
                        text: qsTr("黑底")
                        selectGroup: 2
                        modeValue: 2
                    }
                    ModeChip {
                        objectName: "imageBgChipWhite"
                        text: qsTr("白底")
                        selectGroup: 2
                        modeValue: 3
                    }

                    // Preserved dropdown entries: the quick chips cover the wide layout,
                    // the menus stay reachable (and their items testable) on every width.
                    ImageCommandButton {
                        objectName: "imageViewModeButton"
                        visible: false
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
                    ImageCommandButton {
                        objectName: "imageBackgroundButton"
                        visible: false
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
            }

            // Row 2 (edit) — the comparison band is replaced wholesale by the edit ribbon.
            // The original file is never written to: the session edits a working copy and
            // saving always produces a new file. Every tool takes its geometry in image
            // pixels, so zooming or panning cannot drift a selection, a stroke or an
            // annotation. The tool only changes what the left button does; middle-drag
            // still pans. 进入/结束编辑的入口固定在第 1 层（编辑画面/结束编辑）。
            Item {
                id: editBarHost

                width: parent.width
                visible: control.editModeActive && control.hasPrimary && control.imageEdit !== null
                height: visible ? editRibbon.implicitHeight : 0

                Column {
                    id: editRibbon

                    anchors {
                        left: parent.left
                        right: parent.right
                        top: parent.top
                        margins: 4
                    }
                    spacing: Theme.spaceMain

                    Flow {
                        id: editRow
                        objectName: "imageEditRow"
                        width: parent.width
                        spacing: 6
                        visible: control.hasPrimary && control.imageEdit !== null
                        // See the forceLayout note in Component.onCompleted above.
                        onWidthChanged: {
                            forceLayout();
                            Qt.callLater(commandColumn.forceLayout);
                        }

                        Text {
                            objectName: "imageEditingSourceLabel"
                            height: 30
                            verticalAlignment: Text.AlignVCenter
                            text: control.imageEdit ? qsTr("正在编辑：%1 的副本 (%2×%3)").arg(control.editSourceSlot === 3 ? "B" : "A").arg(control.imageEdit.imageWidth).arg(control.imageEdit.imageHeight) : ""
                            color: Theme.secondaryText
                            font.pixelSize: Theme.fontCaption
                            elide: Text.ElideRight
                            width: Math.min(200, implicitWidth)
                        }

                        RowSeparator {
                            hairlineHeight: 20
                        }

                        ImageCommandButton {
                            objectName: "imageEditToolCropButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "crop"
                            mirrorsState: true
                            text: qsTr("裁剪")
                            onClicked: control.editTool = "crop"
                        }
                        ImageCommandButton {
                            objectName: "imageEditToolSmartSelectButton"
                            // Always present: hiding the tool when the runtime is missing made
                            // its own "检测模型" entry unreachable (you had to select the tool
                            // to find the button that enables it). Disabled plus an actionable
                            // reason keeps the path discoverable.
                            visible: control.editModeActive
                            enabled: !!(control.imageEdit && control.imageEdit.smartSegmentAvailable)
                            checkable: true
                            checked: control.editTool === "smart_select"
                            mirrorsState: true
                            text: qsTr("智能抠图")
                            helpText: (control.imageEdit && control.imageEdit.smartSegmentAvailable) ? qsTr("框选目标物体，AI 自动分割轮廓并抠出透明背景。") : qsTr("需要 models 目录下的 onnxruntime.dll 与 sam2_encoder/decoder.onnx。放入文件后，点击下方编辑栏的「检测模型」。")
                            onClicked: control.editTool = "smart_select"
                        }
                        ImageCommandButton {
                            objectName: "imageEditToolBrushButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "brush"
                            mirrorsState: true
                            text: qsTr("画笔")
                            onClicked: control.editTool = "brush"
                        }
                        ImageCommandButton {
                            objectName: "imageEditToolSelectButton"
                            visible: control.editModeActive
                            checkable: true
                            checked: control.editTool === "select"
                            mirrorsState: true
                            text: qsTr("选择")
                            helpText: qsTr("点击选中标注后可拖动；Delete 键或「删除标注」可删除。")
                            onClicked: control.editTool = "select"
                        }

                        // Drawing and annotation tools dropdown
                        ImageCommandButton {
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

                        // Whole-image geometry, not a brush: these change the working copy's size rather
                        // than the pixels under the cursor, so they sit together in their own group.
                        ImageCommandButton {
                            objectName: "imageEditScaleButton"
                            visible: control.editModeActive
                            text: qsTr("缩放…")
                            helpText: qsTr("把画面重采样到指定的像素尺寸；锁定比例时改一边会自动推算另一边。")
                            onClicked: imageEditDialogs.openScale()
                        }
                        ImageCommandButton {
                            objectName: "imageEditFillButton"
                            visible: control.editModeActive
                            text: qsTr("填充画布…")
                            helpText: qsTr("把原图居中放到更大的画布上，四周用指定颜色补齐。")
                            onClicked: imageEditDialogs.openFill()
                        }

                        ImageCommandButton {
                            objectName: "imageEditTransformButton"
                            visible: control.editModeActive
                            text: qsTr("旋转 / 翻转 ▾")
                            enabled: control.imageEdit && !control.imageEdit.strokeActive
                            helpText: qsTr("水平镜像、垂直翻转、90° 或 180° 旋转；每次操作都可撤销。")
                            onClicked: transformMenu.open()

                            VcsMenu {
                                id: transformMenu
                                objectName: "imageEditTransformMenu"
                                menuWidth: 240

                                VcsMenuItem {
                                    objectName: "imageEditMirrorHorizontalItem"
                                    text: qsTr("水平镜像（左右）")
                                    onTriggered: control.applyImageMirror(false)
                                }
                                VcsMenuItem {
                                    objectName: "imageEditFlipVerticalItem"
                                    text: qsTr("垂直翻转（上下）")
                                    onTriggered: control.applyImageMirror(true)
                                }
                                VcsMenuSeparator {}
                                VcsMenuItem {
                                    objectName: "imageEditRotateClockwiseItem"
                                    text: qsTr("顺时针旋转 90°")
                                    onTriggered: control.applyImageRotation(1)
                                }
                                VcsMenuItem {
                                    objectName: "imageEditRotateCounterclockwiseItem"
                                    text: qsTr("逆时针旋转 90°")
                                    onTriggered: control.applyImageRotation(-1)
                                }
                                VcsMenuItem {
                                    objectName: "imageEditRotateHalfTurnItem"
                                    text: qsTr("旋转 180°")
                                    onTriggered: control.applyImageRotation(2)
                                }
                            }
                        }

                        RowSeparator {
                            visible: control.editModeActive
                            hairlineHeight: 20
                        }

                        ImageCommandButton {
                            objectName: "imageEditUndoButton"
                            visible: control.editModeActive
                            text: qsTr("撤销")
                            enabled: control.imageEdit && control.imageEdit.canUndo
                            onClicked: {
                                if (control.imageEdit)
                                    control.imageEdit.undo();
                            }
                        }
                        ImageCommandButton {
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

                        ImageCommandButton {
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
                            font.pixelSize: Theme.fontCaption
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
                        radius: Theme.radiusSubtle
                        color: Theme.canvas
                        border.width: 1
                        border.color: Theme.border

                        Flow {
                            id: editParamsRow
                            objectName: "imageEditParamsRow"
                            spacing: Theme.spaceMain
                            visible: control.hasPrimary && control.editModeActive
                            // See the forceLayout note in Component.onCompleted above.
                            onWidthChanged: {
                                forceLayout();
                                Qt.callLater(commandColumn.forceLayout);
                            }

                            anchors {
                                left: parent.left
                                right: parent.right
                                leftMargin: 10
                                rightMargin: 10
                                verticalCenter: parent.verticalCenter
                            }

                            // Persistent model probe: reachable from every tool, so a user who
                            // drops the runtime into models/ never has to discover a hidden
                            // tool to re-check. Its result lands in the workspace status line.
                            ReviewActionButton {
                                objectName: "imageEditProbeSmartSelectButton"
                                visible: control.imageEdit !== null
                                text: qsTr("检测模型")
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                helpText: qsTr("在 models 目录中查找 onnxruntime.dll、sam2_encoder.onnx 与 sam2_decoder.onnx；就绪后「智能抠图」会变为可用。")
                                onClicked: {
                                    if (control.imageEdit)
                                        // QML does not apply C++ default arguments, so the
                                        // explicit empty directory is required here.
                                        control.imageEdit.probeSmartSegment("");
                                }
                            }
                            Text {
                                objectName: "imageEditSmartSegmentReadyText"
                                visible: control.imageEdit !== null && control.imageEdit.smartSegmentAvailable
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("智能抠图已就绪")
                                color: Theme.mutedText
                                font.pixelSize: Theme.fontCaption
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
                                font.pixelSize: Theme.fontCaption
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
                                font.pixelSize: Theme.fontCaption
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
                                font.pixelSize: Theme.fontCaption
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
                                font.pixelSize: Theme.fontCaption
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
                                font.pixelSize: Theme.fontCaption
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
                            Text {
                                objectName: "imageEditCropHelpText"
                                visible: control.editTool === "crop" && control.cropSelection === null
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: qsTr("在画面上拖拽框选裁剪区域；支持 8 锚点调整与移动；Enter 确认，Esc 取消")
                                color: Theme.mutedText
                                font.pixelSize: Theme.fontCaption
                            }
                            ReviewActionButton {
                                objectName: "imageEditCancelCropButton"
                                visible: control.editTool === "crop" && control.cropSelection !== null
                                text: qsTr("取消选区")
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                onClicked: {
                                    control.cropSelection = null;
                                    control.forceActiveFocus();
                                }
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
                            Text {
                                objectName: "imageEditSmartSelectHelpText"
                                visible: control.editTool === "smart_select" && control.cropSelection === null
                                height: 30
                                verticalAlignment: Text.AlignVCenter
                                text: {
                                    if (!control.imageEdit)
                                        return "";
                                    if (!control.imageEdit.smartSegmentAvailable)
                                        return qsTr("AI 模型未就绪：请将 onnxruntime.dll 与 sam2_encoder/decoder.onnx 放入 models 目录，再点击「检测模型」。");
                                    return qsTr("在画面上拖拽框选要抠出的目标物体 (支持调整选区与锚点)");
                                }
                                color: (control.imageEdit && control.imageEdit.smartSegmentAvailable) ? Theme.mutedText : Theme.warning
                                font.pixelSize: Theme.fontCaption
                            }
                            CheckBox {
                                objectName: "imageEditSmartSelectTightCropCheck"
                                visible: control.editTool === "smart_select" && control.imageEdit && control.imageEdit.smartSegmentAvailable
                                text: qsTr("紧凑裁剪")
                                checked: control.smartSegmentTightCrop
                                onToggled: control.smartSegmentTightCrop = checked
                            }
                            ReviewActionButton {
                                objectName: "imageEditCancelSmartSelectButton"
                                visible: control.editTool === "smart_select" && control.cropSelection !== null
                                text: qsTr("取消选区")
                                implicitHeight: 30
                                leftPadding: 10
                                rightPadding: 10
                                onClicked: {
                                    control.cropSelection = null;
                                    control.forceActiveFocus();
                                }
                            }
                            ReviewActionButton {
                                objectName: "imageEditApplySmartSelectButton"
                                visible: control.editTool === "smart_select"
                                prominent: true
                                text: control.cropSelection ? qsTr("识别并抠出目标 (%1×%2)").arg(control.cropSelection.width).arg(control.cropSelection.height) : qsTr("识别并抠出目标")
                                enabled: control.cropSelection !== null && control.imageEdit && control.imageEdit.active && control.imageEdit.smartSegmentAvailable
                                implicitHeight: 30
                                leftPadding: 12
                                rightPadding: 12
                                helpText: qsTr("使用 SAM 2.1 智能识别选区内的主体轮廓，背景自动转为透明。")
                                onClicked: {
                                    control.applyCropSelection();
                                    control.forceActiveFocus();
                                }
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
                                font.pixelSize: Theme.fontCaption
                            }
                        }
                    }
                }
            }
        }
    }

    // File, view and edit actions share label-sized buttons. Short labels such as 100%
    // must not reserve the same width as an opening action and push useful tools onto a new row.
    component ImageCommandButton: ReviewActionButton {
        implicitHeight: 30
        leftPadding: 10
        rightPadding: 10
        // implicitContentWidth does not notify, so a binding to it keeps the value it had
        // during construction; bind the label's own width instead.
        implicitWidth: Math.max(64, contentItem.implicitWidth + leftPadding + rightPadding + 8)
        // Hand the focus back to the workspace root after the click: a held AbstractButton maps
        // Space to its own activation, so leaving focus on the button would make Space re-run
        // the last command instead of toggling A/B as the shortcut help promises. The transport
        // bar restores focus the same way after a click.
        focusTarget: control
    }

    // Left/Right/Up/Down walk folder pairs when a comparison list is loaded; with only a single
    // image open they step through that image's own directory instead (FLOW3). Home/End jump to
    // the first/last complete pair, or to the first/last neighbour in the single-image case.
    Keys.onLeftPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(-1);
            event.accepted = true;
        } else if (control.folderBrowseActive) {
            control.folderStepRequested(-1);
            event.accepted = true;
        }
    }
    Keys.onRightPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(1);
            event.accepted = true;
        } else if (control.folderBrowseActive) {
            control.folderStepRequested(1);
            event.accepted = true;
        }
    }
    Keys.onUpPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(-1);
            event.accepted = true;
        } else if (control.folderBrowseActive) {
            control.folderStepRequested(-1);
            event.accepted = true;
        }
    }
    Keys.onDownPressed: event => {
        if (control.hasFolders && folderPairSidebar) {
            folderPairSidebar.stepPair(1);
            event.accepted = true;
        } else if (control.folderBrowseActive) {
            control.folderStepRequested(1);
            event.accepted = true;
        }
    }
    // Manual in-place comparison: Space and T switch to (or flip) the single-source view
    // from every pair mode, not only from the single-image A/B mode.
    Keys.onSpacePressed: event => {
        if (control.hasPair && !control.editModeActive && !event.isAutoRepeat) {
            control.toggleSinglePairSource();
            event.accepted = true;
        }
    }
    Keys.onReleased: event => {
        // Peek ends on the key release (and on every other exit path tracked by endPeek).
        // autoRepeat releases (if a platform synthesizes them) must not end a still-held key.
        if (event.key === Qt.Key_QuoteLeft && !event.isAutoRepeat) {
            control.endPeek();
            event.accepted = true;
        }
    }
    Keys.onPressed: event => {
        if (event.key === Qt.Key_Home) {
            if (control.hasFolders && folderPairSidebar) {
                folderPairSidebar.stepFirst();
                event.accepted = true;
            } else if (control.folderBrowseActive) {
                control.folderJumpRequested(false);
                event.accepted = true;
            }
        } else if (event.key === Qt.Key_End) {
            if (control.hasFolders && folderPairSidebar) {
                folderPairSidebar.stepLast();
                event.accepted = true;
            } else if (control.folderBrowseActive) {
                control.folderJumpRequested(true);
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_T) {
            // Plain T only: Ctrl+T / Alt+T keep their platform meanings and must never
            // flip the pair. T stays the A/B in-place toggle synonym of Space.
            if (control.hasPair && !event.isAutoRepeat) {
                control.toggleSinglePairSource();
                event.accepted = true;
            }
        } else if (event.key === Qt.Key_QuoteLeft && !event.isAutoRepeat && control.peekModeAvailable && (event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier || event.modifiers === Qt.ShiftModifier)) {
            // Backtick is hold-to-peek, no longer a toggle: plain ` shows A, Shift+`
            // shows B while a difference or fade view is active; releasing restores the
            // snapshot. Ctrl/Alt variants keep their platform meanings, and an autoRepeat
            // press must never re-arm or overwrite the snapshot.
            control.beginPeek(event.modifiers === Qt.ShiftModifier);
            event.accepted = true;
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_W) {
            // E1: W toggles the wipe split axis, but only while a real wipe composite is on
            // screen (compare mode 5) - the same condition the wipe overlay itself uses.
            if (control.hasPair && control.compareMode === 5 && control.imageReview && !event.isAutoRepeat) {
                control.imageReview.wipeOrientation = control.imageReview.wipeOrientation === 1 ? 0 : 1;
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
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_P) {
            if (control.hasPrimary && !event.isAutoRepeat) {
                control.crosshairVerbose = !control.crosshairVerbose;
                event.accepted = true;
            }
        } else if ((event.modifiers === Qt.NoModifier || event.modifiers === Qt.KeypadModifier) && event.key === Qt.Key_Z) {
            // Same verb as a canvas double-click: flip fit-window and true 1:1 pixels.
            if (control.hasPrimary && !event.isAutoRepeat) {
                control.toggleTrueSize();
                event.accepted = true;
            }
        } else if ((event.key === Qt.Key_Delete || event.key === Qt.Key_Backspace) && control.editModeActive && control.imageEdit && control.imageEdit.selectedAnnotation >= 0) {
            control.imageEdit.deleteSelectedAnnotation();
            event.accepted = true;
        } else if ((event.key === Qt.Key_Return || event.key === Qt.Key_Enter) && control.editModeActive && control.cropSelection !== null && (control.editTool === "crop" || control.editTool === "mosaic" || control.editTool === "smart_select")) {
            control.applyCropSelection();
            event.accepted = true;
        } else if (event.key === Qt.Key_Escape && control.editModeActive && control.cropSelection !== null) {
            control.cropSelection = null;
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

                // Manual A/B comparison badge stays inside the image stage. It is a two-
                // segment switch: the filled dot and the border name the side currently on
                // screen (A = accent blue, B = amber), the other segment names where a click
                // (or Space) would go, so a few blinks can never leave the user unsure of
                // which side they are looking at.
                Rectangle {
                    id: imageInPlaceBadge
                    objectName: "imageInPlaceBadge"
                    visible: control.canSwitchSingleSource && control.compareMode === 0 && !control.peekActive
                    z: 30
                    width: Math.min(stageContent.width - 28, inPlaceBadgeContent.implicitWidth + 20)
                    height: 28
                    radius: Theme.radiusSubtle
                    color: Theme.oscGlass
                    border.width: 1
                    border.color: control.singleViewShowSecondary ? Theme.sourceBBorder : Theme.sourceABorder
                    anchors {
                        top: parent.top
                        right: parent.right
                        margins: 14
                    }

                    function selectSource(showSecondary) {
                        if (!control.canSwitchSingleSource)
                            return;
                        if (showSecondary === control.singleViewShowSecondary)
                            return;
                        control.toggleSinglePairSource();
                    }

                    Row {
                        id: inPlaceBadgeContent

                        spacing: 8
                        anchors.centerIn: parent

                        // Segment A
                        Item {
                            objectName: "imageInPlaceCurrentSegment"
                            // Explicit geometry: an Item's implicitWidth alone does not give
                            // it a hit area, so the click target would collapse to nothing.
                            width: inPlaceSegmentA.implicitWidth
                            height: 18
                            anchors.verticalCenter: parent.verticalCenter

                            Row {
                                id: inPlaceSegmentA
                                spacing: Theme.spaceMicro

                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: Theme.radiusMicro
                                    color: Theme.sourceA
                                    opacity: control.singleViewShowSecondary ? 0.35 : 1
                                    anchors.verticalCenter: parent.verticalCenter
                                }

                                Text {
                                    text: control.singleViewShowSecondary ? qsTr("A 原图") : qsTr("当前 A (原图)")
                                    color: control.singleViewShowSecondary ? Theme.mutedText : Theme.primaryText
                                    font.pixelSize: Theme.fontCaption
                                    font.weight: control.singleViewShowSecondary ? Font.Normal : Font.DemiBold
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            MouseArea {
                                objectName: "imageInPlaceSelectA"
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: imageInPlaceBadge.selectSource(false)
                            }
                        }

                        Rectangle {
                            width: 1
                            height: 14
                            color: Theme.oscBorder
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        // Segment B
                        Item {
                            objectName: "imageInPlaceSwitchSegment"
                            // Explicit geometry, as for segment A: the click target needs a
                            // real size, and an implicitWidth alone does not give it one.
                            width: inPlaceSegmentB.implicitWidth
                            height: 18
                            anchors.verticalCenter: parent.verticalCenter

                            Row {
                                id: inPlaceSegmentB
                                spacing: Theme.spaceMicro

                                Rectangle {
                                    width: 7
                                    height: 7
                                    radius: Theme.radiusMicro
                                    color: Theme.sourceB
                                    opacity: control.singleViewShowSecondary ? 1 : 0.35
                                    anchors.verticalCenter: parent.verticalCenter
                                }

                                Text {
                                    text: control.singleViewShowSecondary ? qsTr("当前 B (原图)") : qsTr("切至 B · 空格")
                                    color: control.singleViewShowSecondary ? Theme.primaryText : Theme.mutedText
                                    font.pixelSize: Theme.fontCaption
                                    font.weight: control.singleViewShowSecondary ? Font.DemiBold : Font.Normal
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            MouseArea {
                                objectName: "imageInPlaceSelectB"
                                anchors.fill: parent
                                cursorShape: Qt.PointingHandCursor
                                onClicked: imageInPlaceBadge.selectSource(true)
                            }
                        }
                    }
                }

                // Peek badge: while a difference/fade view is suppressed by a held backtick,
                // this plate names which original is on screen and that releasing restores
                // the previous view, in the same plate style as the A/B badge.
                Rectangle {
                    id: imagePeekBadge
                    objectName: "imagePeekBadge"
                    visible: control.peekActive
                    z: 30
                    width: Math.min(stageContent.width - 28, peekBadgeContent.implicitWidth + 20)
                    height: 28
                    radius: Theme.radiusSubtle
                    color: Theme.oscGlass
                    border.width: 1
                    border.color: control.peekSource === 1 ? Theme.sourceBBorder : Theme.sourceABorder
                    anchors {
                        top: parent.top
                        right: parent.right
                        margins: 14
                    }

                    Row {
                        id: peekBadgeContent

                        spacing: 7
                        anchors.centerIn: parent

                        Rectangle {
                            width: 7
                            height: 7
                            radius: Theme.radiusMicro
                            color: control.peekSource === 1 ? Theme.sourceB : Theme.sourceA
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Text {
                            text: qsTr("peek：%1 原图（松开 ` 恢复）").arg(control.peekSource === 1 ? "B" : "A")
                            color: Theme.primaryText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }
                }

                // Alpha & background observation HUD badge. Same plate as the A/B badge so the
                // two never look like unrelated widgets; the dot carries the state colour and
                // text names the state in words. The body click only cycles the background
                // (checker → black → white → dark → checker); channel observations keep their
                // own toolbar entries, and the separate × button restores the default
                // observation (RGBA view on the neutral checkerboard) in one step.
                Rectangle {
                    id: imageAlphaObservationBadge
                    objectName: "imageAlphaObservationBadge"
                    visible: control.hasPrimary && (control.viewMode !== 0 || control.backgroundMode !== 1)
                    z: 30
                    width: Math.min(stageContent.width - (imageInPlaceBadge.visible ? imageInPlaceBadge.width + 52 : 28), alphaBadgeContent.implicitWidth + 44)
                    height: 28
                    radius: Theme.radiusSubtle
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
                        anchors {
                            left: parent.left
                            leftMargin: 10
                            verticalCenter: parent.verticalCenter
                        }

                        Rectangle {
                            width: 7
                            height: 7
                            radius: Theme.radiusMicro
                            // All three view modes are observations, not alerts: information
                            // marks the two derived views and accent the plain one, so the
                            // warning yellow stays reserved for real warnings.
                            color: {
                                if (control.viewMode === 1 || control.viewMode === 2)
                                    return Theme.information;
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
                            text: qsTr("点击换底色")
                            color: Theme.mutedText
                            font.pixelSize: Theme.fontCaption
                            anchors.verticalCenter: parent.verticalCenter
                        }
                    }

                    // One-step restore of the default observation state.
                    Rectangle {
                        id: alphaObservationResetButton
                        objectName: "imageAlphaObservationResetButton"
                        width: 22
                        height: 22
                        radius: Theme.radiusMicro
                        anchors {
                            right: parent.right
                            rightMargin: 4
                            verticalCenter: parent.verticalCenter
                        }
                        color: alphaResetMouse.containsMouse ? Theme.fluentHover : "transparent"
                        border.width: 1
                        border.color: alphaResetMouse.containsMouse ? Theme.borderHover : Theme.controlBorder

                        Text {
                            anchors.centerIn: parent
                            text: "×"
                            color: alphaResetMouse.containsMouse ? Theme.primaryText : Theme.mutedText
                            font.pixelSize: 12
                            font.weight: Font.DemiBold
                        }

                        MouseArea {
                            id: alphaResetMouse
                            anchors.fill: parent
                            hoverEnabled: true
                            cursorShape: Qt.PointingHandCursor
                            onClicked: {
                                if (control.imageReview)
                                    control.imageReview.viewMode = 0;
                                control.backgroundMode = 1;
                            }
                        }

                        HoverHandler {
                            id: alphaResetHover
                        }

                        VcsToolTip {
                            visible: alphaResetHover.hovered
                            text: qsTr("还原默认观察（RGBA 视图 + 棋盘格底色）")
                        }
                    }

                    MouseArea {
                        // Body click cycles the background only, never the channel view.
                        anchors {
                            left: parent.left
                            top: parent.top
                            bottom: parent.bottom
                            right: alphaObservationResetButton.left
                        }
                        cursorShape: Qt.PointingHandCursor
                        onClicked: control.backgroundMode = (control.backgroundMode + 1) % 4
                    }
                }

                Row {
                    visible: control.hasPrimary && (control.compareMode === 0 || control.compareMode === 1 || !control.hasPair)
                    anchors.fill: parent
                    spacing: 12

                    ImageViewport {
                        id: primaryViewport

                        objectName: "primaryViewport"
                        slot: control.editModeActive ? control.editSourceSlot : ((control.compareMode === 0 && control.hasPair && control.singleViewShowSecondary) ? 3 : 2)
                        width: control.compareMode === 1 && control.hasPair ? parent.width / 2 - 6 : parent.width
                        height: parent.height
                        // While editing, the pane shows the working copy; the compared
                        // originals (and every difference statistic) stay untouched.
                        imageUrl: control.editModeActive && control.editSourceSlot === slot && control.imageEdit ? control.imageEdit.editedImageUrl : (control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(slot) : "")
                        label: {
                            if (control.editModeActive && control.editSourceSlot === slot)
                                return qsTr("%1 · 编辑副本 · %2×%3").arg(slot === 3 ? "B" : "A").arg(control.imageEdit.imageWidth).arg(control.imageEdit.imageHeight);
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
                    // The diff image only exists while this viewport is visible; requesting
                    // image://vcs-review/4 otherwise makes the provider fail (slot 4 has no
                    // source image) and pollutes the hover chain with load errors.
                    imageUrl: visible && control.imageReview && control.imageReview.contentGeneration >= 0 ? control.imageReview.imageUrl(4) : ""
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
                    // E1: wipe split axis. 0 = Vertical (left/right), 1 = Horizontal
                    // (top/bottom). Owned by ImageReviewController like wipePosition; the
                    // clip rect, identity plates, hover side selection and the handle all
                    // read this same value.
                    readonly property int wipeOrientation: control.imageReview ? control.imageReview.wipeOrientation : 0
                    readonly property bool wipeHorizontal: wipeOrientation === 1
                    readonly property real splitY: height * control.wipePosition
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

                    ImagePixelGrid {
                        objectName: "imagePixelGrid-wipeA"
                        imageItem: wipePrimaryImage
                        viewportItem: wipeOverlay
                    }

                    Item {
                        id: wipeClip

                        objectName: "wipeClip"
                        width: wipeOverlay.wipeHorizontal ? wipeOverlay.width : Math.round(wipeOverlay.splitX)
                        height: wipeOverlay.wipeHorizontal ? Math.round(wipeOverlay.splitY) : wipeOverlay.height
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
                        ImagePixelGrid {
                            objectName: "imagePixelGrid-wipeB"
                            imageItem: wipeSecondaryImage
                            viewportItem: wipeClip
                        }
                    }

                    // T6: persistent A/B identity for the wipe halves. The first side of the
                    // split shows the secondary image and the other side shows the primary
                    // (left/right in vertical, top/bottom in horizontal), so the labels name
                    // them in that order and can never be misread as reversed while the
                    // split is dragged. Both ride on translucent plates like the viewport
                    // labels, so they stay readable over bright imagery.
                    Rectangle {
                        visible: control.hasPair && control.hasSecondary && (wipeOverlay.wipeHorizontal ? wipeOverlay.splitY > 44 : wipeOverlay.splitX > 44)
                        width: wipeIdentityLeft.width + 18
                        height: wipeIdentityLeft.implicitHeight + 12
                        radius: Theme.radiusSmall
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
                            width: Math.max(0, (wipeOverlay.wipeHorizontal ? wipeOverlay.width : wipeOverlay.splitX) - 36)
                            anchors.centerIn: parent
                        }
                    }

                    Rectangle {
                        visible: control.hasPair && (wipeOverlay.wipeHorizontal ? (wipeOverlay.height - wipeOverlay.splitY) > 44 : (wipeOverlay.width - wipeOverlay.splitX) > 44)
                        width: wipeIdentityRight.width + 18
                        height: wipeIdentityRight.implicitHeight + 12
                        radius: Theme.radiusSmall
                        color: Theme.stageLabel
                        border.width: 1
                        border.color: Theme.stageLabelBorder
                        anchors {
                            bottom: wipeOverlay.wipeHorizontal ? parent.bottom : undefined
                            right: wipeOverlay.wipeHorizontal ? undefined : parent.right
                            top: wipeOverlay.wipeHorizontal ? undefined : parent.top
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
                            width: Math.max(0, (wipeOverlay.wipeHorizontal ? wipeOverlay.width : wipeOverlay.width - wipeOverlay.splitX) - 36)
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
                                const baseW = Math.max(1, wipeOverlay.drawWidth / Math.max(0.001, control.zoom));
                                const baseH = Math.max(1, wipeOverlay.drawHeight / Math.max(0.001, control.zoom));
                                const dx = (mouse.x - dragStart.x) / baseW;
                                const dy = (mouse.y - dragStart.y) / baseH;
                                control.imageReview.panBy(dx, dy);
                                dragStart = Qt.point(mouse.x, mouse.y);
                            }
                            const firstSide = wipeOverlay.wipeHorizontal ? mouse.y < wipeOverlay.splitY : mouse.x < wipeOverlay.splitX;
                            control.applyHover({
                                "image": firstSide ? wipeSecondaryImage : wipePrimaryImage
                            }, mouse.x, mouse.y, firstSide ? 3 : 2);
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
                                    if (m0 && m1 && !(m0.x >= wipePrimaryImage.sourceSize.width || m0.y >= wipePrimaryImage.sourceSize.height || m1.x <= 0 || m1.y <= 0)) {
                                        const sw = wipePrimaryImage.sourceSize.width;
                                        const sh = wipePrimaryImage.sourceSize.height;
                                        const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                                        const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                                        const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                                        const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                                        if (control.imageReview)
                                            control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                                    } else {
                                        control.showMarqueeNotice(qsTr("框选超出画面范围，未放大"));
                                    }
                                } else if (Math.abs(right - left) >= 8 || Math.abs(bottom - top) >= 8) {
                                    control.showMarqueeNotice(qsTr("框选区域太小（至少 8×8 像素），未放大"));
                                }
                            }
                        }
                        onDoubleClicked: mouse => {
                            control.toggleTrueSize();
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
                            const steps = wheel.angleDelta.y / 120.0;
                            const factor = Math.pow(1.15, steps);
                            control.imageReview.zoomBy(factor, ax, ay);
                            wheel.accepted = true;
                        }
                    }

                    WipeHandle {
                        surfaceItem: wipeOverlay
                        orientation: wipeOverlay.wipeOrientation
                        position: control.wipePosition
                        onPositionRequested: position => control.imageReview.wipePosition = position
                    }

                    // Linked crosshair: the pointer's normalized coordinate over the shared
                    // primary-geometry box drives the probe lines, so wipe mode keeps the
                    // pixel-accurate coordinates of the other modes.
                    OverlayCrosshair {
                        imageItem: wipePrimaryImage
                        key: "wipe"
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

                    ImagePixelGrid {
                        objectName: "imagePixelGrid-fade"
                        imageItem: fadePrimaryImage
                        viewportItem: fadeOverlay
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
                                const baseW = Math.max(1, fadeOverlay.drawWidth / Math.max(0.001, control.zoom));
                                const baseH = Math.max(1, fadeOverlay.drawHeight / Math.max(0.001, control.zoom));
                                const dx = (mouse.x - dragStart.x) / baseW;
                                const dy = (mouse.y - dragStart.y) / baseH;
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
                                    if (m0 && m1 && !(m0.x >= fadePrimaryImage.sourceSize.width || m0.y >= fadePrimaryImage.sourceSize.height || m1.x <= 0 || m1.y <= 0)) {
                                        const sw = fadePrimaryImage.sourceSize.width;
                                        const sh = fadePrimaryImage.sourceSize.height;
                                        const normX0 = Math.max(0, Math.min(1, m0.x / sw));
                                        const normY0 = Math.max(0, Math.min(1, m0.y / sh));
                                        const normX1 = Math.max(0, Math.min(1, m1.x / sw));
                                        const normY1 = Math.max(0, Math.min(1, m1.y / sh));
                                        if (control.imageReview)
                                            control.imageReview.zoomToRect(normX0, normY0, normX1, normY1);
                                    } else {
                                        control.showMarqueeNotice(qsTr("框选超出画面范围，未放大"));
                                    }
                                } else if (Math.abs(right - left) >= 8 || Math.abs(bottom - top) >= 8) {
                                    control.showMarqueeNotice(qsTr("框选区域太小（至少 8×8 像素），未放大"));
                                }
                            }
                        }
                        onDoubleClicked: mouse => {
                            control.toggleTrueSize();
                        }
                        onWheel: wheel => {
                            if (!control.imageReview)
                                return;
                            const mapped = control.mapToImage({
                                "image": fadePrimaryImage
                            }, wheel.x, wheel.y);
                            const ax = mapped ? Math.max(0, Math.min(1, mapped.x / Math.max(1, fadePrimaryImage.sourceSize.width))) : 0.5;
                            const ay = mapped ? Math.max(0, Math.min(1, mapped.y / Math.max(1, fadePrimaryImage.sourceSize.height))) : 0.5;
                            const steps = wheel.angleDelta.y / 120.0;
                            const factor = Math.pow(1.15, steps);
                            control.imageReview.zoomBy(factor, ax, ay);
                            wheel.accepted = true;
                        }
                    }

                    // Linked crosshair over the blend, driven by the same normalized
                    // coordinate mapping as wipe (the hover samples the primary side).
                    OverlayCrosshair {
                        imageItem: fadePrimaryImage
                        key: "fade"
                    }
                }

                // View-command HUD, bottom-left of the canvas: the same glass chips as the
                // video viewport's viewCommandRow, so zoom/base/reset live in one muscle
                // memory across both workspaces. The marquee hint names the Shift gesture
                // that the canvas already understands.
                Row {
                    id: imageViewCommandHud
                    objectName: "imageViewCommandHud"
                    visible: control.hasPrimary
                    z: 30
                    spacing: Theme.spaceMain
                    anchors {
                        left: parent.left
                        leftMargin: 12
                        bottom: parent.bottom
                        bottomMargin: 12
                    }

                    HudCommandButton {
                        objectName: "imageFitButton"
                        checkable: true
                        checked: !control.trueSize
                        mirrorsState: true
                        enabled: control.hasPrimary
                        text: qsTr("适应窗口")
                        helpText: qsTr("按窗口大小显示整张图片（快捷键 Z 在此与 100% 之间切换）。")
                        onClicked: {
                            control.trueSize = false;
                            if (control.imageReview)
                                control.imageReview.resetView();
                        }
                    }
                    HudCommandButton {
                        objectName: "imageTrueSizeButton"
                        checkable: true
                        checked: control.trueSize
                        mirrorsState: true
                        enabled: control.hasPrimary
                        text: qsTr("100% (%1%)").arg(control.displayPercent)
                        helpText: qsTr("1 个图像像素 = 1 个物理像素；括号内是当前实际显示比例。双击画面或按 Z 可切回适应窗口。")
                        onClicked: {
                            control.trueSize = true;
                            if (control.imageReview)
                                control.imageReview.setZoom(1.0);
                        }
                    }
                    HudCommandButton {
                        objectName: "imageResetViewButton"
                        enabled: control.hasPrimary
                        text: qsTr("重置")
                        helpText: qsTr("回到适应窗口并居中（清除缩放与平移）。")
                        onClicked: {
                            control.trueSize = false;
                            if (control.imageReview)
                                control.imageReview.resetView();
                        }
                    }
                    HudCommandButton {
                        objectName: "imagePixelGridButton"
                        text: control.pixelGridEnabled ? qsTr("网格 开") : qsTr("网格 关")
                        helpText: qsTr("像素网格：仅在最近邻采样且实际放大达到 800% 时显示。")
                        onClicked: control.pixelGridEnabled = !control.pixelGridEnabled
                    }
                    Rectangle {
                        id: imageMarqueeHintChip
                        objectName: "imageMarqueeHintChip"
                        height: 24
                        implicitWidth: marqueeHintText.implicitWidth + 16
                        radius: Theme.radiusMicro
                        color: Theme.oscGlass
                        border.width: 1
                        border.color: Theme.oscBorder

                        Row {
                            id: marqueeHintText
                            spacing: Theme.spaceMicro
                            anchors.centerIn: parent

                            Text {
                                text: qsTr("框选放大")
                                color: Theme.mutedText
                                font.pixelSize: Theme.fontCaption
                                anchors.verticalCenter: parent.verticalCenter
                            }
                        }

                        HoverHandler {
                            id: marqueeHintHover
                        }

                        VcsToolTip {
                            visible: marqueeHintHover.hovered
                            text: qsTr("按住 Shift 并在画面上拖动，框出要放大的区域（至少 8×8 像素）；在画面外松开会提示并取消。")
                        }
                    }
                }
            }
        }
    }

    // Fade blend control floats over the viewport so it never occupies toolbar space;
    // 0 shows A, 1 shows B, and the readout names both sides so the direction cannot be
    // misread while dragging (semantics unchanged from the previous inline slider). The
    // snap chips park the blend on the three review positions, and the wheel over the
    // plate adjusts it in ±5% steps without hunting for the handle.
    Rectangle {
        id: fadeSliderOverlay

        visible: control.hasPair && control.compareMode === 7
        width: fadeRow.implicitWidth + 24
        height: 36
        radius: Theme.radiusSmall
        color: Theme.oscGlass
        border.width: 1
        border.color: Theme.oscBorder
        anchors {
            right: stage.right
            bottom: stage.bottom
            margins: 14
        }
        z: 20

        WheelHandler {
            objectName: "imageFadeWheelHandler"
            acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
            onWheel: event => {
                if (!control.imageReview)
                    return;
                const step = event.angleDelta.y / 120 * 0.05;
                const next = Math.max(0, Math.min(1, Number(control.imageReview.fadePosition) + step));
                control.imageReview.fadePosition = next;
                event.accepted = true;
            }
        }

        Row {
            id: fadeRow

            spacing: 10
            anchors.centerIn: parent

            Rectangle {
                id: fadeSnap0
                objectName: "imageFadeSnap0"
                width: 36
                height: Theme.hitAreaMinimum
                radius: Theme.radiusMicro
                color: fadeSnap0Mouse.pressed ? Theme.fluentPressed : (control.imageReview && control.imageReview.fadePosition <= 0.001 ? Theme.controlChecked : (fadeSnap0Mouse.containsMouse ? Theme.fluentHover : "transparent"))
                border.width: 1
                border.color: fadeSnap0Mouse.containsMouse ? Theme.borderHover : Theme.controlBorder

                Text {
                    anchors.centerIn: parent
                    text: qsTr("全A")
                    color: Theme.secondaryText
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    id: fadeSnap0Mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (control.imageReview)
                            control.imageReview.fadePosition = 0;
                    }
                }
            }
            Rectangle {
                id: fadeSnap50
                objectName: "imageFadeSnap50"
                width: 36
                height: Theme.hitAreaMinimum
                radius: Theme.radiusMicro
                color: fadeSnap50Mouse.pressed ? Theme.fluentPressed : (control.imageReview && Math.abs(control.imageReview.fadePosition - 0.5) <= 0.001 ? Theme.controlChecked : (fadeSnap50Mouse.containsMouse ? Theme.fluentHover : "transparent"))
                border.width: 1
                border.color: fadeSnap50Mouse.containsMouse ? Theme.borderHover : Theme.controlBorder

                Text {
                    anchors.centerIn: parent
                    text: "50%"
                    color: Theme.secondaryText
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    id: fadeSnap50Mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (control.imageReview)
                            control.imageReview.fadePosition = 0.5;
                    }
                }
            }
            Rectangle {
                id: fadeSnap100
                objectName: "imageFadeSnap100"
                width: 36
                height: Theme.hitAreaMinimum
                radius: Theme.radiusMicro
                color: fadeSnap100Mouse.pressed ? Theme.fluentPressed : (control.imageReview && control.imageReview.fadePosition >= 0.999 ? Theme.controlChecked : (fadeSnap100Mouse.containsMouse ? Theme.fluentHover : "transparent"))
                border.width: 1
                border.color: fadeSnap100Mouse.containsMouse ? Theme.borderHover : Theme.controlBorder

                Text {
                    anchors.centerIn: parent
                    text: qsTr("全B")
                    color: Theme.secondaryText
                    font.pixelSize: Theme.fontCaption
                }

                MouseArea {
                    id: fadeSnap100Mouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: {
                        if (control.imageReview)
                            control.imageReview.fadePosition = 1;
                    }
                }
            }

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
        // The bar grows with the readout it carries: a two-line probe block (or a pair
        // readout that wrapped in a narrow window) needs its lines, and the alternative -
        // squeezing the numbers into a shorter row - is exactly what this layout removed.
        // Reserve a readout row before the first sample so hovering cannot resize the image
        // underneath the pointer and change the coordinate it names.
        height: statusHints.height + Math.max(Theme.hitAreaMinimum, statusDetails.implicitHeight) + Theme.spaceMain
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
                right: parent.right
                rightMargin: 14
                top: statusHints.bottom
                topMargin: Theme.spaceMicro
            }

            // Structured two-line probe readout. The old single sentence carried the
            // coordinate, RGBA, the hex value, the alpha percentage, the observation label
            // and the high-depth detail in one elided Text: at the 960 px floor the hex and
            // the trailing detail were cut off and only a tooltip could show them. The
            // fields are now separate fixed-width monospace Texts laid out as a Flow, so a
            // narrow window wraps them onto further lines instead of hiding them, and a
            // value 1 -> 255 or a coordinate gaining a digit reshapes nothing.
            Item {
                id: pixelReadout
                objectName: "imagePixelReadout"
                // The single-side sentence (native-depth detail, edit-copy coordinates)
                // yields the row while the pair columns are live: the same-coordinate
                // A/B/Δ readout is the comparison's hard requirement, not a second copy
                // of one side's values.
                visible: control.readoutPairSample === null && Boolean((control.editModeActive && control.hoverPoint) || (control.statusPixel && control.statusPixel.valid))
                // Bounded field: the block never pushes the zoom or folder context out of
                // the bar, and the Flow inside wraps rather than clips when the window (or
                // an open sidebar) leaves less room than the fields need.
                Layout.fillWidth: true
                Layout.minimumWidth: 240
                implicitHeight: readoutFlow.height

                readonly property bool showNativeDetail: !control.editModeActive && control.statusPixel && control.statusPixel.nativeBitDepth !== undefined && control.statusPixel.nativeBitDepth > 8

                Flow {
                    id: readoutFlow

                    spacing: 6
                    width: parent.width

                    Text {
                        objectName: "imagePixelReadoutCoordinate"
                        text: {
                            if (control.editModeActive)
                                return control.hoverPoint ? qsTr("编辑副本 · 像素 (%1, %2)").arg(control.hoverPoint.imgX).arg(control.hoverPoint.imgY) : "";
                            if (!control.statusPixel || !control.statusPixel.valid)
                                return "";
                            return qsTr("像素 (%1, %2)").arg(control.statusPixel.x).arg(control.statusPixel.y);
                        }
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePixelReadoutHex"
                        // Always #RRGGBBAA: the alpha byte is never dropped and the order is
                        // never ARGB, so the value in the status bar is the value the copy
                        // button puts on the clipboard.
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        text: {
                            if (!control.statusPixel || !control.statusPixel.valid)
                                return "";
                            return control.imageReview.hexText(control.statusPixel.r, control.statusPixel.g, control.statusPixel.b, control.statusPixel.a);
                        }
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                    }
                    Text {
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        text: !control.editModeActive && control.statusPixel && control.statusPixel.valid ? qsTr("α %1%").arg(control.statusPixel.alphaPercent) : ""
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        text: {
                            if (!control.statusPixel || !control.statusPixel.valid)
                                return "";
                            if (control.statusPixel.channelView === "alphaGray")
                                return qsTr("观察 · Alpha 灰度");
                            if (control.statusPixel.channelView === "rgbOpaque")
                                return qsTr("观察 · RGB 忽略透明度");
                            if (control.imageReview && (control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted))
                                return qsTr("显示 RGBA8");
                            return qsTr("原码值");
                        }
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    // A pixel the active alpha threshold hides keeps its original RGBA here
                    // and says so: the filter changes the picture, never the reading.
                    Text {
                        objectName: "imagePixelReadoutAlphaHidden"
                        visible: Boolean(control.statusPixel && control.statusPixel.valid && control.statusPixel.alphaHidden === true)
                        text: qsTr("已被当前 Alpha 阈值隐藏")
                        color: Theme.warningText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePixelColR"
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        width: control.channelColumnWidth
                        horizontalAlignment: Text.AlignRight
                        text: control.statusPixel && control.statusPixel.valid ? "R " + control.fmtChannel(control.statusPixel.r) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePixelColG"
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        width: control.channelColumnWidth
                        horizontalAlignment: Text.AlignRight
                        text: control.statusPixel && control.statusPixel.valid ? "G " + control.fmtChannel(control.statusPixel.g) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePixelColB"
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        width: control.channelColumnWidth
                        horizontalAlignment: Text.AlignRight
                        text: control.statusPixel && control.statusPixel.valid ? "B " + control.fmtChannel(control.statusPixel.b) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePixelColA"
                        visible: !control.editModeActive && Boolean(control.statusPixel && control.statusPixel.valid)
                        width: control.channelColumnWidth
                        horizontalAlignment: Text.AlignRight
                        text: control.statusPixel && control.statusPixel.valid ? "A " + control.fmtChannel(control.statusPixel.a) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    // I-02: a >8-bit source also reports its original-depth code values so the
                    // readout never implies the 8-bit display numbers are the file's codes.
                    // The numbers follow the same integer/float switch and stay labelled as
                    // converted values, never as raw file planes.
                    Text {
                        objectName: "imagePixelReadoutNative"
                        visible: pixelReadout.showNativeDetail
                        text: {
                            const sample = control.statusPixel;
                            if (!pixelReadout.showNativeDetail || !sample)
                                return "";
                            // Read, not consumed: the binding must re-evaluate when the
                            // numeric format flips even though the sample is unchanged
                            // (invokable state reads are invisible to QML).
                            const bindingDependency = control.numericFloat;
                            return qsTr("源 %1-bit → 转换后 16-bit：A %2（%3%） R %4 G %5 B %6").arg(sample.nativeBitDepth).arg(control.imageReview.channelText16(sample.a16)).arg((Number(sample.a16) / 65535 * 100).toFixed(4)).arg(control.imageReview.channelText16(sample.r16)).arg(control.imageReview.channelText16(sample.g16)).arg(control.imageReview.channelText16(sample.b16));
                        }
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                }

                HoverHandler {
                    id: pixelReadoutHover
                }

                VcsToolTip {
                    visible: pixelReadoutHover.hovered && control.readoutPairSample === null && Boolean(control.statusPixel && control.statusPixel.valid)
                    // pixelReadoutSentence is declared on the enclosing statusDetails
                    // RowLayout, not on `control` and not on the readout Item itself: a wrong
                    // scope here silently kills the whole tooltip with a TypeError.
                    text: statusDetails.pixelReadoutSentence()
                }
            }

            // The plain one-line sentence, kept as the tooltip and as the single source of
            // truth for the numbers a test can read back.
            function pixelReadoutSentence() {
                if (control.editModeActive)
                    return control.hoverPoint ? qsTr("编辑副本 · 像素 (%1, %2)").arg(control.hoverPoint.imgX).arg(control.hoverPoint.imgY) : "";
                const sample = control.statusPixel;
                if (!sample || !sample.valid)
                    return "";
                const converted = Boolean(control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted);
                let text = converted ? qsTr("像素 (%1, %2) [显示 RGBA8: R %3 G %4 B %5 A %6 %7 α %8%]").arg(sample.x).arg(sample.y).arg(control.fmtChannel(sample.r)).arg(control.fmtChannel(sample.g)).arg(control.fmtChannel(sample.b)).arg(control.fmtChannel(sample.a)).arg(control.imageReview.hexText(sample.r, sample.g, sample.b, sample.a)).arg(sample.alphaPercent) : qsTr("像素 (%1, %2)  R %3  G %4  B %5  A %6  %7  α %8%").arg(sample.x).arg(sample.y).arg(control.fmtChannel(sample.r)).arg(control.fmtChannel(sample.g)).arg(control.fmtChannel(sample.b)).arg(control.fmtChannel(sample.a)).arg(control.imageReview.hexText(sample.r, sample.g, sample.b, sample.a)).arg(sample.alphaPercent);
                if (sample.channelView === "alphaGray")
                    text += " · " + qsTr("Alpha 灰度");
                else if (sample.channelView === "rgbOpaque")
                    text += " · " + qsTr("RGB 忽略透明度");
                if (sample.alphaHidden === true)
                    text += " · " + qsTr("已被当前 Alpha 阈值隐藏");
                if (sample.nativeBitDepth !== undefined && sample.nativeBitDepth > 8)
                    text += " · " + qsTr("源 %1-bit → 转换后 16-bit：A %2（%3%） R %4 G %5 B %6").arg(sample.nativeBitDepth).arg(sample.a16).arg((Number(sample.a16) / 65535 * 100).toFixed(4)).arg(sample.r16).arg(sample.g16).arg(sample.b16);
                return text;
            }

            // Same-coordinate pair readout as real fixed-width columns: every channel is one
            // right-aligned fixed-width monospace Text, so a value 1→255 or a coordinate
            // gaining a digit reshapes nothing — each column keeps its start, and the A/B/Δ
            // triple stays readable down to the 960px floor. The columns live in a Flow, so
            // a narrow window (or an open sidebar) wraps the A, B and Δ groups onto further
            // lines instead of truncating the coordinates or the hex values. Delta columns
            // carry the beyond-threshold warning colour at the column level (no rich text,
            // no collapsed spaces, no elide on the numbers themselves).
            Item {
                id: pairPixelReadout
                objectName: "imagePairPixelReadout"
                visible: control.readoutPairSample !== null
                Layout.fillWidth: true
                Layout.minimumWidth: 240
                implicitHeight: pairReadoutFlow.height

                readonly property var sample: control.readoutPairSample
                readonly property bool aOk: Boolean(sample && sample.a && sample.a.valid)
                readonly property var b: sample ? sample.b : null
                readonly property bool bOk: Boolean(b && b.valid)
                readonly property bool bNamed: Boolean(b && b.unaligned)
                // Channel deltas for the four Δ columns, null-guarded so the colour and text
                // bindings stay valid in every state.
                readonly property int dR: aOk && bOk ? Number(b.r) - Number(sample.a.r) : 0
                readonly property int dG: aOk && bOk ? Number(b.g) - Number(sample.a.g) : 0
                readonly property int dB: aOk && bOk ? Number(b.b) - Number(sample.a.b) : 0
                readonly property int dA: aOk && bOk ? Number(b.a) - Number(sample.a.a) : 0
                readonly property bool deltaReady: aOk && bOk
                // dR/dG/dB/dA above stay in raw 8-bit steps in either display format.
                readonly property real deltaWarnStep: control.pairDeltaWarnThreshold

                function deltaExceeds(value) {
                    return Math.abs(Number(value)) > pairPixelReadout.deltaWarnStep + 1e-9;
                }

                // #RRGGBBAA for one side, or "" when the side has no valid sample. The same
                // helper the copy button uses, so the bar and the clipboard cannot differ.
                function sideHex(sample) {
                    if (!sample || sample.valid !== true)
                        return "";
                    return String(control.imageReview.hexText(sample.r, sample.g, sample.b, sample.a));
                }

                Flow {
                    id: pairReadoutFlow

                    spacing: 4
                    width: parent.width

                    Text {
                        objectName: "imagePairCoordA"
                        visible: pairPixelReadout.aOk
                        width: 112
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.aOk ? "A (" + pairPixelReadout.sample.a.x + ", " + pairPixelReadout.sample.a.y + ")" : ""
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairHexA"
                        visible: pairPixelReadout.aOk
                        width: 78
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        // `visible` only hides the item; the binding still evaluates. Without
                        // the guard a window that never had a pair (or one whose pair was just
                        // closed) dereferences a null `sample` on every re-evaluation.
                        text: pairPixelReadout.aOk ? pairPixelReadout.sideHex(pairPixelReadout.sample.a) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColAR"
                        visible: pairPixelReadout.aOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.aOk ? control.fmtChannel(pairPixelReadout.sample.a.r) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColAG"
                        visible: pairPixelReadout.aOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.aOk ? control.fmtChannel(pairPixelReadout.sample.a.g) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColAB"
                        visible: pairPixelReadout.aOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.aOk ? control.fmtChannel(pairPixelReadout.sample.a.b) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColAAlpha"
                        visible: pairPixelReadout.aOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.aOk ? control.fmtChannel(pairPixelReadout.sample.a.a) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        visible: !pairPixelReadout.aOk
                        width: 224
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        text: qsTr("A: 无有效值")
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        elide: Text.ElideRight
                    }

                    Text {
                        objectName: "imagePairCoordB"
                        visible: pairPixelReadout.bOk
                        width: 112
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bOk ? "B (" + pairPixelReadout.b.x + ", " + pairPixelReadout.b.y + ")" : ""
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairHexB"
                        visible: pairPixelReadout.bOk
                        width: 78
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.sideHex(pairPixelReadout.b)
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColBR"
                        visible: pairPixelReadout.bOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bOk ? control.fmtChannel(pairPixelReadout.b.r) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColBG"
                        visible: pairPixelReadout.bOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bOk ? control.fmtChannel(pairPixelReadout.b.g) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColBB"
                        visible: pairPixelReadout.bOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bOk ? control.fmtChannel(pairPixelReadout.b.b) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColBAlpha"
                        visible: pairPixelReadout.bOk
                        width: control.channelColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bOk ? control.fmtChannel(pairPixelReadout.b.a) : ""
                        color: Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        visible: !pairPixelReadout.bOk
                        width: 224
                        height: 20
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.bNamed ? qsTr("B: 未对齐无有效值") : qsTr("B: 无有效值")
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        elide: Text.ElideRight
                    }

                    Text {
                        objectName: "imagePairDeltaLabel"
                        visible: pairPixelReadout.deltaReady
                        width: 56
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: "Δ(B−A)"
                        color: Theme.mutedText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        objectName: "imagePairColDeltaR"
                        visible: pairPixelReadout.deltaReady
                        width: control.deltaColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.deltaReady ? control.fmtSigned(pairPixelReadout.sample.a.r, pairPixelReadout.b.r) : ""
                        color: pairPixelReadout.deltaReady && pairPixelReadout.deltaExceeds(pairPixelReadout.dR) ? Theme.warningText : Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "imagePairColDeltaG"
                        visible: pairPixelReadout.deltaReady
                        width: control.deltaColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.deltaReady ? control.fmtSigned(pairPixelReadout.sample.a.g, pairPixelReadout.b.g) : ""
                        color: pairPixelReadout.deltaReady && pairPixelReadout.deltaExceeds(pairPixelReadout.dG) ? Theme.warningText : Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "imagePairColDeltaB"
                        visible: pairPixelReadout.deltaReady
                        width: control.deltaColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.deltaReady ? control.fmtSigned(pairPixelReadout.sample.a.b, pairPixelReadout.b.b) : ""
                        color: pairPixelReadout.deltaReady && pairPixelReadout.deltaExceeds(pairPixelReadout.dB) ? Theme.warningText : Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                    }
                    Text {
                        objectName: "imagePairColDeltaAlpha"
                        visible: pairPixelReadout.deltaReady
                        width: control.deltaColumnWidth
                        height: 20
                        horizontalAlignment: Text.AlignRight
                        verticalAlignment: Text.AlignVCenter
                        text: pairPixelReadout.deltaReady ? control.fmtSigned(pairPixelReadout.sample.a.a, pairPixelReadout.b.a) : ""
                        color: pairPixelReadout.deltaReady && pairPixelReadout.deltaExceeds(pairPixelReadout.dA) ? Theme.warningText : Theme.primaryText
                        font.pixelSize: 12
                        font.family: Theme.fontFamilyMono
                        font.weight: Font.DemiBold
                    }
                }

                HoverHandler {
                    id: pairReadoutHover
                }

                VcsToolTip {
                    visible: pairReadoutHover.hovered
                    text: control.pairReadoutText()
                }
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

        Flow {
            id: statusHints
            objectName: "imageStatusHints"
            onWidthChanged: forceLayout()
            anchors {
                left: parent.left
                leftMargin: 14
                right: parent.right
                rightMargin: 14
                top: parent.top
                topMargin: Theme.spaceMicro
            }
            spacing: 12

            Text {
                objectName: "imageZoomReadout"
                text: qsTr("缩放 %1%（%2）").arg(control.displayPercent).arg(control.displayPercentMode)
                color: Theme.primaryText
                font.pixelSize: 12
                // The one readout the theme's tnum token was written for: zooming re-renders
                // these digits in place, and proportional figures made the whole status row
                // shift left and right on every step (UX-18).
                font.features: Theme.fontFeaturesNumeric
                height: Theme.hitAreaMinimum
                verticalAlignment: Text.AlignVCenter
            }
            // T6: current folder-row context — which row is committed, whether one side
            // is missing, and whether the pairing is ambiguous.
            Text {
                objectName: "imagePairContextLabel"
                height: Theme.hitAreaMinimum
                verticalAlignment: Text.AlignVCenter
                visible: Boolean(control.pairModel && control.pairModel.pairCount > 0 && control.pairModel.currentPair >= 0)
                text: {
                    const model = control.pairModel;
                    if (!model || model.pairCount <= 0)
                        return "";
                    // Number(undefined) is NaN, and NaN fails every comparison, so a
                    // model without a current pair must be rejected with !(row >= 0)
                    // instead of a bare `< 0` guard before touching row helpers.
                    const row = Number(model.currentPair);
                    if (!(row >= 0) || typeof model.pairUrlsAt !== "function")
                        return "";
                    const info = model.pairUrlsAt(row);
                    if (!info || info.row === undefined)
                        return "";
                    const parts = [qsTr("第 %1/%2 行").arg(Number(info.row) + 1).arg(model.pairCount)];
                    if (!info.hasBoth)
                        parts.push(info.hasLeft ? qsTr("仅 A 存在") : qsTr("仅 B 存在"));
                    if (info.caseConflict)
                        parts.push(qsTr("大小写冲突"));
                    return parts.join(" · ");
                }
                color: Theme.mutedText
                font.pixelSize: 12
                elide: Text.ElideRight
            }
            // ---- Numeric format -------------------------------------------------------------
            // One compact selector for the whole readout family. Integer code values or
            // normalized float; Hex stays hexadecimal either way. It only changes what is
            // printed and copied, never the image, a sampling value, the difference field
            // or a statistic.
            ImageCommandButton {
                objectName: "imageNumericFormatButton"
                implicitHeight: Theme.hitAreaMinimum
                leftPadding: 8
                rightPadding: 8
                text: control.numericFloat ? qsTr("数值：归一化浮点 ▾") : qsTr("数值：整数码值 ▾")
                enabled: control.hasPrimary
                helpText: qsTr("整数码值：缓冲区实际码值（8-bit 0–255）。\n归一化浮点：channel/255；转换后 16-bit 为 channel16/65535，并另行标注。\n不表示线性光、HDR 或色彩管理后的值；Hex 始终是十六进制。")
                onClicked: numericFormatMenu.open()

                VcsMenu {
                    id: numericFormatMenu

                    menuWidth: 260

                    VcsRadioMenuItem {
                        objectName: "imageNumericFormatInteger"
                        text: qsTr("整数码值")
                        checked: !control.numericFloat
                        onTriggered: control.setNumericFormat(0)
                    }
                    VcsRadioMenuItem {
                        objectName: "imageNumericFormatFloat"
                        text: qsTr("归一化浮点")
                        checked: control.numericFloat
                        onTriggered: control.setNumericFormat(1)
                    }
                }
            }

            // ---- Probe colour copy -----------------------------------------------------------
            // The button copies the frozen sample, so the pointer may leave the pixel long
            // before the click. Its menu names the material, the coordinate and the value,
            // and offers RGB / RGBA / Hex instead of three permanent buttons.
            ImageCommandButton {
                objectName: "imageCopyColorButton"
                implicitHeight: Theme.hitAreaMinimum
                leftPadding: 8
                rightPadding: 8
                text: qsTr("复制颜色 ▾")
                enabled: control.probeSampleReady
                helpText: control.probeSampleLabel + "\n" + qsTr("复制的是该样本的原始解码像素，不是按钮下的屏幕颜色。\n源被关闭、替换或图像代次改变后样本失效。")
                onClicked: probeCopyMenu.open()

                VcsMenu {
                    id: probeCopyMenu

                    menuWidth: 300

                    VcsMenuItem {
                        objectName: "imageCopyColorSample"
                        enabled: false
                        text: control.probeSampleLabel
                        contentItem: Text {
                            text: control.probeSampleLabel
                            wrapMode: Text.Wrap
                            color: Theme.mutedText
                            font.pixelSize: Theme.fontSecondary
                            verticalAlignment: Text.AlignVCenter
                        }
                    }
                    VcsMenuSeparator {}
                    VcsMenuItem {
                        objectName: "imageCopyRgbAction"
                        text: control.numericFloat ? qsTr("复制 RGB（归一化浮点）") : qsTr("复制 RGB")
                        enabled: control.probeSampleReady
                        onTriggered: control.copyProbeColor(0)
                    }
                    VcsMenuItem {
                        objectName: "imageCopyRgbaAction"
                        text: control.numericFloat ? qsTr("复制 RGBA（归一化浮点）") : qsTr("复制 RGBA（Alpha 0–255）")
                        enabled: control.probeSampleReady
                        onTriggered: control.copyProbeColor(1)
                    }
                    VcsMenuItem {
                        objectName: "imageCopyHexAction"
                        text: qsTr("复制 Hex（#RRGGBBAA）")
                        enabled: control.probeSampleReady
                        onTriggered: control.copyProbeColor(2)
                    }
                }
            }

            Text {
                objectName: "imageMarqueeNotice"
                visible: control.marqueeNoticeText.length > 0
                text: control.marqueeNoticeText
                color: Theme.secondaryText
                font.pixelSize: 12
                elide: Text.ElideRight
                width: Math.min(implicitWidth, 230)
                height: Theme.hitAreaMinimum
                verticalAlignment: Text.AlignVCenter
            }
            // COLOR3: the colour state is its own always-visible A/B pair. It is driven by
            // the embedded-profile state, not by the RGBA8 conversion badge below, so a
            // native RGBA8 image with an ICC profile still shows "配置未应用". The badge
            // text is the controller's compact state sentence; the tooltip is the long form
            // of the same state, so the two can never disagree.
            StatusBadge {
                id: primaryColorStatusBadge
                objectName: "imagePrimaryColorStatusBadge"
                visible: Boolean(control.imageReview && control.hasPrimary)
                text: control.imageReview ? qsTr("A · ") + control.imageReview.primaryColorStateCompactText : ""
                // "未探测" / "配置未转换" are absences of colour management, not successes. Only
                // a conversion that actually ran earns the filled information dot; an
                // unsupported/corrupt profile keeps warning; everything else is the hollow
                // neutral ring that means "no reading". Without this split the badge reported
                // "I do not know" in the same green as "applied", which is the one thing this
                // product must not do (product intent section 2).
                //
                // The dot reads colorDisplayApplied, not colorConversionApplied: the latter
                // only records the decode-time format conversion and stays false for a native
                // RGBA8 source that still gets a display-profile transform. Display-applied is
                // the very predicate colorStateCompactText uses to decide it may print
                // "已转换", so the ring and the sentence are driven by one expression and can
                // never disagree.
                readonly property bool colorFallback: control.imageReview ? control.imageReview.primaryColorProfileNeedsFallback : false
                readonly property bool colorApplied: control.imageReview ? control.imageReview.primaryColorDisplayApplied : false
                dotColor: colorFallback ? Theme.warning : (colorApplied ? Theme.information : Theme.mutedText)
                dotHollow: !colorFallback && !colorApplied

                HoverHandler {
                    id: primaryColorStatusHover
                }

                VcsToolTip {
                    visible: primaryColorStatusHover.hovered
                    text: control.imageReview ? control.imageReview.primaryColorStatusText : ""
                }
            }

            StatusBadge {
                id: secondaryColorStatusBadge
                objectName: "imageSecondaryColorStatusBadge"
                visible: Boolean(control.imageReview && control.hasSecondary)
                text: control.imageReview ? qsTr("B · ") + control.imageReview.secondaryColorStateCompactText : ""
                // Same three-state rule as the A badge, same predicate: display-applied is what
                // the "已转换" sentence means, so the ring follows it.
                readonly property bool colorFallback: control.imageReview ? control.imageReview.secondaryColorProfileNeedsFallback : false
                readonly property bool colorApplied: control.imageReview ? control.imageReview.secondaryColorDisplayApplied : false
                dotColor: colorFallback ? Theme.warning : (colorApplied ? Theme.information : Theme.mutedText)
                dotHollow: !colorFallback && !colorApplied

                HoverHandler {
                    id: secondaryColorStatusHover
                }

                VcsToolTip {
                    visible: secondaryColorStatusHover.hovered
                    text: control.imageReview ? control.imageReview.secondaryColorStatusText : ""
                }
            }

            // COLOR4: the only in-product way to observe the display conversion on/off. The
            // conversion is display-only, so the tooltip repeats what does not change
            // (probe values, statistics, exported source values) and that the Windows output
            // path is still unverified.
            StatusBadge {
                id: colorDisplayToggleBadge
                objectName: "imageColorDisplayBadge"
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryColorProfileStatus === 2 || control.imageReview.secondaryColorProfileStatus === 2))
                text: control.imageReview && control.imageReview.colorDisplayEnabled ? qsTr("颜色转换：开") : qsTr("颜色转换：关")
                dotColor: Theme.information

                MouseArea {
                    objectName: "imageColorDisplayToggleButton"
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: control.toggleColorDisplay()
                }

                HoverHandler {
                    id: colorDisplayToggleHover
                }

                VcsToolTip {
                    visible: colorDisplayToggleHover.hovered
                    text: qsTr("按嵌入的 SDR 配置文件把画面转换到 sRGB 显示。只影响显示：像素读数、差异统计、导出码值和原始缓冲都不变。Windows 输出链路与物理显示尚未验证。")
                }
            }

            StatusBadge {
                id: displayConversionBadge
                objectName: "displayConversionBadge"
                visible: Boolean(control.imageReview && control.hasPrimary && (control.imageReview.primaryDisplayConverted || control.imageReview.secondaryDisplayConverted))
                text: qsTr("显示缓冲 RGBA8")
                // Extra information about a conversion that already happened, not a warning:
                // the theme's information token exists for exactly this "neutral note" case.
                dotColor: Theme.information

                HoverHandler {
                    id: displayConversionHover
                }

                VcsToolTip {
                    visible: displayConversionHover.hovered
                    text: qsTr("像素格式/位深已转换到 RGBA8 显示缓冲，不是文件平面原始码值。\nA/B 颜色徽章写明是否已应用文件颜色配置转换；显示环境仍未验证。\n高位深源请参考像素状态栏的转换后 16-bit 码值。")
                }
            }

            StatusBadge {
                id: resampleBadge
                objectName: "imageResampleBadge"
                visible: Boolean(control.imageReview && control.hasPair && (control.imageReview.diffResampled || (control.sizesDiffer && control.imageReview.resampleAllowed)))
                text: control.imageReview && control.imageReview.diffResampled ? qsTr("空间已重采样") : qsTr("重采样：开")
                // Same information semantics as the conversion badge: a resample that already
                // happened is a fact about the comparison, not a warning about it.
                dotColor: Theme.information

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

            // Alpha threshold observation filter state. It says what the filter is doing
            // *right now*, including the two states a user must not have to guess: the
            // threshold at which nothing passes, and the modes where the filter is not
            // applied at all.
            StatusBadge {
                id: alphaFilterBadge
                objectName: "imageAlphaFilterBadge"
                visible: control.alphaFilterBadgeText.length > 0
                text: control.alphaFilterBadgeText
                dotColor: Theme.information

                HoverHandler {
                    id: alphaFilterBadgeHover
                }

                VcsToolTip {
                    visible: alphaFilterBadgeHover.hovered
                    text: qsTr("仅显示 A > T（严格大于；A=25、T=25 隐藏，A=26 显示）。\n这是观察滤镜：不修改素材、编辑副本、原始读数、差异或统计。\n无 Alpha 的素材按完全不透明处理；高位深 Alpha 不降到 8-bit 再比较。\n当前仅在 RGB 忽略透明度观察（单图／并排／Wipe）生效。")
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
                font.pixelSize: Theme.fontCaption
            }
        }
    }

    // Marquee notices are transient: a couple of seconds on the status bar, then the text
    // leaves the row again so the readouts keep their space.
    Timer {
        id: marqueeNoticeTimer

        interval: 2600
        repeat: false
        onTriggered: control.marqueeNoticeText = ""
    }

    // Bounded commit of the alpha-threshold slider: one threshold change per quiet
    // interval, plus one immediate commit when the drag ends. Rebuilding a derived channel
    // view is a full-image pass over the decoded buffer, so an unbounded per-pixel commit
    // would stall the GUI on a large source; this keeps the number of rebuilds bounded by
    // the drag duration, not by the slider's travel.
    Timer {
        id: alphaThresholdCommit

        interval: 120
        repeat: false
        property int pendingValue: 0
        onTriggered: control.setAlphaThreshold(pendingValue)
    }

    // Window deactivation (alt-tab, another window on top) must end a held peek the same
    // way a key release does: the user cannot keep holding a key that now belongs to
    // another window. The attached lookup must go through control: a Connections object
    // is not an Item, so a bare `Window.window` inside the handler cannot resolve and
    // the whole handler would error out instead of ending the peek.
    Connections {
        target: control.Window.window

        function onActiveChanged() {
            const window = control.Window.window;
            if (window && !window.active)
                control.endPeek();
        }
    }

    Connections {
        target: control.imageEdit

        // Isolated harnesses mock the edit controller without every signal; the real
        // ImageEditController does carry historyChanged.
        ignoreUnknownSignals: true

        function onHistoryChanged() {
            // Committed edits and undo/redo retire rectangles from the previous image state.
            control.cropSelection = null;
        }
    }

    // T4/T6: warm the next complete pair in the background after a successful folder
    // commit, so stepping onto it is a cache hit. Bounded to one neighbour; a user
    // request cancels outstanding prefetches and the visible pair never changes.
    Connections {
        target: control.pairModel

        // Same mock tolerance as above: ImageFolderPairModel carries the signal.
        ignoreUnknownSignals: true

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
