pragma ComponentBehavior: Bound

import QtQuick

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
