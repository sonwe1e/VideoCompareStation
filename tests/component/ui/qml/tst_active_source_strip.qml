pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 720
    height: 160

    ListModel {
        id: sources

        ListElement {
            sourceId: 0
            sourceIdentity: "source-a"
            filename: "reference.mp4"
            parentLabel: ""
            fullPath: ""
            changedOnDisk: false
        }
        ListElement {
            sourceId: 1
            sourceIdentity: "source-b"
            filename: "prediction.mp4"
            parentLabel: ""
            fullPath: ""
            changedOnDisk: false
        }
    }

    Dvs.ActiveSourceStrip {
        id: strip

        width: parent.width
        sourcesModel: sources
        sourceCount: 2
        singleMode: false
        canonicalSourceIndex: 0
        canonicalSourceIdentity: "source-a"
        referenceSourceIndex: 0
        referenceSourceIdentity: "source-a"
        pendingSourceIdentities: []
        sourceIdentities: ["source-a", "source-b"]
        displayOrder: ["source-a", "source-b"]
        onMoveRequested: (from, to) => {
            const order = displayOrder.slice();
            const moved = order.splice(from, 1)[0];
            order.splice(to, 0, moved);
            displayOrder = order;
        }
    }

    SignalSpy {
        id: referenceSpy

        target: strip
        signalName: "referenceRequested"
    }

    SignalSpy {
        id: removalSpy

        target: strip
        signalName: "removeRequested"
    }

    SignalSpy {
        id: viewerFocusSpy

        target: strip
        signalName: "viewerFocusRequested"
    }

    DragHandler {
        id: stealingDrag
        target: null
        dragThreshold: 30000
        grabPermissions: PointerHandler.CanTakeOverFromAnything | PointerHandler.ApprovesTakeOverByAnything
    }

    SignalSpy {
        id: moveSpy
        target: strip
        signalName: "moveRequested"
    }

    SignalSpy {
        id: selectionSpy
        target: strip
        signalName: "selectionToggled"
    }

    TestCase {
        name: "ActiveSourceStrip"
        when: windowShown

        function cleanup() {
            root.visible = true;
            // Restore two-source model in case a test mutated it (e.g. Test B).
            sources.clear();
            sources.append({sourceId: 0, sourceIdentity: "source-a", filename: "reference.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            sources.append({sourceId: 1, sourceIdentity: "source-b", filename: "prediction.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            strip.sourceCount = 2;
            strip.sourceIdentities = ["source-a", "source-b"];
            strip.displayOrder = ["source-a", "source-b"];
            strip.canonicalSourceIndex = 0;
            strip.canonicalSourceIdentity = "source-a";
            strip.referenceSourceIndex = 0;
            strip.referenceSourceIdentity = "source-a";
            strip.enabled = true;
            stealingDrag.dragThreshold = 30000;
            root.width = 720;
            moveSpy.clear();
            selectionSpy.clear();
            wait(0);

            const firstMenu = findChild(strip, "sourceOverflowButton-0").sourceMenuControl;
            const secondMenu = findChild(strip, "sourceOverflowButton-1").sourceMenuControl;
            firstMenu.close();
            secondMenu.close();
            referenceSpy.clear();
            removalSpy.clear();
            viewerFocusSpy.clear();
            strip.pendingSourceIdentities = [];
            strip.returnViewerFocusAfterClose = false;
            wait(0);
        }

        function prepareDragSources() {
            root.width = 1100;
            sources.clear();
            sources.append({sourceId: 0, sourceIdentity: "source-a", filename: "prediction-a.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            sources.append({sourceId: 1, sourceIdentity: "source-b", filename: "ground-truth.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            sources.append({sourceId: 2, sourceIdentity: "source-c", filename: "prediction-c.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            strip.sourceCount = 3;
            strip.sourceIdentities = ["source-a", "source-b", "source-c"];
            strip.displayOrder = ["source-a", "source-b", "source-c"];
            strip.referenceSourceIndex = 1;
            strip.referenceSourceIdentity = "source-b";
            moveSpy.clear();
            selectionSpy.clear();
            wait(30);
        }

        function dragChip(identity, deltaX, duringDrag, button = Qt.LeftButton, modifiers = Qt.NoModifier) {
            const chip = strip.chipAt(identity);
            verify(chip !== null);
            compare(chip.resolvedSourceIdentity, identity);
            const start = chip.mapToItem(root, chip.width / 2, chip.height / 2);
            const end = Qt.point(start.x + deltaX, start.y);
            mousePress(root, start.x, start.y, button, modifiers);
            try {
                const firstDelta = Math.sign(deltaX) * Math.min(30, Math.abs(deltaX) / 2);
                mouseMove(root, start.x + firstDelta, start.y, 30);
                mouseMove(root, end.x, end.y, 30);
                if (duringDrag)
                    duringDrag(chip, start, end);
            } finally {
                mouseRelease(root, end.x, end.y, button, modifiers);
            }
            wait(0);
        }

        function test_realDragMovesLeftAndRight_data() {
            return [{tag: "right", identity: "source-a", delta: 620, from: 0, to: 2, order: "source-b,source-c,source-a"},
                    {tag: "left", identity: "source-c", delta: -620, from: 2, to: 0, order: "source-c,source-a,source-b"}];
        }

        function test_realDragMovesLeftAndRight(data) {
            prepareDragSources();
            const sourceIds = [strip.chipAt("source-a").sourceId, strip.chipAt("source-b").sourceId, strip.chipAt("source-c").sourceId];
            dragChip(data.identity, data.delta, function(chip) {
                verify(chip.dragArmed);
                verify(Math.abs(chip.x - chip.layoutX) > 500);
                verify(chip.z > strip.chipAt("source-b").z);
            });
            compare(moveSpy.count, 1);
            compare(moveSpy.signalArguments[0][0], data.from);
            compare(moveSpy.signalArguments[0][1], data.to);
            compare(strip.displayOrder.join(","), data.order);
            compare(selectionSpy.count, 0);
            compare(strip.referenceSourceIdentity, "source-b");
            compare(strip.canonicalSourceIdentity, "source-a");
            compare([strip.chipAt("source-a").sourceId, strip.chipAt("source-b").sourceId, strip.chipAt("source-c").sourceId].join(","), sourceIds.join(","));
            const moved = strip.chipAt(data.identity);
            compare(moved.x, moved.layoutX);
        }

        function test_repeatedDragStartsFromTheNewLayout() {
            prepareDragSources();
            dragChip("source-a", 620);
            dragChip("source-a", -620);
            compare(moveSpy.count, 2);
            compare(moveSpy.signalArguments[1][0], 2);
            compare(moveSpy.signalArguments[1][1], 0);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
        }

        function test_dragWithinOwnPositionDoesNotReorder_data() {
            return [{tag: "right-small", delta: 40}, {tag: "left-small", delta: -40}];
        }

        function test_dragWithinOwnPositionDoesNotReorder(data) {
            prepareDragSources();
            dragChip("source-b", data.delta);
            compare(moveSpy.count, 0);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
            compare(selectionSpy.count, 0);
        }

        function test_smallPointerJitterRemainsAnOrdinaryClick() {
            prepareDragSources();
            dragChip("source-a", 2);
            compare(moveSpy.count, 0);
            compare(selectionSpy.count, 1);
            compare(selectionSpy.signalArguments[0][0], "source-a");
        }

        function test_disabledDragCancelsWithoutCommitting_data() {
            return [{tag: "disabled", hide: false}, {tag: "hidden", hide: true}];
        }

        function test_disabledDragCancelsWithoutCommitting(data) {
            prepareDragSources();
            dragChip("source-a", 620, function(chip) {
                verify(chip.dragArmed);
                if (data.hide)
                    root.visible = false;
                else
                    strip.enabled = false;
            });
            compare(moveSpy.count, 0);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
            const chip = strip.chipAt("source-a");
            compare(chip.x, chip.layoutX);
        }

        function test_stolenGrabCancelsWithoutCommitting() {
            prepareDragSources();
            dragChip("source-a", 620, function(chip, start, end) {
                verify(chip.dragArmed);
                stealingDrag.dragThreshold = 0;
                mouseMove(root, end.x + 20, end.y, 30);
                verify(stealingDrag.active);
            });
            compare(moveSpy.count, 0);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
            const chip = strip.chipAt("source-a");
            compare(chip.x, chip.layoutX);
        }

        function test_sourceChangesInvalidateAnInFlightDrag_data() {
            return [{tag: "replacement", mode: "replace"}, {tag: "removal", mode: "remove"},
                    {tag: "addition", mode: "add"}, {tag: "reorder", mode: "reorder"},
                    {tag: "dragged-replacement", mode: "replace-dragged"},
                    {tag: "dragged-removal", mode: "remove-dragged"}];
        }

        function test_sourceChangesInvalidateAnInFlightDrag(data) {
            prepareDragSources();
            if (data.mode === "add") {
                sources.remove(2);
                strip.sourceCount = 2;
                strip.sourceIdentities = ["source-a", "source-b"];
                strip.displayOrder = strip.sourceIdentities.slice();
            }
            let expectedOrder = "";
            dragChip("source-a", 620, function() {
                if (data.mode === "replace-dragged") {
                    sources.setProperty(0, "sourceIdentity", "replacement-a");
                    strip.sourceIdentities = ["replacement-a", "source-b", "source-c"];
                } else if (data.mode === "remove-dragged") {
                    sources.remove(0);
                    strip.sourceCount = 2;
                    strip.sourceIdentities = ["source-b", "source-c"];
                } else if (data.mode === "replace") {
                    sources.setProperty(2, "sourceIdentity", "replacement-c");
                    strip.sourceIdentities = ["source-a", "source-b", "replacement-c"];
                } else if (data.mode === "remove") {
                    sources.remove(2);
                    strip.sourceCount = 2;
                    strip.sourceIdentities = ["source-a", "source-b"];
                } else if (data.mode === "add") {
                    sources.append({sourceId: 2, sourceIdentity: "source-c", filename: "prediction-c.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
                    strip.sourceCount = 3;
                    strip.sourceIdentities = ["source-a", "source-b", "source-c"];
                }
                strip.displayOrder = data.mode === "reorder" ? ["source-c", "source-b", "source-a"] : strip.sourceIdentities.slice();
                expectedOrder = strip.displayOrder.join(",");
            });
            compare(moveSpy.count, 0);
            compare(strip.displayOrder.join(","), expectedOrder);
            compare(selectionSpy.count, 0);
        }

        function test_menuClickDoesNotDragOrSelectTheChip() {
            prepareDragSources();
            const overflow = findChild(strip, "sourceOverflowButton-1");
            mouseClick(overflow, overflow.width / 2, overflow.height / 2);
            const menu = overflow.sourceMenuControl;
            tryCompare(menu, "opened", true);
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
            compare(moveSpy.count, 0);
            compare(selectionSpy.count, 0);
        }

        function test_modifiedOrRightButtonDragDoesNotReorder_data() {
            return [{tag: "control-drag", button: Qt.LeftButton, modifiers: Qt.ControlModifier},
                    {tag: "right-button", button: Qt.RightButton, modifiers: Qt.NoModifier}];
        }

        function test_modifiedOrRightButtonDragDoesNotReorder(data) {
            prepareDragSources();
            dragChip("source-a", 620, null, data.button, data.modifiers);
            compare(moveSpy.count, 0);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
        }

        function test_reference_is_a_badge_and_all_sources_have_action_menus() {
            const tag = findChild(strip, "sourceReferenceTag-0");
            const firstOverflow = findChild(strip, "sourceOverflowButton-0");
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            verify(tag !== null);
            verify(firstOverflow !== null);
            verify(secondOverflow !== null);
            verify(tag.visible);
            compare(tag.text, "参考");

            mouseClick(secondOverflow, secondOverflow.width / 2, secondOverflow.height / 2);
            const menu = secondOverflow.sourceMenuControl;
            tryCompare(menu, "opened", true);
            compare(strip.anyMenuOpen, true);
            compare(menu.popupType, Popup.Window);
            verify(secondOverflow.makeReferenceAction.visible);
            menu.close();
            tryCompare(strip, "anyMenuOpen", false);
        }

        function test_actions_emit_identity_after_the_menu_closes() {
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            const menu = secondOverflow.sourceMenuControl;
            const makeReference = secondOverflow.makeReferenceAction;
            menu.popup(root, 300, 24);
            tryCompare(menu, "opened", true);
            makeReference.triggered();
            tryCompare(referenceSpy, "count", 1);
            compare(referenceSpy.signalArguments[0][0], "source-b");
            tryCompare(menu, "opened", false);

            const firstOverflow = findChild(strip, "sourceOverflowButton-0");
            const firstMenu = firstOverflow.sourceMenuControl;
            const remove = firstOverflow.removeSourceAction;
            firstMenu.popup(root, 300, 24);
            tryCompare(firstMenu, "opened", true);
            remove.triggered();
            tryCompare(removalSpy, "count", 1);
            compare(removalSpy.signalArguments[0][0], "source-a");
        }

        function test_pending_identity_disables_only_the_affected_source() {
            const firstOverflow = findChild(strip, "sourceOverflowButton-0");
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            strip.pendingSourceIdentities = ["source-b"];
            tryCompare(firstOverflow, "enabled", true);
            tryCompare(secondOverflow, "enabled", false);
        }

        // Test B: model reset while menu is open must not leak openMenuCount.
        function test_model_reset_clears_open_menu_count() {
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            mouseClick(secondOverflow, secondOverflow.width / 2, secondOverflow.height / 2);
            const menu = secondOverflow.sourceMenuControl;
            tryCompare(menu, "opened", true);
            compare(strip.anyMenuOpen, true);
            compare(strip.openMenuCount, 1);

            // Simulate external model teardown (e.g. shell closing all sources).
            sources.clear();
            sources.append({sourceId: 0, sourceIdentity: "source-x", filename: "new.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            strip.sourceCount = 1;
            wait(0);

            tryCompare(strip, "anyMenuOpen", false);
            compare(strip.openMenuCount, 0);

            // Restore two-source model so cleanup() does not crash.
            sources.clear();
            sources.append({sourceId: 0, sourceIdentity: "source-a", filename: "reference.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            sources.append({sourceId: 1, sourceIdentity: "source-b", filename: "prediction.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            strip.sourceCount = 2;
            wait(0);
        }

        // Triggered path (makeReference) must emit viewerFocusRequested exactly once.
        function test_triggered_action_emits_viewer_focus_requested() {
            viewerFocusSpy.clear();
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            const menu = secondOverflow.sourceMenuControl;
            const makeReference = secondOverflow.makeReferenceAction;
            menu.popup(root, 300, 24);
            tryCompare(menu, "opened", true);
            makeReference.triggered();
            tryCompare(menu, "opened", false);
            compare(viewerFocusSpy.count, 1);
        }

        // Escape-close (no triggered action) must NOT emit viewerFocusRequested.
        function test_escape_close_does_not_emit_viewer_focus_requested() {
            viewerFocusSpy.clear();
            const secondOverflow = findChild(strip, "sourceOverflowButton-1");
            const menu = secondOverflow.sourceMenuControl;
            menu.popup(root, 300, 24);
            tryCompare(menu, "opened", true);
            // Press Escape to close the popup without triggering any action.
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "opened", false);
            compare(viewerFocusSpy.count, 0);
        }
    }
}
