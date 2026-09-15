pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "VcsTheme.js" as Theme
// qmllint disable import
import Dvs.Ui 1.0

// qmllint enable import

Rectangle {
    id: control

    required property var controller
    required property var preferences
    required property var session
    required property var alignmentHost
    required property color borderColor
    required property color primaryTextColor
    required property color mutedTextColor
    required property bool singleMode
    required property int sourceCount
    required property bool wipeMode
    required property bool differenceMode
    required property bool analysisGridMode
    required property var differenceEdges
    required property var sourceIdentities
    required property int differenceEdge
    required property int referenceSourceIndex
    required property bool differenceThresholdEnabled
    required property int differenceThresholdCode
    required property int differenceThresholdPolicy
    required property real wipePosition
    required property bool roiEnabled
    required property bool graphicsReady
    required property bool dropFrameTimecode
    required property int currentFrame
    required property int inFrame
    required property int outFrame
    required property bool rangePlaybackActive

    signal differenceEdgeRequested(int edge)
    signal referenceRequested(string sourceIdentity)
    signal differenceThresholdEnabledRequested(bool enabled)
    signal differenceThresholdCodeRequested(int code)
    signal differenceThresholdPolicyRequested(int policy)
    signal wipePositionRequested(real position)
    signal resetViewportRequested
    signal clearRoiRequested
    signal dropFrameTimecodeRequested(bool enabled)
    signal inPointRequested
    signal outPointRequested
    signal clearRangeRequested
    signal rangeLoopToggleRequested

    objectName: "tabbedInspector"
    width: Math.max(300, Math.min(380, parent ? parent.width * 0.3 : 360))
    color: Theme.panel
    border.color: control.borderColor

    readonly property int effectiveTab: control.singleMode && tabs.currentIndex < 2 ? 2 : tabs.currentIndex

    function differenceEdgeIndex(preferenceValue) {
        for (let index = 0; index < control.differenceEdges.length; ++index) {
            if (Number(control.differenceEdges[index].preferenceValue) === Number(preferenceValue))
                return index;
        }
        return 0;
    }

    component DarkTabButton: TabButton {
        id: darkTabButton

        leftPadding: 10
        rightPadding: 10
        topPadding: 8
        bottomPadding: 8

        contentItem: Text {
            text: darkTabButton.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
            color: darkTabButton.checked ? Theme.primaryText : (darkTabButton.hovered ? Theme.primaryText : Theme.mutedText)
            font.pixelSize: 12
            font.weight: darkTabButton.checked ? Font.DemiBold : Font.Normal
        }

        background: Rectangle {
            color: darkTabButton.checked ? Theme.controlChecked : (darkTabButton.hovered ? Theme.control : "transparent")

            Rectangle {
                visible: darkTabButton.checked
                height: 2
                color: Theme.accent
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                    leftMargin: 8
                    rightMargin: 8
                }
            }
        }
    }

    component DarkCheckBox: CheckBox {
        id: darkCheckBox

        leftPadding: 0
        spacing: 8

        contentItem: Text {
            text: darkCheckBox.text
            font.pixelSize: 12
            color: darkCheckBox.enabled ? Theme.primaryText : Theme.disabledText
            verticalAlignment: Text.AlignVCenter
            leftPadding: darkCheckBox.indicator.width + darkCheckBox.spacing
        }

        indicator: Rectangle {
            x: darkCheckBox.leftPadding
            y: darkCheckBox.height / 2 - height / 2
            implicitWidth: 16
            implicitHeight: 16
            radius: 3
            color: darkCheckBox.checked ? Theme.accent : Theme.raisedPanel
            border.color: darkCheckBox.checked ? Theme.accent : (darkCheckBox.hovered ? Theme.accent : Theme.menuBorder)
            border.width: 1

            Text {
                anchors.centerIn: parent
                text: darkCheckBox.checked ? "✓" : ""
                color: Theme.inverseText
                font.pixelSize: 10
                font.bold: true
            }
        }
    }

    TabBar {
        id: tabs

        objectName: "inspectorTabBar"
        width: parent.width
        background: Rectangle {
            color: Theme.menu

            Rectangle {
                anchors {
                    left: parent.left
                    right: parent.right
                    bottom: parent.bottom
                }
                height: 1
                color: Theme.border
            }
        }
        DarkTabButton {
            objectName: "compareTabButton"
            visible: !control.singleMode
            text: qsTr("对比")
        }
        DarkTabButton {
            objectName: "alignmentTabButton"
            visible: !control.singleMode
            text: qsTr("对齐")
        }
        DarkTabButton {
            objectName: "reviewTabButton"
            text: qsTr("播放")
        }
        DarkTabButton {
            objectName: "infoTabButton"
            text: qsTr("信息")
        }
    }

    StackLayout {
        currentIndex: control.effectiveTab
        anchors {
            top: tabs.bottom
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }

        Flickable {
            clip: true
            contentHeight: compareColumn.implicitHeight + 28

            Column {
                id: compareColumn

                width: parent.width - 28
                spacing: 10
                x: 14
                y: 14

                Label {
                    text: qsTr("对比")
                    color: control.primaryTextColor
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }

                Label {
                    text: qsTr("对比对")
                    visible: pairCombo.visible
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    id: pairCombo
                    visible: control.sourceCount === 3 && (control.wipeMode || control.differenceMode || control.analysisGridMode)
                    width: parent.width
                    model: control.differenceEdges
                    textRole: "label"
                    currentIndex: control.differenceEdgeIndex(control.differenceEdge)
                    onActivated: index => {
                        if (index >= 0 && index < control.differenceEdges.length)
                            control.differenceEdgeRequested(Number(control.differenceEdges[index].preferenceValue));
                    }
                }

                Label {
                    text: qsTr("参考源")
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    width: parent.width
                    model: control.controller ? control.controller.sources : null
                    textRole: "filename"
                    currentIndex: Math.max(0, control.referenceSourceIndex)
                    onActivated: index => {
                        if (index >= 0 && index < control.sourceIdentities.length)
                            control.referenceRequested(String(control.sourceIdentities[index]));
                    }
                }

                Label {
                    visible: control.differenceMode
                    text: qsTr("差异度量")
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    visible: control.differenceMode
                    width: parent.width
                    model: [qsTr("RGB 绝对值"), qsTr("亮度"), qsTr("色度"), qsTr("热力图"), qsTr("精确平面"), qsTr("带符号相减"), qsTr("高亮")]
                    currentIndex: Math.min(6, Number(control.preferences.differenceMetric))
                    onActivated: index => control.preferences.differenceMetric = index
                }

                Label {
                    visible: control.differenceMode
                    text: qsTr("增益")
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    visible: control.differenceMode
                    width: parent.width
                    model: [qsTr("1×"), qsTr("2×"), qsTr("4×"), qsTr("8×"), qsTr("16×")]
                    currentIndex: Number(control.preferences.differenceGain)
                    onActivated: index => control.preferences.differenceGain = index
                }

                Label {
                    visible: control.differenceMode
                    text: qsTr("滤镜")
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    visible: control.differenceMode
                    width: parent.width
                    model: [qsTr("最近邻"), qsTr("双线性"), qsTr("双三次")]
                    currentIndex: Number(control.preferences.differenceFilter)
                    onActivated: index => control.preferences.differenceFilter = index
                }

                DarkCheckBox {
                    visible: control.differenceMode
                    text: qsTr("阈值")
                    checked: control.differenceThresholdEnabled
                    onToggled: control.differenceThresholdEnabledRequested(checked)
                }
                ReviewOffsetSpinBox {
                    id: thresholdSpinBox

                    visible: control.differenceMode && control.differenceThresholdEnabled
                    width: parent.width
                    from: 0
                    to: 255
                    value: control.differenceThresholdCode
                    editable: true
                    Accessible.name: qsTr("差异阈值（8 位码值）")
                    onValueModified: control.differenceThresholdCodeRequested(value)
                }
                ToolbarCombo {
                    visible: control.differenceMode && control.differenceThresholdEnabled
                    width: parent.width
                    model: [qsTr("亮度"), qsTr("任一通道"), qsTr("全部通道")]
                    currentIndex: control.differenceThresholdPolicy
                    onActivated: index => control.differenceThresholdPolicyRequested(index)
                }

                Label {
                    visible: control.wipeMode
                    text: qsTr("擦除位置 · %1%").arg(Math.round(control.wipePosition * 100))
                    color: control.mutedTextColor
                }
                Slider {
                    id: wipeSlider

                    visible: control.wipeMode
                    width: parent.width
                    from: 0
                    to: 1
                    value: control.wipePosition
                    padding: 6
                    onMoved: control.wipePositionRequested(value)

                    background: Rectangle {
                        x: wipeSlider.leftPadding + (wipeSlider.horizontal ? 0 : (wipeSlider.availableWidth - width) / 2)
                        y: wipeSlider.topPadding + (wipeSlider.horizontal ? (wipeSlider.availableHeight - height) / 2 : 0)
                        width: wipeSlider.horizontal ? wipeSlider.availableWidth : implicitWidth
                        height: wipeSlider.horizontal ? implicitHeight : wipeSlider.availableHeight
                        implicitWidth: 200
                        implicitHeight: 6
                        radius: 3
                        color: Theme.menuBorder

                        Rectangle {
                            y: wipeSlider.horizontal ? 0 : wipeSlider.visualPosition * parent.height
                            width: wipeSlider.horizontal ? wipeSlider.position * parent.width : 6
                            height: wipeSlider.horizontal ? 6 : wipeSlider.position * parent.height
                            radius: 3
                            color: Theme.accent
                        }
                    }

                    handle: Rectangle {
                        x: wipeSlider.leftPadding + (wipeSlider.horizontal ? wipeSlider.visualPosition * (wipeSlider.availableWidth - width) : (wipeSlider.availableWidth - width) / 2)
                        y: wipeSlider.topPadding + (wipeSlider.horizontal ? (wipeSlider.availableHeight - height) / 2 : wipeSlider.visualPosition * (wipeSlider.availableHeight - height))
                        implicitWidth: 16
                        implicitHeight: 16
                        radius: width / 2
                        color: wipeSlider.pressed ? Theme.controlPressed : Theme.accent
                        border.width: wipeSlider.activeFocus ? 2 : 1
                        border.color: wipeSlider.activeFocus ? Theme.strongFocus : Theme.primaryText
                    }
                }

                ReviewActionButton {
                    width: parent.width
                    implicitHeight: 34
                    text: qsTr("重置缩放和平移")
                    helpText: qsTr("恢复完整画面视图。")
                    onClicked: control.resetViewportRequested()
                }
                ReviewActionButton {
                    visible: control.roiEnabled
                    width: parent.width
                    implicitHeight: 34
                    text: qsTr("清除 ROI")
                    helpText: qsTr("移除当前关注的区域。")
                    onClicked: control.clearRoiRequested()
                }
            }
        }

        AlignmentInspector {
            host: control.alignmentHost
        }

        Flickable {
            clip: true
            contentHeight: reviewColumn.implicitHeight + 28

            Column {
                id: reviewColumn

                width: parent.width - 28
                spacing: 10
                x: 14
                y: 14

                Label {
                    text: qsTr("播放")
                    color: control.primaryTextColor
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Label {
                    text: qsTr("画面控件")
                    color: control.mutedTextColor
                }
                ToolbarCombo {
                    width: parent.width
                    model: [qsTr("按需"), qsTr("固定"), qsTr("自动隐藏"), qsTr("隐藏")]
                    currentIndex: control.preferences && Number(control.preferences.oscMode) >= 0 ? Number(control.preferences.oscMode) + 1 : 0
                    onActivated: index => control.preferences.oscMode = index - 1
                }
                DarkCheckBox {
                    visible: control.controller && control.controller.dropFrameTimecodeAvailable
                    text: checked ? qsTr("丢帧时间码（DF）") : qsTr("非丢帧时间码（NDF）")
                    checked: control.dropFrameTimecode
                    onToggled: control.dropFrameTimecodeRequested(checked)
                }
                Label {
                    text: qsTr("入点：%1").arg(control.inFrame >= 0 ? control.inFrame + 1 : "—")
                    color: control.mutedTextColor
                }
                Label {
                    text: qsTr("出点：%1").arg(control.outFrame >= 0 ? control.outFrame + 1 : "—")
                    color: control.mutedTextColor
                }

                GridLayout {
                    width: parent.width
                    columns: 2
                    columnSpacing: 8
                    rowSpacing: 8

                    ReviewActionButton {
                        objectName: "setInButton"
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: qsTr("设置入点")
                        helpText: qsTr("将区间起点设为当前帧（I）。")
                        enabled: control.currentFrame >= 0
                        onClicked: control.inPointRequested()
                    }
                    ReviewActionButton {
                        objectName: "setOutButton"
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: qsTr("设置出点")
                        helpText: qsTr("将区间终点设为当前帧（O）。")
                        enabled: control.currentFrame >= 0
                        onClicked: control.outPointRequested()
                    }
                    ReviewActionButton {
                        objectName: "clearRangeButton"
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: qsTr("清除区间")
                        helpText: qsTr("清除入点、出点和循环播放。")
                        enabled: control.inFrame >= 0 || control.outFrame >= 0
                        onClicked: control.clearRangeRequested()
                    }
                    ReviewActionButton {
                        objectName: "loopRangeButton"
                        Layout.fillWidth: true
                        implicitHeight: 34
                        text: control.rangePlaybackActive ? qsTr("停止循环") : qsTr("循环区间")
                        helpText: qsTr("切换选中区间的播放。")
                        enabled: control.inFrame >= 0 && control.outFrame >= control.inFrame
                        onClicked: control.rangeLoopToggleRequested()
                    }
                }

                Label {
                    text: qsTr("标记图例")
                    color: control.primaryTextColor
                    font.weight: Font.DemiBold
                }
                Label {
                    width: parent.width
                    wrapMode: Text.WordWrap
                    text: qsTr("红 · 缺失    橙 · 重复    紫 · 多余\n青 · 锚点    黄 · 低置信度")
                    color: control.mutedTextColor
                }
                Label {
                    objectName: "markerOverflowNotice"
                    width: parent.width
                    visible: Boolean(control.controller) && Number(control.controller.alignmentTimelineMarkerOverflowCount) > 0
                    wrapMode: Text.WordWrap
                    text: qsTr("时间轴上未显示其余 %1 个对齐标记。").arg(Number(control.controller.alignmentTimelineMarkerOverflowCount))
                    color: Theme.warning
                }
            }
        }

        Flickable {
            clip: true
            contentHeight: infoColumn.implicitHeight + 28

            Column {
                id: infoColumn

                width: parent.width - 28
                spacing: 10
                x: 14
                y: 14

                Label {
                    text: qsTr("媒体信息")
                    color: control.primaryTextColor
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Repeater {
                    objectName: "mediaInfoRepeater"
                    model: control.controller ? control.controller.sourceMediaInfo : []

                    Rectangle {
                        id: mediaCard

                        required property var modelData
                        width: infoColumn.width
                        height: mediaInfo.implicitHeight + 20
                        radius: 6
                        color: Theme.raisedPanel
                        border.color: control.borderColor

                        Label {
                            id: mediaInfo
                            width: parent.width - 20
                            x: 10
                            y: 10
                            wrapMode: Text.Wrap
                            readonly property string decoderFallbackSuffix: mediaCard.modelData.decodeFallbackReason ? qsTr(" (%1)").arg(String(mediaCard.modelData.decodeFallbackReason)) : ""
                            text: qsTr("源 %1 · %2\n%3 × %4 · %5 · %6 帧\n%7 · %8 · %9 位\n%10 · %11 · %12\n解码：%13%14 · 角色：%15").arg(String(mediaCard.modelData.label)).arg(String(mediaCard.modelData.filename)).arg(Number(mediaCard.modelData.width)).arg(Number(mediaCard.modelData.height)).arg(String(mediaCard.modelData.frameRate)).arg(Number(mediaCard.modelData.frameCount)).arg(String(mediaCard.modelData.timingMode)).arg(String(mediaCard.modelData.codec)).arg(Number(mediaCard.modelData.bitDepth)).arg(String(mediaCard.modelData.pixelFormat)).arg(String(mediaCard.modelData.colorMatrix)).arg(String(mediaCard.modelData.colorRange)).arg(String(mediaCard.modelData.decodeBackend)).arg(decoderFallbackSuffix).arg(String(mediaCard.modelData.role))
                            color: control.mutedTextColor
                            font.pixelSize: 11
                        }
                    }
                }
                Label {
                    text: control.graphicsReady ? qsTr("D3D11 渲染就绪") : qsTr("图形设备不可用")
                    color: control.graphicsReady ? Theme.success : Theme.warning
                }
            }
        }
    }
}
