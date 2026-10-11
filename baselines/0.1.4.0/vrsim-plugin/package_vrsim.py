# package_vrsim.py - KOP packaging for the VRSIM product plugin (VRSIM-PRODUCT-01 / R3c).
# Structural clone of the PROVEN ov11 package_ov11.py flow:
# CatalogCreator -> KopBuilder -> KopImporter verify. New identity, no ID reuse.
# FULL preflight (build receipt, 18 sources, native manifest facts, 4 fresh DLLs) runs BEFORE
# anything is created under packages/ or evidence/ and before any official exe is invoked.
# This card MUST NOT package: the 0.1.3.9 baseline guard below aborts until Root locks a new
# version and rebuilds the native manifest on a separate card.
import hashlib, json, os, re, shutil, subprocess, sys, zipfile
from pathlib import Path
from datetime import datetime


def abort(msg):
    raise SystemExit('PACKAGE_BUILD_ABORTED: ' + msg)


def sha256_file(p):
    return hashlib.sha256(Path(p).read_bytes()).hexdigest()


def normp(p):
    return os.path.normcase(os.path.normpath(str(p)))


def same_path(a, b):
    return normp(a) == normp(b)


def norm_rel(rel):
    return str(rel).replace('\\', '/').lower()


def read_utf8_strict(p):
    try:
        return Path(p).read_bytes().decode('utf-8')
    except UnicodeDecodeError as e:
        abort('source is not valid UTF-8 (no errors=replace masking): ' + str(p) + ' (' + str(e) + ')')


def check_current(path, sha, nbytes, label, prefix='NATIVE_STALE'):
    p = Path(str(path))
    if not p.is_file():
        abort(prefix + ': ' + label + ' file missing: ' + str(p))
    if p.stat().st_size != nbytes:
        abort('%s: %s bytes drift (%s): current=%s expected=%s' % (prefix, label, p, p.stat().st_size, nbytes))
    actual = sha256_file(p)
    if actual != str(sha).lower():
        abort('%s: %s sha drift (%s): current=%s expected=%s' % (prefix, label, p, actual, str(sha).lower()))


def shared_build_profile(row, label):
    """Strict shared build profile gate: the three fields must be present with exact types;
    an old profile-less row is rejected, never treated as default 1536. Returns a frozen
    (bool, int, str-or-None) tuple; never mutates the input and never reads ENV/defaults."""
    if not isinstance(row, dict):
        abort('NATIVE_STALE: ' + label + ' shared profile row is not a JSON object')
    for f in ('shared2048', 'sharedSourceEdge', 'sharedCompileSymbol'):
        if f not in row:
            abort('NATIVE_STALE: ' + label + ' shared profile field missing: ' + f
                  + ' (old profile-less manifests/receipts are rejected, never treated as default 1536)')
    flag, edge, symbol = row['shared2048'], row['sharedSourceEdge'], row['sharedCompileSymbol']
    if not isinstance(flag, bool):
        abort('NATIVE_STALE: ' + label + ' shared profile shared2048 is not a strict JSON boolean (string/int/float rejected): ' + repr(flag))
    if not isinstance(edge, int) or isinstance(edge, bool):
        abort('NATIVE_STALE: ' + label + ' shared profile sharedSourceEdge is not a strict integer (bool/string/float rejected): ' + repr(edge))
    if flag:
        if not isinstance(symbol, str) or symbol != 'VRSIM_SHARED_2048':
            abort('NATIVE_STALE: ' + label + ' shared profile sharedCompileSymbol must be exactly the string VRSIM_SHARED_2048 for the 2048 profile (empty string/=0/wrong case/null rejected): ' + repr(symbol))
        if edge != 2048:
            abort('NATIVE_STALE: ' + label + ' shared profile sharedSourceEdge != 2048 for shared2048=true: ' + repr(edge))
    else:
        if symbol is not None:
            abort('NATIVE_STALE: ' + label + ' shared profile sharedCompileSymbol must be JSON null for the 1536 profile (empty string/=0/value rejected): ' + repr(symbol))
        if edge != 1536:
            abort('NATIVE_STALE: ' + label + ' shared profile sharedSourceEdge != 1536 for shared2048=false: ' + repr(edge))
    return (flag, edge, symbol)


