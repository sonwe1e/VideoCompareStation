import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 720
    height: 400

    Item {
        id: surface

        x: 40
        y: 40
        width: 640
        height: 320
    }

    Dvs.WipeHandle {
        id: handle

        surfaceItem: surface
        position: 0.5
    }

    SignalSpy {
        id: positionSpy

        target: handle
        signalName: "positionRequested"
    }

    TestCase {
        name: "WipeHandle"
        when: windowShown

        function test_geometry_tracks_surface_split() {
            const knob = findChild(handle, "wipeKnob");
            verify(knob !== null);
            compare(handle.width, 52);
            compare(knob.width, 20);
            compare(knob.height, 84);

            for (const position of [0.25, 0.5, 0.75]) {
                handle.position = position;
                wait(0);
                const actualCenter = handle.x + handle.width / 2;
                const expectedCenter = surface.x + surface.width * position;
                verify(Math.abs(actualCenter - expectedCenter) * Screen.devicePixelRatio <= 1);
            }
        }

        // UX-15 (handle path): Tab focus lights the 2 px ring, and the arrow keys move the
        // split along the handle's own axis - 1% per press, Shift for 5% - with the same
        // axis gating as the global Alt+arrows: the wrong arrow pair is ignored.
        function test_keyboard_arrows_step_along_own_axis() {
            const ring = findChild(handle, "wipeFocusRing");
            verify(ring !== null);
            verify(!ring.visible);

            handle.forceActiveFocus();
            verify(handle.activeFocus);
            verify(ring.visible);

            handle.position = 0.5;
            positionSpy.clear();
            keyClick(Qt.Key_Left);
            compare(positionSpy.count, 1);
            compare(positionSpy.signalArguments[0][0], 0.49);
            keyClick(Qt.Key_Right);
            compare(positionSpy.count, 2);
            compare(positionSpy.signalArguments[1][0], 0.51);

            keyClick(Qt.Key_Right, Qt.ShiftModifier);
            compare(positionSpy.count, 3);
            // No host rewinds `position` in this harness, so every press steps from the 0.5
            // set above; the shift-sized press pins the 5% delta on its own.
            compare(positionSpy.signalArguments[2][0], 0.55);

            // Vertical split: Up/Down are the wrong axis and stay silent.
            keyClick(Qt.Key_Up);
            keyClick(Qt.Key_Down);
            compare(positionSpy.count, 3);

            // Horizontal split mirrors the gate: Up/Down move, Left/Right stay silent.
            handle.orientation = 1;
            handle.position = 0.5;
            positionSpy.clear();
            keyClick(Qt.Key_Up);
            compare(positionSpy.count, 1);
            compare(positionSpy.signalArguments[0][0], 0.49);
            keyClick(Qt.Key_Left);
            compare(positionSpy.count, 1);
            handle.orientation = 0;
        }

        function test_keyboard_steps_clamp_to_endpoints() {
            handle.forceActiveFocus();

            handle.position = 0.005;
            positionSpy.clear();
            keyClick(Qt.Key_Left, Qt.ShiftModifier);
            compare(positionSpy.count, 1);
            compare(positionSpy.signalArguments[0][0], 0);

            handle.position = 0.995;
            positionSpy.clear();
            keyClick(Qt.Key_Right, Qt.ShiftModifier);
            compare(positionSpy.count, 1);
            compare(positionSpy.signalArguments[0][0], 1);
        }
    }
}
