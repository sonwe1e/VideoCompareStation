pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

Button {
    id: control

    property bool blocksGlobalMediaShortcuts: true
    property string helpText: ""
    property bool prominent: false
    property color textColor: Theme.primaryText
    property color accentColor: Theme.accent
    // Optional item that takes the focus back after a click. A held AbstractButton maps Space
    // to its own activation, so a surface whose root owns the Space shortcut (the image
    // workspace's manual A/B flicker) sets this to that root. Null keeps today's behaviour.
    property var focusTarget: null
    // `checked` mirrors state the caller owns (a mode, a tool). Qt flips `checked` itself
    // before onClicked, so re-clicking the selected option would drop its highlight while the
    // state stays selected; undo that flip and leave the caller's binding in charge.
    property bool mirrorsState: false

    implicitWidth: Math.max(116, contentItem.implicitWidth + 36)
    implicitHeight: 42
    leftPadding: 18
    rightPadding: 18
    activeFocusOnTab: true
    hoverEnabled: true
    Accessible.description: helpText
    // A pressed button sinks slightly; hover only changes colour so borders stay pixel-aligned.
    scale: control.down ? 0.97 : 1.0
    onToggled: {
        if (control.mirrorsState)
            control.toggle();
    }

    // Listens to the signal rather than the handler property: usage sites declare their own
    // onClicked, which would replace a handler declared here, but the signal fires regardless.
    Connections {
        target: control

        function onClicked() {
            if (control.focusTarget)
                Qt.callLater(() => control.focusTarget.forceActiveFocus());
        }
    }

    Behavior on scale {
        NumberAnimation {
            duration: Theme.animationFast
            easing.type: Easing.OutCubic
        }
    }

    contentItem: Text {
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: control.text
        // Checked wins over disabled: while busy disables the row, the active mode must
        // stay readable instead of collapsing into the disabled grey.
        color: control.checked ? control.textColor : (!control.enabled ? Theme.disabledText : (control.prominent ? Theme.inverseText : control.textColor))
        font.pixelSize: Theme.fontBody
        font.weight: control.prominent || control.checked ? Font.DemiBold : Font.Normal
        elide: Text.ElideRight
    }

    // A checked toggle takes the same accent-tinted fill as the mode chips; its fill holds
    // steady under hover and press so the selection never flickers.
    background: Rectangle {
        radius: Theme.radiusMedium
        color: {
            // Checked wins over disabled: while busy disables the row, the active mode must
            // stay readable instead of collapsing into the disabled grey.
            if (control.checked)
                return Theme.controlChecked;
            if (!control.enabled)
                return Theme.disabledPanel;
            if (control.prominent)
                return control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accentFill);
            if (control.flat)
                return control.down ? Theme.fluentPressed : (control.hovered ? Theme.fluentHover : "transparent");
            return control.down ? Theme.controlPressed : (control.hovered ? Theme.controlHover : Theme.control);
        }
        // No resting outline: the fill carries the state, the ring is reserved for keyboard focus.
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focus

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationBase
                easing.type: Easing.OutCubic
            }
        }
    }

    VcsToolTip {
        visible: control.hovered && control.helpText.length > 0
        text: control.helpText
    }
}
