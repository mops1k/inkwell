#!/usr/bin/env python3
"""Generate firmware-manifest.json next to the release assets and verify it.

The format mirrors what upstream Paperio Reader publishes, so an Android client
or a release check can read either project's releases the same way:

    {
      "version": "1.1.0",
      "source_commit": "<sha>",
      "file": "inkwell-1.1.0.bin",
      "bytes": 5042399,
      "sha256": "<64 hex>",
      "devices": ["X3", "X4"],
      "hardware_tested": false,
      "applications": [
        {"file": "firmware-x3.bin", "bytes": ..., "sha256": "...",
         "devices": ["X3"], "hardware_tested": false}
      ]
    }

Board mapping: the Inkwell image serves X3 and X4 from one binary (both FreeInk
profiles are compiled in and the board is detected at runtime), so
`inkwell-<version>.bin` and the upstream-compatible `firmware.bin` alias carry
every supported device, while `firmware-x3.bin` / `firmware-x4.bin` are the same
bytes labelled for one model each.

The script fails closed: unknown `.bin` assets, a missing SHA256SUMS entry, or a
manifest hash that does not match the file on disk abort the release build
instead of publishing something the device cannot verify.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import sys

MANIFEST_NAME = 'firmware-manifest.json'
SUMS_NAME = 'SHA256SUMS'

# Asset name -> devices the bytes are built for. Anything else is a mistake in
# the release pipeline and must not be published silently.
ASSET_DEVICES: dict[str, list[str]] = {
    'firmware.bin': [],  # filled with every supported device
    'firmware-x3.bin': ['X3'],
    'firmware-x4.bin': ['X4'],
}

# Published for completeness/debugging, not installable firmware.
IGNORED_ASSETS = {'bootloader.bin', 'partitions.bin'}


def sha256_of(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open('rb') as handle:
        for chunk in iter(lambda: handle.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def is_hex64(text: str) -> bool:
    return len(text) == 64 and all(c in '0123456789abcdefABCDEF' for c in text)


def read_sums(path: pathlib.Path) -> dict[str, str]:
    sums: dict[str, str] = {}
    for raw in path.read_text(encoding='utf-8').splitlines():
        line = raw.strip()
        if not line:
            continue
        parts = line.split(None, 1)
        if len(parts) != 2:
            raise SystemExit(f'{path}: malformed line {raw!r}')
        digest, name = parts[0].strip().lower(), parts[1].strip()
        if name.startswith('./'):
            name = name[2:]
        if not is_hex64(digest):
            raise SystemExit(f'{path}: {name} has a malformed SHA-256')
        sums[name] = digest
    return sums


def build_manifest(dist: pathlib.Path, version: str, source_commit: str, devices: list[str],
                   hardware_tested: bool, primary_name: str) -> dict:
    primary = dist / primary_name
    if not primary.is_file():
        raise SystemExit(f'primary image {primary} is missing')

    names = []
    for path in sorted(dist.glob('*.bin')):
        if path.name in IGNORED_ASSETS:
            continue
        names.append(path.name)
    if primary_name not in names:
        raise SystemExit(f'primary image {primary_name} is missing from {dist}')
    # The device takes the first application whose devices match and requires the
    # asset it downloads to be named there, so the primary image must come first:
    # the model-named aliases (firmware-x3.bin, ...) would otherwise win and the
    # name check in OtaUpdater would reject an otherwise valid release.
    names.remove(primary_name)
    names.insert(0, primary_name)

    applications = []
    for name in names:
        path = dist / name
        if name == primary_name:
            asset_devices = list(devices)
        elif name in ASSET_DEVICES:
            asset_devices = list(ASSET_DEVICES[name]) or list(devices)
        else:
            raise SystemExit(f'unexpected release asset {name}: add it to ASSET_DEVICES or drop it')
        applications.append({
            'file': name,
            'bytes': path.stat().st_size,
            'sha256': sha256_of(path),
            'devices': asset_devices,
            'hardware_tested': hardware_tested,
        })

    if not applications:
        raise SystemExit(f'no .bin assets in {dist}')

    manifest = {
        'version': version,
        'source_commit': source_commit,
        'file': primary_name,
        'bytes': primary.stat().st_size,
        'sha256': sha256_of(primary),
        'devices': list(devices),
        'hardware_tested': hardware_tested,
        'applications': applications,
    }
    return manifest


def verify(dist: pathlib.Path, manifest: dict) -> None:
    sums_path = dist / SUMS_NAME
    if not sums_path.is_file():
        raise SystemExit(f'{sums_path} is missing: run sha256sum before the manifest')
    sums = read_sums(sums_path)

    listed = {app['file'] for app in manifest['applications']}
    bins = {path.name for path in dist.glob('*.bin')} - IGNORED_ASSETS
    if listed != bins:
        raise SystemExit(f'{SUMS_NAME} / manifest mismatch: manifest={sorted(listed)} files={sorted(bins)}')

    for app in manifest['applications']:
        path = dist / app['file']
        actual = sha256_of(path)
        if app['sha256'].lower() != actual:
            raise SystemExit(f'{app["file"]}: manifest sha256 {app["sha256"]} != file {actual}')
        if app['bytes'] != path.stat().st_size:
            raise SystemExit(f'{app["file"]}: manifest size {app["bytes"]} != file {path.stat().st_size}')
        if sums.get(app['file']) != actual:
            raise SystemExit(f'{app["file"]}: {SUMS_NAME} does not list the file sha256')

    primary = dist / manifest['file']
    if manifest['sha256'].lower() != sha256_of(primary):
        raise SystemExit(f'primary {manifest["file"]} sha256 does not match the file')

    print(f'manifest verified: {len(manifest["applications"])} application(s), '
          f'devices={",".join(manifest["devices"])}')


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--dist', required=True, type=pathlib.Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--source-commit', required=True)
    parser.add_argument('--devices', nargs='+', default=['X3', 'X4'])
    parser.add_argument('--hardware-tested', default='false', choices=['true', 'false'])
    parser.add_argument('--primary', default=None, help='primary asset name, default inkwell-<version>.bin')
    args = parser.parse_args(argv)

    dist: pathlib.Path = args.dist
    if not dist.is_dir():
        raise SystemExit(f'{dist} is not a directory')

    primary_name = args.primary or f'inkwell-{args.version}.bin'
    manifest = build_manifest(dist, args.version, args.source_commit, list(args.devices),
                              args.hardware_tested == 'true', primary_name)
    verify(dist, manifest)

    out = dist / MANIFEST_NAME
    out.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + '\n', encoding='utf-8')
    print(f'wrote {out}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
