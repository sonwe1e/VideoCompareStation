pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Window
import "VcsTheme.js" as Theme

Item {
    id: control

    required property int controllerState // 0 pinned, 1 auto, 2 hidden
    // Docked (pinned-below-canvas) vs overlay transport. Bound by Main.qml; defaults to false so
    // the component also instantiates standalone (e.g. in QML unit tests) without binding it.
    property bool docked: false
    property string sourceLabel
    // PairMetricsController context object; optional so standalone QML tests keep working.
    property var metrics: null
    // InterpolationProtocolController context object (P03); optional for the same reason. The
    // host owns the controller, the OSC only relays state and intents like it does for range
    // export.
    property var protocol: null
    // E01 training-sample triplet export context and status.
    property var sampleExport: null
    property bool sampleExportBusy: false
    required property bool playing
    property real playbackRate: 1
    // E04 long-press walk rate, mirrored from the controller by Main.qml like `playbackRate`.
    property real jogRate: 1
    // C-07 playback status rail. Bound by Main.qml; defaults keep standalone QML tests working.
    property string playbackModeLabel: ""
    property string playbackModeDetail: ""
    property real playbackTargetRate: 1
    property real playbackPresentationRate: 0
    property int playbackRunSkippedFrameSets: 0
    // Frames this run finished presenting. 0 = no sample yet, so the skip count is unmeasured.
    property int playbackRunPresentedFrames: 0
    property int playbackLagMilliseconds: 0
    property bool playbackCatchingUp: false
    property int displayGapCount: 0
    property int sourceDuplicateCount: 0
    property string sourceRateText: ""
    property string statusLegend: qsTr("源重复数来自有限的对齐标记；播放器跳过数表示追赶时跳过的完整帧组；呈现间隙表示呈现确认的帧号间隔，可能与播放器跳过重合。三项不能相加，也不代表显示器实际漏刷次数。")
    // I8: mirrors the active shortcut preset so the transport's ±1 s / ±30 s buttons follow the
    // same preset ReviewShortcuts resolves. 0 = frame-review, 1 = player. Forwarded to the
    // TransportBar (tooltips) and consulted below (action routing).
    property int shortcutPreset: 0
    // C-07 quiet-capsule rule: a run earns the full warning rail only once something is
    // actually wrong - skipped FrameSets, catch-up, or >= 200 ms lag. "Not measured yet" is
    // NOT one of them: it is a caveat on the same readouts, so it rides in the quiet capsule
    // in a neutral colour instead of borrowing the yellow that real failures need. Reporting
    // every freshly opened session in warning yellow taught users that yellow means normal.
    readonly property bool playbackStatusAlert: control.playbackRunSkippedFrameSets > 0 || control.playbackCatchingUp || control.playbackLagMilliseconds >= 200
    readonly property bool playbackStatusUnmeasured: control.playbackRunPresentedFrames === 0
    required property bool timelineEnabled
    required property int currentFrame
    required property int totalFrames
    required property real progress
    required property string timecodeText
    required property var markers
    required property var actions
    required property Item focusTarget
    required property bool canFirst
    required property bool canPrevious
    required property bool canPlay
    required property bool canPause
    required property bool canNext
    required property bool canLast
    property int inFrame: -1
    property int outFrame: -1
    property bool loopRangeActive: false
    // E3: back-and-forth range walk toggle state, forwarded to the transport bar.
    property bool loopRangePingPongActive: false
    // Range clip export, forwarded to the transport chip. The host owns the export adapter, so the
    // OSC only relays visibility, gate and progress.
    property bool rangeExportVisible: false
    property bool rangeExportEnabled: false
    property bool rangeExportBusy: false
    property real rangeExportProgress: 0
    readonly property bool canMarkRange: control.timelineEnabled && control.currentFrame >= 0
    property int previewFrame: -1
    property string previewTimecode: "00:00:00:00"
    property url previewThumbnailSource: ""
    property bool previewIsApproximate: false
    property int previewSampleFrame: -1
    property int playbackContinuityPolicy: 1
    property bool revealActive: controllerState === 0
    // The cursor is somewhere on the controls. This is the single gate the auto-hide countdown
    // consults, so a bar under the cursor never fades out — no matter which child owns the hover
    // (the timeline thumb strip, a transport chip, the metric lane) and no matter how the reveal
    // was triggered.
    readonly property bool pointerInsidePanel: panelHover.hovered || tracks.pointerInside
    readonly property bool controlsEnabled: control.docked || control.controllerState === 0 || control.revealActive
    // UX-13 S1 compact-chrome tier (docs/plans/ux13-narrow-window-redesign.md §2 D1): the
    // small-window tier sheds every non-essential layout line. 680, not 640: Main.qml's
    // window minimum is 960x640, so the OS clamps every smaller request and the settled
    // height dithers between 640 and 641 - a 640 threshold sat exactly on the minimum and
    // the tier flipped at random (S2 gate run). 680 gives the minimum + frame rounding a
    // stable band, and real small windows (641-680, the laptop norm) enter the tier
    // reliably. Derived from the hosting window so Main.qml needs no wiring; a plain
    // property so standalone QML tests can pin either tier explicitly. S4 moves the switch
    // up to the shell.
    property bool compactChrome: Window.window !== null && Window.window.height <= 680
    // Single authority for whether the metrics lane renders. Big windows follow the
    // controller's laneEnabled toggle; the compact tier keeps the lane off the stack entirely -
    // the inspector stays the metrics surface, and no sampling is added or removed by this gate.
    readonly property bool metricsLaneVisible: control.metrics !== null && control.metrics.available && !control.compactChrome && control.metrics.laneEnabled
    // Compact-tier overlay: the info line leaves the vertical stack and rides the transport
    // main row's free right side (the row is left-aligned in this tier), so timecode, frame
    // counter and the UX-02 quiet sentence stay on screen without spending a layout line.
    // Width-guarded so the overlay never crowds the buttons on a narrower window.
    readonly property bool readoutOverlay: control.compactChrome && control.width >= 920
    // UX-13 S1: forwards the compact range-row toggle so hosts and the QML suite reach the
    // summary chip's state without digging into the transport bar.
    property alias rangeExpandedCompact: transport.rangeExpandedCompact

    signal previewRequested(int frame)
    signal seekRequested(int frame)
    signal overlayHidden
    // In/out range intents. Forwarded verbatim from the transport's range row; the host (Main.qml)
    // owns the range mutation functions, so the OSC never touches session state directly.
    signal markInRequested
    signal markOutRequested
    signal playRangeRequested
    signal rangeLoopRequested
    signal rangePingPongRequested
    signal clearRangeRequested
    signal exportRangeRequested
    signal exportSampleRequested
    // P03 protocol navigation intents, forwarded verbatim from the transport's protocol row.
    signal protocolLoadRequested
    signal protocolClearRequested
    signal previousGeneratedFrameRequested
    signal nextGeneratedFrameRequested

    objectName: "transport"
    // The trailing gap is the main step, so the transport sits under the timeline the same way
    // it does inside the panel instead of hugging the panel edge.
    // Compact tier: the info line overlays the transport row and the metrics lane stays
    // hidden, so the stack is only the (failure-only) warning line above the timeline; both
    // gaps drop to the micro step. The 13 px clearance still guards the playhead thumb.
    height: control.compactChrome ? Theme.spaceMicro + (statusText.visible ? statusText.height + Theme.spaceMicro : 0) + (!control.readoutOverlay ? readout.height + Theme.spaceMicro : 0) + tracks.height + 13 + transport.implicitHeight + Theme.spaceMicro : Math.max(0, tracks.y) + tracks.height + 13 + transport.implicitHeight + Theme.spaceMain
    visible: controllerState !== 2

    onPointerInsidePanelChanged: {
        // One authority for the countdown, so every way the cursor can leave the controls starts
        // it exactly once — moving off the panel, dropping the timeline's grab, or releasing a
        // scrub past the edge. A handler that only watched one of those sources left the bar stuck
        // visible after a scrub released outside the panel.
        if (control.pointerInsidePanel) {
            hideTimer.stop();
        } else if (control.controllerState === 1 && !control.docked && control.revealActive) {
            hideTimer.restart();
        }
    }

    onControllerStateChanged: {
        hideTimer.stop();
        revealActive = controllerState === 0;
    }

    onRevealActiveChanged: {
        if (!revealActive)
            overlayHidden();
    }

    function reveal() {
        if (controllerState !== 1)
            return;
        revealActive = true;
        // Re-arms the countdown only when the cursor is NOT already on the controls. The host
        // calls this for every transport seek (scrubbing the bar, the wheel on the bar); re-arming
        // the countdown in that case used to fade the bar out from under the cursor 1.2 s later.
        if (control.pointerInsidePanel)
            hideTimer.stop();
        else
            hideTimer.restart();
    }

    // The full status rail: every delivered observation, one part each. A function (not just a
    // binding) so the warning rail and the quiet capsule's hover tooltip render the exact same
    // text and can never drift apart.
    function playbackStatusFullText() {
        const parts = [];
        if (control.playbackModeLabel.length > 0)
            parts.push(control.playbackModeLabel);
        const target = control.playbackTargetRate;
        const actual = control.playbackPresentationRate;
        if (control.playing && actual > 0)
            parts.push(qsTr("目标 %1× · 实际 %2×").arg(target.toFixed(2)).arg(actual.toFixed(2)));
        else
            parts.push(qsTr("目标 %1×").arg(target.toFixed(2)));
        if (control.playbackRunSkippedFrameSets > 0 || control.playbackCatchingUp)
            parts.push(qsTr("播放器跳过 %1 组").arg(control.playbackRunSkippedFrameSets));
        const lagMs = control.playbackLagMilliseconds;
        if (control.playbackCatchingUp)
            parts.push(qsTr("追赶中"));
        else if (lagMs >= 500)
            parts.push(qsTr("明显落后 %1s").arg((lagMs / 1000).toFixed(1)));
        else if (lagMs >= 200)
            parts.push(qsTr("落后 %1s").arg((lagMs / 1000).toFixed(1)));
        if (control.sourceDuplicateCount > 0)
            parts.push(qsTr("重复标记 %1").arg(control.sourceDuplicateCount));
        if (control.displayGapCount > 0)
            parts.push(qsTr("呈现间隙 %1").arg(control.displayGapCount));
        // A run that has not finished presenting a single FrameSet has no sample behind any
        // of those counters. Omitting them reads as "nothing went wrong"; saying so is the
        // difference between an unmeasured run and a clean one (product section 2).
        if (control.playbackRunPresentedFrames === 0)
            parts.push(qsTr("本次播放尚未测量"));
        return parts.join(" · ");
    }

    // C-07 quiet summary: mode, target rate, and - while the run has no sample behind it -
    // the unmeasured admission. Actual presentation rate, duplicates and gaps stay in the
    // full rail (hover) until one of them is actually wrong.
    function playbackStatusQuietText() {
        const parts = [];
        if (control.playbackModeLabel.length > 0)
            parts.push(control.playbackModeLabel);
        parts.push(qsTr("目标 %1×").arg(control.playbackTargetRate.toFixed(2)));
        // Silence would read as "clean" before the first presented frame, so the capsule has
        // to say what it does not know (product intent section 2). It says it in `mutedText`,
        // the same weight as the numbers it qualifies.
        if (control.playbackStatusUnmeasured)
            parts.push(qsTr("本次播放尚未测量"));
        return parts.join(" · ");
    }

    // One tooltip for both status surfaces (quiet capsule and warning rail): the strategy
    // explanation that used to own its own line, the source rate note, the full observation
    // rail and the counters' legend. Nothing delivered is dropped - the capsule only decides
    // how loudly the normal state speaks.
    function playbackStatusToolTipText() {
        const parts = [];
        if (control.playbackModeDetail.length > 0)
            parts.push(control.playbackModeDetail);
        if (control.sourceRateText.length > 0)
            parts.push(control.sourceRateText);
        const detail = parts.join(" · ");
        return (detail.length > 0 ? detail + "\n" : "") + control.playbackStatusFullText() + "\n\n" + control.statusLegend;
    }

    // The range endpoints speak the task language (区间起点 / 区间终点); a marker's kind comes
    // from the one shared mapping (`Theme.timelineMarkerLabel`) that the timeline tooltip and
    // the inspector legend also read, so the same kind never gets two different names.
    function markerLabelForFrame(frame) {
        if (frame < 0)
            return "";
        if (control.inFrame >= 0 && frame === control.inFrame)
            return qsTr("区间起点标记");
        if (control.outFrame >= 0 && frame === control.outFrame)
            return qsTr("区间终点标记");
        for (let i = 0; i < control.markers.length; ++i) {
            const m = control.markers[i];
            if (Math.abs(Number(m.frame) - frame) <= 1)
                return Theme.timelineMarkerLabel(String(m.kind));
        }
        return "";
    }

    Rectangle {
        id: panel

        objectName: "oscPanel"
        // The panel fills the control; the control's height is content-driven (see `height` binding
        // below) so the timeline playhead thumb always clears the TransportBar. The TransportBar
        // anchors to the tracks (the content above it), NOT the panel bottom — anchoring it to the
        // panel would create a bottom-to-bottom binding loop.
        anchors {
            top: parent.top
            bottom: parent.bottom
            left: parent.left
            right: parent.right
            leftMargin: control.docked ? 0 : Math.max(16, (control.width - 920) / 2)
            rightMargin: control.docked ? 0 : Math.max(16, (control.width - 920) / 2)
            bottomMargin: control.docked ? 0 : 8
        }
        radius: control.docked ? 0 : Theme.radiusCard
        opacity: control.controlsEnabled ? 1.0 : 0.0
        enabled: control.controlsEnabled
        color: control.docked ? Theme.panel : Theme.oscGlass
        border.color: control.docked ? Theme.border : Theme.oscBorder

        Behavior on opacity {
            NumberAnimation {
                duration: Theme.animationFast
            }
        }

        HoverHandler {
            id: panelHover

            onHoveredChanged: {
                if (hovered) {
                    control.revealActive = true;
                    hideTimer.stop();
                }
                // Leaving is handled by onPointerInsidePanelChanged below: the cursor may still be
                // on the controls even while this handler lost the hover (the timeline, a chip, a
                // live scrub), and only the aggregate answer can tell.
            }
        }

        // One info line (Phase-3): filename, timecode, frame counter, metric summary, the
        // metric-lane toggle and the quiet status capsule share the row under the panel top.
        // The filename used to own a dedicated line even though the source strip above already
        // names the file for multi-source sessions; it stays readable here (elided) for the
        // single-source sessions where the strip is hidden.
        Row {
            id: readout

            // Compact tier: the row overlays the transport main row's free right side
            // (right-aligned through a computed left margin, so no anchor line switch is
            // needed) and paints above the transport; z keeps it there. The big window keeps
            // the stacked info line under the panel top.
            z: 2
            spacing: control.readoutOverlay ? Theme.spaceMain : Theme.spaceGroup
            anchors {
                top: control.readoutOverlay ? transport.top : parent.top
                left: parent.left
                leftMargin: control.readoutOverlay ? parent.width - Theme.spaceMain - width : 16
                topMargin: control.readoutOverlay ? transport.mainRowCenterY - Math.round(height / 2) : (control.compactChrome ? Theme.spaceMicro : 8)
            }

            Text {
                id: sourceLabelReadout
                objectName: "sourceLabelReadout"
                // Compact overlay drops the filename: the source strip above already names the
                // session and the overlay line budget belongs to the readouts.
                visible: control.sourceLabel !== "" && !control.readoutOverlay
                text: control.sourceLabel
                color: Theme.mutedText
                font.family: Theme.fontFamilyMono
                font.pixelSize: Theme.fontSecondary
                // Elide long filenames instead of overflowing the panel edge (the panel has
                // no clip); the hover tooltip carries the full name.
                width: Math.min(implicitWidth, 180)
                elide: Text.ElideRight
                anchors.verticalCenter: parent.verticalCenter

                HoverHandler {
                    id: sourceLabelHover
                }

                VcsToolTip {
                    visible: sourceLabelHover.hovered && control.sourceLabel.length > 0
                    text: control.sourceLabel
                }
            }

            Text {
                text: control.timecodeText
                color: Theme.primaryText
                font.family: Theme.fontFamilyMono
                font.pixelSize: Theme.fontMono
                font.weight: Font.DemiBold
            }
            Text {
                objectName: "frameCounterText"
                text: control.currentFrame >= 0 ? qsTr("第 %1 / %2 帧").arg(control.currentFrame + 1).arg(control.totalFrames) : qsTr("无帧")
                color: Theme.mutedText
                font.pixelSize: Theme.fontSecondary
                // Stepping re-writes these digits in place; tabular figures keep the row from
                // shuffling sideways on every frame (UX-18).
                font.features: Theme.fontFeaturesNumeric
            }
            // The in/out range readout moved into the transport's range row
            // (`transportRangeLabel`): endpoints, their buttons and the frame count now sit in one
            // cluster instead of being split between the status rail and the transport.
            // V-07 compact pair-metrics readout. Sits next to the timecode so the current
            // frame's MAE/PSNR answer "how different is this frame" without opening the
            // inspector. Hidden entirely when metrics are unavailable (single source or no
            // service), including in standalone QML tests.
            Text {
                objectName: "metricsSummaryText"

                // Compact tier hides the inline summary with the lane: the inspector carries
                // the metrics surface there.
                visible: control.metrics !== null && control.metrics.available && !control.compactChrome
                text: {
                    if (control.metrics === null)
                        return "";
                    if (!control.metrics.hasCurrentSample)
                        return control.metrics.sampling ? qsTr("指标 采样中…") : qsTr("指标 未采样");
                    if (!control.metrics.currentComparable)
                        return qsTr("指标 不可比");
                    const psnr = control.metrics.currentPsnrDb >= 999.0 ? qsTr("∞") : control.metrics.currentPsnrDb.toFixed(1) + qsTr(" dB");
                    return qsTr("MAE %1 · PSNR %2").arg(control.metrics.currentMae.toFixed(2)).arg(psnr);
                }
                // A measurement, not a warning: the warning yellow is reserved for playback
                // alerts, and this panel shows those one row down.
                color: Theme.secondaryText
                font.pixelSize: Theme.fontSecondary
                font.family: Theme.fontFamilyMono
            }

            // Phase-3: the metric curve rides behind an explicit toggle. Collapsed keeps the
            // single-frame MAE/PSNR summary above as the always-on readout; the button is a
            // Control, so its keyboard focus ring follows the visualFocus-only contract.
            ReviewActionButton {
                id: metricsLaneToggle

                objectName: "oscMetricsLaneToggle"
                visible: control.metrics !== null && control.metrics.available && !control.compactChrome
                enabled: visible
                // The theme's hit-area minimum, not a smaller literal: this is an interactive
                // control sharing the info row with texts, the row's height is content-driven
                // (nothing below anchors into a fixed band), and the token-literal gate keeps
                // 26 px sites on a frozen baseline.
                implicitHeight: Theme.hitAreaMinimum
                leftPadding: 10
                rightPadding: 10
                text: control.metrics !== null && control.metrics.laneEnabled ? qsTr("收起指标曲线 ▴") : qsTr("展开指标曲线 ▾")
                helpText: qsTr("展开或收起全时间轴的差异指标曲线（MAE 泳道）。\n收起时保留上方当前帧的 MAE / PSNR 摘要。")
                onClicked: {
                    if (control.metrics !== null)
                        control.metrics.laneEnabled = !control.metrics.laneEnabled;
                }
            }

            // C-07 quiet capsule, inline in the info line: a normal run keeps one calm summary
            // on the same row as timecode, frame counter and metrics. Only a run that skipped
            // FrameSets, is catching up or lags >= 200 ms expands the dedicated warning rail
            // below; an unmeasured run is stated here instead, in the same neutral colour as
            // the readouts it qualifies. The full text stays one hover away in both states.
            Text {
                id: statusQuiet

                objectName: "playbackStatusQuiet"
                visible: !control.playbackStatusAlert && control.playbackModeLabel.length > 0
                text: control.playbackStatusQuietText()
                color: Theme.mutedText
                font.pixelSize: Theme.fontSecondary
                // Compact overlay caps the capsule so the info line never reaches the buttons;
                // the hover tooltip keeps the full sentence in every tier (UX-02).
                elide: Text.ElideRight
                width: control.readoutOverlay ? Math.min(implicitWidth, 150) : implicitWidth
                Accessible.name: text
                Accessible.description: control.playbackStatusToolTipText()
                anchors.verticalCenter: parent.verticalCenter

                HoverHandler {
                    id: statusQuietHover
                }

                VcsToolTip {
                    visible: statusQuietHover.hovered
                    text: control.playbackStatusToolTipText()
                }
            }
        }

        // C-07 playback status rail, failure-only edition: the row exists while something is
        // actually wrong (see playbackStatusAlert), showing the full warning text in the
        // warning colour. An unmeasured run never reaches this rail - it is admitted by the
        // neutral quiet capsule above, so warning yellow keeps meaning "look at this".
        // Source duplicates, player FrameSet skips, and display/present gaps stay separated
        // so a stall is never mistaken for judder.
        Text {
            id: statusText

            objectName: "playbackStatusText"
            visible: control.playbackStatusAlert && control.playbackModeLabel.length > 0
            // No height binding on purpose: this Text is anchored (not a positioner child),
            // the items below switch their top anchor on `statusText.visible`, and the hidden
            // statusHint anchor consumes nothing - so the old `visible ? implicitHeight : 0`
            // collapse had no reader while invisible while re-entering QQuickText's implicit
            // height (Qt logged a height binding loop on every alert-state flip). The default
            // implicit-height tracking gives the identical visible-state layout.
            elide: Text.ElideRight
            text: control.playbackStatusFullText()
            color: Theme.warning
            font.pixelSize: Theme.fontSecondary
            Accessible.name: text
            Accessible.description: control.playbackStatusToolTipText()
            anchors {
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
                top: control.readoutOverlay ? parent.top : readout.bottom
                topMargin: Theme.spaceMicro
            }

            HoverHandler {
                id: statusHover
            }

            VcsToolTip {
                visible: statusHover.hovered
                text: control.playbackStatusToolTipText()
            }
        }

        // The strategy explanation (playbackModeDetail) and the source-rate note moved into the
        // status tooltip (playbackStatusToolTipText). The item stays as a hidden anchor so its
        // objectName and text stay reachable for the contract suite's font assertions, but it
        // no longer spends a layout line in either state.
        Text {
            id: statusHint

            objectName: "playbackStatusHint"
            visible: false
            height: 0
            text: control.playbackModeDetail + (control.sourceRateText.length > 0 ? " · " + control.sourceRateText : "")
            // Live readouts, not a disabled state: mutedText keeps ~4.5:1 on the panel glass,
            // while disabledText is reserved for controls that cannot be used.
            color: Theme.mutedText
            font.pixelSize: Theme.fontSecondary
            elide: Text.ElideRight
            anchors {
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
                top: statusText.bottom
                topMargin: 1
            }
        }

        // V-07 metric timeline lane. Optional (metrics may be null in standalone tests); when
        // present it sits between the status rail and the slider. The control's height is
        // content-driven via tracks.y, so the lane automatically grows the OSC panel.
        MetricTimelineLane {
            id: metricLane

            metrics: control.metrics
            totalFrames: control.totalFrames
            progress: control.progress
            // Overrides the component's own visible binding: the compact tier keeps the lane
            // (curve and header) off the stack entirely, per metricsLaneVisible above.
            visible: control.metricsLaneVisible
            anchors {
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
                top: statusText.visible ? statusText.bottom : readout.bottom
                topMargin: Theme.spaceMicro
            }
            onSeekRequested: frame => control.seekRequested(frame)
        }

        // Vertical bus for the timeline's top edge. The timeline anchors to this single static
        // line: branching the three sources straight onto its top anchor made Qt's anchor
        // watcher report possible vertical anchor loops once the compact overlay joined the
        // chain (S1 gate, 4x per run). Plain-number bindings on a zero-height item keep the
        // identical layout while the anchor system sees one unambiguous dependency.
        Item {
            id: timelineTopBus

            anchors.left: parent.left
            anchors.right: parent.right
            y: metricLane.visible ? metricLane.y + metricLane.height : (statusText.visible ? statusText.y + statusText.height : (control.readoutOverlay ? 0 : readout.y + readout.height))
            height: 0
        }

        TimelineTracks {
            id: tracks

            markers: control.markers
            totalFrames: control.totalFrames
            progress: control.progress
            enabled: control.timelineEnabled && control.controlsEnabled
            inFrame: control.inFrame
            outFrame: control.outFrame
            anchors {
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
                top: timelineTopBus.top
                // Six pixels: the time axis is the object the whole panel is read against, so it
                // gets one step more air than the status block above it. The compact tier spends
                // the micro step instead - those pixels are the canvas the tier recovers.
                topMargin: control.compactChrome ? Theme.spaceMicro : 6
            }
            onPreviewRequested: frame => control.previewRequested(frame)
            onSeekRequested: frame => control.seekRequested(frame)
        }

        TimelineThumbnailPopup {
            id: thumbnailPopup

            objectName: "timelineThumbnailPopup"
            visible: tracks.hoverFrame >= 0 && control.totalFrames > 0 && (control.previewThumbnailSource.toString().length > 0 || control.previewTimecode.length > 0)
            previewFrame: Math.max(0, control.previewFrame)
            previewTimecode: control.previewTimecode
            thumbnailSource: control.previewThumbnailSource
            isApproximate: control.previewIsApproximate
            sampleFrame: control.previewSampleFrame
            comparisonState: control.markerLabelForFrame(tracks.hoverFrame)
            x: Math.max(8, Math.min(control.width - width - 8, tracks.x + tracks.positionForFrame(tracks.hoverFrame) * tracks.width - width / 2))
            y: -height - 6
            z: 20
        }

        TransportBar {
            // Local id `transport` so the control's `height: transport.bottom + 5` always resolves to
            // THIS TransportBar, independent of the id the PlayerOsc instance is given at its
            // instantiation site (e.g. `id: osc` in the unit test vs `id: transport` in Main.qml).
            id: transport
            objectName: "transportBar"
            compact: true
            compactChrome: control.compactChrome
            // Anchored to the tracks (the content above it), NOT the panel bottom — anchoring to the
            // panel bottom would form a bottom-to-bottom binding loop with the control's height. The
            // 13 px top margin clears the playhead thumb, which extends ~6 px below the tracks box.
            // All transport rows share the panel width and centre, including the permanent
            // range row and the optional adjacent-frame strip.
            anchors {
                top: tracks.bottom
                topMargin: 13
                left: parent.left
                right: parent.right
            }
            canFirst: control.canFirst
            canPrevious: control.canPrevious
            canPlay: control.canPlay
            canPause: control.canPause
            canNext: control.canNext
            canLast: control.canLast
            playing: control.playing
            playbackRate: control.playbackRate
            jogRate: control.jogRate
            shortcutPreset: control.shortcutPreset
            playbackContinuityPolicy: control.playbackContinuityPolicy
            currentFrame: control.currentFrame
            totalFrames: control.totalFrames
            focusTarget: control.focusTarget
            rangeControlsVisible: true
            canMarkRange: control.canMarkRange
            rangeInFrame: control.inFrame
            rangeOutFrame: control.outFrame
            rangeLoopActive: control.loopRangeActive
            rangePingPongActive: control.loopRangePingPongActive
            rangeExportVisible: control.rangeExportVisible
            rangeExportEnabled: control.rangeExportEnabled
            rangeExportBusy: control.rangeExportBusy
            rangeExportProgress: control.rangeExportProgress
            protocol: control.protocol
            sampleExport: control.sampleExport
            sampleExportBusy: control.sampleExportBusy
            onExportRangeRequested: control.exportRangeRequested()
            onExportSampleRequested: control.exportSampleRequested()
            onMarkInRequested: control.markInRequested()
            onMarkOutRequested: control.markOutRequested()
            onPlayRangeRequested: control.playRangeRequested()
            onRangeLoopRequested: control.rangeLoopRequested()
            onRangePingPongRequested: control.rangePingPongRequested()
            onClearRangeRequested: control.clearRangeRequested()
            onProtocolLoadRequested: control.protocolLoadRequested()
            onProtocolClearRequested: control.protocolClearRequested()
            onPreviousGeneratedFrameRequested: control.previousGeneratedFrameRequested()
            onNextGeneratedFrameRequested: control.nextGeneratedFrameRequested()
            onFirstRequested: control.actions.firstFrame()
            // I8: the player preset turns the second-step buttons into the same ±30 s jump
            // their Ctrl+←/→ shortcuts perform; the frame-review preset keeps the exact
            // second. The typeof guard keeps minimal standalone harnesses working, like every
            // other optional action below.
            onPreviousSecondRequested: {
                if (control.shortcutPreset === 1 && control.actions && typeof control.actions.stepSeconds === "function")
                    control.actions.stepSeconds(-30);
                else
                    control.actions.stepBackwardSecond();
            }
            onPreviousFiveRequested: control.actions.stepBackwardFive()
            // E04: hold-to-jog on the single-frame buttons — pressed starts the walk (tap step
            // plus hold window), release stops feeding. Clicks still step exactly one frame.
            // typeof guards keep standalone harnesses with minimal action mocks working.
            onPreviousPressed: {
                if (control.actions && typeof control.actions.jogPress === "function")
                    control.actions.jogPress(-1);
            }
            onNextPressed: {
                if (control.actions && typeof control.actions.jogPress === "function")
                    control.actions.jogPress(1);
            }
            onNavigationReleased: {
                if (control.actions && typeof control.actions.jogRelease === "function")
                    control.actions.jogRelease();
            }
            onPreviousRequested: control.actions.previousFrame()
            onPlaybackRequested: control.actions.togglePlayback()
            onPlaybackRateRequested: rate => {
                if (control.actions && typeof control.actions.setPlaybackRate === "function")
                    control.actions.setPlaybackRate(rate);
            }
            onJogRateRequested: rate => {
                if (control.actions && typeof control.actions.setJogRate === "function")
                    control.actions.setJogRate(rate);
            }
            onContinuityPolicyRequested: policy => {
                if (control.actions && typeof control.actions.setPlaybackContinuityPolicy === "function")
                    control.actions.setPlaybackContinuityPolicy(policy);
            }
            onStepFramesRequested: delta => {
                if (control.actions && typeof control.actions.stepFrames === "function")
                    control.actions.stepFrames(delta);
            }
            onNextRequested: control.actions.nextFrame()
            onNextFiveRequested: control.actions.stepForwardFive()
            onNextSecondRequested: {
                if (control.shortcutPreset === 1 && control.actions && typeof control.actions.stepSeconds === "function")
                    control.actions.stepSeconds(30);
                else
                    control.actions.stepForwardSecond();
            }
            onLastRequested: control.actions.lastFrame()
        }
    }

    Item {
        id: wakeArea

        objectName: "oscWakeArea"
        visible: control.controllerState === 1 && !control.controlsEnabled && !control.docked
        height: 10
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        HoverHandler {
            onHoveredChanged: {
                if (hovered) {
                    control.revealActive = true;
                    hideTimer.stop();
                }
            }
        }
    }

    Timer {
        id: hideTimer

        interval: 1200
        repeat: false
        onTriggered: {
            // The countdown may only fire once the cursor is off the controls. A countdown that
            // ignores the cursor hides the bar while the user is looking straight at it.
            if (control.controllerState === 1 && !control.docked)
                control.revealActive = false;
        }
    }
}
