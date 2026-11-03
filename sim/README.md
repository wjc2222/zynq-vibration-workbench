# Genuine archived XSim screenshots

`xsim/` contains 27 byte-identical existing lossless pixel crops from genuine Vivado 2025.2 XSim windows. No waveform was redrawn, resized or synthetically replaced. The mapping retains each original image hash, crop hash/box, signals, saved zoom and version scope. Full-window originals, native WCFG and large WDB files are excluded from this public subset because they contain private workstation context or are not portable without the wave databases. This folder cannot independently reopen the archived WDB.

These are RTL/IP and behavioral-DDR simulations, not physical ILA, sensor recordings or new hardware tests. Screenshots 21–27 are explicitly V6 history from 2026-10-31; they are not proof of V7 symmetric FIR optimization. Latest V7 numerical and routed implementation evidence is in `../build/reports/`.

Existing scripts, tests and coefficient data are under `../src/workspace/zynq_vibration_workbench_v7/hardware/analysis_v6/`. The effective V7 RTL and paired coefficient data are under `../src/fpga-effective/source/analysis_v6/`. The `analysis_v6` directory name is retained by V7. `stage_analysis.ps1` assembles these exact source files into a fresh ASCII-only work directory before either launcher runs; the source trees are preserved. Use a licensed matching Vivado/IP installation for the full simulation, which requires generated FFT XCI and test vectors. Fresh isolated bus/FIR validation is reported separately under `results/` and does not recreate the 27 historical screenshots.

Run `python3 validate_public_evidence.py` to verify this public evidence subset, its exact image hashes, fixture bytes and preserved numerical acceptance. This is a publication integrity check, not a rerun of RTL or hardware.

For a new actual XSim run on Windows with PowerShell 7, Python/NumPy and licensed Vivado 2025.2 available:

```powershell
./run_xsim.ps1 -WorkDirectory (Join-Path $env:TEMP ('vibration-xsim-' + [guid]::NewGuid())) -Vivado D:/vivado/2025.2/Vivado/bin/vivado.bat -Python python
```

The wrapper creates fresh vectors, generates the FFT IP from source and invokes the existing full raw-to-FFT testbench. Its Tcl checks all four profiles and 32768 three-axis sample positions; process exit alone is not the check. It does not reproduce the 27 historical screenshots. Current fresh-run status and its separate golden comparison are recorded under `results/`; launching the wrapper alone does not establish numerical acceptance. The separate existing `compare_fft_model.py` is Windows-specific and refers to a fixed vendor installation path. `run_fft_compare.py` accepts `--cmodel-zip` or `VIVADO_FFT_CMODEL_ZIP` and changes only that installation-path binding in an auxiliary staged copy. The vendor C-model DLL and ZIP are not redistributed. Neither wrapper claims completion of that separate full FFT numerical comparison unless it actually runs successfully.

After a successful full XSim run, the separate vendor-model comparison can be run with:

```powershell
python ./run_fft_compare.py E:/your-xsim-work/source/analysis_v6 E:/your-xsim-work/sim_project/sim_v6.sim/sim_1/behav/xsim E:/your-xsim-work/sim E:/your-xsim-work/comparison --cmodel-zip D:/vivado/2025.2/Vivado/data/ip/xilinx/xfft_v9_1/cmodel/xfft_v9_1_bitacc_cmodel_nt64.zip
```

Replace `E:/your-xsim-work` with the fresh work directory selected for the RTL run, and adjust the vendor installation path if needed. This auxiliary helper requires Windows and the installed AMD model dependencies. It preserves the existing comparison algorithm, coefficients, thresholds and original source. Its current observed acceptance is recorded under `results/`.

For isolated tests with the FFT explicitly stubbed:

```powershell
./run_isolated.ps1 -WorkDirectory (Join-Path $env:TEMP ('vibration-isolated-' + [guid]::NewGuid())) -VivadoBin D:/vivado/2025.2/Vivado/bin -Suite All
```

`-Suite ResultPort` checks the 64-bit immutable result protocol and V7 low-word protocol (37345 transactions each); internal result RAM is preloaded and DONE is forced, so this does not verify FIR or FFT arithmetic. `-Suite FIR` generates independent integer fixtures and checks 28 symmetric-FIR cases plus four direct-evaluator adversarial cases. FFT is stubbed, and the final state is forced solely to read FIR results/statistics. `All` executes both groups. Each simulator result is accepted only if the expected full completion marker is present. The wrappers never access physical hardware.