root = Path(__file__).resolve().parent
builder = Path(os.environ.get('APPBUILDER_ROOT') or r'C:\Program Files\KUKA\iiQWorks.App Builder 1.2')
if not builder.is_dir():
    abort('APPBUILDER_ROOT is not a directory: ' + str(builder))
template_dir = builder / 'plugins' / 'KopTemplates' / 'Default'
if not template_dir.is_dir():
    abort('KopTemplates\\Default not found: ' + str(template_dir))

name = 'VRSIM.Sim'
version = '0.1.4.0'
dlls = ['UX.VRSIM.Sim.Plugin.dll', 'VRSIM.Native.dll', 'NC01.Readback.dll', 'NC02.PairStage.dll']
expected_native_rel = {
    'VRSIM.Native.dll': 'vrsim-native/bin/VRSIM.Native.dll',
    'NC01.Readback.dll': 'source-readback/bin/NC01.Readback.dll',
    'NC02.PairStage.dll': 'pair-stage/bin/NC02.PairStage.dll',
}
expected_src = sorted([
    norm_rel(x) for x in [
        'CameraSweepController.cs', 'CaptureProfile.cs', 'EntryCalibration.cs',
        'HostedCaptureOwner.NC02XrQA.cs', 'HostedCaptureOwner.SharedGPU.cs',
        'HostedStagingOwner.NC02XrQA.cs', 'KernelUiPulse.cs',
        'NativeCaptureOwner.cs', 'StereoResampleWorker.cs', 'TrackingSelfTest.cs', 'VCFrameProducer.cs',
        'VRMonitorWindow.cs', 'VRSIMPlugin.cs', 'VRSIMPlugin.SharedGPU.cs',
        'Xr/DeviceIdentityResolver.cs', 'Xr/IXRProviders.cs', 'Xr/KnownDeviceRegistry.cs',
        'Xr/OpenXRRuntimeDiscovery.cs', 'Xr/XRDeviceManager.cs', 'Xr/XRDeviceModel.cs',
    ]
])

# ---- baseline guard: the 0.1.3.9 bits are stale-by-version; packaging is FORBIDDEN on this card.
# Root must lock a new version and rebuild the native manifest on a separate card first. ----
if version == '0.1.3.9':
    abort('version 0.1.3.9 is the old baseline; a version-bit change forbids packaging these '
          'artifacts until a new version + fresh native-build-manifest are locked by Root')

# ---- version-consistency gate: five literals, each exactly once, all equal to each other and
# to manifest.sourceVersion; missing/extra/divergent values or non-UTF8 bytes abort. ----
cs_text = read_utf8_strict(root / 'src' / 'VRSIMPlugin.cs')
av = re.findall(r'\[assembly: System\.Reflection\.AssemblyVersion\("([0-9.]+)"\)\]', cs_text)
fv = re.findall(r'\[assembly: System\.Reflection\.AssemblyFileVersion\("([0-9.]+)"\)\]', cs_text)
pv = re.findall(r'private const string PluginVersion = "([0-9.]+)"', cs_text)
native_text = read_utf8_strict(root.parent / 'vrsim-native' / 'VRSIMNative.cpp')
sv = re.findall(r'strcpy_s\(out->semanticVersion,\s*"([0-9.]+)"\)', native_text)
for label, hits in (('AssemblyVersion', av), ('AssemblyFileVersion', fv),
                    ('PluginVersion', pv), ('native semanticVersion', sv)):
    if len(hits) != 1:
        abort('version literal %s occurrences=%d (need exactly 1)' % (label, len(hits)))
five = {'assembly_version': av[0], 'assembly_file_version': fv[0], 'plugin_version': pv[0],
        'native_semantic_version': sv[0], 'package_version': version}
