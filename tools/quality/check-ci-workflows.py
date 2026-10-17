"""Validate actual YAML routing and exercise counterexamples without building or opening UI."""

import argparse
import copy
from pathlib import Path

import yaml


def validate(workflows, action):
    checks = 0

    def require(condition, message):
        nonlocal checks
        checks += 1
        if not condition:
            raise ValueError(message)

    required = {"build-test.yml", "quality.yml", "release.yml", "hardware-performance.yml"}
    require(required <= workflows.keys(), "A required workflow is missing")
    for name, workflow in workflows.items():
        events = workflow.get("on", {})
        require(isinstance(events, dict), f"{name}: explicit event map required")
        if name == "hardware-performance.yml":
            require(set(events) == {"workflow_dispatch"}, "Hardware must be dispatch-only")
        for job_name, job in workflow.get("jobs", {}).items():
            require("uses" not in job, f"{name}/{job_name}: reusable jobs must be audited")
            runner = job.get("runs-on")
            hardware = name == "hardware-performance.yml"
            require(
                runner == ["self-hosted", "Windows", "X64", "dvs-gpu"]
                if hardware else runner in ("windows-2022", "ubuntu-24.04"),
                f"{name}/{job_name}: unexpected runner {runner}",
            )
            if not hardware and runner == "windows-2022" and job_name in (
                "build-test", "native-quality", "quality", "release"
            ):
                require(any(s.get("uses") == "./.github/actions/setup-windows"
                            for s in job.get("steps", [])), f"{name}: missing hosted setup")
    build = workflows["build-test.yml"]["jobs"]["build-test"]
    require({x["preset"] for x in build["strategy"]["matrix"]["include"]} == {"dev", "release"},
            "Both native build configurations are required")
    full = [s for s in build["steps"] if s.get("name") == "Complete software regression"]
    require(len(full) == 1, "Exactly one complete preset regression step is required")
    command = full[0].get("run", "")
    require("--preset ${{ matrix.preset }}" in command and "--no-tests=error" in command,
            "Full regression must use the preset and reject zero tests")
    require(not any(x in command.split() for x in ("-R", "-E", "-L", "-LE", "--exclude-regex")),
            "Complete regression must not filter tests")
    for preset in ("core-dev", "ui-dev"):
        require(any(f"ctest --preset {preset}" in s.get("run", "") for s in build["steps"]),
                f"Missing {preset} validation")
    for mode in ("on", "off"):
        require(any("check-ui-test-routing.py" in s.get("run", "") and
                    f"--adapters {mode}" in s["run"] for s in build["steps"]),
                f"Missing generated source check, adapters={mode}")
    quality = workflows["quality.yml"]["jobs"]["native-quality"]
    for target in ("format-check", "lint"):
        require(any(f"--target {target}" in s.get("run", "") for s in quality["steps"]),
                f"Missing {target} gate")
    release = workflows["release.yml"]
    guard = release["jobs"]["hardware-performance"]
    require(guard.get("if") == "github.event_name == 'workflow_dispatch'",
            "Hardware evidence checks are manual release steps")
    require(any("test-hardware-evidence.ps1" in s.get("run", "") for s in guard["steps"]),
            "Release must validate manual hardware evidence")
    publish = release["jobs"]["release"]
    require("hardware-performance" in publish["needs"], "Publication lost the hardware prerequisite")
    require(publish.get("if") == "github.event_name == 'workflow_dispatch'",
            "Tag pushes must not publish or trigger workstation work")
    steps = action["runs"]["steps"]
    require(any("RUNNER_ENVIRONMENT" in s.get("run", "") and "github-hosted" in s["run"]
                for s in steps), "Hosted provisioning must reject self-hosted runners")
    require(any(s.get("with", {}).get("ref") == "7d1c862c25e1bde631d866d82fdc86213983271c"
                for s in steps), "Pinned vcpkg checkout missing")
    require(any("vcpkg-archives" in s.get("with", {}).get("path", "") and
                "hashFiles" in s["with"].get("key", "") and "msvc" in s["with"]["key"] and
                "inputs.cache-scope" in s["with"]["key"]
                for s in steps), "Dependency cache must include compiler and manifest identity")
    return checks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[2]
    paths = sorted((root / ".github/workflows").glob("*.y*ml"))
    workflows = {p.name: yaml.load(p.read_text(encoding="utf-8"), Loader=yaml.BaseLoader) for p in paths}
    action = yaml.load((root / ".github/actions/setup-windows/action.yml").read_text(encoding="utf-8"),
                       Loader=yaml.BaseLoader)
    checks = validate(workflows, action)
    # Exact count detects removed jobs/checks; new workflows require a deliberate policy update.
    if len(paths) != 4 or checks != 45:
        raise ValueError(f"Expected 4 workflows and 45 checks, got {len(paths)} and {checks}")
    cases = 0
    if args.self_test:
        def rejected(mutate):
            nonlocal cases
            sample, setup = copy.deepcopy(workflows), copy.deepcopy(action)
            mutate(sample, setup)
            try:
                validate(sample, setup)
            except (ValueError, KeyError):
                cases += 1
                return
            raise ValueError("Counterexample was accepted")

        rejected(lambda w, a: w["build-test.yml"]["jobs"]["build-test"].update(
            {"runs-on": ["self-hosted", "Windows", "X64", "dvs-toolchain-4.4"]}))
        rejected(lambda w, a: w["hardware-performance.yml"]["on"].update({"push": {}}))
        rejected(lambda w, a: w["hardware-performance.yml"]["on"].update({"workflow_call": {}}))
        rejected(lambda w, a: w["release.yml"]["jobs"]["release"].update({"needs": ["quality"]}))
        rejected(lambda w, a: w["build-test.yml"]["jobs"]["build-test"]["steps"].__setitem__(
            next(i for i, s in enumerate(w["build-test.yml"]["jobs"]["build-test"]["steps"])
                 if s.get("name") == "Complete software regression"),
            {"name": "Complete software regression", "run": "ctest --preset ${{ matrix.preset }} --no-tests=error -L unit"}))
        rejected(lambda w, a: w.pop("quality.yml"))
        rejected(lambda w, a: a["runs"]["steps"].pop(0))
        rejected(lambda w, a: w["quality.yml"]["jobs"]["native-quality"].update({"steps": []}))
        for mode in ("on", "off"):
            rejected(lambda w, a, mode=mode: [s.update({"run": "echo removed"})
                     for s in w["build-test.yml"]["jobs"]["build-test"]["steps"]
                     if f"--adapters {mode}" in s.get("run", "")])
        if cases != 10:
            raise ValueError(f"Expected 10 counterexamples, ran {cases}")
    print(f"CI workflow policy passed: {len(paths)} workflows, {checks} checks, {cases} counterexamples.")


if __name__ == "__main__":
    main()
