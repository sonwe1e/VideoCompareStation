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
    property color focusColor: Theme.accent
    property color highlightColor: Theme.controlPressed
    property color highlightedTextColor: Theme.primaryText

    implicitWidth: Math.max(implicitBackgroundWidth + leftInset + rightInset, implicitContentWidth + leftPadding + rightPadding)
    implicitHeight: Math.max(implicitBackgroundHeight + topInset + bottomInset, implicitContentHeight + topPadding + bottomPadding, implicitIndicatorHeight + topPadding + bottomPadding)
    leftPadding: control.mirrored ? 32 : 12
    rightPadding: control.mirrored ? 12 : 32
    activeFocusOnTab: true
    hoverEnabled: true
    font.pixelSize: 12
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
            radius: 5
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
            font.pixelSize: 12
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
        radius: 6
        color: control.enabled ? control.panelColor : control.disabledPanelColor
        border.width: control.activeFocus ? 2 : 1
        border.color: control.activeFocus ? control.focusColor : (control.hovered ? Theme.borderHover : control.borderColor)
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
            radius: 8
            color: Theme.menu
            border.width: 1
            border.color: Theme.menuBorder
        }
    }
}