if len(set(five.values())) != 1 or av[0] != version:
    abort('version mismatch ' + json.dumps(five))
print('version-consistency OK: ' + version)

bin_dir = root / 'bin'
receipt_path = bin_dir / 'build-receipt.json'


def verify_native_manifest(manifest_path):
    """Equivalent read-only re-verification of VRSIM_NATIVE_BUILD_MANIFEST_V1 facts
    (same schema/fields as native-build-manifest.json); aborts on any drift."""
    mpath = Path(str(manifest_path))
    if not mpath.is_file():
        abort('NATIVE_STALE: native manifest missing: ' + str(mpath))
    mf = json.loads(read_utf8_strict(mpath))
    if mf.get('schema') != 'VRSIM_NATIVE_BUILD_MANIFEST_V1':
        abort('NATIVE_STALE: bad manifest schema: ' + str(mf.get('schema')))
    mf_profile = shared_build_profile(mf, 'native manifest')
    for flag in ('compilePass', 'sourceStable', 'PUnchanged', 'compileOnly'):
        if mf.get(flag) is not True:
            abort('NATIVE_STALE: manifest %s not true' % flag)
    if mf.get('configuration') != 'Release' or mf.get('platform') != 'x64':
        abort('NATIVE_STALE: manifest configuration/platform not Release/x64')
    src_version = mf.get('sourceVersion')
    if not src_version:
        abort('NATIVE_STALE: manifest sourceVersion missing')
    if src_version != version:
        abort('NATIVE_STALE: manifest sourceVersion %s != package version %s' % (src_version, version))
    if src_version != av[0]:
        abort('NATIVE_STALE: manifest sourceVersion %s != current C# AssemblyVersion %s' % (src_version, av[0]))
    if not same_path(root.parent, mf.get('candidateRoot', '')):
        abort('NATIVE_STALE: manifest candidateRoot differs from this script native root: ' + str(mpath))
    cand_root = Path(mf['candidateRoot'])
    # Fixed native input closure: exact 22 canonical relative paths (set comparison, not just
    # length), unique relative and source, source == candidateRoot+relative inside a
    # separator-bounded candidateRoot, then current SHA/bytes. Mirrors build.ps1:135-157/273-296
    # and BuildPortableSharedNativeThreeCompileOnly.ps1:23-39 (shared bridge 3 + shared helper 4
    # included). Schema VRSIM_NATIVE_BUILD_MANIFEST_V1 and all manifest fields unchanged.
    expected_native_inputs = [
        'openxr-probe/vendor/MIT.txt',
        'openxr-probe/vendor/openxr/openxr_platform_defines.h',
        'openxr-probe/vendor/openxr/openxr_platform.h',
        'openxr-probe/vendor/openxr/openxr.h',
        'pair-stage/NC02PairStage.cpp',
        'pair-stage/NC02PairStage.h',
        'source-readback/D3D9SurfaceReadback.cpp',
        'vrsim-native/GpuProjection.hlsl',
        'vrsim-native/GpuProjectionPass.cpp',
        'vrsim-native/GpuProjectionPass.h',
        'vrsim-native/VRSIMNative.cpp',
        'vrsim-native/VRSIMNative.h',
        'vrsim-native/VRSIMNative.vcxproj',
        'source-readback/NC01Readback.vcxproj',
        'pair-stage/NC02PairStage.vcxproj',
        'vrsim-native/SharedStereoBridge.cpp',
        'vrsim-native/SharedStereoBridge.h',
        'vrsim-native/SharedStereoWire.h',
        'vrsim-native/shared-helpers/ObsD3D9ActualSourceQA.cpp',
        'vrsim-native/shared-helpers/ObsD3D9ActualSourceQA.h',
        'vrsim-native/shared-helpers/ObsD3D9SharedColorQA.cpp',
        'vrsim-native/shared-helpers/ObsD3D9LayoutQA.cpp',
    ]
    expected_native_inputs_lower = sorted(norm_rel(x) for x in expected_native_inputs)
    inputs = mf.get('inputs')
    if not isinstance(inputs, list) or len(inputs) != 22:
        abort('NATIVE_STALE: manifest inputs count=%s (need exactly 22)' % (len(inputs) if isinstance(inputs, list) else 'none'))
    seen_rel, seen_src = set(), set()
    root_boundary = normp(str(cand_root)) + os.sep
    for inp in inputs:
        rel, src, sha, byt = inp.get('relative'), inp.get('source'), inp.get('sha256'), inp.get('bytes')
        if not isinstance(rel, str) or not rel or not isinstance(src, str) or not src \
                or not sha or not isinstance(byt, int):
            abort('NATIVE_STALE: manifest input entry missing relative/source/sha256/bytes')
        # Reject noncanonical/escaping relative BEFORE deriving: rooted/drive/UNC, colon,
        # wildcard, empty slash component, '.', '..', leading/trailing slash.
        # Only normalization performed is slash-separator + case, like build.ps1.
        rel_slash = rel.replace('\\', '/')
        if ':' in rel_slash or '?' in rel_slash or '*' in rel_slash:
            abort('NATIVE_STALE: manifest input relative must be plain relative (no drive/colon/UNC/wildcard): ' + rel)
        if os.path.isabs(rel_slash) or any(part in ('', '.', '..') for part in rel_slash.split('/')):
            abort('NATIVE_STALE: manifest input relative not canonical or escapes candidate tree: ' + rel)
        nr = norm_rel(rel)
        if nr not in expected_native_inputs_lower:
            abort('NATIVE_STALE: manifest input relative not in fixed native input set: ' + rel)
        if nr in seen_rel:
            abort('NATIVE_STALE: duplicate manifest input relative: ' + rel)
        seen_rel.add(nr)
        derived = os.path.normpath(os.path.join(str(cand_root), rel))
        if not normp(derived).startswith(root_boundary):
            abort('NATIVE_STALE: manifest input derived path outside candidateRoot boundary: ' + rel)
        if not same_path(src, derived):
            abort('NATIVE_STALE: manifest input source not candidateRoot+relative (root moved or source drifted): ' + rel)
        ns = normp(src)
        if ns in seen_src:
            abort('NATIVE_STALE: duplicate manifest input source: ' + src)
        seen_src.add(ns)
        check_current(src, sha, byt, 'manifest input ' + rel)
    if sorted(seen_rel) != expected_native_inputs_lower:
        abort('NATIVE_STALE: manifest inputs do not cover the exact fixed 22 native input set')
    tools = mf.get('tools')
    if not isinstance(tools, list) or len(tools) != 4:
        abort('NATIVE_STALE: manifest tools count=%s (need exactly 4)' % (len(tools) if isinstance(tools, list) else 'none'))
    for t in tools:
        if not t.get('path') or not t.get('sha256') or not isinstance(t.get('bytes'), int):
            abort('NATIVE_STALE: manifest tool entry missing path/bytes/sha256')
        check_current(t['path'], t['sha256'], t['bytes'], 'manifest tool ' + str(t['path']))
    gbh = mf.get('generatedBytecodeHeaders')
    if not isinstance(gbh, list) or len(gbh) != 2:
        abort('NATIVE_STALE: generatedBytecodeHeaders count=%s (need exactly 2)' % (len(gbh) if isinstance(gbh, list) else 'none'))
    bases = sorted(os.path.normcase(os.path.basename(str(h.get('path', '')))) for h in gbh)
    if bases != ['gpuprojectionps.inc', 'gpuprojectionvs.inc']:
        abort('NATIVE_STALE: generatedBytecodeHeaders basenames wrong: ' + json.dumps(bases))
    for h in gbh:
        if not h.get('path') or not h.get('sha256') or not isinstance(h.get('bytes'), int):
            abort('NATIVE_STALE: generated header entry missing path/bytes/sha256')
        check_current(h['path'], h['sha256'], h['bytes'], 'manifest generated header ' + str(h['path']))
    crec = mf.get('compileReceipt')
    if not isinstance(crec, dict) or not crec.get('path') or not crec.get('sha256'):
        abort('NATIVE_STALE: manifest compileReceipt incomplete')
    cp = Path(crec['path'])
    if not cp.is_file():
        abort('NATIVE_STALE: compile receipt missing: ' + str(cp))
    if sha256_file(cp) != str(crec['sha256']).lower():
        abort('NATIVE_STALE: compile receipt sha mismatch: ' + str(cp))
    cr = json.loads(read_utf8_strict(cp))
    if cr.get('compilerPass') is not True or cr.get('sourceUnchanged') is not True or cr.get('productUnchanged') is not True:
        abort('NATIVE_STALE: compile receipt flags not all true')
    cr_profile = shared_build_profile(cr, 'native compile receipt')
    if mf_profile != cr_profile:
        abort('NATIVE_STALE: native manifest shared profile != native compile receipt shared profile: manifest='
              + repr(mf_profile) + ' compileReceipt=' + repr(cr_profile))
    arts = mf.get('artifacts')
    if not isinstance(arts, list) or len(arts) != 3:
        abort('NATIVE_STALE: manifest artifacts count=%s (need exactly 3)' % (len(arts) if isinstance(arts, list) else 'none'))
    art_by_name = {}
    for a in arts:
        an = a.get('name')
        if an not in expected_native_rel or an in art_by_name:
            abort('NATIVE_STALE: unexpected or duplicate artifact name: ' + str(an))
        art_by_name[an] = a
    if len(art_by_name) != 3:
        abort('NATIVE_STALE: artifacts must cover the 3 official names once each')
    outs = cr.get('outputs')
    if not isinstance(outs, list) or len(outs) != 3:
        abort('NATIVE_STALE: compile receipt outputs count=%s (need exactly 3)' % (len(outs) if isinstance(outs, list) else 'none'))
    covered = set()
    for o in outs:
        d = o.get('dll') or {}
        if o.get('compilerExit') != 0 or o.get('x64') is not True:
            abort('NATIVE_STALE: compile receipt output not (exit 0, x64): ' + str(d.get('path')))
        bn = os.path.basename(str(d.get('path', '')))
        if bn not in art_by_name:
            abort('NATIVE_STALE: compile receipt output not a manifest artifact: ' + bn)
        a = art_by_name[bn]
        if not same_path(d.get('path'), a.get('source')) or str(d.get('sha256', '')).lower() != str(a['sha256']).lower() \
                or d.get('bytes') != a.get('bytes'):
            abort('NATIVE_STALE: compile receipt output facts != manifest artifact for ' + bn)
        covered.add(bn)
    if covered != set(art_by_name):
        abort('NATIVE_STALE: compile receipt outputs do not cover all 3 manifest artifacts')
    for a in arts:
        an = a['name']
        if norm_rel(a.get('intendedCandidateRelative', '')) != norm_rel(expected_native_rel[an]):
            abort('NATIVE_STALE: artifact intendedCandidateRelative wrong for ' + an)
        src = str(a.get('source', ''))
        if not os.path.isabs(src):
            abort('NATIVE_STALE: artifact source not absolute for ' + an)
        check_current(src, a['sha256'], a['bytes'], 'QA compile-output ' + an)
        official = os.path.normpath(os.path.join(str(cand_root), str(a['intendedCandidateRelative'])))
        check_current(official, a['sha256'], a['bytes'], 'candidate official bin ' + an)
    return mf


