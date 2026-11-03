#!/usr/bin/env python3
"""Read-only public evidence integrity check; does not simulate RTL or access hardware."""
from pathlib import Path
import hashlib, json
root=Path(__file__).resolve().parents[1]
load=lambda p:json.loads((root/p).read_text(encoding="utf-8-sig"))
m=load("sim/screenshot_mapping.json")
assert len(m["images"])==27 and m["analysis21_27_are_V6"] is True
for item in m["images"]:
    assert hashlib.sha256((root/"sim"/item["crop"]).read_bytes()).hexdigest()==item["crop_sha256"],item["name"]
fixture=root/"data/synthetic_numeric_multisine_50k_s16le3.bin"
assert fixture.stat().st_size==495306
assert hashlib.sha256(fixture.read_bytes()).hexdigest()=="02fe2c7c661375e46de4adc766525950116c97bbec1effa0b14ce133527b002a"
a=load("board/numeric_acceptance.json")
assert a["synthetic"] and not a["new_hardware_tests_performed_by_extractor"]
assert a["metrics"]["evaluated_and_passed_channel_band_comparisons"]==42
for item in load("build/reports/public_evidence_provenance.json")["files"]:
    assert hashlib.sha256((root/item["public"]).read_bytes()).hexdigest()==item["public_sha256"],item["public"]
fresh=root/"sim/results/layout_revalidation.json"
if fresh.is_file():
    current=json.loads(fresh.read_text(encoding="utf-8-sig"))
    for item in current["source_files"]:
        assert hashlib.sha256((root/item["path"]).read_bytes()).hexdigest()==item["sha256"],item["path"]
    assert all(item["status"]=="passed" for item in current["isolated_tests"])
    for item in current.get("integer_capture_artifacts",[]):
        assert hashlib.sha256((root/item["public_path"]).read_bytes()).hexdigest()==item["public_sha256"],item["public_path"]
    fresh_provenance=root/"sim/results/provenance.json"
    if fresh_provenance.is_file():
        for item in json.loads(fresh_provenance.read_text(encoding="utf-8"))["files"]:
            assert hashlib.sha256((root/item["path"]).read_bytes()).hexdigest()==item["sha256"],item["path"]
    if current["full_AMD_model_comparison"]["status"]=="passed":
        golden=load("sim/results/full_numeric_verification.json")
        assert golden["status"]=="passed" and golden["FIR_sample_checks"]==98304 and golden["fft_complex_checks"]==98304
        assert len(golden["RTL_vs_installed_AMD_model"])==12 and all(item["bit_exact"] for item in golden["RTL_vs_installed_AMD_model"])
    print("Fresh-layout source bindings and isolated PASS records: verified. Full RTL/model status:",current["full_raw_to_fft"]["status"],current["full_AMD_model_comparison"]["status"])
print("PASS: 27 historical XSim crops, exact synthetic fixture, archived numerical acceptance and public provenance hashes. This integrity checker does not run RTL or access hardware.")
