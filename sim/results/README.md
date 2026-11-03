# Fresh public-layout validation

These results were generated from the public canonical source staging layout with Vivado 2025.2 on 2026-11-04. They are separate from the 27 archived XSim screenshots.

- Two isolated result-port suites passed 37345 transactions each. Internal RAM/DONE were controlled by the isolation test; this is bus protocol acceptance.
- Symmetric FIR: 28 cases and 688128 exact sample reads. Legacy direct evaluator: four adversarial cases and 98304 exact sample reads. FFT was explicitly stubbed in these isolated suites.
- Full actual vendor-XFFT RTL: all four raw-to-FFT profiles completed, with 98304 three-axis FIR sample checks and 98304 complex FFT codes compared independently against the installed AMD bit-accurate C model. All 12 profile/axis combinations are bit-exact; raw statistics and Hann input are exact.
- The full comparator does not establish power/band-sum equality (`power_and_bands_exact: null`), hardware operation, sensor installation, mechanical fault accuracy or model training.

`full_numeric_verification.json` is copied byte-for-byte from the new comparator output. `fft_captures/` contains actual integer RTL AXIS input/output and statistics tables under synthetic stimuli, not measured mechanical data. Every file is checked for integer-only contents and copied without conversion. Source bindings, exact PASS/cycle observations, raw private-log hashes and original/public capture SHA256 are in `layout_revalidation.json`; `provenance.json` hashes the new public evidence and helper files. Vendor IP/DLL, project binaries, WDB files and logs with private absolute paths are excluded.

`isolated_fir_fixtures.json` describes the independently generated synthetic integer fixtures. It is not a mechanical dataset or model evaluation result. Use the commands in the parent `README.md` to rerun the tests and vendor-model comparison with your installed tools.
