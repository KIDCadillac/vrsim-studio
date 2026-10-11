# build.ps1 - VRSIM product managed build for iiQWorks.Sim 1.3 (VRSIM-PRODUCT-01 / R3c).
# Roslyn csc, /target:library /platform:x64 /deterministic /optimize+ /unsafe /langversion:7.3,
# references straight from the discovered Sim install. No VRSIM_NATIVE_QA define, no SDK objects.
# The native side is NOT built here: native-build-manifest.json (VRSIM_NATIVE_BUILD_MANIFEST_V1)
# gates three pre-staged Release x64 DLLs which are copied from their official candidate bin
# paths beside the managed output. Failures throw NATIVE_STALE before csc or any copy.
param(
  [string] $OutDir,
  [string] $SimRoot = 'C:\Program Files\KUKA\iiQWorks.Sim 1.3',
  [string] $Csc = 'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\Roslyn\csc.exe',
  [string] $WpfDir = 'C:\Program Files (x86)\Reference Assemblies\Microsoft\Framework\.NETFramework\v4.8',
  [string] $NativeManifestPath = '',
  [switch] $Shared2048,
  [Parameter(Mandatory=$true)][string] $ManagedSourcePinsPath,
  [Parameter(Mandatory=$true)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string] $ManagedSourcePinsSha256
)
$ErrorActionPreference = 'Stop'

# Shared build profile: frozen once here from the single switch. Never reread from ENV, never an arbitrary define list.
$requestedShared2048 = [bool] $Shared2048
$requestedSharedEdge = if ($requestedShared2048) { 2048 } else { 1536 }
$requestedSharedSymbol = if ($requestedShared2048) { 'VRSIM_SHARED_2048' } else { $null }
# Pre-initialized so a failure receipt can never hit a secondary error on these.
$cscArguments = $null
$cscDefines = @()
$nativeManifestProfile = $null
$nativeCompileReceiptProfile = $null

function Assert-SharedBuildProfile($row, [string] $label) {
  foreach ($f in @('shared2048', 'sharedSourceEdge', 'sharedCompileSymbol')) {
    if ($null -eq $row -or -not $row.PSObject.Properties[$f]) { throw ('NATIVE_STALE: ' + $label + ' shared profile field missing: ' + $f + ' (old profile-less manifests/receipts are rejected, never treated as default 1536)') }
  }
  if ($row.shared2048 -isnot [bool]) { throw ('NATIVE_STALE: ' + $label + ' shared profile shared2048 is not a strict JSON boolean (string/int rejected): ' + $row.shared2048) }
  if (($row.sharedSourceEdge -isnot [int]) -and ($row.sharedSourceEdge -isnot [long])) { throw ('NATIVE_STALE: ' + $label + ' shared profile sharedSourceEdge is not a strict integer (string/float/bool rejected): ' + $row.sharedSourceEdge) }
  if ($requestedShared2048) {
    if (($row.sharedCompileSymbol -isnot [string]) -or ($row.sharedCompileSymbol -cne 'VRSIM_SHARED_2048')) { throw ('NATIVE_STALE: ' + $label + ' shared profile sharedCompileSymbol must be exactly the string VRSIM_SHARED_2048 for the selected 2048 profile: ' + $row.sharedCompileSymbol) }
  } else {
    if ($null -ne $row.sharedCompileSymbol) { throw ('NATIVE_STALE: ' + $label + ' shared profile sharedCompileSymbol must be JSON null for the selected 1536 profile (empty string/=0/value rejected): ' + $row.sharedCompileSymbol) }
  }
  if ($row.shared2048 -ne $requestedShared2048) { throw ('NATIVE_STALE: ' + $label + ' shared profile mismatch with requested profile: shared2048 found=' + $row.shared2048 + ' requested=' + $requestedShared2048) }
  if ($row.sharedSourceEdge -ne $requestedSharedEdge) { throw ('NATIVE_STALE: ' + $label + ' shared profile mismatch with requested profile: sharedSourceEdge found=' + $row.sharedSourceEdge + ' requested=' + $requestedSharedEdge) }
  if ($row.sharedCompileSymbol -cne $requestedSharedSymbol) { throw ('NATIVE_STALE: ' + $label + ' shared profile mismatch with requested profile: sharedCompileSymbol differs from the frozen request') }
}

