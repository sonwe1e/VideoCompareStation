pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText

    signal openVideoFolderRequested
    signal openVideosRequested
    signal openImageRequested
    signal openImagePairRequested
    signal compareFoldersRequested

    objectName: "emptyReviewView"
    color: Theme.canvas

    // One quiet drop card: the mark and headline say what the window is for, the buttons are
    // the entry points, and the silent-playback note is a footnote rather than a callout.
    Rectangle {
        id: card

        objectName: "emptyReviewCard"
        width: Math.min(parent.width - 48, 600)
        height: content.implicitHeight + 72
        radius: Theme.radiusCard
        color: Theme.panel
        border.width: 0
        anchors.centerIn: parent

        Column {
            id: content

            width: parent.width - 56
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
                text: qsTr("看清每一处细节")
                color: control.textColor
                font.pixelSize: 28
                font.weight: Font.DemiBold
                horizontalAlignment: Text.AlignHCenter
            }

            Item {
                width: 1
                height: 8
            }

            Text {
                width: parent.width
                text: qsTr("拖入视频或图片，开始查看与比较。")
                color: control.mutedTextColor
                font.pixelSize: Theme.fontBody
                lineHeight: Theme.lineHeightCompact
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
            }

            Item {
                width: 1
                height: 28
            }

            Flow {
                width: Math.min(parent.width, 300)
                spacing: 12
                anchors.horizontalCenter: parent.horizontalCenter

                ReviewActionButton {
                    id: openVideosButton

                    objectName: "emptyOpenVideosButton"
                    text: qsTr("打开视频…")
                    implicitWidth: (parent.width - 12) / 2
                    prominent: true
                    helpText: qsTr("选择一个视频播放，或选择两三个视频进行对比。")
                    onClicked: control.openVideosRequested()
                }

                ReviewActionButton {
                    id: openImageButton

                    objectName: "emptyOpenImageButton"
                    text: qsTr("打开图片…")
                    implicitWidth: (parent.width - 12) / 2
                    helpText: qsTr("查看图片、透明度和像素值。")
                    onClicked: control.openImageRequested()
                }
            }

            Item {
                width: 1
                height: 28
            }

            Rectangle {
                width: parent.width
                height: 1
                color: Theme.border
            }

            Item {
                width: 1
                height: 16
            }

            Flow {
                width: parent.width
                spacing: 4

                ReviewActionButton {
                    objectName: "emptyOpenVideoFolderButton"
                    text: qsTr("浏览视频文件夹…")
                    implicitWidth: (parent.width - 8) / 3
                    implicitHeight: 36
                    leftPadding: 6
                    rightPadding: 6
                    flat: true
                    helpText: qsTr("从文件夹中选择视频，支持上一项与下一项。")
                    onClicked: control.openVideoFolderRequested()
                }

                ReviewActionButton {
                    objectName: "emptyOpenImagePairButton"
                    text: qsTr("对比两张图片…")
                    implicitWidth: (parent.width - 8) / 3
                    implicitHeight: 36
                    leftPadding: 6
                    rightPadding: 6
                    flat: true
                    onClicked: control.openImagePairRequested()
                }

                ReviewActionButton {
                    id: compareFoldersButton

                    objectName: "emptyCompareFoldersButton"
                    text: qsTr("对比文件夹…")
                    implicitWidth: (parent.width - 8) / 3
                    implicitHeight: 36
                    leftPadding: 6
                    rightPadding: 6
                    flat: true
                    helpText: qsTr("选择两个图片文件夹，按配对列表逐一比较。")
                    onClicked: control.compareFoldersRequested()
                }
            }

            Item {
                width: 1
                height: 20
            }

            Text {
                width: parent.width
                text: qsTr("专注画面 · 静音播放")
                color: control.mutedTextColor
                font.pixelSize: Theme.fontSecondary
                horizontalAlignment: Text.AlignHCenter
            }
        }
    }
}