def verify_fresh_build():
    rp = bin_dir / 'build-receipt.json'
    if not rp.is_file():
        abort('build receipt missing: ' + str(rp))
    br = json.loads(read_utf8_strict(rp))
    if br.get('schema') != 'VRSIM_PRODUCT_BUILD_V1' or br.get('success') is not True:
        abort('build receipt schema/success wrong: ' + str(br.get('schema')) + '/' + str(br.get('success')))
    br_profile = shared_build_profile(br, 'build receipt')
    if br.get('configuration') != 'Release' or br.get('platform') != 'x64' \
            or br.get('optimization') is not True or br.get('qaDefine') is not False or br.get('installed') is not False:
        abort('build receipt build-flag fields wrong')
    # 20 current sources still match the receipt (SHA/bytes), and receipt paths map to src/ here.
    src_files = br.get('sourceFiles')
    if not isinstance(src_files, list) or len(src_files) != 20:
        abort('build receipt sourceFiles count=%s (need exactly 20)' % (len(src_files) if isinstance(src_files, list) else 'none'))
    rels = sorted(norm_rel(e.get('relative', '')) for e in src_files)
    if rels != expected_src:
        abort('build receipt sourceFiles relative set != the exact 20 product sources')
    src_dir_norm = normp(root / 'src')
    for e in src_files:
        p = Path(str(e.get('path', '')))
        if not same_path(p, os.path.normpath(os.path.join(str(root / 'src'), str(e['relative'])))):
            abort('build receipt source path does not map to this tree/src: ' + str(p))
        if normp(p.parent) != src_dir_norm and not normp(str(p)).startswith(src_dir_norm + os.sep):
            abort('build receipt source path outside tree/src: ' + str(p))
        check_current(p, e.get('sha256'), e.get('bytes'), 'source ' + str(e.get('relative')), prefix='BUILD_INPUT_STALE')
    actual_srcs = sorted(norm_rel(p.relative_to(root / 'src')) for p in (root / 'src').rglob('*.cs'))
    if actual_srcs != expected_src:
        abort('src tree no longer contains exactly the 20 product sources: ' + json.dumps(actual_srcs))
    # native manifest: current SHA matches the receipt, then full equivalent re-verification.
    mpath = Path(str(br.get('nativeManifestPath', '')))
    if not mpath.is_absolute() or not mpath.is_file():
        abort('build receipt nativeManifestPath not a present absolute path: ' + str(mpath))
    if sha256_file(mpath) != str(br.get('nativeManifestSha256', '')).lower():
        abort('NATIVE_STALE: native manifest sha drift vs build receipt: ' + str(mpath))
    mf = verify_native_manifest(mpath)
    mf_profile_now = shared_build_profile(mf, 'native manifest')
    if mf_profile_now != br_profile:
        abort('NATIVE_STALE: build receipt shared profile != native manifest shared profile: receipt='
              + repr(br_profile) + ' manifest=' + repr(mf_profile_now))
    # four DLLs once each in root/bin, current hash/bytes == receipt artifacts == manifest facts.
    art = br.get('artifacts')
    if not isinstance(art, list) or len(art) != 4:
        abort('build receipt artifacts count=%s (need exactly 4)' % (len(art) if isinstance(art, list) else 'none'))
    names = [str(e.get('name', '')) for e in art]
    if sorted(names) != sorted(dlls) or len(set(names)) != 4:
        abort('build receipt artifact names must be exactly the 4 official basenames once each: ' + json.dumps(names))
    bin_dlls = sorted(p.name for p in bin_dir.glob('*.dll'))
    if bin_dlls != sorted(dlls):
        abort('root/bin must contain exactly the 4 official DLLs, found: ' + json.dumps(bin_dlls))
    art_by_name = {str(e['name']): e for e in art}
    for dn in dlls:
        e = art_by_name[dn]
        p = Path(str(e.get('path', '')))
        if not same_path(p, bin_dir / dn):
            abort('build receipt artifact path does not correspond to this bin/ basename (old-bin impersonation): %s' % str(p))
        check_current(bin_dir / dn, e.get('sha256'), e.get('bytes'), 'bin artifact ' + dn)
    for an, a in art_by_name.items():
        if an in expected_native_rel:
            if str(a['sha256']).lower() != str(mf_art_sha(mf, an)).lower() or a['bytes'] != mf_art_bytes(mf, an):
                abort('NATIVE_STALE: build receipt artifact facts != native manifest for ' + an)
    return br, mf


