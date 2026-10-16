pragma ComponentBehavior: Bound

import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 960
    height: 640

    Dvs.VcsMenu {
        id: rootMenu

        objectName: "rootMenu"
        title: "Root"

        Dvs.VcsMenuItem {
            objectName: "rootMenuItem"
            text: "Root action"
        }

        Dvs.VcsMenu {
            id: nestedMenu

            objectName: "nestedMenu"
            title: "Nested"

            Dvs.VcsMenuItem {
                objectName: "nestedMenuItem"
                text: "Nested action"
            }
        }

        Dvs.VcsRadioMenuItem {
            id: radioMenuItem

            objectName: "radioMenuItem"
            text: "Radio action"
            checked: true
        }

        Dvs.VcsMenuItem {
            id: checkMenuItem

            objectName: "checkMenuItem"
            text: "Check action"
            checkable: true
            checked: true
        }

        Dvs.VcsMenuItem {
            id: hiddenMenuItem

            objectName: "hiddenMenuItem"
            text: "Hidden action"
            visible: false
        }

        Dvs.VcsMenuSeparator {
            id: hiddenSeparator

            objectName: "hiddenSeparator"
            visible: false
        }
    }

    TestCase {
        name: "VcsMenu"
        when: windowShown

        function cleanup() {
            nestedMenu.close();
            rootMenu.close();
            wait(0);
        }

        function test_uses_non_native_opaque_popup_and_custom_delegate() {
            compare(rootMenu.popupType, Popup.Window);
            compare(nestedMenu.popupType, Popup.Window);
            compare(rootMenu.background.color.a, 1.0);
            compare(nestedMenu.background.color.a, 1.0);
            compare(rootMenu.background.border.color.a, 1.0);
            compare(nestedMenu.background.border.color.a, 1.0);
            verify(rootMenu.popupInputContext);
            verify(findChild(rootMenu, "rootMenuItem").popupInputContext);

            rootMenu.popup(root, 24, 24);
            tryCompare(rootMenu, "opened", true);
            verify(rootMenu.x >= 0);
            verify(rootMenu.y >= 0);

            nestedMenu.popup(root, 320, 24);
            tryCompare(nestedMenu, "opened", true);
            verify(nestedMenu.x >= 0);
            verify(nestedMenu.y >= 0);
        }

        function test_disabled_menu_rejects_programmatic_open() {
            rootMenu.enabled = false;
            rootMenu.open();
            wait(0);
            compare(rootMenu.opened, false);
            rootMenu.enabled = true;
        }

        function test_radio_and_checkbox_indicators_keep_distinct_semantics() {
            rootMenu.popup(root, 24, 24);
            tryCompare(rootMenu, "opened", true);
            const radioIndicator = findChild(rootMenu, "radioMenuItemIndicator");
            const radioDot = findChild(rootMenu, "radioMenuItemRadioDot");
            const checkIndicator = findChild(rootMenu, "checkMenuItemIndicator");
            const checkDot = findChild(rootMenu, "checkMenuItemRadioDot");
            verify(radioIndicator !== null);
            verify(radioDot !== null);
            verify(checkIndicator !== null);
            verify(checkDot !== null);

            compare(radioMenuItem.radioIndicator, true);
            compare(radioMenuItem.autoExclusive, true);
            compare(radioMenuItem.checked, true);
            compare(radioIndicator.radius, radioIndicator.width / 2);
            compare(radioDot.visible, true);
            compare(checkMenuItem.radioIndicator, false);
            compare(checkIndicator.radius, 3);
            compare(checkDot.visible, false);
        }

        function test_hidden_rows_and_separators_collapse_to_zero_height() {
            compare(hiddenMenuItem.implicitHeight, 0);
            compare(hiddenMenuItem.height, 0);
            compare(hiddenSeparator.implicitHeight, 0);
            compare(hiddenSeparator.height, 0);

            nestedMenu.menuItemVisible = false;
            wait(0);
            const nestedRow = rootMenu.itemAt(1);
            verify(nestedRow !== null);
            compare(nestedRow.visible, false);
            compare(nestedRow.implicitHeight, 0);
            compare(nestedRow.height, 0);
            nestedMenu.menuItemVisible = true;
        }

        function test_escape_closes_popup() {
            rootMenu.popup(root, 24, 24);
            tryCompare(rootMenu, "opened", true);
            keyClick(Qt.Key_Escape);
            tryCompare(rootMenu, "opened", false);
        }
    }
}
