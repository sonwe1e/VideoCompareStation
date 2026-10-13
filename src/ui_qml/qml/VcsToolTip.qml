pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import "VcsTheme.js" as Theme

ToolTip {
    id: control

    objectName: "vcsToolTip"
    delay: 500
    timeout: 6000
    padding: 10
    margins: 8
    popupType: Popup.Item

    contentItem: Text {
        text: control.text
        color: Theme.primaryText
        font.pixelSize: 12
        wrapMode: Text.Wrap
    }

    background: Rectangle {
        objectName: "vcsToolTipBackground"
        color: Theme.menu
        radius: Theme.radiusSmall
        border.width: 0
    }
}