def mf_art_sha(mf, an):
    return next(a['sha256'] for a in mf['artifacts'] if a['name'] == an)


def mf_art_bytes(mf, an):
    return next(a['bytes'] for a in mf['artifacts'] if a['name'] == an)


# ---- FULL preflight BEFORE creating evidence/output or invoking any official tool ----
br, mf = verify_fresh_build()
print('preflight OK: fresh build receipt + native manifest + 4 DLLs verified')

dll = 'UX.VRSIM.Sim.Plugin.dll'
native = 'VRSIM.Native.dll'
ev = root / 'evidence' / 'product01'
ev.mkdir(parents=True, exist_ok=True)

output = root / 'packages' / datetime.now().strftime('%Y%m%d-%H%M%S')
output.mkdir(parents=True, exist_ok=False)
source = output / 'source' / 'Applications' / 'iiQWorks.Sim' / 'Plugin'
source.mkdir(parents=True)
for dn in dlls:
    shutil.copy2(bin_dir / dn, source / dn)
for dn in dlls:
    if sha256_file(source / dn) != sha256_file(bin_dir / dn):
        abort('post-copy sha mismatch in package staging: ' + dn)
template = output / 'template'
shutil.copytree(template_dir, template)
mapping = output / 'mapping.txt'
mapping.write_text('.\\Applications\\iiQWorks.Sim\\* => Applications\\iiQWorks.Sim\n', encoding='utf-8')
# Sim install here is 1.3.0.7080 (BuildInfo.txt) -> [1.3;1.4) is the correct range.
metadata = output / 'metadata.xml'
metadata.write_text('''<?xml version="1.0" encoding="utf-8"?>
<KopFileMetadata xmlns:xsi="http://www.w3.org/2001/XMLSchema-instance" xmlns:xsd="http://www.w3.org/2001/XMLSchema" xmlns:ac="http://AdditionalKopFileContents">
<Dependencies><Dependency><DependencyType>Application</DependencyType><Identifier>IiQWorksSim</Identifier><Versions><DependencyVersion><IsSupported>true</IsSupported><Version>[1.3;1.4)</Version></DependencyVersion></Versions></Dependency></Dependencies>
<FileManipulations/><DeploymentControlRelevant>true</DeploymentControlRelevant><GenerateMd5>true</GenerateMd5></KopFileMetadata>''', encoding='utf-8')


