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
    readonly property bool modalVisible: comparisonDialog.visible || videoFilesDialog.visible || addVideoDialog.visible || replaceVideoDialog.visible || videoFolderDialog.visible || mediaFilesDialog.visible

    signal videoFolderAccepted(url folder)
    signal videoFolderRejected
    signal openVideosAccepted(var urls)
    signal openVideosRejected
    // Merged 打开文件 entry: one multi-select dialog for videos and still images; Main
    // classifies the accepted selection and routes it to the video or image pipeline.
    signal mediaFilesAccepted(var urls)
    signal mediaFilesRejected
    signal addVideoAccepted(url url)
    signal addVideoRejected
    // V02 "替换文件": one file replaces the source whose menu asked for it; Main owns which
    // identity is being replaced and stages the swap.
    signal replaceVideoAccepted(url url)
    signal replaceVideoRejected
    signal moveRequested(int fromIndex, int toIndex)
    signal comparisonAccepted(int referenceIndex)
    signal comparisonClosed
    signal comparisonRejected

    function openVideos() {
        videoFilesDialog.open();
    }

    function openMediaFiles() {
        mediaFilesDialog.open();
    }

    function openVideoFolder() {
        videoFolderDialog.open();
    }

    function openAddVideo() {
        addVideoDialog.open();
    }

    function openReplaceVideo() {
        replaceVideoDialog.open();
    }

    function openComparison() {
        comparisonDialog.open();
    }

    NativeDialogs.FolderDialog {
        id: videoFolderDialog
        objectName: "videoFolderDialog"
        title: qsTr("选择视频文件夹")
        onAccepted: control.videoFolderAccepted(selectedFolder)
        onRejected: control.videoFolderRejected()
    }

    NativeDialogs.FileDialog {
        id: videoFilesDialog

        objectName: "videoFilesDialog"
        title: qsTr("打开 1–3 个视频")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.m4v)"), qsTr("所有文件 (*)")]
        onAccepted: control.openVideosAccepted(selectedFiles)
        onRejected: control.openVideosRejected()
    }

    NativeDialogs.FileDialog {
        id: mediaFilesDialog

        objectName: "mediaFilesDialog"
        title: qsTr("打开文件")
        fileMode: NativeDialogs.FileDialog.OpenFiles
        nameFilters: [qsTr("媒体文件 (*.mp4 *.mkv *.mov *.avi *.m4v *.png *.jpg *.jpeg *.bmp *.gif *.webp *.tif *.tiff *.pnm *.ppm *.pgm *.pbm *.pam)"), qsTr("所有文件 (*)")]
        onAccepted: control.mediaFilesAccepted(selectedFiles)
        onRejected: control.mediaFilesRejected()
    }

    NativeDialogs.FileDialog {
        id: addVideoDialog

        objectName: "addSourceFileDialog"
        title: qsTr("添加视频")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.m4v)"), qsTr("所有文件 (*)")]
        onAccepted: control.addVideoAccepted(selectedFile)
        onRejected: control.addVideoRejected()
    }

    NativeDialogs.FileDialog {
        id: replaceVideoDialog

        objectName: "replaceSourceFileDialog"
        title: qsTr("替换视频文件")
        fileMode: NativeDialogs.FileDialog.OpenFile
        nameFilters: [qsTr("视频文件 (*.mp4 *.mkv *.mov *.avi *.m4v)"), qsTr("所有文件 (*)")]
        onAccepted: control.replaceVideoAccepted(selectedFile)
        onRejected: control.replaceVideoRejected()
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
        onClosed: control.comparisonClosed()
    }
}
