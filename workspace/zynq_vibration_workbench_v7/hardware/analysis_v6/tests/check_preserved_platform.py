"""Verify that V7 capture/display platform sources retain protected V6 bytes."""
import argparse
import hashlib
import json
from pathlib import Path


def sha256(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("protected_analysis", type=Path)
    parser.add_argument("build", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    source = Path(__file__).resolve().parents[1] / "platform_source"
    protected = args.protected_analysis / "platform_source"
    relpaths = sorted(p.relative_to(protected) for p in protected.rglob("*") if p.is_file())
    current = sorted(p.relative_to(source) for p in source.rglob("*") if p.is_file())
    assert current == relpaths, "Platform source file set changed"
    manifest = []
    for rel in relpaths:
        expected = sha256(protected / rel)
        assert sha256(source / rel) == expected, (rel, "Protected platform source differs")
        # Only the generated BD-creation script adds the independent accelerator.
        staged = args.build / "source" / rel
        staged_unchanged = rel.as_posix() != "combined/scripts/create_combined.tcl"
        if staged_unchanged:
            assert sha256(staged) == expected, (rel, "Staged capture/display source differs")
        manifest.append(dict(file=rel.as_posix(), sha256=expected, staged_unchanged=staged_unchanged))
    report = dict(status="passed", scope="byte identity of capture/display platform source and staged implementation inputs",
                  protected_v6_files=len(manifest), modified_capture_or_display_files=0,
                  integration_exception="Generated create_combined.tcl adds an independent AXI-Lite analysis module at 0x43c20000; source baseline script is unchanged",
                  files=manifest)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n")
    print(f"PRESERVED_PLATFORM_PASS files={len(manifest)} capture_display_changes=0")


if __name__ == "__main__":
    main()
