import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

// V01 "panorama + locals" strip, renderer-direct-sampling edition: the strip stages each
// window cell's content rect into surface coordinates, mirrors published footage extents as
// letterbox chrome, and cycles the 适配ROI/100%/200%/400% zoom ladder. All sampling geometry
// itself lives in the C++ pure functions and is covered there; these tests pin the staging
// contract and the chrome degradation.
Item {
    id: root

    width: 960
    height: 560

    // Stand-in for the live panorama surface: the strip only maps into its coordinates.
    Item {
        id: dummySurface

        width: 600
        height: 400
    }

    Dvs.RoiTripletStrip {
        id: strip

        objectName: "strip"
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
        height: 200
        sourceItem: dummySurface
        sourceCount: 3
        sourceLabels: ["GT.mp4", "A.mp4", "B.mp4"]
        roiEnabled: true
        zoomPercent: 0
        smoothSampling: true
        windowRects: []
    }

    SignalSpy {
        id: zoomSpy

        target: strip
        signalName: "zoomStepRequested"
    }

    TestCase {
        name: "RoiTripletStrip"
        when: windowShown

        function near(actual, expected) {
            verify(Math.abs(actual - expected) < 0.000001, "expected " + expected + ", got " + actual);
        }

        function cell(slot) {
            return strip.cellItem(slot);
        }

        function init() {
            strip.roiEnabled = true;
            strip.sourceCount = 3;
            strip.zoomPercent = 0;
            strip.smoothSampling = true;
            strip.windowRects = [];
            wait(0);
        }

        function test_windows_exist_per_source_count() {
            const repeater = findChild(strip, "roiTripletWindowRepeater");
            verify(repeater !== null);
            compare(repeater.count, 3);
            strip.sourceCount = 2;
            wait(0);
            compare(repeater.count, 2);
            strip.sourceCount = 3;
            wait(0);
            compare(repeater.count, 3);
        }

        function test_staged_windows_cover_cell_content_rects() {
            const staged = strip.stagedWindows;
            const row = findChild(strip, "roiTripletWindowRepeater").parent;
            compare(staged.length, 3);
            const cellWidth = (row.width - 2 * row.spacing) / 3;
            for (let slot = 0; slot < 3; ++slot) {
                const entry = staged[slot];
                compare(Number(entry.slot), slot);
                // The content rect is the cell minus the 8 px chrome inset, mapped into the
                // surface item's coordinates.
                near(entry.width, cellWidth - 16);
                near(entry.height, cell(slot).height - 16);
                compare(entry.zoomPercent, 0);
                compare(entry.smoothSampling, true);
            }
            // Same strip, same row: the entries differ only by the column offset.
            near(staged[1].x - staged[0].x, cellWidth + row.spacing);
            near(staged[2].x - staged[1].x, cellWidth + row.spacing);
            near(staged[1].y, staged[0].y);
            near(staged[2].y, staged[0].y);
            // The mapping really lands inside the stand-in surface coordinates.
            verify(staged[0].x >= dummySurface.x);
            verify(staged[0].y >= dummySurface.y + dummySurface.height);
        }

        function test_staged_windows_follow_zoom_and_filter() {
            strip.zoomPercent = 200;
            strip.smoothSampling = false;
            wait(0);
            const staged = strip.stagedWindows;
            compare(staged.length, 3);
            for (let slot = 0; slot < 3; ++slot) {
                compare(Number(staged[slot].zoomPercent), 200);
                compare(staged[slot].smoothSampling, false);
            }
        }

        function test_hidden_strip_stages_nothing() {
            strip.visible = false;
            wait(0);
            compare(strip.stagedWindows.length, 0);
            strip.visible = true;
            wait(0);
            compare(strip.stagedWindows.length, 3);
        }

        function test_zoom_button_cycles_fit_then_source_pixel_steps() {
            const button = findChild(strip, "roiTripletZoomButton");
            verify(button !== null);
            compare(strip.zoomPercent, 0);
            compare(button.text, qsTr("局部 适配ROI"));
            zoomSpy.clear();
            button.clicked();
            compare(zoomSpy.count, 1);
            compare(zoomSpy.signalArguments[0][0], 100);
            strip.zoomPercent = 100;
            wait(0);
            compare(button.text, qsTr("局部 100%"));
            strip.zoomPercent = 200;
            button.clicked();
            compare(zoomSpy.signalArguments[1][0], 400);
            strip.zoomPercent = 400;
            button.clicked();
            // 400% wraps back to 适配ROI.
            compare(zoomSpy.signalArguments[2][0], 0);
        }

        function test_published_footage_extents_drive_letterbox_chrome() {
            const row = findChild(strip, "roiTripletWindowRepeater").parent;
            const cellWidth = (row.width - 2 * row.spacing) / 3;
            // No published extents: every window reports unavailable and the bars cover it.
            for (let slot = 0; slot < 3; ++slot) {
                verify(!cell(slot).footageValid);
                const unavailable = findChild(cell(slot), "roiTripletWindowUnavailable");
                verify(unavailable !== null);
                compare(unavailable.visible, true);
            }
            strip.windowRects = [
                { "slot": 0, "valid": true, "width": 100, "height": 80 },
                {
                    "slot": 1,
                    "valid": true,
                    "width": cellWidth - 16,
                    "height": cell(1).height - 16
                },
                { "slot": 2, "valid": false, "width": 0, "height": 0 }
            ];
            wait(0);
            verify(cell(0).footageValid);
            // Footage is centred inside the content rect.
            near(cell(0).footageX, (cell(0).width - 100) / 2);
            near(cell(0).footageY, (cell(0).height - 80) / 2);
            verify(cell(1).footageValid);
            verify(!cell(2).footageValid);
            const unavailable = findChild(cell(2), "roiTripletWindowUnavailable");
            compare(unavailable.visible, true);
        }

        function test_hint_follows_roi_state() {
            const hint = findChild(strip, "roiTripletHint");
            verify(hint !== null);
            strip.roiEnabled = false;
            wait(0);
            compare(hint.visible, true);
            strip.roiEnabled = true;
            wait(0);
            compare(hint.visible, false);
        }

        function test_labels_follow_source_list() {
            for (let slot = 0; slot < 3; ++slot) {
                const label = findChild(cell(slot), "roiTripletWindowLabel");
                verify(label !== null);
                compare(label.text, ["GT.mp4", "A.mp4", "B.mp4"][slot]);
                compare(label.visible, true);
            }
            strip.sourceCount = 2;
            wait(0);
            verify(cell(2) === null);
        }
    }
}
