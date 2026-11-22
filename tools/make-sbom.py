"""Writes the SPDX 2.3 SBOM of the release DLL.

The DLL links its libraries statically, so a scanner finds nothing in it. This reads what the build
used instead: the SPDX file vcpkg writes for each installed port, and the commits of the vendored
submodules.

    python tools/make-sbom.py --dll <SC4SeamlessRegion.dll> --version <tag> --output <file.spdx.json>
"""
import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO = 'https://github.com/caspervg/sc4-seamless-region'
TRIPLET = 'x86-windows-static-md'

# Vendored sources compiled into the DLL: submodule path, name, licence.
SUBMODULES = [
    ('vendor/gzcom-dll', 'gzcom-dll', 'LGPL-2.1-or-later'),
    ('vendor/sc4-dll-utilities', 'sc4-dll-utilities', 'LGPL-2.1-or-later'),
]
# Headers and the delay-load stub are compiled in; SC4RenderServices.dll, which also exports Dear
# ImGui, is needed at run time.
RENDER_SERVICES = ('vendor/sc4-render-services', 'sc4-render-services', 'LGPL-2.1-only')
# vcpkg ports that are build tools, not part of the DLL.
BUILD_ONLY_PORTS = {'vcpkg-cmake', 'vcpkg-cmake-config'}
VCPKG_REPO = 'git+https://github.com/Microsoft/vcpkg@'


def git(*args, cwd='.'):
    return subprocess.run(['git', *args], cwd=cwd, check=True, capture_output=True, text=True).stdout.strip()


def spdx_id(name):
    return 'SPDXRef-Package-' + re.sub(r'[^A-Za-z0-9.-]', '-', name)


def github_purl(location):
    """pkg:github purl for a git+https://github.com/owner/repo@ref location, else None."""
    m = re.fullmatch(r'git\+https://github\.com/([^/]+)/([^/@]+?)(?:\.git)?@(.+)', location)
    return f'pkg:github/{m[1].lower()}/{m[2].lower()}@{m[3]}' if m else None


def package(name, version, licence, location, comment=None):
    pkg = {
        'SPDXID': spdx_id(name),
        'name': name,
        'versionInfo': version,
        'downloadLocation': location,
        'filesAnalyzed': False,
        'licenseConcluded': licence,
        'licenseDeclared': licence,
        'copyrightText': 'NOASSERTION',
    }
    purl = github_purl(location)
    if purl:
        pkg['externalRefs'] = [{'referenceCategory': 'PACKAGE-MANAGER', 'referenceType': 'purl',
                                'referenceLocator': purl}]
    if comment:
        pkg['comment'] = comment
    return pkg


def vcpkg_ports(installed):
    """One package per port, from the SPDX file vcpkg wrote for it."""
    packages = []
    for spdx_file in sorted(installed.glob('share/*/vcpkg.spdx.json')):
        doc = json.loads(spdx_file.read_text(encoding='utf-8'))
        port = doc['packages'][0]
        if port['name'] in BUILD_ONLY_PORTS:
            continue
        version = port['versionInfo']
        sources = [p for p in doc['packages'][1:]
                   if p.get('downloadLocation', 'NONE') not in ('NONE', 'NOASSERTION')
                   and not p['downloadLocation'].startswith(VCPKG_REPO)]
        patches = [p['downloadLocation'] for p in sources if p['name'].endswith('.patch')]
        upstream = next((p['downloadLocation'] for p in sources if not p['name'].endswith('.patch')),
                        'NOASSERTION')
        comment = f'vcpkg port {port["name"]} {version}, triplet {TRIPLET}.'
        if patches:
            comment += ' Patched with: ' + ', '.join(patches)
        packages.append(package(port['name'], version.split('#')[0], port.get('licenseConcluded') or 'NOASSERTION',
                                upstream, comment))
    if not packages:
        sys.exit(f'No vcpkg ports found in {installed}')
    return packages


def submodule(path, name, licence):
    url = git('config', '-f', '.gitmodules', '--get', f'submodule.{path}.url')
    url = re.sub(r'^git@github\.com:', 'https://github.com/', url).removesuffix('.git')
    commit = git('rev-parse', 'HEAD', cwd=path)
    return package(name, commit, licence, f'git+{url}@{commit}')


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--dll', required=True, type=Path)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', required=True, type=Path)
    parser.add_argument('--vcpkg-installed', type=Path, default=Path('vcpkg_installed') / TRIPLET)
    args = parser.parse_args()

    commit = git('rev-parse', 'HEAD')
    epoch = int(os.environ.get('SOURCE_DATE_EPOCH') or git('show', '-s', '--format=%ct'))
    created = datetime.fromtimestamp(epoch, timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ')
    dll = args.dll.read_bytes()

    root = package('SC4SeamlessRegion', args.version.removeprefix('v'), 'LGPL-3.0-or-later',
                   f'git+{REPO}@{commit}')
    root['packageFileName'] = args.dll.name
    root['checksums'] = [{'algorithm': 'SHA256', 'checksumValue': hashlib.sha256(dll).hexdigest()},
                         {'algorithm': 'SHA1', 'checksumValue': hashlib.sha1(dll).hexdigest()}]
    root['primaryPackagePurpose'] = 'LIBRARY'

    contained = vcpkg_ports(args.vcpkg_installed) + [submodule(*s) for s in SUBMODULES]
    render_services = submodule(*RENDER_SERVICES)
    render_services['comment'] = ('Headers and the delay-load stub are compiled in. SC4RenderServices.dll, '
                                  'which also exports Dear ImGui, is needed at run time.')
    runtime = package('Microsoft Visual C++ Redistributable', 'NOASSERTION', 'NOASSERTION', 'NOASSERTION',
                      'Needed at run time: the DLL links the C runtime dynamically.')

    relationships = [{'spdxElementId': 'SPDXRef-DOCUMENT', 'relationshipType': 'DESCRIBES',
                      'relatedSpdxElement': root['SPDXID']}]
    relationships += [{'spdxElementId': root['SPDXID'], 'relationshipType': 'CONTAINS',
                       'relatedSpdxElement': p['SPDXID']} for p in contained]
    relationships += [{'spdxElementId': root['SPDXID'], 'relationshipType': 'DEPENDS_ON',
                       'relatedSpdxElement': p['SPDXID']} for p in (render_services, runtime)]

    doc = {
        'spdxVersion': 'SPDX-2.3',
        'dataLicense': 'CC0-1.0',
        'SPDXID': 'SPDXRef-DOCUMENT',
        'name': f'SC4SeamlessRegion-{args.version}',
        'documentNamespace': f'{REPO}/spdx/{args.version}-{commit}',
        'creationInfo': {'created': created, 'creators': ['Tool: sc4-seamless-region tools/make-sbom.py']},
        'packages': [root, *contained, render_services, runtime],
        'relationships': relationships,
    }
    args.output.write_text(json.dumps(doc, indent=2) + '\n', encoding='utf-8')
    print(f'{args.output}: {len(contained)} contained, 2 run-time packages')


if __name__ == '__main__':
    main()
