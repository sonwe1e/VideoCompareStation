import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

// V01 "全景 + 三局部" strip: three local windows share one ROI and one transform, sample
// rects stay pinned to the same source pixels across zoom/pan, and ROI edges clamp
// half-open inside the frame so no letterbox can bleed into the windows.
Item {
    id: root

    width: 960
    height: 560

    // Stand-in for the live panorama surface: the strip only needs its flattened size.
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
        panels: [
            {
                "slot": 0,
                "x": 0,
                "y": 0,
                "width": 200,
                "height": 400,
                "contentWidth": 200,
                "contentHeight": 112.5
            },
            {
                "slot": 1,
                "x": 200,
                "y": 0,
                "width": 200,
                "height": 400,
                "contentWidth": 200,
                "contentHeight": 112.5
            },
            {
                "slot": 2,
                "x": 400,
                "y": 0,
                "width": 200,
                "height": 400,
                "contentWidth": 200,
                "contentHeight": 112.5
            }
        ]
        sourceInfos: [
            {
                "width": 1920,
                "height": 1080,
                "rotationDegrees": 0
            },
            {
                "width": 1920,
                "height": 1080,
                "rotationDegrees": 0
            },
            {
                "width": 1920,
                "height": 1080,
                "rotationDegrees": 0
            }
        ]
        sourceCount: 3
        sourceLabels: ["GT.mp4", "A.mp4", "B.mp4"]
        roiEnabled: true
        roiLeft: 0.25
        roiTop: 0.25
        roiRight: 0.75
        roiBottom: 0.75
        viewScale: 1
        viewCenterX: 0.5
        viewCenterY: 0.5
        zoomPercent: 100
        smoothSampling: true
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

        function init() {
            strip.roiEnabled = true;
            strip.roiLeft = 0.25;
            strip.roiTop = 0.25;
            strip.roiRight = 0.75;
            strip.roiBottom = 0.75;
            strip.viewScale = 1;
            strip.viewCenterX = 0.5;
            strip.viewCenterY = 0.5;
            strip.zoomPercent = 100;
            wait(0);
        }

        function test_three_windows_exist_and_share_one_roi() {
            const repeater = findChild(strip, "roiTripletWindowRepeater");
            verify(repeater !== null);
            compare(repeater.count, 3);
            const windows = [];
            for (let slot = 0; slot < 3; ++slot) {
                const rect = strip.windowTextureRect(slot);
                verify(Boolean(rect));
                windows.push(rect);
            }
            // Same ROI and same transform: the three rects are identical modulo the panel
            // offset on the flattened panorama (each panel is 1/3 of the texture wide).
            for (let slot = 1; slot < 3; ++slot) {
                near(windows[slot].x - windows[slot - 1].x, 1 / 3);
                near(windows[slot].y, windows[0].y);
                near(windows[slot].width, windows[0].width);
                near(windows[slot].height, windows[0].height);
            }
        }

        function test_roi_change_syncs_all_windows() {
            // At window zoom 100% with the panorama at fit, every window shows the whole
            // shared ROI fitted into its panel's content rect: with static stand-in panels
            // the rect is therefore invariant under pure ROI geometry changes - in the real
            // surface the content rect itself follows the ROI aspect (sourcePanelRects
            // contentWidth/Height are the ROI-shaped fitted extent). Sync is observable in
            // the half-open clamp: an ROI touching the frame edge loses the last texel row,
            // and that loss must reach all three windows identically.
            const fitted = [strip.windowTextureRect(0), strip.windowTextureRect(1), strip.windowTextureRect(2)];
            for (let slot = 0; slot < 3; ++slot) {
                verify(Boolean(fitted[slot]));
                near(fitted[slot].x - slot / 3, 0);
                near(fitted[slot].width, 200 / 600);
                near(fitted[slot].height, 112.5 / 400);
            }
            strip.roiLeft = 0.5;
            strip.roiTop = 0.5;
            strip.roiRight = 1.0;
            strip.roiBottom = 1.0;
            wait(0);
            // The half-open clamp keeps the right/bottom edge at the last valid pixel
            // centre (1919.5/1920 and 1079.5/1080), so on each axis the crop covers
            // 2*edge - 1 of the (unclamped) sample.
            const expectedX = 2 * (1919.5 / 1920) - 1;
            const expectedY = 2 * (1079.5 / 1080) - 1;
            const after = [strip.windowTextureRect(0), strip.windowTextureRect(1), strip.windowTextureRect(2)];
            for (let slot = 0; slot < 3; ++slot) {
                verify(Boolean(after[slot]));
                near(after[slot].x - slot / 3, 0);
                near(after[slot].width, expectedX * (200 / 600));
                near(after[slot].height, expectedY * (112.5 / 400));
                verify(Math.abs(fitted[slot].width - after[slot].width) > 0.000001);
            }
            // Still one shared ROI: the delta is again only the panel offset.
            near(after[1].x - after[0].x, 1 / 3);
            near(after[2].x - after[1].x, 1 / 3);
            near(after[1].width, after[0].width);
            near(after[2].width, after[0].width);
        }

        function test_sample_stays_inside_content_bounds() {
            // Zoom the panorama so the ROI is larger than the visible sample: the windows
            // must clamp to what the panorama actually shows and never sample letterbox.
            strip.viewScale = 2;
            strip.viewCenterX = 0.6;
            wait(0);
            for (let slot = 0; slot < 3; ++slot) {
                const rect = strip.windowTextureRect(slot);
                verify(Boolean(rect));
                const boundsLeft = slot / 3;
                const boundsRight = (slot + 1) / 3;
                verify(rect.x >= boundsLeft - 0.000001);
                verify(rect.x + rect.width <= boundsRight + 0.000001);
            }
        }

        function test_half_open_boundary_clamps_to_last_pixel() {
            strip.roiLeft = 0.5;
            strip.roiTop = 0.5;
            strip.roiRight = 1.0;
            strip.roiBottom = 1.0;
            wait(0);
            const clamped = strip.clampedOrientedRoi(0);
            verify(Boolean(clamped));
            // Half-open [0,w) x [0,h): the right/bottom edges stay at the last valid pixel
            // centre, so the sampler cannot average in the texel at w or h.
            verify((clamped.left + clamped.width) * 1920 <= 1920 - 0.5 + 0.000001);
            verify((clamped.top + clamped.height) * 1080 <= 1080 - 0.5 + 0.000001);
            verify(clamped.left >= 0);
            verify(clamped.top >= 0);
            // The same clamp reaches the sampled rects.
            const rect = strip.windowTextureRect(0);
            verify(Boolean(rect));
            verify(rect.x + rect.width <= 1 / 3 + 0.000001);
        }

        function test_zoom_steps_shrink_sample_proportionally() {
            const wide = strip.windowTextureRect(0);
            strip.zoomPercent = 200;
            wait(0);
            const half = strip.windowTextureRect(0);
            verify(Boolean(half));
            // 200% shows half the ROI width about its centre.
            near(half.width, wide.width / 2);
            near(half.x, wide.x + wide.width / 4);
            strip.zoomPercent = 400;
            wait(0);
            const quarter = strip.windowTextureRect(0);
            verify(Boolean(quarter));
            near(quarter.width, wide.width / 4);
            near(quarter.x, wide.x + wide.width * 3 / 8);
        }

        function test_zoom_button_cycles_shared_steps() {
            const button = findChild(strip, "roiTripletZoomButton");
            verify(button !== null);
            compare(strip.zoomPercent, 100);
            zoomSpy.clear();
            button.clicked();
            compare(zoomSpy.count, 1);
            compare(zoomSpy.signalArguments[0][0], 200);
            strip.zoomPercent = 400;
            button.clicked();
            compare(zoomSpy.signalArguments[1][0], 100);
        }

        function test_transform_mapping_follows_panorama_zoom() {
            // Panorama zoom 2 about the centre plus window zoom 200%: the visible sample and
            // the window crop both cover the middle half of the ROI, so the window maps
            // exactly onto the panel's whole content rect.
            strip.viewScale = 2;
            strip.viewCenterX = 0.5;
            strip.viewCenterY = 0.5;
            strip.zoomPercent = 200;
            wait(0);
            let rect = strip.windowTextureRect(0);
            verify(Boolean(rect));
            near(rect.x, 0);
            near(rect.y, 143.75 / 400);
            near(rect.width, 200 / 600);
            near(rect.height, 112.5 / 400);
            // Off-centre panorama pan at window zoom 100%: the crop (whole ROI) extends
            // beyond the visible sample, so the window clamps to what the panorama shows -
            // it fills the content rect exactly, pinned to its left/top edge, and its right
            // edge stays at the content bounds.
            strip.viewCenterX = 0.45;
            strip.zoomPercent = 100;
            wait(0);
            rect = strip.windowTextureRect(0);
            verify(Boolean(rect));
            near(rect.x, 0);
            near(rect.y, 143.75 / 400);
            near(rect.width, 200 / 600);
            near(rect.height, 112.5 / 400);
            near(rect.x + rect.width, 200 / 600);
        }

        function test_below_fit_keeps_the_whole_roi_in_the_sample() {
            // Below fit the visible fraction is capped at 1 (1/max(scale,1)), so the sample
            // stays the whole ROI while the panel content shrinks about its centre (to
            // 100x56.25 centred at (50, 171.875)); the window still covers that entire
            // shrunk content rect instead of sliding out of the ROI.
            strip.viewScale = 0.5;
            strip.viewCenterX = 0.5;
            strip.viewCenterY = 0.5;
            wait(0);
            const rect = strip.windowTextureRect(0);
            verify(Boolean(rect));
            near(rect.x, 50 / 600);
            near(rect.y, 171.875 / 400);
            near(rect.width, 100 / 600);
            near(rect.height, 56.25 / 400);
            strip.viewScale = 1;
        }

        function test_rotation_maps_roi_into_oriented_frame() {
            strip.sourceInfos[0].rotationDegrees = 90;
            wait(0);
            const clamped = strip.clampedOrientedRoi(0);
            verify(Boolean(clamped));
            // 90 degrees: source (l .2, t .3, r .6, b .8) maps to oriented
            // (left .3, top .4, width .5, height .4); oriented frame is 1080x1920.
            strip.roiLeft = 0.2;
            strip.roiTop = 0.3;
            strip.roiRight = 0.6;
            strip.roiBottom = 0.8;
            wait(0);
            const rotated = strip.clampedOrientedRoi(0);
            verify(Boolean(rotated));
            near(rotated.left, 0.3);
            near(rotated.top, 0.4);
            near(rotated.width, 0.5);
            near(rotated.height, 0.4);
            compare(rotated.orientedWidth, 1080);
            compare(rotated.orientedHeight, 1920);
            strip.sourceInfos[0].rotationDegrees = 0;
        }

        function test_smooth_flag_controls_snapshot_filtering() {
            const snapshot = findChild(strip, "roiTripletSnapshot");
            verify(snapshot !== null);
            compare(snapshot.smooth, true);
            strip.smoothSampling = false;
            wait(0);
            compare(snapshot.smooth, false);
            strip.smoothSampling = true;
        }

        function test_hint_and_label_chrome() {
            const hint = findChild(strip, "roiTripletHint");
            verify(hint !== null);
            strip.roiEnabled = false;
            wait(0);
            compare(hint.visible, true);
            strip.roiEnabled = true;
            wait(0);
            compare(hint.visible, false);
            const labels = [];
            for (let slot = 0; slot < 3; ++slot) {
                const cell = findChild(strip, "roiTripletWindowRepeater").itemAt(slot);
                verify(cell !== null);
                const label = findChild(cell, "roiTripletWindowLabel");
                verify(label !== null);
                labels.push(label.text);
            }
            compare(labels[0], "GT.mp4");
            compare(labels[1], "A.mp4");
            compare(labels[2], "B.mp4");
        }

        function test_missing_frame_rejects_window() {
            strip.sourceInfos[2] = {
                "width": 0,
                "height": 0,
                "rotationDegrees": 0
            };
            wait(0);
            verify(strip.windowTextureRect(2) === null);
            verify(strip.windowTextureRect(0) !== null);
            strip.sourceInfos[2] = {
                "width": 1920,
                "height": 1080,
                "rotationDegrees": 0
            };
            wait(0);
            verify(strip.windowTextureRect(2) !== null);
        }
    }
}
