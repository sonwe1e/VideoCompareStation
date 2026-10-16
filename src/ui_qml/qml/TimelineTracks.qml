pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Item {
    id: control

    required property var markers
    required property int totalFrames
    required property real progress
    property int inFrame: -1
    property int outFrame: -1
    property int hoverFrame: -1
    property bool dragging: false
    // The pointer is on the timeline, or is dragging its playhead. The OSC's auto-hide countdown
    // asks for this so a bar under the cursor never fades out; the plain-property bridge exists
    // because the timeline's mouse area fills the whole box (so the rail and the empty space above
    // it are the same answer) and because a drag keeps the grab even when the cursor leaves.
    readonly property bool pointerInside: timelineMouse.containsMouse || control.dragging
    property real zoomFactor: 1.0
    property real windowStartFrame: 0
    readonly property int visibleFrameCount: Math.max(2, Math.min(totalFrames, Math.ceil(totalFrames / zoomFactor)))
    readonly property real maximumWindowStart: Math.max(0, totalFrames - visibleFrameCount)
    // I5 playhead auto-follow safety band. While zoomed in, playback or stepping that carries
    // the current frame past 90% (or before 10%) of the visible window scrolls the window so
    // the playhead re-enters at its centre; a frame inside the band never moves the window.
    readonly property real followLowerBand: 0.1
    readonly property real followUpperBand: 0.9
    readonly property int playheadFrame: Math.round(progress * Math.max(0, totalFrames - 1))

    signal previewRequested(int frame)
    signal seekRequested(int frame)

    objectName: "timelineSlider"
    height: 42
    clip: true
    activeFocusOnTab: enabled
    Accessible.role: Accessible.Slider
    Accessible.name: qsTr("视频时间轴")

    function frameAt(position) {
        if (totalFrames <= 1)
            return 0;
        return Math.round(windowStartFrame + Math.max(0, Math.min(1, position)) * (visibleFrameCount - 1));
    }

    function positionForFrame(frame) {
        if (visibleFrameCount <= 1)
            return 0;
        return (Number(frame) - windowStartFrame) / (visibleFrameCount - 1);
    }

    function setZoom(nextZoom, anchorPosition) {
        const anchorFrame = frameAt(anchorPosition);
        zoomFactor = Math.max(1, Math.min(32, nextZoom));
        const nextVisible = Math.max(2, Math.min(totalFrames, Math.ceil(totalFrames / zoomFactor)));
        windowStartFrame = Math.max(0, Math.min(totalFrames - nextVisible, anchorFrame - anchorPosition * (nextVisible - 1)));
    }

    function resetZoom() {
        zoomFactor = 1.0;
        windowStartFrame = 0;
    }

    function panFrames(delta) {
        windowStartFrame = Math.max(0, Math.min(maximumWindowStart, windowStartFrame + delta));
    }

    // I5: scroll the visible window so the playhead sits at its centre again. Called from the
    // progress change while zoomed in; the guard belongs to the caller, which knows whether a
    // manual drag currently owns the window. Clamped to [0, maximumWindowStart] so the follow
    // itself can never push the window out of range at either edge.
    function followPlayhead() {
        if (zoomFactor <= 1.0 || totalFrames <= 1)
            return false;
        const visible = visibleFrameCount;
        const frame = playheadFrame;
        const lowerEdge = windowStartFrame + (visible - 1) * followLowerBand;
        const upperEdge = windowStartFrame + (visible - 1) * followUpperBand;
        if (frame >= lowerEdge && frame <= upperEdge)
            return false;
        windowStartFrame = Math.max(0, Math.min(maximumWindowStart, Math.round(frame - (visible - 1) / 2)));
        return true;
    }

    onPlayheadFrameChanged: {
        // A live scrub owns the window: the follow must not fight the hand that is dragging
        // the playhead. Zoom changes and wheel pans re-arm it naturally because the next
        // frame change re-runs this handler.
        if (!dragging)
            followPlayhead();
    }
    onTotalFramesChanged: {
        // A new task (or the first frame-count answer) invalidates the window's frame base;
        // restart from the whole timeline rather than keeping a clamped stale window.
        windowStartFrame = 0;
    }

    Rectangle {
        id: mainRail

        height: timelineMouse.containsMouse || control.dragging ? 6 : 4
        radius: height / 2
        color: Theme.timelineRail
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            bottomMargin: 5
        }

        // E4: whole-timeline position indicator. When the window is zoomed in, this thin
        // strip inside the rail shows WHERE the visible window sits in the full timeline -
        // start/middle/end are all distinguishable at any zoom, including 32x. Static by
        // design: zoom/pan and followPlayhead remain the only way the window moves, and this
        // strip adds no second seek controller. A tooltip names the visible frame range in
        // the same 1-based frames the transport uses.
        //
        // Geometry is computed in one direction only: clamped frame endpoints -> raw width ->
        // x (x may read width, width never reads x), so the pair cannot form an evaluation
        // cycle. firstVisibleFrame carries an explicit upper bound (totalFrames - 2) so a
        // stale windowStart from a shrunken session can never display from > to. Degenerate
        // timelines (0 or 1 frame) take the explicit empty branch: unit = 0 and finite x /
        // width, because hidden items still evaluate their bindings and dividing by
        // totalFrames - 1 = 0 would poison them with Infinity/NaN.
        Rectangle {
            id: windowPositionStrip

            objectName: "timelineWindowPositionStrip"
            visible: control.zoomFactor > 1 && control.totalFrames > 1
            // Frame endpoints are clamped strictly inside [0, totalFrames-1] in both
            // directions: first gets the totalFrames-2 upper bound, last gets the
            // first+1 lower bound AND its own totalFrames-1 upper bound, so a degenerate
            // 1-frame timeline resolves to 0/0 instead of last overrunning the last frame.
            readonly property int firstVisibleFrame: Math.max(0, Math.min(control.totalFrames - 2, control.windowStartFrame))
            readonly property int lastVisibleFrame: Math.min(Math.max(firstVisibleFrame + 1, Math.min(control.totalFrames - 1, control.windowStartFrame + control.visibleFrameCount - 1)), control.totalFrames - 1)
            readonly property real unit: control.totalFrames > 1 ? control.width / (control.totalFrames - 1) : 0
            readonly property real rawX: control.totalFrames > 1 ? firstVisibleFrame * unit : 0
            readonly property real rawWidth: control.totalFrames > 1 ? Math.min(control.width, (lastVisibleFrame - firstVisibleFrame) * unit + 2) : 2
            width: Math.max(2, rawWidth)
            x: Math.max(0, Math.min(control.width - width, rawX))
            height: 2
            radius: height / 2
            color: Theme.information
            opacity: 0.9
            anchors.verticalCenter: parent.verticalCenter

            Accessible.role: Accessible.Indicator
            Accessible.name: {
                const from = firstVisibleFrame + 1;
                const to = lastVisibleFrame + 1;
                return from === to ? qsTr("可见窗口：第 %1 帧 / 共 %2 帧").arg(from).arg(control.totalFrames) : qsTr("可见窗口：第 %1 至 %2 帧 / 共 %3 帧").arg(from).arg(to).arg(control.totalFrames);
            }

            MouseArea {
                id: windowStripHover

                anchors.fill: parent
                acceptedButtons: Qt.NoButton
                hoverEnabled: true
            }

            VcsToolTip {
                visible: windowStripHover.containsMouse
                text: {
                    const from = windowPositionStrip.firstVisibleFrame + 1;
                    const to = windowPositionStrip.lastVisibleFrame + 1;
                    return from === to ? qsTr("第 %1 帧").arg(from) : qsTr("第 %1 – %2 帧").arg(from).arg(to);
                }
            }
        }

        Behavior on height {
            NumberAnimation {
                duration: Theme.animationFast
                easing.type: Easing.OutQuad
            }
        }
        Behavior on radius {
            NumberAnimation {
                duration: Theme.animationFast
                easing.type: Easing.OutQuad
            }
        }
    }

    Repeater {
        model: 9

        delegate: Rectangle {
            required property int index
            readonly property real fraction: (index + 1) / 10.0
            width: 1
            height: (index === 4) ? 5 : 3
            color: Theme.disabledText
            opacity: 0.45
            x: Math.round(fraction * control.width)
            anchors.bottom: mainRail.top
            anchors.bottomMargin: 2
        }
    }

    Rectangle {
        id: hoverScrubberGuide

        objectName: "timelineHoverGuide"
        visible: control.hoverFrame >= 0 && control.enabled && control.totalFrames > 0
        width: 1
        height: 15
        color: Theme.accentText
        opacity: 0.85
        z: 3
        x: Math.max(0, Math.min(control.width - 1, control.positionForFrame(control.hoverFrame) * control.width))
        anchors.verticalCenter: mainRail.verticalCenter
    }

    // The range band. An endpoint that is not marked yet leaves the band open to that edge of
    // the window, so a lone in-point already shows where the reviewed range begins. A reversed
    // pair (out before in) hides the band; the bracket handles below still mark both endpoints.
    Rectangle {
        objectName: "rangeHighlight"

        readonly property int windowEndFrame: control.windowStartFrame + control.visibleFrameCount - 1
        readonly property bool pairReversed: control.inFrame >= 0 && control.outFrame >= 0 && control.outFrame < control.inFrame
        readonly property real bandIn: control.inFrame >= 0 ? Math.max(control.windowStartFrame, Math.min(windowEndFrame, control.inFrame)) : control.windowStartFrame
        readonly property real bandOut: control.outFrame >= 0 ? Math.max(control.windowStartFrame, Math.min(windowEndFrame, control.outFrame)) : windowEndFrame
        visible: control.totalFrames > 1 && (control.inFrame >= 0 || control.outFrame >= 0) && !pairReversed && bandOut >= bandIn && (control.inFrame < 0 || control.inFrame <= windowEndFrame) && (control.outFrame < 0 || control.outFrame >= control.windowStartFrame)
        x: Math.max(0, control.positionForFrame(bandIn) * control.width)
        width: bandOut < bandIn ? 0 : Math.max(2, (bandOut - bandIn) / Math.max(1, control.visibleFrameCount - 1) * control.width)
        height: 11
        radius: 2
        color: Theme.rangeBand
        y: mainRail.y - 3
    }

    // Professional In-point bracket handle ([). It renders on its own, not as part of the band,
    // so the in-point stays visible while the out-point is still unset or the pair is reversed.
    Item {
        objectName: "inPointBracket"

        visible: control.inFrame >= 0 && control.totalFrames > 1 && control.inFrame >= control.windowStartFrame && control.inFrame <= control.windowStartFrame + control.visibleFrameCount - 1
        width: 4
        height: 11
        x: Math.min(control.width - width, Math.max(0, control.positionForFrame(control.inFrame) * control.width))
        y: mainRail.y - 3

        Rectangle {
            width: 2
            height: parent.height
            color: Theme.accentText
        }
        Rectangle {
            width: 4
            height: 2
            color: Theme.accentText
        }
        Rectangle {
            width: 4
            height: 2
            anchors.bottom: parent.bottom
            color: Theme.accentText
        }
    }

    // Professional Out-point bracket handle (]). Independent of the band for the same reason.
    Item {
        objectName: "outPointBracket"

        visible: control.outFrame >= 0 && control.totalFrames > 1 && control.outFrame >= control.windowStartFrame && control.outFrame <= control.windowStartFrame + control.visibleFrameCount - 1
        width: 4
        height: 11
        x: Math.max(0, Math.min(control.width - width, control.positionForFrame(control.outFrame) * control.width))
        y: mainRail.y - 3

        Rectangle {
            width: 2
            height: parent.height
            anchors.right: parent.right
            color: Theme.accentText
        }
        Rectangle {
            width: 4
            height: 2
            anchors.right: parent.right
            color: Theme.accentText
        }
        Rectangle {
            width: 4
            height: 2
            anchors.right: parent.right
            anchors.bottom: parent.bottom
            color: Theme.accentText
        }
    }

    Repeater {
        model: control.markers
        z: 2

        delegate: Rectangle {
            id: marker

            objectName: "timelineMarker"

            required property var modelData
            readonly property string kind: String(modelData.kind)
            readonly property real trackPosition: control.positionForFrame(Number(modelData.frame))
            visible: trackPosition >= 0 && trackPosition <= 1
            width: kind === "anchor" ? 6 : 4
            height: 11
            x: Math.max(0, Math.min(control.width - width, trackPosition * control.width - width / 2))
            y: mainRail.y - (height - mainRail.height) / 2
            radius: 2
            color: Theme.timelineMarkerColor(kind)

            HoverHandler {
                id: markerHover
            }

            VcsToolTip {
                visible: markerHover.hovered
                text: qsTr("源 %1 · %2\n第 %3 帧\n置信度 %4%").arg(String(marker.modelData.source)).arg(Theme.timelineMarkerLabel(marker.kind)).arg(Number(marker.modelData.frame) + 1).arg(Number(marker.modelData.confidence))
            }
        }
    }

    Rectangle {
        width: Math.max(0, Math.min(mainRail.width, control.positionForFrame(Math.round(control.progress * Math.max(0, control.totalFrames - 1))) * mainRail.width))
        height: mainRail.height
        radius: mainRail.radius
        color: Theme.accent
        anchors {
            left: mainRail.left
            verticalCenter: mainRail.verticalCenter
        }
    }

    Rectangle {
        // The playhead thumb. Its full box (including the ~6 px that extend below the rail) must
        // clear the transport bar above which TimelineTracks sits — see PlayerOsc's content-driven
        // height, which reserves exactly this much vertical room.
        id: playheadThumbItem
        objectName: "playheadThumb"
        width: 13
        height: 13
        // Geometric half of the 13 px thumb, like VcsScrollBar's width/2 - not a token step.
        radius: 6.5
        x: Math.max(0, Math.min(control.width - width, control.positionForFrame(Math.round(control.progress * Math.max(0, control.totalFrames - 1))) * control.width - width / 2))
        color: control.enabled ? Theme.inverseText : Theme.disabledText
        border.width: 2.5
        border.color: control.enabled ? Theme.accent : Theme.disabledBorder
        anchors.verticalCenter: mainRail.verticalCenter
        scale: control.dragging ? 1.25 : (timelineMouse.containsMouse ? 1.15 : 1.0)

        Behavior on scale {
            NumberAnimation {
                duration: Theme.animationFast
                easing.type: Easing.OutQuad
            }
        }
    }

    MouseArea {
        id: timelineMouse
        anchors.fill: parent
        hoverEnabled: true
        enabled: control.enabled
        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
        preventStealing: true
        onPositionChanged: mouse => {
            control.hoverFrame = control.frameAt(mouse.x / width);
            control.previewRequested(control.hoverFrame);
        }
        onExited: {
            if (!pressed)
                control.hoverFrame = -1;
        }
        onPressed: mouse => {
            control.forceActiveFocus();
            control.dragging = true;
            control.previewRequested(control.frameAt(mouse.x / width));
        }
        onReleased: mouse => {
            const frame = control.frameAt(mouse.x / width);
            control.dragging = false;
            control.seekRequested(frame);
        }
        onCanceled: control.dragging = false
        onWheel: wheel => {
            if ((wheel.modifiers & Qt.ControlModifier) !== 0) {
                control.setZoom(control.zoomFactor * (wheel.angleDelta.y > 0 ? 1.5 : (1 / 1.5)), Math.max(0, Math.min(1, wheel.x / width)));
            } else if (control.zoomFactor > 1) {
                const direction = wheel.angleDelta.y > 0 ? -1 : 1;
                control.panFrames(direction * Math.max(1, Math.round(control.visibleFrameCount / 10)));
            } else if (control.totalFrames > 0) {
                const step = (wheel.modifiers & Qt.ShiftModifier) ? (wheel.angleDelta.y > 0 ? 5 : -5) : (wheel.angleDelta.y > 0 ? 1 : -1);
                const current = Math.round(control.progress * Math.max(0, control.totalFrames - 1));
                const target = Math.max(0, Math.min(control.totalFrames - 1, current + step));
                control.seekRequested(target);
            }
            wheel.accepted = true;
        }
    }

    // I5: the zoom readout is also the one-click way back to the whole timeline. The pill
    // states the factor; clicking (or keyboard activation once focused) resets zoom and
    // window, so leaving a 32x inspection no longer depends on remembering Ctrl+wheel.
    Rectangle {
        id: zoomBadge

        objectName: "timelineZoomBadge"
        visible: control.zoomFactor > 1
        // Reachable by keyboard like the U10 lane toggle: an action-bearing item must not be
        // mouse-only.
        activeFocusOnTab: visible
        radius: Theme.radiusMedium
        color: zoomMouse.containsMouse || zoomBadge.activeFocus ? Theme.oscGlassHover : Theme.oscGlass
        border.width: zoomBadge.activeFocus ? 2 : 1
        border.color: zoomBadge.activeFocus ? Theme.focus : Theme.oscBorder
        width: zoomBadgeLabel.implicitWidth + 18
        height: 20
        // The zoom factor is a must-read value, so it sits at the secondary size, never the
        // decorative fontMicro floor.
        Accessible.role: Accessible.Button
        Accessible.name: qsTr("缩放 %1×，点击重置为完整时间轴").arg(control.zoomFactor.toFixed(1))
        anchors {
            right: parent.right
            top: parent.top
        }

        Text {
            id: zoomBadgeLabel

            anchors.centerIn: parent
            text: qsTr("%1× · 点击重置").arg(control.zoomFactor.toFixed(1))
            color: Theme.information
            font.pixelSize: Theme.fontSecondary
            // The digits tick while the user scrubs the zoom; tabular figures keep the badge
            // from breathing horizontally (E: fontFeaturesNumeric token).
            font.features: Theme.fontFeaturesNumeric
        }

        MouseArea {
            id: zoomMouse

            anchors.fill: parent
            hoverEnabled: true
            cursorShape: Qt.PointingHandCursor
            onClicked: {
                control.resetZoom();
                control.forceActiveFocus();
            }
        }

        Keys.onSpacePressed: event => {
            control.resetZoom();
            event.accepted = true;
        }
        Keys.onReturnPressed: event => {
            control.resetZoom();
            event.accepted = true;
        }
        Keys.onEnterPressed: event => {
            control.resetZoom();
            event.accepted = true;
        }
    }
}
