pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme

Item {
    id: control

    // The host registers ComparisonSurface dynamically, so qmllint cannot prove
    // its QQuickItem inheritance even though the runtime type is a QQuickItem.
    required property var surfaceItem
    // E1: wipe split axis. 0 = Vertical (left/right split along X, vertical handle bar),
    // 1 = Horizontal (top/bottom split along Y, horizontal handle bar). Must match the
    // surface's wipeOrientation.
    property int orientation: 0
    property real position: 0.5
    readonly property bool horizontal: orientation === 1
    readonly property real splitLogicalX: surfaceItem && typeof surfaceItem.wipeSplitLogicalX === "number" ? surfaceItem.wipeSplitLogicalX : surfaceItem.width * position
    readonly property real splitLogicalY: surfaceItem && typeof surfaceItem.wipeSplitLogicalY === "number" ? surfaceItem.wipeSplitLogicalY : surfaceItem.height * position

    signal positionRequested(real position)

    objectName: "wipeHandle"
    // Vertical: a full-height 52 px column centred on the split. Horizontal: a full-width
    // 52 px row centred on the split. Same hit minimum, mirrored on the other axis.
    x: surfaceItem.x + (horizontal ? 0 : Math.round(splitLogicalX) - width / 2)
    y: surfaceItem.y + (horizontal ? Math.round(splitLogicalY) - height / 2 : 0)
    width: horizontal ? surfaceItem.width : 52
    height: horizontal ? 52 : surfaceItem.height

    function updatePosition(sceneX, sceneY) {
        const point = surfaceItem.mapFromItem(null, sceneX, sceneY);
        if (horizontal) {
            if (surfaceItem && typeof surfaceItem.wipePositionForLogicalY === "function")
                positionRequested(surfaceItem.wipePositionForLogicalY(point.y));
            else
                positionRequested(Math.max(0, Math.min(1, point.y / Math.max(1, surfaceItem.height))));
        } else {
            if (surfaceItem && typeof surfaceItem.wipePositionForLogicalX === "function")
                positionRequested(surfaceItem.wipePositionForLogicalX(point.x));
            else
                positionRequested(Math.max(0, Math.min(1, point.x / Math.max(1, surfaceItem.width))));
        }
    }

    // A faint dark halo keeps the white split line visible over bright footage.
    Rectangle {
        width: control.horizontal ? parent.width : 4
        height: control.horizontal ? 4 : parent.height
        color: Theme.lineHalo
        anchors.centerIn: parent
    }

    Rectangle {
        id: rail

        objectName: "wipeRail"
        width: control.horizontal ? parent.width : 2
        height: control.horizontal ? 2 : parent.height
        color: drag.active || hover.hovered ? Theme.accent : Theme.inverseText
        opacity: drag.active || hover.hovered ? 1.0 : 0.8
        anchors.centerIn: parent

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
    }

    Rectangle {
        id: knob
        objectName: "wipeKnob"
        // Vertical wipe: 20 px wide, 84 px tall grip. Horizontal wipe: mirrored - 84 px
        // wide, 20 px tall - so the grip bars stay perpendicular to the split line and the
        // hit area keeps the same 20 px minimum on the drag axis.
        width: control.horizontal ? 84 : 20
        height: control.horizontal ? 20 : 84
        radius: Theme.radiusMedium
        // No hover scale: a scaled edge and 2 px grip bars land on fractional pixels and blur.
        color: drag.active ? Theme.controlChecked : (hover.hovered ? Theme.controlHover : Theme.raisedPanel)
        border.width: 1.5
        border.color: drag.active || hover.hovered ? Theme.accent : Theme.mutedText
        anchors.centerIn: parent

        Behavior on color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }
        Behavior on border.color {
            ColorAnimation {
                duration: Theme.animationFast
            }
        }

        // Grip: two short bars perpendicular to the split line read as "drag me" without
        // the cramped arrow glyphs. Vertical wipe: a side-by-side column of vertical bars;
        // horizontal wipe: a stacked row of horizontal bars. Two sibling layouts avoid the
        // Grid rows*columns transient warning during the axis flip.
        Column {
            visible: !control.horizontal
            spacing: Theme.spaceMicro
            anchors.centerIn: parent

            Rectangle {
                width: 2
                height: 20
                radius: Math.min(width, height) / 2
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }

            Rectangle {
                width: 2
                height: 20
                radius: Math.min(width, height) / 2
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }
        }

        Row {
            visible: control.horizontal
            spacing: Theme.spaceMicro
            anchors.centerIn: parent

            Rectangle {
                width: 20
                height: 2
                radius: Math.min(width, height) / 2
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }

            Rectangle {
                width: 20
                height: 2
                radius: Math.min(width, height) / 2
                color: drag.active || hover.hovered ? Theme.primaryText : Theme.secondaryText
            }
        }
    }

    DragHandler {
        id: drag

        target: null
        xAxis.enabled: !control.horizontal
        yAxis.enabled: control.horizontal
        onCentroidChanged: {
            if (active)
                control.updatePosition(centroid.scenePosition.x, centroid.scenePosition.y);
        }
    }

    TapHandler {
        acceptedButtons: Qt.LeftButton
        exclusiveSignals: TapHandler.DoubleTap
        onDoubleTapped: control.positionRequested(0.5)
    }

    HoverHandler {
        id: hover

        // SizeHorCursor / SizeVerCursor map to the standard Windows IDC_SIZEWE /
        // IDC_SIZENS cursors. SplitHCursor is a Qt-private pixmap cursor on Windows and hits
        // qpixmap_win.cpp:200 when the cursor image resources are missing from a deploy.
        cursorShape: control.horizontal ? Qt.SizeVerCursor : Qt.SizeHorCursor
    }
}
