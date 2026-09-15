pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "VcsTheme.js" as Theme

Popup {
    id: control

    property bool playerPreset: false

    objectName: "shortcutHelpOverlay"
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, parent.width - 48)
    modal: true
    dim: true
    focus: true
    padding: 22
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    background: Rectangle {
        color: Theme.menu
        radius: 10
        border.color: Theme.menuBorder
    }

    contentItem: Column {
        spacing: 12

        Label {
            text: qsTr("键盘快捷键")
            color: Theme.primaryText
            font.pixelSize: 20
            font.weight: Font.DemiBold
        }
        Label {
            text: control.playerPreset ? qsTr("播放器快捷键") : qsTr("逐帧检查快捷键")
            color: "#9fc3ff"
            font.pixelSize: 12
        }
        GridLayout {
            width: parent.width
            columns: 2
            columnSpacing: 22
            rowSpacing: 7

            Repeater {
                model: control.playerPreset ? [[qsTr("← / →"), qsTr("快退 / 快进 5 秒")], [qsTr("Ctrl+← / →"), qsTr("快退 / 快进 30 秒")], [qsTr(", / ."), qsTr("上一帧 / 下一帧")], [qsTr("空格"), qsTr("播放 / 暂停")], [qsTr("I / O"), qsTr("设置入点 / 出点")], [qsTr("\\"), qsTr("播放选中区间")], [qsTr("F11 或双击"), qsTr("全屏")], [qsTr("右键"), qsTr("查看器命令")], [qsTr("Tab"), qsTr("隐藏界面")], [qsTr("?"), qsTr("本帮助")]] : [[qsTr("← / →"), qsTr("上一帧 / 下一帧")], [qsTr("Shift+← / →"), qsTr("前进 / 后退 5 帧")], [qsTr("Ctrl+← / →"), qsTr("前进 / 后退 1 秒")], [qsTr("A / D"), qsTr("上一帧 / 下一帧")], [qsTr("空格"), qsTr("播放 / 暂停")], [qsTr("I / O"), qsTr("设置入点 / 出点")], [qsTr("\\"), qsTr("播放选中区间")], [qsTr("F11 或双击"), qsTr("全屏")], [qsTr("右键"), qsTr("查看器命令")], [qsTr("Tab"), qsTr("隐藏界面")], [qsTr("?"), qsTr("本帮助")]]

                delegate: RowLayout {
                    required property var modelData
                    Layout.columnSpan: 2
                    Layout.fillWidth: true

                    Label {
                        Layout.preferredWidth: 180
                        text: String(parent.modelData[0])
                        color: "#9fc3ff"
                        font.pixelSize: 13
                    }
                    Label {
                        Layout.fillWidth: true
                        text: String(parent.modelData[1])
                        color: "#d8e2f2"
                        font.pixelSize: 13
                    }
                }
            }
        }
    }
}
