pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText

    signal openVideosRequested
    signal openImageRequested
    signal openImagePairRequested
    signal compareFoldersRequested

    objectName: "emptyReviewView"
    color: "transparent"

    Column {
        spacing: 14
        anchors.centerIn: parent

        Text {
            text: qsTr("把视频或图片拖到这里")
            color: control.textColor
            font.pixelSize: 24
            font.weight: Font.DemiBold
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            text: qsTr("打开一个视频即可播放，两三个视频可逐帧对比；图片也可以直接拖进来查看。")
            color: control.mutedTextColor
            font.pixelSize: 13
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Text {
            text: qsTr("仅画面播放 · 不播放声音")
            color: control.mutedTextColor
            font.pixelSize: 12
            anchors.horizontalCenter: parent.horizontalCenter
        }

        Row {
            spacing: 10
            anchors.horizontalCenter: parent.horizontalCenter

            ReviewActionButton {
                id: openVideosButton

                objectName: "emptyOpenVideosButton"
                text: qsTr("打开视频")
                prominent: true
                onClicked: control.openVideosRequested()
            }

            ReviewActionButton {
                id: openImageButton

                objectName: "emptyOpenImageButton"
                text: qsTr("打开图片…")
                onClicked: control.openImageRequested()
            }

            ReviewActionButton {
                objectName: "emptyOpenImagePairButton"
                text: qsTr("对比两张图片…")
                onClicked: control.openImagePairRequested()
            }

            ReviewActionButton {
                id: compareFoldersButton

                objectName: "emptyCompareFoldersButton"
                text: qsTr("对比文件夹…")
                onClicked: control.compareFoldersRequested()
            }
        }
    }
}
