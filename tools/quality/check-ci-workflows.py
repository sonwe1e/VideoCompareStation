"""Validate actual YAML routing and exercise counterexamples without building or opening UI."""

import argparse
import copy
import subprocess
from pathlib import Path

import yaml


def validate(workflows, action, save_action):
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
                "build-test", "native-quality", "quality", "release", "ui-models", "core-windows"
            ):
                require(any(s.get("uses") == "./.github/actions/setup-windows"
                            for s in job.get("steps", [])), f"{name}: missing hosted setup")
                require(any(s.get("uses") == "./.github/actions/save-windows-cache" and
                            s.get("if") == "always()" and s.get("with", {}).get("key") ==
                            "${{ steps.hosted.outputs.vcpkg-cache-key }}" for s in job["steps"]) and
                        any(s.get("id") == "hosted" and s.get("uses") ==
                            "./.github/actions/setup-windows" for s in job["steps"]),
                        f"{name}/{job_name}: preserve completed archives after failures")
            for step in job.get("steps", []):
                script = step.get("run", "")
                if "invoke-low-impact.ps1" in script and "\n" in script.strip():
                    require(script.lstrip().startswith(
                        "$PSNativeCommandUseErrorActionPreference = $true\n"),
                        f"{name}/{job_name}: a later command could hide a failed native gate")
    build = workflows["build-test.yml"]["jobs"]["build-test"]
    require({x["preset"] for x in build["strategy"]["matrix"]["include"]} == {"dev", "release"},
            "Both native build configurations are required")
    full = [s for s in build["steps"] if s.get("name") == "Complete software regression"]
    require(len(full) == 1, "Exactly one complete preset regression step is required")
    command = full[0].get("run", "")
    require(any("check-ci-test-routing.py --preset ${{ matrix.preset }}" in s.get("run", "")
                for s in build["steps"]), "Generated software/hardware inventory must be verified")
    require("--preset ${{ matrix.preset }}" in command and "--no-tests=error" in command,
            "Full regression must use the preset and reject zero tests")
    require(not any(x in command.split() for x in ("-R", "-E", "-L", "-LE", "--exclude-regex")),
            "Complete regression must not filter tests")
    model = workflows["build-test.yml"]["jobs"]["ui-models"]
    core = workflows["build-test.yml"]["jobs"]["core-windows"]
    native_steps = build["steps"] + model["steps"] + core["steps"]
    for preset in ("core-dev", "ui-dev"):
        require(any(f"ctest --preset {preset}" in s.get("run", "") for s in native_steps),
                f"Missing {preset} validation")
    for mode in ("on", "off"):
        require(any("check-ui-test-routing.py" in s.get("run", "") and
                    f"--adapters {mode}" in s["run"] for s in native_steps),
                f"Missing generated source check, adapters={mode}")
    quality = workflows["quality.yml"]["jobs"]["native-quality"]
    for target in ("format-check", "lint"):
        require(any(f"--target {target}" in s.get("run", "") for s in quality["steps"]),
                f"Missing {target} gate")
    release = workflows["release.yml"]
    tag_ref = release["jobs"]["validate-version"]["steps"][0]["with"]["ref"]
    require("refs/tags/" in tag_ref and all(
        release["jobs"][name]["steps"][0]["with"]["ref"] == "${{ needs.validate-version.outputs.commit }}"
        for name in ("quality", "hardware-performance", "release")),
        "Resolve the tag namespace once, then check out the immutable validated SHA")
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
    require(any("subst.exe" in s.get("run", "") and "VCPKG_ROOT=$shortRoot" in s["run"] and
                "$mappedSha -cne $expected" in s["run"] and ".Length -ge 240" in s["run"]
                for s in steps), "Qt dependencies need a verified short build root")
    require(any("vcpkg-archives" in s.get("with", {}).get("path", "") and
                "hashFiles" in s["with"].get("key", "") and "msvc" in s["with"]["key"] and
                "sdk" in s["with"]["key"] and "inputs.cache-scope" in s["with"]["key"]
                for s in steps), "Dependency cache must include compiler and manifest identity")
    require(action.get("outputs", {}).get("vcpkg-cache-key", {}).get("value") ==
            "${{ steps.vcpkg-cache.outputs.cache-primary-key }}" and
            any(s.get("id") == "vcpkg-cache" and s.get("uses", "").startswith(
                "actions/cache/restore@") for s in steps), "Expose the exact restored cache key")
    require(any(s.get("uses", "").startswith("actions/cache/save@") and
                s.get("with", {}).get("key") == "${{ inputs.key }}" and
                s.get("if") == "steps.archives.outputs.present == 'True'"
                for s in save_action["runs"]["steps"]), "Save only completed dependency archives")
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
    save_action = yaml.load((root / ".github/actions/save-windows-cache/action.yml").read_text(
        encoding="utf-8"), Loader=yaml.BaseLoader)
    checks = validate(workflows, action, save_action)
    # Exact count detects removed jobs/checks; new workflows require a deliberate policy update.
    if len(paths) != 4 or checks != 70:
        raise ValueError(f"Expected 4 workflows and 70 checks, got {len(paths)} and {checks}")
    cases = 0
    if args.self_test:
        def rejected(mutate):
            nonlocal cases
            sample, setup = copy.deepcopy(workflows), copy.deepcopy(action)
            mutate(sample, setup)
            try:
                validate(sample, setup, save_action)
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
                     for s in (w["build-test.yml"]["jobs"]["build-test"]["steps"] +
                               w["build-test.yml"]["jobs"]["ui-models"]["steps"])
                     if f"--adapters {mode}" in s.get("run", "")])
        rejected(lambda w, a: w["release.yml"]["jobs"]["validate-version"]["steps"][0]["with"].update(
            {"ref": "${{ inputs.tag || github.ref }}"}))
        rejected(lambda w, a: [s.update({"if": "success()"})
                 for s in w["quality.yml"]["jobs"]["native-quality"]["steps"]
                 if s.get("uses") == "./.github/actions/save-windows-cache"])
        rejected(lambda w, a: [s.update({"run": s["run"].replace(
            "$PSNativeCommandUseErrorActionPreference = $true\n", "")})
            for s in w["build-test.yml"]["jobs"]["ui-models"]["steps"] if "run" in s])
        rejected(lambda w, a: a["outputs"]["vcpkg-cache-key"].update({"value": "stale-key"}))
        rejected(lambda w, a: [s.update({"run": s["run"].replace("VCPKG_ROOT=$shortRoot",
                 "VCPKG_ROOT=$vcpkgRoot")}) for s in a["runs"]["steps"] if "run" in s])
        rejected(lambda w, a: [s.update({"run": "echo removed"})
                 for s in w["build-test.yml"]["jobs"]["build-test"]["steps"]
                 if "check-ci-test-routing.py" in s.get("run", "")])
        if cases != 16:
            raise ValueError(f"Expected 16 counterexamples, ran {cases}")
        # Exercise pwsh native failure propagation without configuring/building the product.
        runs = 0
        for exit_code in (0, 19):
            script = (
                "$ErrorActionPreference = 'Stop'; "
                "$PSNativeCommandUseErrorActionPreference = $true; "
                f"& pwsh -NoProfile -NonInteractive -Command 'exit {exit_code}'; "
                "& pwsh -NoProfile -NonInteractive -Command 'exit 0'"
            )
            result = subprocess.run(["pwsh", "-NoProfile", "-NonInteractive", "-Command", script],
                                    capture_output=True, text=True)
            if (result.returncode == 0) != (exit_code == 0):
                raise ValueError("A later successful command hid a failed native gate")
            runs += 1
        if runs != 2:
            raise ValueError(f"Expected 2 native exit probes, ran {runs}")
    print(f"CI workflow policy passed: {len(paths)} workflows, {checks} checks, "
          f"{cases} counterexamples, {2 if args.self_test else 0} native exit probes.")


if __name__ == "__main__":
    main()
