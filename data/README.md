# Synthetic engineering fixtures

All included signals are synthetic. There are no raw measured mechanical files, labeled real-world accuracy datasets or trained models here.

`synthetic_numeric_multisine_50k_s16le3.bin` is the exact archived numerical comparison input: 50000 Hz, 82551 frames, three frame-interleaved signed int16 little-endian channels, 495306 bytes, no CAPDDR2/CAPSTRM1 header. SHA256: `02fe2c7c661375e46de4adc766525950116c97bbec1effa0b14ce133527b002a`. It is not a GUI BIN capture container. The proof and host reference preserve original numerical values; see `../board/numeric_acceptance.json` for bounded archived board comparison.

To regenerate, copy `generate_numeric_multisine.py` into a fresh temporary directory and run `python3 generate_numeric_multisine.py` there. It writes the historical uppercase filename beside the copied script; compare its SHA256 with the public lowercase alias. Do not overwrite the archived fixture. The generator does not test hardware.

For a new synthetic CAPDDR2 display/parser fixture (output path must not exist):

```sh
python3 generate_capture_fixture.py /tmp/new_synthetic.bin --seconds 2 --sample-rate 5000 --rpm 1800 --header-ppr 1 --profile demo --seed 20261103
```

The CAPDDR2 generator marks synthetic=true and fault_label=null. Generator commands and numerical fixtures do not validate sensor installation, machine behavior or classification accuracy. The historical suggested command in the original numeric proof is provenance; use the reviewed profile in `../board/` for a new target run.
