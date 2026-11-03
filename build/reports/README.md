# Existing implementation and numerical evidence

These are genuine archived Vivado 2025.2 routed/synthesis reports and existing verification JSON, not a new hardware or simulation run. `timing_summary.rpt` and `utilization.rpt` describe the full routed platform; `synthesis/` contains isolated FIR comparisons. The full platform has WNS 0.603 ns and WHS 0.045 ns; it is not a zero-warning build. Read `implementation_manifest.json` for inherited CDC/constraint limitations.

`numeric_verification.json` and `fir_performance_verification.json` preserve existing numerical checks. `performance/` preserves actual bounded board benchmark comparisons and the native fixed-protocol audit, including its `validated: false` and independent-machine limitations. Performance is not mechanical recognition accuracy. No raw mechanical captures or trained models are published.

Only explicit host/command headers and absolute private source paths were redacted. Numerical values are preserved. `public_evidence_provenance.json` records original and public SHA256, including unchanged files. Original full private receipts remain outside this public repository. This directory contains no FPGA bitstream, executable, netlist, BOOT image or root filesystem.

## Fresh public-layout host check

[host_revalidation.json](host_revalidation.json) and its [CTest log](host_revalidation.log) record the new source-only Release build and 67/67 passing tests under Qt5 xcb/Xvfb. Source identities are checked against the staged public files. This host regression does not claim new hardware, physical touch, boot or mechanical-classification acceptance.
