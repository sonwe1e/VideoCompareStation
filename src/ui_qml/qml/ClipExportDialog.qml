pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import "VcsTheme.js" as Theme

// Lossless range-clip export: destination picker, progress, cancel.
//
// The dialog stays deliberately spare. A stream copy has almost nothing to configure — codec,
// resolution and frame rate all come from the source — so the only real decision is where the
// file goes, and the only thing worth explaining is the keyframe alignment that a lossless cut
// implies. The two caveat lines are the honest version of that, kept visible rather than hidden
// in a tooltip: a clip that starts a little before the in point or runs a few frames past the out
// point looks like a bug until the reason is on screen.
Item {
    id: control

    objectName: "clipExportDialog"

    // Clip-export controller from the composition root. Null in lightweight QML-only harnesses,
    // where the dialog simply never opens.
    required property var service

    // canExport/rangeSummary/suggestedFileName re-read the session snapshot live, but they only
    // NOTIFY on stateChanged — and the range itself lives on the shell, so marking an in point
    // never notifies this dialog. Bumping rangeRevision on every open forces the bindings below
    // to re-read the current range; without that the dialog would repeat the last export's range.
    property int rangeRevision: 0

    readonly property bool busy: control.service ? Boolean(control.service.busy) : false
    readonly property real progress: control.service ? Math.max(0, Math.min(1, Number(control.service.progress))) : 0
    // Mirrors the readout on the transport chip so both entry points agree while the export runs.
    readonly property int progressPercent: Math.round(control.progress * 100)
    readonly property string rangeSummary: {
        control.rangeRevision;
        return control.service ? String(control.service.rangeSummary) : "";
    }
    readonly property string fileName: {
        control.rangeRevision;
        return control.service ? String(control.service.suggestedFileName) : "";
    }
    readonly property string statusText: control.service ? String(control.service.lastStatus) : ""
    readonly property string failureDetail: control.service ? String(control.service.lastFailureDetail) : ""
    readonly property string outputPath: control.service ? String(control.service.lastOutputPath) : ""
    readonly property bool dialogVisible: dialog.visible

    function open() {
        control.rangeRevision += 1;
        dialog.open();
    }

    function close() {
        dialog.close();
    }

    // Read from the controller on every open rather than bound once: the session, and with it the
    // source folder and the range, can change between two exports, and a stale destination is
    // worse than no suggestion at all.
    function suggestedTarget() {
        return control.service ? control.service.suggestedTarget() : "";
    }

    // Opens the native save dialog on the source folder with the suggested clip name.
    function chooseTarget() {
        if (control.busy || !control.service)
            return;

        targetDialog.currentFile = control.suggestedTarget();

        targetDialog.open();
    }

    NativeDialogs.FileDialog {
        id: targetDialog

        objectName: "clipExportTargetDialog"
        title: qsTr("导出区间片段")
        fileMode: NativeDialogs.FileDialog.SaveFile
        nameFilters: [qsTr("视频文件 (*.mp4 *.mov *.mkv)"), qsTr("所有文件 (*)")]
        onAccepted: {
            const picked = selectedFile && selectedFile.toString().length > 0 ? selectedFile : currentFile;

            if (control.service && picked && picked.toString().length > 0)
                control.service.exportRange(picked);
        }
    }

    Popup {
        id: dialog

        objectName: "clipExportPopup"
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        popupType: Popup.Item
        modal: true
        dim: true
        focus: true
        padding: 0
        width: Math.min(460, (parent ? parent.width : 800) - 48)
        // Closing while an export runs is allowed on purpose: the work continues on the worker
        // thread, the transport chip keeps showing progress, and the outcome still lands in the
        // status HUD, so nobody has to babysit a dialog.
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        Overlay.modal: Rectangle {
            color: Theme.modalScrim
        }

        background: Rectangle {
            color: Theme.menu
            radius: 10
            border.width: 1
            border.color: Theme.menuBorder
        }

        contentItem: Item {
            implicitWidth: 412
            implicitHeight: shellColumn.implicitHeight

            Column {
                id: shellColumn

                width: parent.width

                Rectangle {
                    width: parent.width
                    height: 48
                    color: Theme.panel
                    radius: 10

                    Rectangle {
                        height: 10
                        color: parent.color
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                        }
                    }

                    Row {
                        anchors {
                            left: parent.left
                            leftMargin: 20
                            verticalCenter: parent.verticalCenter
                        }
                        spacing: 10

                        Text {
                            text: qsTr("导出区间片段")
                            color: Theme.primaryText
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: streamCopyText.implicitWidth + 12
                            implicitHeight: 20
                            radius: 4
                            color: "#064e3b"
                            border.width: 1
                            border.color: "#059669"

                            Text {
                                id: streamCopyText
                                anchors.centerIn: parent
                                text: qsTr("无损流拷贝 · Stream Copy")
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                color: "#34d399"
                            }
                        }
                    }
                }

                Item {
                    width: parent.width
                    height: bodyColumn.implicitHeight + 28

                    Column {
                        id: bodyColumn

                        width: parent.width - 40
                        anchors {
                            top: parent.top
                            topMargin: 16
                            horizontalCenter: parent.horizontalCenter
                        }
                        spacing: 12

                        // Structured Metadata Card
                        Rectangle {
                            width: parent.width
                            implicitHeight: infoCol.implicitHeight + 18
                            radius: 6
                            color: "#111622"
                            border.width: 1
                            border.color: "#1e293b"

                            Column {
                                id: infoCol
                                width: parent.width - 20
                                anchors.centerIn: parent
                                spacing: 8

                                Row {
                                    spacing: 8
                                    width: parent.width

                                    Text {
                                        text: qsTr("导出区间：")
                                        color: Theme.mutedText
                                        font.pixelSize: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Text {
                                        width: parent.width - 70
                                        text: control.rangeSummary.length > 0 ? control.rangeSummary : qsTr("未设区间")
                                        color: Theme.primaryText
                                        font.pixelSize: 13
                                        font.weight: Font.DemiBold
                                        elide: Text.ElideRight
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }

                                Row {
                                    width: parent.width
                                    spacing: 8

                                    Text {
                                        text: qsTr("目标文件：")
                                        color: Theme.mutedText
                                        font.pixelSize: 12
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Text {
                                        width: parent.width - 70
                                        text: control.fileName.length > 0 ? control.fileName : qsTr("（待选择）")
                                        color: control.fileName.length > 0 ? Theme.primaryText : Theme.disabledText
                                        font.family: "Consolas"
                                        font.pixelSize: 12
                                        elide: Text.ElideMiddle
                                        anchors.verticalCenter: parent.verticalCenter
                                    }
                                }
                            }
                        }

                        // Progress replaces nothing: the range and the file name stay readable while
                        // the export runs, so cancelling is an informed decision rather than a guess.
                        Item {
                            width: parent.width
                            height: 18
                            visible: control.busy

                            Rectangle {
                                id: progressTrack

                                width: parent.width - 48
                                height: 8
                                radius: 4
                                color: Theme.control
                                anchors.verticalCenter: parent.verticalCenter

                                Rectangle {
                                    width: Math.round(progressTrack.width * control.progress)
                                    height: parent.height
                                    radius: 4
                                    color: Theme.accent
                                }
                            }

                            Text {
                                text: control.progressPercent + "%"
                                color: Theme.accent
                                font.pixelSize: 11
                                font.weight: Font.DemiBold
                                anchors {
                                    right: parent.right
                                    verticalCenter: parent.verticalCenter
                                }
                            }
                        }

                        Text {
                            width: parent.width
                            visible: control.statusText.length > 0
                            text: control.statusText
                            color: control.failureDetail.length > 0 ? Theme.error : (control.outputPath.length > 0 ? Theme.success : Theme.mutedText)
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            width: parent.width
                            visible: control.outputPath.length > 0
                            text: control.outputPath
                            color: Theme.mutedText
                            font.family: "Consolas"
                            font.pixelSize: 11
                            elide: Text.ElideMiddle
                        }

                        // The adapter's own words, kept verbatim and only shown when an export
                        // actually failed: a translated summary would hide what FFmpeg rejected.
                        Text {
                            width: parent.width
                            visible: control.failureDetail.length > 0
                            text: control.failureDetail
                            color: Theme.warning
                            font.pixelSize: 11
                            wrapMode: Text.WordWrap
                        }

                        // Keyframe & Stream Copy Information Card
                        Rectangle {
                            width: parent.width
                            implicitHeight: tipCol.implicitHeight + 16
                            radius: 6
                            color: Qt.rgba(30 / 255, 41 / 255, 59 / 255, 0.45)
                            border.width: 1
                            border.color: "#334155"

                            Column {
                                id: tipCol
                                width: parent.width - 20
                                anchors.centerIn: parent
                                spacing: 4

                                Text {
                                    text: qsTr("ℹ️ 无损剪辑特性说明：")
                                    color: "#93c5fd"
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                }

                                Text {
                                    width: parent.width
                                    text: qsTr("• 无损流拷贝保持原始编码、分辨率与帧率。\n• 起点会对齐到前一个关键帧，片段可能比入点稍早开始；B 帧素材的结尾可能多出几帧参考帧。要精确成帧需要重新编码，尚未提供。")
                                    color: "#94a3b8"
                                    font.pixelSize: 11
                                    lineHeight: 1.35
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: footerRow.implicitHeight + 24
                    color: Theme.panel
                    radius: 10

                    Rectangle {
                        height: 10
                        color: parent.color
                        anchors {
                            left: parent.left
                            right: parent.right
                            top: parent.top
                        }
                    }

                    Row {
                        id: footerRow

                        spacing: 8
                        anchors {
                            right: parent.right
                            rightMargin: 12
                            verticalCenter: parent.verticalCenter
                        }

                        ReviewActionButton {
                            objectName: "clipExportCancelButton"
                            visible: control.busy
                            text: qsTr("停止导出")
                            implicitWidth: 96
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: {
                                if (control.service)
                                    control.service.cancelExport();
                            }
                        }

                        ReviewActionButton {
                            objectName: "clipExportCloseButton"
                            text: qsTr("关闭")
                            implicitWidth: 84
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: dialog.close()
                        }

                        ReviewActionButton {
                            objectName: "clipExportChooseButton"
                            text: qsTr("选择位置…")
                            prominent: true
                            enabled: !control.busy && control.service !== null
                            implicitWidth: 112
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: control.chooseTarget()
                        }
                    }
                }
            }
        }
    }
}
