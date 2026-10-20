.pragma library

// Static semantic tokens for CompareStation's Apple-flavoured dark theme. Keep state colours
// here so controls rendered by different Qt Quick Controls styles cannot silently drift apart.
//
// The palette follows macOS dark-mode conventions: neutral charcoal surfaces (no blue cast),
// translucent white hairlines instead of solid outlines, one system-blue accent, and text
// colours taken from the system hierarchy (primary / secondary / tertiary / quaternary).

// Surfaces, darkest to lightest. The review stage sits below the chrome so footage keeps the
// strongest contrast; every raised layer is one step lighter than the layer it sits on.
// Tuned to macOS dark-mode neutrals: pure neutral charcoal, no blue or purple cast.
var canvas = "#0a0a0b"
var window = "#1c1c1e"
// The DWM caption colour in DesktopApplication.cpp mirrors this value; change both together.
var headerBackground = "#1c1c1e"
var panel = "#242426"
var panelElevated = "#2c2c2e"
var menu = "#343438"
var raisedPanel = "#3a3a3c"
var disabledPanel = "#2a2a2c"
// Opaque row highlight for popup lists (combo boxes). The translucent control veils are for
// flat chrome sitting on known surfaces; a list row floats over the popup and must keep one
// solid fill so its label keeps its measured contrast.
var raisedPanelHover = "#48484a"

// Neutral control states. Resting fills are translucent white so the same control reads
// correctly on the window, a panel or a floating surface; hover and press only deepen the
// veil, and a checked control keeps an accent-tinted fill (never just a border).
var control = "#1affffff"
var controlHover = "#29ffffff"
var controlPressed = "#3dffffff"
var controlChecked = "#3d0a84ff"

var primaryText = "#f5f5f7"
var secondaryText = "#e5e5ea"
var mutedText = "#a5a5ab"
// Inactive controls are exempt from WCAG 1.4.3, and the exemption is only honest while
// nothing a user must read is published in this colour. The old value (#6c6c70) measured
// 2.37:1 on `menu`, which made a greyed row unreadable rather than merely inactive - and the
// comparison menu used to publish its whole "why" through disabled rows. Raised to Apple's
// dark systemGray (#8e8e93: ~3.7:1 on `menu`, ~5.1:1 on `window`) so the state still reads
// as unavailable while the label remains legible. Explanations live in `mutedText`, never
// here; tst_vcs_theme pins both the floor and the separation from `mutedText`.
var disabledText = "#8e8e93"
var inverseText = "#ffffff"

// accent is for borders, indicators and focus; filled buttons use accentFill so white labels
// keep an AA contrast ratio, and accentText is the readable accent for text on dark panels.
// Aligned to macOS systemBlue dark.
var accent = "#0a84ff"
var accentText = "#64d2ff"
var accentFill = "#0964d4"
var accentHover = "#0b6bdc"
var accentPressed = "#0054b8"
// Keyboard focus contract (one rule per kind of item, so rings cannot drift apart again -
// backlog U12). Control subclasses (buttons, combos, spin boxes) drive the ring from
// `visualFocus`: the 2 px `focus` ring shows for keyboard navigation only and a mouse click
// leaves no mark; ReviewActionButton and VcsToolButton pin this in tst_review_action_button.
// A custom Item that is not a Control has no `visualFocus` at all - binding it is silently
// never true, and qmllint does not flag the missing property on a self-referencing id (U10).
// Prefer turning such an item into a suitable Control so the rule above applies. A genuinely
// custom focus mechanism is not prescribed here: whatever drives it must be proven by real
// tests (Tab and Shift+Tab arrival from a neighbour, mouse-click entry, focus loss and
// return), and a bare `activeFocus` ring is not that mechanism - a mouse forceActiveFocus
// lights it too.
// Editable Controls with an internal editor (e.g. an editable SpinBox) forward focus to the
// contentItem; the Control itself then never reports activeFocus/visualFocus while the editor
// holds it (verified against Qt 6.11 QQuickAbstractSpinBox::handleFocusInEvent - Control is
// not a FocusScope). Such a ring must derive from `contentItem.activeFocus` plus the Control's
// `focusReason` naming a keyboard reason; ReviewOffsetSpinBox's keyboardFocusRing is the
// pattern and tst_review_offset_spin_box pins it.
var focus = "#64d2ff"

