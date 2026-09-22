pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

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
        sourceIdentities: ["source-a", "source-b", "source-c"]
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

        function test_single_video_compare_is_disabled_and_cannot_open() {
            const compareMenu = findChild(menuBar, "compareMenu");
            const compareButton = findChild(menuBar, "compareMenuButton");
            verify(compareMenu !== null);
            verify(compareButton !== null);

            menuBar.sourceCount = 1;
            tryCompare(compareMenu, "enabled", false);
            tryCompare(compareButton, "enabled", false);

            compareMenu.open();
            wait(0);
            compare(compareMenu.opened, false);

            mouseClick(compareButton, compareButton.width / 2, compareButton.height / 2);
            wait(0);
            compare(compareMenu.opened, false);

            keyClick(compareButton, Qt.Key_Return);
            wait(0);
            compare(compareMenu.opened, false);
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
