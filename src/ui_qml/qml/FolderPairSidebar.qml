pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Folder-comparison sidebar: lists same-named image pairs from two selected folders.
// Rows present on both sides open as a compare pair; single-sided rows are listed but
// disabled so missing files stay visible. Navigation buttons plus the model's row list
// give the "next pair / previous pair" flow of a folder diff tool.
Rectangle {
    id: control

    required property var pairModel
    required property bool hasFolders

    signal pairSelected(int row)
    signal changeFoldersRequested

    objectName: "folderPairSidebar"
    color: Theme.panel
    border.color: Theme.border
    border.width: 1

    readonly property int currentRow: {
        if (!pairModel)
            return -1;
        const row = Number(pairModel.currentPair);
        return row >= 0 ? row : -1;
    }

    function selectRow(row) {
        if (!pairModel || row < 0 || row >= pairModel.pairCount)
            return;
        // The model advances currentPair only when the pair actually committed, so a failed
        // open keeps the previous selection locked to the still-displayed canvas pair.
        if (!pairModel.openPairAt(row))
            return;
        pairSelected(row);
    }

    // T6: a single-sided row opens the side that exists, so a missing file can be
    // inspected without a file explorer. Same commit protocol as selectRow.
    function openMissingSide(row) {
        if (!pairModel || row < 0 || row >= pairModel.pairCount)
            return false;
        if (!pairModel.openSingleSideAt(row))
            return false;
        pairSelected(row);
        return true;
    }

    function stepPair(delta) {
        if (!pairModel)
            return;
        const row = pairModel.stepCompleteRow(delta);
        if (row >= 0)
            selectRow(row);
    }

    Column {
        anchors.fill: parent
        spacing: 0

        // Header: folder names plus a change action.
        Rectangle {
            id: sidebarHeader

            objectName: "folderPairSidebarHeader"
            width: parent.width
            height: headerColumn.implicitHeight + 20
            color: Theme.raisedPanel

            Column {
                id: headerColumn

                spacing: 6
                anchors {
                    left: parent.left
                    right: parent.right
                    top: parent.top
                    margins: 10
                }

                Text {
                    width: parent.width
                    text: qsTr("文件夹对比")
                    color: Theme.primaryText
                    font.pixelSize: 13
                    font.weight: Font.DemiBold
                }

                // T6: the pairing rule and its outcome counts. "已配对" only means the
                // names match on both sides — it never claims the contents are equal.
                Text {
                    objectName: "folderPairRuleLabel"
                    width: parent.width
                    visible: control.hasFolders
                    text: control.pairModel ? qsTr("同名配对（大小写不敏感）") : ""
                    color: Theme.mutedText
                    font.pixelSize: 11
                }

                // T6: pairing outcome counts. "已配对" only means the names match on both
                // sides — it never claims the contents are equal.
                Text {
                    objectName: "folderPairCountsLabel"
                    width: parent.width
                    visible: control.hasFolders
                    text: control.pairModel ? qsTr("完整 %1 · 缺失 %2 · 大小写冲突 %3").arg(control.pairModel.completeCount).arg(control.pairModel.missingCount).arg(control.pairModel.conflictCount) : ""
                    color: Theme.mutedText
                    font.pixelSize: 11
                    elide: Text.ElideRight
                }

                Text {
                    objectName: "folderPairLeftName"
                    width: parent.width
                    visible: control.hasFolders
                    text: control.pairModel ? "%1 · %2".arg(qsTr("A")).arg(control.pairModel.leftFolderName) : ""
                    color: Theme.mutedText
                    font.pixelSize: 11
                    elide: Text.ElideMiddle

                    HoverHandler {
                        id: leftFolderHover
                    }

                    VcsToolTip {
                        visible: leftFolderHover.hovered && control.pairModel && control.pairModel.leftFolderPath.length > 0
                        text: control.pairModel ? control.pairModel.leftFolderPath : ""
                    }
                }

                Text {
                    objectName: "folderPairRightName"
                    width: parent.width
                    visible: control.hasFolders
                    text: control.pairModel ? "%1 · %2".arg(qsTr("B")).arg(control.pairModel.rightFolderName) : ""
                    color: Theme.mutedText
                    font.pixelSize: 11
                    elide: Text.ElideMiddle

                    HoverHandler {
                        id: rightFolderHover
                    }

                    VcsToolTip {
                        visible: rightFolderHover.hovered && control.pairModel && control.pairModel.rightFolderPath.length > 0
                        text: control.pairModel ? control.pairModel.rightFolderPath : ""
                    }
                }

                ReviewActionButton {
                    objectName: "folderPairChangeButton"
                    width: parent.width
                    implicitHeight: 28
                    leftPadding: 8
                    rightPadding: 8
                    text: qsTr("更换文件夹…")
                    onClicked: control.changeFoldersRequested()
                }
            }
        }

        // Pair list.
        Rectangle {
            objectName: "folderPairListChrome"
            width: parent.width
            height: parent.height - sidebarHeader.height - footerBar.height
            color: "transparent"

            ScrollView {
                id: pairScroll

                objectName: "folderPairScroll"
                anchors.fill: parent
                clip: true

                ListView {
                    id: pairList

                    objectName: "folderPairList"
                    anchors.fill: parent
                    model: control.pairModel
                    boundsBehavior: Flickable.StopAtBounds
                    // T6: the committed selection drives currentIndex so the highlighted row
                    // and the auto-scroll below stay on the pair the canvas actually shows.
                    currentIndex: control.currentRow
                    // Keyboard stepping through a long folder keeps the selected row visible.
                    onCurrentIndexChanged: {
                        if (pairList.currentIndex >= 0)
                            pairList.positionViewAtIndex(pairList.currentIndex, ListView.Contain);
                    }

                    delegate: Rectangle {
                        id: pairRow

                        required property int index
                        required property string fileName
                        required property bool hasLeft
                        required property bool hasRight
                        required property bool hasBoth
                        required property bool caseConflict
                        required property string caseConflictDetail

                        objectName: "folderPairRow-" + index
                        width: pairList.width
                        height: 34
                        color: pairRow.hasBoth ? (pairRow.index === control.currentRow ? Theme.controlChecked : (rowHover.hovered ? Theme.control : "transparent")) : "transparent"
                        opacity: pairRow.hasBoth ? 1.0 : 0.55

                        Rectangle {
                            id: statusDot

                            objectName: "folderPairDot-" + pairRow.index
                            width: 8
                            height: 8
                            radius: 4
                            color: pairRow.hasBoth ? Theme.success : Theme.warning
                            anchors {
                                left: parent.left
                                leftMargin: 10
                                verticalCenter: parent.verticalCenter
                            }
                        }

                        Text {
                            width: parent.width - statusDot.width - 28
                            anchors {
                                left: statusDot.right
                                leftMargin: 8
                                verticalCenter: parent.verticalCenter
                            }
                            text: pairRow.fileName
                            color: pairRow.hasBoth ? Theme.primaryText : Theme.mutedText
                            font.pixelSize: 12
                            elide: Text.ElideMiddle
                        }

                        // T6: an explicit marker for names that fold to the same key on one
                        // side, so an ambiguous pairing is listed instead of silently chosen.
                        Text {
                            visible: pairRow.caseConflict
                            anchors {
                                right: parent.right
                                rightMargin: pairRow.hasBoth ? 34 : 44
                                verticalCenter: parent.verticalCenter
                            }
                            text: "⚠"
                            color: Theme.warning
                            font.pixelSize: 12

                            HoverHandler {
                                id: conflictHover
                            }

                            VcsToolTip {
                                visible: conflictHover.hovered && pairRow.caseConflictDetail.length > 0
                                text: pairRow.caseConflictDetail
                            }
                        }

                        Text {
                            visible: !pairRow.hasBoth
                            anchors {
                                right: parent.right
                                rightMargin: 10
                                verticalCenter: parent.verticalCenter
                            }
                            text: pairRow.hasLeft ? qsTr("仅 A") : qsTr("仅 B")
                            color: Theme.mutedText
                            font.pixelSize: 10
                        }

                        HoverHandler {
                            id: rowHover
                        }

                        VcsToolTip {
                            visible: rowHover.hovered
                            text: pairRow.hasBoth ? pairRow.fileName : qsTr("查看存在的一侧：%1").arg(pairRow.fileName)
                        }

                        MouseArea {
                            anchors.fill: parent
                            onClicked: {
                                if (pairRow.hasBoth)
                                    control.selectRow(pairRow.index);
                                else
                                    control.openMissingSide(pairRow.index);
                            }
                        }
                    }

                    Text {
                        anchors.centerIn: parent
                        visible: control.pairModel && control.pairModel.pairCount === 0
                        text: control.pairModel && control.pairModel.errorText.length > 0 ? control.pairModel.errorText : qsTr("没有可对比的图片。")
                        color: Theme.mutedText
                        font.pixelSize: 12
                    }
                }
            }
        }

        // Footer: previous / index / next navigation.
        Rectangle {
            id: footerBar

            objectName: "folderPairFooter"
            width: parent.width
            height: 44
            color: Theme.raisedPanel

            Row {
                spacing: 6
                anchors {
                    horizontalCenter: parent.horizontalCenter
                    verticalCenter: parent.verticalCenter
                }

                ReviewActionButton {
                    objectName: "folderPairPreviousButton"
                    implicitHeight: 28
                    implicitWidth: 72
                    leftPadding: 6
                    rightPadding: 6
                    text: qsTr("◀ 上一对")
                    enabled: control.pairModel && control.pairModel.pairCount > 0
                    onClicked: control.stepPair(-1)
                }

                Text {
                    objectName: "folderPairPositionLabel"
                    anchors.verticalCenter: parent.verticalCenter
                    text: {
                        if (!control.pairModel || control.pairModel.pairCount === 0)
                            return "0/0";
                        const base = "%1/%2".arg(control.currentRow >= 0 ? control.currentRow + 1 : 0).arg(control.pairModel.pairCount);
                        return control.pairModel.openPending ? qsTr("%1 打开中…").arg(base) : base;
                    }
                    color: Theme.primaryText
                    font.pixelSize: 12
                }

                ReviewActionButton {
                    objectName: "folderPairNextButton"
                    implicitHeight: 28
                    implicitWidth: 72
                    leftPadding: 6
                    rightPadding: 6
                    text: qsTr("下一对 ▶")
                    enabled: control.pairModel && control.pairModel.pairCount > 0
                    onClicked: control.stepPair(1)
                }
            }
        }
    }
}
