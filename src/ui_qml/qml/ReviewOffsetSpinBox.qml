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

    // The keyboard focus ring of the U12 contract (VcsTheme.js): Tab and Backtab show
    // the 2 px Theme.focus ring, a mouse click does not. An editable Qt Quick SpinBox
    // forwards every real focus arrival to its TextInput editor
    // (QQuickAbstractSpinBox::handleFocusInEvent), and a Control is not a FocusScope,
    // so while the editor holds the focus the control's own activeFocus and
    // visualFocus read false - a bare `visualFocus` ring would never light here. The
    // ring therefore follows the editor's activeFocus together with the control's
    // focusReason, which QQuickControl keeps updated for editor focus changes: only
    // keyboard reasons (Tab, Backtab, Shortcut) turn it on. visualFocus covers a
    // non-editable configuration, where focus rests on the control itself.
    readonly property bool keyboardFocusRing: control.visualFocus || (control.contentItem.activeFocus && (control.focusReason === Qt.TabFocusReason || control.focusReason === Qt.BacktabFocusReason || control.focusReason === Qt.ShortcutFocusReason))

    from: -16
    to: 16
    editable: true
    implicitWidth: 124
    implicitHeight: 34

    // The stock Basic indicators are 40 px wide on each side and reserve 46 px of
    // padding on both, which collapses a 76 px control's editable text to zero width -
    // the number was neither visible nor clickable. Two side-by-side 32 x 32 step
    // buttons (Theme.hitAreaMinimum, so a 100% DPI pointer still lands on them) keep a
    // 42 px text area: 6 + 42 + 4 + 32 + 4 + 32 + 4 = 124.
    leftPadding: 6
    rightPadding: 76

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
        clip: width < implicitWidth
    }

    // The spin box maps presses inside these items onto up/down itself, so the items
    // carry visuals only. Each is a 32 x 32 hit target (Theme.hitAreaMinimum); two of
    // them cannot stack inside the 34 px height, so they sit side by side on the right
    // with a 4 px gap. `enabled` is each item's own, which the spin box clears at the
    // matching range bound.
    up.indicator: Item {
        x: control.width - width - 4
        y: (control.height - height) / 2
        width: 32
        height: 32

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: Theme.radiusMicro
            color: control.up.pressed ? Theme.fluentPressed : Theme.fluentHover
            visible: control.up.pressed || control.up.hovered
        }

        Text {
            anchors.centerIn: parent
            text: "+"
            color: parent.enabled ? control.textColor : control.mutedTextColor
            font.pixelSize: Theme.fontSecondary
        }
    }

    down.indicator: Item {
        x: control.width - 2 * width - 8
        y: (control.height - height) / 2
        width: 32
        height: 32

        Rectangle {
            anchors.fill: parent
            anchors.margins: 2
            radius: Theme.radiusMicro
            color: control.down.pressed ? Theme.fluentPressed : Theme.fluentHover
            visible: control.down.pressed || control.down.hovered
        }

        Text {
            anchors.centerIn: parent
            text: "\u2212"
            color: parent.enabled ? control.textColor : control.mutedTextColor
            font.pixelSize: Theme.fontSecondary
        }
    }

    background: Rectangle {
        radius: Theme.radiusSmall
        color: control.hovered ? Theme.controlHover : Theme.control
        // keyboardFocusRing (not activeFocus) so a mouse click leaves no ring behind -
        // the contract in VcsTheme.js; the ring colour matches ReviewActionButton's.
        border.width: control.keyboardFocusRing ? 2 : 1
        border.color: control.keyboardFocusRing ? Theme.focus : "transparent"

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
    }
}
