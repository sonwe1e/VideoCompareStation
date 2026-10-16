pragma ComponentBehavior: Bound

import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

/*
 * Focus-ring behaviour for the shared offset spin box. ReviewOffsetSpinBox used to ring on
 * bare activeFocus with Theme.accent, so a mouse click left a blue frame behind (the U12
 * three-standards drift). The contract - same as ReviewActionButton and ToolbarCombo - is a
 * 2 px Theme.focus ring for keyboard navigation only: Tab and Backtab light it, a mouse
 * click into the field does not. An editable Qt Quick SpinBox keeps the real focus on its
 * TextInput editor (the control forwards focus-in to the editor and is not a FocusScope),
 * so the cases observe the editor's activeFocus, the component's keyboardFocusRing state
 * and the border itself. Every transition below is a real key or mouse event between
 * neighbours; a forced focus reason proves nothing about traversal, so none is forced
 * here. Deleting the real binding while keeping the comments must fail this suite.
 */
Item {
    id: root

    width: 320
    height: 120

    // Tab neighbours so the spin box is entered and left by real key events. The spin box
    // opts into tab traversal explicitly so the case does not depend on style defaults.
    Item {
        id: beforeProbe

        objectName: "beforeProbe"
        x: 4
        y: 50
        width: 20
        height: 20
        activeFocusOnTab: true
    }

    Dvs.ReviewOffsetSpinBox {
        id: spinBox

        objectName: "reviewOffsetSpinBoxUnderTest"
        x: 40
        y: 44
        activeFocusOnTab: true
    }

    Item {
        id: afterProbe

        objectName: "afterProbe"
        x: 180
        y: 50
        width: 20
        height: 20
        activeFocusOnTab: true
    }

    TestCase {
        name: "ReviewOffsetSpinBox"
        when: windowShown

        function init() {
            root.forceActiveFocus();
            wait(0);
        }

        function test_focus_ring_tracks_real_keyboard_and_mouse_focus() {
            const background = spinBox.background;

            // The editable field must keep its planned text area at the implicit size:
            // with the stock 40 px side indicators it collapsed to zero width, and no
            // real mouse event could ever reach the editor.
            verify(spinBox.contentItem.width >= 40,
                "the editable spin box must keep a clickable text area");

            // Resting: one transparent hairline, no ring.
            compare(background.border.width, 1);
            verify(Qt.colorEqual(background.border.color, "transparent"));

            // Real mouse focus: a click into the field focuses the editor with a mouse
            // reason, never by Tab, so no ring may appear. The point is the editor's
            // centre, computed from the live geometry so the click always lands in the
            // text area beside the step buttons.
            beforeProbe.forceActiveFocus();
            verify(!spinBox.contentItem.activeFocus);
            mouseClick(spinBox, spinBox.leftPadding + spinBox.contentItem.width / 2,
                spinBox.height / 2);
            tryVerify(() => spinBox.contentItem.activeFocus);
            verify(!spinBox.keyboardFocusRing, "a mouse-focused spin box must show no ring");
            tryVerify(() => background.border.width === 1);
            verify(Qt.colorEqual(background.border.color, "transparent"));

            // Real Tab in from the left neighbour: the editor takes the focus and the
            // ring appears.
            beforeProbe.forceActiveFocus();
            keyClick(Qt.Key_Tab);
            tryCompare(spinBox.contentItem, "activeFocus", true);
            tryVerify(() => spinBox.keyboardFocusRing);
            tryVerify(() => background.border.width === 2);
            verify(Qt.colorEqual(background.border.color, Theme.focus),
                "ring colour must be Theme.focus, the shared focus-ring token");

            // Real Tab out to the right neighbour: the ring leaves with the focus.
            keyClick(Qt.Key_Tab);
            tryCompare(spinBox.contentItem, "activeFocus", false);
            verify(!spinBox.keyboardFocusRing);
            tryVerify(() => background.border.width === 1);
            verify(Qt.colorEqual(background.border.color, "transparent"));

            // Real BackTab in from the right neighbour: the ring appears again.
            afterProbe.forceActiveFocus();
            keyClick(Qt.Key_Backtab);
            tryCompare(spinBox.contentItem, "activeFocus", true);
            tryVerify(() => spinBox.keyboardFocusRing);
            tryVerify(() => background.border.width === 2);
            verify(Qt.colorEqual(background.border.color, Theme.focus));
        }

        function test_step_buttons_meet_the_hit_area_minimum() {
            // VcsTheme.js hitAreaMinimum: common interactive controls are at least
            // 32 x 32 logical pixels so a 100% DPI pointer still lands on them. Two
            // such targets cannot stack inside the 34 px control height, so they must
            // sit side by side without overlapping.
            const upButton = spinBox.up.indicator;
            const downButton = spinBox.down.indicator;
            verify(upButton.width >= Theme.hitAreaMinimum && upButton.height >= Theme.hitAreaMinimum,
                "the increase button must keep a 32 x 32 hit area");
            verify(downButton.width >= Theme.hitAreaMinimum && downButton.height >= Theme.hitAreaMinimum,
                "the decrease button must keep a 32 x 32 hit area");
            verify(upButton.x >= downButton.x + downButton.width
                || downButton.x >= upButton.x + upButton.width,
                "the step buttons must not overlap");

            // Real clicks at the buttons' centres, mapped through the control's own
            // coordinates: stepping must work through the full-size hit targets, and a
            // button click must neither grab focus nor light the ring.
            const before = spinBox.value;
            const upCentre = spinBox.mapFromItem(upButton, upButton.width / 2, upButton.height / 2);
            beforeProbe.forceActiveFocus();
            mouseClick(spinBox, upCentre.x, upCentre.y);
            tryCompare(spinBox, "value", before + 1);
            verify(!spinBox.contentItem.activeFocus, "a step button must not grab focus");
            verify(!spinBox.keyboardFocusRing);

            const downCentre = spinBox.mapFromItem(downButton, downButton.width / 2,
                downButton.height / 2);
            mouseClick(spinBox, downCentre.x, downCentre.y);
            tryCompare(spinBox, "value", before);
            verify(!spinBox.contentItem.activeFocus);
            verify(!spinBox.keyboardFocusRing);
        }

        function test_selection_colours_keep_their_tokens() {
            // The accent property stays on Theme.accent on purpose: it colours the text
            // selection inside the editable field, not the focus ring.
            compare(spinBox.accentColor.toString(), Theme.accent.toString());
            compare(spinBox.contentItem.selectionColor, spinBox.accentColor);
            compare(spinBox.contentItem.selectedTextColor, Theme.inverseText);
        }
    }
}