// Hairline separators and control outlines. Everything is a white veil so the line adapts to
// the surface underneath; only floating popups carry a slightly stronger edge.
var border = "#14ffffff"
var borderHover = "#2effffff"
var controlBorder = "#1affffff"
var menuBorder = "#22ffffff"
var disabledBorder = "#0fffffff"

// Red is reserved for real failures (a decode error, a failed save). Limits and unavailable
// states such as a missing frame or an inexact pairing use the warning family instead.
var error = "#ff453a"
var errorPanel = "#3b1719"
var errorBorder = "#ff453a"
var errorText = "#ff8f87"
var warning = "#ffd60a"
var warningPanel = "#332a10"
var warningBorder = "#b89b0e"
var warningText = "#ffe066"
var success = "#30d158"
var successFill = "#248a3d"
var successBorder = "#30d158"
// Badge ground for success-coloured text; successFill is too light for it to reach AA.
var successPanel = "#123f22"
// Neutral "this is extra information", not a state: the Alpha channel-view dot and the timeline
// zoom readout. It used to be #38bdf8, byte-identical to sourceA, so an image-workspace badge
// read as "source A". Green is the one saturated hue band still clear of all three sources.
// It shares a family with `success`, which is deliberate and constrained: the two never render
// in the same component (success grounds toasts, information dots a channel badge), and
// `ui.vcs_theme` pins the separation from every source identity colour.
var information = "#22c55e"

// These alpha-bearing colours are intentional semantic overlays, not popup backgrounds.
var modalScrim = "#a6000000"
var oscPanel = "#ed1f1f21"
var thumbnailPanel = "#f229292c"
var thumbnailWell = "#0a0a0b"

// Translucent overlays drawn on top of footage: they must stay legible over bright pixels.
var stageLabel = "#c71c1c1e"
var stageLabelBorder = "#2bffffff"
var oscGlass = "#eb1f1f21"
var oscBorder = "#2bffffff"
var oscGlassHover = "#f2262629"
// Drag-and-drop scrim. Heavier than modalScrim so the drop hint stays legible over bright
// footage; nothing behind it needs to be read while a file is being dragged.
var dropScrim = "#e00a0a0c"

// The stage itself: the well behind footage and the marks drawn on the picture. Their
// translucency lives in the colour, never in `opacity`, so child items cannot fade with them.
var stageWell = "#050506"
var stageDivider = "#bfd8e2f2"
var lineHalo = "#4d000000"
var selectionFill = "#220a84ff"
var cropFill = "#2214b8a6"
var cropBorder = "#5eead4"
// Pixel probe. Yellow stays clear of the accent-blue selection, the teal crop and every
// source hue; the pointer's own panel gets a faint white line so it never hides the cursor.
var probe = "#facc15"
var probeLine = "#d9facc15"
var probeLineMuted = "#55ffffff"
var probePip = "#ccffffff"

var timelineRail = "#2e2e31"
// In/out band: accentText at 30 % alpha, so the brackets drawn on top of it stay fully opaque.
var rangeBand = "#4d5aabff"
// Alignment markers. A missing frame is a real gap and uses error; the other kinds only need
// to be told apart from each other.
//
// These four deliberately sit outside the A/B/C source-identity hues (cyan, orange, purple).
// The extra-frame marker is pink to keep it clear of source C. The rail is 4-11 px wide, so a marker that
// merely *resembles* a source chip reads as "this is source B" instead of "this frame repeated":
// duplicate and information used to be byte-identical to sourceB and sourceA, and extra was
// too close to sourceC. Anchor and other are near-neutral, which separates them from every
// saturated hue by chroma rather than by hue. `ui.vcs_theme` asserts the separation.
var markerDuplicate = "#a3e635"
var markerExtra = "#f4a4ca"
var markerAnchor = "#e2e8f0"
var markerOther = "#a8a29e"
var keycap = "#2a2a2c"
var keycapBorder = "#454548"

