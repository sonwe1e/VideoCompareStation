pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

Item {
    id: root

    width: 960
    height: 640

    QtObject {
        id: controller

        property bool canFirst: true
        property int lastPairEdge: -1
        property int lastPairPolicy: -1
        property int lastDefaultPairPolicy: -1
        function estimateAlignment() {
        }
        function analyzeSequenceAlignment() {
        }
        function cancelAlignmentAnalysis() {
        }
        function applyComparisonPairFromEdge(edge, policy) {
            lastPairEdge = edge;
            lastPairPolicy = policy;
            return true;
        }
        function applyDefaultPairPolicy(policy) {
            lastDefaultPairPolicy = policy;
            return true;
        }
    }

    QtObject {
        id: preferences

        property int viewMode: 0
        property int differenceEdge: 0
        property int defaultPairPolicy: 2
        property int shortcutPreset: 0
    }

    QtObject {
        id: session

        property string lastReferenceIdentity: ""
        property bool inspectorVisible: false
        function changeReferenceByIdentity(sourceIdentity) {
            lastReferenceIdentity = sourceIdentity;
            return true;
        }
    }

    Dvs.ApplicationMenuBar {
        id: menuBar

        controller: controller
        preferences: preferences
        session: session
        sourceCount: 2
        busy: false
        canonicalSourceIndex: 0
        referenceSourceIndex: 0
        effectiveDifferenceEdge: 0
        currentViewMode: 0
        inspectorOpen: false
        graphicsReady: true
        currentFrame: 0
        alignmentAnalysisRunning: false
        chromeVisible: true
        fullScreen: false
        shortcutPreset: 0
        autoplayOnOpen: true
        sourceIdentities: ["source-a", "source-b", "source-c"]
        // A real menu is always attached to a session; the comparison rows gate on it, so the
        // fixture has to say which state it represents instead of leaving it at false.
        videoHasSession: true
    }

    TestCase {
        name: "ApplicationMenuBar"
        when: windowShown

        function cleanup() {
            findChild(menuBar, "compareMenu").close();
            findChild(menuBar, "viewMenu").close();
            menuBar.sourceCount = 2;
            menuBar.currentViewMode = 0;
            wait(0);
        }

        function menuRowsHeight(menu) {
            let total = 0;
            for (let index = 0; index < menu.count; ++index) {
                const item = menu.itemAt(index);
                if (item)
                    total += item.height;
            }
            return total;
        }

        // UX-04: the menu used to take fontMono (13) for its label - one step below body copy -
        // and then render the submenu arrow at fontBody (14), larger than the label it follows.
        // Both are now pinned here so the hierarchy cannot invert again silently.
        function test_menu_typography_uses_the_body_step() {
            const wipeLabel = findChild(menuBar, "wipeMenuItemLabel");
            const openFiles = findChild(menuBar, "openFilesMenuItem");
            const openFilesTrailing = findChild(menuBar, "openFilesMenuItemTrailing");
            verify(wipeLabel !== null);
            verify(openFiles !== null);
            verify(openFilesTrailing !== null);
            compare(wipeLabel.font.pixelSize, Theme.fontBody);
            compare(openFilesTrailing.font.pixelSize, Theme.fontSecondary);
            verify(openFilesTrailing.font.pixelSize < wipeLabel.font.pixelSize,
                "the trailing column must never outweigh the label it follows");
        }

        // UX-07 / UX-05: the help entry exists even without a video session, and F10 opens the
        // first menu at its item - the two keyboard/menu gaps the review found.
        function test_help_menu_reports_the_shortcut_table_and_f10_opens_the_first_menu() {
            const helpMenu = findChild(menuBar, "helpMenu");
            const helpItem = findChild(menuBar, "shortcutHelpMenuItem");
            const fileMenu = findChild(menuBar, "fileMenu");
            verify(helpMenu !== null);
            verify(helpItem !== null);
            verify(fileMenu !== null);

            let fired = 0;
            const handler = function() {
                fired += 1;
            };
            menuBar.shortcutHelpRequested.connect(handler);
            helpItem.triggered();
            compare(fired, 1, "the help row raises the same request the ? key does");
            menuBar.shortcutHelpRequested.disconnect(handler);

            verify(menuBar.focusFirstMenu());
            tryCompare(fileMenu, "opened", true);
            fileMenu.close();
            tryCompare(fileMenu, "opened", false);
        }

        function test_hides_three_source_layout_for_a_pair() {
            const layout = findChild(menuBar, "layoutMenu");
            const pair = findChild(menuBar, "pairMenu");
            verify(layout !== null);
            verify(pair !== null);
            compare(layout.menuItemVisible, false);
            compare(pair.menuItemVisible, false);

            menuBar.sourceCount = 3;
            tryCompare(layout, "menuItemVisible", true);
            tryCompare(pair, "menuItemVisible", true);
        }

        function test_compare_modes_use_radio_indicators() {
            const side = findChild(menuBar, "sideBySideMenuItem");
            const wipe = findChild(menuBar, "wipeMenuItem");
            const difference = findChild(menuBar, "differenceMenuItem");
            verify(side !== null);
            verify(wipe !== null);
            verify(difference !== null);

            compare(side.radioIndicator, true);
            compare(wipe.radioIndicator, true);
            compare(difference.radioIndicator, true);
            compare(side.checked, true);
            compare(wipe.checked, false);
            compare(difference.checked, false);

            menuBar.currentViewMode = 3;
            tryCompare(side, "checked", false);
            tryCompare(wipe, "checked", false);
            tryCompare(difference, "checked", true);
        }

        function test_two_video_compare_menu_collapses_three_video_rows() {
            const compareMenu = findChild(menuBar, "compareMenu");
            const layoutMenu = findChild(menuBar, "layoutMenu");
            const pairMenu = findChild(menuBar, "pairMenu");
            const referenceMenu = findChild(menuBar, "referenceMenu");
            verify(compareMenu !== null);
            verify(layoutMenu !== null);
            verify(pairMenu !== null);
            verify(referenceMenu !== null);

            // Two-source sessions hide the three-up layout and pair submenus; reference stays.
            menuBar.sourceCount = 2;
            tryCompare(layoutMenu, "menuItemVisible", false);
            tryCompare(pairMenu, "menuItemVisible", false);
            tryCompare(referenceMenu, "menuItemVisible", true);

            compareMenu.popup(root, 24, 24);
            tryCompare(compareMenu, "opened", true);
            const compactRowsHeight = menuRowsHeight(compareMenu);
            compareMenu.close();
            tryCompare(compareMenu, "opened", false);

            // Three-source sessions restore both collapsed rows (two 35px menu rows).
            menuBar.sourceCount = 3;
            tryCompare(layoutMenu, "menuItemVisible", true);
            tryCompare(pairMenu, "menuItemVisible", true);
            compareMenu.popup(root, 24, 24);
            tryCompare(compareMenu, "opened", true);
            compare(menuRowsHeight(compareMenu), compactRowsHeight + 70);
        }

        function test_single_video_menu_stays_open_and_gates_only_comparison_rows() {
            const compareMenu = findChild(menuBar, "compareMenu");
            const compareButton = findChild(menuBar, "compareMenuButton");
            const side = findChild(menuBar, "sideBySideMenuItem");
            const continuity = findChild(menuBar, "playbackContinuityMenu");
            const inspector = findChild(menuBar, "compareInspectorMenuItem");
            verify(compareMenu !== null);
            verify(compareButton !== null);
            verify(side !== null);
            verify(continuity !== null);
            verify(inspector !== null);

            // UX-09: one video no longer locks the whole menu. The title button stays live and
            // the menu opens; only the rows that need a second source are unavailable, while
            // the session-scoped rows in the same menu (continuity, inspector) stay usable.
            menuBar.sourceCount = 1;
            tryCompare(compareButton, "enabled", true);
            compareMenu.open();
            tryCompare(compareMenu, "opened", true);
            compare(side.enabled, false);
            compare(continuity.menuItemEnabled, true);
            compare(inspector.enabled, true);
            // Three grey rows with no reason is the failure mode this replaced, so the menu has
            // to name the reason and the note has to follow the state instead of staying on.
            const gateNote = findChild(compareMenu, "compareGateNote");
            verify(gateNote !== null);
            compare(gateNote.visible, true);
            verify(gateNote.noteText.indexOf("至少两个视频") >= 0, gateNote.noteText);
            menuBar.sourceCount = 2;
            tryCompare(gateNote, "visible", false);
            menuBar.sourceCount = 1;
            compareMenu.close();
            tryCompare(compareMenu, "opened", false);
        }

        function test_open_state_and_reference_use_source_identity() {
            const view = findChild(menuBar, "viewMenu");
            verify(view !== null);
            view.popup(root, 24, 24);
            tryCompare(view, "opened", true);
            compare(menuBar.anyMenuOpen, true);
            compare(view.popupType, Popup.Window);
            view.close();
            tryCompare(menuBar, "anyMenuOpen", false);

            verify(menuBar.changeReferenceByIndex(1));
            compare(session.lastReferenceIdentity, "source-b");
        }

        function test_all_compare_and_view_submenus_are_opaque_window_popups() {
            menuBar.sourceCount = 3;
            const menuNames = ["compareMenu", "layoutMenu", "pairMenu", "referenceMenu", "viewMenu", "shortcutPresetMenu"];
            for (const menuName of menuNames) {
                const menu = findChild(menuBar, menuName);
                verify(menu !== null, menuName);
                compare(menu.popupType, Popup.Window, menuName);
                compare(menu.background.color.a, 1.0, menuName);
                menu.popup(root, 24, 24);
                tryCompare(menu, "opened", true);
                verify(menu.x >= 0, menuName);
                verify(menu.y >= 0, menuName);
                menu.close();
                tryCompare(menu, "opened", false);
            }
        }

        function test_pair_menu_uses_committed_edge_and_policy_command() {
            menuBar.sourceCount = 3;
            menuBar.effectiveDifferenceEdge = 1;
            const pairMenu = findChild(menuBar, "pairMenu");
            verify(pairMenu !== null);
            pairMenu.popup(root, 24, 24);
            tryCompare(pairMenu, "opened", true);

            // User selection sends the pair command and does not write preference.differenceEdge.
            const before = preferences.differenceEdge;
            const edgeItem = pairMenu.itemAt(1);
            verify(edgeItem !== null);
            edgeItem.triggered();
            compare(controller.lastPairEdge, 1);
            compare(preferences.differenceEdge, before);

            const policyMenu = findChild(menuBar, "defaultPairPolicyMenu");
            verify(policyMenu !== null);
            const policyItem = policyMenu.itemAt(0);
            verify(policyItem !== null);
            policyItem.triggered();
            compare(controller.lastDefaultPairPolicy, 0);
            compare(preferences.defaultPairPolicy, 0);
            pairMenu.close();
        }
    }
}
