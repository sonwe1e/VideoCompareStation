.pragma library

// Static semantic tokens for CompareStation's dark desktop theme. Keep state colours here so
// controls rendered by different Qt Quick Controls styles cannot silently drift apart.

// Surfaces, darkest to lightest. The review stage sits below the chrome so footage keeps the
// strongest contrast; every raised layer is one step lighter than the layer it sits on.
var canvas = "#0b0f17"
var window = "#111722"
// The DWM caption colour in DesktopApplication.cpp mirrors this value; change both together.
var headerBackground = "#111722"
var panel = "#151c28"
var panelElevated = "#19212f"
var menu = "#1a2231"
var raisedPanel = "#1e2737"
var disabledPanel = "#1a2230"

// Neutral control states. Checked controls carry an accent-tinted fill, hover never does.
var control = "#232e40"
var controlHover = "#2c394e"
var controlPressed = "#1f2a3b"
var controlChecked = "#1d3a66"

var primaryText = "#f1f5f9"
var secondaryText = "#cbd5e1"
var mutedText = "#94a3b8"
var disabledText = "#64748b"
var inverseText = "#ffffff"

// accent is for borders, indicators and focus; filled buttons use accentFill so white labels
// keep an AA contrast ratio, and accentText is the readable accent for text on dark panels.
var accent = "#3b82f6"
var accentText = "#7cb4ff"
var accentFill = "#2563eb"
var accentHover = "#2f6ff0"
var accentPressed = "#1d4ed8"
var focus = "#60a5fa"
var strongFocus = "#bfdbfe"

var border = "#263244"
var borderHover = "#40526d"
var controlBorder = "#334259"
var menuBorder = "#34445c"
var disabledBorder = "#212b3a"

// Red is reserved for real failures (a decode error, a failed save). Limits and unavailable
// states such as a missing frame or an inexact pairing use the warning family instead.
var error = "#f87171"
var errorPanel = "#2a1216"
var errorBorder = "#b91c1c"
var errorText = "#fca5a5"
var warning = "#fbbf24"
var warningPanel = "#241b0d"
var warningBorder = "#7c5a14"
var warningText = "#fde68a"
var success = "#34d399"
var successFill = "#065f46"
var successFillHover = "#047857"
var successBorder = "#10b981"
// Badge ground for success-coloured text; successFill is too light for it to reach AA.
var successPanel = "#064e3b"
// Neutral "this is extra information", not a state: the Alpha channel-view dot and the timeline
// zoom readout. It used to be #38bdf8, byte-identical to sourceA, so an image-workspace badge
// read as "source A". Green is the one saturated hue band still clear of all three sources.
// It shares a family with `success`, which is deliberate and constrained: the two never render
// in the same component (success grounds toasts, information dots a channel badge), and
// `ui.vcs_theme` pins the separation from every source identity colour.
var information = "#22c55e"

// These alpha-bearing colours are intentional semantic overlays, not popup backgrounds.
var modalScrim = "#b3070a10"
var oscPanel = "#ed151c28"
var thumbnailPanel = "#f21a2231"
var thumbnailWell = "#05080d"

// Translucent overlays drawn on top of footage: they must stay legible over bright pixels.
var stageLabel = "#c7090d15"
var stageLabelBorder = "#2d3b50"
var oscGlass = "#eb141b27"
var oscBorder = "#33435b"
var oscGlassHover = "#f21a2332"
// Drag-and-drop scrim. Heavier than modalScrim so the drop hint stays legible over bright
// footage; nothing behind it needs to be read while a file is being dragged.
var dropScrim = "#df0b1421"

// The stage itself: the well behind footage and the marks drawn on the picture. Their
// translucency lives in the colour, never in `opacity`, so child items cannot fade with them.
var stageWell = "#06080d"
var stageDivider = "#bfd8e2f2"
var lineHalo = "#4d000000"
var selectionFill = "#223b82f6"
var cropFill = "#2214b8a6"
var cropBorder = "#5eead4"
// Pixel probe. Yellow stays clear of the accent-blue selection, the teal crop and every
// source hue; the pointer's own panel gets a faint white line so it never hides the cursor.
var probe = "#facc15"
var probeLine = "#d9facc15"
var probeLineMuted = "#55ffffff"
var probePip = "#ccffffff"

var timelineRail = "#243146"
// In/out band: accentText at 30 % alpha, so the brackets drawn on top of it stay fully opaque.
var rangeBand = "#4d7cb4ff"
// Alignment markers. A missing frame is a real gap and uses error; the other kinds only need
// to be told apart from each other.
//
// These four deliberately sit outside the A/B/C source-identity hues (sky #38bdf8 198 deg,
// orange #fb923c 27 deg, violet #a78bfa 255 deg). The rail is 4-11 px wide, so a marker that
// merely *resembles* a source chip reads as "this is source B" instead of "this frame repeated":
// duplicate and information used to be byte-identical to sourceB and sourceA, and extra was
// ~6 deg from sourceC. Anchor and other are near-neutral, which separates them from every
// saturated hue by chroma rather than by hue. `ui.vcs_theme` asserts the separation.
var markerDuplicate = "#a3e635"
var markerExtra = "#f0abfc"
var markerAnchor = "#e2e8f0"
var markerOther = "#a8a29e"
var keycap = "#111827"
var keycapBorder = "#2f3d52"

// Shape and interaction tokens.
var radiusSmall = 4
var radiusMedium = 6
var radiusLarge = 8
var radiusCard = 12
var radiusPill = 999

var fluentHover = "#14ffffff"
var fluentPressed = "#0cffffff"
var subtleBorder = "#1fffffff"

// Source identity follows the application icon: A is sky, B is orange, C is violet. Use the
// helpers so chips, badges, labels and cards cannot assign different colours to one slot.
var sourceA = "#38bdf8"
var sourceABackground = "#0b2536"
var sourceABorder = "#0e7fb8"

var sourceB = "#fb923c"
var sourceBBackground = "#33190a"
var sourceBBorder = "#c2560f"

var sourceC = "#a78bfa"
var sourceCBackground = "#241640"
var sourceCBorder = "#7c5ce0"

// Dark text for letters drawn on a filled source colour.
var sourceInk = "#0b0f17"

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

// Maps a source label such as "A" or "源 B" to its slot; -1 when no letter is present.
function slotForLabel(label) {
    const match = /[ABC]/.exec(String(label || ""));
    return match ? match[0].charCodeAt(0) - 65 : -1;
}
