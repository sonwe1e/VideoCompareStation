"""Verify the real CTest inventory and preserve hardware cases in the manual preset."""

import argparse
import copy
import json
import subprocess


HARDWARE = {
    "platform.GpuTransferActorTests.PublishesDecoderOwnedNv12ArraySlicesWithoutACopy",
    "platform.GpuTransferActorTests.HardwareOnlySetPublishesWithoutAnImmediateContextFlush",
    "platform.GpuTransferActorTests.ProbePixelReadsNv12ArraySliceVisibleRegionCodeValues",
    "platform.GpuTransferActorTests.ProbePixelReadsP010ArraySliceTenBitCodeValues",
    "media.SoftwareDecoderHardwareTests.RetainsNv12AndP010D3d11VaSurfacesOnTheSharedDevice",
}

# Pinned on the last cloud enumeration. Any commit that adds, removes or re-routes a test must
# re-enumerate and update both numbers here *and* the self-test's construction below: the two
# literals are deliberately independent so a one-sided edit fails the self-test instead of
# turning it into a tautology.
#
# 1465/1443 comes from PR #53 (merge of 66475bb onto the workspace work), +56 over the 1409/1387
# of 4f2e0dd: 55 new gtest cases plus the new quality.qmltypes_contract entry. The inventory
# carries 11 hardware-labelled tests; only the five named below are asserted by identity.
EXPECTED_TOTAL = 1465
EXPECTED_SOFTWARE = 1443


def validate(inventory, software, hardware):
    names = {t["name"] for t in inventory}
    excluded = {"hardware", "performance", "packaged", "soak"}
    expected_software = {t["name"] for t in inventory if not excluded.intersection(t["labels"])}
    expected_hardware = {t["name"] for t in inventory if "hardware" in t["labels"]}
    checks = (
        len(HARDWARE) == 5,
        len(inventory) == len(names) == EXPECTED_TOTAL,
        len(software) == len(set(software)) == EXPECTED_SOFTWARE,
        set(software) == expected_software,
        set(hardware) == expected_hardware,
        HARDWARE <= names,
        HARDWARE <= set(hardware),
        HARDWARE.isdisjoint(software),
    )
    if len(checks) != 8 or not all(checks):
        # Report the observed inventory: the numbers are re-pinned from a real enumeration,
        # and a bare tuple of booleans does not say what the merged tree actually registered.
        raise ValueError(
            f"CTest routing failed: {checks}; observed total={len(inventory)} "
            f"software={len(software)} hardware={len(hardware)}; pinned "
            f"total={EXPECTED_TOTAL} software={EXPECTED_SOFTWARE}"
        )
    return len(checks)


def read_tests(*arguments):
    result = subprocess.run(["ctest", *arguments, "--show-only=json-v1"],
                            check=True, capture_output=True, text=True, encoding="utf-8")
    tests = json.loads(result.stdout)["tests"]
    return [{"name": t["name"], "labels": next(
        (p["value"] for p in t.get("properties", []) if p["name"] == "LABELS"), [])}
        for t in tests]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--preset", choices=("dev", "release"))
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        # Independent of EXPECTED_SOFTWARE on purpose: a one-sided pin fails here.
        software = [f"software.{i}" for i in range(1443)]
        hardware = sorted(HARDWARE)
        inventory = ([{"name": n, "labels": []} for n in software] +
                     [{"name": n, "labels": ["hardware"]} for n in hardware] +
                     [{"name": f"packaged.{i}", "labels": ["packaged"]} for i in range(17)])
        validate(inventory, software, hardware)
        cases = 0
        for kind in range(4):
            items, soft, hard = copy.deepcopy(inventory), software.copy(), hardware.copy()
            if kind == 0:
                items = [t for t in items if t["name"] != hardware[0]]
            elif kind == 1:
                soft[-1] = hardware[0]
            elif kind == 2:
                hard.pop()
            else:
                soft.pop()
            try:
                validate(items, soft, hard)
            except ValueError:
                cases += 1
                continue
            raise ValueError("A missing or misrouted test was accepted")
        if cases != 4:
            raise ValueError(f"Expected four counterexamples, got {cases}")
        print("CTest routing self-test passed: 8 checks, 4 counterexamples.")
        return
    if args.preset is None:
        parser.error("--preset or --self-test is required")
    inventory = read_tests("--test-dir", f"out/build/{args.preset}")
    software = [t["name"] for t in read_tests("--preset", args.preset)]
    hardware = [t["name"] for t in read_tests("--preset", "hardware-d3d11", "--test-dir",
                                             f"out/build/{args.preset}")]
    count = validate(inventory, software, hardware)
    print(f"CTest routing passed: {count} checks, {EXPECTED_TOTAL} total, "
          f"{EXPECTED_SOFTWARE} software; "
          "five decoder cases preserved in the manual hardware preset.")


if __name__ == "__main__":
    main()
