#!/usr/bin/env python3
"""Run the existing AMD-model comparator with an explicit local vendor ZIP.

Only its installation-path literal is replaced in an auxiliary staged copy.
Production source and numerical comparison code remain unchanged.
"""
import argparse
import os
from pathlib import Path
import subprocess
import sys


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('analysis_source', type=Path, help='Canonical staged source/analysis_v6 created by run_xsim.ps1')
    parser.add_argument('rtl_capture_directory', type=Path, help='sim_project/sim_v6.sim/sim_1/behav/xsim')
    parser.add_argument('vector_directory', type=Path, help='sim directory containing vectors_0.npz etc.')
    parser.add_argument('output_directory', type=Path)
    parser.add_argument('--cmodel-zip', type=Path, default=os.environ.get('VIVADO_FFT_CMODEL_ZIP'))
    parser.add_argument('--profiles', type=int, default=4)
    parser.add_argument('--prefix', default='fft_')
    args = parser.parse_args()
    if os.name != 'nt':
        parser.error('The existing comparator requires the Windows AMD model DLL and Python runtime.')
    if not args.cmodel_zip or not args.cmodel_zip.is_file():
        parser.error('Provide an installed vendor C-model ZIP via --cmodel-zip or VIVADO_FFT_CMODEL_ZIP.')
    source = args.analysis_source.resolve()
    repository = Path(__file__).resolve().parents[1]
    if source == repository or repository in source.parents:
        parser.error('Use the canonical staged analysis directory outside the public repository; original source is preserved.')
    for required in ('rtl/detection_accel.sv', 'data/fir_all.mem', 'data/hann_8192.mem'):
        if not (source / required).is_file():
            parser.error('Canonical staged analysis source is incomplete: ' + required)
    original = source / 'tests/compare_fft_model.py'
    code = original.read_text(encoding='utf-8-sig')
    literal = "'D:/vivado/2025.2/Vivado/data/ip/xilinx/xfft_v9_1/cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip'"
    if code.count(literal) != 1:
        parser.error('Comparator installation-path binding changed; review the source before running.')
    portable = source / 'tests/compare_fft_model_portable.py'
    if portable.exists():
        parser.error('Auxiliary comparator already exists; use a fresh staged project.')
    portable.write_text(code.replace(literal, "os.environ['VIVADO_FFT_CMODEL_ZIP']"), encoding='utf-8')
    environment = os.environ.copy()
    environment['VIVADO_FFT_CMODEL_ZIP'] = str(args.cmodel_zip.resolve())
    result = subprocess.run([
        sys.executable, str(portable), str(args.rtl_capture_directory.resolve()),
        str(args.vector_directory.resolve()), str(args.output_directory.resolve()),
        str(args.profiles), args.prefix,
    ], env=environment)
    return result.returncode


if __name__ == '__main__':
    raise SystemExit(main())
