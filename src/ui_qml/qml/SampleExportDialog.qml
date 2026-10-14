pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Dialogs as NativeDialogs
import "VcsTheme.js" as Theme

// E01 training-sample triplet export dialog: input0, GT, input1 and prediction PNG frames
// with manifest.json. Non-blocking export runs on background worker thread.
Item {
    id: control

    objectName: "sampleExportDialog"

    required property var service
    property int targetFrame: -1
    property string targetDirectory: ""
    property string sampleIdText: ""
    property bool cropToRoi: false

    readonly property bool busy: control.service ? Boolean(control.service.busy) : false
    readonly property real progress: control.service ? Math.max(0, Math.min(1, Number(control.service.progress))) : 0
    readonly property int progressPercent: Math.round(control.progress * 100)
    readonly property string lastStatus: control.service ? String(control.service.lastStatus) : ""
    readonly property string lastFailureDetail: control.service ? String(control.service.lastFailureDetail) : ""
    readonly property string lastOutputPath: control.service ? String(control.service.lastOutputPath) : ""

    readonly property var planSummary: {
        if (control.service && control.targetFrame >= 0)
            return control.service.planSummaryAtFrame(control.targetFrame);
        return ({});
    }
    readonly property bool canExport: control.service && control.targetFrame >= 0 ? control.service.canExportAtFrame(control.targetFrame) : false
    readonly property bool dialogVisible: dialog.visible

    function openWithFrame(frame) {
        control.targetFrame = frame;
        if (control.service) {
            control.sampleIdText = control.service.suggestedSampleId(frame);
            if (control.targetDirectory.length === 0)
                control.targetDirectory = control.service.suggestedTargetDirectory();
        }
        dialog.open();
    }

    function open() {
        control.openWithFrame(control.targetFrame);
    }

    function close() {
        dialog.close();
    }

    function startExport() {
        if (!control.service || control.busy)
            return;
        const sId = sampleIdField.text.trim();
        if (sId.length === 0)
            return;
        control.service.exportTriplet(control.targetDirectory, sId, control.cropToRoi, control.targetFrame);
    }

    NativeDialogs.FolderDialog {
        id: folderDialog

        objectName: "sampleExportFolderDialog"
        title: qsTr("选择样本保存目录")
        onAccepted: {
            const picked = selectedFolder && selectedFolder.toString().length > 0 ? selectedFolder : currentFolder;
            if (picked && picked.toString().length > 0)
                control.targetDirectory = picked.toString();
        }
    }

    Popup {
        id: dialog

        objectName: "sampleExportPopup"
        // Keyboard-ownership marker read by Main.qml's focus walk (same contract as VcsMenu):
        // while this modal popup holds focus, workspace shortcuts stay inert instead of acting
        // on the session behind the dialog.
        property bool popupInputContext: true
        parent: Overlay.overlay
        anchors.centerIn: Overlay.overlay
        popupType: Popup.Item
        modal: true
        dim: true
        focus: true
        padding: 0
        width: Math.min(500, (parent ? parent.width : 800) - 48)
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
            implicitWidth: 460
            implicitHeight: shellColumn.implicitHeight

            Column {
                id: shellColumn

                width: parent.width

                // Header
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
                            text: qsTr("导出训练样本 · Training Triplet")
                            color: Theme.primaryText
                            font.pixelSize: 15
                            font.weight: Font.DemiBold
                            anchors.verticalCenter: parent.verticalCenter
                        }

                        Rectangle {
                            anchors.verticalCenter: parent.verticalCenter
                            implicitWidth: formatBadgeText.implicitWidth + 12
                            implicitHeight: 20
                            radius: 4
                            color: Theme.successPanel
                            border.width: 1
                            border.color: Theme.successBorder

                            Text {
                                id: formatBadgeText

                                anchors.centerIn: parent
                                text: qsTr("无损 PNG + manifest.json")
                                font.pixelSize: 10
                                font.weight: Font.DemiBold
                                color: Theme.success
                            }
                        }
                    }
                }

                // Body
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

                        // Triplet summary card
                        Rectangle {
                            width: parent.width
                            implicitHeight: tripletCol.implicitHeight + 16
                            color: Theme.panel
                            radius: 6
                            border.width: 1
                            border.color: Theme.border

                            Column {
                                id: tripletCol

                                width: parent.width - 24
                                anchors.centerIn: parent
                                spacing: 8

                                Row {
                                    anchors.horizontalCenter: parent.horizontalCenter
                                    spacing: 12

                                    Rectangle {
                                        width: 80
                                        height: 44
                                        radius: 4
                                        color: Theme.raisedPanel
                                        border.width: 1
                                        border.color: Theme.border

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 2

                                            Text {
                                                text: qsTr("输入帧 0")
                                                color: Theme.mutedText
                                                font.pixelSize: 10
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }

                                            Text {
                                                text: control.planSummary && control.planSummary.input0Frame !== undefined ? ("#" + (Number(control.planSummary.input0Frame) + 1)) : "—"
                                                color: Theme.primaryText
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }
                                        }
                                    }

                                    Text {
                                        text: "→"
                                        color: Theme.mutedText
                                        font.pixelSize: 14
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Rectangle {
                                        width: 100
                                        height: 44
                                        radius: 4
                                        color: Theme.accentPanel
                                        border.width: 1
                                        border.color: Theme.accentBorder

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 2

                                            Text {
                                                text: qsTr("生成帧 / GT")
                                                color: Theme.accentText
                                                font.pixelSize: 10
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }

                                            Text {
                                                text: {
                                                    if (!control.planSummary || control.planSummary.outputFrame === undefined)
                                                        return "—";
                                                    const f = Number(control.planSummary.outputFrame) + 1;
                                                    const r = control.planSummary.outputRatio !== undefined ? Number(control.planSummary.outputRatio).toFixed(3) : "";
                                                    return r.length > 0 ? ("#" + f + " (" + r + ")") : ("#" + f);
                                                }
                                                color: Theme.accentText
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }
                                        }
                                    }

                                    Text {
                                        text: "→"
                                        color: Theme.mutedText
                                        font.pixelSize: 14
                                        anchors.verticalCenter: parent.verticalCenter
                                    }

                                    Rectangle {
                                        width: 80
                                        height: 44
                                        radius: 4
                                        color: Theme.raisedPanel
                                        border.width: 1
                                        border.color: Theme.border

                                        Column {
                                            anchors.centerIn: parent
                                            spacing: 2

                                            Text {
                                                text: qsTr("输入帧 1")
                                                color: Theme.mutedText
                                                font.pixelSize: 10
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }

                                            Text {
                                                text: control.planSummary && control.planSummary.input1Frame !== undefined ? ("#" + (Number(control.planSummary.input1Frame) + 1)) : "—"
                                                color: Theme.primaryText
                                                font.pixelSize: 12
                                                font.weight: Font.DemiBold
                                                anchors.horizontalCenter: parent.horizontalCenter
                                            }
                                        }
                                    }
                                }

                                Text {
                                    width: parent.width
                                    text: {
                                        const gtText = control.planSummary && control.planSummary.hasGt ? qsTr("包含完整真值 (GT) 与模型推理帧") : qsTr("无真值 (GT) 参考，仅导出候选推理帧");
                                        return gtText;
                                    }
                                    color: Theme.secondaryText
                                    font.pixelSize: 11
                                    horizontalAlignment: Text.AlignHCenter
                                }
                            }
                        }

                        // Sample ID input
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: qsTr("样本标识 (sampleId)：")
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }

                            TextField {
                                id: sampleIdField

                                objectName: "sampleExportIdField"
                                width: parent.width
                                text: control.sampleIdText
                                color: Theme.primaryText
                                font.pixelSize: 12
                                selectByMouse: true
                                background: Rectangle {
                                    radius: Theme.radiusSmall
                                    color: Theme.panel
                                    border.width: 1
                                    border.color: sampleIdField.activeFocus ? Theme.accent : Theme.controlBorder
                                }
                            }

                            Text {
                                text: qsTr("将保存为：<目标目录>/%1/frames/{input0, gt, input1, prediction}.png").arg(sampleIdField.text.length > 0 ? sampleIdField.text : "sample_id")
                                color: Theme.mutedText
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                                width: parent.width
                            }
                        }

                        // Directory Selection
                        Column {
                            width: parent.width
                            spacing: 4

                            Text {
                                text: qsTr("保存目录：")
                                color: Theme.mutedText
                                font.pixelSize: 11
                            }

                            Row {
                                width: parent.width
                                spacing: 8

                                Rectangle {
                                    width: parent.width - 88
                                    height: 30
                                    radius: Theme.radiusSmall
                                    color: Theme.panel
                                    border.width: 1
                                    border.color: Theme.controlBorder
                                    clip: true

                                    Text {
                                        anchors {
                                            left: parent.left
                                            right: parent.right
                                            verticalCenter: parent.verticalCenter
                                            margins: 8
                                        }
                                        text: control.targetDirectory.length > 0 ? control.targetDirectory : qsTr("未选择目录")
                                        color: control.targetDirectory.length > 0 ? Theme.primaryText : Theme.mutedText
                                        font.pixelSize: 11
                                        elide: Text.ElideMiddle
                                    }
                                }

                                ReviewActionButton {
                                    objectName: "sampleExportBrowseButton"
                                    text: qsTr("浏览…")
                                    implicitWidth: 80
                                    implicitHeight: 30
                                    enabled: !control.busy
                                    onClicked: {
                                        if (control.targetDirectory.length > 0)
                                            folderDialog.currentFolder = control.targetDirectory;
                                        folderDialog.open();
                                    }
                                }
                            }
                        }

                        // ROI Crop Option
                        Rectangle {
                            width: parent.width
                            implicitHeight: cropRow.implicitHeight + 12
                            radius: 4
                            color: Theme.panel
                            border.width: 1
                            border.color: Theme.border

                            Row {
                                id: cropRow

                                width: parent.width - 16
                                anchors.centerIn: parent
                                spacing: 8

                                Rectangle {
                                    width: 16
                                    height: 16
                                    radius: 3
                                    color: control.cropToRoi ? Theme.accentFill : Theme.panel
                                    border.width: 1
                                    border.color: control.cropToRoi ? Theme.accent : Theme.controlBorder
                                    anchors.verticalCenter: parent.verticalCenter

                                    Text {
                                        anchors.centerIn: parent
                                        visible: control.cropToRoi
                                        text: "✓"
                                        color: Theme.inverseText
                                        font.pixelSize: 10
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        enabled: control.planSummary && Boolean(control.planSummary.hasCrop)
                                        cursorShape: enabled ? Qt.PointingHandCursor : Qt.ArrowCursor
                                        onClicked: control.cropToRoi = !control.cropToRoi
                                    }
                                }

                                Column {
                                    width: parent.width - 24
                                    anchors.verticalCenter: parent.verticalCenter
                                    spacing: 2

                                    Text {
                                        text: qsTr("裁剪至当前 ROI 区域")
                                        color: (control.planSummary && Boolean(control.planSummary.hasCrop)) ? Theme.primaryText : Theme.mutedText
                                        font.pixelSize: 11
                                        font.weight: Font.DemiBold
                                    }

                                    Text {
                                        text: {
                                            if (control.planSummary && Boolean(control.planSummary.hasCrop))
                                                return qsTr("当前 ROI：%1×%2 (起点 %3, %4)").arg(control.planSummary.cropWidth).arg(control.planSummary.cropHeight).arg(control.planSummary.cropX).arg(control.planSummary.cropY);
                                            return qsTr("当前未激活 ROI 区域，将导出完整源分辨率");
                                        }
                                        color: Theme.mutedText
                                        font.pixelSize: 10
                                    }
                                }
                            }
                        }

                        // Progress and Status
                        Column {
                            width: parent.width
                            visible: control.busy || control.lastStatus.length > 0 || control.lastOutputPath.length > 0 || control.lastFailureDetail.length > 0
                            spacing: 4

                            Row {
                                width: parent.width
                                visible: control.busy
                                spacing: 8

                                Rectangle {
                                    width: parent.width - 50
                                    height: 6
                                    radius: 3
                                    color: Theme.panel
                                    anchors.verticalCenter: parent.verticalCenter

                                    Rectangle {
                                        width: parent.width * control.progress
                                        height: parent.height
                                        radius: 3
                                        color: Theme.accent
                                    }
                                }

                                Text {
                                    text: control.progressPercent + "%"
                                    color: Theme.accentText
                                    font.pixelSize: 11
                                    font.weight: Font.DemiBold
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                            }

                            Text {
                                objectName: "sampleExportStatusText"
                                width: parent.width
                                visible: control.lastStatus.length > 0
                                text: control.lastStatus
                                color: control.lastFailureDetail.length > 0 ? Theme.error : (control.lastOutputPath.length > 0 ? Theme.success : Theme.secondaryText)
                                font.pixelSize: 11
                                wrapMode: Text.WordWrap
                            }

                            Text {
                                objectName: "sampleExportOutputPath"
                                width: parent.width
                                visible: control.lastOutputPath.length > 0
                                text: control.lastOutputPath
                                color: Theme.mutedText
                                font.family: "Consolas"
                                font.pixelSize: 10
                                elide: Text.ElideMiddle
                            }

                            Text {
                                objectName: "sampleExportFailureDetail"
                                width: parent.width
                                visible: control.lastFailureDetail.length > 0
                                text: control.lastFailureDetail
                                color: Theme.warning
                                font.pixelSize: 10
                                wrapMode: Text.WordWrap
                            }
                        }
                    }
                }

                // Footer
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
                            objectName: "sampleExportCancelButton"
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
                            objectName: "sampleExportCloseButton"
                            text: qsTr("关闭")
                            implicitWidth: 84
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: dialog.close()
                        }

                        ReviewActionButton {
                            objectName: "sampleExportStartButton"
                            text: qsTr("开始导出")
                            prominent: true
                            enabled: !control.busy && control.canExport && sampleIdField.text.length > 0 && control.targetDirectory.length > 0
                            implicitWidth: 104
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: control.startExport()
                        }
                    }
                }
            }
        }
    }
}
