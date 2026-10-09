pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "VcsTheme.js" as Theme

Popup {
    id: control

    property bool playerPreset: false
    property bool imagePreset: false

    // Keyboard rows for the video presets are generated from ReviewShortcuts' own binding
    // table (helpKeys/helpLabel declared next to each Shortcut), so the help cannot drift
    // from the bindings. Mouse verbs and the Main-owned M capture stay static tails.
    property var mediaEntries: []

    // The shortcut table is built on first open rather than at startup: modal help is opened
    // rarely, and its label tree is part of the instantiation bill otherwise. contentReady
    // keeps the table resident afterwards so repeated opens are immediate.
    property bool contentReady: false

    readonly property var staticVideoTails: [[qsTr("M"), qsTr("记录当前问题，可填备注")], [qsTr("Shift+拖动"), qsTr("框选局部并放大")], [qsTr("Alt+拖动"), qsTr("框选 ROI 区域对比")], [qsTr("双击"), qsTr("切换 100% 真实尺寸 / 适应窗口")], [qsTr("滚轮"), qsTr("以光标位置为中心缩放")], [qsTr("右键"), qsTr("查看器命令")]]

    readonly property var shortcutModel: {
        if (control.imagePreset) {
            return [[qsTr("A"), qsTr("切换 Alpha 灰度通道独立显示")], [qsTr("O"), qsTr("切换 RGB 忽略透明度模式")], [qsTr("空格 / T"), qsTr("手动闪烁模式切换 A / B")], [qsTr("点击画面"), qsTr("手动闪烁模式切换 A / B")], [qsTr("← / →"), qsTr("上一对 / 下一对图片")], [qsTr("Home / End"), qsTr("第一对 / 最后一对图片")], [qsTr("对调 A/B"), qsTr("交换两侧槽位方向（不改变配对身份）")], [qsTr("换图…"), qsTr("只替换 A 或只替换 B，另一侧保持不变")], [qsTr("Shift+拖动"), qsTr("框选局部并同步放大 A/B")], [qsTr("双击"), qsTr("切换 100% 真实尺寸 / 适应窗口")], [qsTr("滚轮"), qsTr("以光标位置为中心缩放")], [qsTr("拖动 / 中键"), qsTr("平移视口画布")], [qsTr("M"), qsTr("记录当前问题，可填备注")], [qsTr("Tab"), qsTr("隐藏界面")], [qsTr("?"), qsTr("本帮助")]];
        }
        // mediaEntries rows are [keycaps, label, playerLabel, presetMask]: -1 shows in both
        // video presets, 1 in the player preset only; playerLabel retells rows whose meaning
        // differs between the presets.
        const rows = [];
        for (let i = 0; i < control.mediaEntries.length; ++i) {
            const entry = control.mediaEntries[i];
            if (entry.length < 4 || entry[3] === 1 && !control.playerPreset)
                continue;
            rows.push([entry[0], control.playerPreset ? entry[2] : entry[1]]);
        }
        return rows.concat(control.staticVideoTails);
    }

    objectName: "shortcutHelpOverlay"
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    width: Math.min(560, parent.width - 48)
    modal: true
    dim: true
    focus: true
    padding: 22
    closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside

    onOpened: control.contentReady = true

    // Opaque like every other popup surface, so dimmed footage never bleeds through the text.
    background: Rectangle {
        color: Theme.menu
        radius: Theme.radiusCard
        border.width: 1
        border.color: Theme.menuBorder
    }

    contentItem: Loader {
        active: control.opened || control.contentReady

        sourceComponent: shortcutTable
    }

    Component {
        id: shortcutTable

        Column {
            spacing: 14

            Item {
                width: parent.width
                height: helpTitle.implicitHeight

                Label {
                    id: helpTitle

                    text: qsTr("键盘快捷键")
                    color: Theme.primaryText
                    font.pixelSize: 20
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("Esc 关闭")
                    color: Theme.disabledText
                    font.pixelSize: 12
                    anchors {
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                    }
                }
            }
            Label {
                text: control.imagePreset ? qsTr("图像与透明度检查快捷键") : (control.playerPreset ? qsTr("播放器快捷键") : qsTr("逐帧检查快捷键"))
                color: Theme.accentText
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }
            GridLayout {
                width: parent.width
                columns: 2
                columnSpacing: 22
                rowSpacing: 8

                Repeater {
                    model: control.shortcutModel

                    delegate: RowLayout {
                        id: shortcutRow
                        required property var modelData
                        Layout.columnSpan: 2
                        Layout.fillWidth: true
                        spacing: 12

                        // Keys sit left-aligned in a fixed column as keycaps sized to their
                        // text, so descriptions start on one edge and short keys stay compact.
                        Item {
                            Layout.preferredWidth: 150
                            implicitHeight: 24

                            Rectangle {
                                width: Math.min(parent.width, keyLabel.implicitWidth + 16)
                                height: parent.height
                                radius: Theme.radiusSmall
                                color: Theme.keycap
                                border.width: 1
                                border.color: Theme.keycapBorder

                                Label {
                                    id: keyLabel

                                    anchors.centerIn: parent
                                    text: String(shortcutRow.modelData[0])
                                    color: Theme.primaryText
                                    font.pixelSize: 12
                                    font.weight: Font.DemiBold
                                }
                            }
                        }

                        Label {
                            Layout.fillWidth: true
                            text: String(shortcutRow.modelData[1])
                            color: Theme.secondaryText
                            font.pixelSize: 13
                        }
                    }
                }
            }
        }
    }
}
