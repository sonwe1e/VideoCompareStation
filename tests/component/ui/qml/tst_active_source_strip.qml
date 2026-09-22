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

    TestCase {
        name: "ActiveSourceStrip"
        when: windowShown

        function cleanup() {
            // Restore two-source model in case a test mutated it (e.g. Test B).
            sources.clear();
            sources.append({sourceId: 0, sourceIdentity: "source-a", filename: "reference.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            sources.append({sourceId: 1, sourceIdentity: "source-b", filename: "prediction.mp4", parentLabel: "", fullPath: "", changedOnDisk: false});
            strip.sourceCount = 2;
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
