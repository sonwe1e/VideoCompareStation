pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Windows "默认打开方式" settings: register as a choosable application, jump to the system
// default-apps page, repair the recorded paths after the ZIP moved, and remove this
// application's registration.
//
// Two promises are visible on purpose rather than buried in a tooltip: setting this application
// as the video default does not add sound (the product is a silent visual review tool, and audio
// formats are never registered), and opening the system settings page is not success - the
// read-back row below is what reports who owns the default, so the user can see the actual
// effect of their system choice instead of trusting that a page appeared.
Item {
    id: control

    objectName: "fileAssociationDialog"

    // File-association controller from the composition root. Null in lightweight QML-only
    // harnesses, where the dialog is simply never opened.
    required property var service

    readonly property bool busy: control.service ? Boolean(control.service.busy) : false
    // The popup's visibility, not this Item's: the item is always "visible" as a layout object,
    // so Main.qml's modal gate must read the popup state through this property (the same
    // dialogVisible contract the export dialogs expose).
    readonly property bool dialogVisible: dialog.visible
    readonly property string lastError: control.service ? String(control.service.lastError) : ""
    readonly property string lastResult: control.service ? String(control.service.lastResult) : ""
    // FileAssociationHealth: 0 not registered, 1 current, 2 registered with another path.
    readonly property int registrationHealth: control.service ? Number(control.service.registrationHealth) : 0
    // DefaultAppOwnership: 0 not set, 1 this application, 2 another/partial.
    readonly property int videoDefault: control.service ? Number(control.service.videoDefault) : 0
    readonly property int imageDefault: control.service ? Number(control.service.imageDefault) : 0

    // The service reports failures in English (shell-layer strings); the dialog owns the
    // user-facing wording. Known failure causes map to plain Chinese, anything unexpected is
    // wrapped but kept intact so support can still read the registry detail.
    function localizedError() {
        const raw = control.lastError;
        if (raw.length === 0)
            return "";
        if (raw.indexOf("the registration needs CompareStation.exe") >= 0)
            return qsTr("注册需要正式安装的 CompareStation.exe，当前进程位置无法注册。");
        if (raw.indexOf("the executable does not exist") >= 0)
            return qsTr("注册的程序文件不存在，请重新解压或安装后再试。");
        if (raw.indexOf("(rolled back)") >= 0)
            return qsTr("写入系统注册表失败，已回滚到操作前的状态（%1）。").arg(raw);
        if (raw.indexOf("registry error") >= 0)
            return qsTr("访问系统注册表失败（%1）。").arg(raw);
        return raw;
    }

    function open() {
        dialog.open();
        if (control.service && !control.busy)
            control.service.refreshStatus();
    }

    function close() {
        dialog.close();
    }

    function ownershipLabel(state) {
        if (state === 1)
            return qsTr("本应用");
        if (state === 2)
            return qsTr("其他应用 / 部分");
        return qsTr("未设置");
    }

    function healthLabel() {
        if (control.registrationHealth === 1)
            return qsTr("已注册（当前安装）");
        if (control.registrationHealth === 2)
            return qsTr("已注册（路径已变化，建议修复）");
        return qsTr("未注册");
    }

    Popup {
        id: dialog

        objectName: "fileAssociationPopup"
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
        width: Math.min(520, (parent ? parent.width : 900) - 48)
        closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

        Overlay.modal: Rectangle {
            color: Theme.modalScrim
        }

        background: Rectangle {
            color: Theme.menu
            radius: Theme.radiusMedium
            border.width: 1
            border.color: Theme.menuBorder
        }

        contentItem: Item {
            implicitWidth: 460
            implicitHeight: shellColumn.implicitHeight

            Column {
                id: shellColumn

                width: parent.width

                Rectangle {
                    width: parent.width
                    height: 48
                    color: Theme.panel
                    radius: Theme.radiusMedium

                    Rectangle {
                        height: 10
                        color: parent.color
                        anchors {
                            left: parent.left
                            right: parent.right
                            bottom: parent.bottom
                        }
                    }

                    Text {
                        anchors {
                            left: parent.left
                            leftMargin: 20
                            verticalCenter: parent.verticalCenter
                        }
                        text: qsTr("文件关联 · Windows 默认打开")
                        color: Theme.primaryText
                        font.pixelSize: 15
                        font.weight: Font.DemiBold
                    }
                }

                Item {
                    width: parent.width
                    implicitHeight: bodyColumn.implicitHeight + 28

                    Column {
                        id: bodyColumn

                        width: parent.width - 40
                        anchors {
                            top: parent.top
                            topMargin: 16
                            horizontalCenter: parent.horizontalCenter
                        }
                        spacing: 12

                        Text {
                            objectName: "fileAssociationSilentNote"
                            width: parent.width
                            text: qsTr("本程序是静音视觉审查工具：设为视频默认打开后不播放音频；不注册任何音频文件类型。视频与图片分别注册，可只选其一设为默认。")
                            color: Theme.mutedText
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Rectangle {
                            width: parent.width
                            implicitHeight: statusColumn.implicitHeight + 18
                            radius: 6
                            color: Theme.window
                            border.width: 1
                            border.color: Theme.border

                            Column {
                                id: statusColumn

                                width: parent.width - 20
                                anchors.centerIn: parent
                                spacing: 6

                                Text {
                                    objectName: "fileAssociationHealthText"
                                    width: parent.width
                                    text: qsTr("注册状态：%1").arg(control.healthLabel())
                                    color: Theme.primaryText
                                    font.pixelSize: 12
                                    elide: Text.ElideMiddle
                                }

                                Text {
                                    objectName: "fileAssociationVideoDefaultText"
                                    width: parent.width
                                    text: qsTr("视频默认打开（mp4/mkv/mov/avi/m4v）：%1").arg(control.ownershipLabel(control.videoDefault))
                                    color: Theme.primaryText
                                    font.pixelSize: 12
                                }

                                Text {
                                    objectName: "fileAssociationImageDefaultText"
                                    width: parent.width
                                    text: qsTr("图片默认打开（png/jpg/bmp/webp 等 13 种）：%1").arg(control.ownershipLabel(control.imageDefault))
                                    color: Theme.primaryText
                                    font.pixelSize: 12
                                }

                                Text {
                                    objectName: "fileAssociationReadBackNote"
                                    width: parent.width
                                    text: qsTr("状态读自系统记录，打开设置页本身不算完成；在系统中选择后回到此处刷新。")
                                    color: Theme.mutedText
                                    font.pixelSize: 11
                                    wrapMode: Text.WordWrap
                                }
                            }
                        }

                        // Registration never writes UserChoice, so it can never change the
                        // default by itself. Saying so up front is the difference between
                        // "the button did something" and "the button did nothing".
                        Text {
                            objectName: "fileAssociationDefaultNote"
                            width: parent.width
                            text: qsTr("「注册为可选应用」只把本应用加入系统的「打开方式」列表；要把视频或图片默认打开为本应用，请在系统设置中选择本应用。")
                            color: Theme.mutedText
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "fileAssociationResultText"
                            width: parent.width
                            visible: !control.busy && control.lastResult.length > 0
                            text: control.lastResult
                            color: Theme.success
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Text {
                            objectName: "fileAssociationErrorText"
                            width: parent.width
                            visible: control.lastError.length > 0
                            text: control.localizedError()
                            color: Theme.error
                            font.pixelSize: 12
                            wrapMode: Text.WordWrap
                        }

                        Row {
                            objectName: "fileActionButtonRow1"
                            width: parent.width
                            spacing: Theme.spaceMain

                            ReviewActionButton {
                                objectName: "fileAssociationRegisterButton"
                                text: qsTr("注册为可选应用")
                                prominent: true
                                helpText: qsTr("加入「打开方式」列表；不会、也无法直接更改默认应用。")
                                enabled: !control.busy && control.service !== null
                                onClicked: control.service.registerApplication()
                            }

                            ReviewActionButton {
                                objectName: "fileAssociationSettingsButton"
                                text: qsTr("前往系统设置…")
                                enabled: !control.busy && control.service !== null
                                onClicked: control.service.openSystemSettings()
                            }
                        }

                        Row {
                            objectName: "fileActionButtonRow2"
                            width: parent.width
                            spacing: Theme.spaceMain

                            ReviewActionButton {
                                objectName: "fileAssociationRepairButton"
                                text: qsTr("修复当前路径")
                                enabled: !control.busy && control.service !== null
                                onClicked: control.service.repairPaths()
                            }

                            ReviewActionButton {
                                objectName: "fileAssociationRemoveButton"
                                text: qsTr("移除本应用注册")
                                enabled: !control.busy && control.service !== null
                                onClicked: control.service.removeRegistration()
                            }

                            ReviewActionButton {
                                objectName: "fileAssociationRefreshButton"
                                text: qsTr("刷新状态")
                                enabled: !control.busy && control.service !== null
                                onClicked: control.service.refreshStatus()
                            }
                        }

                        Text {
                            objectName: "fileAssociationBusyText"
                            width: parent.width
                            visible: control.busy
                            text: qsTr("正在执行…")
                            color: Theme.mutedText
                            font.pixelSize: 12
                        }
                    }
                }

                Rectangle {
                    width: parent.width
                    height: 52
                    color: Theme.panel

                    Row {
                        spacing: Theme.spaceMain
                        anchors {
                            right: parent.right
                            rightMargin: 12
                            verticalCenter: parent.verticalCenter
                        }

                        ReviewActionButton {
                            objectName: "fileAssociationCloseButton"
                            text: qsTr("关闭")
                            implicitWidth: 84
                            implicitHeight: 30
                            leftPadding: 12
                            rightPadding: 12
                            onClicked: dialog.close()
                        }
                    }
                }
            }
        }
    }
}
