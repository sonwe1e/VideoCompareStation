pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property var host

    objectName: "advancedAlignmentInspector"
    width: 360
    color: Theme.panel
    border.color: control.host.borderColor

    ScrollView {
        anchors.fill: parent
        clip: true
        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff

        Column {
            id: alignmentControls

            width: control.width - 30
            spacing: 7
            leftPadding: 14
            rightPadding: 14
            topPadding: 14
            bottomPadding: 14

            Text {
                objectName: "alignmentModeStatus"
                width: parent.width - parent.leftPadding - parent.rightPadding
                text: control.host.anyManualAlignmentActive ? qsTr("手动对齐") : (control.host.autoAlignmentActive ? qsTr("自动对齐") : qsTr("严格索引"))
                color: control.host.anyManualAlignmentActive || control.host.autoAlignmentActive ? Theme.warning : Theme.success
                font.pixelSize: 12
                font.weight: Font.DemiBold
            }

            Text {
                objectName: "manualOffsetStatusLabel"
                width: parent.width - parent.leftPadding - parent.rightPadding
                text: control.host.manualOffsetActive ? qsTr("帧对齐偏移 · 已启用") : qsTr("帧对齐偏移")
                color: control.host.manualOffsetActive ? Theme.warning : control.host.mutedTextColor
                font.pixelSize: 11
            }

            Repeater {
                id: sourceOffsetRepeater

                objectName: "sourceOffsetRepeater"
                model: control.host.controller ? control.host.controller.sources : null

                delegate: Row {
                    id: sourceOffsetDelegate

                    required property int sourceId
                    required property int role
                    required property int manualOffset
                    property int sourceIdValue: sourceId
                    width: alignmentControls.width - alignmentControls.leftPadding - alignmentControls.rightPadding
                    spacing: 8

                    Text {
                        width: parent.width - sourceOffsetInput.width - parent.spacing
                        text: qsTr("源 %1 偏移（帧）").arg(String.fromCharCode(65 + sourceOffsetDelegate.sourceIdValue))
                        color: control.host.mutedTextColor
                        anchors.verticalCenter: parent.verticalCenter
                    }

                    ReviewOffsetSpinBox {
                        id: sourceOffsetInput

                        objectName: "sourceOffset-" + sourceOffsetDelegate.sourceIdValue
                        textColor: control.host.primaryTextColor
                        mutedTextColor: control.host.mutedTextColor
                        accentColor: control.host.accentColor
                        panelColor: control.host.raisedPanelColor
                        borderColor: control.host.borderColor
                        value: control.host.sourceOffset(sourceOffsetDelegate.sourceIdValue, sourceOffsetDelegate.manualOffset)
                        enabled: sourceOffsetDelegate.sourceIdValue !== (control.host.referenceSourceIndex >= 0 ? control.host.referenceSourceIndex : 0) && !control.host.busy
                        Accessible.name: qsTr("源 %1 全局帧偏移").arg(String.fromCharCode(65 + sourceOffsetDelegate.sourceIdValue))
                        onValueChanged: control.host.updateSourceOffset(sourceOffsetDelegate.sourceIdValue, value)
                    }
                }
            }

            ReviewActionButton {
                objectName: "estimateAlignmentButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                text: qsTr("估算全局帧偏移")
                helpText: qsTr("估算每个源相对参考源的恒定偏移。")
                enabled: control.host.graphicsReady && !control.host.busy && !control.host.alignmentAnalysisRunning && Boolean(control.host.controller && control.host.controller.canFirst)
                onClicked: control.host.controller.estimateAlignment()
            }

            ReviewActionButton {
                objectName: "analyzeSequenceButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                text: control.host.alignmentAnalysisRunning ? qsTr("取消分析") : qsTr("分析缺失 / 重复帧")
                helpText: qsTr("扫描丢帧、重复帧和局部时基变化。")
                enabled: control.host.graphicsReady && !control.host.busy && Boolean(control.host.controller && (control.host.alignmentAnalysisRunning || control.host.controller.canFirst))
                onClicked: control.host.alignmentAnalysisRunning ? control.host.controller.cancelAlignmentAnalysis() : control.host.controller.analyzeSequenceAlignment()
            }

            ReviewActionButton {
                objectName: "confirmAutomaticAlignmentButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                visible: control.host.automaticAlignmentPending
                text: control.host.canConfirmAutomaticAlignment ? qsTr("确认建议映射") : qsTr("确认前先分析序列")
                helpText: qsTr("在查看置信度和异常后，接受建议的自动映射。")
                enabled: control.host.graphicsReady && !control.host.busy && !control.host.alignmentAnalysisRunning && control.host.canConfirmAutomaticAlignment
                onClicked: control.host.controller.confirmAutomaticAlignment()
            }

            ReviewActionButton {
                objectName: "undoAutomaticAlignmentButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                visible: control.host.canUndoAutomaticAlignment
                text: qsTr("撤销自动映射")
                helpText: qsTr("恢复上次确认自动对齐之前的映射。")
                enabled: control.host.graphicsReady && !control.host.busy && !control.host.alignmentAnalysisRunning
                onClicked: control.host.controller.undoAutomaticAlignment()
            }

            ReviewActionButton {
                objectName: "manualAnchorsButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                text: control.host.manualAnchorActive ? qsTr("已设手动锚点…") : qsTr("编辑手动锚点…")
                helpText: qsTr("时基漂移时，可手动映射孤立帧点。")
                enabled: control.host.graphicsReady && !control.host.busy && Boolean(control.host.controller && control.host.controller.canFirst)
                onClicked: control.host.openManualAnchorsDialog()
            }

            ReviewActionButton {
                objectName: "applyAlignmentButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                text: qsTr("应用帧偏移")
                helpText: qsTr("应用上方显示的每个源的固定偏移。")
                enabled: control.host.graphicsReady && !control.host.busy && Boolean(control.host.controller && control.host.controller.canFirst)
                onClicked: control.host.controller.applySourceOffsets(control.host.sourceOffsets())
            }

            ReviewActionButton {
                objectName: "resetAlignmentButton"
                width: parent.width - parent.leftPadding - parent.rightPadding
                implicitHeight: 34
                text: qsTr("回到严格索引")
                helpText: qsTr("清除固定偏移，用相同基准帧号比较所有源。")
                enabled: control.host.graphicsReady && !control.host.busy && (control.host.anyManualAlignmentActive || control.host.autoAlignmentActive) && Boolean(control.host.controller && control.host.controller.canFirst)
                onClicked: {
                    control.host.resetSourceOffsets();
                    control.host.controller.applySourceOffsets(control.host.sourceOffsets());
                }
            }

            Text {
                visible: control.host.anyManualAlignmentActive || control.host.autoAlignmentActive
                text: qsTr("缺失的映射帧保持黑色；偏移不会被截断。")
                color: control.host.mutedTextColor
                font.pixelSize: 10
                width: parent.width - parent.leftPadding - parent.rightPadding
                wrapMode: Text.WordWrap
            }

            Text {
                visible: control.host.compatibilityDetails().length > 0
                text: control.host.compatibilityDetails()
                color: Theme.warning
                font.pixelSize: 10
                width: parent.width - parent.leftPadding - parent.rightPadding
                wrapMode: Text.WordWrap
            }
        }
    }
}