// Shape and interaction tokens. Larger, more uniform radii follow Apple's continuous-corner
// language: small controls round at 8, cards float at 18. The micro and subtle steps collect
// the 4 and 6 px corners that chips, badges and inline wells already used.
var radiusMicro = 4
var radiusSubtle = 6
var radiusSmall = 8
var radiusMedium = 10
var radiusLarge = 12
var radiusCard = 18
var radiusPill = 999

// Animation tokens. macOS UI moves on ~160-220 ms OutCubic springs, not linear 80 ms fades.
var animationFast = 120
var animationBase = 180
var animationSlow = 260
var animationEasing = "easeOutCubic"
var animationEasingSpring = "easeOutBack"

// Typography tokens (Qt logical pixels, never source pixels). Body copy is 14; 12 is the
// secondary size for muted captions, status readouts and diagnostics; 16 leads groups.
// fontMono sizes the compact monospaced numeric readouts (timecode, frame counters).
// fontMicro is decorative only - it must never carry a key action or a must-read conclusion;
// anything that states a result or invites a click uses fontSecondary or larger.
// fontCaption (11) collects the dense metadata size that 78 call sites already used - it is
// the smallest size that may carry a readable, but non-critical, label.
var fontBody = 14
var fontSecondary = 12
var fontGroupTitle = 16
var fontMicro = 10
var fontMono = 13
var fontCaption = 11

// Font families. QML's `font` value type exposes a single `family` string only (Qt 6.11 has
// no `font.families`; tst_vcs_theme records the probe), so a CSS-style fallback stack cannot
// be expressed. Characters the named family lacks are covered by Qt's own font-merging
// fallback (contextFontMerging exists since 6.8); which substitute font Qt picks is not
// promised here. Binding either token forces the family - text that should keep the platform
// default simply leaves font.family unset.
//
// fontFamilyUi is the one token QML never binds: a single-family binding is exactly what the
// token exists to avoid. Its consumer is the application font in DesktopApplication.cpp,
// which can carry the fallback list ("Segoe UI Variable Text" -> "Segoe UI" ->
// "Microsoft YaHei UI"). Change this name and that list together; every Text that does not
// name a family inherits the result.
var fontFamilyUi = "Segoe UI Variable Text"
var fontFamilyMono = "Cascadia Mono"

// Tabular numerals for numeric mixed text that ticks in place (zoom badges, metric readouts)
// while staying in the UI font with its Chinese fallback. The monospaced readouts keep
// Cascadia Mono - coordinates and tables need real column alignment, not just even digits.
// font.features ships since Qt 6.6; the rendered effect depends on the font actually
// carrying the feature, which is why binding sites must not promise a platform guarantee.
var fontFeaturesNumeric = { "tnum": 1 }

// Line heights for wrapped body and compact readout text.
var lineHeightBody = 1.35
var lineHeightCompact = 1.25

// Spacing ladder, from the inside of a control outwards:
//   micro   (4)  gap between the icons or key labels of one group
//   main    (8)  horizontal breathing room inside a control
//   group   (12) between groups - a 1 px divider may take this slot instead
//   section (16) block/card padding and the space between a card and its neighbours
// Deliberate, component-specific paddings outside this ladder stay literals in their
// components; every step above is a multiple of the micro step so nothing drifts to 5 or 7.
var spaceMicro = 4
var spaceMain = 8
var spaceGroup = 12
var spaceSection = 16

// Hit areas. Common interactive controls are at least 32x32 logical pixels so a 100% DPI
// pointer still lands on them; the main play/pause button sits in its own 40-44 band.
var hitAreaMinimum = 32
var playButtonHeight = 40
var playButtonHeightLarge = 44

// Chrome rows above the stage (source strip, compare action bar) share one height band.
var barRowHeight = 40
var barRowHeightMin = 36
var barRowHeightMax = 40

