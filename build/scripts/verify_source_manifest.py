"""Verify the frozen candidate; .git metadata is outside the release manifest."""
from pathlib import Path, PurePosixPath
import hashlib, json, sys
root = Path(__file__).resolve().parents[2]
try:
    manifest = json.loads((root / 'source_manifest.json').read_text('utf-8'))
    rows = manifest['files']; expected = set()
    for row in rows:
        rel = row['path']; posix = PurePosixPath(rel)
        if not isinstance(rel, str) or posix.is_absolute() or '..' in posix.parts or '\\' in rel or ':' in rel or rel in expected:
            raise ValueError('Invalid or duplicate manifest path')
        expected.add(rel)
        p = root / rel
        if not p.is_file() or p.is_symlink() or not p.resolve().is_relative_to(root):
            raise ValueError('Missing or unsafe member: ' + rel)
        h = hashlib.sha256()
        with p.open('rb') as f:
            for chunk in iter(lambda: f.read(1024 * 1024), b''): h.update(chunk)
        if p.stat().st_size != row['bytes'] or h.hexdigest() != row['sha256']:
            raise ValueError('Member changed: ' + rel)
    actual = {p.relative_to(root).as_posix() for p in root.rglob('*')
              if p.is_file() and '.git' not in p.relative_to(root).parts
              and p.relative_to(root).as_posix() != 'source_manifest.json'}
    if expected != actual: raise ValueError('Missing or extra release files')
    print('PASS', len(expected), 'source files')
except (OSError, ValueError, KeyError, TypeError) as e:
    print('FAIL:', e, file=sys.stderr); sys.exit(1)
