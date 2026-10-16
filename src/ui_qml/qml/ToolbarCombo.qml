pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Templates as T
import "VcsTheme.js" as Theme

T.ComboBox {
    id: control

    property bool blocksGlobalMediaShortcuts: true
    property bool popupInputContext: popup.visible
    property color panelColor: Theme.raisedPanel
    property color disabledPanelColor: Theme.disabledPanel
    property color textColor: Theme.primaryText
    property color disabledTextColor: Theme.mutedText
    property color borderColor: Theme.border
    property color focusColor: Theme.focus
    property color highlightColor: Theme.raisedPanelHover
    property color highlightedTextColor: Theme.primaryText

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding, implicitIndicatorHeight + topPadding + bottomPadding)
    leftPadding: control.mirrored ? 32 : 12
    rightPadding: control.mirrored ? 12 : 32
    activeFocusOnTab: true
    hoverEnabled: true
    font.pixelSize: Theme.fontSecondary
    Accessible.role: Accessible.ComboBox

    delegate: T.ItemDelegate {
        id: delegateControl

        required property int index
        property bool blocksGlobalMediaShortcuts: true
        property bool popupInputContext: true

        objectName: "toolbarComboDelegate-" + index
        width: ListView.view ? ListView.view.width : control.width
        implicitHeight: 34
        hoverEnabled: true
        highlighted: control.highlightedIndex === index
        Accessible.name: control.textAt(index)

        contentItem: Text {
            objectName: "toolbarComboDelegateText-" + delegateControl.index
            text: control.textAt(delegateControl.index)
            color: control.enabled ? (delegateControl.highlighted || delegateControl.hovered || control.currentIndex === delegateControl.index ? control.highlightedTextColor : control.textColor) : control.disabledTextColor
            font.family: control.font.family
            font.pixelSize: control.font.pixelSize
            font.weight: control.currentIndex === delegateControl.index ? Font.DemiBold : Font.Normal
            verticalAlignment: Text.AlignVCenter
            elide: Text.ElideRight
        }

        background: Rectangle {
            objectName: "toolbarComboDelegateBackground-" + delegateControl.index
            radius: Theme.radiusSmall
            color: delegateControl.highlighted || delegateControl.hovered || control.currentIndex === delegateControl.index ? control.highlightColor : control.panelColor
        }
    }

    indicator: Item {
        x: control.mirrored ? 4 : control.width - width - 4
        y: (control.height - height) / 2
        implicitWidth: 24
        implicitHeight: 24

        Text {
            anchors.centerIn: parent
            text: "\u25be"
            color: control.enabled ? control.textColor : control.disabledTextColor
            font.pixelSize: Theme.fontSecondary
        }
    }

    contentItem: Text {
        objectName: "toolbarComboDisplayText"
        text: control.displayText
        color: control.enabled ? control.textColor : control.disabledTextColor
        font: control.font
        verticalAlignment: Text.AlignVCenter
        elide: Text.ElideRight
    }

    background: Rectangle {
        objectName: "toolbarComboBackground"
        implicitWidth: 122
        implicitHeight: 34
        radius: Theme.radiusMedium
        color: {
            if (!control.enabled)
                return control.disabledPanelColor;
            return control.hovered ? Theme.controlHover : Theme.control;
        }
        // Flat by default; the keyboard focus ring is the only outline. No colour animation:
        // a disabled combo must snap to its disabled fill so the state change is observable
        // in the same frame the caller sets it. visualFocus (not activeFocus) so a mouse
        // click leaves no ring behind - the contract in VcsTheme.js.
        border.width: control.visualFocus ? 2 : 1
        border.color: control.visualFocus ? control.focusColor : "transparent"
    }

    popup: T.Popup {
        id: comboPopup

        objectName: "toolbarComboPopup"
        y: control.height + 3
        width: control.width
        implicitHeight: contentItem.implicitHeight + topPadding + bottomPadding
        height: Math.min(implicitHeight, 260)
        topMargin: 6
        bottomMargin: 6
        padding: 4
        closePolicy: T.Popup.CloseOnEscape | T.Popup.CloseOnPressOutsideParent

        contentItem: ListView {
            objectName: "toolbarComboList"
            property bool blocksGlobalMediaShortcuts: true
            property bool popupInputContext: true

            clip: true
            implicitHeight: contentHeight
            model: control.delegateModel
            currentIndex: control.highlightedIndex
            highlightMoveDuration: 0
            boundsBehavior: Flickable.StopAtBounds
        }

        background: Rectangle {
            objectName: "toolbarComboPopupBackground"
            radius: Theme.radiusLarge
            color: Theme.menu
            border.width: 1
            border.color: Theme.menuBorder
        }
    }
}
