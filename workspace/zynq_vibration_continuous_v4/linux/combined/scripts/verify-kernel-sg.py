#!/usr/bin/env python3
"""Verify the new SG kernel candidate; no simple-profile verdict is imported."""
import hashlib
import json
import pathlib
import subprocess
import sys

out = pathlib.Path(sys.argv[1]).resolve()
project = pathlib.Path(__file__).resolve().parents[3]
kernel = out / "build/linux-custom"
images = out / "images"
lock_path = kernel / "ax7z020-combined-sg-source-lock.json"
lock = json.loads(lock_path.read_text())
for relative, digest in lock["source_sha256"].items():
    if hashlib.sha256((project / relative).read_bytes()).hexdigest() != digest:
        raise RuntimeError(f"SG source drift after build: {relative}")
for name, relative in {"ax7z020_capture_sg.c": "hardware/combined/linux/ax7z020_capture_sg.c",
    "ax7z020_capture.h": "hardware/combined/include/ax7z020_capture.h",
    "ax7z020_capture_preview.h": "hardware/combined/include/ax7z020_capture_preview.h",
    "ax7z020_capture_stream.h": "hardware/combined/include/ax7z020_capture_stream.h"}.items():
    if hashlib.sha256((kernel / "drivers/misc/ax7z020_capture_sg" / name).read_bytes()).hexdigest() != lock["source_sha256"][relative]:
        raise RuntimeError(f"Compiled SG source copy mismatch: {name}")
config = dict(line.split("=", 1) for line in (kernel / ".config").read_text().splitlines() if line.startswith("CONFIG_") and "=" in line)
required = {"CONFIG_AX7Z020_CAPTURE_SG": "y", "CONFIG_CRC32": "y", "CONFIG_FB_CODEX_AX7Z020": "y",
    "CONFIG_SMP": "y", "CONFIG_NR_CPUS": "2", "CONFIG_CMA_SIZE_MBYTES": "128"}
if any(config.get(k) != v for k, v in required.items()) or config.get("CONFIG_AX7Z020_CAPTURE") == "y" or config.get("CONFIG_DEVMEM") == "y":
    raise RuntimeError("SG kernel config does not match the selected exclusive driver/profile")
dtb = images / "zynq-ax7z020-combined-sg.dtb"
fdtget = out.parent / "output-display-udev/host/bin/fdtget"
def get(node, prop, typ="x"):
    return subprocess.check_output([str(fdtget), "-t", typ, str(dtb), node, prop], text=True).strip()
checks = {("/memory@0", "reg"): "0 40000000",
    ("/axi/capture@43c10000", "reg"): "43c10000 10000 40400000 10000",
    ("/axi/capture@43c10000", "interrupts"): "0 1e 4",
    ("/axi/capture@43c10000", "codex,sg-length-width"): "1a",
    ("/axi/capture@43c10000", "codex,dma-address-width"): "20",
    ("/axi/capture@43c10000", "codex,sg-control-status-stream"): "0",
    ("/axi/capture@43c10000", "codex,preview-period-us"): "9c40",
    ("/capture-clock", "clock-frequency"): "2faf080",
    ("/axi/framebuffer@43000000", "reg"): "43000000 10000 43c00000 10000 41200000 10000",
    ("/axi/framebuffer@43000000", "interrupts"): "0 1d 4"}
for (node, name), expected in checks.items():
    if get(node, name) != expected:
        raise RuntimeError(f"SG DT mismatch: {node} {name}")
if get("/axi/capture@43c10000", "compatible", "s") != "codex,ax7z020-capture-sg1":
    raise RuntimeError("Wrong SG compatible")
if get("/axi/capture@43c10000", "clocks") != get("/capture-clock", "phandle"):
    raise RuntimeError("Wrong physical capture clock")
if b"SG preview driver refuses simple DMA hardware" not in (kernel / "vmlinux").read_bytes():
    raise RuntimeError("SG-only runtime guard absent from linked kernel")
maximum = 2097024
worst_bds = (maximum + 199) // 200
maximum_bd_bytes = worst_bds * 64
report = {"schema": "ax7z020-sg-preview-kernel-static-verification-v1", "status": "cross_compiled_static_checked_only",
    "source_lock_sha256": hashlib.sha256(lock_path.read_bytes()).hexdigest(),
    "driver_sha256": lock["source_sha256"]["hardware/combined/linux/ax7z020_capture_sg.c"],
    "preview_uapi_sha256": lock["source_sha256"]["hardware/combined/include/ax7z020_capture_preview.h"],
    "stream_uapi_sha256": lock["source_sha256"]["hardware/combined/include/ax7z020_capture_stream.h"],
    "kernel_version": (kernel / "include/config/kernel.release").read_text().strip(), "config": required,
    "dt": [{"node": n, "property": p, "hex": v} for (n, p), v in checks.items()],
    "sg_contract": {"all_bds_preposted": True, "cyclic_mode": False, "bd_alignment": 64, "address_bits": 32,
        "length_bits": 26, "period_us": 40000, "monitor_ms": 5,
        "completion_authority": "contiguous BD.Completed prefix, exact transferred bytes, SOF/EOF and record/index/tick/flag validation",
        "no_software_rearm_between_blocks_finite": True, "preview_ioctls_bytes": {"info": 64, "copy": 8272}},
    "stream_contract": {"additive_abi": 1, "ring_chunks": 256, "maximum_ring_bytes": 16384000,
        "period_us": 40000, "cyclic_mode": False, "ownership": "COMPLETE -> validate -> copy_to_user success -> clear completion -> dma_wmb -> advance TAIL",
        "normal_stop": "next complete 40ms packet boundary; drain final BD, DMA quiesce without producer ABORT",
        "status_bytes": 160, "start_bytes": 48, "caps_bytes": 64},
    "memory": {"ddr_bytes": 1073741824, "cma_bytes": 134217728, "maximum_payload_bytes": maximum * 32,
        "worst_descriptor_count_at_5khz": worst_bds, "worst_descriptor_bytes": maximum_bd_bytes,
        "display_double_buffer_bytes": 7372800,
        "minimum_cma_margin_bytes": 134217728 - maximum * 32 - maximum_bd_bytes - 7372800},
    "files": {name: {"bytes": (images / name).stat().st_size, "sha256": hashlib.sha256((images / name).read_bytes()).hexdigest()}
              for name in ("zImage", "zynq-ax7z020-combined-sg.dtb")},
    "unverified": ["SG-enabled XSA and physical probe", "BD writeback ordering and preview under real DDR contention",
        "preview rate/latency and GUI frame rate", "cancel/close/restart and maximum job on SG hardware",
        "real sensor measurements, calibration, physical HDMI/touch and independent SD boot"]}
(images / "kernel-sg-verification.json").write_text(json.dumps(report, indent=2) + "\n")
print(json.dumps(report, indent=2))
