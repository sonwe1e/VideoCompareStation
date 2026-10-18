pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
import "VcsTheme.js" as Theme
import "LayoutMetrics.js" as LayoutMetrics
import Dvs.Ui 1.0

Rectangle {
    id: control

    property bool pixelGridEnabled: false
    // The enum values stay bound to the real type rather than copying numbers; the
    // generated .qmltypes makes qmllint resolve them now.
    readonly property int videoFilterAutoValue: ComparisonSurface.VideoFilterAuto
    readonly property int videoFilterSmoothValue: ComparisonSurface.VideoFilterSmooth
    readonly property int videoFilterPixelValue: ComparisonSurface.VideoFilterPixel

    Repeater {
        parent: surfaceLayer
        model: control.pixelGridEnabled ? dualVideoSurface.pixelGridPanels : []

        PixelGrid {
            required property var modelData
            objectName: "videoPixelGrid"
            x: Number(modelData.x)
            y: Number(modelData.y)
            width: Number(modelData.width)
            height: Number(modelData.height)
            originX: Number(modelData.originX)
            originY: Number(modelData.originY)
            stepX: Number(modelData.stepX)
            stepY: Number(modelData.stepY)
            requested: control.pixelGridEnabled
            nearest: dualVideoSurface.videoFilterMode === control.videoFilterPixelValue || (dualVideoSurface.videoFilterMode === control.videoFilterAutoValue && dualVideoSurface.physicalScalePercent >= dualVideoSurface.autoNearestThresholdPercent)
            z: 2
        }
    }

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
    // Hold-to-peek: while true the difference pass shows the session's reference (GT) source.
    required property bool differenceSuppressed
    // While true the difference canvas shows the reference (GT) source instead of the difference
    // pass. The indicator plate names what is on screen for as long as the state holds, so a
    // locked peek cannot be mistaken for the difference view after the HUD line has faded.
    // Plain properties (not required) so the viewport can be instantiated without a session.
    property bool differencePeekActive: false
    property bool differencePeekLocked: false
    property string differencePeekTargetLabel: ""
    // V02 (P05) full-canvas direct look: -1 = regular comparison view, otherwise the whole
    // canvas shows only that source slot (session slot order). Presentation state owned by the
    // shell; forwarded to the surface, which mirrors it into the renderer snapshot and the
    // panel geometry, so overlays and hit mapping follow the same single panel.
    property int canvasFocusSlot: -1
    property bool dualDifferenceMode: false
    // V04 dual-difference caliber: Main computes one legend/converted-note pair for the two
    // rendered panes (both candidate slots against the reference under one shared
    // threshold/gain/metric/colour scale). Empty text falls back to the single-pair wording.
    property string dualDifferenceLegendText: ""
    property bool dualDifferenceLegendExact: false
    property string dualDifferenceInexactDetail: ""
    required property int referenceSourceIndex
    required property int sourceCount
    required property bool wipeMode
    required property bool differenceMode
    required property bool analysisGridMode
    required property bool immersiveHudVisible
    required property string immersiveHudText
    required property bool showFramePending
    // U03: the waiting pill's wording is owned by the host so it can name the actual work
    // (an ordinary latest-frame fetch vs. preparing reverse-walk frames) without the
    // viewport learning about the jog state machine.
    property string framePendingText: qsTr("正在获取最新帧…")
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
    // True while the transport plays; the pixel readout re-samples on paused frame steps but
    // never per playback frame, so it cannot add readback work to the render loop.
    required property bool playbackActive
    // Mirrors the transport's reveal state while playing: the floating OSC's countdown hides it
    // after pointer idle, any pointer activity or the wake strip reveals it again, and the
    // docked transport and every paused session stay revealed. The bottom-corner HUD follows it,
    // so a playing view keeps the picture clear without losing the controls on demand.
    required property bool playbackHudRevealed
    // Paused sessions show the HUD unconditionally; the transport contract keeps revealActive
    // true while paused, and this guard keeps the viewport honest even if that ever drifts.
    readonly property bool hudRevealed: !control.playbackActive || control.playbackHudRevealed

    property string alignmentModeName: ""
    property string inexactReason: ""

    // V01 "全景 + 局部" task preset. The shell already gates the mode by source count; the
    // local guard keeps the strip off if that ever drifts. The surface itself never renders
    // this mode - the panorama keeps one column per source (ThreeUp with three sources so the
    // third stays presented; SideBySide with two) and the renderer draws the local windows as
    // additional draws of the same presented frame set, so no extra decode path exists.
    // qmllint disable unqualified
    readonly property bool roiTripletMode: control.effectiveViewMode === ComparisonSurface.RoiTriplet && control.sourceCount >= 2
    // qmllint enable unqualified
    // V02 (P05): full-canvas focus suspends the preset - the panorama collapses to the focused
    // source, so there are no panels for the local windows to sample. The strip and
    // its outline hide, and the stage reclaims their space; the ROI itself stays on the
    // surface and survives leaving the focus.
    readonly property bool roiTripletStageActive: control.roiTripletMode && control.canvasFocusSlot < 0
    // Window zoom owned here so it survives leaving and re-entering the preset; the ROI
    // itself lives on the surface and therefore survives every mode switch. 0 = 适配ROI,
    // otherwise physical source-pixel percent.
    property int roiTripletZoomPercent: 0
    // U01: the strip's share of the workspace height. Session-scoped view preference driven
    // by the drag splitter; the clamp keeps it inside the 28-40 % band at all times.
    property real roiTripletStripShare: 0.34
    // How far the preset strip lifts the bottom chrome row so badges never cover the cells.
    readonly property real roiTripletStageInset: control.roiTripletStageActive ? roiTripletStrip.height + 8 : 0

    property bool roiSelecting: false
    // I02: visible affordance for the ROI marquee. Armed by the 框选区域 button (Alt+drag
    // keeps working without it), the cross cursor plus the checked button name the mode; a
    // plain drag then draws the comparison ROI instead of panning. Esc, toggling the button
    // off or resetting the view leaves it; while it is off, the default drag stays pan.
    property bool roiSelectMode: false
    // Shift marquee zooms to the selection (product "放大"); Alt marquee sets comparison ROI.
    property bool roiModeIsZoom: true
    property int roiPanel: -1
    property real roiStartX: 0
    property real roiStartY: 0
    property real roiCurrentX: 0
    property real roiCurrentY: 0
    property real panLastX: 0
    property real panLastY: 0

    // Pixel readout (1009 plan C slice 3). The surface's render-thread probes answer
    // asynchronously against the frame set the renderer is presenting. A delivery is accepted
    // only while it still describes that frame - its own frame/session/generation identity must
    // equal the identity the surface presents right now - and only while its pixel coordinate
    // still matches what the current hover position maps to for that slot. Results from a
    // cursor that moved on, or from a frame that was seeked away, are dropped instead of shown
    // next to the wrong position; a value that survives while the frame moved on is labelled
    // frozen instead of being passed off as current.
    property bool pixelReadoutPositionValid: false
    property real pixelReadoutSourceX: 0
    property real pixelReadoutSourceY: 0
    property var pixelReadoutExpected: ({})
    property var pixelReadoutResults: ({})
    // Last position+frame this viewport actually spent a readback on, so mouse jitter over one
    // parked pixel cannot turn into a probe batch per event.
    property var pixelReadoutIssuedKey: null
    // Diagnostics counter (like the surface's droppedFrames): how many probe batches left
    // this viewport, from hover moves and paused frame steps alike.
    property int pixelReadoutBatchesIssued: 0
    property int pixelReadoutRequestId: 0

    signal wipePositionRequested(real position)
    signal oscRevealRequested
    signal contextMenuRequested
    signal roiRejected(string reason)
    // Mirrors the last roiRejected() argument so the C++ contract suite (which does not link
    // QtTest/QSignalSpy) can observe that a dropped marquee carried a non-empty reason.
    property string lastRoiRejectReason: ""
    property alias surface: dualVideoSurface
    property alias videoOutput: surfaceLayer
    readonly property bool roiEnabled: dualVideoSurface.roiEnabled
    // Normalized shared-ROI rect (V04 nearby-frames strip). Re-exported from the typed
    // surface so other files never touch the var-typed surface alias.
    readonly property real surfaceRoiLeft: dualVideoSurface.roiLeft
    readonly property real surfaceRoiTop: dualVideoSurface.roiTop
    readonly property real surfaceRoiRight: dualVideoSurface.roiRight
    readonly property real surfaceRoiBottom: dualVideoSurface.roiBottom
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

    // View commands shared by the chrome buttons and the keyboard shortcuts. The badge's
    // content/DPR/ROI/SAR-aware readout owns the native-size target; zoomAt follows its focal
    // point and clamps scale and center to the viewport bounds.
    function fitToWindow() {
        dualVideoSurface.resetViewport();
    }

    function resetView() {
        dualVideoSurface.clearRoi();
        dualVideoSurface.resetViewport();
        // I02: 重置视图 is the one-shot "back to normal navigation" verb; leaving the marquee
        // mode armed with its cross cursor would contradict the reset the user asked for.
        control.roiSelectMode = false;
    }

    function zoomViewStep(factor) {
        dualVideoSurface.zoomAt(0.5, 0.5, factor);
    }

    function zoomToNativeSize() {
        if (!pixelScaleBadge.targetReachable || pixelScaleBadge.pixelExact)
            return;
        dualVideoSurface.zoomAt(0.5, 0.5, 100 / pixelScaleBadge.effectivePercent);
    }

    function toggleNativeFit() {
        if (pixelScaleBadge.pixelExact)
            dualVideoSurface.resetViewport();
        else
            zoomToNativeSize();
    }

    function panelPoint(x, y) {
        return dualVideoSurface.mapSurfacePoint(x, y);
    }

    // Hover/drag moves extracted from the navigation MouseArea so contract tests can drive
    // the exact path (pan, marquee tracking, pixel readout) with synthetic positions.
    function handleNavigationMove(x, y) {
        control.oscRevealRequested();
        const point = control.panelPoint(x, y);
        if (control.roiSelecting) {
            control.roiCurrentX = x;
            control.roiCurrentY = y;
            return;
        }
        if (viewportNavigation.pressed && point.insideContent && point.panel === control.roiPanel) {
            dualVideoSurface.panBy(point.x - control.panLastX, point.y - control.panLastY);
            control.panLastX = point.x;
            control.panLastY = point.y;
            // The pan moved the content under the cursor; re-map before re-issuing.
            control.issuePixelReadout(control.panelPoint(x, y));
            return;
        }
        control.issuePixelReadout(point);
    }

    // Issues one probe per session source at the hovered content position. The surface stages
    // each slot independently and resolves the whole batch against one presented frame set, so
    // every readout row samples the same content point of the same frame. Playback never issues:
    // the readout is on-demand numeric review, and a moving cursor must not add readbacks to the
    // render loop while frames stream.
    function issuePixelReadout(point) {
        const usable = Boolean(point) && Boolean(point.insideContent) && point.sourceX !== undefined && point.sourceY !== undefined && control.sourceCount > 0 && !control.overlayVisible;
        const wasUsable = control.pixelReadoutPositionValid;
        control.pixelReadoutPositionValid = usable;
        if (!usable) {
            control.pixelReadoutIssuedKey = null;
            // Retiring the readout also drops its in-flight readback instead of letting it run.
            if (wasUsable)
                dualVideoSurface.cancelSourcePixelProbes();
            return;
        }
        control.pixelReadoutSourceX = Number(point.sourceX);
        control.pixelReadoutSourceY = Number(point.sourceY);
        control.pixelReadoutExpected = expectedPixelsForPosition(control.pixelReadoutSourceX, control.pixelReadoutSourceY);
        if (control.playbackActive) {
            return;
        }
        issueProbesForAllSlots();
    }

    function issueProbesForAllSlots() {
        if (!control.pixelReadoutPositionValid || control.playbackActive)
            return;
        if (!pixelReadoutWantsFreshSample())
            return;
        control.pixelReadoutBatchesIssued += 1;
        control.pixelReadoutRequestId += 1;
        for (let slot = 0; slot < control.sourceCount; ++slot)
            dualVideoSurface.requestSourcePixelProbe(slot, control.pixelReadoutSourceX, control.pixelReadoutSourceY, control.pixelReadoutRequestId);
    }

    // The renderer resolves each slot's pixel from the same clamped normalized position and
    // that slot's own frame dimensions; recomputing it here yields the exact coordinate a
    // fresh delivery must carry to count as current.
    function expectedPixelsForPosition(nx, ny) {
        const upperLimit = 1.0 - Number.EPSILON;
        const cx = Math.min(Math.max(Number(nx), 0.0), upperLimit);
        const cy = Math.min(Math.max(Number(ny), 0.0), upperLimit);
        const expected = {};
        for (let slot = 0; slot < control.sourceCount; ++slot) {
            const info = slot < control.sourceMediaInfo.length ? control.sourceMediaInfo[slot] : null;
            if (!info)
                continue;
            expected[slot] = {
                "x": Math.floor(cx * Number(info.width)),
                "y": Math.floor(cy * Number(info.height))
            };
        }
        return expected;
    }

    // Same cursor pixel, same frame, same presented frame set: the rows already answer it. A new
    // key means the content under the cursor or the frame on screen moved, so a fresh batch is
    // worth its readback.
    function pixelReadoutWantsFreshSample() {
        const key = [JSON.stringify(control.pixelReadoutExpected), presentedIdentityKey()].join(":");
        if (control.pixelReadoutIssuedKey === key)
            return false;
        control.pixelReadoutIssuedKey = key;
        return true;
    }

    // Identity of the frame set the surface is presenting right now, or "none" while nothing has
    // been drawn. This is the frame the readout is allowed to speak for.
    function presentedIdentityKey() {
        if (!dualVideoSurface.presentedFrameValid)
            return "none";
        return dualVideoSurface.presentedFrameId + ":" + dualVideoSurface.presentedSessionEpoch + ":" + dualVideoSurface.presentedPlaybackGeneration;
    }

    // The renderer resolves a probe against the set it is presenting, so a fulfilled delivery
    // carries that set's identity, including failures. An explicitly invalid identity denotes
    // no presented frame; untagged results are rejected by the sink.
    function probeResultIdentityKey(result) {
        if (!Boolean(result.identityValid))
            return "none";
        return result.frameId + ":" + result.sessionEpoch + ":" + result.playbackGeneration;
    }

    // Slot-filtered sink for sourcePixelProbed. A fulfilled read must still describe the frame on
    // screen; a failure carries no values of its own, so it is accepted only while the screen still
    // shows the frame set the request was made against. Either way the row never mixes coordinates
    // with foreign values.
    function acceptPixelProbeResult(result) {
        if (!result || !control.pixelReadoutPositionValid || control.playbackActive)
            return;
        if (result.requestId === undefined || Number(result.requestId) !== control.pixelReadoutRequestId || result.identityValid === undefined)
            return;
        if (probeResultIdentityKey(result) !== presentedIdentityKey())
            return;
        const slot = Number(result.slot);
        const expected = control.pixelReadoutExpected[slot];
        if (!expected)
            return;
        if (Boolean(result.valid)) {
            if (Number(result.x) !== expected.x || Number(result.y) !== expected.y)
                return;
        }
        const updated = Object.assign({}, control.pixelReadoutResults);
        updated[slot] = result;
        control.pixelReadoutResults = updated;
    }

    // True while the accepted sample still describes the presented frame. A value that outlived a
    // seek or a frame step is kept for reference but labelled, never presented as current.
    function probeResultIsCurrent(result) {
        const identity = probeResultIdentityKey(result);
        const expected = control.pixelReadoutExpected[Number(result.slot)];
        return !control.playbackActive && identity === presentedIdentityKey() && Boolean(expected) && Number(result.x) === expected.x && Number(result.y) === expected.y;
    }

    // The frame planes a probe reads are the normalized NV12/P010 buffers the display path uses,
    // so a source that is not already 4:2:0 was resampled on the way in. Naming that per source
    // is the difference between "the file's own code values" and "what this frame holds".
    function pixelReadoutConverted(info) {
        const format = info && info.pixelFormat !== undefined ? String(info.pixelFormat) : "";
        if (format.length === 0)
            return false;
        return !["nv12", "p010le", "yuv420p", "yuv420p10le"].includes(format);
    }

    function pixelReadoutRowText(slot) {
        const result = control.pixelReadoutResults[slot];
        const info = slot < control.sourceMediaInfo.length ? control.sourceMediaInfo[slot] : null;
        if (!info)
            return qsTr("无元数据");
        if (!result)
            return control.playbackActive ? qsTr("播放中 · 暂停后读取") : qsTr("读取中…");
        if (!Boolean(result.valid)) {
            const reason = String(result.reason);
            if (reason === "no-frame")
                return qsTr("帧未就绪");
            if (reason === "missing-slot")
                return qsTr("该源无帧");
            if (reason === "device-busy")
                return qsTr("设备忙");
            if (reason === "readback-failed")
                return qsTr("读取失败");
            return qsTr("超出画面");
        }
        const depth = Number(result.bitDepth) > 0 ? qsTr("%1-bit").arg(Number(result.bitDepth)) : "";
        const range = info.colorRange ? String(info.colorRange) : qsTr("范围未知");
        const converted = control.pixelReadoutConverted(info) ? qsTr("已转换") : "";
        const normalizedFormat = Number(result.bitDepth) === 10 ? "P010" : "NV12";
        const sourceFormat = info.pixelFormat ? qsTr("来源 %1").arg(String(info.pixelFormat)) : qsTr("来源格式未知");
        const meta = [normalizedFormat, depth, range, sourceFormat, converted].filter(part => part.length > 0).join(" ");
        const values = qsTr("像素 (%1, %2)  Y %3  Cb %4  Cr %5%6").arg(Number(result.x)).arg(Number(result.y)).arg(Number(result.luma)).arg(Number(result.cb)).arg(Number(result.cr)).arg(meta.length > 0 ? "  " + meta : "");
        if (control.probeResultIsCurrent(result))
            return values;
        return qsTr("冻结于第 %1 帧 · %2").arg(Number(result.frameId) + 1).arg(values);
    }

    // Phase-3 table decomposition of the same data. pixelReadoutRowText() stays the legacy
    // full string (the C++ contract suite invokes it directly and asserts exact substrings);
    // the helpers below feed the aligned micro-table's columns.
    function pixelReadoutHasValidSample(slot) {
        const result = control.pixelReadoutResults[slot];
        return Boolean(result) && Boolean(result.valid);
    }

    function pixelReadoutCoordText(slot) {
        if (!control.pixelReadoutHasValidSample(slot))
            return "";
        const result = control.pixelReadoutResults[slot];
        return "(" + Number(result.x) + ", " + Number(result.y) + ")";
    }

    function pixelReadoutValuesText(slot) {
        if (!control.pixelReadoutHasValidSample(slot))
            return "";
        const result = control.pixelReadoutResults[slot];
        const frozenPrefix = control.probeResultIsCurrent(result) ? "" : qsTr("冻结于第 %1 帧 · ").arg(Number(result.frameId) + 1);
        return frozenPrefix + qsTr("Y %1  Cb %2  Cr %3").arg(Number(result.luma)).arg(Number(result.cb)).arg(Number(result.cr));
    }

    // Delta column: this source minus the session's reference source. Code values only compare
    // on one numerical scale: both sides must be current samples (same presented frame set,
    // same parked coordinate) AND share the frame bit depth and the source colour range. The
    // renderer's normalized NV12/P010 display planes say nothing about cross-depth value
    // equality - a 10-bit code next to an 8-bit code, or limited next to full, is not a delta,
    // it is two scales. Anything less shows the em dash with the reason one hover away.
    function pixelReadoutDeltaReason(slot) {
        const reference = control.referenceSourceIndex;
        if (reference < 0 || reference >= control.sourceCount)
            return qsTr("会话未识别参考（GT）源");
        if (!control.pixelReadoutHasValidSample(slot) || !control.pixelReadoutHasValidSample(reference))
            return qsTr("样本不完整（读取中/失败或不可用）");
        const mine = control.pixelReadoutResults[slot];
        const base = control.pixelReadoutResults[reference];
        if (!control.probeResultIsCurrent(mine) || !control.probeResultIsCurrent(base))
            return qsTr("样本已过期（跨帧不能相减）");
        const myDepth = Number(mine.bitDepth);
        const baseDepth = Number(base.bitDepth);
        if (!(myDepth > 0) || !(baseDepth > 0) || myDepth !== baseDepth)
            return qsTr("位深不同（%1 与 %2）").arg(myDepth > 0 ? myDepth : qsTr("未知")).arg(baseDepth > 0 ? baseDepth : qsTr("未知"));
        const myInfo = slot < control.sourceMediaInfo.length ? control.sourceMediaInfo[slot] : null;
        const baseInfo = reference < control.sourceMediaInfo.length ? control.sourceMediaInfo[reference] : null;
        const myRange = myInfo && myInfo.colorRange ? String(myInfo.colorRange) : "";
        const baseRange = baseInfo && baseInfo.colorRange ? String(baseInfo.colorRange) : "";
        if (myRange.length === 0 || baseRange.length === 0)
            return qsTr("色彩范围未知");
        if (myRange !== baseRange)
            return qsTr("色彩范围不同（%1 与 %2）").arg(myRange).arg(baseRange);
        return "";
    }

    function pixelReadoutDeltaText(slot) {
        if (control.pixelReadoutDeltaReason(slot).length > 0)
            return "—";
        const reference = control.referenceSourceIndex;
        const mine = control.pixelReadoutResults[slot];
        const base = control.pixelReadoutResults[reference];
        const signed = value => (value > 0 ? "+" : "") + Math.round(value);
        return "Δ " + signed(Number(mine.luma) - Number(base.luma)) + " " + signed(Number(mine.cb) - Number(base.cb)) + " " + signed(Number(mine.cr) - Number(base.cr));
    }

    function pixelReadoutFormatText(slot) {
        if (!control.pixelReadoutHasValidSample(slot))
            return "";
        const result = control.pixelReadoutResults[slot];
        const info = slot < control.sourceMediaInfo.length ? control.sourceMediaInfo[slot] : null;
        if (!info)
            return "";
        const depth = Number(result.bitDepth) > 0 ? qsTr("%1-bit").arg(Number(result.bitDepth)) : "";
        const range = info.colorRange ? String(info.colorRange) : qsTr("范围未知");
        const converted = control.pixelReadoutConverted(info) ? qsTr("已转换") : "";
        const normalizedFormat = Number(result.bitDepth) === 10 ? "P010" : "NV12";
        const sourceFormat = info.pixelFormat ? qsTr("来源 %1").arg(String(info.pixelFormat)) : qsTr("来源格式未知");
        return [normalizedFormat, depth, range, sourceFormat, converted].filter(part => part.length > 0).join(" ");
    }

    // A paused frame step keeps the parked cursor's values current; during playback the
    // readout freezes instead of adding readbacks to every presented frame.
    function refreshPixelReadoutAfterFrameChange() {
        if (control.playbackActive || !control.pixelReadoutPositionValid)
            return;
        issueProbesForAllSlots();
    }

    onPlaybackActiveChanged: {
        if (control.playbackActive) {
            dualVideoSurface.cancelSourcePixelProbes();
            control.pixelReadoutIssuedKey = null;
        } else {
            refreshPixelReadoutAfterFrameChange();
        }
    }
    onOverlayVisibleChanged: {
        if (overlayVisible) {
            pixelReadoutPositionValid = false;
            pixelReadoutIssuedKey = null;
            dualVideoSurface.cancelSourcePixelProbes();
        }
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
    // I02: Esc leaves the marquee mode. The viewport is the stage's focus item, so this is the
    // receiver for an Esc no other layer owns: the presentation Esc (full screen / hidden
    // chrome) and the jog-end Esc stay enabled in exactly those states, and Qt's shortcut
    // layer runs before item key delivery, so this handler only sees the idle-chrome case.
    Keys.onEscapePressed: event => {
        if (control.roiSelectMode) {
            control.roiSelectMode = false;
            event.accepted = true;
        }
    }
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

        // Docked only while the preset is active; otherwise this reduces exactly to the old
        // anchors.fill geometry (height = parent - 2 margins), so the default view is
        // byte-identical when the preset is off.
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            margins: control.chromeVisible ? 1 : 0
        }
        height: parent.height - 2 * (control.chromeVisible ? 1 : 0) - (control.roiTripletStageActive ? roiTripletStrip.height + 8 : 0)

        // The type is runtime-registered; startup smoke coverage verifies the registration.
        // qmllint disable import unqualified unresolved-type
        ComparisonSurface {
            id: dualVideoSurface

            // Auto sampling keys off the badge's physical percent so the readout, the 100% action
            // and the sampler can never disagree.
            physicalScalePercent: pixelScaleBadge.effectivePercent
            objectName: "dualVideoSurface"
            Accessible.name: qsTr("CompareStation 同步对比画面")
            // The preset keeps the panorama on one column per source; the renderer has no panel
            // layout for RoiTriplet and the local windows are renderer-side extra draws. ThreeUp
            // (not SideBySide) with three sources because the third column must stay presented;
            // with two sources SideBySide presents both.
            viewMode: control.roiTripletMode ? (control.sourceCount >= 3 ? ComparisonSurface.ThreeUp : ComparisonSurface.SideBySide) : control.effectiveViewMode
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
            canvasFocusSlot: control.canvasFocusSlot
            sourceDisplayInfo: control.sourceMediaInfo
            anchors.fill: parent
            onSourcePixelProbed: result => control.acceptPixelProbeResult(result)
            onPresentedFrameIdentityChanged: control.refreshPixelReadoutAfterFrameChange()
            onDifferenceSuppressedChanged: {
                // The GT can have another aspect ratio or rotation. The parked hover belonged
                // to the previous canvas geometry; retire it until the next pointer mapping.
                control.pixelReadoutPositionValid = false;
                control.pixelReadoutIssuedKey = null;
                dualVideoSurface.cancelSourcePixelProbes();
            }
            onCanvasFocusSlotChanged: {
                // Same rule as the GT peek: the focused source can have another aspect ratio
                // or rotation, so the parked hover described the previous canvas geometry.
                // Retire it until the next pointer mapping; the readout then re-maps through
                // the single focus panel and cannot quote the wrong slot.
                control.pixelReadoutPositionValid = false;
                control.pixelReadoutIssuedKey = null;
                dualVideoSurface.cancelSourcePixelProbes();
            }
        }
        // qmllint enable import unqualified unresolved-type

        // The panorama marks the shared ROI while the preset is active ("全景标出区域").
        // Rects come from the surface, resolved with the same pure geometry the render path
        // draws, so the outline can never drift from the picture. Hidden while the canvas is
        // focused on one source - the strip is hidden then, and there is no panorama to mark.
        Repeater {
            objectName: "roiTripletOutlineRepeater"

            model: control.roiTripletMode && control.canvasFocusSlot < 0 && dualVideoSurface.roiEnabled ? dualVideoSurface.roiTripletOutlineRects : []

            delegate: Rectangle {
                id: roiOutline

                required property var modelData

                objectName: "roiTripletOutline"
                readonly property bool resolved: Boolean(roiOutline.modelData) && Number(roiOutline.modelData.width) > 1 && Number(roiOutline.modelData.height) > 1

                visible: roiOutline.resolved
                x: roiOutline.resolved ? Number(roiOutline.modelData.x) : 0
                y: roiOutline.resolved ? Number(roiOutline.modelData.y) : 0
                width: roiOutline.resolved ? Number(roiOutline.modelData.width) : 0
                height: roiOutline.resolved ? Number(roiOutline.modelData.height) : 0
                color: "transparent"
                border.color: control.accentColor
                border.width: 2
                radius: 2
                z: 9
            }
        }
    }

    Repeater {
        objectName: "panelDividerRepeater"
        // Full-canvas focus collapses the layout to one panel, so there is no split to draw.
        model: control.sourceCount > 1 && control.canvasFocusSlot < 0 && !control.wipeMode && (!control.differenceMode || control.analysisGridMode || control.dualDifferenceMode) ? dualVideoSurface.sourcePanelRects : []

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

    // E1: expose the surface's wipe split axis and position conversions so the host can
    // route the W toggle and the Alt+Up/Down moves without touching internals.
    readonly property int wipeOrientation: dualVideoSurface.wipeOrientation
    function setWipeOrientation(orientation) {
        dualVideoSurface.wipeOrientation = orientation;
    }
    function wipePositionForLogicalY(y) {
        return dualVideoSurface.wipePositionForLogicalY(y);
    }
    function wipeSplitLogicalY() {
        // wipeSplitLogicalY on the surface is a qreal Q_PROPERTY, not a method: read it as a
        // property value here so callers of this wrapper never invoke it as a function.
        return dualVideoSurface.wipeSplitLogicalY;
    }

    WipeHandle {
        // Hidden while the canvas is focused: no wipe composite is rendered then, so a
        // draggable split would promise interaction the picture does not have.
        visible: control.wipeMode && control.canvasFocusSlot < 0
        z: 50
        surfaceItem: dualVideoSurface
        orientation: dualVideoSurface.wipeOrientation
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
                font.pixelSize: Theme.fontSecondary
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
        radius: Theme.radiusSubtle
        color: Theme.oscGlass
        border.color: Theme.oscBorder
        anchors {
            bottom: parent.bottom
            bottomMargin: 18 + control.roiTripletStageInset
            horizontalCenter: parent.horizontalCenter
        }

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationFast
            }
        }

        Text {
            id: immersiveHudLabel

            text: control.immersiveHudText
            color: control.primaryTextColor
            font.pixelSize: Theme.fontBody
            anchors.centerIn: parent
        }
    }

    Rectangle {
        objectName: "framePendingIndicator"
        visible: control.showFramePending && control.currentFrame >= 0
        z: 40
        radius: Theme.radiusLarge
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
                text: control.framePendingText
                color: control.mutedTextColor
                font.pixelSize: Theme.fontSecondary
            }
        }
    }

    MouseArea {
        id: viewportNavigation

        objectName: "viewportNavigation"
        // Panorama gestures stop at the preset strip; identical to the old fill+margins
        // geometry while the preset is off.
        anchors {
            top: parent.top
            left: parent.left
            right: parent.right
            bottom: control.roiTripletStageActive ? roiTripletStrip.top : parent.bottom
            margins: 1
        }
        acceptedButtons: Qt.LeftButton | Qt.RightButton
        hoverEnabled: true
        // I02: the cursor names the interaction before the first press - cross while the
        // marquee is armed or being drawn, the standard arrow for pan otherwise.
        cursorShape: control.roiSelectMode || control.roiSelecting ? Qt.CrossCursor : Qt.ArrowCursor

        onWheel: wheel => {
            control.oscRevealRequested();
            const point = control.panelPoint(wheel.x, wheel.y);
            if (!point.insideContent) {
                control.issuePixelReadout(point);
                wheel.accepted = false;
                return;
            }
            dualVideoSurface.zoomAt(point.x, point.y, wheel.angleDelta.y > 0 ? 1.25 : 0.8);
            // The zoom moved the content under the cursor; re-map before re-issuing.
            control.issuePixelReadout(control.panelPoint(wheel.x, wheel.y));
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
            } else if (control.roiSelectMode) {
                // I02: the visible 框选区域 mode turns the plain drag into the ROI marquee,
                // so the gesture no longer depends on remembering the Alt chord.
                control.roiSelecting = true;
                control.roiModeIsZoom = false;
                control.roiStartX = mouse.x;
                control.roiStartY = mouse.y;
                control.roiCurrentX = mouse.x;
                control.roiCurrentY = mouse.y;
            }
            control.forceActiveFocus();
        }
        onPositionChanged: mouse => control.handleNavigationMove(mouse.x, mouse.y)
        onExited: control.pixelReadoutPositionValid = false
        onReleased: mouse => {
            if (control.roiSelecting) {
                const start = control.panelPoint(control.roiStartX, control.roiStartY);
                const end = control.panelPoint(mouse.x, mouse.y);
                if (start.insideContent && end.insideContent && start.panel === end.panel && start.sourceX !== undefined && start.sourceY !== undefined && end.sourceX !== undefined && end.sourceY !== undefined) {
                    if (control.roiModeIsZoom)
                        dualVideoSurface.zoomToNormalizedRect(start.sourceX, start.sourceY, end.sourceX, end.sourceY);
                    else
                        dualVideoSurface.setRoiNormalized(start.sourceX, start.sourceY, end.sourceX, end.sourceY);
                } else if (!start.insideContent || !end.insideContent || start.panel !== end.panel) {
                    // Released in the letterbox or the other panel: nothing was selected, so the
                    // marquee is dropped - but never silently. roiSelectMode stays armed per its
                    // documented contract; only the silence is removed here.
                    control.lastRoiRejectReason = qsTr("框选未应用：请在画面内按住并拖动");
                    control.roiRejected(control.lastRoiRejectReason);
                } else {
                    // Inside one panel, but a corner did not map to a source pixel (empty or
                    // sub-pixel drag), so there is no rect to apply.
                    control.lastRoiRejectReason = qsTr("框选未应用：区域太小");
                    control.roiRejected(control.lastRoiRejectReason);
                }
            }
            control.roiSelecting = false;
            control.roiModeIsZoom = true;
            control.roiPanel = -1;
        }
        // Double-click unifies with the image workspace: toggle native size <-> fit. Full
        // screen stays on F11 only. An active ROI marquee keeps its own double-click verb.
        onDoubleClicked: {
            if (control.roiEnabled)
                control.clearRoi();
            else
                control.toggleNativeFit();
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
        visible: control.chromeVisible && control.hudRevealed && (control.differenceMode || dualVideoSurface.roiEnabled || control.alignmentModeName.length > 0 || control.differenceThresholdEnabled)
        opacity: control.hudRevealed ? 1 : 0
        z: 30

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }
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

            objectName: "analysisStatusText"
            text: {
                const parts = [];
                if (control.alignmentModeName.length > 0)
                    parts.push(qsTr("对齐：%1").arg(control.alignmentModeName));
                if (control.differenceMode) {
                    // Dual difference reads one combined legend for both panes; the single-pair
                    // dimension wording stays for plain difference/grid modes.
                    if (control.dualDifferenceMode && control.dualDifferenceLegendText.length > 0)
                        parts.push(control.dualDifferenceLegendText);
                    else
                        parts.push(control.comparisonDimensionsLabel(control.selectedDifferenceEdge));
                }
                if (control.differenceThresholdEnabled)
                    parts.push(qsTr("阈值已启用 (%1)").arg(control.differenceThresholdCode));
                if (dualVideoSurface.roiEnabled)
                    parts.push(qsTr("ROI 已启用"));
                return parts.join(" · ");
            }
            // Exactness describes the pixel comparison (format, colour space, geometry), so only
            // difference mode is tinted; plain side-by-side viewing stays neutral. In dual mode
            // the tint covers both rendered panes under their one shared caliber.
            color: !control.differenceMode ? Theme.secondaryText : (control.dualDifferenceMode && control.dualDifferenceLegendText.length > 0 ? (control.dualDifferenceLegendExact ? Theme.success : Theme.warning) : (control.inexactReason.length === 0 && control.selectedDifferenceExactness === 0 ? Theme.success : Theme.warning))
            font.pixelSize: Theme.fontCaption
            width: Math.max(0, parent.width - 18)
            elide: Text.ElideRight
            anchors.centerIn: parent
        }
    }

    // Reuse the pixel-scale badge as a fixed 100% action. The adjacent readout keeps
    // the actual scale visible when aspect ratio or the surface limits prevent 1:1.
    ReviewActionButton {
        id: pixelScaleBadge

        objectName: "viewportPixelScaleBadge"
        enabled: visible && control.sourceCount > 0 && !control.overlayVisible && targetReachable
        text: targetReachable ? qsTr("设为 100%") : qsTr("100% 不可达")
        helpText: qsTr("按首个显示源围绕观察中心缩放到 100%，保留 ROI。\n小于适应窗口时显示完整 ROI 并居中；放大时中心受视口范围约束。\n非方形像素保留显示比例。\n放大上限仍为适应窗口倍率的 64 倍；缩小时每个显示轴至少占一个物理像素。\n以旁边的实际倍率为准；适应窗口请用上方按钮。")
        Accessible.name: text
        Accessible.description: pixelScaleLabel.text + "\n" + helpText
        visible: control.chromeVisible && control.hudRevealed && pixelScaleBadge.effectivePercent > 0
        opacity: control.hudRevealed ? 1 : 0
        z: 30

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }
        height: 28
        width: pixelScaleContents.implicitWidth + 18
        leftPadding: 9
        rightPadding: 9
        topPadding: 0
        bottomPadding: 0
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: parent.bottom
            bottomMargin: 12 + control.bottomOverlayInset + control.roiTripletStageInset
        }

        readonly property var firstPanel: dualVideoSurface.sourcePanelRects.length > 0 ? dualVideoSurface.sourcePanelRects[0] : null
        readonly property int sourceSlot: firstPanel ? Number(firstPanel.slot) : -1
        readonly property var sourceExtent: {
            if (sourceSlot < 0 || sourceSlot >= control.sourceMediaInfo.length)
                return Qt.size(0, 0);
            const info = control.sourceMediaInfo[sourceSlot];
            const roiWidth = dualVideoSurface.roiEnabled ? dualVideoSurface.roiRight - dualVideoSurface.roiLeft : 1;
            const roiHeight = dualVideoSurface.roiEnabled ? dualVideoSurface.roiBottom - dualVideoSurface.roiTop : 1;
            const width = Number(info.width) * roiWidth;
            const height = Number(info.height) * roiHeight;
            if (!isFinite(width) || !isFinite(height) || width <= 0 || height <= 0)
                return Qt.size(0, 0);
            const rotation = Number(info.rotationDegrees);
            return (rotation === 90 || rotation === 270) ? Qt.size(height, width) : Qt.size(width, height);
        }
        readonly property real devicePixelRatio: Window.window ? Window.window.devicePixelRatio : 1
        readonly property real horizontalPercent: firstPanel && sourceExtent.width > 0 ? Number(firstPanel.contentWidth) / sourceExtent.width * dualVideoSurface.viewScale * devicePixelRatio * 100 : 0
        readonly property real verticalPercent: firstPanel && sourceExtent.height > 0 ? Number(firstPanel.contentHeight) / sourceExtent.height * dualVideoSurface.viewScale * devicePixelRatio * 100 : 0
        readonly property real effectivePercent: isFinite(horizontalPercent) && isFinite(verticalPercent) ? Math.min(horizontalPercent, verticalPercent) : 0
        readonly property real targetScale: effectivePercent > 0 ? dualVideoSurface.viewScale * 100 / effectivePercent : 0
        readonly property bool targetReachable: isFinite(targetScale) && targetScale > 0 && dualVideoSurface.minimumViewScale > 0 && targetScale <= 64 && dualVideoSurface.canDisplayViewScale(targetScale)
        readonly property string targetLimitText: targetScale > 64 ? qsTr("超过 64 倍上限") : qsTr("显示范围不足")
        readonly property bool uniformScale: Math.abs(horizontalPercent - verticalPercent) < 0.001
        readonly property bool pixelExact: Math.abs(horizontalPercent - 100) < 0.001 && Math.abs(verticalPercent - 100) < 0.001

        function percentText(value) {
            return value > 0 && value < 1 ? "<1" : String(Math.round(value));
        }

        contentItem: Row {
            id: pixelScaleContents

            spacing: 10
            Label {
                text: pixelScaleBadge.text
                color: control.primaryTextColor
                font.pixelSize: Theme.fontCaption
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }
            Label {
                id: pixelScaleLabel

                objectName: "viewportPixelScaleLabel"
                text: {
                    const actual = pixelScaleBadge.pixelExact ? qsTr("100% 真实尺寸") : (pixelScaleBadge.uniformScale ? qsTr("画面 %1%").arg(pixelScaleBadge.percentText(pixelScaleBadge.effectivePercent)) : qsTr("横 %1% · 纵 %2%").arg(pixelScaleBadge.percentText(pixelScaleBadge.horizontalPercent)).arg(pixelScaleBadge.percentText(pixelScaleBadge.verticalPercent)));
                    return pixelScaleBadge.targetReachable ? actual : actual + " · " + pixelScaleBadge.targetLimitText;
                }
                color: pixelScaleBadge.pixelExact ? Theme.success : control.mutedTextColor
                font.pixelSize: Theme.fontCaption
                anchors.verticalCenter: parent.verticalCenter
            }
        }
        background: Rectangle {
            radius: 5
            color: pixelScaleBadge.hovered ? Theme.oscGlassHover : Theme.oscGlass
            border.width: pixelScaleBadge.activeFocus ? 2 : 1
            border.color: pixelScaleBadge.activeFocus ? Theme.focus : (pixelScaleBadge.pixelExact ? Theme.success : Theme.oscBorder)
        }
        onClicked: {
            if (!pixelScaleBadge.targetReachable)
                return;
            // The existing content/DPR/ROI/SAR-aware readout owns the target. zoomAt
            // follows its focal point and clamps both scale and center to the viewport bounds.
            const factor = 100 / pixelScaleBadge.effectivePercent;
            dualVideoSurface.zoomAt(0.5, 0.5, factor);
        }
    }

    // Sampling mode for the video slots: Auto follows the zoom (bilinear <= 200%, nearest
    // above), the explicit modes pin one behaviour. One button cycles the three modes so the
    // chrome stays compact; the label states the active mode.
    ReviewActionButton {
        id: videoFilterBadge

        objectName: "viewportVideoFilterBadge"
        enabled: visible && control.sourceCount > 0 && !control.overlayVisible
        visible: control.chromeVisible && control.hudRevealed
        opacity: control.hudRevealed ? 1 : 0
        z: 30

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }
        height: 28
        leftPadding: 9
        rightPadding: 9
        topPadding: 0
        bottomPadding: 0
        anchors {
            left: pixelScaleBadge.right
            leftMargin: 8
            bottom: parent.bottom
            bottomMargin: 12 + control.bottomOverlayInset + control.roiTripletStageInset
        }

        readonly property int mode: dualVideoSurface.videoFilterMode
        readonly property string modeText: mode === control.videoFilterPixelValue ? qsTr("像素") : (mode === control.videoFilterSmoothValue ? qsTr("平滑") : qsTr("自动"))
        text: qsTr("采样 %1").arg(modeText)
        helpText: qsTr("视频画面采样方式。\n自动：200% 以上用最近邻，保证高倍放大时像素边界可见；其余用双线性。\n平滑：始终双线性。\n像素：始终最近邻。")
        Accessible.name: text
        Accessible.description: helpText

        onClicked: {
            // Auto -> Smooth -> Pixel -> Auto.
            dualVideoSurface.videoFilterMode = (mode + 1) % 3;
        }
    }

    ReviewActionButton {
        objectName: "videoPixelGridButton"
        visible: videoFilterBadge.visible
        enabled: control.sourceCount > 0
        z: 30
        anchors.left: videoFilterBadge.right
        anchors.leftMargin: 8
        anchors.bottom: videoFilterBadge.bottom
        height: Theme.hitAreaMinimum
        text: control.pixelGridEnabled ? qsTr("网格 开") : qsTr("网格 关")
        helpText: qsTr("像素网格：仅在最近邻采样且横纵实际放大均达到 800% 时显示。")
        onClicked: control.pixelGridEnabled = !control.pixelGridEnabled
    }

    // Per-source frame-plane readout (1009 plan C slice 3): one row per session source, sampled
    // at the hovered content position. The values are the YCbCr codes held in the frame planes
    // the display path uses - the numeric review path; the picture itself is the display-converted
    // viewing path. Those planes are the normalized NV12/P010 buffers, so a source that was not
    // already 4:2:0 arrives resampled and its row says so. Presentation follows the image
    // workspace's cursorPixel readout.
    Rectangle {
        id: pixelReadoutPlate

        objectName: "viewportPixelReadoutPlate"
        visible: control.chromeVisible && control.hudRevealed && control.pixelReadoutPositionValid && control.sourceCount > 0
        opacity: control.hudRevealed ? 1 : 0
        clip: true
        z: 30

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }
        radius: 5
        color: Theme.oscGlass
        border.width: 1
        border.color: Theme.oscBorder
        width: Math.min(parent.width - 24, pixelReadoutColumn.implicitWidth + 16)
        height: pixelReadoutColumn.implicitHeight + 12
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: viewCommandRow.top
            bottomMargin: 8
        }
        Accessible.name: qsTr("像素规范化帧值读数")
        Accessible.description: qsTr("光标位置的帧平面 YCbCr 码值，逐源显示；Δ 列为该源减去当前参考源的差值。")

        // Fixed column widths measured on the widest value each column can carry, so digits
        // changing under the cursor never make neighbouring columns jitter. The monospace
        // family stays until the theme ships a mono font-stack token.
        TextMetrics {
            id: readoutCoordMetrics

            font.family: Theme.fontFamilyMono
            font.pixelSize: Theme.fontCaption
            text: "(4095, 4095)"
        }
        TextMetrics {
            id: readoutValuesMetrics

            font.family: Theme.fontFamilyMono
            font.pixelSize: Theme.fontCaption
            text: qsTr("冻结于第 9999 帧 · Y 1023  Cb 1023  Cr 1023")
        }
        TextMetrics {
            id: readoutDeltaMetrics

            font.family: Theme.fontFamilyMono
            font.pixelSize: Theme.fontCaption
            text: "Δ +1023 +1023 +1023"
        }

        Column {
            id: pixelReadoutColumn

            spacing: 3
            anchors.centerIn: parent

            Row {
                spacing: 6

                Text {
                    text: qsTr("规范化帧值 YCbCr")
                    color: control.mutedTextColor
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }

                Text {
                    visible: control.referenceSourceIndex >= 0 && control.referenceSourceIndex < control.sourceCount
                    // Delta baseline naming: the column reads "this source minus X" only when
                    // the session actually identifies a reference.
                    text: qsTr("· Δ 相对 %1").arg(String.fromCharCode(65 + control.referenceSourceIndex))
                    color: Theme.accentText
                    font.pixelSize: 10
                    font.weight: Font.DemiBold
                }

                HoverHandler {
                    id: pixelReadoutHeaderHover
                }

                VcsToolTip {
                    visible: pixelReadoutHeaderHover.hovered
                    text: qsTr("光标位置的帧平面 YCbCr 码值（数值审查路径）。\n读数是 NV12/P010 帧平面中的样本；来源的色度或位深可能在解码时已转换，逐行标出来源格式和「已转换」。\n画面本身经过显示转换（观看路径），两者不承诺一致。\n位深来自帧格式，Limited/Full 来自源元数据。\nΔ 列 = 该源 − 当前参考源（同一呈现帧、同一坐标下的差值）；无参考源或样本不完整时显示 —。\n播放中冻结读数并标出帧号；暂停后按呈现帧和光标位置重新取样。")
                }
            }

            Repeater {
                id: pixelReadoutRows

                objectName: "pixelReadoutRows"
                model: control.sourceCount

                // One aligned table row. The row container carries the legacy
                // `pixelReadoutRowText` objectName (the C++ suite's per-row handle); the
                // columns decompose the very same data the legacy string renders.
                delegate: Row {
                    id: pixelReadoutRow

                    required property int index

                    objectName: "pixelReadoutRowText"
                    readonly property bool hasValidSample: control.pixelReadoutHasValidSample(pixelReadoutRow.index)

                    spacing: 8

                    Rectangle {
                        width: 18
                        height: 15
                        radius: 3
                        color: Theme.sourceColor(pixelReadoutRow.index)
                        border.width: 1
                        border.color: Theme.sourceBorder(pixelReadoutRow.index)

                        Text {
                            anchors.centerIn: parent
                            text: String.fromCharCode(65 + pixelReadoutRow.index)
                            color: Theme.sourceInk
                            font.bold: true
                            font.pixelSize: 10
                        }
                    }
                    Text {
                        // Coordinates: fixed width, left aligned.
                        width: readoutCoordMetrics.advanceWidth
                        text: control.pixelReadoutCoordText(pixelReadoutRow.index)
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        // Y / Cb / Cr values (with the frozen marker when the sample outlived
                        // its frame); unavailable rows keep the legacy wording in this column.
                        width: pixelReadoutRow.hasValidSample ? readoutValuesMetrics.advanceWidth : Math.min(implicitWidth, readoutValuesMetrics.advanceWidth + readoutDeltaMetrics.advanceWidth)
                        elide: Text.ElideRight
                        text: pixelReadoutRow.hasValidSample ? control.pixelReadoutValuesText(pixelReadoutRow.index) : control.pixelReadoutRowText(pixelReadoutRow.index)
                        color: control.primaryTextColor
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.fontFamilyMono
                    }
                    Text {
                        // Delta vs the reference source; "—" while the pair is not comparable
                        // (no reference, undelivered side, stale sample, or differing bit
                        // depth / colour range), with the reason one hover away. Unavailable
                        // rows let their wording span this column instead.
                        visible: pixelReadoutRow.hasValidSample
                        width: visible ? readoutDeltaMetrics.advanceWidth : 0
                        text: control.pixelReadoutDeltaText(pixelReadoutRow.index)
                        color: control.primaryTextColor
                        font.pixelSize: Theme.fontCaption
                        font.family: Theme.fontFamilyMono

                        HoverHandler {
                            id: deltaColumnHover
                        }

                        VcsToolTip {
                            visible: deltaColumnHover.hovered
                            text: {
                                const reason = control.pixelReadoutDeltaReason(pixelReadoutRow.index);
                                return reason.length > 0 ? qsTr("Δ 不可用：%1").arg(reason) : qsTr("Δ = 本源 − 参考源（同一呈现帧、同一坐标、位深与色彩范围一致）");
                            }
                        }
                    }
                    Text {
                        // Format badge: normalized plane format, depth, range, source format
                        // and the conversion marker, elided at the cap rather than pushing the
                        // plate wider.
                        visible: pixelReadoutRow.hasValidSample
                        width: Math.min(implicitWidth, 150)
                        elide: Text.ElideRight
                        text: control.pixelReadoutFormatText(pixelReadoutRow.index)
                        color: control.mutedTextColor
                        font.pixelSize: 10
                    }
                }
            }
        }
    }

    // Hold-to-peek indicator: names the source the difference canvas is showing while the peek
    // holds, including when it is locked. It outlives the transient HUD line, so "I am looking
    // at the GT" stays visible for as long as it is true.
    Rectangle {
        id: differencePeekIndicator

        objectName: "differencePeekIndicator"
        visible: control.differencePeekActive && control.differenceMode
        opacity: visible ? 1 : 0
        radius: 5
        color: Theme.oscGlass
        border.width: 1
        border.color: Theme.oscBorder
        width: Math.min(Math.max(0, parent.width - 24), peekLabel.implicitWidth + 20)
        height: 26
        z: 32
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: pixelReadoutPlate.visible ? pixelReadoutPlate.top : viewCommandRow.top
            bottomMargin: 8
        }

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }

        Text {
            id: peekLabel

            anchors.centerIn: parent
            width: parent.width - 20
            elide: Text.ElideRight
            text: control.differencePeekLocked ? qsTr("已锁定直看 GT：%1").arg(control.differencePeekTargetLabel) : qsTr("直看 GT：%1").arg(control.differencePeekTargetLabel)
            color: control.primaryTextColor
            font.pixelSize: Theme.fontCaption
        }
    }

    // Fit / reset commands, same product verbs as the image workspace toolbar.
    Row {
        id: viewCommandRow

        objectName: "viewportViewCommands"
        enabled: visible && control.sourceCount > 0 && !control.overlayVisible
        visible: control.chromeVisible && control.hudRevealed
        opacity: control.hudRevealed ? 1 : 0
        z: 30

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationBase
            }
        }
        spacing: 6
        anchors {
            left: parent.left
            leftMargin: 12
            bottom: pixelScaleBadge.top
            bottomMargin: 8
        }

        ViewCommandButton {
            objectName: "viewportFitButton"
            text: qsTr("适应窗口")
            helpText: qsTr("适应窗口并居中，保留 ROI；不改变当前帧或比较对象。")
            onClicked: control.fitToWindow()
        }
        // E1: wipe split axis toggle - a discoverable entry next to the fit/reset commands,
        // visible only in wipe mode while the full canvas is not in a direct look. The label
        // names the halves (左右/上下) instead of the axis jargon. ViewCommandButton is
        // 24 px tall by default, below the 32 px hit-area minimum, so this new instance
        // raises only its own height (the legacy buttons keep theirs; no whitelist entry).
        ViewCommandButton {
            objectName: "viewportWipeOrientationButton"
            visible: control.wipeMode && control.canvasFocusSlot < 0
            enabled: visible && !control.overlayVisible
            height: Theme.hitAreaMinimum
            text: control.wipeOrientation === 1 ? qsTr("分屏：上下") : qsTr("分屏：左右")
            helpText: control.wipeOrientation === 1 ? qsTr("当前上下切分；点击改为左右切分（W 键）。分屏比例保持不变。") : qsTr("当前左右切分；点击改为上下切分（W 键）。分屏比例保持不变。")
            onClicked: control.setWipeOrientation(control.wipeOrientation === 1 ? 0 : 1)
        }
        ViewCommandButton {
            objectName: "viewportResetButton"
            text: qsTr("重置视图")
            helpText: qsTr("清除 ROI，并适应窗口、居中；不改变当前帧或比较对象。")
            onClicked: control.resetView()
        }
        // I02: the visible ROI entry. Checkable with the mode bar's mirrorsState contract -
        // the checked fill names the armed state, the cross cursor repeats it on the canvas,
        // and Alt+drag keeps working without the button.
        ViewCommandButton {
            id: roiSelectButton

            objectName: "viewportRoiSelectButton"
            checkable: true
            checked: control.roiSelectMode
            mirrorsState: true
            text: control.roiSelectMode ? qsTr("框选区域 · 开") : qsTr("框选区域")
            helpText: qsTr("开启后拖动框选比较 ROI（画布为十字光标）；Esc、重置视图或再次点击退出。\nAlt+拖动仍可直接框选；Shift+拖动放大所选区域；未开启时默认拖动为平移，滚轮以光标为中心缩放。")
            onClicked: control.roiSelectMode = !control.roiSelectMode
        }
    }

    component ViewCommandButton: ReviewActionButton {
        id: commandButton

        Accessible.name: text
        Accessible.description: helpText
        width: commandLabel.implicitWidth + 16
        height: 24
        leftPadding: 8
        rightPadding: 8
        topPadding: 0
        bottomPadding: 0
        contentItem: Label {
            id: commandLabel

            text: commandButton.text
            color: commandButton.hovered ? control.primaryTextColor : control.mutedTextColor
            font.pixelSize: Theme.fontCaption
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
        }
        background: Rectangle {
            radius: Theme.radiusMicro
            color: commandButton.hovered ? Theme.oscGlassHover : Theme.oscGlass
            border.width: commandButton.activeFocus ? 2 : 1
            border.color: commandButton.activeFocus ? Theme.focus : (commandButton.hovered ? Theme.borderHover : Theme.oscBorder)
        }
    }

    Rectangle {
        id: differenceUnavailableOverlay

        objectName: "differenceUnavailableOverlay"
        visible: control.differenceUnavailableDetail.length > 0
        width: Math.min(parent.width - 48, 460)
        height: unavailableColumn.implicitHeight + 32
        radius: Theme.radiusSmall
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
        // Dual difference aggregates the two rendered panes' reasons into one note (the active
        // pair selector may describe a pair the canvas is not showing).
        readonly property string effectiveInexactReason: control.dualDifferenceMode && control.dualDifferenceInexactDetail.length > 0 ? control.dualDifferenceInexactDetail : control.inexactReason

        visible: control.chromeVisible && control.differenceMode && effectiveInexactReason.length > 0
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

            text: qsTr("⚠️ 对比非精确对应：%1").arg(inexactDifferenceBanner.effectiveInexactReason)
            color: Theme.warningText
            font.pixelSize: Theme.fontCaption
            font.weight: Font.DemiBold
            anchors.centerIn: parent
        }
    }

    Rectangle {
        id: candidateDifferenceBanner

        objectName: "candidateDifferenceBanner"
        visible: control.chromeVisible && control.differenceMode && !control.dualDifferenceMode && control.selectedDifferenceEdge && Boolean(control.selectedDifferenceEdge.isCandidateOnlyDifference)
        radius: 5
        color: Theme.panel
        border.color: Theme.borderHover
        border.width: 1
        height: candidateBannerText.implicitHeight + 10
        width: Math.min(parent.width - 24, candidateBannerText.implicitWidth + 20)
        z: 20
        anchors {
            top: inexactDifferenceBanner.visible ? inexactDifferenceBanner.bottom : (alignmentStatus.visible ? alignmentStatus.bottom : parent.top)
            topMargin: inexactDifferenceBanner.visible || alignmentStatus.visible ? 8 : control.stageBannerTop
            horizontalCenter: parent.horizontalCenter
        }

        Text {
            id: candidateBannerText

            text: qsTr("ℹ️ 候选差异，不代表质量优劣")
            color: Theme.accentText
            font.pixelSize: Theme.fontCaption
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
                radius: Theme.radiusSmall
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
                        if (control.dualDifferenceMode) {
                            const refLetter = String.fromCharCode(65 + control.referenceSourceIndex);
                            const candLetter = String.fromCharCode(65 + surfaceLabel.sourceSlot);
                            return qsTr("差异：%1 ↔ %2 (参考)").arg(candLetter).arg(refLetter);
                        }
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
        radius: Theme.radiusSubtle
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
                font.pixelSize: Theme.fontBody
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "frameErrorBannerDetail"
                width: parent.width
                text: control.errorDetail
                color: control.primaryTextColor
                font.pixelSize: Theme.fontSecondary
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
            spacing: Theme.spaceMain
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
                font.pixelSize: Theme.fontSecondary
                lineHeight: Theme.lineHeightCompact
                wrapMode: Text.Wrap
            }
        }
    }

    // V01 "全景 + 局部": the local windows are renderer-side extra draws of the same presented
    // frame set. Every ROI and transform input is the surface's own state, so the windows share
    // one ROI and follow zoom/pan/source switches by construction; only the window zoom and the
    // staged content rects live here. Declared last so its controls sit above the navigation
    // MouseArea. Entering or leaving the preset never seeks, changes GT or playback: the only
    // state it owns is the window zoom percent.
    RoiTripletStrip {
        id: roiTripletStrip

        objectName: "roiTripletStrip"
        visible: control.roiTripletStageActive
        enabled: visible
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            margins: control.chromeVisible ? 1 : 0
        }
        height: visible ? Math.max(LayoutMetrics.localStripMinimumHeight, control.height * LayoutMetrics.clampLocalStripShare(control.roiTripletStripShare)) : 0
        // qmllint disable incompatible-type
        // ComparisonSurface is runtime-registered, so qmllint cannot see it derives Item.
        sourceItem: dualVideoSurface
        // qmllint enable incompatible-type
        sourceCount: control.sourceCount
        sourceLabels: control.sourceNames
        roiEnabled: dualVideoSurface.roiEnabled
        zoomPercent: control.roiTripletZoomPercent
        // Shared sampling preference, not a second knob: the pixel mode pins nearest, auto
        // mirrors the surface's own nearest rule against the window zoom (the threshold comes
        // from the surface, so the two cannot drift), smooth stays bilinear. 适配ROI (0) keeps
        // the smooth default.
        smoothSampling: dualVideoSurface.videoFilterMode !== control.videoFilterPixelValue && !(dualVideoSurface.videoFilterMode === control.videoFilterAutoValue && control.roiTripletZoomPercent >= dualVideoSurface.autoNearestThresholdPercent)
        // Published footage extents chrome; resolved by the same pure geometry the renderer
        // draws with.
        windowRects: dualVideoSurface.roiTripletWindowRects
        onZoomStepRequested: percent => control.roiTripletZoomPercent = percent
        // Phase-3 local-window drag: the strip reports logical-pixel deltas, the surface owns
        // the window->source geometry (rotation and magnification included) in one place.
        onRoiPanRequested: (slot, deltaLogicalX, deltaLogicalY) => dualVideoSurface.panRoiByLocalWindowPixels(slot, deltaLogicalX, deltaLogicalY)
    }

    // U01: the local strip's drag splitter, on the strip's top edge. Dragging up enlarges the
    // strip's share of the workspace; the clamp keeps the share in the 28-40 % band, so the
    // panorama can neither crush the locals nor swallow the workspace.
    MouseArea {
        id: roiTripletStripSplitter

        objectName: "roiTripletStripSplitter"
        visible: control.roiTripletStageActive
        enabled: visible
        z: 60
        height: 8
        cursorShape: Qt.SizeVerCursor
        anchors {
            left: parent.left
            right: parent.right
            bottom: roiTripletStrip.top
            bottomMargin: -roiTripletStripSplitter.height / 2
        }

        property real dragStartShare: 0

        onPressed: mouse => {
            roiTripletStripSplitter.dragStartShare = control.roiTripletStripShare;
            mouse.accepted = true;
        }
        onPositionChanged: mouse => {
            if (!roiTripletStripSplitter.pressed)
                return;
            const workspaceHeight = Math.max(1.0, control.height);
            control.roiTripletStripShare = LayoutMetrics.clampLocalStripShare(roiTripletStripSplitter.dragStartShare - (mouse.y - roiTripletStripSplitter.height / 2) / workspaceHeight);
        }
    }

    // qmllint disable incompatible-type
    // ComparisonSurface is runtime-registered, so qmllint cannot see the target derives Item.
    Binding {
        // The strip stages each window cell's content rect (surface item coordinates) and the
        // renderer turns it into an additional draw of the presented frame set. A hidden strip
        // stages an empty list, so no stale local draw can outlive the preset.
        target: dualVideoSurface
        property: "roiTripletWindows"
        value: roiTripletStrip.stagedWindows
    }
    // qmllint enable incompatible-type
}
