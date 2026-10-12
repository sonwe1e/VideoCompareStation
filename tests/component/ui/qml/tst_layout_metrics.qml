import QtQuick
import QtTest
import "../../../../src/ui_qml/qml/LayoutMetrics.js" as LayoutMetrics
import "../../../../src/ui_qml/qml/VcsTheme.js" as Theme

/*
 * U01 layout arithmetic, pinned against fixed inputs. Main.qml and ComparisonViewport run the
 * same functions on the live window, so a reflow rule changed here fails the design before it
 * ships. Each case names the panel states it covers: chrome shown/hidden, file sidebar
 * shown/suppressed, inspector docked/drawer.
 */
Item {
    width: 100
    height: 100

    TestCase {
        name: "LayoutMetrics"
        when: windowShown

        function test_stage_constants_match_the_chrome_anchors() {
            compare(LayoutMetrics.canvasEdgeMargin, 14);
            compare(LayoutMetrics.fileSidebarLoaderMargin, 14);
            compare(LayoutMetrics.fileSidebarGap, 8);
        }

        function test_available_canvas_width_across_panel_states() {
            // Wide window, chrome visible, nothing expanded.
            compare(LayoutMetrics.availableCanvasWidth(1440, true, false, 0, false, 0), 1412);
            // File sidebar visible at its 232 default.
            compare(LayoutMetrics.availableCanvasWidth(1440, true, true, 232, false, 0), 1158);
            // Same window with the inspector docked at 288.
            compare(LayoutMetrics.availableCanvasWidth(1440, true, true, 232, true, 288), 870);
            // The drawer inspector overlays and consumes no canvas width.
            compare(LayoutMetrics.availableCanvasWidth(960, true, true, 232, false, 0), 678);
            // Immersive chrome hidden: the whole window is canvas.
            compare(LayoutMetrics.availableCanvasWidth(1440, false, false, 0, false, 0), 1440);
        }

        function test_inspector_docking_respects_the_canvas_guard() {
            verify(LayoutMetrics.inspectorShouldDock(1412, 288, 600));
            // Exactly the guard still docks; one pixel below goes to the drawer.
            verify(LayoutMetrics.inspectorShouldDock(888, 288, 600));
            verify(!LayoutMetrics.inspectorShouldDock(887.5, 288, 600));
            verify(!LayoutMetrics.inspectorShouldDock(800, 288, 600));
            // A wider drag of the inspector docks later, not sooner.
            verify(!LayoutMetrics.inspectorShouldDock(888, 320, 600));
            verify(LayoutMetrics.inspectorShouldDock(920, 320, 600));
        }

        function test_file_sidebar_guard() {
            // The sidebar stays while the canvas (no docked inspector) keeps the guard.
            verify(LayoutMetrics.fileSidebarAllowed(678, 600));
            verify(LayoutMetrics.fileSidebarAllowed(600, 600));
            verify(!LayoutMetrics.fileSidebarAllowed(599, 600));
        }

        function test_panel_width_clamps_match_the_token_bands() {
            compare(LayoutMetrics.clampFileSidebarWidth(999), Theme.fileSidebarWidthMax);
            compare(LayoutMetrics.clampFileSidebarWidth(1), Theme.fileSidebarWidthMin);
            compare(LayoutMetrics.clampFileSidebarWidth(Theme.fileSidebarWidthDefault), 232);
            compare(LayoutMetrics.clampInspectorWidth(999), Theme.inspectorWidthMax);
            compare(LayoutMetrics.clampInspectorWidth(1), Theme.inspectorWidthMin);
            compare(LayoutMetrics.clampInspectorWidth(Theme.inspectorWidthDefault), 288);
        }

        function test_local_strip_share_clamp() {
            compare(LayoutMetrics.clampLocalStripShare(0.9), Theme.localStripShareMax);
            compare(LayoutMetrics.clampLocalStripShare(0.1), Theme.localStripShareMin);
            compare(LayoutMetrics.clampLocalStripShare(0.34), 0.34);
            compare(LayoutMetrics.localStripMinimumHeight, 132);
        }
    }
}
