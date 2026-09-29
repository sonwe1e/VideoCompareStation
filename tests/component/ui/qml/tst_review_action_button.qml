import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

// Regression guard for ReviewActionButton.mirrorsState. Qt flips a checkable button's
// `checked` before onClicked, so a button whose `checked` mirrors caller-owned state used to
// lose its highlight when the already-selected option was clicked again. The contract tests
// invoke `clicked` directly and never reach that flip; these tests drive real input.
Item {
    id: root

    property bool trueSize: false
    property bool menuModeActive: false
    property int menuOpens: 0

    width: 520
    height: 120

    Row {
        spacing: 8

        Dvs.ReviewActionButton {
            id: fitButton

            text: "Fit"
            checkable: true
            checked: !root.trueSize
            mirrorsState: true
            onClicked: root.trueSize = false
        }
        Dvs.ReviewActionButton {
            id: trueSizeButton

            text: "100%"
            checkable: true
            checked: root.trueSize
            mirrorsState: true
            onClicked: root.trueSize = true
        }
        // Mirrors a mode only a menu choice can change, like the difference-mode button.
        Dvs.ReviewActionButton {
            id: menuButton

            text: "Diff"
            checkable: true
            checked: root.menuModeActive
            mirrorsState: true
            onClicked: root.menuOpens += 1
        }
        // A toggle that owns its state keeps Qt's own flip.
        Dvs.ReviewActionButton {
            id: ownedToggle

            text: "Lock"
            checkable: true
        }
    }

    TestCase {
        name: "ReviewActionButton"
        when: windowShown

        function init() {
            // Flip each state once so the bindings re-evaluate even when an earlier case
            // left a stale `checked` behind; every case then starts from a fresh value.
            root.trueSize = true;
            root.trueSize = false;
            root.menuModeActive = true;
            root.menuModeActive = false;
            root.menuOpens = 0;
            ownedToggle.checked = false;
        }

        function test_reclick_keeps_selected_option_checked() {
            verify(fitButton.checked);
            mouseClick(fitButton);
            verify(!root.trueSize);
            verify(fitButton.checked, "re-clicking the selected option must keep its highlight");
            verify(!trueSizeButton.checked);
        }

        function test_click_moves_selection() {
            mouseClick(trueSizeButton);
            verify(root.trueSize);
            verify(trueSizeButton.checked);
            verify(!fitButton.checked);
            mouseClick(fitButton);
            verify(!root.trueSize);
            verify(fitButton.checked);
            verify(!trueSizeButton.checked);
        }

        function test_space_keeps_selected_option_checked() {
            fitButton.forceActiveFocus();
            keyClick(Qt.Key_Space);
            verify(!root.trueSize);
            verify(fitButton.checked, "Space on the selected option must keep its highlight");
        }

        function test_click_without_state_change_does_not_check() {
            mouseClick(menuButton);
            compare(root.menuOpens, 1);
            verify(!menuButton.checked, "a click that only opens a menu must not light the button");
        }

        function test_owned_toggle_still_flips() {
            mouseClick(ownedToggle);
            verify(ownedToggle.checked);
            mouseClick(ownedToggle);
            verify(!ownedToggle.checked);
        }
    }
}
