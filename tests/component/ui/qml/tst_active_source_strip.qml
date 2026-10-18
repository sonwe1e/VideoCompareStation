pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtQuick.Window
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
        canvasFocusIdentity: ""
        // Stand-in for the shell wiring Main.qml owns: the strip only emits, the owner keeps
        // the identity-keyed selection state.
        onSelectionToggled: identity => {
            const updated = selectedIdentities.slice();
            const index = updated.indexOf(identity);
            if (index >= 0)
                updated.splice(index, 1);
            else
                updated.push(identity);
            selectedIdentities = updated;
        }
        onSelectionCleared: selectedIdentities = []
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
        id: replaceSpy

        target: strip
        signalName: "replaceRequested"
    }

    SignalSpy {
        id: directLookSpy

        target: strip
        signalName: "directLookRequested"
    }

    SignalSpy {
        id: viewerFocusSpy

        target: strip
        signalName: "viewerFocusRequested"
    }

    SignalSpy {
        id: selectionSpy

        target: strip
        signalName: "selectionToggled"
    }

    SignalSpy {
        id: selectionClearedSpy

        target: strip
        signalName: "selectionCleared"
    }

    SignalSpy {
        id: removeSelectedSpy

        target: strip
        signalName: "removeSelectedRequested"
    }

    TestCase {
        name: "ActiveSourceStrip"
        when: windowShown

        function cleanup() {
            root.visible = true;
            // Restore two-source model in case a test mutated it.
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
            strip.canvasFocusIdentity = "";
            strip.manageSelection = false;
            strip.selectedIdentities = [];
            strip.enabled = true;
            root.width = 720;
            strip.pendingSourceIdentities = [];
            strip.returnViewerFocusAfterClose = false;
            wait(0);

            const firstMenu = findChild(strip, "sourceChip-0").sourceMenuControl;
            const secondMenu = findChild(strip, "sourceChip-1").sourceMenuControl;
            firstMenu.close();
            secondMenu.close();
            referenceSpy.clear();
            removalSpy.clear();
            replaceSpy.clear();
            directLookSpy.clear();
            viewerFocusSpy.clear();
            selectionSpy.clear();
            selectionClearedSpy.clear();
            removeSelectedSpy.clear();
            wait(0);
        }

        function prepareThreeSources() {
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
            wait(30);
        }

        function dragChip(identity, deltaX) {
            const chip = strip.chipAt(identity);
            verify(chip !== null);
            verify(chip.resolvedSourceIdentity === identity);
            const start = chip.mapToItem(root, chip.width / 2, chip.height / 2);
            const end = Qt.point(start.x + deltaX, start.y);
            mousePress(root, start.x, start.y, Qt.LeftButton, Qt.NoModifier);
            try {
                const firstDelta = Math.sign(deltaX) * Math.min(30, Math.abs(deltaX) / 2);
                mouseMove(root, start.x + firstDelta, start.y, 30);
                mouseMove(root, end.x, end.y, 30);
            } finally {
                mouseRelease(root, end.x, end.y, Qt.LeftButton, Qt.NoModifier);
            }
            wait(0);
        }

        // V02: a plain chip-body click is the direct look of that source, addressed by session
        // slot. It never toggles the management selection - selection lives behind the explicit
        // manage mode.
        function test_plainChipClickRequestsDirectLook() {
            const firstChip = strip.chipAt("source-a");
            mouseClick(firstChip, firstChip.width / 2, firstChip.height / 2);
            compare(directLookSpy.count, 1);
            compare(directLookSpy.signalArguments[0][0], 0);
            compare(selectionSpy.count, 0);

            const secondChip = strip.chipAt("source-b");
            mouseClick(secondChip, secondChip.width / 2, secondChip.height / 2);
            compare(directLookSpy.count, 2);
            compare(directLookSpy.signalArguments[1][0], 1);
            compare(selectionSpy.count, 0);
        }

        // V02: the third source gets the same entry as the first two - the click carries slot
        // 2, no matter which source carries the reference role.
        function test_thirdSourceChipHasEqualDirectLookEntry() {
            prepareThreeSources();
            const thirdChip = strip.chipAt("source-c");
            // Keep the full layout and DPI, but pan this identity test's harness to the
            // third card: the hosted VM can clamp the window below the row's pixel width.
            const left = thirdChip.mapToItem(root, 0, 0).x;
            strip.x -= left;
            try {
                const point = thirdChip.mapToItem(root, thirdChip.width / 2, thirdChip.height / 2);
                verify(point.x >= 0 && point.x < root.Window.window.width);
                verify(point.y >= 0 && point.y < root.Window.window.height);
                mouseClick(thirdChip, thirdChip.width / 2, thirdChip.height / 2);
            } finally {
                strip.x = 0;
            }
            compare(directLookSpy.count, 1);
            compare(directLookSpy.signalArguments[0][0], 2);
            compare(selectionSpy.count, 0);
        }

        // V02: batch management is an explicit mode. The chip click only selects once the
        // manage toggle is on, and the remove button only exists there.
        function test_manageModeTurnsChipClicksIntoSelection() {
            const manageButton = findChild(strip, "manageSourcesButton");
            verify(manageButton !== null);
            compare(strip.manageSelection, false);
            compare(findChild(strip, "removeSelectedChipButton").visible, false);

            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            verify(strip.manageSelection);

            const firstChip = strip.chipAt("source-a");
            mouseClick(firstChip, firstChip.width / 2, firstChip.height / 2);
            compare(selectionSpy.count, 1);
            compare(selectionSpy.signalArguments[0][0], "source-a");
            compare(directLookSpy.count, 0);
            compare(strip.selectedIdentities, ["source-a"]);
            verify(findChild(strip, "removeSelectedChipButton").visible);

            const secondChip = strip.chipAt("source-b");
            mouseClick(secondChip, secondChip.width / 2, secondChip.height / 2);
            compare(selectionSpy.count, 2);
            compare(strip.selectedIdentities, ["source-a", "source-b"]);
        }

        // V02: the management mode must not survive a session change. The shell clears its
        // selection list when the source set changes; a strip still in manage mode would turn
        // the next ordinary chip click into a selection instead of the direct look the user
        // expects.
        function test_sessionChangeLeavesTheManageMode() {
            const manageButton = findChild(strip, "manageSourcesButton");
            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            verify(strip.manageSelection);

            strip.displayOrder = ["source-a", "replacement-b"];
            wait(0);
            compare(strip.manageSelection, false);

            // And once more through the source count (an append/remove rather than a replace).
            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            verify(strip.manageSelection);
            strip.sourceCount = 3;
            wait(0);
            compare(strip.manageSelection, false);
        }

        // V02: a chip that is mid-replacement/reopen has no stable identity to focus; the click
        // is refused like every sibling action, and the ready chips keep working.
        function test_pendingChipClickDoesNotRequestDirectLook() {
            strip.pendingSourceIdentities = ["source-b"];
            wait(0);
            const pendingChip = strip.chipAt("source-b");
            mouseClick(pendingChip, pendingChip.width / 2, pendingChip.height / 2);
            compare(directLookSpy.count, 0);
            compare(selectionSpy.count, 0);

            const readyChip = strip.chipAt("source-a");
            mouseClick(readyChip, readyChip.width / 2, readyChip.height / 2);
            compare(directLookSpy.count, 1);
            compare(directLookSpy.signalArguments[0][0], 0);
        }

        // Leaving the manage mode drops the selection, so a hidden batch state can never
        // linger under a click that now means direct look.
        function test_leavingManageModeClearsSelection() {
            const manageButton = findChild(strip, "manageSourcesButton");
            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            const firstChip = strip.chipAt("source-a");
            mouseClick(firstChip, firstChip.width / 2, firstChip.height / 2);
            compare(selectionSpy.count, 1);

            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            verify(!strip.manageSelection);
            compare(selectionClearedSpy.count, 1);
            compare(strip.selectedIdentities, []);
            compare(findChild(strip, "removeSelectedChipButton").visible, false);
        }

        // The remove button stays a management-mode action and still fires its signal.
        function test_removeSelectedOnlyFiresInManageMode() {
            const manageButton = findChild(strip, "manageSourcesButton");
            mouseClick(manageButton, manageButton.width / 2, manageButton.height / 2);
            const firstChip = strip.chipAt("source-a");
            mouseClick(firstChip, firstChip.width / 2, firstChip.height / 2);
            const removeButton = findChild(strip, "removeSelectedChipButton");
            verify(removeButton.visible);
            mouseClick(removeButton, removeButton.width / 2, removeButton.height / 2);
            compare(removeSelectedSpy.count, 1);
        }

        // V02: dragging a chip must never reorder anything. The picture panels follow the
        // session slot order, so a chip-only reorder would be a label change pretending to be
        // a layout change; the drag is disabled and the strip stays where the shell put it.
        function test_dragDoesNotReorder_data() {
            return [{tag: "right", identity: "source-a", delta: 620},
                    {tag: "left", identity: "source-c", delta: -620}];
        }

        function test_dragDoesNotReorder(data) {
            prepareThreeSources();
            dragChip(data.identity, data.delta);
            compare(strip.displayOrder.join(","), "source-a,source-b,source-c");
            const chip = strip.chipAt(data.identity);
            compare(chip.x, chip.layoutX);
            compare(selectionSpy.count, 0);
            compare(directLookSpy.count, 0);
        }

        // Small pointer jitter stays an ordinary click - and in the default mode that click
        // is the direct look, not a selection.
        function test_smallPointerJitterRemainsAnOrdinaryClick() {
            prepareThreeSources();
            dragChip("source-a", 2);
            compare(directLookSpy.count, 1);
            compare(directLookSpy.signalArguments[0][0], 0);
            compare(selectionSpy.count, 0);
        }

        function test_menuClickDoesNotDirectLookOrSelectTheChip() {
            prepareThreeSources();
            const overflow = findChild(strip, "sourceChip-1");
            mouseClick(overflow, overflow.width / 2, overflow.height / 2, Qt.RightButton);
            const menu = overflow.sourceMenuControl;
            tryCompare(menu, "opened", true);
            keyClick(Qt.Key_Escape);
            tryCompare(menu, "visible", false);
            compare(directLookSpy.count, 0);
            compare(selectionSpy.count, 0);
        }

        function test_reference_is_a_badge_and_all_sources_have_action_menus() {
            const tag = findChild(strip, "sourceReferenceTag-0");
            const firstOverflow = findChild(strip, "sourceChip-0");
            const secondOverflow = findChild(strip, "sourceChip-1");
            verify(tag !== null);
            verify(firstOverflow !== null);
            verify(secondOverflow !== null);
            verify(tag.visible);
            compare(tag.text, "参考");

            mouseClick(secondOverflow, secondOverflow.width / 2, secondOverflow.height / 2, Qt.RightButton);
            const menu = secondOverflow.sourceMenuControl;
            tryCompare(menu, "opened", true);
            compare(strip.anyMenuOpen, true);
            compare(menu.popupType, Popup.Window);
            verify(secondOverflow.makeReferenceAction.visible);
            menu.close();
            tryCompare(strip, "anyMenuOpen", false);
        }

        function test_actions_emit_identity_after_the_menu_closes() {
            const secondOverflow = findChild(strip, "sourceChip-1");
            const menu = secondOverflow.sourceMenuControl;
            const makeReference = secondOverflow.makeReferenceAction;
            menu.popup(root, 300, 24);
            tryCompare(menu, "opened", true);
            makeReference.triggered();
            tryCompare(referenceSpy, "count", 1);
            compare(referenceSpy.signalArguments[0][0], "source-b");
            tryCompare(menu, "opened", false);

            const firstOverflow = findChild(strip, "sourceChip-0");
            const firstMenu = firstOverflow.sourceMenuControl;
            const remove = firstOverflow.removeSourceAction;
            firstMenu.popup(root, 300, 24);
            tryCompare(firstMenu, "opened", true);
            remove.triggered();
            tryCompare(removalSpy, "count", 1);
            compare(removalSpy.signalArguments[0][0], "source-a");
        }

        // V02 "替换文件": the menu carries the third per-source action, and it emits the
        // frozen identity after the menu closes, exactly like reference and removal.
        function test_replace_action_emits_identity_after_the_menu_closes() {
            const secondOverflow = findChild(strip, "sourceChip-1");
            const menu = secondOverflow.sourceMenuControl;
            const replace = secondOverflow.replaceSourceAction;
            verify(replace !== null);
            menu.popup(root, 300, 24);
            tryCompare(menu, "opened", true);
            // A third row means the list needs another frame to materialize the delegate.
            tryCompare(replace, "visible", true);
            replace.triggered();
            tryCompare(replaceSpy, "count", 1);
            compare(replaceSpy.signalArguments[0][0], "source-b");
            tryCompare(menu, "opened", false);
        }

        function test_pending_identity_disables_only_the_affected_source() {
            const firstOverflow = findChild(strip, "sourceChip-0");
            const secondOverflow = findChild(strip, "sourceChip-1");
            strip.pendingSourceIdentities = ["source-b"];
            tryCompare(firstOverflow, "pending", false);
            tryCompare(secondOverflow, "pending", true);
            verify(!strip.openSourceMenu(secondOverflow, Qt.point(10, 10)));
        }

        // V02: the focused source shows the 直看 tag so the four per-source states (direct
        // look, reference role, timeline master, plain) stay visually distinct.
        function test_direct_look_tag_marks_the_focused_chip() {
            prepareThreeSources();
            compare(findChild(strip, "sourceDirectLookTag-1").visible, false);
            strip.canvasFocusIdentity = "source-b";
            wait(0);
            verify(findChild(strip, "sourceDirectLookTag-1").visible);
            compare(findChild(strip, "sourceDirectLookTag-0").visible, false);
            compare(findChild(strip, "sourceDirectLookTag-2").visible, false);
        }

        // Test B: model reset while menu is open must not leak openMenuCount.
        function test_model_reset_clears_open_menu_count() {
            const secondOverflow = findChild(strip, "sourceChip-1");
            mouseClick(secondOverflow, secondOverflow.width / 2, secondOverflow.height / 2, Qt.RightButton);
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
            const secondOverflow = findChild(strip, "sourceChip-1");
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
            const secondOverflow = findChild(strip, "sourceChip-1");
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
