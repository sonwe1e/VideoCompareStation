pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property int previewFrame
    required property string previewTimecode
    property string comparisonState: ""
    property url thumbnailSource: ""

    objectName: "timelineThumbnailPopup"
    width: 184
    height: 112
    radius: 7
    color: Theme.thumbnailPanel
    border.color: "#50637f"

    Rectangle {
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

    Text {
        text: qsTr("%1  ·  第 %2 帧").arg(control.previewTimecode).arg(control.previewFrame + 1)
        color: Theme.primaryText
        font.pixelSize: 11
        anchors {
            left: parent.left
            leftMargin: 8
            bottom: comparisonLabel.top
            bottomMargin: 2
        }
    }

    Text {
        id: comparisonLabel

        text: control.comparisonState
        visible: text.length > 0
        color: "#9fc3ff"
        font.pixelSize: 10
        anchors {
            left: parent.left
            leftMargin: 8
            bottom: parent.bottom
            bottomMargin: 6
        }
    }
}
