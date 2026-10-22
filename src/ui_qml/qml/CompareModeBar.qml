pragma ComponentBehavior: Bound

import QtQuick
import "VcsTheme.js" as Theme
import Dvs.Ui 1.0

Rectangle {
    id: control

    required property int sourceCount
    required property int currentMode
    required property int differenceMetric
    required property var differenceEdges
    required property int currentEdgeIndex
    required property bool inspectorOpen
    required property bool busy
    // The keyboard lock outlives the button press, so the button has to say so.
    required property bool peekLocked
    // Set while the session has no identifiable reference (GT) source; the peek is offered by
    // the keyboard only when this is false, and the button explains itself instead of lying.
    required property bool peekTargetValid
    // V02 (P05): session slot the full canvas is focused on (-1 = regular comparison view),
    // and the slot carrying the reference role. Both default to "unknown" so an unwired bar
    // stays inert instead of checking an entry at random; the contract tests cross-check the
    // bindings once Main.qml wires them.
    property int canvasFocusSlot: -1
    property int referenceSourceSlot: -1
    // UX-13 S4 compact tier (docs/plans/ux13-narrow-window-redesign.md §2 D4, ruling Q2):
    // below 680 px window height this bar shares one 40 px row with the source strip. The
    // four daily tasks stay on the row as two-character segmented labels (并排/分割/局部/
    // 差异, the UX-10 vocabulary shortened, each keeping its wide-row helpText); the direct
    // look buttons, the layout submenu, the pair selector, the candidate switch and both
    // comparison exports fold into the on-demand 更多 menu. The hold-to-peek GT button
    // stays on the row while a difference task is active - a menu row cannot be held down.
    // A plain property so standalone hosts pin the tier explicitly; Main.qml derives it
    // from the hosting window the same way PlayerOsc and ImageWorkspace do.
    property bool compactChrome: false
    property color panelColor: Theme.menu
    property color borderColor: Theme.border
    property color accentColor: Theme.accent
    property color textColor: Theme.primaryText

    signal modeRequested(int mode)
    signal edgeRequested(int preferenceValue)
    // Step-2 difference entry: one control offers the two flavors the review named —
    // overlay-highlight to locate defects on the original, pure diff for distribution.
    signal differenceViewRequested(int metric)
    // Hold-to-peek: while held the surface replaces the difference pass with the reference (GT)
    // source of the session; releasing restores the difference. Same target the keyboard peek
    // and the viewport indicator name.
    signal differencePeekChanged(bool held)
    // Step-2 "固定 GT 切候选": one action flips the candidate side against the fixed
    // reference; Main keeps the frame, zoom/pan and wipe split untouched.
    signal switchCandidateRequested
    // V02 (P05) full-canvas direct look: one request switches which participating source the
    // whole canvas shows. Same frame, same ROI/zoom/pan, no seek, no GT role change, no
    // re-pairing - presentation state only. requestCode follows ReviewShellController's
    // CanvasFocusRequest (0 off, 1 reference, 2 session slot); sourceSlot addresses the
    // session order (A=0, B=1). Re-clicking the active entry requests Off.
    signal fullCanvasFocusRequested(int requestCode, int sourceSlot)
    // Step-2 comparison export: one action copies the comparison picture, one saves it as a
    // PNG file. Copying cannot reach a folder, so the same capture gets both entry points.
    signal copyComparisonRequested
    signal saveComparisonRequested
    signal inspectorRequested
    // EX-03: a single-video session keeps this bar on screen, and the way out of that state is
    // the very same "添加视频" action the menu bar and the source strip already use.
    signal addVideoRequested

    // qmllint disable import unqualified unresolved-type
    readonly property bool pairRelevant: currentMode === ComparisonSurface.Wipe || currentMode === ComparisonSurface.Difference || currentMode === ComparisonSurface.AnalysisGrid
    // V03: the three secondary layouts keep living one level down, under the 布局 menu - they
    // refine how the sources are arranged, they are not daily tasks of their own.
    readonly property bool layoutModeActive: currentMode === ComparisonSurface.ThreeUp || currentMode === ComparisonSurface.ReferenceFocus || currentMode === ComparisonSurface.AnalysisGrid
    readonly property string layoutModeLabel: {
        if (currentMode === ComparisonSurface.ThreeUp)
            return qsTr("三联");
        if (currentMode === ComparisonSurface.ReferenceFocus)
            return qsTr("参考聚焦");
        if (currentMode === ComparisonSurface.AnalysisGrid)
            return qsTr("分析网格");
        return "";
    }
    // The button names the active quick flavor; other metrics (signed/heatmap/...) keep
    // the plain "差异" label and stay selectable from the inspector. V03: the dual-reference
    // difference counts as a difference-locating flavor, so the task button names it too.
    readonly property string differenceFlavorLabel: currentMode === ComparisonSurface.Difference ? (differenceMetric === ComparisonSurface.Highlight ? qsTr("叠加高亮") : (differenceMetric === ComparisonSurface.RgbAbsolute ? qsTr("纯差异图") : "")) : (currentMode === ComparisonSurface.DualDifference ? qsTr("双参考") : "")
    // qmllint enable import unqualified unresolved-type

    // V02 (P05): request codes mirroring ReviewShellController::CanvasFocusRequest. Exposed as
    // properties so the emitted values are readable in one place; the shell contract tests
    // cross-check these against the C++ enum so a drift fails a test instead of silently
    // re-targeting the canvas.
    readonly property int canvasFocusRequestOff: 0
    readonly property int canvasFocusRequestReference: 1
    readonly property int canvasFocusRequestSourceSlot: 2

    // UX-13 S4: guarded accessors for the folded pair-edge rows of the compact 更多 menu.
    // differenceEdges is a QVariantList; the static bounded rows (three sources cap the set
    // at C(3,2) = 3 edges) read through these so an absent entry yields "" / -1 instead of a
    // TypeError on undefined - and the row's own visible guard keeps the empty values off
    // the screen. preferenceValue follows the exact conversion the wide-row combo's
    // onActivated applies.
    function pairEdgeLabel(index) {
        return index >= 0 && index < differenceEdges.length ? String(differenceEdges[index].pairLabel) : "";
    }

    function pairEdgePreference(index) {
        return index >= 0 && index < differenceEdges.length ? Number(differenceEdges[index].preferenceValue) : -1;
    }

    // EX-03: a session with one video is not a comparison yet, but it is the state every
    // comparison starts from - so the bar stays visible and the four comparison tasks stay on
    // screen, disabled, next to the one sentence that says why. The entries that only make
    // sense once a second source exists (direct look, layout menu) appear with it.
    readonly property bool comparisonReady: sourceCount > 1
    readonly property bool singleSource: sourceCount === 1

    objectName: "compareModeBar"
    // Compact tier pins the row to the 40 px band (barRowHeight; barRowHeightMin is not used
    // by UX-13): a wrap here must be an overflow into the 更多 menu, never a second line.
    height: sourceCount >= 1 ? (compactChrome ? Theme.barRowHeight : Math.max(Theme.barRowHeight, modeActions.implicitHeight + 8)) : 0
    visible: sourceCount >= 1
    color: "transparent"
    border.color: "transparent"

    // Flows created before the window is exposed otherwise keep their width-zero layout
    // until the first render. Mode changes also reveal conditional actions between frames.
    Component.onCompleted: modeActions.forceLayout()
    onCurrentModeChanged: Qt.callLater(modeActions.forceLayout)
    onSourceCountChanged: Qt.callLater(modeActions.forceLayout)
    // S4/S3 lesson: the tier flip rewrites the Flow's right anchor and every button's
    // implicit width between frames, and the Flow's own onWidthChanged fires before those
    // settle - re-arm the explicit relayout once, after the current binding cycle.
    onCompactChromeChanged: Qt.callLater(modeActions.forceLayout)

    // The mode bar is the last chrome row above the stage; one hairline separates the two.
    Rectangle {
        height: 1
        color: Theme.border
        anchors {
            left: parent.left
            right: parent.right
            bottom: parent.bottom
        }
    }

    Flow {
        id: modeActions

        objectName: "compareModeActions"
        spacing: 6
        onWidthChanged: forceLayout()
        anchors {
            left: parent.left
            leftMargin: 12
            // Compact tier: the folded copy/save buttons leave the right edge, so the flow
            // reaches the inspector toggle directly (same AnchorLine type, discrete switch).
            right: control.compactChrome ? inspectorToggleButton.left : saveComparisonButton.left
            rightMargin: 12
            top: parent.top
            topMargin: 4
        }

        // qmllint disable import unqualified unresolved-type
        ModeButton {
            objectName: "sideModeButton"
            // Compact tier keeps the two-character segmented label (ruling Q2).
            text: qsTr("并排")
            modeValue: ComparisonSurface.SideBySide
            helpText: qsTr("按 1 → 左右两窗看同一帧")
        }
        ModeButton {
            objectName: "wipeModeButton"
            // One name for this control everywhere it appears: toolbar, menu, inspector and
            // help all say 分割线 (UX-10). The old toolbar label was the only 划线. The
            // compact tier shortens it to 分割 - the same UX-10 noun minus its suffix, with
            // the full wording living on in the item's helpText below.
            text: control.compactChrome ? qsTr("分割") : qsTr("分割线")
            modeValue: ComparisonSurface.Wipe
            helpText: qsTr("按 2 → 拖分界线对照同一位置")
        }
        // V03: local comparison is a top-level task, not an overflow entry. V01 already
        // samples the decoded source textures for 2-3 same-ROI windows under one shared ROI,
        // so the entry only has to reach the existing RoiTriplet preset - same mode enum,
        // same renderer path, nothing new to invent. Two sources give two windows, three
        // give three.
        ModeButton {
            objectName: "roiTripletTaskButton"
            text: control.compactChrome ? qsTr("局部") : qsTr("局部联动")
            modeValue: ComparisonSurface.RoiTriplet
            helpText: qsTr("同一关注区域 → 2–3 个窗口同步放大")
        }
        // Step-2 difference entry: the button itself offers the flavors the review named
        // (overlay highlight / pure diff), mirroring the image workspace's diff dropdown.
        DiffModeButton {
            objectName: "diffModeButton"
            // Compact tier: the two-character task label; the active flavor stays readable
            // through the flavor menu's checked rows instead of the button title.
            text: control.compactChrome ? qsTr("差异 ▾") : (control.differenceFlavorLabel.length > 0 ? qsTr("差异·%1 ▾").arg(control.differenceFlavorLabel) : qsTr("差异定位 ▾"))
            helpText: qsTr("按 3 → 画出像素差，再回原图确认")
        }
        // EX-03: the four tasks above are disabled without a second source, and a greyed
        // button alone never says why. This sentence sits in the same row, in the muted
        // caption colour, and wraps instead of shrinking whenever the window is narrow.
        Text {
            objectName: "singleSourceCompareHint"

            visible: control.singleSource
            // Compact tier: one 40 px row cannot afford the wrap, so the sentence elides on
            // a fixed band and the full wording rides on the tooltip - the reason stays on
            // screen next to the disabled tasks either way (EX-03), never shrunk or hidden.
            width: control.compactChrome ? 120 : 160
            text: qsTr("对比需要至少两个视频")
            color: Theme.mutedText
            font.pixelSize: Theme.fontCaption
            lineHeight: Theme.lineHeightCompact
            elide: control.compactChrome ? Text.ElideRight : Text.ElideNone
            wrapMode: control.compactChrome ? Text.NoWrap : Text.WordWrap

            HoverHandler {
                id: singleHintHover
            }

            VcsToolTip {
                visible: control.compactChrome && singleHintHover.hovered
                text: qsTr("对比需要至少两个视频")
            }
        }
        // EX-03: the way out of the single-source state, in the bar itself - the same action
        // the 文件 menu and the source strip run, so there is one add path, not a second one.
        VcsToolButton {
            objectName: "modeBarAddVideoButton"
            visible: control.singleSource
            text: qsTr("添加视频…")
            implicitWidth: 96
            implicitHeight: Theme.hitAreaMinimum
            labelPixelSize: Theme.fontBody
            enabled: !control.busy
            helpText: qsTr("选好第二个视频 → 自动进入对比")
            Accessible.name: qsTr("添加视频")
            onClicked: control.addVideoRequested()
        }
        // qmllint disable import unqualified unresolved-type
        // Middle group - "who is compared against whom": one shallow capsule carries the pair
        // selector, the candidate switch and (while a difference task with a valid reference is
        // active) the hold-to-peek entry, so the pairing cluster reads as one unit between the
        // layout tasks (left) and the direct-look group (right). The capsule keeps the row's
        // own 32 px band so the bar stays at its 40 px height, and the Flow still wraps the
        // whole capsule as one item on narrow windows.
        // The visibility condition states the INTENT (three sources in a pair-relevant task,
        // or a difference task with a peekable reference) from control properties only. It
        // must not OR the children's own `visible` values: reading an item's `visible` from
        // QML yields the effective visibility (ancestors included), so once this capsule hid
        // itself - e.g. created in side-by-side - the children would read as hidden THROUGH
        // the capsule and the group could never light up again (self-latch).
        Rectangle {
            objectName: "comparePairGroup"
            visible: ((control.sourceCount === 3 && control.pairRelevant && !control.compactChrome) || ((control.currentMode === ComparisonSurface.Difference || control.currentMode === ComparisonSurface.DualDifference) && control.peekTargetValid))
            // The Flow positions by width/height, and this capsule's own inputs are only
            // correct after the inner Row has distributed its children - which, in the
            // no-render hosts this bar's explicit relayout chain exists for, needs that
            // chain to run first. Stating width/height here (instead of leaving them to
            // follow the implicit size) makes the Flow read the settled values, and the
            // visible-gated collapse keeps a hidden capsule from reserving a ghost slot.
            implicitWidth: pairGroupRow.implicitWidth + 8
            width: visible ? implicitWidth : 0
            implicitHeight: Theme.hitAreaMinimum
            height: implicitHeight
            // The width collapse above makes this capsule a moving target for the outer
            // Flow, and the Flow's own re-layout is driven by the explicit hooks below the
            // bar - not by a render pass. Re-arm that hook whenever the capsule resizes, or
            // the Flow keeps stale offsets for the entries after it.
            onWidthChanged: Qt.callLater(modeActions.forceLayout)
            // Positioners skip children whose EFFECTIVE visibility is off, and this
            // capsule's own flip can evaluate after its children's width flips: a re-layout
            // armed by the children alone then runs while the capsule is still hidden and
            // skips every child, never to be re-armed. Re-run the chain once the capsule's
            // own visibility settles, after the current binding cycle completes.
            onVisibleChanged: Qt.callLater(() => {
                pairGroupRow.forceLayout();
                modeActions.forceLayout();
            })
            radius: Theme.radiusMedium
            color: Theme.control
            border.width: 1
            border.color: Theme.border

            Row {
                id: pairGroupRow

                spacing: Theme.spaceMicro
                anchors {
                    verticalCenter: parent.verticalCenter
                    left: parent.left
                    leftMargin: 4
                }
                // The children's widths flip with their `visible` (collapse trick above),
                // but this bar's explicit-forceLayout discipline exists precisely because
                // its host can pump the event loop without a render-driven polish pass
                // (the contract harness never renders). childrenRect follows the children's
                // geometry, so re-distribute off it instead of waiting for a polish that
                // never comes; the call settles once the row is laid out.
                onChildrenRectChanged: forceLayout()

                ToolbarCombo {
                    id: pairComboBox

                    objectName: "pairCombo"
                    // Compact tier: the pair selector folds into the 更多 menu (per-edge rows
                    // named by the same pairLabel role); the capsule below then carries the
                    // peek alone.
                    visible: control.sourceCount === 3 && control.pairRelevant && !control.compactChrome
                    // Positioners reserve width for invisible children; collapse so a
                    // peek-only capsule (dual difference) does not carry the combo's ghost.
                    width: visible ? implicitWidth : 0
                    implicitWidth: 104
                    implicitHeight: Theme.hitAreaMinimum
                    // V02: the selector names the two real files ("gt.mp4 ↔ model.mp4") instead
                    // of the A/B/C slot letters, so a same-letter relabel can never masquerade as
                    // a pair switch. The role keeping the letters ("label") stays for the
                    // inspector rows.
                    model: control.differenceEdges
                    textRole: "pairLabel"
                    currentIndex: control.currentEdgeIndex
                    Accessible.name: qsTr("比较对象")
                    hoverEnabled: true
                    leftPadding: 10
                    rightPadding: 26
                    font.pixelSize: Theme.fontBody
                    onActivated: index => {
                        if (index >= 0 && index < control.differenceEdges.length)
                            control.edgeRequested(Number(control.differenceEdges[index].preferenceValue));
                    }
                }

                VcsToolButton {
                    id: switchCandidateButton

                    objectName: "switchCandidateButton"
                    text: qsTr("切候选")
                    visible: control.sourceCount === 3 && control.pairRelevant && !control.compactChrome
                    width: visible ? implicitWidth : 0
                    enabled: !control.busy
                    implicitWidth: 86
                    implicitHeight: Theme.hitAreaMinimum
                    labelPixelSize: Theme.fontBody
                    helpText: qsTr("按 C → 参考不动，换另一个版本")
                    Accessible.name: qsTr("固定参考切换候选")
                    onClicked: control.switchCandidateRequested()
                }

                // Hold-to-peek: while held the surface shows the reference (GT) source instead
                // of the difference pass; releasing restores the difference view. The label
                // names that target and stays visible while the keyboard lock holds it.
                // D2: the dual-reference difference is a difference task too, and the keyboard
                // backtick works there - so the button follows the same condition instead of
                // hiding while the shortcut stays armed. Both entries additionally require an
                // identified reference, matching Main's differencePeekAvailable.
                VcsToolButton {
                    id: differencePeekButton

                    objectName: "differencePeekButton"
                    // Compact tier keeps the hold on the row: a menu row cannot be held down,
                    // so the entry narrows to 按住 instead of folding away. The lock wording
                    // shortens the same way; helpText below keeps both full sentences.
                    text: control.peekLocked ? (control.compactChrome ? qsTr("已锁定") : qsTr("GT 原图·已锁定")) : (control.compactChrome ? qsTr("按住") : qsTr("按住看 GT 原图"))
                    visible: (control.currentMode === ComparisonSurface.Difference || control.currentMode === ComparisonSurface.DualDifference) && control.peekTargetValid
                    width: visible ? implicitWidth : 0
                    enabled: !control.busy && control.peekTargetValid
                    prominent: pressed || control.peekLocked
                    implicitWidth: control.compactChrome ? 64 : 136
                    implicitHeight: Theme.hitAreaMinimum
                    labelPixelSize: Theme.fontBody
                    helpText: control.peekTargetValid ? qsTr("按住 → 看参考原图，松开恢复（`）") : qsTr("会话里还没有可直看的参考源")
                    Accessible.name: text
                    Accessible.description: control.peekTargetValid ? qsTr("按住显示会话的参考（GT）源，即使 GT 不在当前比较对中；松开恢复差异视图。键盘 ` 切换，Shift+` 锁定。") : qsTr("当前会话没有可直看的参考（GT）源。")
                    onPressed: control.differencePeekChanged(true)
                    onReleased: control.differencePeekChanged(false)
                    onCanceled: control.differencePeekChanged(false)
                }
            }
        }
        // qmllint enable import unqualified unresolved-type

        // V02: one direct-look entry per participating source, generated from the session
        // (A/B/C). The source that currently carries the reference (GT) role shows a GT badge
        // on its own button - GT is a role, never a fourth source - so "GT = A" never
        // duplicates the A entry and C gets the same entry A and B have. A click addresses
        // the session slot; re-clicking the checked entry requests Off and returns to the
        // comparison view. The switch keeps the frame, ROI/zoom/pan, the GT role and every
        // other entry on this bar untouched (P05 canvasFocus chain, no new state).
        Rectangle {
            // Compact tier: the separator folds with the button group behind it; the focused
            // source stays visible through the strip chip's own highlight (V02).
            visible: control.comparisonReady && !control.compactChrome
            width: 1
            height: Theme.hitAreaMinimum
            color: Theme.border
        }

        Repeater {
            objectName: "canvasFocusButtonRepeater"
            model: Math.max(0, control.sourceCount)

            FocusButton {
                id: canvasFocusButton

                required property int index
                objectName: "canvasFocus" + String.fromCharCode(65 + index) + "Button"
                text: qsTr("看 %1").arg(String.fromCharCode(65 + index))
                focusSlot: index
                referenceBadge: index === control.referenceSourceSlot
                // EX-03: direct look is a way to look at a source *instead of* comparing it, so
                // it appears with the second source. The delegate stays instantiated and hidden
                // rather than dropped from the model, so a single-source session has no entry
                // here but the per-source binding still exists for anyone holding it.
                // Compact tier (S4): the row folds these into the 更多 menu - the strip chip
                // click stays the one-gesture direct-look entry in both tiers.
                visible: control.comparisonReady && !control.compactChrome
                helpText: qsTr("单击 → 整屏只看 %1，再点返回").arg(String.fromCharCode(65 + index))
            }
        }

        // V03: the three secondary layouts move one level down into a labelled 布局 menu
        // instead of sharing a flat "…" overflow with the daily tasks. The button names the
        // active layout so the current arrangement stays readable from the top level.
        VcsToolButton {
            id: layoutModesButton

            objectName: "layoutModesButton"
            text: control.layoutModeActive ? qsTr("布局·%1 ▾").arg(control.layoutModeLabel) : qsTr("布局 ▾")
            // Compact tier: the submenu moves into the 更多 menu (ruling Q2), where each
            // layout keeps its own checked row and its digit shortcut.
            visible: control.comparisonReady && !control.compactChrome
            implicitWidth: control.layoutModeActive ? 136 : 84
            implicitHeight: Theme.hitAreaMinimum
            helpText: qsTr("换排布 → 三联、参考聚焦、分析网格")
            onClicked: layoutMenu.open()
            labelPixelSize: Theme.fontBody

            VcsMenu {
                id: layoutMenu
                menuWidth: 220

                // qmllint disable import unqualified unresolved-type
                VcsRadioMenuItem {
                    id: modeBarThreeUpItem

                    objectName: "modeBarThreeUpItem"
                    text: qsTr("三联")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ThreeUp
                    onTriggered: control.modeRequested(ComparisonSurface.ThreeUp)
                }
                VcsRadioMenuItem {
                    id: modeBarReferenceFocusItem

                    objectName: "modeBarReferenceFocusItem"
                    text: qsTr("参考聚焦")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ReferenceFocus
                    onTriggered: control.modeRequested(ComparisonSurface.ReferenceFocus)
                }
                VcsRadioMenuItem {
                    id: modeBarAnalysisGridItem

                    objectName: "modeBarAnalysisGridItem"
                    text: qsTr("分析网格")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.AnalysisGrid
                    onTriggered: control.modeRequested(ComparisonSurface.AnalysisGrid)
                }
                // qmllint enable import unqualified unresolved-type
            }
        }

        // UX-13 S4 compact tier (ruling Q2): one labelled 更多 menu carries everything the
        // merged row cannot spend width on - the direct-look group, the layout submenu, the
        // pair selector, the candidate switch and both comparison exports. Every entry
        // drives the exact signal its wide-row twin drives, and keeps its digit/letter
        // shortcut in the trailing column, so folding costs width only, never a path. The
        // menu is a popup: its rows materialize on open, nothing sits in the scene graph
        // while it is closed (plan §4).
        VcsToolButton {
            id: compactModeMoreButton

            objectName: "compactModeMoreButton"
            visible: control.compactChrome
            text: qsTr("更多 ▾")
            implicitWidth: 72
            implicitHeight: Theme.hitAreaMinimum
            labelPixelSize: Theme.fontBody
            helpText: qsTr("直看、布局排布、比较对象与对比图导出")
            onClicked: compactMoreMenu.open()

            VcsMenu {
                id: compactMoreMenu

                objectName: "compactModeMoreMenu"
                menuWidth: 220

                // V02 direct look, folded: one radio row per slot, the checked row naming
                // the source the whole canvas shows; the strip chip click stays the
                // one-gesture entry, this is the labelled one.
                VcsRadioMenuItem {
                    objectName: "compactCanvasFocusAItem"
                    visible: control.comparisonReady
                    text: control.referenceSourceSlot === 0 ? qsTr("看 A（GT）") : qsTr("看 A")
                    helpText: qsTr("单击 → 整屏只看 A，再点返回")
                    checked: control.canvasFocusSlot === 0
                    onTriggered: {
                        if (control.canvasFocusSlot === 0)
                            control.fullCanvasFocusRequested(control.canvasFocusRequestOff, 0);
                        else
                            control.fullCanvasFocusRequested(control.canvasFocusRequestSourceSlot, 0);
                    }
                }
                VcsRadioMenuItem {
                    objectName: "compactCanvasFocusBItem"
                    visible: control.comparisonReady
                    text: control.referenceSourceSlot === 1 ? qsTr("看 B（GT）") : qsTr("看 B")
                    helpText: qsTr("单击 → 整屏只看 B，再点返回")
                    checked: control.canvasFocusSlot === 1
                    onTriggered: {
                        if (control.canvasFocusSlot === 1)
                            control.fullCanvasFocusRequested(control.canvasFocusRequestOff, 0);
                        else
                            control.fullCanvasFocusRequested(control.canvasFocusRequestSourceSlot, 1);
                    }
                }
                VcsRadioMenuItem {
                    objectName: "compactCanvasFocusCItem"
                    visible: control.comparisonReady && control.sourceCount === 3
                    text: control.referenceSourceSlot === 2 ? qsTr("看 C（GT）") : qsTr("看 C")
                    helpText: qsTr("单击 → 整屏只看 C，再点返回")
                    checked: control.canvasFocusSlot === 2
                    onTriggered: {
                        if (control.canvasFocusSlot === 2)
                            control.fullCanvasFocusRequested(control.canvasFocusRequestOff, 0);
                        else
                            control.fullCanvasFocusRequested(control.canvasFocusRequestSourceSlot, 2);
                    }
                }
                VcsMenuSeparator {
                    visible: control.comparisonReady
                }
                // The layout submenu, one level flatter than the wide row's 布局 button.
                // qmllint disable import unqualified unresolved-type
                VcsRadioMenuItem {
                    objectName: "compactThreeUpItem"
                    text: qsTr("三联")
                    shortcutText: qsTr("4")
                    // Same wording the shortcut table builds for this key (ReviewShortcuts
                    // ModeKeyShortcut: 切换视图：%1), so the menu row and the help table say
                    // the same sentence (EX-04).
                    helpText: qsTr("按 4 → 切换视图：三联")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ThreeUp
                    onTriggered: control.modeRequested(ComparisonSurface.ThreeUp)
                }
                VcsRadioMenuItem {
                    objectName: "compactReferenceFocusItem"
                    text: qsTr("参考聚焦")
                    shortcutText: qsTr("5")
                    helpText: qsTr("按 5 → 切换视图：参考聚焦")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.ReferenceFocus
                    onTriggered: control.modeRequested(ComparisonSurface.ReferenceFocus)
                }
                VcsRadioMenuItem {
                    objectName: "compactAnalysisGridItem"
                    text: qsTr("分析网格")
                    shortcutText: qsTr("6")
                    helpText: qsTr("按 6 → 切换视图：分析网格")
                    enabled: control.sourceCount === 3
                    checked: control.currentMode === ComparisonSurface.AnalysisGrid
                    onTriggered: control.modeRequested(ComparisonSurface.AnalysisGrid)
                }
                // qmllint enable import unqualified unresolved-type
                VcsMenuSeparator {
                    visible: control.sourceCount === 3 && control.pairRelevant
                }
                // The pair selector, folded: one radio row per edge, named by the same
                // pairLabel role the wide-row combo shows (the two real files, never A↔B).
                // Static bounded rows, not dynamic insertion: the product caps a session at
                // three sources, so C(3,2) = 3 edges is the hard maximum, and this Qt build's
                // VcsMenu instances do not expose QQuickMenu's indexOf/insertItem/removeItem
                // invokables at runtime (the S4 gate TypeError). Every row is a pure binding
                // over differenceEdges with its own length guard - no invokable, no
                // dynamic menu mutation, nothing to relayout.
                VcsRadioMenuItem {
                    objectName: "compactPairEdgeItem-0"

                    visible: control.sourceCount === 3 && control.pairRelevant && control.differenceEdges.length > 0
                    text: control.pairEdgeLabel(0)
                    // Same selector the wide-row combo exposes (Accessible.name), so the
                    // folded row explains itself with the one word the pair UI uses.
                    helpText: qsTr("比较对象：切换要对比的两个视频")
                    checked: control.currentEdgeIndex === 0
                    onTriggered: control.edgeRequested(control.pairEdgePreference(0))
                }
                VcsRadioMenuItem {
                    objectName: "compactPairEdgeItem-1"

                    visible: control.sourceCount === 3 && control.pairRelevant && control.differenceEdges.length > 1
                    text: control.pairEdgeLabel(1)
                    helpText: qsTr("比较对象：切换要对比的两个视频")
                    checked: control.currentEdgeIndex === 1
                    onTriggered: control.edgeRequested(control.pairEdgePreference(1))
                }
                VcsRadioMenuItem {
                    objectName: "compactPairEdgeItem-2"

                    visible: control.sourceCount === 3 && control.pairRelevant && control.differenceEdges.length > 2
                    text: control.pairEdgeLabel(2)
                    helpText: qsTr("比较对象：切换要对比的两个视频")
                    checked: control.currentEdgeIndex === 2
                    onTriggered: control.edgeRequested(control.pairEdgePreference(2))
                }
                VcsMenuItem {
                    id: compactSwitchCandidateItem

                    objectName: "compactSwitchCandidateItem"
                    visible: control.sourceCount === 3 && control.pairRelevant
                    text: qsTr("切候选（固定参考）")
                    shortcutText: qsTr("C")
                    // EX-04: the exact sentence the wide-row switchCandidateButton carries.
                    helpText: qsTr("按 C → 参考不动，换另一个版本")
                    enabled: !control.busy
                    onTriggered: control.switchCandidateRequested()
                }
                VcsMenuSeparator {}
                VcsMenuItem {
                    objectName: "compactCopyComparisonItem"
                    text: qsTr("复制对比图")
                    // EX-04: the exact sentence the wide-row copyComparisonButton carries.
                    helpText: qsTr("复制当前对比画面到剪贴板")
                    enabled: !control.busy
                    onTriggered: control.copyComparisonRequested()
                }
                VcsMenuItem {
                    objectName: "compactSaveComparisonItem"
                    text: qsTr("另存为对比图…")
                    // EX-04: the exact sentence the wide-row saveComparisonButton carries.
                    helpText: qsTr("把当前对比画面另存为 PNG 文件")
                    enabled: !control.busy
                    onTriggered: control.saveComparisonRequested()
                }
            }
        }
    }

    component ModeButton: ReviewActionButton {
        id: modeButton

        required property int modeValue
        checkable: true
        checked: control.canvasFocusSlot < 0 && control.currentMode === modeValue
        mirrorsState: true
        implicitHeight: Theme.hitAreaMinimum
        // Compact tier: the segmented band tightens around the two-character labels.
        implicitWidth: control.compactChrome ? 56 : 80
        // EX-03: a comparison task needs something to compare against. The reason is printed
        // beside the buttons (singleSourceCompareHint), never left to the grey alone.
        enabled: !control.busy && control.comparisonReady
        leftPadding: 10
        rightPadding: 10
        onClicked: control.modeRequested(modeValue)

        contentItem: Text {
            text: modeButton.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            // Checked wins over disabled: while busy disables the row, the active mode must
            // stay readable instead of collapsing into the disabled grey.
            color: modeButton.checked ? Theme.primaryText : (!modeButton.enabled ? Theme.disabledText : (modeButton.hovered ? Theme.primaryText : Theme.mutedText))
            font.pixelSize: Theme.fontBody
            font.weight: modeButton.checked ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (modeButton.checked)
                    return Theme.controlChecked;
                if (!modeButton.enabled)
                    return "transparent";
                if (modeButton.down)
                    return Theme.fluentPressed;
                if (modeButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: modeButton.activeFocus ? 2 : 1
            border.color: modeButton.activeFocus ? Theme.focus : "transparent"

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
        }
    }

    // V02: one full-canvas direct-look entry per source slot. Checked mirrors the shell's
    // focus slot; clicking the checked entry requests Off so the group always has a way back
    // to the regular comparison view. referenceBadge marks the slot that carries the session's
    // GT role - a badge on the source's own button, never a duplicated GT button.
    component FocusButton: ReviewActionButton {
        id: focusButton

        required property int focusSlot
        property bool referenceBadge: false
        checkable: true
        checked: control.canvasFocusSlot >= 0 && control.canvasFocusSlot === focusSlot
        mirrorsState: true
        implicitHeight: Theme.hitAreaMinimum
        implicitWidth: focusButton.referenceBadge ? 104 : 72
        enabled: !control.busy
        leftPadding: 10
        rightPadding: 10
        onClicked: {
            if (focusButton.checked)
                control.fullCanvasFocusRequested(control.canvasFocusRequestOff, 0);
            else
                control.fullCanvasFocusRequested(control.canvasFocusRequestSourceSlot, focusSlot);
        }

        contentItem: Row {
            spacing: Theme.spaceMicro

            Text {
                text: focusButton.text
                // Same checked-over-disabled rule as ModeButton.
                color: focusButton.checked ? Theme.primaryText : (!focusButton.enabled ? Theme.disabledText : (focusButton.hovered ? Theme.primaryText : Theme.mutedText))
                font.pixelSize: Theme.fontBody
                font.weight: focusButton.checked ? Font.DemiBold : Font.Normal
                anchors.verticalCenter: parent.verticalCenter
            }

            Rectangle {
                visible: focusButton.referenceBadge
                width: gtBadgeLabel.implicitWidth + 8
                height: 18
                radius: height / 2
                color: Theme.controlChecked
                border.width: 1
                border.color: Theme.accent
                anchors.verticalCenter: parent.verticalCenter
                Accessible.name: qsTr("参考源（GT）")
                Accessible.description: qsTr("该源当前承担会话的对比基准角色")

                Text {
                    id: gtBadgeLabel

                    anchors.centerIn: parent
                    text: qsTr("GT")
                    color: Theme.primaryText
                    font.pixelSize: Theme.fontSecondary
                    font.weight: Font.DemiBold
                }
            }
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (focusButton.checked)
                    return Theme.controlChecked;
                if (!focusButton.enabled)
                    return "transparent";
                if (focusButton.down)
                    return Theme.fluentPressed;
                if (focusButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            // Direct-look is a strong canvas state: while the whole canvas shows this one
            // source, the button wears the source's own identity colour as its edge, so a
            // focused single view cannot be mistaken for a comparison. Keyboard focus keeps
            // the canonical ring.
            border.width: focusButton.checked ? 2 : (focusButton.activeFocus ? 2 : 1)
            border.color: focusButton.checked ? Theme.sourceBorder(focusButton.focusSlot) : (focusButton.activeFocus ? Theme.focus : "transparent")

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
        }
    }

    // qmllint disable import unqualified unresolved-type
    component DiffModeButton: ReviewActionButton {
        id: diffModeButton

        checkable: true
        // V03: "差异定位" covers both pair differences and the GT-anchored dual difference -
        // one task, two scopes.
        checked: control.canvasFocusSlot < 0 && (control.currentMode === ComparisonSurface.Difference || control.currentMode === ComparisonSurface.DualDifference)
        // Opening the flavour menu must not light the button up before a flavour is chosen.
        mirrorsState: true
        implicitHeight: Theme.hitAreaMinimum
        implicitWidth: control.compactChrome ? 64 : (control.differenceFlavorLabel.length > 0 ? 136 : 100)
        // Same single-source rule as ModeButton (EX-03).
        enabled: !control.busy && control.comparisonReady
        leftPadding: 10
        rightPadding: 10
        onClicked: diffFlavorMenu.open()

        contentItem: Text {
            text: diffModeButton.text
            horizontalAlignment: Text.AlignHCenter
            verticalAlignment: Text.AlignVCenter
            // Same checked-over-disabled rule as ModeButton.
            color: diffModeButton.checked ? Theme.primaryText : (!diffModeButton.enabled ? Theme.disabledText : (diffModeButton.hovered ? Theme.primaryText : Theme.mutedText))
            font.pixelSize: Theme.fontBody
            font.weight: diffModeButton.checked ? Font.DemiBold : Font.Normal
            elide: Text.ElideRight
        }

        background: Rectangle {
            radius: Theme.radiusMedium
            color: {
                if (diffModeButton.checked)
                    return Theme.controlChecked;
                if (!diffModeButton.enabled)
                    return "transparent";
                if (diffModeButton.down)
                    return Theme.fluentPressed;
                if (diffModeButton.hovered)
                    return Theme.fluentHover;
                return "transparent";
            }
            border.width: diffModeButton.activeFocus ? 2 : 1
            border.color: diffModeButton.activeFocus ? Theme.focus : "transparent"

            Behavior on color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
            Behavior on border.color {
                ColorAnimation {
                    duration: Theme.animationFast
                }
            }
        }

        VcsMenu {
            id: diffFlavorMenu
            objectName: "diffFlavorMenu"
            menuWidth: 220

            // V03: with three sources and an identified GT, the GT-anchored dual difference is
            // the recommended way to locate defects (both candidates against the same
            // baseline), so it leads the menu and says so. Without a GT the item explains
            // itself instead of silently picking a pseudo-baseline, and the pair-difference
            // flavors below describe plain source-vs-source differences.
            VcsRadioMenuItem {
                id: dualDifferenceMenuItem

                objectName: "dualDifferenceMenuItem"
                visible: control.sourceCount === 3
                enabled: !control.busy && control.peekTargetValid
                text: control.peekTargetValid ? qsTr("双参考差异（GT 基准）") : qsTr("双参考差异（需先设置参考源）")
                checked: control.currentMode === ComparisonSurface.DualDifference
                onTriggered: control.modeRequested(ComparisonSurface.DualDifference)
            }
            VcsRadioMenuItem {
                objectName: "diffPureMenuItem"

                text: qsTr("纯差异图")
                checked: control.currentMode === ComparisonSurface.Difference && control.differenceMetric === ComparisonSurface.RgbAbsolute
                onTriggered: control.differenceViewRequested(ComparisonSurface.RgbAbsolute)
            }
            VcsRadioMenuItem {
                objectName: "diffHighlightMenuItem"

                text: qsTr("原图叠加高亮")
                checked: control.currentMode === ComparisonSurface.Difference && control.differenceMetric === ComparisonSurface.Highlight
                onTriggered: control.differenceViewRequested(ComparisonSurface.Highlight)
            }
        }
    }
    // qmllint enable import unqualified unresolved-type

    ReviewActionButton {
        id: inspectorToggleButton

        objectName: "inspectorToggleButton"
        // Compact tier: the merged row's right cluster is this toggle (plus the single-video
        // add entry); copy/save live one level down in the 更多 menu.
        text: control.inspectorOpen ? qsTr("隐藏检查器") : qsTr("检查器")
        implicitHeight: Theme.hitAreaMinimum
        leftPadding: 12
        rightPadding: 12
        prominent: control.inspectorOpen
        helpText: qsTr("打开检查器 → 看源信息与差异统计")
        Accessible.name: control.inspectorOpen ? qsTr("隐藏检查器") : qsTr("检查器")
        onClicked: control.inspectorRequested()
        anchors {
            right: parent.right
            rightMargin: 12
            top: parent.top
            topMargin: 4
        }
    }

    // Step-2 comparison export: copies the comparison picture - the rendered comparison with
    // every viewport overlay suppressed - to the clipboard in one action.
    VcsToolButton {
        id: copyComparisonButton

        objectName: "copyComparisonButton"
        // Compact tier: folded into the 更多 menu (compactCopyComparisonItem).
        visible: !control.compactChrome
        text: qsTr("复制对比图")
        enabled: !control.busy
        implicitWidth: 112
        implicitHeight: Theme.hitAreaMinimum
        labelPixelSize: Theme.fontBody
        helpText: qsTr("复制当前对比画面到剪贴板")
        Accessible.name: qsTr("复制对比图")
        onClicked: control.copyComparisonRequested()
        anchors {
            right: inspectorToggleButton.left
            rightMargin: 8
            top: parent.top
            topMargin: 4
        }
    }

    // The same capture as a file: the clipboard cannot reach a folder, so the picture gets a
    // destination picker beside the clipboard entry point.
    VcsToolButton {
        id: saveComparisonButton

        objectName: "saveComparisonButton"
        // Compact tier: folded into the 更多 menu (compactSaveComparisonItem).
        visible: !control.compactChrome
        text: qsTr("另存为…")
        enabled: !control.busy
        implicitWidth: 96
        implicitHeight: Theme.hitAreaMinimum
        labelPixelSize: Theme.fontBody
        helpText: qsTr("把当前对比画面另存为 PNG 文件")
        Accessible.name: qsTr("另存为对比图")
        onClicked: control.saveComparisonRequested()
        anchors {
            right: copyComparisonButton.left
            rightMargin: 8
            top: parent.top
            topMargin: 4
        }
    }
}