function Get-FullPath([string] $p) { return [System.IO.Path]::GetFullPath($p) }
function Get-Sha256Lower([string] $p) { return (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash.ToLower() }
function Normalize-Rel([string] $rel) { return ($rel -replace '\\', '/').ToLower() }
function Test-IsAncestorDir([string] $anc, [string] $desc) {
  $a = (Get-FullPath $anc).TrimEnd('\')
  if ($a -match '^[A-Za-z]:$') { $a += '\' } else { $a += '\' }
  $d = (Get-FullPath $desc).TrimEnd('\') + '\'
  return $d.StartsWith($a, [System.StringComparison]::OrdinalIgnoreCase)
}
function Assert-NoReparse([string] $path, [string] $label) {
  $p = Get-FullPath $path
  while ($true) {
    if (Test-Path -LiteralPath $p) {
      $it = Get-Item -LiteralPath $p -Force
      if (($it.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0) { throw ('REPARSE: ' + $label + ' or an ancestor is a reparse point: ' + $p) }
    }
    $parent = Split-Path $p -Parent
    if (-not $parent -or $parent -eq $p) { break }
    $p = $parent
  }
}
$script:capturedInputs = $null
function Add-CapturedInput([string] $path, [string] $label, [string] $ExpectedSha = '', $ExpectedBytes = $null) {
  $full = Get-FullPath $path
  if (-not (Test-Path -LiteralPath $full -PathType Leaf)) { throw ('BUILD_INPUT_STALE: snapshot input missing: ' + $label + ' ' + $full) }
  Assert-NoReparse $full ('snapshot input ' + $label)
  $len = (Get-Item -LiteralPath $full -Force).Length
  $sha = Get-Sha256Lower $full
  if ($ExpectedSha) {
    if ($ExpectedSha -notmatch '^[A-Fa-f0-9]{64}$') { throw ('BUILD_INPUT_STALE: expected sha not 64-hex (' + $label + '): ' + $ExpectedSha) }
    if ($sha -ne $ExpectedSha.ToLower()) { throw ('BUILD_INPUT_STALE: current sha differs from earlier expected (' + $label + '): current=' + $sha + ' expected=' + $ExpectedSha.ToLower()) }
  }
  if ($null -ne $ExpectedBytes -and $len -ne $ExpectedBytes) { throw ('BUILD_INPUT_STALE: current bytes differ from earlier expected (' + $label + '): current=' + $len + ' expected=' + $ExpectedBytes) }
  $key = $full.ToLower()
  if ($script:capturedInputs.Contains($key)) {
    $prev = $script:capturedInputs[$key]
    if ($prev.bytes -ne $len -or $prev.sha256 -ne $sha) { throw ('BUILD_INPUT_STALE: same snapshot path with differing bytes/sha: ' + $full) }
    return
  }
  $script:capturedInputs[$key] = [ordered]@{ label = $label; path = $full; bytes = $len; sha256 = $sha }
}
function Assert-CapturedInputs([string] $when) {
  if (-not $script:capturedInputs -or $script:capturedInputs.Count -eq 0) { throw ('BUILD_INPUT_STALE: input snapshot empty at ' + $when) }
  foreach ($key in @($script:capturedInputs.Keys)) {
    $row = $script:capturedInputs[$key]
    if (-not (Test-Path -LiteralPath $row.path -PathType Leaf)) { throw ('BUILD_INPUT_STALE: input vanished (' + $when + '): ' + $row.label + ' ' + $row.path) }
    Assert-NoReparse $row.path ('input recheck (' + $when + ') ' + $row.label)
    $len = (Get-Item -LiteralPath $row.path -Force).Length
    if ($len -ne $row.bytes) { throw ('BUILD_INPUT_STALE: input bytes drift (' + $when + '): ' + $row.label + ' ' + $row.path + ' current=' + $len + ' frozen=' + $row.bytes) }
    $sha = Get-Sha256Lower $row.path
    if ($sha -ne $row.sha256) { throw ('BUILD_INPUT_STALE: input sha drift (' + $when + '): ' + $row.label + ' ' + $row.path + ' current=' + $sha + ' frozen=' + $row.sha256) }
  }
}
function Read-Utf8Strict([string] $p) {
  $enc = New-Object System.Text.UTF8Encoding($false, $true)
  return [System.IO.File]::ReadAllText($p, $enc)
}
function Assert-CurrentFile([string] $path, [string] $sha, $bytes, [string] $label) {
  if (-not $path -or -not (Test-Path -LiteralPath $path -PathType Leaf)) { throw ('NATIVE_STALE: ' + $label + ' file missing: ' + $path) }
  $len = (Get-Item -LiteralPath $path).Length
  if ($len -ne $bytes) { throw ('NATIVE_STALE: ' + $label + ' bytes drift: ' + $path + ' current=' + $len + ' manifest=' + $bytes) }
  $actual = Get-Sha256Lower $path
  if ($actual -ne $sha.ToLower()) { throw ('NATIVE_STALE: ' + $label + ' sha drift: ' + $path + ' current=' + $actual + ' manifest=' + $sha.ToLower()) }
}

$here = $PSScriptRoot
$rootDir = Split-Path $here -Parent
if (-not $OutDir) { $OutDir = Join-Path $here 'bin' }
if (-not [System.IO.Path]::IsPathRooted($OutDir)) { throw ('OutDir must be an absolute path: ' + $OutDir) }
$OutDir = Get-FullPath $OutDir
$receiptPath = Join-Path $OutDir 'build-receipt.json'
$pinsFile = if ([System.IO.Path]::IsPathRooted($ManagedSourcePinsPath)) { Get-FullPath $ManagedSourcePinsPath } else { $ManagedSourcePinsPath }

# Pin this build script's own initial facts at setup, before any later stage could hash a mutated copy.
$buildScriptPath = Get-FullPath (Join-Path $here 'build.ps1')
if (-not (Test-Path -LiteralPath $buildScriptPath -PathType Leaf)) { throw ('BUILD_SCRIPT_MISSING: ' + $buildScriptPath) }
$buildScriptSha = Get-Sha256Lower $buildScriptPath
$buildScriptBytes = (Get-Item -LiteralPath $buildScriptPath -Force).Length

# Fresh-output gate, FIRST: it must run before the stale-receipt-invalidation below can write anything.
$h2Dir = Get-FullPath (Split-Path $rootDir -Parent)
if ($OutDir -eq (Get-FullPath $h2Dir)) { throw ('REFUSING OutDir equal to H2 review root (must be strictly inside): ' + $OutDir) }
if (-not $OutDir.StartsWith((Get-FullPath $h2Dir).TrimEnd('\') + '\', [System.StringComparison]::OrdinalIgnoreCase)) { throw ('REFUSING OutDir outside H2 boundary: ' + $OutDir) }
$forbiddenOutDirs = @((Join-Path $here 'src'),
  (Join-Path $rootDir 'vrsim-native'), (Join-Path $rootDir 'source-readback'),
  (Join-Path $rootDir 'pair-stage'), (Join-Path $rootDir 'openxr-probe'), $rootDir)
foreach ($fd in $forbiddenOutDirs) {
  $ffd = Get-FullPath $fd
  if ($OutDir -eq $ffd) { throw ('REFUSING source directory as OutDir: ' + $ffd) }
  if (Test-IsAncestorDir $OutDir $ffd) { throw ('REFUSING OutDir that is an ancestor of a source directory: ' + $ffd) }
  if ($ffd -ne (Get-FullPath $rootDir) -and (Test-IsAncestorDir $ffd $OutDir)) { throw ('REFUSING OutDir inside source directory: ' + $ffd) }
}
if (Test-Path -LiteralPath $OutDir) { throw ('FRESH_OUTPUT: OutDir already exists - refusing to reuse or Force-overwrite: ' + $OutDir) }

if (-not $NativeManifestPath) {
  $NativeManifestPath = Join-Path $rootDir 'native-build-manifest.json'
} elseif (-not [System.IO.Path]::IsPathRooted($NativeManifestPath)) {
  throw ('NativeManifestPath must be a full path: ' + $NativeManifestPath)
}
$NativeManifestPath = Get-FullPath $NativeManifestPath

# Reparse preflight: script, candidate root, src, native manifest, OutDir ancestor chain.
Assert-NoReparse $here 'script dir'
Assert-NoReparse $rootDir 'candidate root'
Assert-NoReparse (Join-Path $here 'src') 'managed src dir'
Assert-NoReparse $NativeManifestPath 'native manifest path'
Assert-NoReparse $OutDir 'OutDir chain'

$expectedSrc = @(
  'CameraSweepController.cs','CaptureProfile.cs','EntryCalibration.cs',
  'HostedCaptureOwner.NC02XrQA.cs','HostedCaptureOwner.SharedGPU.cs',
  'HostedStagingOwner.NC02XrQA.cs','KernelUiPulse.cs',
  'NativeCaptureOwner.cs','StereoResampleWorker.cs','TrackingSelfTest.cs','VCFrameProducer.cs',
  'VRMonitorWindow.cs','VRSIMPlugin.cs','VRSIMPlugin.SharedGPU.cs',
  'Xr/DeviceIdentityResolver.cs','Xr/IXRProviders.cs','Xr/KnownDeviceRegistry.cs',
  'Xr/OpenXRRuntimeDiscovery.cs','Xr/XRDeviceManager.cs','Xr/XRDeviceModel.cs')
# Fixed native input closure: exactly 22 canonical relative paths (forward slashes, stored verbatim).
$expectedNativeInputs = @(
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
  'vrsim-native/shared-helpers/ObsD3D9LayoutQA.cpp')
$expectedNativeInputsLower = @($expectedNativeInputs | ForEach-Object { $_.ToLower() })
$expectedNativeRel = @{
  'VRSIM.Native.dll'   = 'vrsim-native/bin/VRSIM.Native.dll'
  'NC01.Readback.dll'  = 'source-readback/bin/NC01.Readback.dll'
  'NC02.PairStage.dll' = 'pair-stage/bin/NC02.PairStage.dll'
}
foreach ($nb in $expectedNativeRel.Values) {
  if ($OutDir -eq (Get-FullPath (Join-Path $rootDir $nb))) { throw ('REFUSING OutDir equal to native official bin: ' + $nb) }
}

$stage = 'stale-receipt-invalidation'
try {
  # A previous success=true receipt must never survive into a new (possibly failing) round.
  if (Test-Path -LiteralPath $receiptPath -PathType Leaf) {
    try {
      $old = Get-Content -LiteralPath $receiptPath -Raw -Encoding UTF8 | ConvertFrom-Json
      if ($old -and $old.PSObject.Properties['schema'] -and $old.schema -eq 'VRSIM_PRODUCT_BUILD_V1' -and $old.success -eq $true) {
        $void = [ordered]@{ schema = 'VRSIM_PRODUCT_BUILD_V1'; utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ')
          success = $false; stage = 'invalidated-before-new-round'; error = 'previous success receipt voided by new build round'
          installed = $false; candidateReady = $false; promotionReady = $false }
        $void | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
      }
    } catch { }
  }

  $stage = 'preflight'
  if (-not (Test-Path -LiteralPath $SimRoot -PathType Container)) { throw ('SIM_ROOT not a directory: ' + $SimRoot) }
  $required = @('Create3D.Shared.dll','Caliburn.Micro.dll','UX.Shared.dll','UX.Shared.Infrastructure.dll')
  $missing = @(); foreach ($r in $required) { if (-not (Test-Path -LiteralPath (Join-Path $SimRoot $r) -PathType Leaf)) { $missing += $r } }
  if ($missing.Count) { throw ('Sim install missing reference assemblies: ' + ($missing -join ', ')) }
  if (-not (Test-Path -LiteralPath $Csc -PathType Leaf)) { throw ('csc.exe not found: ' + $Csc) }
  if (-not (Test-Path -LiteralPath $WpfDir -PathType Container)) { throw ('WPF ref dir not found: ' + $WpfDir) }

  $outFull = Get-FullPath $OutDir
  foreach ($b in @('vrsim-native\bin','source-readback\bin','pair-stage\bin')) {
    if ($outFull -eq (Get-FullPath (Join-Path $rootDir $b))) { throw ('REFUSING self-copy: OutDir equals native source bin ' + $b) }
  }

  # Exact managed source set: sorted, case-normalized relative-path comparison (not just count).
  $srcDir = Join-Path $here 'src'
  if (-not (Test-Path -LiteralPath $srcDir -PathType Container)) { throw ('src directory missing: ' + $srcDir) }

  # Root managed-source pins: expected hashes/bytes come ONLY from this pinned JSON, never recomputed here.
  $stage = 'managed-source-pins'
  if (-not [System.IO.Path]::IsPathRooted($ManagedSourcePinsPath)) { throw ('MANAGED_PINS: ManagedSourcePinsPath must be absolute: ' + $ManagedSourcePinsPath) }
  if (-not (Test-Path -LiteralPath $pinsFile -PathType Leaf)) { throw ('MANAGED_PINS: pins file missing: ' + $pinsFile) }
  Assert-NoReparse $pinsFile 'managed source pins file'
  if ((Get-Sha256Lower $pinsFile) -ne $ManagedSourcePinsSha256.ToLower()) { throw ('MANAGED_PINS: pins file sha mismatch: ' + $pinsFile) }
  $pins = Read-Utf8Strict $pinsFile | ConvertFrom-Json
  if ($pins.schema -ne 'VRSIM_MANAGED_SOURCE_PINS_V1') { throw ('MANAGED_PINS: bad pins schema: ' + $pins.schema) }
  if (-not $pins.candidateRoot -or -not [System.IO.Path]::IsPathRooted($pins.candidateRoot)) { throw ('MANAGED_PINS: pins candidateRoot not absolute: ' + $pins.candidateRoot) }
  if ((Get-FullPath $pins.candidateRoot) -ne (Get-FullPath $rootDir)) { throw 'MANAGED_PINS: pins candidateRoot differs from this script candidate root' }
  if ($pins.sourceRootRelative -ne 'vrsim-plugin/src') { throw ('MANAGED_PINS: pins sourceRootRelative must be exactly vrsim-plugin/src: ' + $pins.sourceRootRelative) }
  $pinsSrcRoot = Get-FullPath (Join-Path $pins.candidateRoot $pins.sourceRootRelative)
  if ($pinsSrcRoot -ne (Get-FullPath $srcDir)) { throw 'MANAGED_PINS: pins source root does not resolve to this script src dir' }
  $pinRows = @($pins.inputs)
  if ($pinRows.Count -ne $expectedSrc.Count) { throw ('MANAGED_PINS: pins inputs count=' + $pinRows.Count + ' (need exactly ' + $expectedSrc.Count + ')') }
  $expectedSrcLower = @($expectedSrc | ForEach-Object { Normalize-Rel $_ })
  $seenPinRel = @{}
  $pinsBoundary = $pinsSrcRoot.TrimEnd('\') + '\'
  foreach ($pin in $pinRows) {
    if (-not $pin.relative -or -not $pin.sha256 -or ($pin.bytes -isnot [Int64] -and $pin.bytes -isnot [Int32]) -or ($pin.bytes -lt 0)) { throw 'MANAGED_PINS: pin entry missing relative/sha256/bytes or bytes negative' }
    if ($pin.sha256 -notmatch '^[A-Fa-f0-9]{64}$') { throw ('MANAGED_PINS: pin sha256 not 64-hex: ' + $pin.relative) }
    $pr = ($pin.relative -replace '\\', '/')
    if ([System.IO.Path]::IsPathRooted($pin.relative) -or [System.IO.Path]::IsPathRooted($pr) -or $pr.Contains(':') -or $pr.Contains('?') -or $pr.Contains('*')) { throw ('MANAGED_PINS: pin relative must be plain relative (no rooted/drive/UNC/wildcard): ' + $pin.relative) }
    if ($pr.StartsWith('/') -or $pr.EndsWith('/') -or $pr.Contains('//') -or $pr -match '(^|/)(\.|\.\.)(/|$)') { throw ('MANAGED_PINS: pin relative not canonical or escapes src tree: ' + $pin.relative) }
    $prn = $pr.ToLower()
    if ($expectedSrcLower -notcontains $prn) { throw ('MANAGED_PINS: pin relative not in expected managed source set: ' + $pin.relative) }
    if ($seenPinRel.ContainsKey($prn)) { throw ('MANAGED_PINS: duplicate pin relative: ' + $pin.relative) }
    $seenPinRel[$prn] = $true
    $pinActual = Get-FullPath (Join-Path $pinsSrcRoot $pr)
    if (-not $pinActual.StartsWith($pinsBoundary, [System.StringComparison]::OrdinalIgnoreCase)) { throw ('MANAGED_PINS: pin path outside src separator boundary: ' + $pin.relative) }
    Assert-NoReparse $pinActual ('pinned source ' + $pin.relative)
    Assert-CurrentFile $pinActual $pin.sha256 $pin.bytes ('pinned source ' + $pin.relative)
  }
  $missingPins = @($expectedSrcLower | Where-Object { -not $seenPinRel.ContainsKey($_) })
  if ($missingPins.Count) { throw ('MANAGED_PINS: pins missing expected members: ' + ($missingPins -join ', ')) }

  $srcPrefixLen = (Get-FullPath $srcDir).Length + 1
  $foundAll = @(Get-ChildItem -LiteralPath $srcDir -Recurse -Filter '*.cs' -File | ForEach-Object { Normalize-Rel (Get-FullPath $_.FullName).Substring($srcPrefixLen) })
  $foundNorm = @($foundAll | Sort-Object)
  $expectedNorm = @($expectedSrc | ForEach-Object { Normalize-Rel $_ } | Sort-Object)
  if ($foundNorm.Count -ne ($foundNorm | Select-Object -Unique).Count) { throw 'SOURCE_SET: duplicate relative source paths under src' }
  $extra = @($foundNorm | Where-Object { $expectedNorm -notcontains $_ })
  $absent = @($expectedNorm | Where-Object { $foundNorm -notcontains $_ })
  if ($extra.Count -or $absent.Count) {
    throw ('SOURCE_SET: expected exactly 20 sources; unknown=[' + ($extra -join ', ') + '] missing=[' + ($absent -join ', ') + '] - refuse, sources are never deleted to pass')
  }

  $stage = 'native-manifest'
  if (-not (Test-Path -LiteralPath $NativeManifestPath -PathType Leaf)) { throw ('NATIVE_STALE: manifest missing: ' + $NativeManifestPath) }
  $nativeManifestSha256 = Get-Sha256Lower $NativeManifestPath
  $mf = Read-Utf8Strict $NativeManifestPath | ConvertFrom-Json
  if ($mf.schema -ne 'VRSIM_NATIVE_BUILD_MANIFEST_V1') { throw ('NATIVE_STALE: bad manifest schema: ' + $mf.schema) }
  Assert-SharedBuildProfile $mf 'manifest'
  $nativeManifestProfile = [ordered]@{ shared2048 = $mf.shared2048; sharedSourceEdge = $mf.sharedSourceEdge; sharedCompileSymbol = $mf.sharedCompileSymbol }
  if ($mf.compilePass -ne $true) { throw 'NATIVE_STALE: manifest compilePass not true' }
  if ($mf.sourceStable -ne $true) { throw 'NATIVE_STALE: manifest sourceStable not true' }
  if ($mf.PUnchanged -ne $true) { throw 'NATIVE_STALE: manifest PUnchanged not true' }
  if ($mf.configuration -ne 'Release') { throw ('NATIVE_STALE: manifest configuration not Release: ' + $mf.configuration) }
  if ($mf.platform -ne 'x64') { throw ('NATIVE_STALE: manifest platform not x64: ' + $mf.platform) }
  if ($mf.compileOnly -ne $true) { throw 'NATIVE_STALE: manifest compileOnly not true' }
  # compileOnly is a build-input gate only; promotionReady/candidateReady/installed are NOT build gates.
  if (-not $mf.sourceVersion) { throw 'NATIVE_STALE: manifest sourceVersion missing' }

  $candidateRoot = $mf.candidateRoot
  if (-not $candidateRoot -or -not [System.IO.Path]::IsPathRooted($candidateRoot)) { throw ('NATIVE_STALE: manifest candidateRoot not an absolute path: ' + $candidateRoot) }
  if (-not (Test-Path -LiteralPath $candidateRoot -PathType Container)) { throw ('NATIVE_STALE: manifest candidateRoot not a directory: ' + $candidateRoot) }
  if ((Get-FullPath $candidateRoot) -ne (Get-FullPath $rootDir)) { throw 'NATIVE_STALE: manifest candidateRoot differs from this script native root' }

  # native sourceVersion must equal the current native semanticVersion literal; no fallback to timestamps or old QA aliases.
  $nativeCpp = Get-FullPath (Join-Path $candidateRoot 'vrsim-native\VRSIMNative.cpp')
  if (-not (Test-Path -LiteralPath $nativeCpp -PathType Leaf)) { throw ('NATIVE_STALE: native source missing: ' + $nativeCpp) }
  $nvs = @([regex]::Matches((Read-Utf8Strict $nativeCpp), 'strcpy_s\(out->semanticVersion,\s*"([0-9.]+)"\)') | ForEach-Object { $_.Groups[1].Value })
  if ($nvs.Count -ne 1) { throw ('NATIVE_STALE: native semanticVersion literal count=' + $nvs.Count + ' (need exactly 1)') }
  if ($nvs[0] -ne $mf.sourceVersion) { throw ('NATIVE_STALE: native semanticVersion ' + $nvs[0] + ' != manifest sourceVersion ' + $mf.sourceVersion) }

  # inputs: exact fixed set of 22 relative paths (set comparison, not just count), unique,
  # canonical non-escaping relative inside candidateRoot boundary, source == derived, current SHA/bytes.
  $inputs = @($mf.inputs)
  if ($inputs.Count -ne $expectedNativeInputs.Count) { throw ('NATIVE_STALE: manifest inputs count=' + $inputs.Count + ' (need exactly ' + $expectedNativeInputs.Count + ')') }
  $rootBoundary = (Get-FullPath $candidateRoot).TrimEnd('\') + '\'
  $seenRel = @{}; $seenSrc = @{}
  foreach ($inp in $inputs) {
    if (-not $inp.relative -or -not $inp.source -or -not $inp.sha256 -or ($inp.bytes -isnot [Int64] -and $inp.bytes -isnot [Int32])) { throw 'NATIVE_STALE: manifest input entry missing relative/source/sha256/bytes' }
    $relSlash = ($inp.relative -replace '\\', '/')
    if ([System.IO.Path]::IsPathRooted($inp.relative) -or [System.IO.Path]::IsPathRooted($relSlash) -or $relSlash.Contains(':') -or $relSlash.Contains('?') -or $relSlash.Contains('*')) { throw ('NATIVE_STALE: manifest input relative must be plain relative (no rooted/drive/UNC/wildcard): ' + $inp.relative) }
    if ($relSlash.StartsWith('/') -or $relSlash.EndsWith('/') -or $relSlash.Contains('//') -or $relSlash -match '(^|/)(\.|\.\.)(/|$)') { throw ('NATIVE_STALE: manifest input relative not canonical or escapes candidate tree: ' + $inp.relative) }
    $nr = $relSlash.ToLower()
    if ($expectedNativeInputsLower -notcontains $nr) { throw ('NATIVE_STALE: manifest input relative not in fixed native input set: ' + $inp.relative) }
    if ($seenRel.ContainsKey($nr)) { throw ('NATIVE_STALE: duplicate manifest input relative: ' + $inp.relative) }
    $seenRel[$nr] = $true
    $derived = Get-FullPath (Join-Path $candidateRoot $inp.relative)
    if (-not $derived.StartsWith($rootBoundary, [System.StringComparison]::OrdinalIgnoreCase)) { throw ('NATIVE_STALE: manifest input derived path outside candidateRoot boundary: ' + $inp.relative) }
    if ((Get-FullPath $inp.source) -ne $derived) { throw ('NATIVE_STALE: manifest input source not candidateRoot+relative (native root moved or source drifted): ' + $inp.relative) }
    if ($seenSrc.ContainsKey($derived.ToLower())) { throw ('NATIVE_STALE: duplicate manifest input source: ' + $inp.source) }
    $seenSrc[$derived.ToLower()] = $true
    Assert-CurrentFile $inp.source $inp.sha256 $inp.bytes ('manifest input ' + $inp.relative)
  }
  $missingInputs = @($expectedNativeInputsLower | Where-Object { -not $seenRel.ContainsKey($_) })
  if ($missingInputs.Count) { throw ('NATIVE_STALE: manifest inputs missing fixed-set members: ' + ($missingInputs -join ', ')) }

  # tools: exactly 4 {path,bytes,sha256}, current existence/bytes/SHA at path; no exe/name synthesis.
  $tools = @($mf.tools)
  if ($tools.Count -ne 4) { throw ('NATIVE_STALE: manifest tools count=' + $tools.Count + ' (need exactly 4)') }
  foreach ($t in $tools) {
    if (-not $t.path -or -not $t.sha256 -or ($t.bytes -isnot [Int64] -and $t.bytes -isnot [Int32])) { throw 'NATIVE_STALE: manifest tool entry missing path/bytes/sha256' }
    Assert-CurrentFile $t.path $t.sha256 $t.bytes ('manifest tool ' + $t.path)
  }

  # generatedBytecodeHeaders: exactly 2, basenames GpuProjectionVS.inc/GpuProjectionPS.inc, unique, current facts.
  $gbh = @($mf.generatedBytecodeHeaders)
  if ($gbh.Count -ne 2) { throw ('NATIVE_STALE: generatedBytecodeHeaders count=' + $gbh.Count + ' (need exactly 2)') }
  $gbhNames = @($gbh | ForEach-Object { [System.IO.Path]::GetFileName($_.path).ToLower() })
  if (($gbhNames | Select-Object -Unique).Count -ne 2) { throw 'NATIVE_STALE: generatedBytecodeHeaders basenames duplicated' }
  if (-not (($gbhNames -contains 'gpuprojectionvs.inc') -and ($gbhNames -contains 'gpuprojectionps.inc'))) { throw ('NATIVE_STALE: generatedBytecodeHeaders basenames wrong: ' + ($gbhNames -join ', ')) }
  foreach ($h in $gbh) { Assert-CurrentFile $h.path $h.sha256 $h.bytes ('manifest generated header ' + $h.path) }

  # compileReceipt: file + SHA verified, then parsed output facts must equal manifest artifacts.
  if (-not $mf.compileReceipt -or -not $mf.compileReceipt.path -or -not $mf.compileReceipt.sha256) { throw 'NATIVE_STALE: manifest compileReceipt incomplete' }
  if (-not (Test-Path -LiteralPath $mf.compileReceipt.path -PathType Leaf)) { throw ('NATIVE_STALE: compile receipt missing: ' + $mf.compileReceipt.path) }
  if ((Get-Sha256Lower $mf.compileReceipt.path) -ne $mf.compileReceipt.sha256.ToLower()) { throw ('NATIVE_STALE: compile receipt sha mismatch: ' + $mf.compileReceipt.path) }
  $cr = Read-Utf8Strict $mf.compileReceipt.path | ConvertFrom-Json
  if ($cr.compilerPass -ne $true) { throw 'NATIVE_STALE: compile receipt compilerPass not true' }
  if ($cr.sourceUnchanged -ne $true) { throw 'NATIVE_STALE: compile receipt sourceUnchanged not true' }
  if ($cr.productUnchanged -ne $true) { throw 'NATIVE_STALE: compile receipt productUnchanged not true' }
  Assert-SharedBuildProfile $cr 'compile receipt'
  $nativeCompileReceiptProfile = [ordered]@{ shared2048 = $cr.shared2048; sharedSourceEdge = $cr.sharedSourceEdge; sharedCompileSymbol = $cr.sharedCompileSymbol }
  $arts = @($mf.artifacts)
  if ($arts.Count -ne 3) { throw ('NATIVE_STALE: manifest artifacts count=' + $arts.Count + ' (need exactly 3)') }
  $artByName = @{}
  foreach ($a in $arts) {
    if (-not $a.name -or -not $expectedNativeRel.ContainsKey($a.name)) { throw ('NATIVE_STALE: unexpected artifact name: ' + $a.name) }
    if ($artByName.ContainsKey($a.name)) { throw ('NATIVE_STALE: duplicate artifact name: ' + $a.name) }
    $artByName[$a.name] = $a
  }
  if ($artByName.Count -ne 3) { throw 'NATIVE_STALE: artifacts must cover the 3 official names once each' }
  $crOut = @($cr.outputs)
  if ($crOut.Count -ne 3) { throw ('NATIVE_STALE: compile receipt outputs count=' + $crOut.Count + ' (need exactly 3)') }
  $covered = @{}
  foreach ($o in $crOut) {
    if ($o.compilerExit -ne 0) { throw ('NATIVE_STALE: compile receipt compilerExit not 0 for ' + $o.dll.path) }
    if ($o.x64 -ne $true) { throw ('NATIVE_STALE: compile receipt x64 not true for ' + $o.dll.path) }
    if (-not $o.dll -or -not $o.dll.path -or -not $o.dll.sha256) { throw 'NATIVE_STALE: compile receipt output dll incomplete' }
    $bn = [System.IO.Path]::GetFileName($o.dll.path)
    if (-not $artByName.ContainsKey($bn)) { throw ('NATIVE_STALE: compile receipt output not a manifest artifact: ' + $bn) }
    $a = $artByName[$bn]
    if ((Get-FullPath $o.dll.path) -ne (Get-FullPath $a.source)) { throw ('NATIVE_STALE: compile receipt dll.path != manifest artifact source for ' + $bn) }
    if ($o.dll.sha256.ToLower() -ne $a.sha256.ToLower()) { throw ('NATIVE_STALE: compile receipt dll sha != manifest artifact sha for ' + $bn) }
    if ($o.dll.bytes -ne $a.bytes) { throw ('NATIVE_STALE: compile receipt dll bytes != manifest artifact bytes for ' + $bn) }
    $covered[$bn] = $true
  }
  if ($covered.Count -ne 3) { throw 'NATIVE_STALE: compile receipt outputs do not cover all 3 manifest artifacts' }

  # artifacts: both the QA compile source and the official candidate bin must exist with the artifact SHA/bytes.
  foreach ($a in $arts) {
    if (-not $a.intendedCandidateRelative) { throw ('NATIVE_STALE: artifact missing intendedCandidateRelative: ' + $a.name) }
    $icrSlash = ($a.intendedCandidateRelative -replace '\\', '/')
    if ([System.IO.Path]::IsPathRooted($a.intendedCandidateRelative) -or [System.IO.Path]::IsPathRooted($icrSlash) -or $icrSlash.Contains(':') -or $icrSlash.Contains('?') -or $icrSlash.Contains('*')) { throw ('NATIVE_STALE: artifact intendedCandidateRelative must be plain relative (no rooted/drive/UNC/wildcard): ' + $a.intendedCandidateRelative) }
    if ($icrSlash.StartsWith('/') -or $icrSlash.EndsWith('/') -or $icrSlash.Contains('//') -or $icrSlash -match '(^|/)(\.|\.\.)(/|$)') { throw ('NATIVE_STALE: artifact intendedCandidateRelative not canonical or escapes candidate tree: ' + $a.intendedCandidateRelative) }
    if ((Normalize-Rel $a.intendedCandidateRelative) -ne (Normalize-Rel $expectedNativeRel[$a.name])) { throw ('NATIVE_STALE: artifact intendedCandidateRelative wrong for ' + $a.name + ': ' + $a.intendedCandidateRelative) }
    if (-not $a.source -or -not [System.IO.Path]::IsPathRooted($a.source)) { throw ('NATIVE_STALE: artifact source not absolute for ' + $a.name) }
    Assert-CurrentFile $a.source $a.sha256 $a.bytes ('QA compile-output ' + $a.name)
    $candPath = Get-FullPath (Join-Path $candidateRoot $a.intendedCandidateRelative)
    if (-not $candPath.StartsWith($rootBoundary, [System.StringComparison]::OrdinalIgnoreCase)) { throw ('NATIVE_STALE: artifact candidate path outside candidateRoot boundary: ' + $a.name) }
    Assert-CurrentFile $candPath $a.sha256 $a.bytes ('candidate official bin ' + $a.name)
  }

  # diagnostic only: file-date staleness must not gate copies (manifest/hash gates above are authoritative).
  try {
    foreach ($a in $arts) {
      $qaPath = Get-FullPath (Join-Path $candidateRoot $a.intendedCandidateRelative)
      if ((Get-Item -LiteralPath $qaPath).LastWriteTimeUtc -lt (Get-Item -LiteralPath $nativeCpp).LastWriteTimeUtc) {
        Write-Output ('DIAG date-staleness (informational only): ' + $a.name + ' older than VRSIMNative.cpp by file time')
      }
    }
  } catch { }

  $stage = 'compile'
  Write-Output ('SIM  ' + $SimRoot)
  Write-Output ('CSC  ' + $Csc)
  Write-Output ('OUT  ' + $OutDir)
  Write-Output ('NATIVE-MANIFEST  ' + $NativeManifestPath)
  New-Item -ItemType Directory -Path $OutDir | Out-Null

  $srcFiles = @($expectedSrc | ForEach-Object { Get-FullPath (Join-Path $srcDir (($_ -replace '/', [IO.Path]::DirectorySeparatorChar))) })
  # sourceFilesJson is built after the snapshot, from Root-pinned expected values verified against current reads.

  # net48 reference preflight: each assembly must exist at its explicit path before csc runs.
  $net48Refs = @(
    (Join-Path $WpfDir 'System.dll'), (Join-Path $WpfDir 'System.Core.dll'), (Join-Path $WpfDir 'System.Xaml.dll'),
    (Join-Path $WpfDir 'Facades\System.Runtime.dll'), (Join-Path $WpfDir 'System.ComponentModel.Composition.dll'),
    (Join-Path $WpfDir 'PresentationCore.dll'), (Join-Path $WpfDir 'PresentationFramework.dll'), (Join-Path $WpfDir 'WindowsBase.dll'))
  foreach ($r in $net48Refs) {
    if (-not (Test-Path -LiteralPath $r -PathType Leaf)) { throw ('REFERENCE_MISSING: net48 reference assembly not found: ' + $r) }
  }
  $wpfRefArgs = @(
    ('/reference:' + (Join-Path $WpfDir 'System.dll')),
    ('/reference:' + (Join-Path $WpfDir 'System.Core.dll')),
    ('/reference:' + (Join-Path $WpfDir 'System.Xaml.dll')),
    ('/reference:' + (Join-Path $WpfDir 'Facades\System.Runtime.dll')),
    ('/reference:' + (Join-Path $WpfDir 'System.ComponentModel.Composition.dll')),
    ('/reference:' + (Join-Path $WpfDir 'PresentationFramework.dll')),
    ('/reference:' + (Join-Path $WpfDir 'PresentationCore.dll')),
    ('/reference:' + (Join-Path $WpfDir 'WindowsBase.dll')))

  # B2: freeze every real build input before csc; earlier-verified expected values (pins/manifest) are
  # re-checked against current reads and never replaced by late actuals. No-expected inputs freeze as-is.
  $stage = 'input-snapshot'
  $script:capturedInputs = [ordered]@{}
  Add-CapturedInput $buildScriptPath 'this build script' $buildScriptSha $buildScriptBytes
  Add-CapturedInput $pinsFile 'root managed source pins' $ManagedSourcePinsSha256 $null
  $sourceFilesJson = @()
  for ($i = 0; $i -lt $expectedSrc.Count; $i++) {
    $relN = Normalize-Rel $expectedSrc[$i]
    $pinRow = @($pinRows | Where-Object { (Normalize-Rel $_.relative) -eq $relN })
    if ($pinRow.Count -ne 1) { throw ('BUILD_INPUT_STALE: root pins rows matching ' + $expectedSrc[$i] + ' count=' + $pinRow.Count) }
    Add-CapturedInput $srcFiles[$i] ('managed source ' + $expectedSrc[$i]) $pinRow[0].sha256 $pinRow[0].bytes
    $sourceFilesJson += [ordered]@{ relative = $expectedSrc[$i]; path = $srcFiles[$i]; sha256 = $pinRow[0].sha256.ToLower(); bytes = $pinRow[0].bytes }
  }
  Add-CapturedInput $NativeManifestPath 'native build manifest' $nativeManifestSha256 $null
  foreach ($inp in $inputs) { Add-CapturedInput $inp.source ('manifest input ' + $inp.relative) $inp.sha256 $inp.bytes }
  foreach ($t in $tools) { Add-CapturedInput $t.path ('manifest tool ' + $t.path) $t.sha256 $t.bytes }
  foreach ($h in $gbh) { Add-CapturedInput $h.path ('generated bytecode header ' + $h.path) $h.sha256 $h.bytes }
  Add-CapturedInput $mf.compileReceipt.path 'native compile receipt' $mf.compileReceipt.sha256 $mf.compileReceipt.bytes
  foreach ($a in $arts) { Add-CapturedInput $a.source ('artifact QA compile source ' + $a.name) $a.sha256 $a.bytes }
  foreach ($a in $arts) { Add-CapturedInput (Get-FullPath (Join-Path $candidateRoot $a.intendedCandidateRelative)) ('candidate official bin ' + $a.name) $a.sha256 $a.bytes }
  Add-CapturedInput $Csc 'csc.exe'
  foreach ($r in $net48Refs) { Add-CapturedInput $r 'net48 reference assembly' }
  foreach ($r in $required) { Add-CapturedInput (Join-Path $SimRoot $r) ('sim SDK reference ' + $r) }
  $stage = 'compile'

  $cscArguments = @(
    '/nologo', '/target:library', '/platform:x64', '/deterministic', '/optimize+', '/unsafe', '/langversion:7.3',
    ('/out:' + $OutDir + '\UX.VRSIM.Sim.Plugin.dll'))
  $cscArguments += $wpfRefArgs
  $cscArguments += ('/reference:' + $SimRoot + '\Create3D.Shared.dll')
  $cscArguments += ('/reference:' + $SimRoot + '\Caliburn.Micro.dll')
  $cscArguments += ('/reference:' + $SimRoot + '\UX.Shared.dll')
  $cscArguments += ('/reference:' + $SimRoot + '\UX.Shared.Infrastructure.dll')
  $cscDefines = @()
  if ($requestedShared2048) {
    $cscArguments += '/define:VRSIM_SHARED_2048'
    $cscDefines = @('VRSIM_SHARED_2048')
  }
  $cscArguments += $srcFiles
  & $Csc @cscArguments
  if ($LASTEXITCODE -ne 0) { throw ('csc exit ' + $LASTEXITCODE) }
  $cscExit = $LASTEXITCODE
  Assert-CapturedInputs 'post-csc'

  $stage = 'native-copy'
  # native backends ride with the plugin (proven pattern: FingerprintHelper.dll in OV11 KOP),
  # copied from the manifest-declared official candidate bin paths, re-hashed after copy.
  $artifactRows = @([ordered]@{
      name = 'UX.VRSIM.Sim.Plugin.dll'; path = (Get-FullPath (Join-Path $OutDir 'UX.VRSIM.Sim.Plugin.dll'))
      sha256 = (Get-Sha256Lower (Join-Path $OutDir 'UX.VRSIM.Sim.Plugin.dll')); bytes = (Get-Item -LiteralPath (Join-Path $OutDir 'UX.VRSIM.Sim.Plugin.dll')).Length })
  foreach ($a in $arts) {
    $candPath = Get-FullPath (Join-Path $candidateRoot $a.intendedCandidateRelative)
    $dest = Get-FullPath (Join-Path $OutDir $a.name)
    Copy-Item -LiteralPath $candPath -Destination $dest -Force
    $after = Get-Sha256Lower $dest
    $afterBytes = (Get-Item -LiteralPath $dest).Length
    if ($after -ne $a.sha256.ToLower()) { throw ('post-copy sha mismatch for ' + $a.name + ': ' + $after + ' != ' + $a.sha256.ToLower()) }
    if ($afterBytes -ne $a.bytes) { throw ('post-copy bytes mismatch for ' + $a.name + ': ' + $afterBytes + ' != ' + $a.bytes) }
    $artifactRows += [ordered]@{ name = $a.name; path = $dest; sha256 = $after; bytes = $afterBytes }
  }

  # all four outputs staged: managed must equal its just-compiled row, natives their manifest facts, then full input recheck.
  $stage = 'output-stability'
  foreach ($row in $artifactRows) {
    Assert-NoReparse $row.path ('output ' + $row.name)
    if (-not (Test-Path -LiteralPath $row.path -PathType Leaf)) { throw ('BUILD_INPUT_STALE: output vanished before receipt: ' + $row.path) }
    $nowSha = Get-Sha256Lower $row.path
    $nowBytes = (Get-Item -LiteralPath $row.path).Length
    if ($nowSha -ne $row.sha256 -or $nowBytes -ne $row.bytes) { throw ('BUILD_INPUT_STALE: output drifted before receipt: ' + $row.name + ' current=' + $nowSha + '/' + $nowBytes + ' frozen=' + $row.sha256 + '/' + $row.bytes) }
  }
  Assert-CapturedInputs 'pre-receipt'

  $stage = 'receipt'
  $receipt = [ordered]@{
    schema = 'VRSIM_PRODUCT_BUILD_V1'
    utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ')
    success = $true
    sourceFiles = $sourceFilesJson
    nativeManifestPath = $NativeManifestPath
    nativeManifestSha256 = $nativeManifestSha256
    managedSourcePinsPath = $pinsFile
    managedSourcePinsSha256 = $ManagedSourcePinsSha256.ToLower()
    sourceVersion = $mf.sourceVersion
    artifacts = $artifactRows
    configuration = 'Release'
    platform = 'x64'
    optimization = $true
    qaDefine = $false
    shared2048 = $requestedShared2048
    sharedSourceEdge = $requestedSharedEdge
    sharedCompileSymbol = $requestedSharedSymbol
    cscArguments = $cscArguments
    cscDefines = $cscDefines
    nativeManifestProfile = $nativeManifestProfile
    nativeCompileReceiptProfile = $nativeCompileReceiptProfile
    installed = $false
    candidateReady = $false
    promotionReady = $false
    cscExit = $cscExit
    buildScriptSha = $buildScriptSha
    inputSnapshotCount = $script:capturedInputs.Count
    inputChecks = @('frozen-before-csc', 'post-csc', 'pre-receipt')
    inputStable = $true
    sourceStable = $true
    capturedInputs = @($script:capturedInputs.Values)
  }
  $receipt | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $receiptPath -Encoding UTF8

  foreach ($d in (Get-ChildItem -LiteralPath $OutDir -Filter '*.dll' -File)) {
    Write-Output ('ARTIFACT ' + $d.Name + ' sha256=' + (Get-FileHash -LiteralPath $d.FullName -Algorithm SHA256).Hash.ToLower() + ' size=' + $d.Length)
  }
  Write-Output ('RECEIPT ' + $receiptPath)
} catch {
  # Never leave a stale success=true receipt: the pre-round invalidation above already voided
  # any old one; if OutDir exists, record this round as an explicit failure receipt.
  if (Test-Path -LiteralPath $OutDir -PathType Container) {
    try {
      $fail = [ordered]@{
        schema = 'VRSIM_PRODUCT_BUILD_V1'
        utc = (Get-Date).ToUniversalTime().ToString('yyyy-MM-ddTHH:mm:ss.fffffffZ')
        success = $false
        stage = $stage
        error = $_.Exception.Message
        configuration = 'Release'; platform = 'x64'; optimization = $true; qaDefine = $false
        shared2048 = $requestedShared2048
        sharedSourceEdge = $requestedSharedEdge
        sharedCompileSymbol = $requestedSharedSymbol
        cscArguments = $cscArguments
        cscDefines = $cscDefines
        nativeManifestProfile = $nativeManifestProfile
        nativeCompileReceiptProfile = $nativeCompileReceiptProfile
        installed = $false; candidateReady = $false; promotionReady = $false
      }
      $fail | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath $receiptPath -Encoding UTF8
    } catch { }
  }
  throw
}
