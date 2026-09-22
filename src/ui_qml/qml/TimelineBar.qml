pragma ComponentBehavior: Bound

import QtQuick

Rectangle {
    id: control

    required property var host
    required property var actions
    required property Item focusTarget

    objectName: "transport"
    height: control.host.chromeVisible ? 128 : 0
    visible: control.host.chromeVisible
    color: control.host.panelColor
    border.color: control.host.borderColor
    anchors {
        bottom: parent.bottom
        left: parent.left
        right: parent.right
    }

    Text {
        id: frameCounter

        objectName: "frameCounter"
        text: control.host.frameText
        color: control.host.primaryTextColor
        font.pixelSize: 14
        font.weight: Font.DemiBold
        Accessible.name: text
        anchors {
            top: parent.top
            topMargin: 15
            horizontalCenter: parent.horizontalCenter
        }
    }

    Text {
        objectName: "droppedFramesLabel"
        visible: control.host.droppedFramesText.length > 0 || (control.host.playbackContinuityPolicyName !== undefined && control.host.playbackContinuityPolicyName.length > 0)
        text: {
            const policy = control.host.playbackContinuityPolicyName || "";
            const drops = control.host.droppedFramesText || "";
            const skipped = control.host.playbackSkippedFrameSets > 0 ? qsTr("播放器跳过 %1 组").arg(control.host.playbackSkippedFrameSets) : "";
            const parts = [];
            if (policy.length > 0)
                parts.push(policy);
            if (skipped.length > 0)
                parts.push(skipped);
            if (drops.length > 0)
                parts.push(qsTr("呈现间隙 %1").arg(control.host.droppedFrames));
            return parts.join(" · ");
        }
        color: "#f59e0b"
        font.pixelSize: 12
        anchors {
            left: frameCounter.right
            leftMargin: 12
            baseline: frameCounter.baseline
        }
    }

    Item {
        id: progressTrack

        property bool blocksGlobalMediaShortcuts: true

        objectName: "timelineSlider"
        height: 28
        enabled: control.host.timelineEnabled
        activeFocusOnTab: enabled
        Accessible.role: Accessible.Slider
        Accessible.name: qsTr("帧时间轴")
        Accessible.description: control.host.frameText
        anchors {
            top: frameCounter.bottom
            topMargin: 5
            left: parent.left
            leftMargin: 48
            right: parent.right
            rightMargin: 48
        }

        Rectangle {
            id: timelineRail

            width: parent.width
            height: 4
            radius: 2
            color: control.host.borderColor
            anchors.centerIn: parent
        }

        Rectangle {
            width: timelineRail.width * control.host.timelineProgress
            height: timelineRail.height
            radius: timelineRail.radius
            color: control.host.accentColor
            anchors {
                left: timelineRail.left
                verticalCenter: timelineRail.verticalCenter
            }
        }

        Repeater {
            model: control.host.alignmentTimelineMarkers

            delegate: Rectangle {
                required property var modelData

                width: modelData.kind === "anchor" ? 7 : 4
                height: modelData.kind === "low-confidence" ? 14 : 11
                radius: modelData.kind === "anchor" ? 1 : 2
                x: Math.max(0, Math.min(progressTrack.width - width, Number(modelData.frame) / Math.max(1, Number(control.host.totalFrames) - 1) * progressTrack.width - width / 2))
                color: control.host.alignmentMarkerColor(modelData.kind)
                opacity: modelData.kind === "low-confidence" ? 0.72 : 0.95
                rotation: modelData.kind === "anchor" ? 45 : 0
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Rectangle {
            width: 14
            height: 14
            radius: 7
            x: Math.max(0, Math.min(parent.width - width, control.host.timelineProgress * parent.width - width / 2))
            color: progressTrack.enabled ? control.host.accentColor : "#536176"
            border.width: progressTrack.activeFocus ? 2 : 1
            border.color: progressTrack.activeFocus ? "white" : "#b7d3ff"
            anchors.verticalCenter: parent.verticalCenter
        }

        MouseArea {
            anchors.fill: parent
            enabled: progressTrack.enabled
            cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            preventStealing: true

            onPressed: mouse => {
                progressTrack.forceActiveFocus();
                control.host.timelineDragging = true;
                control.host.timelinePreviewFrame = control.host.frameAtTimelinePosition(mouse.x / width);
            }
            onPositionChanged: mouse => {
                if (pressed)
                    control.host.timelinePreviewFrame = control.host.frameAtTimelinePosition(mouse.x / width);
            }
            onReleased: mouse => {
                control.host.timelinePreviewFrame = control.host.frameAtTimelinePosition(mouse.x / width);
                const target = control.host.timelinePreviewFrame;
                control.host.timelineDragging = false;
                if (control.host.controller) {
                    if (!control.host.chromeVisible)
                        control.host.manualHudPending = true;
                    control.host.controller.seekFrame(target);
                }
            }
            onCanceled: {
                control.host.timelineDragging = false;
                control.host.timelinePreviewFrame = -1;
            }
        }
    }

    TransportBar {
        objectName: "transportBar"
        anchors {
            bottom: parent.bottom
            bottomMargin: 20
            horizontalCenter: parent.horizontalCenter
        }
        canFirst: control.host.canFirstAction
        canPrevious: control.host.canPreviousAction
        canPlay: control.host.canPlayAction
        canPause: control.host.canPauseAction
        canNext: control.host.canNextAction
        canLast: control.host.canLastAction
        playing: control.host.playing
        focusTarget: control.focusTarget
        onFirstRequested: control.actions.firstFrame()
        onPreviousSecondRequested: control.actions.stepBackwardSecond()
        onPreviousFiveRequested: control.actions.stepBackwardFive()
        onPreviousRequested: control.actions.previousFrame()
        onPlaybackRequested: control.actions.togglePlayback()
        onNextRequested: control.actions.nextFrame()
        onNextFiveRequested: control.actions.stepForwardFive()
        onNextSecondRequested: control.actions.stepForwardSecond()
        onLastRequested: control.actions.lastFrame()
    }

    Text {
        visible: control.host.width >= 1260
        text: qsTr("A/D 或 ←/→：±1 帧 · Shift+←/→：±5 帧 · Ctrl+←/→：±1 秒")
        color: control.host.mutedTextColor
        font.pixelSize: 11
        anchors {
            right: parent.right
            rightMargin: 22
            bottom: parent.bottom
            bottomMargin: 13
        }
    }
}
