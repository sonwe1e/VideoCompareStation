pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls

Item {
    id: control

    property bool canFirst: false
    property bool canPrevious: false
    property bool canPlay: false
    property bool canPause: false
    property bool canNext: false
    property bool canLast: false
    property bool playing: false
    property real playbackRate: 1
    property Item focusTarget: null
    property bool compact: false
    property int playbackContinuityPolicy: 1
    property int currentFrame: -1
    property int totalFrames: 0
    // In/out range state. The endpoints live on the transport (not only in the inspector) because
    // marking a range and replaying it is part of frame-by-frame review, not a session setting.
    // `rangeControlsVisible` stays opt-in so hosts that do not own range plumbing (the docked
    // TimelineBar) keep rendering the bar unchanged.
    property bool rangeControlsVisible: false
    property bool canMarkRange: false
    property int rangeInFrame: -1
    property int rangeOutFrame: -1
    property bool rangeLoopActive: false
    readonly property bool hasRangeEndpoints: control.rangeInFrame >= 0 || control.rangeOutFrame >= 0
    readonly property bool hasCompleteRange: control.rangeInFrame >= 0 && control.rangeOutFrame >= control.rangeInFrame
    // Range clip export. Opt-in like the range row itself so hosts without an export adapter keep
    // the chip hidden. `rangeExportBusy` deliberately does not disable the chip: a click during a
    // running export reopens the dialog so progress and the cancel action stay reachable.
    property bool rangeExportVisible: false
    property bool rangeExportEnabled: false
    property bool rangeExportBusy: false
    property real rangeExportProgress: 0

    signal firstRequested
    signal previousSecondRequested
    signal previousFiveRequested
    signal previousRequested
    signal playbackRequested
    signal nextRequested
    signal nextFiveRequested
    signal nextSecondRequested
    signal lastRequested
    signal playbackRateRequested(real rate)
    signal continuityPolicyRequested(int policyCode)
    signal stepFramesRequested(int delta)
    signal markInRequested
    signal markOutRequested
    signal playRangeRequested
    signal rangeLoopRequested
    signal clearRangeRequested
    signal exportRangeRequested

    implicitWidth: transportColumn.implicitWidth
    implicitHeight: transportColumn.implicitHeight

    function restoreFocus() {
        if (focusTarget)
            focusTarget.forceActiveFocus();
    }

    component TransportButton: VcsToolButton {
        id: button

        implicitWidth: control.compact ? 38 : 48
        implicitHeight: control.compact ? 34 : 42
        iconExtent: control.compact ? 20 : 24
        toolTipDelay: 650
    }

    // In/out range chip. Sized from the label text instead of a fixed width so the four short
    // Chinese labels stay narrow. Declares its own `clicked` signal (instead of relying on the
    // MouseArea) so both keyboard handlers and tests can drive a chip without synthesising
    // mouse events.
    component RangeChip: Rectangle {
        id: chip

        property string chipText: ""
        property string chipHelp: ""
        property bool chipEnabled: true
        property bool chipActive: false

        signal clicked

        implicitWidth: chipLabel.implicitWidth + (control.compact ? 16 : 20)
        implicitHeight: control.compact ? 24 : 28
        radius: 4
        color: !chip.chipEnabled ? "#0f172a" : (chip.chipActive ? "#2563eb" : (chipMouse.containsMouse ? "#26364d" : "#1e293b"))
        border.color: !chip.chipEnabled ? "#1e293b" : (chip.chipActive ? "#3b82f6" : "#334155")
        opacity: chip.chipEnabled ? 1.0 : 0.5

        Accessible.role: Accessible.Button
        Accessible.name: chip.chipText
        Accessible.description: chip.chipHelp

        Text {
            id: chipLabel

            anchors.centerIn: parent
            text: chip.chipText
            font.pixelSize: control.compact ? 11 : 12
            font.weight: chip.chipActive ? Font.DemiBold : Font.Normal
            color: !chip.chipEnabled ? "#64748b" : (chip.chipActive ? "#ffffff" : "#94a3b8")
        }

        MouseArea {
            id: chipMouse

            anchors.fill: parent
            enabled: chip.chipEnabled
            hoverEnabled: true
            cursorShape: chip.chipEnabled ? Qt.PointingHandCursor : Qt.ArrowCursor
            onClicked: {
                chip.clicked();
                control.restoreFocus();
            }
        }

        ToolTip.visible: chip.chipHelp.length > 0 && chipMouse.containsMouse
        ToolTip.text: chip.chipHelp
        ToolTip.delay: 650
    }

    Column {
        id: transportColumn

        spacing: 4
        anchors.horizontalCenter: parent.horizontalCenter

        Row {
            id: buttons

            spacing: control.compact ? 4 : 6

            TransportButton {
                id: firstButton

                objectName: "firstButton"
                iconSource: "qrc:/icons/first.svg"
                helpText: qsTr("第一帧\n快捷键：Home")
                enabled: control.canFirst
                onClicked: {
                    control.firstRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                objectName: "previousSecondButton"
                iconSource: "qrc:/icons/previous-second.svg"
                helpText: qsTr("后退 1 秒\n快捷键：Ctrl+← / Ctrl+A")
                enabled: control.canPrevious
                onClicked: {
                    control.previousSecondRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                objectName: "previousFiveButton"
                iconSource: "qrc:/icons/previous-five.svg"
                helpText: qsTr("后退 5 帧\n快捷键：Shift+← / Shift+A")
                enabled: control.canPrevious
                onClicked: {
                    control.previousFiveRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: previousButton

                objectName: "previousButton"
                iconSource: "qrc:/icons/previous.svg"
                helpText: qsTr("上一帧\n快捷键：← / A")
                enabled: control.canPrevious
                onClicked: {
                    control.previousRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: playbackButton

                objectName: "playbackButton"
                implicitWidth: control.compact ? 42 : 54
                iconSource: control.playing ? "qrc:/icons/pause.svg" : "qrc:/icons/play.svg"
                helpText: control.playing ? qsTr("暂停\n快捷键：空格") : qsTr("播放\n快捷键：空格")
                enabled: control.playing ? control.canPause : control.canPlay
                onClicked: {
                    control.playbackRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: nextButton

                objectName: "nextButton"
                iconSource: "qrc:/icons/next.svg"
                helpText: qsTr("下一帧\n快捷键：→ / D")
                enabled: control.canNext
                onClicked: {
                    control.nextRequested();
                    control.restoreFocus();
                }
            }
            ToolbarCombo {
                id: playbackRateCombo

                objectName: "playbackRateCombo"
                implicitWidth: control.compact ? 58 : 68
                model: [qsTr("0.25×"), qsTr("0.5×"), qsTr("1×"), qsTr("1.5×"), qsTr("2×"), qsTr("4×")]
                currentIndex: {
                    const rate = control.playbackRate;
                    if (rate < 0.375)
                        return 0;
                    if (rate < 0.75)
                        return 1;
                    if (rate < 1.25)
                        return 2;
                    if (rate < 1.75)
                        return 3;
                    if (rate < 3)
                        return 4;
                    return 5;
                }
                Accessible.name: qsTr("播放速度")
                onActivated: index => {
                    const ladder = [0.25, 0.5, 1, 1.5, 2, 4];
                    control.playbackRateRequested(ladder[index]);
                    control.restoreFocus();
                }
            }
            TransportButton {
                objectName: "nextFiveButton"
                iconSource: "qrc:/icons/next-five.svg"
                helpText: qsTr("前进 5 帧\n快捷键：Shift+→ / Shift+D")
                enabled: control.canNext
                onClicked: {
                    control.nextFiveRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                objectName: "nextSecondButton"
                iconSource: "qrc:/icons/next-second.svg"
                helpText: qsTr("前进 1 秒\n快捷键：Ctrl+→ / Ctrl+D")
                enabled: control.canNext
                onClicked: {
                    control.nextSecondRequested();
                    control.restoreFocus();
                }
            }
            TransportButton {
                id: lastButton

                objectName: "lastButton"
                iconSource: "qrc:/icons/last.svg"
                helpText: qsTr("最后一帧\n快捷键：End")
                enabled: control.canLast
                onClicked: {
                    control.lastRequested();
                    control.restoreFocus();
                }
            }

            Row {
                id: continuityGroup

                objectName: "continuityPolicyToggleGroup"
                spacing: 2
                anchors.verticalCenter: parent.verticalCenter

                Rectangle {
                    id: realTimeBtn

                    objectName: "realTimeContinuityButton"
                    implicitWidth: control.compact ? 60 : 70
                    implicitHeight: control.compact ? 28 : 34
                    radius: 4
                    color: control.playbackContinuityPolicy === 1 ? "#2563eb" : "#1e293b"
                    border.color: control.playbackContinuityPolicy === 1 ? "#3b82f6" : "#334155"

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("流畅观看")
                        font.pixelSize: control.compact ? 11 : 12
                        font.weight: control.playbackContinuityPolicy === 1 ? Font.DemiBold : Font.Normal
                        color: control.playbackContinuityPolicy === 1 ? "#ffffff" : "#94a3b8"
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            control.continuityPolicyRequested(1);
                            control.restoreFocus();
                        }
                    }
                }

                Rectangle {
                    id: reviewEveryFrameBtn

                    objectName: "reviewEveryFrameContinuityButton"
                    implicitWidth: control.compact ? 60 : 70
                    implicitHeight: control.compact ? 28 : 34
                    radius: 4
                    color: control.playbackContinuityPolicy === 0 ? "#2563eb" : "#1e293b"
                    border.color: control.playbackContinuityPolicy === 0 ? "#3b82f6" : "#334155"

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("逐帧检查")
                        font.pixelSize: control.compact ? 11 : 12
                        font.weight: control.playbackContinuityPolicy === 0 ? Font.DemiBold : Font.Normal
                        color: control.playbackContinuityPolicy === 0 ? "#ffffff" : "#94a3b8"
                    }

                    MouseArea {
                        anchors.fill: parent
                        cursorShape: Qt.PointingHandCursor
                        onClicked: {
                            control.continuityPolicyRequested(0);
                            control.restoreFocus();
                        }
                    }
                }
            }
        }

        // In/out range controls. Given their own row under the playback buttons: five extra chips
        // inside the button row would push the cluster past the 960 px minimum window width, and
        // the endpoints read better grouped with the range label than interleaved with stepping.
        Row {
            id: rangeRow

            objectName: "transportRangeRow"
            visible: control.rangeControlsVisible
            spacing: control.compact ? 4 : 6
            anchors.horizontalCenter: parent.horizontalCenter

            RangeChip {
                objectName: "transportMarkInButton"
                chipText: qsTr("入点")
                chipHelp: qsTr("把当前帧设为入点\n快捷键：I")
                chipEnabled: control.canMarkRange
                onClicked: control.markInRequested()
            }

            RangeChip {
                objectName: "transportMarkOutButton"
                chipText: qsTr("出点")
                chipHelp: qsTr("把当前帧设为出点\n快捷键：O")
                chipEnabled: control.canMarkRange
                onClicked: control.markOutRequested()
            }

            RangeChip {
                objectName: "transportPlayRangeButton"
                chipText: qsTr("播放区间")
                chipHelp: qsTr("播放入点到出点\n快捷键：\\")
                chipEnabled: control.hasCompleteRange
                onClicked: control.playRangeRequested()
            }

            RangeChip {
                objectName: "transportLoopRangeButton"
                chipText: qsTr("循环")
                chipHelp: control.rangeLoopActive ? qsTr("停止循环，改为单次播放区间") : qsTr("循环播放入点到出点")
                chipEnabled: control.hasCompleteRange
                chipActive: control.rangeLoopActive
                onClicked: control.rangeLoopRequested()
            }

            RangeChip {
                objectName: "transportClearRangeButton"
                chipText: qsTr("清除")
                chipHelp: qsTr("清除入点与出点")
                chipEnabled: control.hasRangeEndpoints
                onClicked: control.clearRangeRequested()
            }

            // Export the marked range as a standalone clip. The work is a lossless stream copy, so
            // the start has to snap back to the preceding keyframe; the tooltip says so rather than
            // letting the user discover it from a clip that starts a little early.
            RangeChip {
                objectName: "transportExportRangeButton"
                visible: control.rangeExportVisible
                chipText: control.rangeExportBusy ? qsTr("导出中 %1%").arg(Math.round(control.rangeExportProgress * 100)) : qsTr("导出")
                chipHelp: qsTr("把入点到出点导出为无损片段（起点对齐到前一个关键帧）")
                chipEnabled: control.rangeExportEnabled || control.rangeExportBusy
                chipActive: control.rangeExportBusy
                onClicked: control.exportRangeRequested()
            }

            // Single source of range truth for the transport cluster: an unset range still gets a
            // one-line hint, so the chips are never a row of unexplained grey labels.
            Text {
                id: rangeLabel

                objectName: "transportRangeLabel"
                text: {
                    if (!control.hasRangeEndpoints)
                        return qsTr("未设区间（I / O 设点）");
                    const inText = control.rangeInFrame >= 0 ? String(control.rangeInFrame + 1) : "—";
                    const outText = control.rangeOutFrame >= 0 ? String(control.rangeOutFrame + 1) : "—";
                    if (!control.hasCompleteRange)
                        return qsTr("入 %1 · 出 %2（区间无效）").arg(inText).arg(outText);
                    const frameCount = control.rangeOutFrame - control.rangeInFrame + 1;
                    return qsTr("入 %1 · 出 %2 · %3 帧%4").arg(inText).arg(outText).arg(frameCount).arg(control.rangeLoopActive ? qsTr(" · 循环") : "");
                }
                color: control.hasCompleteRange ? (control.rangeLoopActive ? "#7dd3fc" : "#9fc3ff") : "#64748b"
                font.family: "Consolas"
                font.pixelSize: control.compact ? 10 : 11
                // Cap instead of overflowing: the row has no clip, and a three-digit frame count
                // still fits, so elision only ever triggers on an unexpectedly narrow window.
                width: Math.min(implicitWidth, control.compact ? 168 : 210)
                elide: Text.ElideRight
                anchors.verticalCenter: parent.verticalCenter
            }
        }

        Row {
            id: adjacentStrip

            objectName: "adjacentFramesStrip"
            visible: !control.playing && control.currentFrame >= 0
            spacing: 4
            anchors.horizontalCenter: parent.horizontalCenter

            Text {
                text: qsTr("相邻帧：")
                font.pixelSize: 10
                color: "#64748b"
                anchors.verticalCenter: parent.verticalCenter
            }

            Repeater {
                model: [-3, -2, -1, 0, 1, 2, 3]

                Rectangle {
                    id: adjacentItem
                    required property int modelData
                    readonly property int delta: adjacentItem.modelData
                    readonly property int targetFrame: control.currentFrame + adjacentItem.delta
                    readonly property bool isCurrent: adjacentItem.delta === 0
                    readonly property bool isValid: adjacentItem.targetFrame >= 0 && (control.totalFrames <= 0 || adjacentItem.targetFrame < control.totalFrames)

                    implicitWidth: adjacentItem.isCurrent ? 58 : 30
                    implicitHeight: 20
                    radius: 3
                    color: adjacentItem.isCurrent ? "#3b82f6" : (adjacentItem.isValid ? "#1e293b" : "#0f172a")
                    border.color: adjacentItem.isCurrent ? "#60a5fa" : (adjacentItem.isValid ? "#334155" : "#1e293b")
                    opacity: adjacentItem.isValid ? 1.0 : 0.35

                    Text {
                        anchors.centerIn: parent
                        text: adjacentItem.isCurrent ? qsTr("当前 %1").arg(adjacentItem.targetFrame + 1) : (adjacentItem.delta > 0 ? "+" + adjacentItem.delta : String(adjacentItem.delta))
                        font.pixelSize: 10
                        font.weight: adjacentItem.isCurrent ? Font.DemiBold : Font.Normal
                        color: adjacentItem.isCurrent ? "#ffffff" : "#94a3b8"
                    }

                    MouseArea {
                        anchors.fill: parent
                        enabled: !adjacentItem.isCurrent && adjacentItem.isValid
                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                        onClicked: {
                            control.stepFramesRequested(adjacentItem.delta);
                            control.restoreFocus();
                        }
                    }
                }
            }
        }
    }
}
