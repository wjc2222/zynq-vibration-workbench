#!/usr/bin/env python3
"""Prepare the separate plugin with hash-verified Qt 5.15.18 integration sources.

Only reads qtbase. Writes generated sources in a new private output directory;
never edits qtbase or its installed original linuxfb plugin.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil

PINNED = {
    "qlinuxfbintegration.cpp": "b8b907cda11aa24832b1038a2cad061b5db2ea2af78cf05c1be4a3b65e8145dc",
    "qlinuxfbintegration.h": "29aa2d394f4d0099adf7d12143adc85703e6812e1c70f6f2612f45884418209a",
    "main.cpp": "171c3e4acb293f3f41c4ecad1dc6f3d34d7cde86e37bf8855060502a6403bf79",
}

def main():
    p = argparse.ArgumentParser()
    p.add_argument("qtbase", type=Path)
    p.add_argument("destination", type=Path)
    p.add_argument("--refresh", action="store_true",
                   help="refresh only a previously prepared ax7zfb source directory")
    args = p.parse_args()
    source = Path(__file__).resolve().parent
    upstream = args.qtbase.resolve() / "src/plugins/platforms/linuxfb"
    destination = args.destination.resolve()
    if destination.exists():
        marker = destination / "source-manifest.json"
        if not args.refresh or not marker.is_file() or json.loads(marker.read_text()).get("plugin") != "ax7zfb":
            raise SystemExit(f"refusing unrecognized/existing preparation directory: {destination}")
    verified = {}
    for name, expected in PINNED.items():
        content = (upstream / name).read_bytes()
        actual = hashlib.sha256(content).hexdigest()
        if actual != expected:
            raise SystemExit(f"unreviewed Qt source {name}: {actual} != {expected}")
        verified[name] = content.decode("utf-8")
    destination.mkdir(parents=True, exist_ok=args.refresh)
    for item in source.iterdir():
        if item.is_file() and item.suffix in (".cpp", ".h", ".pro", ".json"):
            shutil.copyfile(item, destination / item.name)
    (destination / "tests").mkdir(exist_ok=args.refresh)
    for item in (source / "tests").iterdir():
        if item.is_file() and item.suffix in (".cpp", ".pro"):
            shutil.copyfile(item, destination / "tests" / item.name)
    generated = {}
    for name, text in verified.items():
        text = text.replace("QLinuxFbIntegration", "QAx7zFbIntegration")
        text = text.replace("QLINUXFBINTEGRATION", "QAX7ZFBINTEGRATION")
        text = text.replace('"qlinuxfbintegration.h"', '"qax7zfbintegration.h"')
        text = text.replace('"qlinuxfbscreen.h"', '"qax7zfbscreen.h"')
        text = text.replace("new QLinuxFbScreen(paramList)", "new QAx7zFbScreen(paramList)")
        text = text.replace('FILE "linuxfb.json"', 'FILE "ax7zfb.json"')
        text = text.replace('QLatin1String("linuxfb")', 'QLatin1String("ax7zfb")')
        text = text.replace('qWarning("linuxfb: Failed to initialize screen")',
                            'qFatal("ax7zfb: explicit backend initialization failed; refusing fallback")')
        text = text.replace('#if QT_CONFIG(kms)\n#include "qlinuxfbdrmscreen.h"\n#endif\n', '')
        text = text.replace('''#if QT_CONFIG(kms)
    if (qEnvironmentVariableIntValue("QT_QPA_FB_DRM") != 0)
        m_primaryScreen = new QLinuxFbDrmScreen(paramList);
#endif
    if (!m_primaryScreen)
        m_primaryScreen = new QAx7zFbScreen(paramList);''',
                            '    m_primaryScreen = new QAx7zFbScreen(paramList);')
        output = name.replace("qlinuxfbintegration", "qax7zfbintegration")
        data = text.encode("utf-8")
        (destination / output).write_bytes(data)
        generated[output] = hashlib.sha256(data).hexdigest()
    for license_name in ("LICENSE.LGPL3", "LICENSE.GPL2", "LICENSE.GPL3", "LGPL_EXCEPTION.txt"):
        license_path = args.qtbase / license_name
        if license_path.is_file():
            shutil.copyfile(license_path, destination / license_name)
    own_files = [p for p in source.iterdir() if p.is_file() and
                 (p.suffix in (".cpp", ".h", ".pro", ".json", ".py", ".md"))]
    own_files += [p for p in (source / "tests").iterdir() if p.is_file() and p.suffix in (".cpp", ".pro")]
    manifest = {"plugin": "ax7zfb", "qt_version": "5.15.18", "upstream_linuxfb_sha256": PINNED,
                "generated_sha256": generated,
                "changes": ["rename QPA plugin and integration to ax7zfb",
                            "use only QAx7zFbScreen; remove DRM selection",
                            "explicit initialization failure is fatal"],
                "own_source_sha256": {
                    str(p.relative_to(source)): hashlib.sha256(p.read_bytes()).hexdigest()
                    for p in sorted(own_files)}}
    (destination / "source-manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

if __name__ == "__main__":
    main()
