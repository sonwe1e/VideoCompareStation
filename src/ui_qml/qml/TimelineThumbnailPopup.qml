pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property int previewFrame
    required property string previewTimecode
    property string comparisonState: ""
    property url thumbnailSource: ""
    property bool isApproximate: false
    property int sampleFrame: -1
    readonly property bool hasThumbnail: control.thumbnailSource.toString().length > 0

    objectName: "timelineThumbnailPopup"
    width: hasThumbnail ? 192 : Math.max(152, contentCol.implicitWidth + 24)
    height: hasThumbnail ? (control.isApproximate ? 126 : 114) : (contentCol.implicitHeight + 14)
    radius: hasThumbnail ? 10 : 16
    color: Theme.thumbnailPanel
    border.color: hasThumbnail ? Theme.border : Theme.oscBorder

    Rectangle {
        id: imageContainer

        visible: control.hasThumbnail
        width: parent.width - 12
        height: 68
        radius: 6
        color: Theme.thumbnailWell
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
            color: Theme.accentText
            font.pixelSize: 10
            anchors.horizontalCenter: control.hasThumbnail ? undefined : parent.horizontalCenter
        }

        Text {
            id: approximateBadge

            objectName: "previewApproximateBadge"
            text: qsTr("预览 · 邻近第 %1 帧").arg(control.sampleFrame + 1)
            visible: control.sampleFrame >= 0 && control.sampleFrame !== control.previewFrame
            color: Theme.warning
            font.pixelSize: 10
            anchors.horizontalCenter: control.hasThumbnail ? undefined : parent.horizontalCenter
        }
    }
}
