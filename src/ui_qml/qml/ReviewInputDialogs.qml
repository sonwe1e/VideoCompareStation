pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Dialogs as NativeDialogs

Item {
    id: control

    required property var stagedVideos
    required property var fileNameFunction
    property var pathNameFunction: null
    required property int initialReferenceIndex
    readonly property bool comparisonVisible: comparisonDialog.visible
    readonly property bool modalVisible: comparisonDialog.visible || videoFilesDialog.visible || addVideoDialog.visible

    signal openVideosAccepted(var urls)
    signal addVideoAccepted(url url)
    signal moveRequested(int fromIndex, int toIndex)
    signal comparisonAccepted(int referenceIndex)
    signal comparisonRejected

    function openVideos() {
        videoFilesDialog.open();
    }

    function openAddVideo() {
        addVideoDialog.open();
    }

    function openComparison() {
        comparisonDialog.open();
    }

    NativeDialogs.FileDialog {
        id: videoFilesDialog

        objectName: "videoFilesDialog"
        title: qsTr("打开 1–3 个视频")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.m4v)"), qsTr("所有文件 (*)")]
        onAccepted: control.openVideosAccepted(selectedFiles)
    }

    NativeDialogs.FileDialog {
        id: addVideoDialog

        objectName: "addSourceFileDialog"
        title: qsTr("添加视频")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.m4v)"), qsTr("所有文件 (*)")]
        onAccepted: control.addVideoAccepted(selectedFile)
    }

    DropConfirmationDialog {
        id: comparisonDialog

        pendingVideos: control.stagedVideos
        fileNameFunction: control.fileNameFunction
        pathNameFunction: control.pathNameFunction
        initialReferenceIndex: control.initialReferenceIndex
        onMoveRequested: (fromIndex, toIndex) => control.moveRequested(fromIndex, toIndex)
        onAccepted: control.comparisonAccepted(referenceIndex)
        onRejected: control.comparisonRejected()
    }
}
