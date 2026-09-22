pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Item {
    id: control

    required property int controllerState // 0 pinned, 1 auto, 2 hidden
    // Docked (pinned-below-canvas) vs overlay transport. Bound by Main.qml; defaults to false so
    // the component also instantiates standalone (e.g. in QML unit tests) without binding it.
    property bool docked: false
    property string sourceLabel
    required property bool playing
    property real playbackRate: 1
    // C-07 playback status rail. Bound by Main.qml; defaults keep standalone QML tests working.
    property string playbackModeLabel: ""
    property string playbackModeDetail: ""
    property real playbackTargetRate: 1
    property real playbackPresentationRate: 0
    property int playbackRunSkippedFrameSets: 0
    property int playbackLagMilliseconds: 0
    property bool playbackCatchingUp: false
    property int displayGapCount: 0
    property int sourceDuplicateCount: 0
    property string sourceRateText: ""
    property string statusLegend: qsTr("三类现象相互独立：源重复帧＝文件内容本身；播放器跳过＝为追时间丢掉整组；呈现间隙＝显示管线未跟上刷新。")
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
    property int previewFrame: -1
    property string previewTimecode: "00:00:00:00"
    property url previewThumbnailSource: ""
    property bool revealActive: controllerState === 0
    readonly property bool controlsEnabled: control.docked || control.controllerState === 0 || control.revealActive

    signal previewRequested(int frame)
    signal seekRequested(int frame)
    signal overlayHidden

    objectName: "transport"
    // Height is the content stack, expressed directly so it never depends on an anchored
    // child (anchoring the height to the TransportBar while the TransportBar anchors back to the
    // panel is a binding loop — Qt bails and collapses the whole control to 0, the bug that
    // prompted this fix). The TransportBar therefore anchors to the tracks with a 13 px top margin
    // (which clears the playhead thumb's ~6 px overflow below the 42 px tracks); the control height
    // is tracks.y + tracks height + that margin + TransportBar height (34 px) + a 5 px bottom inset.
    // transportDockHeight (Main.qml) reads this height.
    height: Math.max(0, tracks.y) + tracks.height + 13 + 34 + 5
    visible: controllerState !== 2

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
        hideTimer.restart();
    }

    function markerLabelForFrame(frame) {
        if (frame < 0)
            return "";
        if (control.inFrame >= 0 && frame === control.inFrame)
            return qsTr("入点标记");
        if (control.outFrame >= 0 && frame === control.outFrame)
            return qsTr("出点标记");
        for (let i = 0; i < control.markers.length; ++i) {
            const m = control.markers[i];
            if (Math.abs(Number(m.frame) - frame) <= 1) {
                const kind = String(m.kind);
                if (kind === "anchor")
                    return qsTr("人工锚点");
                if (kind === "duplicate")
                    return qsTr("源重复帧");
                if (kind === "missing")
                    return qsTr("源缺失帧");
                if (kind === "extra")
                    return qsTr("冗余帧");
                return qsTr("分析标记");
            }
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
        }
        opacity: control.controlsEnabled ? 1.0 : 0.0
        enabled: control.controlsEnabled
        color: control.docked ? Theme.panel : Theme.oscPanel
        border.color: control.docked ? Theme.border : "#384860"

        Behavior on opacity {
            NumberAnimation {
                duration: 140
            }
        }

        HoverHandler {
            onHoveredChanged: {
                if (hovered) {
                    control.revealActive = true;
                    hideTimer.stop();
                } else if (control.controllerState === 1) {
                    hideTimer.restart();
                }
            }
        }

        Text {
            id: sourceLabelReadout
            objectName: "sourceLabelReadout"
            visible: control.sourceLabel !== ""
            height: visible ? implicitHeight : 0
            text: control.sourceLabel
            color: Theme.mutedText
            font.family: "Consolas"
            font.pixelSize: 12
            // Elide long filenames instead of overflowing the panel edge (the panel has no clip).
            elide: Text.ElideRight
            anchors {
                top: parent.top
                topMargin: 8
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
            }
        }

        Row {
            id: readout

            spacing: 14
            anchors {
                top: sourceLabelReadout.visible ? sourceLabelReadout.bottom : parent.top
                topMargin: sourceLabelReadout.visible ? 2 : 8
                left: parent.left
                leftMargin: 16
            }

            Text {
                text: control.timecodeText
                color: Theme.primaryText
                font.family: "Consolas"
                font.pixelSize: 13
                font.weight: Font.DemiBold
            }
            Text {
                text: control.currentFrame >= 0 ? qsTr("第 %1 / %2 帧").arg(control.currentFrame + 1).arg(control.totalFrames) : qsTr("无帧")
                color: Theme.mutedText
                font.pixelSize: 12
            }
            Text {
                visible: control.inFrame >= 0 || control.outFrame >= 0
                text: qsTr("入点 %1  出点 %2%3").arg(control.inFrame >= 0 ? control.inFrame + 1 : "—").arg(control.outFrame >= 0 ? control.outFrame + 1 : "—").arg(control.loopRangeActive ? qsTr("  ·  循环") : "")
                color: control.loopRangeActive ? "#7dd3fc" : "#9fc3ff"
                font.pixelSize: 11
            }
        }

        // C-07 playback status rail. Separates source duplicates, player FrameSet skips, and
        // display/present gaps so a stall is never mistaken for algorithm judder.
        Text {
            id: statusText

            objectName: "playbackStatusText"
            visible: control.playbackModeLabel.length > 0
            height: visible ? implicitHeight : 0
            elide: Text.ElideRight
            text: {
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
                    parts.push(qsTr("源重复 %1").arg(control.sourceDuplicateCount));
                if (control.displayGapCount > 0)
                    parts.push(qsTr("呈现间隙 %1").arg(control.displayGapCount));
                return parts.join(" · ");
            }
            color: {
                if (control.playbackCatchingUp || control.playbackLagMilliseconds >= 500 || control.playbackRunSkippedFrameSets > 0)
                    return Theme.warning;
                return Theme.mutedText;
            }
            font.pixelSize: 11
            Accessible.name: text
            Accessible.description: (control.playbackModeDetail.length > 0 ? control.playbackModeDetail + "。" : "") + control.statusLegend
            anchors {
                left: parent.left
                leftMargin: 16
                right: parent.right
                rightMargin: 16
                top: readout.bottom
                topMargin: 2
            }
        }

        Text {
            id: statusHint

            objectName: "playbackStatusHint"
            visible: statusText.visible && control.playbackModeDetail.length > 0
            height: visible ? implicitHeight : 0
            text: control.playbackModeDetail + (control.sourceRateText.length > 0 ? " · " + control.sourceRateText : "")
            color: Theme.disabledText
            font.pixelSize: 10
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
                top: statusHint.visible ? statusHint.bottom : (statusText.visible ? statusText.bottom : readout.bottom)
                topMargin: 2
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
            // Anchored to the tracks (the content above it), NOT the panel bottom — anchoring to the
            // panel bottom would form a bottom-to-bottom binding loop with the control's height. The
            // 13 px top margin clears the playhead thumb, which extends ~6 px below the tracks box.
            anchors {
                top: tracks.bottom
                topMargin: 13
                horizontalCenter: parent.horizontalCenter
            }
            canFirst: control.canFirst
            canPrevious: control.canPrevious
            canPlay: control.canPlay
            canPause: control.canPause
            canNext: control.canNext
            canLast: control.canLast
            playing: control.playing
            playbackRate: control.playbackRate
            focusTarget: control.focusTarget
            onFirstRequested: control.actions.firstFrame()
            onPreviousSecondRequested: control.actions.stepBackwardSecond()
            onPreviousFiveRequested: control.actions.stepBackwardFive()
            onPreviousRequested: control.actions.previousFrame()
            onPlaybackRequested: control.actions.togglePlayback()
            onPlaybackRateRequested: rate => {
                if (control.actions && typeof control.actions.setPlaybackRate === "function")
                    control.actions.setPlaybackRate(rate);
            }
            onNextRequested: control.actions.nextFrame()
            onNextFiveRequested: control.actions.stepForwardFive()
            onNextSecondRequested: control.actions.stepForwardSecond()
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
            if (control.controllerState === 1 && !control.docked)
                control.revealActive = false;
        }
    }
}
