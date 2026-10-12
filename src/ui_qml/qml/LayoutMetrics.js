.pragma library

// U01 responsive layout arithmetic. Pure functions only: the same math runs in Main.qml and
// ComparisonViewport on the live window and in tst_layout_metrics.qml against fixed inputs, so
// the reflow rules cannot drift between design and implementation. Sizes are Qt logical
// pixels and come from the VcsTheme tokens; nothing here reads Qt types or item state.

.import "VcsTheme.js" as Theme

// Stage geometry the chrome reserves: the canvas keeps an edge margin on both sides while the
// chrome is visible, the file sidebar's loader sits inside the left margin, and the canvas
// starts one gap right of the sidebar's edge.
var canvasEdgeMargin = 14
var fileSidebarLoaderMargin = 14
var fileSidebarGap = 8

function clamp(value, minimum, maximum) {
    return Math.max(minimum, Math.min(maximum, value));
}

// Stage width the file sidebar consumes while it is visible.
function fileSidebarStageWidth(sidebarWidth) {
    return fileSidebarLoaderMargin + sidebarWidth + fileSidebarGap;
}

// Canvas width after every panel that actually consumes stage width. A drawer inspector
// overlays the canvas and consumes nothing; a docked inspector consumes its full width. This
// is the number the reflow decisions read - never the raw window width.
function availableCanvasWidth(windowWidth, chromeVisible, fileSidebarVisible, fileSidebarWidth, inspectorDocked, inspectorWidth) {
    let width = windowWidth;
    if (chromeVisible)
        width -= 2 * canvasEdgeMargin;
    if (fileSidebarVisible)
        width -= fileSidebarStageWidth(fileSidebarWidth);
    if (inspectorDocked)
        width -= inspectorWidth;
    return width;
}

// The inspector docks only when the canvas would still keep the guard after docking; below
// that the existing drawer overlays instead of squeezing the canvas under the guard.
function inspectorShouldDock(availableWithoutInspector, inspectorWidth, canvasGuard) {
    return availableWithoutInspector - inspectorWidth >= canvasGuard;
}

// The file sidebar stays shown only while the canvas (with no docked inspector claiming
// width) would keep the guard. When the guard fails the sidebar is temporarily suppressed -
// the user's visible flag is untouched, so widening the window brings it back.
function fileSidebarAllowed(availableWithoutSidebar, canvasGuard) {
    return availableWithoutSidebar >= canvasGuard;
}

// Panel resize clamps. The defaults and bands come from the theme tokens so a token change
// moves the clamp with it.
function clampFileSidebarWidth(width) {
    return clamp(width, Theme.fileSidebarWidthMin, Theme.fileSidebarWidthMax);
}

function clampInspectorWidth(width) {
    return clamp(width, Theme.inspectorWidthMin, Theme.inspectorWidthMax);
}

// The local-comparison strip claims a share of the workspace height inside its band; the
// 132 px floor below keeps short workspaces readable rather than proportionally tiny.
function clampLocalStripShare(share) {
    return clamp(share, Theme.localStripShareMin, Theme.localStripShareMax);
}

var localStripMinimumHeight = 132
