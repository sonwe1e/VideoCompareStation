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
        font.pixelSize: 11
        font.weight: Font.DemiBold
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
            id: laneToggle

            objectName: "metricLaneToggle"
            text: lane.metrics && lane.metrics.laneEnabled ? qsTr("收起 ▴") : qsTr("展开 ▾")
            // U10: the toggle is an interactive control, so the pointer has to get the same
            // answer a hovered button gives - the readable accent for text on dark panels.
            color: toggleHover.hovered ? Theme.accentText : Theme.accent
            font.pixelSize: 11
            font.weight: Font.DemiBold
            // U10: collapsed, the lane keeps only this header row, so the toggle has to stay
            // keyboard reachable and show the same focus ring as ReviewActionButton.
            activeFocusOnTab: true

            MouseArea {
                id: toggleArea

                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (lane.metrics !== null)
                        lane.metrics.laneEnabled = !lane.metrics.laneEnabled;
                }
            }

            HoverHandler {
                id: toggleHover
            }

            // A Text has no border of its own, so the ring rides on a transparent child hugging
            // the label. Text is not an AbstractButton, so visualFocus is unavailable here and
            // activeFocus is the correct trigger; the ring colour matches ReviewActionButton.
            Rectangle {
                anchors.fill: parent
                anchors.margins: -3
                color: "transparent"
                radius: 3
                border.width: laneToggle.activeFocus ? 2 : 0
                border.color: Theme.focus
            }

            Keys.onPressed: event => {
                if (event.key !== Qt.Key_Return && event.key !== Qt.Key_Enter && event.key !== Qt.Key_Space)
                    return;
                if (lane.metrics !== null)
                    lane.metrics.laneEnabled = !lane.metrics.laneEnabled;
                event.accepted = true;
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
            color: Theme.canvas
            radius: 6
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
                // A data series, not a failure: red is reserved for real errors, so the MAE
                // curve wears the readable accent the theme designates for dark panels.
                context.strokeStyle = Theme.accentText;
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
                // Same data colour as the curve so a peak marker reads as "this point of the
                // series", with the accent edge keeping it visible over the glass.
                color: Theme.accentText
                border.color: Theme.accent
                border.width: 1

                HoverHandler {
                    id: peakHover
                }

                VcsToolTip {
                    visible: peakHover.hovered
                    text: qsTr("第 %1 帧 · MAE %2").arg(Number(peakMarker.modelData.frame) + 1).arg(Number(peakMarker.modelData.mae).toFixed(2))
                }

                MouseArea {
                    objectName: "peakMarkerHitArea"
                    // U10: the dot stays 6x6 - a data marker on a 22px curve - but the pointer
                    // target grows to the theme's hit-area minimum, centred on the dot and
                    // clamped inside the curve area so a marker at either end cannot reach past
                    // it. curveArea clips, so nothing outside it can be clicked through this.
                    // The raise matters: the curve-area MouseArea is declared after this
                    // Repeater, so without it the whole-lane handler sits on top and swallows
                    // every dot click before the enlarged target can ever be used.
                    z: 1
                    width: Math.max(parent.width, Theme.hitAreaMinimum)
                    height: Math.max(parent.height, Theme.hitAreaMinimum)
                    x: Math.max(0, Math.min(curveArea.width - width, peakMarker.x + peakMarker.width / 2 - width / 2))
                    y: Math.max(0, peakMarker.y + peakMarker.height / 2 - height / 2)
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
