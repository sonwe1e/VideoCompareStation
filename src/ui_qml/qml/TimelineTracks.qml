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
    property real zoomFactor: 1.0
    property real windowStartFrame: 0
    readonly property int visibleFrameCount: Math.max(2, Math.min(totalFrames, Math.ceil(totalFrames / zoomFactor)))
    readonly property real maximumWindowStart: Math.max(0, totalFrames - visibleFrameCount)

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

    function panFrames(delta) {
        windowStartFrame = Math.max(0, Math.min(maximumWindowStart, windowStartFrame + delta));
    }

    Rectangle {
        id: mainRail

        height: 5
        radius: 2.5
        color: Theme.border
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
            bottomMargin: 5
        }
    }

    Rectangle {
        objectName: "rangeHighlight"
        visible: control.inFrame >= 0 && control.outFrame >= control.inFrame && control.totalFrames > 1 && control.outFrame >= control.windowStartFrame && control.inFrame <= control.windowStartFrame + control.visibleFrameCount - 1
        readonly property real visibleIn: Math.max(control.windowStartFrame, Math.min(control.windowStartFrame + control.visibleFrameCount - 1, control.inFrame))
        readonly property real visibleOut: Math.max(control.windowStartFrame, Math.min(control.windowStartFrame + control.visibleFrameCount - 1, control.outFrame))
        x: Math.max(0, control.positionForFrame(visibleIn) * control.width)
        width: visibleOut < visibleIn ? 0 : Math.max(2, (visibleOut - visibleIn) / Math.max(1, control.visibleFrameCount - 1) * control.width)
        height: 9
        radius: 2
        color: "#536dfe"
        opacity: 0.3
        y: mainRail.y - 2
    }

    Repeater {
        model: control.markers
        z: 2

        delegate: Rectangle {
            id: marker

            required property var modelData
            readonly property string kind: String(modelData.kind)
            readonly property real trackPosition: control.positionForFrame(Number(modelData.frame))
            visible: trackPosition >= 0 && trackPosition <= 1
            width: kind === "anchor" ? 6 : 4
            height: 11
            x: Math.max(0, Math.min(control.width - width, trackPosition * control.width - width / 2))
            y: mainRail.y - (height - mainRail.height) / 2
            radius: 2
            color: kind === "missing" ? "#f87171" : (kind === "duplicate" ? "#fb923c" : (kind === "extra" ? "#c084fc" : (kind === "anchor" ? "#22d3ee" : "#facc15")))

            HoverHandler {
                id: markerHover
            }

            VcsToolTip {
                visible: markerHover.hovered
                text: qsTr("源 %1 · %2\n第 %3 帧\n置信度 %4%").arg(String(marker.modelData.source)).arg(marker.kind).arg(Number(marker.modelData.frame) + 1).arg(Number(marker.modelData.confidence))
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
        objectName: "playheadThumb"
        width: 13
        height: 13
        radius: 6.5
        x: Math.max(0, Math.min(control.width - width, control.positionForFrame(Math.round(control.progress * Math.max(0, control.totalFrames - 1))) * control.width - width / 2))
        color: control.enabled ? Theme.accent : Theme.disabledText
        border.color: "#d8e2f2"
        anchors.verticalCenter: mainRail.verticalCenter
    }

    MouseArea {
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
            }
            wheel.accepted = true;
        }
    }

    Text {
        visible: control.zoomFactor > 1
        text: qsTr("%1×").arg(control.zoomFactor.toFixed(1))
        color: Theme.information
        font.pixelSize: 9
        anchors {
            right: parent.right
            top: parent.top
        }
    }
}
