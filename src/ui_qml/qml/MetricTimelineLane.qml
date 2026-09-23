pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// Pair-metrics lane above the timeline slider. The header row is always visible when metrics
// are available; the curve area only when the user expanded the lane (which also widens the
// sampling window on the controller). The whole lane maps the full canonical timeline, so it
// stays readable independent of the slider's zoom window.
Item {
    id: lane

    // PairMetricsController context object; optional so standalone PlayerOsc tests work.
    required property var metrics
    required property int totalFrames
    property real progress: 0
    signal seekRequested(int frame)

    objectName: "metricTimelineLane"
    visible: lane.metrics !== null && lane.metrics.available
    // 16 px header row always; +22 px curve area when expanded. Collapsed keeps the toggle
    // reachable, so the lane can always be brought back.
    height: visible ? (lane.metrics.laneEnabled ? 38 : 16) : 0

    function frameAtPosition(position) {
        if (lane.totalFrames <= 1)
            return 0;
        return Math.max(0, Math.min(lane.totalFrames - 1, Math.round(position * (lane.totalFrames - 1))));
    }

    Text {
        id: laneLabel

        text: qsTr("差异指标 MAE")
        color: Theme.mutedText
        font.pixelSize: 10
        anchors {
            left: parent.left
            verticalCenter: headerRow.verticalCenter
        }
    }

    Row {
        id: headerRow

        spacing: 6
        height: 16
        anchors {
            right: parent.right
        }

        Text {
            objectName: "metricLaneToggle"
            text: lane.metrics && lane.metrics.laneEnabled ? qsTr("收起 ▴") : qsTr("展开 ▾")
            color: Theme.accent
            font.pixelSize: 10

            MouseArea {
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (lane.metrics !== null)
                        lane.metrics.laneEnabled = !lane.metrics.laneEnabled;
                }
            }
        }
    }

    Item {
        id: curveArea

        visible: lane.metrics !== null && lane.metrics.laneEnabled
        height: visible ? 22 : 0
        clip: true
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        Rectangle {
            anchors.fill: parent
            color: "#101722"
            radius: 3
            border.color: Theme.border
            border.width: 1
        }

        Canvas {
            id: curveCanvas

            objectName: "metricLaneCanvas"
            anchors.fill: parent
            anchors.margins: 2

            onPaint: {
                const context = getContext("2d");
                context.reset();
                if (lane.metrics === null || lane.totalFrames <= 1 || lane.metrics.sampleCount <= 0)
                    return;
                const maximum = Math.max(0.0001, lane.metrics.sampleMaxMae);
                const points = lane.metrics.samplePoints(Math.max(8, Math.floor(width / 2)));
                if (points.length === 0)
                    return;
                const frameSpan = Math.max(1, lane.totalFrames - 1);
                context.beginPath();
                let started = false;
                for (let index = 0; index < points.length; ++index) {
                    const sample = points[index];
                    if (!sample.comparable)
                        continue;
                    const x = Number(sample.frame) / frameSpan * width;
                    const y = height - Math.max(0, Math.min(1, Number(sample.mae) / maximum)) * (height - 2) - 1;
                    if (!started) {
                        context.moveTo(x, y);
                        started = true;
                    } else {
                        context.lineTo(x, y);
                    }
                }
                if (!started)
                    return;
                context.strokeStyle = "#f87171";
                context.lineWidth = 1;
                context.stroke();
            }

            Connections {
                function onSamplesChanged() {
                    curveCanvas.requestPaint();
                    lane.refreshPeaks();
                }
                ignoreUnknownSignals: true
                target: lane.metrics
            }
        }

        // Strongest local MAE maxima as clickable markers: the fastest route from "the curve
        // spikes around here" to standing on the worst frame.
        Repeater {
            id: peakRepeater

            model: lane.peakList

            delegate: Rectangle {
                id: peakMarker

                required property var modelData

                readonly property real markerX: Number(peakMarker.modelData.frame) / Math.max(1, lane.totalFrames - 1) * curveArea.width - 3

                width: 6
                height: 6
                radius: 3
                x: Math.max(0, Math.min(curveArea.width - width, markerX))
                y: 1
                color: "#fbbf24"
                border.color: "#78350f"
                border.width: 1

                HoverHandler {
                    id: peakHover
                }

                VcsToolTip {
                    visible: peakHover.hovered
                    text: qsTr("第 %1 帧 · MAE %2").arg(Number(peakMarker.modelData.frame) + 1).arg(Number(peakMarker.modelData.mae).toFixed(2))
                }

                MouseArea {
                    anchors.fill: parent
                    cursorShape: Qt.PointingHandCursor
                    onClicked: lane.seekRequested(Number(peakMarker.modelData.frame))
                }
            }
        }

        MouseArea {
            anchors.fill: parent
            enabled: lane.totalFrames > 1
            cursorShape: Qt.PointingHandCursor
            onClicked: mouse => lane.seekRequested(lane.frameAtPosition(mouse.x / width))
        }

        // Playhead marker so the lane and the slider below always read as one timeline.
        Rectangle {
            objectName: "metricLanePlayhead"
            width: 1
            height: parent.height
            color: Theme.accent
            x: Math.max(0, Math.min(parent.width - 1, lane.progress * parent.width))
        }
    }

    property var peakList: []

    function refreshPeaks() {
        if (lane.metrics === null || !lane.metrics.laneEnabled) {
            lane.peakList = [];
            return;
        }
        const neighborhood = Math.max(2, lane.totalFrames / 100);
        lane.peakList = lane.metrics.peakFrames(4, neighborhood);
    }

    Connections {
        function onLaneEnabledChanged() {
            lane.refreshPeaks();
        }
        function onAvailableChanged() {
            lane.refreshPeaks();
        }
        ignoreUnknownSignals: true
        target: lane.metrics
    }

    Component.onCompleted: lane.refreshPeaks()
    onTotalFramesChanged: lane.refreshPeaks()
}
