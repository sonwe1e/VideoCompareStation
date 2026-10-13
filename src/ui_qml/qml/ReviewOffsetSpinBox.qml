pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

SpinBox {
    id: control

    property bool blocksGlobalMediaShortcuts: true
    property bool textEditingInputContext: true
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText
    property color accentColor: Theme.accent
    property color panelColor: Theme.raisedPanel
    property color borderColor: Theme.border

    from: -16
    to: 16
    editable: true
    implicitWidth: 76
    implicitHeight: 34

    contentItem: TextInput {
        text: control.textFromValue(control.value, control.locale)
        color: control.enabled ? control.textColor : control.mutedTextColor
        selectionColor: control.accentColor
        selectedTextColor: Theme.inverseText
        horizontalAlignment: TextInput.AlignHCenter
        verticalAlignment: TextInput.AlignVCenter
        readOnly: !control.editable
        validator: control.validator
        inputMethodHints: Qt.ImhFormattedNumbersOnly
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.hovered ? Theme.controlHover : Theme.control
        border.width: 1
        border.color: control.activeFocus ? control.accentColor : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: 120
            }
        }
    }
}
