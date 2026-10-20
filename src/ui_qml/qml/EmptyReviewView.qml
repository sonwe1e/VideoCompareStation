pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText

    signal openVideoFolderRequested
    signal openFilesRequested
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
                    radius: Theme.radiusSubtle
                    color: Theme.sourceA
                }

                Rectangle {
                    x: 25
                    width: 21
                    height: parent.height
                    radius: Theme.radiusSubtle
                    color: Theme.sourceB
                }

                Rectangle {
                    width: 12
                    height: 12
                    radius: Theme.radiusSubtle
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

                // One merged entry: the file dialog accepts videos and still images in the
                // same selection, so "which kind am I opening" is not a question the card
                // needs to ask up front.
                ReviewActionButton {
                    id: openFilesButton

                    objectName: "emptyOpenFilesButton"
                    text: qsTr("打开文件…")
                    implicitWidth: parent.width
                    prominent: true
                    helpText: qsTr("选择视频或图片；多选两三个视频进入对比，两张图片自动配对。")
                    onClicked: control.openFilesRequested()
                }
            }

            Item {
                width: 1
                height: 6
            }

            // EX-04: the one sentence a first run needs - what the file dialog will do with
            // the count the user is about to pick. A visible caption, not a tooltip: it has
            // to be readable before the first click.
            Text {
                objectName: "emptyCompareHint"

                width: parent.width
                text: qsTr("选 2–3 个视频自动进入对比；选 2 张图片自动配对。")
                color: control.mutedTextColor
                font.pixelSize: Theme.fontCaption
                lineHeight: Theme.lineHeightCompact
                horizontalAlignment: Text.AlignHCenter
                wrapMode: Text.WordWrap
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
                spacing: Theme.spaceMicro

                ReviewActionButton {
                    objectName: "emptyOpenFolderButton"
                    text: qsTr("打开文件夹…")
                    implicitWidth: (parent.width - 8) / 3
                    implicitHeight: 36
                    leftPadding: 6
                    rightPadding: 6
                    flat: true
                    helpText: qsTr("从文件夹中选择视频或图片，支持上一项与下一项。")
                    onClicked: control.openVideoFolderRequested()
                }

                ReviewActionButton {
                    objectName: "emptyOpenImagePairButton"
                    // Same name as the menu item, the dialog title and the help row: one action
                    // called 打开图片对, not two actions that look different (UX-11). The
                    // sentence below still explains the outcome for a first-time user.
                    text: qsTr("打开图片对…")
                    implicitWidth: (parent.width - 8) / 3
                    implicitHeight: 36
                    leftPadding: 6
                    rightPadding: 6
                    flat: true
                    helpText: qsTr("选择两张图片，并排、分割线、单图 A/B 或淡化比较。")
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
