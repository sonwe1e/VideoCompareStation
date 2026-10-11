import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    QtObject {
        id: controller

        property bool canFirst: true
        property bool canPrevious: true
        property bool canNext: true
        property bool canLast: true
        property bool canPlay: true
        property bool canPause: false
        property bool playing: false
        property bool jogging: false
        property bool jogActive: false
        property int jogDirection: 0
        property real jogRate: 1
        property int firstCalls: 0
        property int previousCalls: 0
        property int nextCalls: 0
        property int lastCalls: 0
        property int toggleCalls: 0
        property var stepCalls: []
        property var jogPressCalls: []
        property int jogReleaseCalls: 0
        property int jogCancelCalls: 0

        function first() {
            firstCalls += 1;
        }
        function previous() {
            previousCalls += 1;
        }
        function next() {
            nextCalls += 1;
        }
        function last() {
            lastCalls += 1;
        }
        function togglePlayback() {
            toggleCalls += 1;
        }
        function stepFrames(delta) {
            const nextSteps = stepCalls.slice(0);
            nextSteps.push(delta);
            stepCalls = nextSteps;
        }
        function jogPress(direction) {
            const nextPresses = jogPressCalls.slice(0);
            nextPresses.push(direction);
            jogPressCalls = nextPresses;
            return true;
        }
        function jogRelease() {
            jogReleaseCalls += 1;
        }
        function jogCancel() {
            jogCancelCalls += 1;
        }
    }

    Dvs.ReviewActions {
        id: actions

        controller: controller
        oneSecondStepFrames: 60
        wipeEnabled: true
        wipePosition: 0.5
    }

    SignalSpy {
        id: wipeSpy

        target: actions
        signalName: "wipePositionRequested"
    }

    SignalSpy {
        id: manualNavigationSpy

        target: actions
        signalName: "manualNavigationRequested"
    }

    TestCase {
        name: "ReviewActions"

        function test_review_commands_do_not_depend_on_transport_buttons() {
            actions.firstFrame();
            actions.previousFrame();
            actions.nextFrame();
            actions.lastFrame();
            actions.togglePlayback();
            actions.stepBackwardFive();
            actions.stepForwardFive();
            actions.stepBackwardSecond();
            actions.stepForwardSecond();

            compare(controller.firstCalls, 1);
            compare(controller.previousCalls, 1);
            compare(controller.nextCalls, 1);
            compare(controller.lastCalls, 1);
            compare(controller.toggleCalls, 1);
            compare(controller.stepCalls, [-5, 5, -60, 60]);
        }

        function test_wipe_keyboard_adjustment_is_clamped() {
            wipeSpy.clear();
            actions.moveWipe(-0.05);
            compare(wipeSpy.count, 1);
            compare(wipeSpy.signalArguments[0][0], 0.45);

            actions.wipePosition = 0.99;
            actions.moveWipe(0.05);
            compare(wipeSpy.signalArguments[1][0], 1.0);
        }

        function test_jog_press_forwards_direction_and_emits_manual_navigation() {
            controller.canNext = true;
            manualNavigationSpy.clear();
            compare(actions.jogPress(1), true);
            compare(controller.jogPressCalls, [1]);
            compare(manualNavigationSpy.count, 1);
        }

        function test_jog_press_at_boundary_is_gated_without_side_effects() {
            controller.canNext = false;
            manualNavigationSpy.clear();
            const pressesBefore = controller.jogPressCalls.length;
            compare(actions.jogPress(1), false);
            compare(controller.jogPressCalls.length, pressesBefore);
            compare(manualNavigationSpy.count, 0);

            controller.canNext = true;
            controller.canPrevious = false;
            compare(actions.jogPress(-1), false);
            compare(controller.jogPressCalls.length, pressesBefore);
            compare(manualNavigationSpy.count, 0);
            controller.canPrevious = true;
        }

        function test_jog_release_and_cancel_forward_to_controller() {
            actions.jogRelease();
            compare(controller.jogReleaseCalls, 1);
            actions.jogCancel();
            compare(controller.jogCancelCalls, 1);
        }

        function test_jog_projection_reads_controller_state() {
            controller.jogging = true;
            controller.jogActive = true;
            controller.jogDirection = -1;
            controller.jogRate = 2;
            compare(actions.jogging, true);
            compare(actions.jogActive, true);
            compare(actions.jogDirection, -1);
            compare(actions.jogRate, 2);
            controller.jogging = false;
            controller.jogActive = false;
            controller.jogDirection = 0;
            controller.jogRate = 1;
        }
    }
}