// The docked control strip (one main timeline plus the first-layer transport controls) has
// its own band; optional diagnostic rows that stack above the timeline are not part of it.
var controlStripMinHeight = 64
var controlStripMaxHeight = 80

// Panel widths. The left file sidebar defaults to 232 and resizes inside 200-288; the right
// inspector defaults to 288 and resizes inside 260-320. Defaults and clamps travel together
// so a resize can never push a panel over the canvas guard (canvasMinimumWidth below).
var fileSidebarWidthDefault = 232
var fileSidebarWidthMin = 200
var fileSidebarWidthMax = 288
var inspectorWidthDefault = 288
var inspectorWidthMin = 260
var inspectorWidthMax = 320

// The local-comparison strip claims a share of the workspace height inside this band.
var localStripShareMin = 0.28
var localStripShareMax = 0.40

// The nearby-frames area's expanded height band (collapsed is header-only).
var nearbyFramesHeightMin = 144
var nearbyFramesHeightMax = 184

// The stage keeps at least this many logical pixels of canvas after the actually expanded
// panels; below it the layout reflows (drawer instead of docked inspector) rather than
// squeezing text or controls.
var canvasMinimumWidth = 600

// Flat-control hover/press veils. One step stronger than `controlHover`/`controlPressed`
// would read as a fill; these stay a whisper so toolbars do not flicker.
var fluentHover = "#1fffffff"
var fluentPressed = "#33ffffff"
var subtleBorder = "#24ffffff"

// Source identity follows the application icon: A is system blue, B is system orange, C is
// system purple — macOS dark-mode system colors, desaturated enough to read as identity, not
// decoration. Use the helpers so chips, badges, labels and cards cannot assign different
// colours to one slot.
var sourceA = "#64d2ff"
var sourceABackground = "#0a2a3a"
var sourceABorder = "#0a84ff"

var sourceB = "#ff9f0a"
var sourceBBackground = "#3a2208"
var sourceBBorder = "#ff9f0a"

var sourceC = "#bf5af2"
var sourceCBackground = "#2a1540"
var sourceCBorder = "#bf5af2"

// Dark text for letters drawn on a filled source colour.
var sourceInk = "#0c0c0e"

function sourceColor(slot) {
    return slot === 0 ? sourceA : (slot === 1 ? sourceB : sourceC);
}

function sourceBackground(slot) {
    return slot === 0 ? sourceABackground : (slot === 1 ? sourceBBackground : sourceCBackground);
}

function sourceBorder(slot) {
    return slot === 0 ? sourceABorder : (slot === 1 ? sourceBBorder : sourceCBorder);
}

function timelineMarkerColor(kind) {
    if (kind === "missing")
        return error;
    if (kind === "duplicate")
        return markerDuplicate;
    if (kind === "extra")
        return markerExtra;
    return kind === "anchor" ? markerAnchor : markerOther;
}

// The localized name of every alignment-marker kind `ReviewController` can mint, so the timeline
// tooltip, the OSC hover readout and the inspector legend cannot drift into different words for
// one kind. Pair it with `timelineMarkerColor` above, which owns the matching colour and the same
// neutral fallback. Plain literals, like the rest of this file: it is a `.pragma library` script,
// so `qsTr` is unavailable here and lupdate never sees these strings — the QML call sites keep
// their own `qsTr` context around them.
function timelineMarkerLabel(kind) {
    if (kind === "missing")
        return "缺失";
    if (kind === "duplicate")
        return "重复";
    if (kind === "extra")
        return "多余";
    if (kind === "anchor")
        return "锚点";
    if (kind === "low-confidence")
        return "低置信度";
    if (kind === "review-segment")
        return "待复核区间";
    if (kind === "rejected-segment")
        return "已拒绝区间";
    // A kind this file does not know yet is still an analysis marker, never a raw enum.
    return "分析标记";
}

// Maps a source label such as "A" or "源 B" to its slot; -1 when no letter is present.
function slotForLabel(label) {
    const match = /[ABC]/.exec(String(label || ""));
    return match ? match[0].charCodeAt(0) - 65 : -1;
}
