pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

ToolButton {
    id: control

    property string helpText: ""
    property url iconSource: ""
    property int iconExtent: 20
    property int labelPixelSize: 16
    property real controlRadius: Theme.radiusMedium
    property bool prominent: false
    property int toolTipDelay: 500

    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    // Tool buttons sit on toolbars and the player OSC, so they rest without a fill and only
    // show a surface on hover; the prominent variant is the one filled action in its group.
    flat: true
    activeFocusOnTab: true
    hoverEnabled: true
    Accessible.name: helpText.length > 0 ? helpText.split("\n")[0] : text
    Accessible.description: helpText

    contentItem: Item {
        Image {
            anchors.centerIn: parent
            visible: control.iconSource.toString().length > 0
            source: control.iconSource
            sourceSize.width: control.iconExtent
            sourceSize.height: control.iconExtent
            fillMode: Image.PreserveAspectFit
            opacity: !control.enabled ? 0.32 : (control.prominent || control.hovered ? 1.0 : 0.82)
            scale: control.down ? 0.9 : 1.0

            Behavior on scale {
                NumberAnimation {
                    duration: Theme.animationFast
                    easing.type: Easing.OutCubic
                }
            }
            Behavior on opacity {
                NumberAnimation {
                    duration: Theme.animationBase
                    easing.type: Easing.OutCubic
                }
            }
        }

        Text {
            anchors.fill: parent
            visible: control.iconSource.toString().length === 0
            text: control.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            // Checked wins over disabled: while busy disables the row, the active mode must
            // stay readable instead of collapsing into the disabled grey.
            color: control.checked ? Theme.primaryText : (!control.enabled ? Theme.disabledText : (control.prominent ? Theme.inverseText : Theme.primaryText))
            font.pixelSize: control.labelPixelSize
            font.weight: Font.DemiBold
            elide: Text.ElideRight
        }
    }

    background: Rectangle {
        radius: control.controlRadius
        color: {
            // Checked wins over disabled: while busy disables the row, the active mode must
            // stay readable instead of collapsing into the disabled grey.
            if (control.checked)
                return Theme.controlChecked;
            if (!control.enabled)
                return control.flat ? "transparent" : Theme.disabledPanel;
            if (control.prominent)
                return control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accentFill);
            if (control.flat)
                return control.down ? Theme.fluentPressed : (control.hovered ? Theme.fluentHover : "transparent");
            return control.down ? Theme.controlPressed : (control.hovered ? Theme.controlHover : Theme.control);
        }
        // Apple-style flat controls carry no resting outline: hover and press are a fill, and
        // the only line the user sees is the keyboard focus ring.
        border.width: control.visualFocus ? 2 : 0
        border.color: Theme.focus

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationBase
                easing.type: Easing.OutCubic
            }
        }
        Behavior on border.color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
    }

    VcsToolTip {
        visible: control.hovered && control.helpText.length > 0
        text: control.helpText
        delay: control.toolTipDelay
    }
}
