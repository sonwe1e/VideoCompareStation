pragma ComponentBehavior: Bound

import QtQuick

QtObject {
    id: cache

    objectName: "timelineThumbnailCache"

    required property Item sourceItem
    required property int currentFrame
    required property int totalFrames
    required property var generation
    // UI trace bridge (DiagnosticsProbe). Optional so a component test can supply a stub; when
    // absent, or when tracing is disabled, recording is a no-op.
    property var diagnostics: (typeof dvsDiagnostics !== "undefined") ? dvsDiagnostics : null
    property int targetWidth: 176
    property int targetHeight: 99
    property int maximumBytes: 8 * 1024 * 1024
    readonly property int sampleInterval: Math.max(1, Math.ceil(Math.max(1, totalFrames) / 120))
    property var urls: ({})
    property var handles: ({})
    property var lruFrames: []
    property int accountedBytes: 0

    function reset() {
        urls = {};
        handles = {};
        lruFrames = [];
        accountedBytes = 0;
    }

    function nearestSample(frame) {
        return Math.max(0, Math.min(Math.max(0, totalFrames - 1), Math.round(Number(frame) / sampleInterval) * sampleInterval));
    }

    function touch(frame) {
        const next = lruFrames.filter(value => Number(value) !== Number(frame));
        next.push(frame);
        lruFrames = next;
    }

    function capture(frame) {
        if (!sourceItem || sourceItem.width <= 0 || sourceItem.height <= 0 || frame < 0)
            return;
        const sample = nearestSample(frame);
        if (sample !== Number(frame) || urls[sample] !== undefined)
            return;
        const requestedGeneration = generation;
        const requestedFrame = Number(frame);
        if (diagnostics)
            diagnostics.record("grab-requested", requestedFrame);
        sourceItem.grabToImage(result => {
            if (diagnostics)
                diagnostics.record("grab-completed", requestedFrame);
            if (requestedGeneration !== cache.generation || requestedFrame !== cache.currentFrame || !result || !result.url)
                return;
            const nextUrls = Object.assign({}, cache.urls);
            const nextHandles = Object.assign({}, cache.handles);
            nextUrls[sample] = result.url;
            nextHandles[sample] = result;
            cache.urls = nextUrls;
            cache.handles = nextHandles;
            cache.accountedBytes += cache.targetWidth * cache.targetHeight * 4;
            cache.touch(sample);
            while (cache.accountedBytes > cache.maximumBytes && cache.lruFrames.length > 1) {
                const evicted = cache.lruFrames[0];
                cache.lruFrames = cache.lruFrames.slice(1);
                const prunedUrls = Object.assign({}, cache.urls);
                const prunedHandles = Object.assign({}, cache.handles);
                delete prunedUrls[evicted];
                delete prunedHandles[evicted];
                cache.urls = prunedUrls;
                cache.handles = prunedHandles;
                cache.accountedBytes -= cache.targetWidth * cache.targetHeight * 4;
            }
        }, Qt.size(targetWidth, targetHeight));
    }

    function previewInfoForFrame(frame) {
        if (frame < 0 || totalFrames <= 0) {
            return {
                url: "",
                sampleFrame: -1,
                isExact: false
            };
        }
        const target = Number(frame);
        const sample = nearestSample(target);
        if (urls[sample] !== undefined) {
            touch(sample);
            return {
                url: urls[sample],
                sampleFrame: sample,
                isExact: sample === target
            };
        }
        let closestSample = -1;
        let minDiff = Infinity;
        const keys = Object.keys(urls);
        for (let i = 0; i < keys.length; ++i) {
            const k = Number(keys[i]);
            const diff = Math.abs(k - target);
            if (diff < minDiff) {
                minDiff = diff;
                closestSample = k;
            }
        }
        // Only borrow a neighbour sample within one grid step; farther frames would mislead.
        if (closestSample >= 0 && urls[closestSample] !== undefined && Math.abs(closestSample - target) <= sampleInterval) {
            return {
                url: urls[closestSample],
                sampleFrame: closestSample,
                isExact: false
            };
        }
        return {
            url: "",
            sampleFrame: -1,
            isExact: false
        };
    }

    function urlForFrame(frame) {
        return previewInfoForFrame(frame).url;
    }

    onCurrentFrameChanged: capture(currentFrame)
    onGenerationChanged: reset()
}
