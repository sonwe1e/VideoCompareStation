import QtQuick
import QtQuick.Controls
import QtTest
import "../../../../src/ui_qml/qml" as Dvs

Item {
    id: root

    width: 600
    height: 700

    Dvs.ClipExportDialog {
        id: dialog

        service: ({
                sourcePath: "",
                busy: false,
                progress: 0,
                rangeSummary: "入 2 · 出 3 · 2 帧",
                suggestedFileName: "canonical_clip_2-3.mp4",
                lastStatus: "",
                lastFailureDetail: "",
                lastOutputPath: ""
            })
    }

    TestCase {
        name: "ClipExportDialog"
        when: windowShown

        function cleanup() {
            dialog.close();
            dialog.service = Object.assign({}, dialog.service, {
                busy: false,
                lastStatus: "",
                lastOutputPath: ""
            });
        }

        function test_sourceIdentity_data() {
            return [
                {
                    tag: "windows",
                    path: "C:\\clips\\canonical.mp4",
                    name: "canonical.mp4"
                },
                {
                    tag: "unix",
                    path: "/clips/canonical.mp4",
                    name: "canonical.mp4"
                },
                {
                    tag: "literal",
                    path: "/clips/<b>source.mp4",
                    name: "<b>source.mp4"
                },
                {
                    tag: "unavailable",
                    path: "",
                    name: "（不可导出）"
                }
            ];
        }

        function test_sourceIdentity(data) {
            dialog.service.sourcePath = data.path;
            dialog.open();
            const label = findChild(dialog, "clipExportSourceName");
            compare(label.text, data.name);
            compare(label.textFormat, Text.PlainText);
            compare(label.ToolTip.text, data.path);
            compare(label.Accessible.name, "导出源（时间线主源）：" + data.path);
        }

        function test_reopenRefreshesSource() {
            dialog.service.sourcePath = "/first/canonical.mp4";
            dialog.open();
            dialog.close();
            dialog.service.sourcePath = "/next/changed.mp4";
            dialog.open();
            compare(findChild(dialog, "clipExportSourceName").text, "changed.mp4");
        }

        function test_jobAndProposalLabels_data() {
            return [
                {
                    tag: "running",
                    busy: true,
                    role: "正在导出（时间线主源）",
                    heading: "当前导出状态",
                    output: ""
                },
                {
                    tag: "finished",
                    busy: false,
                    role: "待导出源（时间线主源）",
                    heading: "上次导出结果",
                    output: "C:\\export\\previous-job.mp4"
                }
            ];
        }

        function test_jobAndProposalLabels(data) {
            dialog.service = Object.assign({}, dialog.service, {
                busy: data.busy,
                lastStatus: data.busy ? "正在导出所选区间…" : "已导出所选区间。",
                lastOutputPath: data.output
            });
            dialog.open();
            dialog.close();
            dialog.open();
            compare(findChild(dialog, "clipExportSourceRole").text, data.role);
            compare(findChild(dialog, "clipExportResultHeading").text, data.heading);
            compare(findChild(dialog, "clipExportOutputPath").text, data.output);
        }

        function test_sourceFitsNarrowDialog() {
            const popup = findChild(dialog, "clipExportPopup");
            popup.width = 300;
            dialog.service.sourcePath = "/clips/a-very-long-canonical-video-filename-with-detail.mp4";
            dialog.open();
            const label = findChild(dialog, "clipExportSourceName");
            verify(label.width > 0 && label.width <= 260);
            compare(label.elide, Text.ElideMiddle);
            popup.width = 460;
        }
    }
}
