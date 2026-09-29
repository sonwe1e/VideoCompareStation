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
    property real controlRadius: 6
    property bool prominent: false
    property int toolTipDelay: 500

    implicitWidth: 34
    implicitHeight: 34
    padding: 0
    // Tool buttons sit on toolbars and the player OSC, so they rest without a fill and only
    // show a surface on hover; the prominent variant is the one filled action in its group.
    flat: true
    activeFocusOnTab: true
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
            opacity: !control.enabled ? 0.35 : (control.prominent || control.hovered ? 1.0 : 0.86)
            scale: control.down ? 0.92 : 1.0

            Behavior on scale {
                NumberAnimation {
                    duration: 80
                    easing.type: Easing.OutQuad
                }
            }
            Behavior on opacity {
                NumberAnimation {
                    duration: 120
                }
            }
        }

        Text {
            anchors.fill: parent
            visible: control.iconSource.toString().length === 0
            text: control.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            color: !control.enabled ? Theme.disabledText : (control.prominent ? Theme.inverseText : Theme.primaryText)
            font.pixelSize: control.labelPixelSize
            font.weight: Font.DemiBold
        }
    }

    background: Rectangle {
        radius: control.controlRadius
        color: {
            if (!control.enabled)
                return control.flat ? "transparent" : Theme.disabledPanel;
            if (control.prominent)
                return control.down ? Theme.accentPressed : (control.hovered ? Theme.accentHover : Theme.accentFill);
            if (control.flat)
                return control.down ? Theme.fluentPressed : (control.hovered ? Theme.fluentHover : "transparent");
            return control.down ? Theme.controlPressed : (control.hovered ? Theme.controlHover : Theme.control);
        }
        border.width: control.activeFocus ? 2 : 1
        border.color: {
            if (control.activeFocus)
                return control.prominent ? Theme.strongFocus : Theme.focus;
            if (!control.enabled)
                return control.flat ? "transparent" : Theme.disabledBorder;
            if (control.prominent)
                return control.hovered ? Theme.focus : Theme.accent;
            if (control.flat)
                return control.hovered ? Theme.subtleBorder : "transparent";
            return Theme.controlBorder;
        }

        Behavior on color {
            ColorAnimation {
                duration: 120
            }
        }
        Behavior on border.color {
            ColorAnimation {
                duration: 120
            }
        }
    }

    VcsToolTip {
        visible: control.hovered && control.helpText.length > 0
        text: control.helpText
        delay: control.toolTipDelay
    }
}
