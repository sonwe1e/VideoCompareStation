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
    // `checked` mirrors state the caller owns (a mode, a tool). Qt flips `checked` itself
    // before onClicked, so re-clicking the selected option would drop its highlight while the
    // state stays selected; undo that flip and leave the caller's binding in charge.
    property bool mirrorsState: false

    implicitWidth: Math.max(112, contentItem.implicitWidth + 34)
    implicitHeight: 40
    leftPadding: 17
    rightPadding: 17
    activeFocusOnTab: true
    // A pressed button sinks slightly; hover only changes colour so borders stay pixel-aligned.
    scale: control.down ? 0.98 : 1.0
    onToggled: {
        if (control.mirrorsState)
            control.toggle();
    }

    Behavior on scale {
        NumberAnimation {
            duration: 80
            easing.type: Easing.OutQuad
        }
    }

    contentItem: Text {
        horizontalAlignment: Text.AlignHCenter
        verticalAlignment: Text.AlignVCenter
        text: control.text
        color: !control.enabled ? Theme.disabledText : (control.prominent ? Theme.inverseText : control.textColor)
        font.pixelSize: 13
        font.weight: Font.DemiBold
        elide: Text.ElideRight
    }

    // A checked toggle takes the same accent-tinted fill as the mode chips; its fill holds
    // steady under hover and press so the selection never flickers.
    background: Rectangle {
        radius: Theme.radiusMedium
        color: {
            if (!control.enabled)
                return Theme.disabledPanel;
            if (control.prominent)
                return control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accentFill);
            if (control.checked)
                return Theme.controlChecked;
            return control.down ? Theme.controlPressed : (control.hovered ? Theme.controlHover : Theme.control);
        }
        border.width: control.activeFocus ? 2 : 1
        border.color: {
            if (control.activeFocus)
                return control.prominent || control.checked ? Theme.strongFocus : control.accentColor;
            if (!control.enabled)
                return Theme.disabledBorder;
            if (control.prominent || control.checked)
                return control.hovered ? Theme.focus : Theme.accent;
            return control.hovered ? Theme.borderHover : Theme.controlBorder;
        }

        Behavior on color {
            ColorAnimation {
                duration: 100
            }
        }
    }

    VcsToolTip {
        visible: control.hovered && control.helpText.length > 0
        text: control.helpText
    }
}
