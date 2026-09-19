import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 400
    height: 300

    // Records the trace-bridge calls the cache makes. The desktop shell injects the real
    // DiagnosticsProbe as a context property; a component test supplies this stub so the grab
    // logic can be exercised without a render window or a trace sink.
    QtObject {
        id: diagnosticsStub
        objectName: "dvsDiagnostics"
        property var stages: []

        function record(stage, value) {
            stages = stages.concat([[stage, value]]);
        }
    }

    // Stands in for the viewport output item the cache samples. It records every grab request so
    // the test can assert when a capture was attempted without depending on a real render pass.
    Item {
        id: grabTarget
        objectName: "grabTarget"
        width: 320
        height: 180
        property int grabRequests: 0
        property var lastRequestedSize: null

        function grabToImage(callback, targetSize) {
            grabRequests += 1;
            lastRequestedSize = targetSize;
            return null;
        }
    }

    Dvs.TimelineThumbnailCache {
        id: cache
        objectName: "thumbnailCache"
        sourceItem: grabTarget
        currentFrame: 0
        totalFrames: 600
        generation: 1
        diagnostics: diagnosticsStub
    }

    TestCase {
        name: "TimelineThumbnailCache"
        when: windowShown

        function init() {
            cache.generation = 1;
            cache.reset();
            cache.currentFrame = 0;
            // Clear the counters last: assigning a sampled frame above starts a grab, and that
            // grab belongs to the reset, not to the test case.
            grabTarget.grabRequests = 0;
            diagnosticsStub.stages = [];
        }

        // Sample interval for a 600-frame clip is every 5th frame, so 0 and 5 are samples and 3 is
        // not. Only sampled frames may start a grab.
        function test_capture_only_samples_the_interval_grid() {
            cache.currentFrame = 5;
            compare(grabTarget.grabRequests, 1, "sampled frame must request a grab");
            cache.currentFrame = 6;
            compare(grabTarget.grabRequests, 1, "non-sampled frame must not request a grab");
        }

        // A sample is captured once. Re-visiting it must not re-grab while it is still cached.
        function test_cached_sample_is_not_re_captured() {
            cache.currentFrame = 10;
            compare(grabTarget.grabRequests, 1);
            cache.urls = Object.assign({}, cache.urls, { 10: "image://stub/10" });
            cache.currentFrame = 15;
            cache.currentFrame = 10;
            compare(grabTarget.grabRequests, 2, "only the uncached sample 15 may start a grab");
            compare(cache.urlForFrame(10), "image://stub/10");
        }

        // urlForFrame stays a pure lookup: an uncached sample returns an empty url so the hover
        // popup hides instead of showing a frame it does not have.
        function test_url_for_uncached_sample_is_empty() {
            compare(cache.urlForFrame(5), "");
            compare(cache.nearestSample(7), 5);
            compare(cache.nearestSample(0), 0);
            // The last frame snaps to the highest sample, or to the final frame when the frame
            // count is not a whole number of intervals.
            compare(cache.nearestSample(594), 595);
            compare(cache.nearestSample(599), 599);
        }

        // The trace bridge reports the requested frame before the grab and the same frame when it
        // completes; a gate or scheduler change must not silently drop either half of the pair.
        function test_trace_bridge_reports_the_requested_frame() {
            cache.currentFrame = 20;
            compare(diagnosticsStub.stages.length, 1);
            compare(diagnosticsStub.stages[0][0], "grab-requested");
            compare(diagnosticsStub.stages[0][1], 20);
        }

        // A new generation replaces the content, so cached samples from the previous generation
        // must not survive it.
        function test_generation_change_resets_cache() {
            cache.urls = Object.assign({}, cache.urls, { 25: "image://stub/25" });
            cache.accountedBytes = 1024;
            cache.generation = 2;
            compare(cache.urlForFrame(25), "");
            compare(cache.accountedBytes, 0);
        }
    }
}
