pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

// Popup shell for the M-key issue capture: Enter saves with the typed note, Escape saves with
// an empty note (the note is optional, never a gate), and the capture happens on close either
// way. Focus lands in the field so the user can type immediately.
Popup {
    id: control

    signal accepted(string note)

    objectName: "issueNoteDialog"
    parent: Overlay.overlay
    anchors.centerIn: Overlay.overlay
    popupType: Popup.Item
    modal: true
    dim: true
    focus: true
    padding: 0
    width: Math.min(460, (parent ? parent.width : 800) - 48)
    // Escape must record without a note rather than dismiss, so it is handled in the field;
    // outside-click dismissals are ignored to keep the capture explicit.
    closePolicy: Popup.NoAutoClose

    function save() {
        const note = noteField.text;
        control.accepted(note);
        control.close();
    }

    // Escape is "skip the note", not "cancel the capture": the issue is still recorded, just
    // without a note. Clearing first keeps save() on a single path.
    function saveEmpty() {
        noteField.clear();
        save();
    }

    onOpened: {
        noteField.clear();
        noteField.forceActiveFocus();
    }

    Overlay.modal: Rectangle {
        color: Theme.modalScrim
    }

    background: Rectangle {
        color: Theme.menu
        radius: Theme.radiusCard
        border.width: 1
        border.color: Theme.menuBorder
    }

    contentItem: Column {
        spacing: 14
        padding: 22

        Label {
            text: qsTr("记录当前问题")
            color: Theme.primaryText
            font.pixelSize: 16
            font.weight: Font.DemiBold
        }
        Label {
            width: parent.width - parent.padding * 2
            text: qsTr("备注可留空。回车保存，Esc 直接记录。")
            color: Theme.secondaryText
            font.pixelSize: 12
            wrapMode: Text.Wrap
        }
        TextField {
            id: noteField

            objectName: "issueNoteField"
            width: parent.width - parent.padding * 2
            placeholderText: qsTr("备注（可选）")
            color: Theme.primaryText
            placeholderTextColor: Theme.disabledText
            selectionColor: Theme.accent
            selectedTextColor: "#ffffff"
            font.pixelSize: 13
            background: Rectangle {
                radius: Theme.radiusSmall
                color: Theme.panel
                border.width: 1
                border.color: noteField.activeFocus ? Theme.accent : Theme.controlBorder
            }
            onAccepted: control.save()
            Keys.onEscapePressed: control.saveEmpty()
        }
        Row {
            spacing: 8
            layoutDirection: Qt.RightToLeft

            anchors {
                right: parent.right
                rightMargin: parent.padding
            }

            VcsToolButton {
                objectName: "issueNoteSaveButton"
                text: qsTr("记录")
                labelPixelSize: 14
                onClicked: control.save()
            }
            VcsToolButton {
                text: qsTr("留空记录")
                labelPixelSize: 14
                onClicked: control.saveEmpty()
            }
        }
    }
}
