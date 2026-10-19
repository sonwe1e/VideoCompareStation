pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Plain Popup shell, not a Dialog: a Dialog with a `title` creates style chrome of its own
// (an auto header that repeats the title plus an empty footer strip) around any custom
// background/contentItem, which reads as stray bars in the dark theme. The Popup draws only
// what this file defines, and still supports modal/dim/close-on-Escape.
Popup {
    id: control

    required property var pendingVideos
    required property var fileNameFunction
    property var pathNameFunction: null
    property int initialReferenceIndex: 0
    property string title: qsTr("确认源顺序和参考源")
    readonly property int referenceIndex: referenceCombo.currentIndex

    signal accepted
    signal rejected
    signal moveRequested(int fromIndex, int toIndex)

    function accept() {
        accepted();
        close();
    }

    function reject() {
        rejected();
        close();
    }

    component CompactMoveButton: VcsToolButton {
        id: button

        implicitWidth: 52
        implicitHeight: 30
        labelPixelSize: 14
    }

    function sourceNameAt(index) {
        if (index < 0 || index >= pendingVideos.length)
            return "";
        return fileNameFunction(pendingVideos[index]);
    }

    function sourcePathAt(index) {
        if (index < 0 || index >= pendingVideos.length)
            return "";
        if (!pathNameFunction)
            return "";
        return pathNameFunction(pendingVideos[index]);
    }

    objectName: "dropReviewDialog"
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    popupType: Popup.Item
    modal: true
    dim: true
    focus: true
    padding: 0
    width: Math.min(600, (parent ? parent.width : 800) - 48)
    closePolicy: Popup.CloseOnEscape
    onOpened: referenceCombo.currentIndex = Math.max(0, Math.min(initialReferenceIndex, pendingVideos.length - 1))

    function requestMove(fromIndex, toIndex) {
        const selected = referenceCombo.currentIndex;
        if (selected === fromIndex)
            referenceCombo.currentIndex = toIndex;
        else if (fromIndex < toIndex && selected > fromIndex && selected <= toIndex)
            referenceCombo.currentIndex = selected - 1;
        else if (toIndex < fromIndex && selected >= toIndex && selected < fromIndex)
            referenceCombo.currentIndex = selected + 1;
        moveRequested(fromIndex, toIndex);
    }

    Overlay.modal: Rectangle {
        color: Theme.modalScrim
    }

    // One continuous opaque shell. Header/body/footer share the same filled column so no
    // 1px seams can show the dimmed overlay between rounded chrome pieces.
    background: Rectangle {
        objectName: "dropDialogBackground"
        color: Theme.menu
        radius: Theme.radiusMedium
        border.width: 1
        border.color: Theme.menuBorder
    }

    // Vertical budget at the 640 px window minimum. The source list is the only part that
    // grows; header and footer stay pinned so 取消/确定 remain reachable without hunting.
    readonly property real shellMaxHeight: Math.max(320, (parent ? parent.height : 640) - 48)

    contentItem: Item {
        implicitWidth: 560
        implicitHeight: Math.min(dialogColumn.implicitHeight, control.shellMaxHeight)

        Column {
            id: dialogColumn

            width: parent.width

            Rectangle {
                id: headerBar

                objectName: "dropDialogHeader"
                width: parent.width
                height: 52
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
                    text: control.title
                    color: Theme.primaryText
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    anchors {
                        left: parent.left
                        leftMargin: 20
                        verticalCenter: parent.verticalCenter
                    }
                }
            }

            // Only the source list scrolls. The dialog previously took the whole column
            // height, so a long list of sources could exceed a short window and leave 确定
            // off-screen with no way to reach it.
            Flickable {
                id: bodyScroll

                objectName: "dropBodyScroll"
                width: parent.width
                height: Math.max(0, Math.min(bodyColumn.implicitHeight + 28, control.shellMaxHeight - headerBar.height - footerBar.height))
                contentWidth: width
                contentHeight: bodyColumn.implicitHeight + 28
                clip: true
                boundsBehavior: Flickable.StopAtBounds
                ScrollBar.vertical: VcsScrollBar {}

                Column {
                    id: bodyColumn

                    x: 20
                    y: 16
                    width: bodyScroll.width - 40
                    spacing: 12

                    Text {
                        width: parent.width
                        text: qsTr("下方顺序将对应源 A/B/C。打开前请选择以哪个源为帧号基准。")
                        color: Theme.mutedText
                        wrapMode: Text.WordWrap
                    }

                    Repeater {
                        model: control.pendingVideos

                        delegate: Column {
                            id: droppedRow

                            required property int index
                            required property var modelData

                            width: bodyColumn.width
                            spacing: 2

                            Row {
                                width: parent.width
                                spacing: Theme.spaceMain

                                Rectangle {
                                    width: 24
                                    height: 24
                                    radius: 5
                                    color: Theme.sourceBackground(droppedRow.index)
                                    border.width: 1
                                    border.color: Theme.sourceBorder(droppedRow.index)
                                    anchors.verticalCenter: parent.verticalCenter

                                    Text {
                                        anchors.centerIn: parent
                                        text: String.fromCharCode(65 + droppedRow.index)
                                        color: Theme.sourceColor(droppedRow.index)
                                        font.pixelSize: Theme.fontCaption
                                        font.bold: true
                                    }
                                }

                                Text {
                                    objectName: "dropSourceName-" + droppedRow.index
                                    width: parent.width - 152
                                    text: control.sourceNameAt(droppedRow.index)
                                    color: Theme.primaryText
                                    elide: Text.ElideMiddle
                                    anchors.verticalCenter: parent.verticalCenter
                                }
                                CompactMoveButton {
                                    text: "↑"
                                    enabled: droppedRow.index > 0
                                    Accessible.name: qsTr("上移源")
                                    onClicked: control.requestMove(droppedRow.index, droppedRow.index - 1)
                                }
                                CompactMoveButton {
                                    text: "↓"
                                    enabled: droppedRow.index + 1 < control.pendingVideos.length
                                    Accessible.name: qsTr("下移源")
                                    onClicked: control.requestMove(droppedRow.index, droppedRow.index + 1)
                                }
                            }

                            Text {
                                objectName: "dropSourcePath-" + droppedRow.index
                                x: 42
                                width: parent.width - 42
                                visible: text.length > 0
                                text: control.sourcePathAt(droppedRow.index)
                                color: Theme.mutedText
                                font.pixelSize: Theme.fontCaption
                                elide: Text.ElideMiddle
                            }
                        }
                    }

                    Row {
                        spacing: 12

                        Text {
                            text: qsTr("参考源")
                            color: Theme.primaryText
                            anchors.verticalCenter: parent.verticalCenter
                        }
                        ToolbarCombo {
                            id: referenceCombo

                            objectName: "dropReferenceCombo"
                            width: 240
                            model: control.pendingVideos.length === 3 ? [qsTr("源 A"), qsTr("源 B"), qsTr("源 C")] : [qsTr("源 A"), qsTr("源 B")]
                        }
                    }
                }
            }

            Rectangle {
                id: footerBar

                objectName: "dropDialogFooter"
                width: parent.width
                height: footerRow.implicitHeight + 24
                color: Theme.panel
                radius: Theme.radiusMedium

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

                    objectName: "dropDialogFooterRow"
                    spacing: Theme.spaceMain
                    anchors {
                        right: parent.right
                        rightMargin: 12
                        verticalCenter: parent.verticalCenter
                    }

                    ReviewActionButton {
                        objectName: "dropFooterCancelButton"
                        text: qsTr("取消")
                        implicitWidth: 88
                        implicitHeight: 30
                        onClicked: control.reject()
                    }
                    ReviewActionButton {
                        objectName: "dropFooterOkButton"
                        text: qsTr("确定")
                        prominent: true
                        implicitWidth: 88
                        implicitHeight: 30
                        // Default action: the popup's focus scope lands here on open, so Enter
                        // confirms without a Tab walk first (AbstractButton alone only maps
                        // Space). Space itself stays inert — modal media review keeps Space
                        // free of any dialog activation (the keyboard ownership contract in
                        // MainQmlContractTests).
                        focus: true
                        Keys.onReturnPressed: control.accept()
                        Keys.onEnterPressed: control.accept()
                        Keys.onSpacePressed: event => event.accepted = true
                        onClicked: control.accept()
                    }
                }
            }
        }
    }
}
