#!/usr/bin/env python3
"""Inject only the new SG driver into a separate output-combined-sg-* kernel."""
import hashlib
import json
import pathlib
import shutil
import sys

project = pathlib.Path(__file__).resolve().parents[3]
kernel = pathlib.Path(sys.argv[1]).resolve()
if "output-combined-sg-" not in str(kernel):
    raise SystemExit("SG injection requires an output-combined-sg-* kernel")
source = project / "hardware/combined/linux"
destination = kernel / "drivers/misc/ax7z020_capture_sg"
destination.mkdir(parents=True, exist_ok=True)
inputs = [source / "ax7z020_capture_sg.c", source / "sg/Kconfig", source / "sg/Makefile"]
inputs += [project / "hardware/combined/include" / name for name in ("ax7z020_capture.h", "ax7z020_capture_preview.h", "ax7z020_capture_stream.h")]
for path in inputs:
    shutil.copyfile(path, destination / path.name)
for path, line in [(kernel / "drivers/misc/Kconfig", 'source "drivers/misc/ax7z020_capture_sg/Kconfig"'),
                   (kernel / "drivers/misc/Makefile", 'obj-$(CONFIG_AX7Z020_CAPTURE_SG) += ax7z020_capture_sg/')]:
    text = path.read_text()
    if line not in text.splitlines():
        path.write_text(text + "\n" + line + "\n")
dts = kernel / "arch/arm/boot/dts"
shutil.copyfile(source / "capture-sg.dtsi", dts / "ax7z020-capture-sg.dtsi")
(dts / "zynq-ax7z020-combined-sg.dts").write_text('''// SPDX-License-Identifier: GPL-2.0-or-later
/dts-v1/;
#include "xilinx/zynq-7000.dtsi"
#include "ax7z020-common.dtsi"
#include "ax7z020-display.dtsi"
#include "ax7z020-capture-sg.dtsi"
/ { model = "ALINX AX7Z020 MRG1 SG DMA-committed preview"; };
''')
board = project / "linux/br2-external/board/ax7z020/dts"
for name in ("ax7z020-common.dtsi", "ax7z020-display.dtsi"):
    shutil.copyfile(board / name, dts / name)
    inputs.append(board / name)
inputs += [source / "capture-sg.dtsi", source / "sg_kernel.fragment", pathlib.Path(__file__).resolve()]
lock = {"schema": 1, "purpose": "SG1 DMA-committed preview kernel inputs",
        "source_sha256": {p.relative_to(project).as_posix(): hashlib.sha256(p.read_bytes()).hexdigest() for p in inputs}}
(kernel / "ax7z020-combined-sg-source-lock.json").write_text(json.dumps(lock, indent=2) + "\n")
print(json.dumps(lock, indent=2))
