import QtQuick
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

// Image workspace suite, driven through the REAL services the C++ runner installs
// (ImageWorkspaceQmlTestMain.cpp): a real ImageReviewController fed with small injected
// images, a real ColorClipboardController, and the review image provider. Mouse and
// keyboard events are real throughout. The runner exists because the stock qmltestrunner
// cannot register Dvs.Ui types, and the typed workspace rejects QtObject stand-ins at
// component compile time.
Item {
    id: root

    width: 960
    height: 640

    // A real decoded 1x1 PNG for the edit-controller stand-in's derived URL.
    readonly property string tinyPng: "data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR42mNk+M9QDwADhgGAWjR9awAAAABJRU5ErkJggg=="

    readonly property var imageReview: fixture.controller

    QtObject {
        id: pairModel

        property int pairCount: 0
        // Stand-ins for the QObject model surface the workspace and the sidebar read
        // (paths, error text and the ListView delegate roles). Without them the sidebar
        // bindings assign [undefined] into QString properties and the ListView treats
        // the bare QtObject as a one-row model whose delegate misses its required roles.
        property string errorText: ""
        property string leftFolderPath: ""
        property string rightFolderPath: ""
        property string fileName: ""
        property bool hasLeft: false
        property bool hasRight: false
        property bool hasBoth: false
        property bool caseConflict: false
        property string caseConflictDetail: ""
    }

    // Minimal edit-controller mock: enough surface for the workspace bindings and the
    // crop-ratio tests (the real behaviour lives in the controller suites). The edit
    // controller is outside this pilot's typed boundary.
    QtObject {
        id: imageEditMock

        property bool active: false
        property int sourceSlot: -1
        property int imageWidth: 64
        property int imageHeight: 48
        property int maximumEdge: 16384
        property int minimumEdge: 1
        property bool strokeActive: false
        property bool canUndo: false
        property bool canRedo: false
        property int selectedAnnotation: -1
        property int annotationCount: 0
        property bool smartSegmentAvailable: false
        property string lastStatus: ""
        property string editedImageUrl: root.tinyPng

        function beginSession(slot, path, url) {
            sourceSlot = slot;
            active = true;
            return true;
        }
        function endSession() {
            active = false;
            sourceSlot = -1;
        }
        function undo() {
        }
        function redo() {
        }
        function cropToImageRect(x, y, w, h) {
            return true;
        }
        function mosaicImageRect(x, y, w, h, block) {
            return true;
        }
        function smartSegmentRect(x, y, w, h, tight) {
            return true;
        }
        function deleteSelectedAnnotation() {
        }
        function clearAnnotations() {
        }
        function probeSmartSegment() {
        }
    }

    Dvs.ImageWorkspace {
        id: workspace
        anchors.fill: parent
        controller: fixture.controller
        colorClipboard: fixture.clipboard
        pairModel: pairModel
        sidebarVisible: false
    }

    TestCase {
        name: "ImageWorkspaceManual"
        when: windowShown

        function init() {
            // Every case starts from the same real material: the standard 64x64 solids.
            // Re-opening bumps the content generation, which is exactly what invalidates
            // stale probe samples and stale hover state from the previous case.
            verify(fixture.openStandardPair());
            imageReview.compareMode = 0;
            imageReview.viewMode = 0;
            imageReview.numericFormat = 0;
            imageReview.fadePosition = 0.5;
            imageReview.wipeOrientation = 0;
            imageReview.wipePosition = 0.5;
            imageReview.diffGain = 4;
            imageReview.alphaFilterEnabled = false;
            imageReview.alphaThreshold = 0;
            imageReview.resetView();
            workspace.singleViewShowSecondary = false;
            workspace.trueSize = false;
            workspace.pixelGridEnabled = false;
            workspace.crosshairVerbose = false;
            workspace.cropRatioLock = 0;
            workspace.editTool = "crop";
            workspace.backgroundMode = 1;
            workspace.endPeek();
            workspace.cropSelection = null;
            workspace.imageEdit = null;
            imageEditMock.endSession();
            workspace.marqueeNoticeText = "";
            workspace.clearImageHover();
            // FLOW3: the directory cursor is host-owned state; a leftover flag would make the
            // arrow keys step through a folder in a case that never opened one.
            workspace.folderBrowseActive = false;
            workspace.folderBrowseText = "";
            root.width = 960;
            workspace.forceActiveFocus();
            waitForRendering(workspace);
        }

        function test_pixelGridRequiresPhysicalMagnificationAndNearest() {
            const image = findChild(workspace, "imageViewport-2");
            const grid = findChild(workspace, "imagePixelGrid-2");
            verify(image !== null && grid !== null);
            tryCompare(image, "status", Image.Ready);
            verify(!grid.requested, "default is off");
            workspace.pixelGridEnabled = true;
            workspace.trueSize = true;
            imageReview.setZoom(7.9);
            wait(0);
            verify(!grid.active, "fit-relative zoom must not bypass 800% physical gate");
            imageReview.setZoom(8);
            tryCompare(grid, "active", true);
            verify(grid.columns + grid.rows < 600, "only visible grid lines are allocated");
            image.smooth = true;
            tryCompare(grid, "active", false);
            image.smooth = Qt.binding(function () { return workspace.zoom <= 2; });
            tryCompare(grid, "active", true);
            imageReview.setZoom(1);
            tryCompare(grid, "active", false);
        }

        function hoverAt(ratioX, ratioY) {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            const image = findChild(workspace, "imageViewport-2");
            tryVerify(() => image.status === Image.Ready);
            // Park the pointer away from the target first: a move onto the exact spot
            // a previous case left the mouse carries no delta to deliver.
            mouseMove(canvas, 4, 4);
            mouseMove(canvas, canvas.width * ratioX, canvas.height * ratioY);
            tryVerify(() => workspace.hoverPoint !== null, 5000,
                "hover did not establish: canvas " + canvas.width + "x" + canvas.height
                + ", image " + image.width + "x" + image.height
                + " at " + image.x + "," + image.y);
            return canvas;
        }

        // Hover at a ratio of the DRAWN IMAGE rect, not of the canvas: the 64x64 fixture
        // paints as a centred square at fit, so canvas ratios outside that square land in
        // the letterbox where applyHover clears the hover instead of establishing one.
        function hoverImageAt(ratioX) {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            const image = findChild(workspace, "imageViewport-2");
            tryVerify(() => image.status === Image.Ready);
            mouseMove(canvas, 4, 4);
            mouseMove(canvas, image.x + image.width * ratioX, image.y + image.height * 0.5);
            tryVerify(() => workspace.hoverPoint !== null, 5000,
                "hover did not establish: canvas " + canvas.width + "x" + canvas.height
                + ", image " + image.width + "x" + image.height
                + " at " + image.x + "," + image.y);
            return canvas;
        }

        function test_canvas_click_switches_pair_without_timer() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            compare(workspace.singleViewShowSecondary, false);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, true);
            mouseClick(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_small_pointer_motion_switches_without_panning() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            workspace.singleViewShowSecondary = false;
            const zoomBefore = imageReview.zoom;
            const panXBefore = imageReview.panX;
            const panYBefore = imageReview.panY;
            const x = canvas.width / 2;
            const y = canvas.height / 2;
            mousePress(canvas, x, y, Qt.LeftButton);
            mouseMove(canvas, x + 2, y);
            mouseRelease(canvas, x + 2, y, Qt.LeftButton);
            compare(imageReview.zoom, zoomBefore);
            compare(imageReview.panX, panXBefore, "a click must not pan the view");
            compare(imageReview.panY, panYBefore);
            compare(workspace.singleViewShowSecondary, true);
        }

        function test_drag_pans_without_switching() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            workspace.singleViewShowSecondary = false;
            const panXBefore = imageReview.panX;
            const panYBefore = imageReview.panY;
            const x = canvas.width / 2;
            const y = canvas.height / 2;
            mousePress(canvas, x, y, Qt.LeftButton);
            mouseMove(canvas, x + 10, y);
            let active = null;
            for (let slot = 2; slot <= 4; ++slot) {
                const area = findChild(workspace, "imageCanvasMouseArea-" + slot);
                if (area && area.parent && area.parent.dragActive) {
                    active = area;
                    break;
                }
            }
            verify(active !== null);
            verify(active.parent.dragActive);
            compare(active.cursorShape, Qt.ClosedHandCursor);
            mouseRelease(canvas, x + 10, y, Qt.LeftButton);
            verify(imageReview.panX !== panXBefore || imageReview.panY !== panYBefore,
                   "a real drag must move the view");
            compare(workspace.singleViewShowSecondary, false);
            compare(active.parent.dragActive, false);
            verify(active.cursorShape !== Qt.ClosedHandCursor);
        }

        // The drawn origin must never expose canvas: at fit the image is centred whatever the
        // pan says, and when it is larger than the viewport its edges stay outside or on the
        // viewport edge. Without this a 100% drag slid the picture off-centre.
        function test_image_draw_edge_never_exposes_canvas() {
            compare(workspace.imageDrawEdge(800, 600, 0.0), 100, "fit stays centred at pan 0");
            compare(workspace.imageDrawEdge(800, 600, 1.0), 100, "fit stays centred at pan 1");
            compare(workspace.imageDrawEdge(800, 1600, 0.0), 0,
                    "zoomed: the leading edge stops at the viewport edge");
            compare(workspace.imageDrawEdge(800, 1600, 1.0), -800,
                    "zoomed: the trailing edge still covers the viewport");
            compare(workspace.imageDrawEdge(800, 1600, 1.4), -800,
                    "an out-of-range pan cannot invert the clamp");
        }

        // Clamping the display is only safe while sampling reads the same geometry. A probe
        // computed from the unclamped pan would look right but report the wrong pixel, which
        // is worse than a blank edge for a quality-review tool.
        function test_probe_pixel_matches_the_clamped_drawn_rect() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            const image = findChild(workspace, "imageViewport-2");
            verify(canvas !== null && image !== null);
            tryVerify(() => image.status === Image.Ready);

            imageReview.restoreObservationPosition(4, 1.0, 1.0);
            tryVerify(() => image.width > canvas.width, 5000);
            verify(image.x <= 0 && image.y <= 0, "a zoomed image must start outside the viewport");
            verify(image.x + image.width >= canvas.width, "no blank column may remain");
            verify(image.y + image.height >= canvas.height, "no blank row may remain");

            mouseMove(canvas, 4, 4);
            mouseMove(canvas, 2, 2);
            tryVerify(() => workspace.hoverPoint !== null, 5000,
                      "the clamped image must still accept a probe at the corner");
            const scale = image.width / image.sourceSize.width;
            verify(Math.abs(workspace.hoverPoint.imgX - Math.floor((2 - image.x) / scale)) <= 1,
                   "probe x must follow the drawn origin, was " + workspace.hoverPoint.imgX);
            verify(Math.abs(workspace.hoverPoint.imgY - Math.floor((2 - image.y) / scale)) <= 1,
                   "probe y must follow the drawn origin, was " + workspace.hoverPoint.imgY);

            imageReview.restoreObservationPosition(1, 0.5, 0.5);
        }

        function test_double_click_zoom_keeps_selected_source() {
            const canvas = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvas !== null);
            workspace.singleViewShowSecondary = false;
            workspace.trueSize = false;
            mouseDoubleClickSequence(canvas, canvas.width / 2, canvas.height / 2, Qt.LeftButton);
            compare(workspace.trueSize, true);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_long_pixel_readout_stays_inside_available_status_width() {
            // A real display-converted high-depth pair: the readout carries the unquantized
            // 16-bit details row plus 4-digit coordinates, the same long content the status
            // bar must lay out without overlap at the 960px floor.
            verify(fixture.openHighDepthPair(1200, 1200), "the high-depth fixture must open");
            imageReview.closeSecondary();
            verify(imageReview.hasPrimary);
            verify(!imageReview.hasPair);
            imageReview.updateCursorPixel(2, 1199, 1199);
            verify(imageReview.captureProbeSample(2, 1199, 1199));
            const details = findChild(workspace, "imageStatusDetails");
            const hints = findChild(workspace, "imageStatusHints");
            const pixel = findChild(workspace, "imagePixelReadout");
            verify(details !== null);
            verify(hints !== null);
            verify(pixel !== null);
            tryCompare(pixel, "visible", true);
            wait(100);
            tryVerify(() => details.y >= hints.y + hints.height);
            tryVerify(() => pixel.width <= details.width);
        }

        // ---- Pair sampling: A/B originals and Δ = B−A ----------------------------------

        function test_pair_readout_reports_b_minus_a_from_originals() {
            hoverAt(0.5, 0.5);
            verify(workspace.pairSample !== null);
            verify(workspace.pairSample.a && workspace.pairSample.a.valid);
            verify(workspace.pairSample.b && workspace.pairSample.b.valid);
            compare(workspace.pairSample.a.r, 120);
            compare(workspace.pairSample.b.r, 122);
            const text = workspace.pairReadoutText();
            verify(text.indexOf("Δ(B−A)") >= 0);
            verify(text.indexOf("+2") >= 0);
            verify(text.indexOf("-2") >= 0);
            const readout = findChild(workspace, "imagePairPixelReadout");
            verify(readout !== null);
            compare(readout.visible, true);
            const colBR = findChild(workspace, "imagePairColBR");
            verify(colBR !== null);
            verify(colBR.text.indexOf("122") >= 0);
            workspace.clearImageHover();
        }

        function test_pair_readout_ignores_channel_observation_view() {
            // A derived channel view repaints the observed values; the pair readout must
            // still name the original buffers (it reads samplePixelOriginal).
            imageReview.viewMode = 1;
            hoverAt(0.5, 0.5);
            verify(workspace.pairSample !== null);
            compare(workspace.pairSample.a.r, 120);
            compare(workspace.pairSample.b.g, 128);
            workspace.clearImageHover();
            imageReview.viewMode = 0;
        }

        function test_unequal_sizes_name_unaligned_b() {
            hoverAt(0.5, 0.5);
            verify(workspace.pairSample !== null);
            verify(workspace.pairSample.b && workspace.pairSample.b.valid);
            // Re-open with an unequal pair. The displayed images reload under the pointer
            // (a stationary pointer over reloading material is not a valid sample), so the
            // hover is re-established against the NEW material: a stale aligned sample must
            // not survive the generation change, and the fresh one must name B unaligned.
            verify(fixture.openPair(64, 64, [120, 130, 140, 255], 48, 48, [122, 128, 140, 255]));
            hoverAt(0.5, 0.5);
            verify(workspace.pairSample !== null);
            verify(workspace.pairSample.b && workspace.pairSample.b.unaligned === true);
            const text = workspace.pairReadoutText();
            verify(text.indexOf("未对齐") >= 0);
            verify(text.indexOf("+2") < 0);
            const readout = findChild(workspace, "imagePairPixelReadout");
            verify(readout !== null);
            const colBR = findChild(workspace, "imagePairColBR");
            verify(colBR !== null);
            compare(colBR.visible, false, "unaligned B must not show fabricated columns");
            workspace.clearImageHover();
        }

        function test_pair_columns_keep_their_positions_across_values_and_widths() {
            // Wide injected geometry so hover positions change the coordinate digit count
            // (3 digits near the left of the picture, 4 digits near the right).
            verify(fixture.openPair(2000, 2000, [1, 2, 3, 4], 2000, 2000, [255, 255, 255, 255]));
            const names = ["imagePairCoordA", "imagePairColAR", "imagePairColAG", "imagePairColAB", "imagePairColAAlpha", "imagePairCoordB", "imagePairColBR", "imagePairColBG", "imagePairColBB", "imagePairColBAlpha", "imagePairDeltaLabel", "imagePairColDeltaR", "imagePairColDeltaG", "imagePairColDeltaB", "imagePairColDeltaAlpha"];
            const columns = {};
            for (const name of names) {
                columns[name] = findChild(workspace, name);
                verify(columns[name] !== null, name);
            }
            hoverImageAt(0.05);
            // The Flow lays its children out during polish, which can land a frame after the
            // first column becomes visible, and a column that starts a wrapped line sits at
            // x = 0 legitimately. So the capture waits until the arrangement stops changing
            // rather than for a particular value: comparing against a still-settling layout
            // would make every later assertion meaningless.
            let settled = "";
            for (let attempt = 0; attempt < 12; ++attempt) {
                const snapshot = names.map(name => columns[name].x).join(",");
                if (snapshot === settled)
                    break;
                settled = snapshot;
                wait(40);
            }
            const xsBefore = {};
            for (const name of names)
                xsBefore[name] = columns[name].x;
            // Values flip 1 → 3 digits, coordinates keep their width budget.
            verify(fixture.openPair(2000, 2000, [100, 101, 102, 103], 2000, 2000, [255, 255, 255, 255]));
            hoverImageAt(0.05);
            wait(30);
            for (const name of names)
                compare(columns[name].x, xsBefore[name], name + " moved on a value change");
            // Move to a 4-digit coordinate region.
            hoverImageAt(0.95);
            wait(30);
            for (const name of names)
                compare(columns[name].x, xsBefore[name], name + " moved on a coordinate digit change");
            // A wider window can unwrap the fields; every value must still fit its field.
            root.width = 1440;
            wait(60);
            for (const name of names) {
                verify(columns[name].contentWidth <= columns[name].width + 0.5, name + " clips its text");
                verify(columns[name].x + columns[name].width <= columns[name].parent.width + 0.5, name + " overflows the readout");
            }
            root.width = 960;
            workspace.clearImageHover();
        }

        function test_each_side_card_names_its_own_original() {
            // Side-by-side: each pane's floating card names that side's own value, so the
            // two cards show two different (individually correct) numbers.
            imageReview.compareMode = 1;
            // Pane widths are anchor-driven and settle synchronously; waitForRendering
            // (a TestCase method, like every other helper here) drives one frame before
            // the probe, and the readiness waits below carry the rest.
            waitForRendering(workspace);
            const canvasA = findChild(workspace, "imageCanvasMouseArea-2");
            verify(canvasA !== null);
            const imageA = findChild(workspace, "imageViewport-2");
            tryVerify(() => imageA.status === Image.Ready);
            mouseMove(canvasA, canvasA.width * 0.25, canvasA.height / 2);
            tryVerify(() => workspace.hoverPoint !== null);
            const sampleA = workspace.crosshairSampleForSlot(2);
            const sampleB = workspace.crosshairSampleForSlot(3);
            verify(sampleA !== null && sampleB !== null);
            compare(sampleA.r, 120);
            compare(sampleB.r, 122);
            const crossA = findChild(workspace, "imageCrosshairCoordinates-2");
            const crossB = findChild(workspace, "imageCrosshairCoordinates-3");
            verify(crossA !== null && crossB !== null);
            tryCompare(crossA, "visible", true);
            tryCompare(crossB, "visible", true);
            imageReview.compareMode = 0;
            workspace.clearImageHover();
        }

        function test_detailed_card_labels_the_sample_identity() {
            workspace.crosshairVerbose = true;
            hoverAt(0.5, 0.5);
            const card = findChild(workspace, "imageCrosshairCoordinateText-2");
            verify(card !== null);
            verify(card.text.indexOf("A 原图") >= 0, "pair hover names the side's original");
            // A single image with a derived channel view must not claim original RGBA.
            imageReview.closeSecondary();
            verify(!imageReview.hasPair);
            workspace.clearImageHover();
            hoverAt(0.5, 0.5);
            imageReview.viewMode = 1;
            workspace.clearImageHover();
            hoverAt(0.5, 0.5);
            const sample = workspace.crosshairSampleForSlot(2);
            verify(sample !== null);
            const label = workspace.crosshairCardLabel(2, sample);
            verify(label.indexOf("Alpha 灰度") >= 0, "a converted view is labelled as an observation");
            imageReview.viewMode = 0;
            workspace.crosshairVerbose = false;
            workspace.clearImageHover();
        }

        // ---- Peek -----------------------------------------------------------------------

        function test_keyboard_peek_holds_and_restores_snapshot() {
            imageReview.compareMode = 7;
            imageReview.fadePosition = 0.3;
            workspace.forceActiveFocus();
            keyPress(Qt.Key_QuoteLeft);
            compare(workspace.peekActive, true);
            compare(imageReview.compareMode, 0);
            compare(workspace.singleViewShowSecondary, false);
            // Drift during the hold must not enter the snapshot.
            imageReview.fadePosition = 0.9;
            keyRelease(Qt.Key_QuoteLeft);
            compare(workspace.peekActive, false);
            compare(imageReview.compareMode, 7);
            compare(imageReview.fadePosition, 0.3);
            // Shift+backtick peeks at B.
            keyPress(Qt.Key_QuoteLeft, Qt.ShiftModifier);
            compare(workspace.peekActive, true);
            compare(workspace.singleViewShowSecondary, true);
            keyRelease(Qt.Key_QuoteLeft);
            compare(workspace.peekActive, false);
            compare(imageReview.compareMode, 7);
            compare(workspace.singleViewShowSecondary, false);
        }

        function test_peek_button_holds_without_self_cancel() {
            imageReview.compareMode = 2;
            const button = findChild(workspace, "imagePeekButton");
            verify(button !== null);
            tryCompare(button, "visible", true);
            // The button joined the Flow when the diff mode made it visible, and the
            // fuller row can wrap, which grows the toolbar band on polish. Wait for a
            // window-wide polish pass (an Item-scoped wait would not flush the Flow,
            // the column heights and the stage anchors in one go); until those settle,
            // the stage's canvas can still float above the wrapped button row and eat
            // the press. The placement probe then confirms the Flow slot itself.
            waitForPolish(workspace.Window.window);
            tryVerify(() => button.x > 0, 5000, "the peek button must be placed by the Flow");
            // Real press-and-hold: the mode flips away from the availability condition the
            // moment the peek begins, and the button must stay able to hold and release.
            mousePress(button, button.width / 2, button.height / 2, Qt.LeftButton);
            const scenePos = button.mapToItem(null, 0, 0);
            verify(button.pressed, "the peek button must own the press (scene " + scenePos.x
                + "," + scenePos.y + ", size " + button.width + "x" + button.height + ")");
            tryCompare(workspace, "peekActive", true);
            wait(80);
            verify(workspace.peekActive, "the held peek must survive event loops");
            verify(button.enabled, "the peek button must stay enabled while peeking");
            mouseRelease(button, button.width / 2, button.height / 2, Qt.LeftButton);
            tryCompare(workspace, "peekActive", false);
            compare(imageReview.compareMode, 2, "release must restore the difference mode");
        }

        function test_peek_ends_when_the_pair_changes() {
            imageReview.compareMode = 7;
            workspace.forceActiveFocus();
            keyPress(Qt.Key_QuoteLeft);
            verify(workspace.peekActive);
            // A new material generation (here: the same images re-opened) must end the peek.
            verify(fixture.openStandardPair());
            tryCompare(workspace, "peekActive", false);
            if (!workspace.peekActive)
                keyRelease(Qt.Key_QuoteLeft);
        }

        function test_peek_ends_on_focus_loss() {
            imageReview.compareMode = 7;
            workspace.forceActiveFocus();
            keyPress(Qt.Key_QuoteLeft);
            verify(workspace.peekActive);
            // Any other item taking the keyboard ends the peek; a plain TextField keeps the
            // focus path isolated from the edit-mode cleanup.
            const field = createTemporaryQmlObject("import QtQuick.Controls; TextField { }", root);
            field.forceActiveFocus();
            tryCompare(workspace, "peekActive", false);
            if (!workspace.peekActive)
                keyRelease(Qt.Key_QuoteLeft);
            workspace.forceActiveFocus();
        }

        // ---- Space / T in every pair mode, with the edit negative -----------------------

        function test_space_and_t_enter_direct_look_from_every_pair_mode() {
            workspace.forceActiveFocus();
            const modes = [1, 5, 7, 2];
            for (const mode of modes) {
                imageReview.compareMode = mode;
                workspace.singleViewShowSecondary = false;
                keyClick(Qt.Key_Space);
                compare(imageReview.compareMode, 0, "Space enters the direct look from mode " + mode);
                compare(workspace.singleViewShowSecondary, true);
            }
            imageReview.compareMode = 5;
            workspace.singleViewShowSecondary = false;
            keyClick(Qt.Key_T);
            compare(imageReview.compareMode, 0);
            compare(workspace.singleViewShowSecondary, true);
        }

        // UX-15: the image split line used to be drag-only while the video surface had
        // Alt+arrows since E1. Same chords, same steps, same axis gate as ReviewShortcuts.
        // They are application Shortcuts for the same reason the video side uses them: arrow
        // keys are consumed by whichever child owns the arrow keys for its own navigation, so
        // a branch in the workspace key handler would only fire when nothing else wanted them.
        function test_alt_arrows_move_the_split_line_on_its_own_axis() {
            imageReview.compareMode = 5;
            imageReview.wipeOrientation = 0; // vertical line: it travels left/right
            imageReview.wipePosition = 0.5;
            keyClick(Qt.Key_Right, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.51);
            keyClick(Qt.Key_Left, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.5);
            keyClick(Qt.Key_Right, Qt.ShiftModifier | Qt.AltModifier);
            compare(imageReview.wipePosition, 0.55, "Shift takes the 5 % step");
            // The axis the line cannot travel stays free for the platform binding.
            keyClick(Qt.Key_Up, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.55);

            imageReview.wipeOrientation = 1; // horizontal line: it travels up/down
            keyClick(Qt.Key_Right, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.55, "the wrong axis must not move the line");
            keyClick(Qt.Key_Up, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.54);
            keyClick(Qt.Key_Down, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.55);

            // Both ends clamp instead of running past the frame.
            imageReview.wipePosition = 0.995;
            keyClick(Qt.Key_Down, Qt.AltModifier);
            compare(imageReview.wipePosition, 1);

            // Edit mode is the only place this workspace has text entry, and it turns the
            // chords off wholesale rather than trusting focus to sort it out.
            imageReview.wipePosition = 0.5;
            workspace.imageEdit = imageEditMock;
            imageEditMock.beginSession(2, "mock", "mock");
            keyClick(Qt.Key_Down, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.5, "editing keeps the split still");
            imageEditMock.endSession();
            workspace.imageEdit = null;

            // Outside the wipe comparison the chord is inert - the same gate the W key uses.
            imageReview.compareMode = 0;
            imageReview.wipePosition = 0.5;
            keyClick(Qt.Key_Down, Qt.AltModifier);
            compare(imageReview.wipePosition, 0.5);
        }

        function test_space_does_not_switch_while_editing_or_in_a_text_field() {
            workspace.imageEdit = imageEditMock;
            imageEditMock.beginSession(2, "mock", "mock");
            imageReview.compareMode = 0;
            workspace.singleViewShowSecondary = false;
            workspace.trueSize = false;
            workspace.crosshairVerbose = false;
            workspace.forceActiveFocus();
            keyClick(Qt.Key_Space);
            compare(workspace.singleViewShowSecondary, false, "editing keeps the single view locked");
            // A focused text field consumes the keys entirely — and actually receives the
            // characters, which proves the workspace never stole them from the editor.
            workspace.editTool = "text";
            const field = findChild(workspace, "imageEditAnnotationTextField");
            verify(field !== null);
            field.forceActiveFocus();
            const before = field.text;
            keyClick(Qt.Key_Space);
            compare(workspace.singleViewShowSecondary, false);
            verify(field.activeFocus);
            compare(field.text, before + " ", "the field received the space");
            keyClick(Qt.Key_T);
            compare(field.text, before + " t", "the field received the t");
            keyClick(Qt.Key_P);
            compare(workspace.crosshairVerbose, false, "P must not toggle behind a text field");
            compare(field.text, before + " tp", "the field received the p");
            keyClick(Qt.Key_Z);
            compare(workspace.trueSize, false, "Z must not flip view state behind a text field");
            compare(field.text, before + " tpz", "the field received the z");
            verify(!workspace.peekActive);
            workspace.editTool = "crop";
            imageEditMock.endSession();
            workspace.imageEdit = null;
        }

        // ---- FLOW3 single-directory stepping ------------------------------------------

        function test_arrows_step_the_single_directory_only_while_a_cursor_is_active() {
            const steps = [];
            const jumps = [];
            const onStep = delta => steps.push(delta);
            const onJump = toLast => jumps.push(toLast);
            workspace.folderStepRequested.connect(onStep);
            workspace.folderJumpRequested.connect(onJump);
            try {
                workspace.forceActiveFocus();
                keyClick(Qt.Key_Right);
                keyClick(Qt.Key_Left);
                compare(steps.length, 0, "without a directory cursor the arrows must stay quiet");

                workspace.folderBrowseActive = true;
                workspace.folderBrowseText = "2 / 7";
                keyClick(Qt.Key_Right);
                keyClick(Qt.Key_Left);
                keyClick(Qt.Key_Down);
                keyClick(Qt.Key_Up);
                compare(steps.join(","), "1,-1,1,-1", "arrow direction maps to directory direction");
                keyClick(Qt.Key_Home);
                keyClick(Qt.Key_End);
                compare(jumps.join(","), "false,true", "Home/End reach the directory edges");

                const label = findChild(workspace, "imageFolderPositionLabel");
                verify(label !== null);
                compare(label.visible, true);
                compare(label.text, "2 / 7", "the toolbar states the position the keys will move from");
            } finally {
                workspace.folderStepRequested.disconnect(onStep);
                workspace.folderJumpRequested.disconnect(onJump);
                workspace.folderBrowseActive = false;
                workspace.folderBrowseText = "";
            }
        }

        function test_modifier_combos_never_toggle_or_peek() {            // Ctrl/Alt variants keep their platform meanings: no pair flip, no peek.
            imageReview.compareMode = 7;
            workspace.singleViewShowSecondary = false;
            workspace.forceActiveFocus();
            keyClick(Qt.Key_T, Qt.ControlModifier);
            compare(imageReview.compareMode, 7);
            compare(workspace.singleViewShowSecondary, false);
            keyClick(Qt.Key_T, Qt.AltModifier);
            compare(imageReview.compareMode, 7);
            compare(workspace.singleViewShowSecondary, false);
            keyClick(Qt.Key_QuoteLeft, Qt.ControlModifier);
            compare(workspace.peekActive, false);
            compare(imageReview.compareMode, 7);
            keyClick(Qt.Key_QuoteLeft, Qt.AltModifier);
            compare(workspace.peekActive, false);
            compare(imageReview.compareMode, 7);
            // Positive control on the same setup: plain Shift+` still peeks at B.
            keyPress(Qt.Key_QuoteLeft, Qt.ShiftModifier);
            compare(workspace.peekActive, true);
            compare(workspace.singleViewShowSecondary, true);
            keyRelease(Qt.Key_QuoteLeft, Qt.ShiftModifier);
            compare(workspace.peekActive, false);
            imageReview.compareMode = 0;
        }

        // ---- Coordinate card verbosity (P) -----------------------------------------------

        function test_p_toggles_concise_and_detailed_card() {
            hoverAt(0.5, 0.5);
            const card = findChild(workspace, "imageCrosshairCoordinateText-2");
            verify(card !== null);
            verify(/^\d+, \d+$/.test(card.text));
            workspace.forceActiveFocus();
            keyClick(Qt.Key_P);
            compare(workspace.crosshairVerbose, true);
            verify(card.text.indexOf("R 120") >= 0, "detailed card names the original sample");
            verify(card.text.indexOf("G 130") >= 0);
            keyClick(Qt.Key_P);
            compare(workspace.crosshairVerbose, false);
            verify(/^\d+, \d+$/.test(card.text));
            workspace.clearImageHover();
        }

        // ---- Z: fit / 100% ---------------------------------------------------------------

        function test_z_flips_fit_and_true_size() {
            workspace.trueSize = false;
            imageReview.setZoom(2.5);
            workspace.forceActiveFocus();
            keyClick(Qt.Key_Z);
            compare(workspace.trueSize, true);
            compare(imageReview.zoom, 1);
            keyClick(Qt.Key_Z);
            compare(workspace.trueSize, false);
            compare(imageReview.zoom, 1);
        }

        // ---- Marquee feedback --------------------------------------------------------------

        function test_marquee_outside_reports_and_inside_zooms() {
            const canvas = hoverAt(0.5, 0.5);
            const zoomBefore = imageReview.zoom;
            // Far-left letterbox: the whole rectangle misses the image.
            mousePress(canvas, 20, 20, Qt.LeftButton, Qt.ShiftModifier);
            mouseMove(canvas, 60, 60);
            mouseRelease(canvas, 60, 60, Qt.LeftButton);
            compare(imageReview.zoom, zoomBefore, "a marquee that misses the image must not zoom");
            tryVerify(() => workspace.marqueeNoticeText.length > 0);
            verify(workspace.marqueeNoticeText.indexOf("框选") >= 0);
            // Negative control: a marquee on the picture zooms and stays quiet.
            const notice = findChild(workspace, "imageMarqueeNotice");
            verify(notice !== null);
            workspace.marqueeNoticeText = "";
            const cx = canvas.width / 2;
            const cy = canvas.height / 2;
            mousePress(canvas, cx - 30, cy - 30, Qt.LeftButton, Qt.ShiftModifier);
            mouseMove(canvas, cx + 30, cy + 30);
            mouseRelease(canvas, cx + 30, cy + 30, Qt.LeftButton);
            tryVerify(() => imageReview.zoom > zoomBefore);
            compare(workspace.marqueeNoticeText, "");
            workspace.clearImageHover();
        }

        // ---- Observation badge: background cycle + one-step reset --------------------------

        function test_observation_badge_cycles_background_and_resets() {
            const badge = findChild(workspace, "imageAlphaObservationBadge");
            verify(badge !== null);
            workspace.backgroundMode = 2;
            tryCompare(badge, "visible", true);
            mouseClick(badge, 6, badge.height / 2, Qt.LeftButton);
            tryCompare(workspace, "backgroundMode", 3);
            mouseClick(badge, 6, badge.height / 2, Qt.LeftButton);
            tryCompare(workspace, "backgroundMode", 0);
            mouseClick(badge, 6, badge.height / 2, Qt.LeftButton);
            tryCompare(workspace, "backgroundMode", 1);
            tryCompare(badge, "visible", false);
            // The reset button restores both observation axes at once.
            imageReview.viewMode = 1;
            workspace.backgroundMode = 2;
            tryCompare(badge, "visible", true);
            const reset = findChild(workspace, "imageAlphaObservationResetButton");
            verify(reset !== null);
            mouseClick(reset, reset.width / 2, reset.height / 2, Qt.LeftButton);
            tryCompare(workspace, "backgroundMode", 1);
            tryCompare(imageReview, "viewMode", 0);
            tryCompare(badge, "visible", false);
        }

        // ---- A/B chip drop-replace ----------------------------------------------------------

        function test_source_chip_drop_replace_signals_and_filter() {
            verify(workspace.isImageFileUrl("file:///pictures/shot.PNG") === true);
            verify(workspace.isImageFileUrl("file:///videos/clip.mp4") === false);
            const dropAreaA = findChild(workspace, "imageSourceChipDropAreaA");
            const dropAreaB = findChild(workspace, "imageSourceChipDropAreaB");
            verify(dropAreaA !== null);
            verify(dropAreaB !== null);
            const spyA = createTemporaryQmlObject("import QtTest; SignalSpy { target: workspace; signalName: \"replacePrimaryDropped\" }", root);
            const spyB = createTemporaryQmlObject("import QtTest; SignalSpy { target: workspace; signalName: \"replaceSecondaryDropped\" }", root);
            // Qt 6 exposes no on* signal handler for external invocation (verified by
            // guard failure), and DropArea.dropped() coerces a plain JS argument to
            // null, so the C++ DropEvent hop into the chip cannot be driven from a
            // QML harness — that hop stays a recorded coverage gap. Drive the chip's
            // own QML-declared signal instead: it is the contract Main.qml consumes,
            // and it carries the URL the drop handler forwards.
            const chipA = findChild(workspace, "imageSourceChipA");
            const chipB = findChild(workspace, "imageSourceChipB");
            verify(chipA !== null);
            verify(chipB !== null);
            chipA.replaceDropped("file:///pictures/new-a.png");
            compare(spyA.count, 1);
            compare(String(spyA.signalArguments[0][0]), "file:///pictures/new-a.png");
            compare(spyB.count, 0);
            chipB.replaceDropped("file:///pictures/new-b.png");
            compare(spyB.count, 1);
            compare(String(spyB.signalArguments[0][0]), "file:///pictures/new-b.png");
        }

        // ---- Crop ratio chips and direct size entry ------------------------------------------

        function test_crop_ratio_and_direct_size_inputs() {
            workspace.imageEdit = imageEditMock;
            verify(workspace.beginImageEdit());
            tryCompare(workspace, "editModeActive", true);
            workspace.cropSelection = {
                "x": 10,
                "y": 8,
                "width": 40,
                "height": 24,
                "fromX": 10,
                "fromY": 8,
                "toX": 50,
                "toY": 32
            };
            const chip169 = findChild(workspace, "imageCropRatioSixteenNine");
            verify(chip169 !== null);
            // Binding-chain guards: the chip rides the floating action bar, whose
            // visibility follows the selection (cropActionBar -> cropOverlay -> box).
            // With this fixture the image maps with a high scale, so the mock's
            // 64x48-pixel selection maps far below the viewport — the bar only stays
            // clickable because its y clamps inside the viewport and above the HUD.
            verify(chip169.visible, "the 16:9 chip must be visible in the crop bar");
            verify(chip169.enabled, "the 16:9 chip must be enabled in the crop bar");
            waitForPolish(chip169);
            mouseClick(chip169, chip169.width / 2, chip169.height / 2, Qt.LeftButton);
            tryVerify(() => workspace.cropRatioLock > 0);
            verify(workspace.cropSelection !== null);
            compare(workspace.cropSelection.width, 40);
            compare(workspace.cropSelection.height, 23, "16:9 reshapes the height from the width");
            // Direct width entry pushes the height through the ratio lock.
            workspace.applyCropSizeFromField("width", 32);
            compare(workspace.cropSelection.width, 32);
            compare(workspace.cropSelection.height, 18);
            // Entries clamp inside the image bounds (64x48, origin x=10).
            workspace.applyCropSizeFromField("width", 60);
            compare(workspace.cropSelection.width, 54);
            // 自由 releases the lock.
            const chipFree = findChild(workspace, "imageCropRatioFree");
            verify(chipFree !== null);
            // The free chip sits at the bar's left end — the end that lands under the
            // bottom-left view-command HUD whenever a clamped bar shares its floor;
            // the product keeps the bar above the HUD band, and the polish wait keeps
            // the Row's own offsets honest before the click.
            waitForPolish(chipFree);
            mouseClick(chipFree, chipFree.width / 2, chipFree.height / 2, Qt.LeftButton);
            tryCompare(workspace, "cropRatioLock", 0);
            const widthField = findChild(workspace, "imageCropWidthField");
            const heightField = findChild(workspace, "imageCropHeightField");
            verify(widthField !== null);
            verify(heightField !== null);
            workspace.applyCropSizeFromField("width", 20);
            compare(workspace.cropSelection.width, 20);
            // The 60 entry clamped to 54 and re-derived height = round(54·9/16) = 30;
            // with the lock released the width entry keeps that unedited height.
            compare(workspace.cropSelection.height, 30, "free ratio keeps the unedited side");
            workspace.finishImageEdit();
            workspace.imageEdit = null;
        }

        // ---- Fade snap points -------------------------------------------------------------------

        function test_fade_snap_points_park_the_blend() {
            imageReview.compareMode = 7;
            const snap0 = findChild(workspace, "imageFadeSnap0");
            const snap50 = findChild(workspace, "imageFadeSnap50");
            const snap100 = findChild(workspace, "imageFadeSnap100");
            verify(snap0 !== null && snap50 !== null && snap100 !== null);
            tryCompare(snap50, "visible", true);
            mouseClick(snap50, snap50.width / 2, snap50.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "fadePosition", 0.5);
            mouseClick(snap0, snap0.width / 2, snap0.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "fadePosition", 0);
            mouseClick(snap100, snap100.width / 2, snap100.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "fadePosition", 1);
            imageReview.compareMode = 0;
        }

        // ---- Gain: inline chips on a wide strip, dropdown fallback below ------------------------

        function test_gain_chips_write_gain_and_narrow_uses_dropdown() {
            imageReview.compareMode = 2;
            const dropdown = findChild(workspace, "imageDiffGainButton");
            verify(dropdown !== null);
            tryCompare(dropdown, "visible", true);
            const chipRow = findChild(workspace, "imageDiffGainChipRow");
            verify(chipRow !== null);
            compare(chipRow.visible, false, "960px uses the dropdown fallback");
            root.width = 1280;
            tryCompare(chipRow, "visible", true);
            // The chip row only joins the Flow's layout once it becomes visible. The
            // placement probe below is the effective guard — it drives the event loop
            // until the Flow has given the row a real slot, where earlier-declared
            // siblings no longer overlap it and eat the click; the polish wait
            // documents the same intent through the real TestCase API.
            waitForPolish(chipRow);
            tryVerify(() => chipRow.x > 0, 5000, "the gain chip row must be placed by the Flow");
            const chip8 = findChild(workspace, "imageDiffGainChip8");
            verify(chip8 !== null);
            mouseClick(chip8, chip8.width / 2, chip8.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "diffGain", 8);
            root.width = 960;
            tryCompare(chipRow, "visible", false);
            imageReview.compareMode = 0;
        }

        // ---- HUD buttons live in the canvas corner ----------------------------------------------

        function test_hud_holds_fit_truesize_and_reset() {
            const hud = findChild(workspace, "imageViewCommandHud");
            verify(hud !== null);
            tryCompare(hud, "visible", true);
            const fit = findChild(workspace, "imageFitButton");
            const trueSize = findChild(workspace, "imageTrueSizeButton");
            const reset = findChild(workspace, "imageResetViewButton");
            verify(fit !== null && trueSize !== null && reset !== null);
            verify(fit.visible && trueSize.visible && reset.visible);
            workspace.trueSize = true;
            imageReview.setZoom(3);
            mouseClick(fit, fit.width / 2, fit.height / 2, Qt.LeftButton);
            tryCompare(workspace, "trueSize", false);
            compare(imageReview.zoom, 1);
            mouseClick(trueSize, trueSize.width / 2, trueSize.height / 2, Qt.LeftButton);
            tryCompare(workspace, "trueSize", true);
            mouseClick(reset, reset.width / 2, reset.height / 2, Qt.LeftButton);
            tryCompare(workspace, "trueSize", false);
            // A HUD click hands the keyboard back to the workspace root (Space keeps
            // working). The hand-back rides Qt.callLater, so it lands one event-loop
            // pass after the click — tryVerify still fails if it never arrives.
            tryVerify(() => workspace.activeFocus, 5000, "HUD buttons must hand focus back to the workspace");
        }

        // ---- In-canvas A/B badge: indicator plus its own switch --------------------------------
        //
        // The badge is the compact current-source indicator that replaced the standalone
        // "切至 A / 切至 B" toolbar button. It must name the side on screen and let the
        // user reach the other one with a real click, so the flicker stays observable
        // without a second permanent button.

        function test_in_canvas_ab_badge_indicates_and_switches() {
            imageReview.compareMode = 0;
            workspace.singleViewShowSecondary = false;
            const badge = findChild(workspace, "imageInPlaceBadge");
            const selectA = findChild(workspace, "imageInPlaceSelectA");
            const selectB = findChild(workspace, "imageInPlaceSelectB");
            verify(badge !== null && selectA !== null && selectB !== null);
            tryCompare(badge, "visible", true);
            // Both segments carry a real hit area (an implicitWidth alone does not), and
            // the plate never runs past its host so it cannot overlap a sibling badge.
            verify(selectA.width > 0 && selectB.width > 0, "both badge segments need a hit area");
            verify(badge.x + badge.width <= badge.parent.width + 0.5,
                   "the badge plate must stay inside its host");

            // Clicking the B segment selects B; clicking the A segment returns to A.
            mouseClick(selectB, selectB.width / 2, selectB.height / 2, Qt.LeftButton);
            tryCompare(workspace, "singleViewShowSecondary", true);
            mouseClick(selectA, selectA.width / 2, selectA.height / 2, Qt.LeftButton);
            tryCompare(workspace, "singleViewShowSecondary", false);

            // The removed duplicate must not come back: no toolbar button may still
            // offer the A/B switch that the chips, Space/T and this badge already cover.
            verify(findChild(workspace, "imageToggleSourceButton") === null,
                   "the standalone 切至 A / 切至 B button must stay removed");

            // While peeking (a difference or fade view suppressed by a held key) the badge
            // hides, so the user is never told "you are on A" while the canvas shows the
            // peeked original.
            const peekBadge = findChild(workspace, "imagePeekBadge");
            verify(peekBadge !== null);
            imageReview.compareMode = 2;
            tryCompare(badge, "visible", false);
            workspace.beginPeek(false);
            tryCompare(peekBadge, "visible", true);
            workspace.endPeek();
            tryCompare(peekBadge, "visible", false);
            compare(imageReview.compareMode, 2, "releasing restores the difference mode");
            imageReview.compareMode = 0;
        }

        // ---- Numeric format selector ---------------------------------------------------------
        //
        // One compact selector for the whole readout family. It only changes what is printed
        // and copied: the image, a sampling value, the difference field and every statistic
        // stay as they were, and Hex stays hexadecimal in both forms.

        function test_numeric_format_selector_switches_only_the_printed_text() {
            const button = findChild(workspace, "imageNumericFormatButton");
            verify(button !== null);
            tryCompare(button, "visible", true);
            verify(button.text.indexOf("整数码值") >= 0, "the integer form is the default");

            workspace.setNumericFormat(0);
            compare(workspace.channelText(12), "12");
            compare(imageReview.hexText(12, 34, 56, 200), "#0C2238C8");

            workspace.setNumericFormat(1);
            tryCompare(imageReview, "numericFormat", 1);
            const pairReadout = findChild(workspace, "imagePairPixelReadout");
            verify(!pairReadout.deltaExceeds(1));
            verify(pairReadout.deltaExceeds(2));
            tryCompare(button, "text", "数值：归一化浮点 ▾");
            // 8-bit display buffer: channel / 255, six decimals.
            compare(workspace.channelText(12), "0.047059");
            compare(workspace.channelText(255), "1.000000");
            compare(imageReview.rgbText(12, 34, 56),
                    "rgb(0.047059, 0.133333, 0.219608)");
            // Hex stays hexadecimal: the switch never changes what it means.
            compare(imageReview.hexText(12, 34, 56, 200), "#0C2238C8");
            // A signed delta keeps its sign, and a zero delta never reads "-0.000000".
            compare(workspace.fmtSigned(12, 34), "+0.086275");
            compare(workspace.fmtSigned(34, 12), "-0.086275");
            compare(workspace.fmtSigned(12, 12), "0.000000");

            workspace.setNumericFormat(0);
            tryCompare(imageReview, "numericFormat", 0);
            compare(workspace.channelText(12), "12");
            compare(workspace.fmtSigned(12, 34), "+22");
        }

        // ---- Probe colour copy -----------------------------------------------------------------
        //
        // The pointer leaves the pixel long before the user reaches the button, so the sample
        // that gets copied is the last valid one of the current material generation. It is not
        // re-sampled at click time and it is not the screen colour under the button.

        function test_copy_colour_button_names_the_frozen_sample_and_its_side() {
            // Establish a hover so a real sample is frozen, then move the pointer away.
            const canvas = hoverImageAt(0.5);
            verify(canvas !== null);
            const button = findChild(workspace, "imageCopyColorButton");
            verify(button !== null);
            mouseMove(button, button.width / 2, button.height / 2);
            tryCompare(workspace, "hoverPoint", null);
            verify(workspace.readoutPairSample !== null);
            compare(workspace.readoutPairSample.a.r, 120);
            const rgb = findChild(workspace, "imageCopyRgbAction");
            const rgba = findChild(workspace, "imageCopyRgbaAction");
            const hex = findChild(workspace, "imageCopyHexAction");
            const label = findChild(workspace, "imageCopyColorSample");
            verify(rgb !== null && rgba !== null && hex !== null && label !== null);
            // The sample survived the pointer leaving the pixel.
            tryCompare(button, "enabled", true);
            verify(label.text.indexOf("A 原图") >= 0, "the menu names the side it will copy");
            verify(label.text.indexOf("像素 (") >= 0, "the menu names the coordinate");
            verify(label.text.indexOf("rgb(") >= 0, "the menu names the value");

            // Each format is offered through the menu instead of a row of permanent buttons,
            // and the payload is the frozen sample, not a fresh guess.
            compare(imageReview.probeColorText(0), "rgb(120, 130, 140)");
            compare(imageReview.probeColorText(1), "rgba(120, 130, 140, 255)");
            compare(imageReview.probeColorText(2), "#78828CFF");

            // The normalized form is offered as its own labelled entry, never silently
            // mixed with the integer one.
            workspace.setNumericFormat(1);
            compare(imageReview.probeColorText(0), "rgb(0.470588, 0.509804, 0.549020)");
            const red = findChild(workspace, "imagePairColAR");
            compare(red.text.trim(), "0.470588");
            compare(imageReview.probeColorText(2), "#78828CFF");
            workspace.setNumericFormat(0);

            // The real adapter writes through the system clipboard and verifies the write.
            // The success path is a mandatory acceptance condition. First confirm the session
            // has a usable clipboard by writing a distinct sentinel DIRECTLY to the clipboard
            // (retried against transient OLE contention, and independent of the copy
            // controller); a clipboard that cannot be written at all skips this case - never
            // counted as a pass. Then drive the real menu action and require the success notice
            // plus the exact payload on the clipboard: a copy that only ever reports failure
            // fails here instead of passing on a failure branch.
            var sentinelWritten = false;
            for (var gi = 0; gi < 5 && !sentinelWritten; ++gi) {
                sentinelWritten = fixture.setClipboardText("DVS-CLIPBOARD-SENTINEL-should-be-overwritten");
                if (!sentinelWritten)
                    wait(20);
            }
            if (!sentinelWritten)
                skip("no usable clipboard in this session: the copy success path cannot be exercised here");
            var copyOk = false;
            for (var ci = 0; ci < 5 && !copyOk; ++ci) {
                mouseClick(button, button.width / 2, button.height / 2);
                tryCompare(rgb.parent, "visible", true);
                rgb.triggered();
                wait(30);
                copyOk = workspace.marqueeNoticeText.indexOf("已复制") >= 0;
            }
            verify(copyOk,
                   "a verified copy must report success, not a silent failure: "
                   + workspace.marqueeNoticeText);
            compare(fixture.clipboardText(), "rgb(120, 130, 140)");
        }

        // ---- Probe colour copy: the failure path -------------------------------------------
        //
        // The controllable failure boundary at the UI: with no frozen sample the copy action
        // explains itself, returns false, and keeps the button disabled - it never writes and
        // never reports a silent success.

        function test_copy_reports_failure_and_returns_false_without_a_sample() {
            const canvas = hoverImageAt(0.5);
            verify(canvas !== null);
            const button = findChild(workspace, "imageCopyColorButton");
            const label = findChild(workspace, "imageCopyColorSample");
            verify(button !== null && label !== null);
            tryCompare(button, "enabled", true);

            // Park the pointer off the pixel so the frozen sample is the last valid one and a
            // live hover cannot re-populate it after it is cleared.
            mouseMove(button, button.width / 2, button.height / 2);
            tryCompare(workspace, "hoverPoint", null);

            // Drop the sample: the copy is disabled and the menu says why.
            imageReview.clearFrozenProbeSample();
            tryCompare(button, "enabled", false);
            verify(label.text.indexOf("无有效样本") >= 0, "the menu explains the disabled state");

            // Driving the action anyway must report the failure and return false.
            const copied = workspace.copyProbeColor(0);
            verify(copied === false, "a copy with no sample must return false");
            verify(workspace.marqueeNoticeText.indexOf("没有可复制的颜色样本") >= 0,
                   "the copy must explain the missing sample: " + workspace.marqueeNoticeText);
        }

        // ---- Alpha threshold observation filter -------------------------------------------------
        //
        // Default off; "off" and "on with T = 0" are different states. Strict greater-than:
        // A=20/T=25 and A=25/T=25 are hidden, A=26/T=25 passes.

        function test_alpha_filter_toggle_threshold_and_badge() {
            const toggle = findChild(workspace, "imageAlphaFilterButton");
            const row = findChild(workspace, "imageAlphaFilterRow");
            const spin = findChild(workspace, "imageAlphaFilterSpin");
            const reset = findChild(workspace, "imageAlphaFilterResetButton");
            const badge = findChild(workspace, "imageAlphaFilterBadge");
            verify(toggle !== null && row !== null && spin !== null && reset !== null
                   && badge !== null);
            // Default off, and the threshold controls stay hidden until it is on.
            verify(!imageReview.alphaFilterEnabled, "the filter is off by default");
            tryCompare(row, "visible", false);
            tryCompare(badge, "visible", false);

            mouseClick(toggle, toggle.width / 2, toggle.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "alphaFilterEnabled", true);
            tryCompare(row, "visible", true);
            compare(toggle.text, "Alpha 过滤：A > 0");
            // T = 0 still hides A = 0: "on with T = 0" is not the same as "off".
            compare(imageReview.alphaThreshold, 0);
            // The badge only claims the filter while the observation it belongs to is on
            // screen; elsewhere it says so instead of silently filtering.
            compare(imageReview.viewMode, 2);
            tryCompare(badge, "text", "Alpha 过滤 A > 0");

            // The spin box commits the exact value the user typed through the same
            // workspace function the slider and the reset use, and it mirrors the
            // controller back through its binding, so the two can never disagree.
            workspace.setAlphaThreshold(25);
            tryCompare(imageReview, "alphaThreshold", 25);
            tryCompare(spin, "value", 25);
            compare(toggle.text, "Alpha 过滤：A > 25");
            // The badge spells out the strict-greater rule so the boundary is not a guess.
            verify(badge.text.indexOf("A >") >= 0);

            // T = 255 lets nothing through, which is a valid result and is stated as one.
            workspace.setAlphaThreshold(255);
            tryCompare(imageReview, "alphaThreshold", 255);
            tryCompare(badge, "text", "Alpha 过滤 A > 255：无像素通过（有效过滤结果）");

            // The reset button is the reliable way back to "off": it restores both the
            // threshold and the switch through the same guarded path the slider uses.
            workspace.resetAlphaThreshold();
            tryCompare(imageReview, "alphaFilterEnabled", false);
            compare(imageReview.alphaThreshold, 0);
            tryCompare(row, "visible", false);

            // Outside the observation it belongs to, the filter is reported as not applied
            // instead of silently filtering (or silently not filtering).
            mouseClick(toggle, toggle.width / 2, toggle.height / 2, Qt.LeftButton);
            tryCompare(imageReview, "alphaFilterEnabled", true);
            imageReview.viewMode = 0;
            tryCompare(badge, "text", "Alpha 过滤未应用（当前不是 RGB 忽略透明度）");
            workspace.resetAlphaThreshold();
            tryCompare(imageReview, "alphaFilterEnabled", false);
            compare(imageReview.alphaThreshold, 0);
            workspace.clearImageHover();
        }

        // ---- Right-click close of one image side -------------------------------------------------
        //
        // The menu target is the chip that owns the side, so a same-named file, a source that
        // was replaced while the menu was open and a still-loading side can never be confused.
        // A right click opens the menu only: it must not fire the left-button direct look.

        function test_right_click_chip_opens_close_menu_without_switching_side() {
            imageReview.compareMode = 0;
            workspace.singleViewShowSecondary = false;
            const chipB = findChild(workspace, "imageSourceChipB");
            const menu = findChild(workspace, "imageSourceChipCloseMenuB");
            const closeAction = findChild(workspace, "imageCloseSecondaryAction");
            const reason = findChild(workspace, "imageCloseSecondaryReason");
            verify(chipB !== null && menu !== null && closeAction !== null && reason !== null);
            tryCompare(chipB, "visible", true);
            // The spy must be bound to the function-local chip explicitly: an unqualified
            // name in the temporary object's scope resolves to nothing, which used to
            // leave the target null and the "no direct look" assertion vacuous.
            const spyDirect = createTemporaryQmlObject(
                "import QtTest; SignalSpy { signalName: \"directLookRequested\" }",
                root);
            verify(spyDirect !== null);
            spyDirect.target = chipB;
            const spyClose = createTemporaryQmlObject(
                "import QtTest; SignalSpy { target: workspace; signalName: \"closeSideRequested\" }",
                root);
            verify(spyClose !== null);

            // A right click must not switch the displayed side first. A TapHandler only
            // emits `tapped` once the press is released, so the whole press/release pair is
            // delivered before the menu is expected. VcsMenu is a Popup.Window, so the menu
            // lives in its own window and `visible` on the item inside the workspace stays
            // false - `opened` is the observable state.
            mouseClick(chipB, chipB.width / 2, chipB.height / 2, Qt.RightButton);
            tryCompare(menu, "opened", true);
            compare(workspace.singleViewShowSecondary, false,
                    "a right click must not perform the left-button direct look");
            compare(spyDirect.count, 0);

            // Closing B keeps A; closing A keeps B. The chip asks for exactly its own side,
            // and the workspace routes it through the guarded single-side close. The real
            // controller's state names the outcome: closing B keeps A as the only image,
            // and closing the last image reaches the empty state.
            closeAction.triggered();
            compare(spyClose.count, 1);
            compare(String(spyClose.signalArguments[0][0]), "3", "the B chip asks for slot 3");
            verify(imageReview.hasPrimary, "closing B must keep A");
            verify(!imageReview.hasSecondary, "closing B removes exactly B");
            const chipA = findChild(workspace, "imageSourceChipA");
            const closeA = findChild(workspace, "imageClosePrimaryAction");
            verify(chipA !== null && closeA !== null);
            closeA.triggered();
            compare(spyClose.count, 2);
            compare(String(spyClose.signalArguments[1][0]), "2", "the A chip asks for slot 2");
            verify(!imageReview.hasPrimary,
                   "closing the last image must reach the empty state, not a fabricated side");
        }
    }
}
