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

    // Keyboard twin of updatePosition: same [0, 1] clamp as ReviewActions.moveWipe, applied to
    // the live `position` both hosts keep bound to the shell (ComparisonViewport binds it to
    // control.wipePosition, ImageWorkspace to imageReview.wipePosition), so a nudge never
    // restarts from a stale offset.
    function stepPosition(delta) {
        control.positionRequested(Math.max(0, Math.min(1, control.position + delta)));
    }

    // UX-15 (handle path): the handle is keyboard-adjustable in its own right, not only a drag
    // affordance. The global Alt+arrows (ReviewShortcuts / ImageWorkspace) already move the
    // split while focus lives elsewhere; this path covers the handle-focused case with the same
    // semantics - 1% steps, Shift for 5%, clamped, and gated to the handle's own axis so the
    // wrong arrow pair cannot drag a split the on-screen rail no longer shows. The pointer
    // handlers above never take focus, so `activeFocus` can only arrive through Tab: the same
    // honest-ring contract as the transport chips (UX-06). Qt 6.11's Accessible attached type
    // has no numeric value property, so the live readout rides in the description.
    Accessible.role: Accessible.Slider
    Accessible.name: control.horizontal ? qsTr("水平分割线位置") : qsTr("垂直分割线位置")
    readonly property int accessiblePositionPercent: Math.round(control.position * 100)
    Accessible.description: control.horizontal ? qsTr("上下切分 · 当前 %1%。上下方向键每次移动 1%，按住 Shift 移动 5%；双击回到正中。").arg(accessiblePositionPercent) : qsTr("左右切分 · 当前 %1%。左右方向键每次移动 1%，按住 Shift 移动 5%；双击回到正中。").arg(accessiblePositionPercent)
    Accessible.focusable: true
    activeFocusOnTab: true

    Keys.onPressed: event => {
        const step = (event.modifiers & Qt.ShiftModifier) !== 0 ? 0.05 : 0.01;
        let delta = 0;
        if (!control.horizontal) {
            if (event.key === Qt.Key_Left)
                delta = -step;
            else if (event.key === Qt.Key_Right)
                delta = step;
        } else if (event.key === Qt.Key_Up) {
            delta = -step;
        } else if (event.key === Qt.Key_Down) {
            delta = step;
        }
        if (delta !== 0) {
            event.accepted = true;
            control.stepPosition(delta);
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

        // 2 px focus ring, keyboard arrival only (same contract as RangeChip, UX-06): the
        // pointer handlers on the root never take focus, so a mouse click cannot light it.
        Rectangle {
            objectName: "wipeFocusRing"
            anchors.fill: parent
            anchors.margins: -2
            radius: knob.radius + 2
            color: "transparent"
            border.width: 2
            border.color: Theme.focus
            visible: control.activeFocus
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
