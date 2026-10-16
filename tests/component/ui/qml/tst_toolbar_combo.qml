pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

Item {
    id: root

    width: 420
    height: 280

    Item {
        id: focusProbe

        objectName: "focusProbe"
        width: 20
        height: 20
        activeFocusOnTab: true
    }

    Dvs.ToolbarCombo {
        id: combo

        objectName: "toolbarComboUnderTest"
        x: 32
        y: 32
        width: 180
        model: ["First", "Second", "Third"]
        Accessible.name: "Test toolbar choice"
    }

    Rectangle {
        id: outsideTarget

        x: 310
        y: 210
        width: 80
        height: 40
        color: "#0d121b"
    }

    TestCase {
        name: "ToolbarCombo"
        when: windowShown

        function linearChannel(channel) {
            return channel <= 0.04045 ? channel / 12.92 : Math.pow((channel + 0.055) / 1.055, 2.4);
        }

        function luminance(color) {
            return 0.2126 * linearChannel(color.r) + 0.7152 * linearChannel(color.g) + 0.0722 * linearChannel(color.b);
        }

        function contrastRatio(foreground, background) {
            const lighter = Math.max(luminance(foreground), luminance(background));
            const darker = Math.min(luminance(foreground), luminance(background));
            return (lighter + 0.05) / (darker + 0.05);
        }

        function openPopup() {
            combo.forceActiveFocus();
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryVerify(() => combo.popup.visible);
            tryCompare(combo.popup.contentItem, "count", combo.count);
            tryVerify(() => findChild(combo.popup.contentItem, "toolbarComboDelegate-0") !== null && findChild(combo.popup.contentItem, "toolbarComboDelegate-1") !== null);
        }

        function init() {
            combo.enabled = true;
            combo.currentIndex = 0;
            combo.popup.close();
            combo.forceActiveFocus();
            wait(0);
        }

        function cleanup() {
            combo.popup.close();
            combo.enabled = true;
            wait(0);
        }

        function test_darkColorsMeetContrastTarget() {
            openPopup();

            const selectedBackground = findChild(combo.popup.contentItem, "toolbarComboDelegateBackground-0");
            const selectedText = findChild(combo.popup.contentItem, "toolbarComboDelegateText-0");
            const normalBackground = findChild(combo.popup.contentItem, "toolbarComboDelegateBackground-1");
            const normalText = findChild(combo.popup.contentItem, "toolbarComboDelegateText-1");
            compare(selectedBackground.color.toString(), combo.highlightColor.toString());
            compare(selectedText.color.toString(), combo.highlightedTextColor.toString());
            compare(normalBackground.color.toString(), combo.panelColor.toString());
            compare(normalText.color.toString(), combo.textColor.toString());
            verify(contrastRatio(selectedText.color, selectedBackground.color) >= 4.5);
            verify(contrastRatio(normalText.color, normalBackground.color) >= 4.5);
        }

        function test_keyboardSelectionAndEscape() {
            openPopup();
            compare(combo.highlightedIndex, 0);

            keyClick(Qt.Key_Down);
            tryCompare(combo, "highlightedIndex", 1);
            const keyboardBackground = findChild(combo.popup.contentItem, "toolbarComboDelegateBackground-1");
            const keyboardText = findChild(combo.popup.contentItem, "toolbarComboDelegateText-1");
            tryCompare(keyboardBackground, "color", combo.highlightColor);
            tryCompare(keyboardText, "color", combo.highlightedTextColor);
            keyClick(Qt.Key_Return);
            tryCompare(combo, "currentIndex", 1);
            tryCompare(combo.popup, "visible", false);

            openPopup();
            keyClick(Qt.Key_Escape);
            tryCompare(combo.popup, "visible", false);
            compare(combo.currentIndex, 1);
        }

        function test_spaceBelongsToFocusedCombo() {
            compare(combo.blocksGlobalMediaShortcuts, true);
            combo.forceActiveFocus();
            keyClick(Qt.Key_Space);
            tryCompare(combo.popup, "visible", true);
            keyClick(Qt.Key_Escape);
            tryCompare(combo.popup, "visible", false);
        }

        function test_tabFocusAndOutsideDismissal() {
            focusProbe.forceActiveFocus();
            keyClick(Qt.Key_Tab);
            tryCompare(combo, "activeFocus", true);
            // Real Tab focus is keyboard navigation, so the focus contract (VcsTheme.js,
            // at Theme.focus) must be up: a 2 px Theme.focus ring while visualFocus is true.
            tryVerify(() => combo.visualFocus);
            const background = findChild(combo, "toolbarComboBackground");
            tryVerify(() => background.border.width === 2);
            verify(Qt.colorEqual(background.border.color, Theme.focus),
                "ring colour must be Theme.focus, the shared focus-ring token");

            openPopup();
            mouseClick(outsideTarget);
            tryCompare(combo.popup, "visible", false);
        }

        // The quiet half of the contract, also with a real event: focus arrives by mouse
        // click from another control, so no ring may appear. The click opens the popup;
        // Escape closes it again.
        function test_mouse_focus_shows_no_ring() {
            const background = findChild(combo, "toolbarComboBackground");
            focusProbe.forceActiveFocus();
            verify(!combo.activeFocus);
            mouseClick(combo, combo.width / 2, combo.height / 2);
            tryVerify(() => combo.activeFocus);
            verify(!combo.visualFocus, "a mouse-focused combo must show no ring");
            tryVerify(() => background.border.width === 1);
            verify(Qt.colorEqual(background.border.color, "transparent"));
            keyClick(Qt.Key_Escape);
            tryCompare(combo.popup, "visible", false);
        }

        function test_disabledColorsRemainReadable() {
            combo.enabled = false;
            const displayText = findChild(combo, "toolbarComboDisplayText");
            const background = findChild(combo, "toolbarComboBackground");
            compare(displayText.color.toString(), combo.disabledTextColor.toString());
            compare(background.color.toString(), combo.disabledPanelColor.toString());
            verify(contrastRatio(displayText.color, background.color) >= 4.5);
        }
    }
}
