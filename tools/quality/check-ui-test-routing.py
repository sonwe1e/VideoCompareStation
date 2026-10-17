"""Check the actual generated UI test compilation, including the model-only preset."""

import argparse
import json
from pathlib import Path
import shutil
import subprocess
import tempfile


MODEL = {
    "ClipExportControllerTests.cpp", "EvidenceIdentityJsonTests.cpp", "JogStateMachineTests.cpp",
    "PairMetricsControllerTests.cpp", "PreviewThumbnailControllerTests.cpp",
    "ReviewPreferencesControllerTests.cpp", "SampleExportControllerTests.cpp",
    "SourceIdentityTests.cpp", "VideoFolderModelTests.cpp",
}
ADAPTER = {
    "DiagnosticsProbeTests.cpp", "HoldKeyHandlerTests.cpp", "JogKeyHandlerTests.cpp",
    "ReviewControllerTests.cpp", "RuntimeBridges.cpp", "StartupSplashTests.cpp",
    "InterpolationProtocolControllerTests.cpp", "IssueLogControllerTests.cpp",
    "PairMetricsThresholdReuseTests.cpp", "TimingDiagnosticsControllerTests.cpp",
}


def validate(actual, adapters):
    if len(MODEL) != 9 or len(ADAPTER) != 10:
        raise ValueError("UI source contract count changed")
    expected = MODEL | ADAPTER if adapters else MODEL
    if actual != expected:
        raise ValueError(f"UI source routing mismatch: missing={expected - actual}, extra={actual - expected}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--adapters", choices=("on", "off"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        root = Path(__file__).resolve().parents[2]
        probe_root = root / "out/verification/ci-ui-routing"
        probe_root.mkdir(parents=True, exist_ok=True)
        probe = Path(tempfile.mkdtemp(dir=probe_root))
        source_path = root / "tests/component/ui/CMakeLists.txt"
        original = source_path.read_text(encoding="utf-8")
        cmake = shutil.which("cmake")
        if cmake is None:
            raise ValueError("CMake is required for the compiler-free source routing probe")

        def evaluate(path, adapters):
            trace = probe / "sources.txt"
            subprocess.run([cmake, f"-DSOURCE_ROOT={root.as_posix()}",
                            f"-DTEST_CMAKE={path.as_posix()}", f"-DTRACE_OUTPUT={trace.as_posix()}",
                            f"-DADAPTERS={'ON' if adapters else 'OFF'}", "-P",
                            str(root / "tests/fixtures/ci-ui-routing/trace.cmake")], check=True)
            return {Path(line).name for line in trace.read_text(encoding="utf-8").splitlines()}

        validate(evaluate(source_path, False), False)
        validate(evaluate(source_path, True), True)
        cases = 2
        bridge = '    "${PROJECT_SOURCE_DIR}/src/app/RuntimeBridges.cpp"\n'
        if original.count(bridge) != 1 or original.count("if(DVS_BUILD_ADAPTERS)") != 1:
            raise ValueError("Expected one bridge source and one adapter guard anchor")
        mutations = [(original.replace(bridge, ""), True),
                     (original.replace("if(DVS_BUILD_ADAPTERS)", "if(TRUE)"), False)]
        for index, (text, adapters) in enumerate(mutations):
            path = probe / f"mutation-{index}.cmake"
            path.write_text(text, encoding="utf-8")
            try:
                validate(evaluate(path, adapters), adapters)
            except ValueError:
                cases += 1
                continue
            raise ValueError("Invalid source routing accepted")
        try:
            validate(set(), False)
        except ValueError:
            cases += 1
        if cases != 5:
            raise ValueError(f"Expected five cases, got {cases}")
        print(f"UI routing contracts passed: {cases} cases, 9 model and 10 adapter sources.")
        return
    if args.build_dir is None or args.adapters is None:
        parser.error("--build-dir and --adapters are required")
    entries = json.loads((args.build_dir / "compile_commands.json").read_text(encoding="utf-8"))
    actual = {
        Path(entry["file"]).name for entry in entries
        if "dvs_ui_component_tests.dir/" in entry["command"].replace("\\", "/")
    }
    validate(actual, args.adapters == "on")
    print(f"Generated UI source routing passed: {len(actual)} sources, adapters={args.adapters}.")


if __name__ == "__main__":
    main()