def run(exe, args, log):
    p = subprocess.run([str(builder / exe), *map(str, args)], capture_output=True,
                       creationflags=subprocess.CREATE_NO_WINDOW)
    (output / log).write_bytes(p.stdout + b'\n' + p.stderr)
    return p


p1 = run('iiQWorks.AppBuilder.CatalogCreator.exe',
         ['create-catalog', '--path', template, '--identifier', name, '--version', version,
          '--creator', 'VRSIM', '--contact', 'local'], 'catalog.log')
kop = output / (name + '-' + version + '.kop')
with zipfile.ZipFile(kop, 'w', zipfile.ZIP_DEFLATED) as z:
    for p in template.rglob('*'):
        if p.is_file():
            z.write(p, p.relative_to(template).as_posix())
work = output / 'work'; work.mkdir()
p2 = run('iiQWorks.KopBuilder.exe',
         [f'mappingFile={mapping}', f'sourceDirectory={output / "source"}', f'targetKop={kop}',
          f'productName={name}', f'productVersion={version}', f'kopFileMetadataFile={metadata}',
          'verbose=True', f'workDirectory={work}'], 'builder.log')

sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
kop_built = kop.exists() and kop.stat().st_size > 0
kop_build = {
    'KOP_PATH': str(kop), 'KOP_SIZE': kop.stat().st_size if kop_built else 0,
    'KOP_SHA256': sha(kop) if kop_built else None,
    'PACKAGE_ID': name, 'VERSION': version,
    'BUILD_TIMESTAMP': datetime.utcnow().isoformat() + 'Z',
    'CATALOG_EXIT_CODE': p1.returncode, 'BUILDER_EXIT_CODE': p2.returncode,
    'catalog_log': str(output / 'catalog.log'), 'builder_log': str(output / 'builder.log'),
    'source_files': [str(source / dn) for dn in dlls],
    'mapping': mapping.read_text(encoding='utf-8').strip(),
    'sim_dependency': 'IiQWorksSim [1.3;1.4) (verified against BuildInfo.txt 1.3.0.7080)',
    'KOP_BUILD': 'PASS' if kop_built and p1.returncode == 0 and p2.returncode == 0 else 'FAIL',
}
(ev / 'kop-build.json').write_text(json.dumps(kop_build, indent=2), encoding='utf-8')

