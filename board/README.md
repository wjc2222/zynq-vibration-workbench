# Board deployment boundary and numerical runner

`deployment_observations.json` summarizes archived 2026-11-03 observations. `numeric_acceptance.json` preserves the existing synthetic multisine comparison: 42/42 required channel-band comparisons passed under the recorded engineering tolerances. Neither file is a new board pass. The input is synthetic, not measured mechanical vibration; no fault accuracy or training acceptance follows.

This repository intentionally distributes source, not BOOT.bin, rootfs, bitstreams, kernel modules or prebuilt applications. Build the board artifacts from the source and the toolchain/vendor components you are entitled to use. Resolve the exact target board model, SD partition layout, boot method, device tree/address map and rollback backup before deployment. An old card layout or unit identity is not a reusable target profile. No generic flash command is supplied.

On a target whose matching V7 image and accelerator driver have already been installed and reviewed, copy the public synthetic fixture and your source-built CLI onto the board. Stop acquisition/UI users of the accelerator before the comparison. The CLI implementation is `../src/workspace/zynq_vibration_workbench_v7/software/accelerator/src/cli.cpp`; its hardware request uses the deployed driver and PL.

```sh
sh run_synthetic_numeric.sh ax7z020-v7-ps-axil-50k-reviewed /absolute/path/vibration-accel-cli /absolute/path/synthetic_numeric_multisine_50k_s16le3.bin
```

The profile token is a required explicit operator acknowledgement, not automatic hardware detection. The runner verifies the exact fixture SHA256, then invokes the existing CLI with unchanged tolerances. It writes no boot partition and was not executed against hardware during publication. The CLI may report hardware errors; a failed or missing comparison must not be described as passed. Physical cold boot, manual touch, sensor installation, mechanical data collection and four-class model acceptance remain separate target-specific work.
