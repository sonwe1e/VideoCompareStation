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

    // One quiet drop card: the mark and headline say what the window is for, the buttons are
    // the four ways in, and the silent-playback note is a footnote rather than a callout.
    Rectangle {
        id: card

        width: Math.min(parent.width - 48, 640)
        height: content.implicitHeight + 76
        radius: Theme.radiusCard
        color: Theme.panel
        border.width: 1
        border.color: Theme.border
        anchors.centerIn: parent

        Column {
            id: content

            width: parent.width - 64
            anchors.centerIn: parent

            // The application mark: two sources split by the wipe handle.
            Item {
                width: 46
                height: 32
                anchors.horizontalCenter: parent.horizontalCenter

                Rectangle {
                    width: 21
                    height: parent.height
                    radius: 6
                    color: Theme.sourceA
                }

                Rectangle {
                    x: 25
                    width: 21
                    height: parent.height
                    radius: 6
                    color: Theme.sourceB
                }

                Rectangle {
                    width: 12
                    height: 12
                    radius: 6
                    color: Theme.inverseText
                    border.width: 2
                    border.color: card.color
                    anchors.centerIn: parent
                }
            }

            Item {
                width: 1
                height: 20
            }

            Text {
                width: parent.width
                text: qsTr("把视频或图片拖到这里")
                color: control.textColor
                font.pixelSize: 22
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Item {
                width: 1
                height: 8
            }

            Text {
                width: parent.width
                text: qsTr("打开一个视频即可播放，两三个视频可逐帧对比；图片也可以直接拖进来查看。")
                color: control.mutedTextColor
                font.pixelSize: 13
                lineHeight: 1.25
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item {
                width: 1
                height: 26
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

            Item {
                width: 1
                height: 22
            }

            Text {
                width: parent.width
                text: qsTr("仅画面播放 · 不播放声音")
                color: Theme.disabledText
                font.pixelSize: 12
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