imported = output / 'import-check'; imported.mkdir()
p3 = run('iiQWorks.AppBuilder.KopImporter.exe',
         ['import', '--kop-file', kop, '--output', imported, '--verbose'], 'importer.log')

# every one of the 4 DLLs must appear exactly once in the official reverse import,
# with hash/bytes equal to this run's root/bin copy; managed/main alone is never enough
artifact_rows = []
import_ok = p3.returncode == 0 and kop_build['KOP_BUILD'] == 'PASS'
for dn in dlls:
    found = [f for f in imported.rglob(dn) if f.is_file()]
    bin_sha, bin_bytes = sha256_file(bin_dir / dn), (bin_dir / dn).stat().st_size
    one_match = (len(found) == 1 and sha(found[0]) == bin_sha
                 and found[0].stat().st_size == bin_bytes)
    import_ok = import_ok and one_match
    artifact_rows.append({
        'name': dn,
        'source': str(bin_dir / dn),
        'sha256': bin_sha,
        'bytes': bin_bytes,
        'importedPath': str(found[0]) if found else None,
        'importedSha256': sha(found[0]) if found else None,
        'importedCount': len(found),
    })
kop_import = {
    'IMPORT_EXIT_CODE': p3.returncode,
    'IMPORT_STDOUT': p3.stdout.decode('utf-8', 'replace')[-4000:],
    'IMPORT_STDERR': p3.stderr.decode('utf-8', 'replace')[-4000:],
    'IMPORT_RESULT': 'PASS' if import_ok else 'FAIL',
    'imported_plugin_dll': next((r['importedPath'] for r in artifact_rows if r['name'] == dll), None),
    'imported_native_dll': next((r['importedPath'] for r in artifact_rows if r['name'] == native), None),
    'imported_dll_sha_matches_build': import_ok,
    'imported_files': [p.relative_to(imported).as_posix() for p in imported.rglob('*') if p.is_file()],
    'importer_log': str(output / 'importer.log'),
}
(ev / 'kop-import.json').write_text(json.dumps(kop_import, indent=2), encoding='utf-8')

receipt = {
    'package': name, 'version': version, 'kop': str(kop), 'kop_sha256': kop_build['KOP_SHA256'],
    'plugin_dll': dll, 'plugin_dll_sha256': sha(root / 'bin' / dll),
    'native_dll': native, 'native_dll_sha256': sha(root / 'bin' / native),
    'artifacts': artifact_rows,
    'build_receipt_native_manifest_sha256': str(br.get('nativeManifestSha256', '')).lower(),
    'kop_build': kop_build['KOP_BUILD'], 'kop_import_verify': kop_import['IMPORT_RESULT'],
    'output_dir': str(output), 'official_import_pass': import_ok,
    'installed': False, 'candidateReady': False, 'promotionReady': False,
}
(output / 'receipt.json').write_text(json.dumps(receipt, indent=2), encoding='utf-8')
print(json.dumps(receipt, indent=2))

# KOP build or import FAIL must exit nonzero; never print a failure and exit 0.
if not (import_ok and kop_build['KOP_BUILD'] == 'PASS'):
    raise SystemExit('PACKAGE_BUILD_ABORTED: KOP build/import verification FAILED (see receipts in '
                     + str(ev) + ' and ' + str(output / 'receipt.json') + ')')
