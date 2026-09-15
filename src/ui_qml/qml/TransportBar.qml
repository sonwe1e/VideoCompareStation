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
    property Item focusTarget: null
    property bool compact: false

    signal firstRequested
    signal previousSecondRequested
    signal previousFiveRequested
    signal previousRequested
    signal playbackRequested
    signal nextRequested
    signal nextFiveRequested
    signal nextSecondRequested
    signal lastRequested

    implicitWidth: buttons.implicitWidth
    implicitHeight: buttons.implicitHeight

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
    }
}
