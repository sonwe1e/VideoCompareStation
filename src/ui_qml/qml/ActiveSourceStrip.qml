pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import "VcsTheme.js" as Theme

Rectangle {
    id: control

    required property var sourcesModel
    required property int sourceCount
    required property bool singleMode
    required property int canonicalSourceIndex
    required property string canonicalSourceIdentity
    required property int referenceSourceIndex
    required property string referenceSourceIdentity
    required property var pendingSourceIdentities
    required property var sourceIdentities
    property color panelColor: Theme.panel
    property color borderColor: Theme.border
    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText
    property color mutedTextColor: Theme.mutedText

    signal addRequested
    signal removeRequested(string sourceIdentity)
    signal referenceRequested(string sourceIdentity)
    // V02 "替换文件": the chip's overflow menu asks for a file replacement of one source;
    // Main owns the dialog and the staged re-open with the displayed position preserved.
    signal replaceRequested(string sourceIdentity)
    // I01 "拖到明确源卡片": dropping a single video on a chip replaces exactly that source
    // through the same staged swap the chip menu uses. The drop names its target - the chip
    // under the cursor - so the replace can never silently retarget another slot.
    signal replaceDropped(string sourceIdentity, url url)
    signal viewerFocusRequested
    // V02 direct look: a plain click on the chip body enters (or exits) the full-canvas
    // direct look of that source through the P05 canvasFocus chain. The signal carries the
    // session slot; Main maps it onto the shell request, toggling Off for the focused slot.
    signal directLookRequested(int sourceSlot)

    property int openMenuCount: 0
    property bool returnViewerFocusAfterClose: false
    readonly property bool anyMenuOpen: openMenuCount > 0
    // C2. The order the user sees, and the sources they have selected. Both are keyed by frozen
    // source identity so that arranging the list cannot reach media truth - the chip keeps its
    // sourceId, and with it its A/B/C role, its accent colour and its pairing. V02: the strip
    // no longer offers a drag reorder - the picture panels follow the session slot order and a
    // chip-only reorder would be a label change masquerading as a layout change - so the
    // display order stays whatever the shell says and this layout only renders it.
    property var displayOrder: []
    property var selectedIdentities: []
    readonly property bool anySelected: control.selectedIdentities.length > 0
    // V02: frozen identity of the source the whole canvas is focused on ("" = comparison
    // view). Pure mirror of the shell's canvasFocusIdentity; the chip only highlights itself.
    property string canvasFocusIdentity: ""
    // V02: batch management (multi-select remove) is an explicit mode, entered through the
    // 管理 toggle. Outside the mode a plain chip click is a direct look; inside it the click
    // toggles the management selection, so selection can never hijack the ordinary click.
    property bool manageSelection: false
    signal selectionToggled(string sourceIdentity)
    signal selectionCleared
    signal removeSelectedRequested
    readonly property real chipSpacing: 7

    // Session change detector: the shell clears its selection list whenever the active source set
    // changes, but the explicit management mode is strip-local. Without leaving it, an ordinary
    // chip click after an open/replace/append would keep selecting instead of direct-looking.
    readonly property string sourceSetSignature: control.displayOrder.join("\u0001") + "|" + control.sourceCount
    onSourceSetSignatureChanged: control.manageSelection = false

    // V02: the drag reorder is gone (see the displayOrder note above). Positions stay
    // bindings over the display order the shell publishes, so nothing needs scheduling.

    // Positions are bindings, not an imperative pass. A Row would place its children in model order,
    // and the display order is exactly what may differ from it, so each chip computes its own x from
    // the order and the widths ahead of it. That matters because a chip's width is derived from its
    // label and settles a frame or more after the delegate appears: an imperative reposition
    // scheduled from a signal runs once, too early, and leaves every chip at a position computed
    // from stale widths. A binding re-evaluates on its own whenever the order or any width changes.
    function chipStartX(position) {
        let cursor = 12;
        for (let index = 0; index < position; ++index) {
            const preceding = chipAt(control.displayOrder[index]);
            if (preceding)
                cursor += preceding.width + control.chipSpacing;
        }
        return cursor;
    }

    // The chip showing one identity, or null when that source has no chip yet. A chip whose identity
    // is missing from the display order falls back to model order so it is still on screen rather
    // than stacked on top of another chip.
    function chipAt(identity) {
        const wanted = String(identity);
        let fallback = null;
        for (let index = 0; index < chips.children.length; ++index) {
            const child = chips.children[index];
            if (child.objectName === "addSourceChipButton" || child.objectName === "removeSelectedChipButton" || child.objectName === "manageSourcesButton")
                continue;
            if (child.resolvedSourceIdentity === wanted)
                return child;
            if (!fallback)
                fallback = child;
        }
        return fallback;
    }

    // Right-click entry point for one chip's source menu. The position is the chip-local
    // press point, so the menu opens where the user asked for it. A chip that is
    // mid-replacement/reopen has no stable identity yet, and the overflow button is
    // disabled in that state for exactly the same reason, so the right click is refused
    // the same way instead of opening a menu whose actions would all be inert.
    function openSourceMenu(chip, position) {
        if (!control.visible || !chip || chip.pending || chip.resolvedSourceIdentity.length === 0)
            return false;
        // The menu belongs to the chip that was right-clicked, so the lookup goes through
        // the chip itself: `sourceMenu` is a per-delegate id and is not in this scope.
        chip.sourceMenuControl.popup(chips, chip.mapToItem(chips, position.x, position.y));
        return true;
    }

    // The trailing buttons follow the chips; each one starts where the previous ended.
    readonly property real trailingButtonX: {
        let cursor = 12;
        for (let index = 0; index < chips.children.length; ++index) {
            const child = chips.children[index];
            if (child.objectName === "addSourceChipButton" || child.objectName === "removeSelectedChipButton" || child.objectName === "manageSourcesButton")
                continue;
            if (child.width > 0.0)
                cursor += child.width + control.chipSpacing;
        }
        return cursor;
    }

    // V02 click semantics. Outside the management mode a plain click on the chip body is the
    // direct look of that source (enter or exit); only inside the explicit 管理 mode does the
    // same click toggle the management selection. The chip's overflow button and the add and
    // manage buttons are children and consume their own clicks first, so this handler sees
    // presses on the chip body alone.
    function chipClicked(chip) {
        // A chip mid-replacement/reopen has no stable identity to focus yet; every sibling action
        // already refuses while pending, and a direct look on a torn-down source would either
        // focus the old identity or be dropped when the new topology lands.
        if (chip.pending || chip.resolvedSourceIdentity.length === 0)
            return;
        if (control.manageSelection) {
            control.selectionToggled(chip.resolvedSourceIdentity);
            return;
        }
        control.directLookRequested(chip.sourceId);
    }

    objectName: "activeSourceStrip"
    height: sourceCount > 1 ? Theme.barRowHeight : 0
    visible: sourceCount > 1
    color: "transparent"
    border.color: "transparent"
    opacity: singleMode && !sourceHover.hovered ? 0.68 : 1.0

    Behavior on opacity {
        NumberAnimation {
            duration: Theme.animationBase
        }
    }

    HoverHandler {
        id: sourceHover
    }

    Item {
        id: chips

        anchors {
            fill: parent
            leftMargin: 12
            rightMargin: 12
            topMargin: 4
            bottomMargin: 4
        }

        Repeater {
            objectName: "activeSourceRepeater"
            model: control.sourcesModel

            delegate: Rectangle {
                id: chip

                objectName: "sourceChip-" + sourceId
                required property int sourceId
                required property string sourceIdentity
                required property string filename
                required property string parentLabel
                required property string fullPath
                required property bool changedOnDisk

                height: chips.height
                // Explicit label width: RowLayout.fillWidth children do not feed back into an
                // implicit width, so sum the pieces here to size the chip from the real text.
                readonly property real chipLabelPlainWidth: letterBadge.width + separatorOne.implicitWidth + filenameLabel.implicitWidth + 10
                readonly property real chipLabelTaggedWidth: chipLabelPlainWidth + (chip.isCanvasFocus ? directLookTag.implicitWidth + 10 : 0) + (chip.isReference ? referenceTag.implicitWidth + separatorTwo.implicitWidth + 10 : 0) + (chip.isTimelineMaster ? masterTag.implicitWidth + separatorThree.implicitWidth + 10 : 0)
                width: Math.min(280, Math.max(128, ((chip.isReference || chip.isTimelineMaster || chip.isCanvasFocus) ? chipLabelTaggedWidth : chipLabelPlainWidth) + (control.singleMode ? 24 : 84)))
                radius: Theme.radiusLarge
                readonly property string resolvedSourceIdentity: chip.sourceIdentity.length > 0 ? chip.sourceIdentity : (chip.sourceId >= 0 && chip.sourceId < control.sourceIdentities.length ? String(control.sourceIdentities[chip.sourceId]) : "")
                // D08: timeline master and comparison reference are independent projections.
                readonly property bool isTimelineMaster: chip.resolvedSourceIdentity.length > 0 ? chip.resolvedSourceIdentity === control.canonicalSourceIdentity : chip.sourceId === control.canonicalSourceIndex
                readonly property bool isReference: chip.resolvedSourceIdentity.length > 0 ? (control.referenceSourceIdentity.length > 0 ? chip.resolvedSourceIdentity === control.referenceSourceIdentity : chip.sourceId === control.referenceSourceIndex) : chip.sourceId === control.referenceSourceIndex
                readonly property bool pending: control.pendingSourceIdentities.indexOf(chip.resolvedSourceIdentity) >= 0 || requestQueued
                property bool requestQueued: false
                readonly property bool selected: control.manageSelection && control.selectedIdentities.indexOf(chip.resolvedSourceIdentity) >= 0
                // V02: this chip's source is the one the whole canvas is focused on. Highlight
                // only mirrors the shell state - the chip never owns focus.
                readonly property bool isCanvasFocus: chip.resolvedSourceIdentity.length > 0 && chip.resolvedSourceIdentity === control.canvasFocusIdentity
                readonly property int displayIndex: control.displayOrder.indexOf(chip.resolvedSourceIdentity)
                // The chip's own source menu, exposed so the strip-level right-click entry
                // can pop the menu of the chip that was actually clicked. `sourceMenu` is a
                // per-delegate id and is not in scope from the strip's own functions.
                readonly property var sourceMenuControl: sourceMenu
                readonly property var makeReferenceAction: useAsReferenceItem
                readonly property var replaceSourceAction: replaceSourceItem
                readonly property var removeSourceAction: removeSourceItem
                // The chip's place in the display order, computed as a binding so it follows the
                // order and the widths ahead of it. A chip the display order does not mention yet
                // falls to the end rather than sitting on top of another chip.
                readonly property real layoutX: chip.displayIndex >= 0 ? control.chipStartX(chip.displayIndex) : control.chipStartX(control.sourceCount)
                x: chip.layoutX

                readonly property color sourceAccent: Theme.sourceColor(chip.sourceId)
                readonly property color sourceBg: Theme.sourceBackground(chip.sourceId)
                readonly property color sourceBorder: Theme.sourceBorder(chip.sourceId)

                // Direct look wins the highlight: it names what the whole canvas is showing
                // right now, while selection is only a management-mode batch state.
                color: chip.isCanvasFocus ? chip.sourceBg : (chip.selected ? chip.sourceBg : (chip.isReference ? chip.sourceBg : Theme.raisedPanel))
                border.color: chip.isCanvasFocus ? chip.sourceAccent : (chip.selected ? chip.sourceAccent : (chip.isReference ? chip.sourceAccent : (chipHover.hovered ? chip.sourceBorder : "transparent")))
                border.width: (chip.isCanvasFocus || chip.selected || chip.isReference) ? 1.5 : 1

                // I01: one dropped video on this chip replaces this source. The visible
                // "松开替换" plate is the drop-target contract - the user sees which slot the
                // file lands in before committing, and dragging away cancels without staging
                // anything. Multi-file and non-URL drags are rejected here so they keep the
                // workspace-level drop semantics (open/append) instead.
                readonly property bool dropReplaceHovered: chipDropArea.containsDrag

                DropArea {
                    id: chipDropArea

                    objectName: "sourceChipDropArea-" + chip.sourceId
                    anchors.fill: parent
                    onEntered: drag => {
                        if (!drag.hasUrls || drag.urls.length !== 1)
                            drag.accepted = false;
                    }
                    onDropped: drop => {
                        drop.acceptProposedAction();
                        control.replaceDropped(chip.resolvedSourceIdentity, drop.urls[0]);
                    }
                }

                Rectangle {
                    objectName: "sourceChipDropHint-" + chip.sourceId
                    visible: chip.dropReplaceHovered
                    anchors.fill: parent
                    radius: Theme.radiusLarge
                    color: Theme.dropScrim
                    border.color: control.accentColor
                    border.width: 2

                    Text {
                        anchors.centerIn: parent
                        text: qsTr("松开替换 %1").arg(String.fromCharCode(65 + chip.sourceId))
                        color: control.textColor
                        font.pixelSize: Theme.fontBody
                        font.weight: Font.DemiBold
                    }
                }

                HoverHandler {
                    id: chipHover
                }

                TapHandler {
                    // Left button only. The chip's overflow button and the add button are children and
                    // consume their own clicks first, so this handler sees presses on the chip body
                    // alone and does not steal the menu from them.
                    acceptedButtons: Qt.LeftButton
                    gesturePolicy: TapHandler.DragThreshold

                    onTapped: function (eventPoint, button) {
                        control.chipClicked(chip);
                    }
                }

                // Right-click on the filename label opens the same source menu the overflow
                // button uses. The handler is declared with acceptedButtons: Qt.RightButton,
                // so a right press is invisible to the left-button handler above - it can
                // never fire the direct look, the management selection or a drag before the
                // menu appears. The menu itself is the one per-chip instance, so the two
                // entries cannot drift apart, and every action it carries is addressed by
                // the chip's frozen source identity (never by its display index or its
                // filename).
                TapHandler {
                    id: chipContextHandler

                    acceptedButtons: Qt.RightButton
                    gesturePolicy: TapHandler.DragThreshold

                    onTapped: function (eventPoint, button) {
                        control.openSourceMenu(chip, eventPoint.position);
                    }
                }

                function requestReference() {
                    if (chip.pending || chip.isReference || chip.resolvedSourceIdentity.length === 0)
                        return;
                    chip.requestQueued = true;
                    control.returnViewerFocusAfterClose = true;
                    sourceMenu.close();
                    Qt.callLater(() => {
                        control.referenceRequested(chip.resolvedSourceIdentity);
                        chip.requestQueued = false;
                    });
                }

                // V02: replace the file behind this slot. The identity disambiguates same-named
                // sources; Main stages the swap and re-opens with the displayed position kept.
                function requestReplacement() {
                    if (chip.pending || chip.resolvedSourceIdentity.length === 0)
                        return;
                    chip.requestQueued = true;
                    control.returnViewerFocusAfterClose = true;
                    sourceMenu.close();
                    Qt.callLater(() => {
                        control.replaceRequested(chip.resolvedSourceIdentity);
                        chip.requestQueued = false;
                    });
                }

                function requestRemoval() {
                    if (chip.pending || chip.resolvedSourceIdentity.length === 0)
                        return;
                    chip.requestQueued = true;
                    control.returnViewerFocusAfterClose = true;
                    sourceMenu.close();
                    Qt.callLater(() => {
                        control.removeRequested(chip.resolvedSourceIdentity);
                        chip.requestQueued = false;
                    });
                }

                RowLayout {
                    id: chipLabel

                    spacing: 5
                    anchors {
                        left: parent.left
                        leftMargin: 12
                        right: control.singleMode ? parent.right : chipControls.left
                        rightMargin: control.singleMode ? 12 : 6
                        verticalCenter: parent.verticalCenter
                    }

                    Rectangle {
                        id: letterBadge
                        implicitWidth: 18
                        implicitHeight: 18
                        Layout.alignment: Qt.AlignVCenter
                        radius: Theme.radiusMicro
                        color: chip.isReference ? chip.sourceAccent : Theme.fluentHover

                        Text {
                            id: letterLabel
                            anchors.centerIn: parent
                            text: String.fromCharCode(65 + chip.sourceId)
                            color: chip.isReference ? Theme.sourceInk : chip.sourceAccent
                            font.pixelSize: Theme.fontSecondary
                            font.weight: Font.Bold
                        }
                    }

                    Text {
                        id: separatorOne

                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontBody
                    }
                    // V02: the fourth per-source state, next to the timeline-master and
                    // reference-role tags - this chip's source is what the whole canvas is
                    // showing right now. Pure mirror of the shell's canvas focus.
                    Text {
                        id: directLookTag

                        objectName: "sourceDirectLookTag-" + chip.sourceId
                        visible: chip.isCanvasFocus
                        text: qsTr("直看")
                        color: chip.sourceAccent
                        font.pixelSize: Theme.fontSecondary
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("全画布直看")
                        Accessible.description: qsTr("整个画布正单独显示该源；单击卡片主体返回比较视图")

                        HoverHandler {
                            id: directLookTagHover
                        }

                        VcsToolTip {
                            visible: directLookTagHover.hovered
                            text: qsTr("全画布直看")
                        }
                    }
                    Text {
                        id: directLookSeparator

                        visible: chip.isCanvasFocus && (chip.isTimelineMaster || chip.isReference)
                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        id: masterTag

                        objectName: "sourceTimelineMasterTag-" + chip.sourceId
                        visible: chip.isTimelineMaster
                        text: qsTr("主时间线")
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontSecondary
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("时间线主源")
                        Accessible.description: qsTr("会话时间轴与帧号由它决定")

                        HoverHandler {
                            id: masterTagHover
                        }

                        VcsToolTip {
                            visible: masterTagHover.hovered
                            text: qsTr("时间线主源")
                        }
                    }
                    Text {
                        id: separatorThree

                        visible: chip.isTimelineMaster && chip.isReference
                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        id: referenceTag

                        objectName: "sourceReferenceTag-" + chip.sourceId
                        visible: chip.isReference
                        text: qsTr("参考")
                        // The reference chip is drawn entirely in its source colour.
                        color: chip.sourceAccent
                        font.pixelSize: Theme.fontSecondary
                        font.weight: Font.DemiBold
                        Accessible.name: qsTr("参考源")
                        Accessible.description: qsTr("对比基准参考源")

                        HoverHandler {
                            id: referenceTagHover
                        }

                        VcsToolTip {
                            visible: referenceTagHover.hovered
                            text: qsTr("参考源")
                        }
                    }
                    Text {
                        id: separatorTwo

                        visible: chip.isReference
                        text: "·"
                        color: control.mutedTextColor
                        font.pixelSize: Theme.fontBody
                    }
                    Text {
                        id: filenameLabel

                        text: chip.parentLabel.length > 0 ? "%1 (%2)".arg(chip.filename).arg(chip.parentLabel) : chip.filename
                        color: control.textColor
                        font.pixelSize: Theme.fontBody
                        elide: Text.ElideMiddle
                        Layout.fillWidth: true

                        HoverHandler {
                            id: filenameHover
                        }

                        VcsToolTip {
                            visible: filenameHover.hovered && chip.fullPath.length > 0
                            text: chip.fullPath
                        }
                    }
                }

                Row {
                    id: chipControls

                    visible: !control.singleMode
                    spacing: 3
                    anchors {
                        right: parent.right
                        verticalCenter: parent.verticalCenter
                    }

                    BusyIndicator {
                        id: pendingIndicator

                        visible: chip.pending
                        running: visible
                        width: 22
                        height: 22
                    }

                    Rectangle {
                        id: changedOnDiskBadge

                        objectName: "changedOnDiskBadge-" + chip.sourceId
                        visible: chip.changedOnDisk
                        width: 24
                        height: 24
                        radius: Theme.radiusSubtle
                        color: Theme.warningPanel
                        border.width: 1
                        border.color: Theme.warningBorder
                        Accessible.name: qsTr("视频文件已在磁盘上被修改")
                        VcsToolTip {
                            visible: changedOnDiskHover.hovered
                            text: qsTr("视频文件已在磁盘上被修改")
                        }

                        HoverHandler {
                            id: changedOnDiskHover
                        }

                        Text {
                            anchors.centerIn: parent
                            text: "!"
                            color: Theme.warning
                            font.pixelSize: Theme.fontSecondary
                            font.weight: Font.DemiBold
                        }
                    }

                    VcsMenu {
                        id: sourceMenu

                        objectName: "sourceMenu-" + chip.sourceId
                        property bool wasOpened: false
                        onOpened: {
                            wasOpened = true;
                            control.openMenuCount += 1;
                        }
                        onClosed: {
                            if (wasOpened) {
                                wasOpened = false;
                                control.openMenuCount = Math.max(0, control.openMenuCount - 1);
                            }
                            if (control.returnViewerFocusAfterClose) {
                                control.returnViewerFocusAfterClose = false;
                                control.viewerFocusRequested();
                            }
                        }
                        Component.onDestruction: {
                            if (wasOpened) {
                                wasOpened = false;
                                control.openMenuCount = Math.max(0, control.openMenuCount - 1);
                            }
                            if (control.returnViewerFocusAfterClose) {
                                control.returnViewerFocusAfterClose = false;
                                control.viewerFocusRequested();
                            }
                        }

                        VcsMenuItem {
                            id: useAsReferenceItem

                            objectName: "makeReferenceAction-" + chip.sourceId
                            // D08: any non-reference source (including the timeline master) can be reference.
                            visible: !chip.isReference
                            text: qsTr("设为参考源")
                            enabled: !chip.pending
                            onTriggered: chip.requestReference()
                        }

                        VcsMenuItem {
                            id: replaceSourceItem

                            objectName: "replaceSourceAction-" + chip.sourceId
                            text: qsTr("替换文件…")
                            enabled: !chip.pending && chip.resolvedSourceIdentity.length > 0
                            onTriggered: chip.requestReplacement()
                        }

                        VcsMenuItem {
                            id: removeSourceItem

                            objectName: "removeSourceAction-" + chip.sourceId
                            text: qsTr("关闭此视频")
                            // Quantity protection is the removal command's own: the last
                            // source of a session cannot be removed through it. The strip is
                            // hidden at sourceCount <= 1, and closing the final video goes
                            // through the session-close path (Main.closeCurrentTask), so this
                            // guard is the boundary and is deliberately not bypassed here.
                            enabled: control.sourceCount > 1
                            onTriggered: chip.requestRemoval()
                        }
                    }
                }
            }
        }

        // V02: batch management is explicit. The toggle enters the selection mode (chip clicks
        // select, the remove button appears); leaving it drops the selection so a hidden
        // batch state can never linger under a click that now means direct look.
        VcsToolButton {
            id: manageSourcesButton

            objectName: "manageSourcesButton"
            // Not checkable on purpose: `checked` is a live mirror of manageSelection and a
            // checkable button would break that binding on the first click. The prominent fill
            // carries the active state instead.
            prominent: control.manageSelection
            x: control.trailingButtonX
            width: 34
            height: chips.height
            text: "☰"
            enabled: true
            helpText: control.manageSelection ? qsTr("退出管理（放弃所选 %1）").arg(control.selectedIdentities.length) : qsTr("管理源（多选移除）")
            Accessible.name: control.manageSelection ? qsTr("退出源管理") : qsTr("管理源")
            onClicked: {
                control.manageSelection = !control.manageSelection;
                if (!control.manageSelection && control.anySelected)
                    control.selectionCleared();
            }
            controlRadius: 8
        }

        VcsToolButton {
            id: addSourceChipButton

            objectName: "addSourceChipButton"
            visible: control.sourceCount < 3
            x: control.trailingButtonX + manageSourcesButton.width + control.chipSpacing
            width: 34
            height: chips.height
            text: "+"
            enabled: true
            helpText: qsTr("添加视频")
            onClicked: control.addRequested()
            controlRadius: 8
        }

        VcsToolButton {
            id: removeSelectedChipButton

            objectName: "removeSelectedChipButton"
            // Selection only ever appears through the manage mode (or an owner-side programmatic
            // select), so "something is selected" is itself the visibility condition - the batch
            // action must stay reachable for whatever put the selection there.
            visible: control.anySelected
            x: control.trailingButtonX + manageSourcesButton.width + control.chipSpacing + (addSourceChipButton.visible ? addSourceChipButton.width + control.chipSpacing : 0)
            width: 34
            height: chips.height
            text: "−"
            enabled: true
            helpText: qsTr("移除所选（%1）").arg(control.selectedIdentities.length)
            Accessible.name: helpText
            onClicked: control.removeSelectedRequested()
            controlRadius: 8
        }
    }
}
