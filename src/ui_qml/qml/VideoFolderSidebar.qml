pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property var folderModel
    property string errorText: ""
    property bool showRecent: false
    readonly property bool compact: height < 460
    readonly property int anchorRow: folderModel ? (folderModel.openPending ? folderModel.pendingRow : folderModel.currentRow) : -1
    signal chooseFolderRequested
    signal fileRequested(int row)
    signal recentFileRequested(int row)
    signal stepRequested(int delta)
    signal closeRequested

    objectName: "videoFolderSidebar"
    color: Theme.panel
    radius: Theme.radiusCard
    border.color: Theme.border
    clip: true

    Column {
        id: header
        width: parent.width - 20
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.top: parent.top
        anchors.topMargin: 10
        spacing: control.compact ? 4 : 8

        Row {
            spacing: 6
            Text {
                width: control.width - 80
                height: control.compact ? 24 : 30
                text: qsTr("文件与历史")
                color: Theme.primaryText
                verticalAlignment: Text.AlignVCenter
                font.bold: true
            }
            VcsToolButton {
                text: "×"
                helpText: qsTr("收起文件与历史侧栏")
                onClicked: control.closeRequested()
            }
        }
        Row {
            spacing: 6
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                objectName: "videoFolderTab"
                text: qsTr("当前文件夹")
                checkable: true
                checked: !control.showRecent
                mirrorsState: true
                onClicked: control.showRecent = false
            }
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                objectName: "videoRecentTab"
                text: qsTr("最近打开")
                checkable: true
                checked: control.showRecent
                mirrorsState: true
                onClicked: control.showRecent = true
            }
        }
        Text {
            width: parent.width
            visible: !control.showRecent
            text: control.folderModel && control.folderModel.folderName.length > 0 ? control.folderModel.folderName : qsTr("请选择本地文件夹")
            color: Theme.mutedText
            elide: Text.ElideMiddle
            ToolTip.visible: folderHover.hovered
            ToolTip.text: control.folderModel ? control.folderModel.folderPath : ""
            HoverHandler {
                id: folderHover
            }
        }
        Row {
            visible: !control.showRecent
            spacing: 6
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                text: qsTr("选择文件夹…")
                onClicked: control.chooseFolderRequested()
            }
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                text: qsTr("刷新")
                enabled: Boolean(control.folderModel && !control.folderModel.scanning && control.folderModel.folderUrl.toString().length > 0)
                onClicked: control.folderModel.refreshFolder()
            }
        }
        Text {
            width: parent.width
            visible: !control.compact
            text: control.showRecent ? qsTr("%1 个视频 · 最近 50 项").arg(control.folderModel ? control.folderModel.recentFiles.length : 0) : (control.folderModel && control.folderModel.scanning ? qsTr("正在读取文件列表…") : qsTr("%1 个视频 · 点击即播放").arg(control.folderModel ? control.folderModel.fileCount : 0))
            color: Theme.mutedText
            font.pixelSize: 12
        }
    }

    ListView {
        id: files
        visible: !control.showRecent
        objectName: "videoFolderFileList"
        property bool blocksGlobalMediaShortcuts: true
        anchors.top: header.bottom
        anchors.topMargin: 10
        anchors.bottom: footer.top
        anchors.bottomMargin: 10
        anchors.left: parent.left
        anchors.right: parent.right
        anchors.margins: 6
        clip: true
        model: control.folderModel
        currentIndex: control.anchorRow
        onHeightChanged: Qt.callLater(function() {
            if (files.currentIndex >= 0)
                files.positionViewAtIndex(files.currentIndex, ListView.Contain);
        })
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: row
            required property int index
            required property string fileName
            required property url fileUrl
            objectName: "videoFolderRow-" + index
            width: files.width
            height: 42
            text: fileName
            highlighted: Boolean(control.folderModel && index === control.folderModel.currentRow)
            enabled: Boolean(control.folderModel && !control.folderModel.scanning)
            background: Rectangle {
                radius: Theme.radiusSmall
                color: row.highlighted ? Theme.controlChecked : (row.hovered ? Theme.controlHover : "transparent")
            }
            ToolTip.visible: hovered
            ToolTip.text: fileUrl.toString()
            onClicked: control.fileRequested(index)
            contentItem: Text {
                text: row.text
                elide: Text.ElideMiddle
                verticalAlignment: Text.AlignVCenter
                color: row.highlighted ? Theme.accentText : Theme.primaryText
                font.pixelSize: 13
            }
        }
        Keys.onReturnPressed: {
            if (currentIndex >= 0)
                control.fileRequested(currentIndex);
        }
        Keys.onEnterPressed: {
            if (currentIndex >= 0)
                control.fileRequested(currentIndex);
        }
        Text {
            visible: Boolean(control.folderModel && !control.folderModel.scanning && control.folderModel.fileCount === 0)
            anchors.centerIn: parent
            text: qsTr("没有可浏览的视频")
            color: Theme.mutedText
            font.pixelSize: 12
        }
    }

    ListView {
        id: recentFiles
        objectName: "videoRecentFileList"
        property bool blocksGlobalMediaShortcuts: true
        visible: control.showRecent
        anchors.fill: files
        clip: true
        model: control.folderModel ? control.folderModel.recentFiles : []
        ScrollBar.vertical: ScrollBar {}
        delegate: ItemDelegate {
            id: recentRow
            required property int index
            required property var modelData
            objectName: "videoRecentRow-" + index
            width: recentFiles.width
            height: 42
            text: modelData.fileName
            highlighted: Boolean(control.folderModel && index === control.folderModel.recentCurrentRow)
            background: Rectangle {
                radius: Theme.radiusSmall
                color: recentRow.highlighted ? Theme.controlChecked : (recentRow.hovered ? Theme.controlHover : "transparent")
            }
            ToolTip.visible: hovered
            ToolTip.text: modelData.fileUrl.toString()
            onClicked: control.recentFileRequested(index)
            contentItem: Text {
                text: recentRow.text
                elide: Text.ElideMiddle
                verticalAlignment: Text.AlignVCenter
                color: recentRow.highlighted ? Theme.accentText : Theme.primaryText
                font.pixelSize: 13
            }
        }
        Keys.onReturnPressed: {
            if (currentIndex >= 0)
                control.recentFileRequested(currentIndex);
        }
        Keys.onEnterPressed: {
            if (currentIndex >= 0)
                control.recentFileRequested(currentIndex);
        }
        Text {
            visible: recentFiles.count === 0
            anchors.centerIn: parent
            text: qsTr("成功打开的视频会显示在这里")
            color: Theme.mutedText
            font.pixelSize: 12
        }
    }

    Column {
        id: footer
        width: parent.width - 20
        anchors.horizontalCenter: parent.horizontalCenter
        anchors.bottom: parent.bottom
        anchors.bottomMargin: 10
        spacing: control.compact ? 4 : 8
        Text {
            width: parent.width
            visible: control.errorText.length > 0
            text: control.errorText
            wrapMode: Text.Wrap
            maximumLineCount: control.compact ? 2 : 4
            elide: Text.ElideRight
            color: Theme.errorText
            font.pixelSize: 12
        }
        Text {
            visible: Boolean(control.folderModel && control.folderModel.openPending)
            text: qsTr("正在打开所选视频…")
            color: Theme.mutedText
            font.pixelSize: 12
        }
        Text {
            width: parent.width
            text: qsTr("点击文件会打开新的单视频任务")
            wrapMode: Text.Wrap
            color: Theme.mutedText
            font.pixelSize: 12
        }
        Row {
            visible: !control.showRecent
            spacing: 6
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                objectName: "videoFolderPrevious"
                text: qsTr("上一项")
                enabled: Boolean(control.folderModel && !control.folderModel.scanning && control.anchorRow > 0)
                onClicked: control.stepRequested(-1)
            }
            ReviewActionButton {
                implicitHeight: control.compact ? 30 : 40
                objectName: "videoFolderNext"
                text: qsTr("下一项")
                enabled: Boolean(control.folderModel && !control.folderModel.scanning && control.folderModel.fileCount > 0 && control.anchorRow < control.folderModel.fileCount - 1)
                onClicked: control.stepRequested(1)
            }
        }
    }
}
