pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

// P06 (D02) nearby-frames diagnostic card: filmstrip showing 5-9 frames around a marked issue,
// provenance badges (retained/generated/missing/unknown), anchor highlight, loop seam indicator,
// and interval playback controls.
Rectangle {
    id: control

    required property int issueIndex
    property var issueData: null
    property var intervalData: null
    property var protocolController: null
    property var reviewController: null
    property int currentFrame: -1

    signal closeRequested
    signal seekRequested(int frame)
    signal loopIntervalRequested(int inFrame, int outFrame)
    signal stepFrameRequested(int delta)

    objectName: "nearbyFramesCard"
    color: Theme.panelElevated
    border.color: Theme.menuBorder
    border.width: 1
    radius: Theme.radiusCard
    implicitWidth: 680
    implicitHeight: 180
    width: implicitWidth
    height: implicitHeight

    readonly property int anchorFrame: intervalData && intervalData.anchorFrame !== undefined ? Number(intervalData.anchorFrame) : -1
    readonly property int inFrame: intervalData && intervalData.inFrame !== undefined ? Number(intervalData.inFrame) : -1
    readonly property int outFrame: intervalData && intervalData.outFrame !== undefined ? Number(intervalData.outFrame) : -1
    readonly property bool loopSeamAtOut: intervalData && intervalData.loopSeamAtOut !== undefined ? Boolean(intervalData.loopSeamAtOut) : false
    readonly property var sessionEpoch: intervalData && intervalData.sessionEpoch !== undefined ? intervalData.sessionEpoch : 0
    readonly property bool epochMatches: !reviewController || sessionEpoch === 0 || Number(reviewController.sessionEpoch) === Number(sessionEpoch)

    readonly property var summariesList: {
        if (inFrame < 0 || outFrame < inFrame)
            return [];

        if (protocolController && Boolean(protocolController.protocolLoaded)) {
            const summaries = protocolController.originSummariesInRange(inFrame, outFrame);
            if (summaries && summaries.length > 0)
                return summaries;
        }

        const fallback = [];
        for (let f = inFrame; f <= outFrame; ++f) {
            fallback.push({
                "frame": f,
                "displayFrame": f + 1,
                "kind": "outside",
                "badgeMarker": ""
            });
        }
        return fallback;
    }

    Column {
        anchors.fill: parent
        anchors.margins: 10
        spacing: 8

        Row {
            width: parent.width
            spacing: 8

            Text {
                text: qsTr("附近帧诊断卡")
                color: Theme.primaryText
                font.pixelSize: 13
                font.weight: Font.DemiBold
                anchors.verticalCenter: parent.verticalCenter
            }

            Text {
                text: control.anchorFrame >= 0 ? qsTr("锚点：第 %1 帧（区间 %2..%3）").arg(control.anchorFrame + 1).arg(control.inFrame + 1).arg(control.outFrame + 1) : ""
                color: Theme.mutedText
                font.pixelSize: 11
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                visible: !control.epochMatches
                height: 20
                width: epochText.implicitWidth + 10
                color: Theme.warningPanel
                border.color: Theme.warningBorder
                border.width: 1
                radius: 3
                anchors.verticalCenter: parent.verticalCenter

                Text {
                    id: epochText

                    anchors.centerIn: parent
                    text: qsTr("跨会话记录")
                    color: Theme.warningText
                    font.pixelSize: 10
                }
            }

            Item {
                height: 1
                width: Math.max(10, parent.width - 450)
            }

            ReviewActionButton {
                objectName: "nearbyFramesCloseButton"
                text: qsTr("收起")
                implicitHeight: 26
                implicitWidth: 60
                anchors.verticalCenter: parent.verticalCenter
                onClicked: control.closeRequested()
            }
        }

        ListView {
            id: filmstripList

            objectName: "nearbyFramesFilmstrip"
            width: parent.width
            height: 80
            orientation: ListView.Horizontal
            spacing: 6
            clip: true
            cacheBuffer: 300
            model: control.summariesList

            delegate: Rectangle {
                id: cellRect

                required property var modelData

                readonly property int cellFrame: modelData && modelData.frame !== undefined ? Number(modelData.frame) : -1
                readonly property int cellDisplayFrame: modelData && modelData.displayFrame !== undefined ? Number(modelData.displayFrame) : (cellFrame + 1)
                readonly property string cellKind: modelData && modelData.kind ? String(modelData.kind) : ""
                readonly property bool isAnchor: cellFrame === control.anchorFrame
                readonly property bool isCurrent: cellFrame === control.currentFrame
                readonly property bool isSeam: cellFrame === control.outFrame && control.loopSeamAtOut

                objectName: cellFrame >= 0 ? ("nearbyFrameCell_" + cellFrame) : ""
                width: 64
                height: 78
                radius: Theme.radiusSmall
                color: isCurrent ? Theme.raisedPanel : Theme.panel
                border.width: isCurrent || isAnchor ? 2 : 1
                border.color: isCurrent ? Theme.accent : (isAnchor ? Theme.focus : Theme.border)

                Column {
                    anchors.fill: parent
                    anchors.margins: 4
                    spacing: 2

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 2

                        Text {
                            text: "#" + cellRect.cellDisplayFrame
                            font.pixelSize: 11
                            font.weight: cellRect.isAnchor ? Font.Bold : Font.Normal
                            color: cellRect.isAnchor ? Theme.accentText : Theme.primaryText
                        }
                    }

                    Rectangle {
                        anchors.horizontalCenter: parent.horizontalCenter
                        width: parent.width - 4
                        height: 20
                        radius: 3
                        color: {
                            if (cellRect.cellKind === "generated")
                                return Theme.controlChecked;
                            if (cellRect.cellKind === "missing")
                                return Theme.errorPanel;
                            if (cellRect.cellKind === "retained")
                                return Theme.control;
                            return "transparent";
                        }
                        border.width: cellRect.cellKind === "missing" ? 1 : 0
                        border.color: cellRect.cellKind === "missing" ? Theme.errorBorder : "transparent"

                        Text {
                            anchors.centerIn: parent
                            font.pixelSize: 10
                            text: {
                                if (cellRect.cellKind === "generated")
                                    return qsTr("生成");
                                if (cellRect.cellKind === "retained")
                                    return qsTr("保留");
                                if (cellRect.cellKind === "missing")
                                    return qsTr("缺帧");
                                if (cellRect.cellKind === "unknown")
                                    return qsTr("未知");
                                if (cellRect.cellKind === "outside")
                                    return qsTr("超界");
                                return "—";
                            }
                            color: {
                                if (cellRect.cellKind === "missing")
                                    return Theme.errorText;
                                if (cellRect.cellKind === "generated")
                                    return Theme.accentText;
                                if (cellRect.cellKind === "retained")
                                    return Theme.secondaryText;
                                return Theme.mutedText;
                            }
                        }
                    }

                    Row {
                        anchors.horizontalCenter: parent.horizontalCenter
                        spacing: 2

                        Rectangle {
                            visible: cellRect.isAnchor
                            width: anchorTag.implicitWidth + 6
                            height: 14
                            radius: 2
                            color: Theme.accentFill

                            Text {
                                id: anchorTag

                                anchors.centerIn: parent
                                text: qsTr("锚点")
                                color: Theme.inverseText
                                font.pixelSize: 9
                            }
                        }

                        Rectangle {
                            id: loopSeamTagRect

                            objectName: "loopSeamTag"
                            visible: cellRect.isSeam
                            width: seamTag.implicitWidth + 6
                            height: 14
                            radius: 2
                            color: Theme.warningPanel
                            border.color: Theme.warningBorder
                            border.width: 1

                            Text {
                                id: seamTag

                                anchors.centerIn: parent
                                text: qsTr("接缝")
                                color: Theme.warningText
                                font.pixelSize: 9
                            }
                        }
                    }
                }

                MouseArea {
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: control.seekRequested(cellRect.cellFrame)
                }
            }
        }

        Row {
            width: parent.width
            spacing: 8

            ReviewActionButton {
                objectName: "nearbyFramesLoopButton"
                text: qsTr("原速循环")
                implicitHeight: 28
                implicitWidth: 84
                enabled: control.inFrame >= 0 && control.outFrame >= control.inFrame
                onClicked: control.loopIntervalRequested(control.inFrame, control.outFrame)
            }

            ReviewActionButton {
                objectName: "nearbyFramesStepPrevButton"
                text: qsTr("上一帧")
                implicitHeight: 28
                implicitWidth: 70
                onClicked: control.stepFrameRequested(-1)
            }

            ReviewActionButton {
                objectName: "nearbyFramesStepNextButton"
                text: qsTr("下一帧")
                implicitHeight: 28
                implicitWidth: 70
                onClicked: control.stepFrameRequested(1)
            }

            ReviewActionButton {
                objectName: "nearbyFramesJumpAnchorButton"
                text: qsTr("跳转锚点")
                implicitHeight: 28
                implicitWidth: 84
                enabled: control.anchorFrame >= 0
                onClicked: control.seekRequested(control.anchorFrame)
            }
        }
    }
}
