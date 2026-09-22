pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property int previewFrame
    required property string previewTimecode
    property string comparisonState: ""
    property url thumbnailSource: ""
    readonly property bool hasThumbnail: control.thumbnailSource.toString().length > 0

    objectName: "timelineThumbnailPopup"
    width: hasThumbnail ? 184 : Math.max(148, contentCol.implicitWidth + 24)
    height: hasThumbnail ? 112 : (contentCol.implicitHeight + 14)
    radius: hasThumbnail ? 7 : 16
    color: Theme.thumbnailPanel
    border.color: hasThumbnail ? "#50637f" : "#6080b0"

    Rectangle {
        id: imageContainer

        visible: control.hasThumbnail
        width: parent.width - 12
        height: 66
        radius: 4
        color: Theme.canvas
        anchors {
            top: parent.top
            topMargin: 6
            horizontalCenter: parent.horizontalCenter
        }

        Image {
            anchors.fill: parent
            source: control.thumbnailSource
            visible: source.toString().length > 0
            fillMode: Image.PreserveAspectFit
            asynchronous: true
            cache: true
        }
    }

    Column {
        id: contentCol

        spacing: 2
        anchors {
            top: control.hasThumbnail ? imageContainer.bottom : undefined
            topMargin: control.hasThumbnail ? 4 : 0
            verticalCenter: control.hasThumbnail ? undefined : parent.verticalCenter
            left: parent.left
            leftMargin: control.hasThumbnail ? 8 : 12
            right: parent.right
            rightMargin: control.hasThumbnail ? 8 : 12
        }

        Row {
            spacing: 6
            anchors.horizontalCenter: control.hasThumbnail ? undefined : parent.horizontalCenter

            Rectangle {
                width: 6
                height: 6
                radius: 3
                color: Theme.accent
                anchors.verticalCenter: parent.verticalCenter
                visible: !control.hasThumbnail
            }

            Text {
                id: frameTextReadout

                objectName: "previewTimecodeText"
                text: qsTr("%1  ·  第 %2 帧").arg(control.previewTimecode).arg(control.previewFrame + 1)
                color: Theme.primaryText
                font.family: "Consolas"
                font.pixelSize: 11
                font.weight: Font.DemiBold
            }
        }

        Text {
            id: comparisonLabel

            objectName: "previewComparisonStateText"
            text: control.comparisonState
            visible: text.length > 0
            color: "#9fc3ff"
            font.pixelSize: 10
            anchors.horizontalCenter: control.hasThumbnail ? undefined : parent.horizontalCenter
        }
    }
}
