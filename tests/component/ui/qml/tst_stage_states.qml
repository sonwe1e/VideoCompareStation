import QtQuick
import QtTest
import "../../../../src/ui_qml/qml/StageStates.js" as StageStates

/*
 * U03 five-state distinction, pinned as pure logic. Main.qml derives its stageStateId from
 * the same function over the same booleans the display surfaces read, so this matrix is the
 * contract both sides must keep: exactly one state for every input combination, and the
 * documented priority (failure > opening > blank > waiting > missing > ready).
 */
Item {
    width: 100
    height: 100

    TestCase {
        name: "StageStates"
        when: windowShown

        function test_state_ids_are_the_documented_five_plus_ready() {
            compare(StageStates.empty, "empty");
            compare(StageStates.opening, "opening");
            compare(StageStates.waitingFrame, "waitingFrame");
            compare(StageStates.openFailed, "openFailed");
            compare(StageStates.missingFrame, "missingFrame");
            compare(StageStates.ready, "ready");
            compare(StageStates.allStates.length, 6);
        }

        function test_every_input_combination_resolves_to_exactly_one_state() {
            const flags = [false, true];
            const seen = [];
            for (const hasContent of flags) {
                for (const graphicsReady of flags) {
                    for (const busy of flags) {
                        for (const frameShown of flags) {
                            for (const hasErrors of flags) {
                                for (const waiting of flags) {
                                    for (const missing of flags) {
                                        const state = StageStates.resolveStageState(hasContent, graphicsReady, busy, frameShown, hasErrors, waiting, missing);
                                        verify(StageStates.allStates.indexOf(state) >= 0, "unknown state " + state);
                                        if (seen.indexOf(state) < 0)
                                            seen.push(state);
                                    }
                                }
                            }
                        }
                    }
                }
            }
            compare(seen.length, StageStates.allStates.length);
            for (const state of StageStates.allStates)
                verify(seen.indexOf(state) >= 0, "state " + state + " is unreachable");
        }

        function test_first_blank_is_empty_until_work_starts() {
            compare(StageStates.resolveStageState(false, true, false, false, false, false, false), StageStates.empty);
        }

        function test_opening_when_a_new_task_has_no_old_picture() {
            compare(StageStates.resolveStageState(true, true, true, false, false, false, false), StageStates.opening);
            // Even with the projection already torn down (content flag lost), a busy open
            // with no frame still reads as opening, not blank.
            compare(StageStates.resolveStageState(false, true, true, false, false, false, false), StageStates.opening);
        }

        function test_old_picture_waits_instead_of_being_masked() {
            // Busy over a kept frame: the old picture stays visible (no fullscreen mask).
            compare(StageStates.resolveStageState(true, true, true, true, false, false, false), StageStates.waitingFrame);
            // The delayed next-frame indicator is the same state.
            compare(StageStates.resolveStageState(true, true, false, true, false, true, false), StageStates.waitingFrame);
        }

        function test_failures_win_but_keep_the_old_session() {
            // Graphics gone: always a failure.
            compare(StageStates.resolveStageState(true, false, false, true, false, false, false), StageStates.openFailed);
            // Error with no frame to keep: the failure overlay owns the stage.
            compare(StageStates.resolveStageState(true, true, false, false, true, false, false), StageStates.openFailed);
            // Error with a kept frame: still openFailed - the banner explains it while the
            // old session stays behind it.
            compare(StageStates.resolveStageState(true, true, false, true, true, false, false), StageStates.openFailed);
        }

        function test_missing_source_frame_needs_a_shown_frame() {
            compare(StageStates.resolveStageState(true, true, false, true, false, false, true), StageStates.missingFrame);
            // Without a frame there is nothing to be missing from: the per-source missing
            // flags default to true before the first frame and must stay silent.
            compare(StageStates.resolveStageState(true, true, false, false, false, false, true), StageStates.ready);
        }

        function test_ready_is_the_quiet_default() {
            compare(StageStates.resolveStageState(true, true, false, true, false, false, false), StageStates.ready);
        }
    }
}
