pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Item {
    id: control

    // The host registers ComparisonSurface dynamically, so qmllint cannot prove
    // its QQuickItem inheritance even though the runtime type is a QQuickItem.
    required property var surfaceItem
    property real position: 0.5
    readonly property real splitLogicalX: surfaceItem && typeof surfaceItem.wipeSplitLogicalX === "number" ? surfaceItem.wipeSplitLogicalX : surfaceItem.width * position

    signal positionRequested(real position)

    objectName: "wipeHandle"
    x: surfaceItem.x + Math.round(splitLogicalX) - width / 2
    y: surfaceItem.y
    width: 52
    height: surfaceItem.height

    function updatePosition(sceneX) {
        const point = surfaceItem.mapFromItem(null, sceneX, 0);
        if (surfaceItem && typeof surfaceItem.wipePositionForLogicalX === "function")
            positionRequested(surfaceItem.wipePositionForLogicalX(point.x));
        else
            positionRequested(Math.max(0, Math.min(1, point.x / Math.max(1, surfaceItem.width))));
    }

    // A faint dark halo keeps the white split line visible over bright footage.
    Rectangle {
        width: 4
        height: parent.height
        color: Theme.lineHalo
        anchors.centerIn: parent
    }

    Rectangle {
        id: rail

        objectName: "wipeRail"
        width: 2
        height: parent.height
        color: drag.active || hover.hovered ? Theme.accent : Theme.inverseText
        opacity: drag.active || hover.hovered ? 1.0 : 0.8
        anchors.centerIn: parent

        Behavior on color {
            ColorAnimation {
                duration: 120
            }
        }
    }

    Rectangle {
        id: knob
        objectName: "wipeKnob"
        width: 20
        height: 84
        radius: 10
        // No hover scale: a scaled edge and 2 px grip bars land on fractional pixels and blur.
        color: drag.active ? Theme.controlChecked : (hover.hovered ? Theme.controlHover : Theme.raisedPanel)
        border.width: 1.5
        border.color: drag.active || hover.hovered ? Theme.accent : Theme.mutedText
        anchors.centerIn: parent

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

        // Grip: two short bars read as "drag me" without the cramped arrow glyphs.
        Row {
            spacing: 4
            anchors.centerIn: parent

            Rectangle {
                width: 2
                height: 20
                radius: 1
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }

            Rectangle {
                width: 2
                height: 20
                radius: 1
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }
        }
    }

    DragHandler {
        id: drag

        target: null
        xAxis.enabled: true
        yAxis.enabled: false
        onCentroidChanged: {
            if (active)
                control.updatePosition(centroid.scenePosition.x);
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        exclusiveSignals: TapHandler.DoubleTap
        onDoubleTapped: control.positionRequested(0.5)
    }

    HoverHandler {
        id: hover

        // SizeHorCursor maps to the standard Windows IDC_SIZEWE cursor. SplitHCursor is a
        // Qt-private pixmap cursor on Windows and hits qpixmap_win.cpp:200 when the cursor
        // image resources are missing from a deploy.
        cursorShape: Qt.SizeHorCursor
    }
}
