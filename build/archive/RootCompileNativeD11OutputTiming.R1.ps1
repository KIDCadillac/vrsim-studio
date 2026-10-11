# Root-only native offline build adapter. Source admission is mandatory; no SDK, DLL load, managed CSC, shader compiler or hardware.
param(
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedScriptSha,
 [Parameter(Mandatory)][string]$SourceAdmissionPath,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$ExpectedSourceAdmissionSha,
 [Parameter(Mandatory)][ValidatePattern('^[A-Fa-f0-9]{64}$')][string]$SourceSha,
 [string]$ExpectedSourceAdmissionSchema='NATIVE_D11_OUTPUT_TIMING_SOURCE_ADMISSION_V1',
 [string]$BuildRoot='C:\Users\KID\Documents\ChatGPT\项目经理\reviews\vrsim-highres-h2\d11-lease-output-gpu-timing-build-r1\compile-only-r1',
 [switch]$RootCompileReviewed
)
$ErrorActionPreference='Stop'
$taskRoot='C:\Users\KID\Documents\ChatGPT\项目经理\reviews\vrsim-highres-h2'
$taskCandidate=Join-Path $taskRoot 'qa-d11-lease-output-gpu-timing-candidate-run1'
$taskClone=Join-Path $taskCandidate 'BYTE_COPY_BASELINE.R1.json'
$taskManifest=Join-Path $taskRoot 'native-lease-output-profiler-build-prep-r1\EA5_NATIVE_BASELINE_COMPILE_MANIFEST.R1.json'
$taskBaseSha='CD673F901601FB5A7B37ED6C889657DDA985B32D31153CE783DBEAD122E1BB67'
$taskCloneSha='B3476AC6D3275E73E28DAAD078FA9328ADC24AC59360863B34DB678C9BE2B97D'
$taskManifestSha='2A348AE19BBACE99AB366AA17A058A32DADCD032B64F806C4FD3BFFFCD45F473'
function TaskNoReparse([string]$p){
 for($a=[IO.Path]::GetFullPath($p);$a;$a=[IO.Path]::GetDirectoryName($a)){
  if((Test-Path -LiteralPath $a)-and(([IO.File]::GetAttributes($a)-band[IO.FileAttributes]::ReparsePoint)-ne0)){throw ('Reparse refused: '+$a)}
 }
}
function TaskSha([string]$p){TaskNoReparse $p;(Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash}
function TaskPin([string]$p,[string]$sha){if((TaskSha $p)-cne$sha.ToUpperInvariant()){throw ('SHA differs: '+$p)}}
function TaskNewUtf8([string]$p,[string]$text){
 $s=[IO.File]::Open($p,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read);$w=$null
 try{$w=[IO.StreamWriter]::new($s,[Text.UTF8Encoding]::new($false));$w.Write($text);$w.Flush()}
 finally{if($w){$w.Dispose()}else{$s.Dispose()}}
}
function TaskFile([string]$p){[ordered]@{path=$p;sha256=TaskSha $p;bytes=(Get-Item -LiteralPath $p).Length}}
function TaskExportNames([string]$log) {
 $names=@([IO.File]::ReadAllLines($log)|ForEach-Object {
  if($_-match'^\s*\d+\s+[0-9A-Fa-f]+\s+[0-9A-Fa-f]{8,16}\s+(VRSIM_[A-Za-z0-9_]+)\s*$'){$Matches[1]}
 })
 if($names.Count-eq0-or@($names|Select-Object -Unique).Count-ne$names.Count){throw 'No unique plain VRSIM exports in dumpbin output'}
 @($names|Sort-Object)
}
function TaskImportNames([string]$log) {
 $dll=$null;$names=@();$parsed=0
 foreach($line in [IO.File]::ReadAllLines($log)){
  if($line-match'^\s*Summary\s*$'){break}
  if($line-match'^\s+([A-Za-z0-9_.-]+\.dll)\s*$'){$dll=$Matches[1].ToLowerInvariant();continue}
  if($null-eq$dll-or[string]::IsNullOrWhiteSpace($line)){continue}
  if($line-match'^\s*[0-9A-Fa-f]+\s+(?:Import (?:Address|Name) Table|time date stamp|Index of first forwarder reference).*$'){continue}
  if($line-match'^\s+Ordinal\s+([0-9]+)\s*$'){$names+=($dll+'!ordinal:'+$Matches[1]);$parsed++;continue}
  if($line-match'^\s+[0-9A-Fa-f]+\s+([A-Za-z_?$@][^\s]*)\s*$'){$names+=($dll+'!'+$Matches[1]);$parsed++;continue}
  if($line-match'^\s*Section contains the following (?:delay load )?imports:\s*$'){$dll=$null;continue}
  throw ('Unknown import table line; retain raw log: '+$line)
 }
 if($parsed-eq0){throw 'No recognized static imports; unknown cannot pass'}
 @($names|Sort-Object -Unique)
}
function TaskCall([string]$exe,[string[]]$argv,[string]$variant,[string]$kind){
 foreach($arg in $argv){if($arg-isnot[string]){throw 'Non-string tool argument refused'}}
 $index=$script:taskCalls.Count+1;$stem=('{0:D2}-{1}-{2}'-f$index,$variant,$kind)
 $out=Join-Path $BuildRoot ($stem+'.stdout.raw');$err=Join-Path $BuildRoot ($stem+'.stderr.raw')
 $os=[IO.File]::Open($out,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 $es=[IO.File]::Open($err,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
 $proc=[Diagnostics.Process]::new();$proc.StartInfo=[Diagnostics.ProcessStartInfo]::new($exe)
 $proc.StartInfo.WorkingDirectory=$BuildRoot;$proc.StartInfo.UseShellExecute=$false;$proc.StartInfo.CreateNoWindow=$true
 $proc.StartInfo.RedirectStandardOutput=$true;$proc.StartInfo.RedirectStandardError=$true
 foreach($arg in $argv){[void]$proc.StartInfo.ArgumentList.Add($arg)}
 $start=[DateTime]::UtcNow;$pidValue=$null;$exitValue=$null;$timeout=$false;$errorValue=$null
 try{
  if(!$proc.Start()){throw 'Native offline tool failed to start'};$pidValue=$proc.Id
  $ot=$proc.StandardOutput.BaseStream.CopyToAsync($os);$et=$proc.StandardError.BaseStream.CopyToAsync($es)
  if(!$proc.WaitForExit(180000)){$timeout=$true;$proc.Kill($true);$proc.WaitForExit()}
  [void]$ot.GetAwaiter().GetResult();[void]$et.GetAwaiter().GetResult();$exitValue=$proc.ExitCode
 }catch{$errorValue=$_.Exception.ToString()}
 finally{$os.Dispose();$es.Dispose();$proc.Dispose()}
 $row=[ordered]@{variant=$variant;kind=$kind;executable=$exe;arguments=@($argv);argumentCount=$argv.Count;actualMacros=@($argv|Where-Object{$_-like'/D*'});startedUtc=$start.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o');actualPid=$pidValue;exitCode=$exitValue;timedOut=$timeout;error=$errorValue;stdout=(TaskFile $out);stderr=(TaskFile $err)}
 $script:taskCalls+=$row
 if($timeout-or$errorValue-or$exitValue-ne0){throw ('Actual offline native tool failed: '+$variant+'/'+$kind)}
 $row
}
function TaskProjection([string]$path){
 # This is a comparison view only. Raw .EP stdout remains untouched and independently pinned.
 if((Get-Item -LiteralPath $path).Length-gt67108864){throw 'DEFAULT preprocessing output exceeds bounded 64MiB reader'}
 $lines=[Collections.Generic.List[string]]::new();$timestamps=[Collections.Generic.List[string]]::new()
 $reader=[IO.StreamReader]::new($path,[Text.UTF8Encoding]::new($false,$true),$true)
 try{
  while($null-ne($line=$reader.ReadLine())){
   $t=$line.Trim();if(!$t){continue}
   if($t-match'^strcpy_s\s*\(\s*out\s*->\s*buildTimestamp\s*,\s*"(?:[^"\\]|\\.)*"\s*\)\s*;\s*$'){
    $timestamps.Add($line);$t='strcpy_s(out->buildTimestamp, <RECORDED_REAL_SOURCE_TIMESTAMP_METADATA>);'
   }
   $lines.Add($t)
  }
 }finally{$reader.Dispose()}
 if($timestamps.Count-ne1){throw 'Exactly one original source buildTimestamp statement required; unknown cannot project'}
 [ordered]@{lines=$lines.ToArray();timestampRawLines=$timestamps.ToArray();comparison='Nonempty trimmed preprocessed lines; only exact original buildTimestamp assignment maps to a marker. Raw output and original timestamp values remain retained.'}
}
if(!$RootCompileReviewed){throw 'Disabled: Root must review actual terminal/current source admission and this compile adapter'}
if([IO.Path]::GetFullPath($PSScriptRoot)-cne$taskRoot){throw 'Fixed Root native build adapter required'}
TaskPin $PSCommandPath $ExpectedScriptSha;TaskPin $taskManifest $taskManifestSha;TaskPin $taskClone $taskCloneSha
foreach($p in @($SourceAdmissionPath,$BuildRoot)){
 TaskNoReparse $p
 if(![IO.Path]::IsPathFullyQualified($p)-or![IO.Path]::GetFullPath($p).StartsWith($taskRoot+'\',[StringComparison]::Ordinal)){throw 'Absolute new H2 build/review paths required'}
}
$BuildRoot=[IO.Path]::GetFullPath($BuildRoot)
if(Test-Path -LiteralPath $BuildRoot){throw 'Fresh CreateNew native build directory required'}
TaskPin $SourceAdmissionPath $ExpectedSourceAdmissionSha
$m=Get-Content -LiteralPath $taskManifest -Raw|ConvertFrom-Json -AsHashtable -DateKind String
$c=Get-Content -LiteralPath $taskClone -Raw|ConvertFrom-Json -AsHashtable -DateKind String
$reviewRaw=[IO.File]::ReadAllText($SourceAdmissionPath)
$a=$reviewRaw|ConvertFrom-Json -AsHashtable -DateKind String
if($a.schema-cne$ExpectedSourceAdmissionSchema){throw 'Exact fresh native source admission schema required'}
foreach($key in @('pass','buildAdmission','sourceOnly','soleWritableNativeOnly','other22ByteExact','scopeExact','forwardByteExact','inverseByteExact')){
 if(!$a.ContainsKey($key)-or$a[$key]-isnot[bool]-or!$a[$key]){throw ('Actual source admission typed true required: '+$key)}
}
foreach($key in @('runtimeAdmission','readyForRun','compilerExecuted','hardwareExecuted','SDKInvoked','productSourceModifiedByReviewer','runtimeAllowed','actualPerformanceProven')){
 if(!$a.ContainsKey($key)-or$a[$key]-isnot[bool]-or$a[$key]){throw ('Source-only typed false required: '+$key)}
}
$SourceSha=$SourceSha.ToUpperInvariant()
if($SourceSha-ceq$taskBaseSha-or$a.finalEditedSourceSha256-cne$SourceSha-or$a.actualSuccessfulEdits-lt1){throw 'Current actually edited sole native SHA and raw successful edit fact required'}
if($a.sourceCount-ne23-or@($a.sourcePins).Count-ne23-or@($a.sourceTransforms).Count-ne1){throw 'Current 23 pins and sole source transform required'}
$x=$a.sourceTransforms[0]
if($x.relative-cne'native\VRSIMNative.cpp'-or$x.baselineSha256-cne$taskBaseSha-or$x.finalSha256-cne$SourceSha-or$x.inverseSha256-cne$taskBaseSha-or$x.forwardByteExact-isnot[bool]-or!$x.forwardByteExact-or$x.inverseByteExact-isnot[bool]-or!$x.inverseByteExact){throw 'Actual source forward/inverse evidence differs'}
foreach($sha in @($taskCloneSha,$taskBaseSha,$SourceSha)){if($reviewRaw-notmatch[regex]::Escape($sha)){throw 'Fresh source admission must bind clone/baseline/current source'}}
if($m.sourceCount-ne23-or$c.sourceCount-ne23-or$c.candidateRoot-cne$taskCandidate){throw 'Original native clone shape differs'}
$taskSources=@()
foreach($old in $m.sourcePins){
 $path=Join-Path $taskCandidate $old.relative
 $expected=if($old.relative-ceq'native\VRSIMNative.cpp'){$SourceSha}else{$old.sha256}
 $pins=@($a.sourcePins|Where-Object{$_.relative-ceq$old.relative})
 if($pins.Count-ne1-or$pins[0].path-cne$path-or$pins[0].sha256-cne$expected){throw ('Fresh source pin mismatch: '+$old.relative)}
 TaskPin $path $expected
 if((Get-Item -LiteralPath $path).Length-ne$pins[0].bytes){throw 'Source review byte length mismatch'}
 if($old.relative-cne'native\VRSIMNative.cpp'-and(!(Get-Item -LiteralPath $path).IsReadOnly-or$pins[0].bytes-ne$old.bytes)){throw 'Other22 source protections/bytes changed'}
 TaskPin $old.path $old.sha256
 $taskSources+=([ordered]@{relative=$old.relative;path=$path;sha256=$expected;bytes=$pins[0].bytes;baselinePath=$old.path;baselineSha256=$old.sha256})
}
$actualNativeFiles=@(Get-ChildItem -LiteralPath $taskCandidate -Recurse -File -Force|Where-Object{$_.Extension.ToLowerInvariant()-in@('.cpp','.h','.hpp','.inc','.hlsl')})
if($actualNativeFiles.Count-ne23-or@($actualNativeFiles|Where-Object{$_.FullName-cnotin$taskSources.path}).Count){throw 'New or missing native/vendor/shader file refused'}
TaskPin $m.actualBuildReceipt.path $m.actualBuildReceipt.sha256
$r=Get-Content -LiteralPath $m.actualBuildReceipt.path -Raw|ConvertFrom-Json -AsHashtable -DateKind String
if($r.schema-cne'ROOT_OUTPUT4096_OFFLINE_COMPILE_ONLY_V1'-or!$r.pass-or!$r.pinsStable-or$null-ne$r.failure){throw 'Original actual670F compiler receipt required'}
TaskPin $m.originalCompileScript.path $m.originalCompileScript.sha256;TaskPin $m.originalRecipe.path $m.originalRecipe.sha256
TaskPin $m.currentRuntimeDll.path $m.currentRuntimeDll.sha256
foreach($pin in @($m.nativeTools)+@($m.nativeLibraries)){TaskPin $pin.path $pin.sha256}
foreach($dir in @($m.originalIncludeDirectories)+@($m.originalLibraryDirectories)){TaskNoReparse $dir;if(!(Test-Path -LiteralPath $dir -PathType Container)){throw 'Original tool include/lib directory missing'}}
foreach($key in @('CL','_CL_','LINK','_LINK_','INCLUDE','LIB')){if([Environment]::GetEnvironmentVariable($key,'Process')){throw ('Implicit compiler options refused: '+$key)}}
foreach($row in $r.calls|Where-Object{$_.kind-in@('PROJECTION_COMPILE','BRIDGE_COMPILE','NATIVE_COMPILE_LINK','EXPORTS','IMPORTS')-and$_.variant-in@('DEFAULT','ORIGINAL_ON','D9_TIMING_ON','QA_OUTPUT4096')}){TaskPin $row.log $row.logSha}
$taskCl=@($m.nativeTools|Where-Object{[IO.Path]::GetFileName($_.path)-ceq'cl.exe'})[0].path
$taskDumpbin=@($m.nativeTools|Where-Object{[IO.Path]::GetFileName($_.path)-ceq'dumpbin.exe'})[0].path
$taskCalls=@();$taskOutputs=@();$taskFrozen=@();$taskVariants=@();$taskEP=$null;$taskFailure=$null;$taskStable=$false;$taskSavedPath=$env:PATH
$taskFrozenRoot=Join-Path $BuildRoot 'frozen-source\native-candidate'
$taskStarted=[DateTime]::UtcNow
[void][IO.Directory]::CreateDirectory($BuildRoot)
try{
 foreach($row in $taskSources){
  $dest=Join-Path $taskFrozenRoot $row.relative
  [void][IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($dest));[IO.File]::Copy($row.path,$dest,$false)
  TaskPin $dest $row.sha256;(Get-Item -LiteralPath $dest).IsReadOnly=$true
  $taskFrozen+=([ordered]@{relative=$row.relative;path=$dest;source=$row.path;sha256=$row.sha256;bytes=$row.bytes})
 }
 $env:PATH=[IO.Path]::GetDirectoryName($taskCl)+';'+$taskSavedPath
 foreach($variant in @('DEFAULT','ORIGINAL_ON','D9_TIMING_ON','QA_OUTPUT4096')){
  $oldVariant=@($r.variants|Where-Object{$_.variant-ceq$variant})[0]
  $oldBin=[IO.Path]::GetDirectoryName($oldVariant.nativeDll)
  $bin=Join-Path $BuildRoot ('native\'+$variant+'\bin');[void][IO.Directory]::CreateDirectory($bin)
  $originalCalls=@($m.originalOrderedNativeCalls|Where-Object{$_.variant-ceq$variant})
  if($originalCalls.Count-ne3){throw 'Original three native compiler calls per variant required'}
  foreach($original in $originalCalls){
   $argv=@($original.arguments|ForEach-Object{([string]$_).Replace($m.frozenSourceRoot,$taskFrozenRoot).Replace($oldBin,$bin)})
   if($argv.Count-ne$original.argumentCount){throw 'Ordered native argument shape differs'}
   for($i=0;$i-lt$argv.Count;$i++){if($argv[$i].Replace($taskFrozenRoot,$m.frozenSourceRoot).Replace($bin,$oldBin)-cne$original.arguments[$i]){throw 'Original native argv inverse differs'}}
   [void](TaskCall $original.executable $argv $variant $original.kind)
  }
  foreach($file in @('GpuProjectionPass.obj','SharedStereoBridge.obj','VRSIMNative.obj','VRSIM.Native.SharedWire.QA.dll','VRSIM.Native.SharedWire.QA.lib','VRSIM.Native.SharedWire.QA.exp')){
   $pin=TaskFile (Join-Path $bin $file);$taskOutputs+=([ordered]@{variant=$variant;role=$file;path=$pin.path;sha256=$pin.sha256;bytes=$pin.bytes})
  }
  $dll=Join-Path $bin 'VRSIM.Native.SharedWire.QA.dll'
  $exportsCall=TaskCall $taskDumpbin @('/nologo','/exports',$dll) $variant 'EXPORTS'
  $importsCall=TaskCall $taskDumpbin @('/nologo','/imports',$dll) $variant 'IMPORTS'
  $exports=@(TaskExportNames $exportsCall.stdout.path);$imports=@(TaskImportNames $importsCall.stdout.path)
  $exportsEqual=@(Compare-Object @($oldVariant.exports) $exports).Count-eq0
  $importsEqual=@(Compare-Object @($oldVariant.imports) $imports).Count-eq0
  if(!$exportsEqual-or!$importsEqual-or$exports.Count-ne$(if($variant-ceq'DEFAULT'){25}else{28})){throw ('Original import/export ABI changed: '+$variant)}
  $baselineOutput=@($m.originalNativeOutputs|Where-Object{$_.variant-ceq$variant-and$_.role-ceq'VRSIM.Native.SharedWire.QA.dll'})[0]
  $taskVariants+=([ordered]@{variant=$variant;sourceEdge=$oldVariant.sourceEdge;outputEdge=$oldVariant.outputEdge;dll=(TaskFile $dll);baselineDll=$baselineOutput;rawDllByteEqualToBaseline=((TaskSha $dll)-ceq$baselineOutput.sha256);exports=$exports;imports=$imports;exportsEqual=$exportsEqual;importsEqual=$importsEqual;orderedOriginalCompilerArgvInverseExact=$true})
 }
 $defaultCall=@($m.originalOrderedNativeCalls|Where-Object{$_.variant-ceq'DEFAULT'-and$_.kind-ceq'NATIVE_COMPILE_LINK'})[0]
 $linkIndex=[Array]::IndexOf($defaultCall.arguments,'/link');if($linkIndex-lt0){throw 'Original DEFAULT link boundary missing'}
 $prefix=@($defaultCall.arguments[0..($linkIndex-1)]|Where-Object{$_-cne'/LD'-and$_-notmatch'^/(Fo|Fd|Fe):'-and$_-notmatch'(?i)\.obj$'})
 $baselineEp=TaskCall $taskCl (@('/EP')+$prefix) 'BASELINE_DEFAULT' 'PREPROCESS_EP'
 $candidatePrefix=@($prefix|ForEach-Object{([string]$_).Replace($m.frozenSourceRoot,$taskFrozenRoot)})
 $candidateEp=TaskCall $taskCl (@('/EP')+$candidatePrefix) 'CANDIDATE_DEFAULT' 'PREPROCESS_EP'
 $baselineView=TaskProjection $baselineEp.stdout.path;$candidateView=TaskProjection $candidateEp.stdout.path
 $projectionEqual=$baselineView.lines.Count-eq$candidateView.lines.Count
 if($projectionEqual){for($i=0;$i-lt$baselineView.lines.Count;$i++){if($baselineView.lines[$i]-cne$candidateView.lines[$i]){$projectionEqual=$false;break}}}
 $baseProjection=Join-Path $BuildRoot 'DEFAULT.BASELINE.code-projection.txt'
 $newProjection=Join-Path $BuildRoot 'DEFAULT.CANDIDATE.code-projection.txt'
 TaskNewUtf8 $baseProjection ($baselineView.lines-join[Environment]::NewLine)
 TaskNewUtf8 $newProjection ($candidateView.lines-join[Environment]::NewLine)
 $taskEP=[ordered]@{baselineRaw=$baselineEp.stdout;candidateRaw=$candidateEp.stdout;rawByteEqual=($baselineEp.stdout.sha256-ceq$candidateEp.stdout.sha256);baselineTimestampRawLines=$baselineView.timestampRawLines;candidateTimestampRawLines=$candidateView.timestampRawLines;timestampRealDifferenceRetained=$true;projectionRule=$baselineView.comparison;baselineProjection=(TaskFile $baseProjection);candidateProjection=(TaskFile $newProjection);codeProjectionEqual=$projectionEqual;binaryByteEqualityNotInferred=$true}
 if(!$projectionEqual){throw 'DEFAULT active code projection differs outside exact recorded buildTimestamp metadata'}
}catch{$taskFailure=$_.Exception.ToString()}
finally{
 $env:PATH=$taskSavedPath
 try{
  TaskPin $PSCommandPath $ExpectedScriptSha;TaskPin $SourceAdmissionPath $ExpectedSourceAdmissionSha;TaskPin $taskManifest $taskManifestSha;TaskPin $taskClone $taskCloneSha
  foreach($row in $taskSources){TaskPin $row.path $row.sha256;TaskPin $row.baselinePath $row.baselineSha256}
  foreach($row in $taskFrozen){TaskPin $row.path $row.sha256}
  foreach($row in $taskOutputs){TaskPin $row.path $row.sha256}
  foreach($pin in @($m.nativeTools)+@($m.nativeLibraries)){TaskPin $pin.path $pin.sha256}
  foreach($row in $taskCalls){TaskPin $row.stdout.path $row.stdout.sha256;TaskPin $row.stderr.path $row.stderr.sha256}
  $taskStable=$taskFrozen.Count-eq23-and$taskSources.Count-eq23
 }catch{$taskFailure=($taskFailure+[Environment]::NewLine+$_.Exception.ToString()).Trim()}
 $receipt=[ordered]@{
  schema='NATIVE_D11_OUTPUT_TIMING_ACTUAL_COMPILE_ONLY_V1';startedUtc=$taskStarted.ToString('o');finishedUtc=[DateTime]::UtcNow.ToString('o')
  scriptPath=$PSCommandPath;scriptSha256=$ExpectedScriptSha;baselineManifestPath=$taskManifest;baselineManifestSha256=$taskManifestSha
  originActualNativeReceiptPath=$m.actualBuildReceipt.path;originActualNativeReceiptSha256=$m.actualBuildReceipt.sha256
  sourceAdmissionPath=$SourceAdmissionPath;sourceAdmissionSha256=$ExpectedSourceAdmissionSha;sourceAdmissionSchema=$a.schema
  currentNativeSourceSha256=$SourceSha;baselineNativeSourceSha256=$taskBaseSha;cloneManifestPath=$taskClone;cloneManifestSha256=$taskCloneSha
  sourceCount=23;sourcePins=$taskSources;frozenSourcePins=$taskFrozen;nativeTools=$m.nativeTools;nativeLibraries=$m.nativeLibraries
  includeDirs=$m.originalIncludeDirectories;libraryDirs=$m.originalLibraryDirectories;calls=$taskCalls;outputs=$taskOutputs;variants=$taskVariants;defaultPreprocessorComparison=$taskEP
  expectedCallCount=22;actualNativeCompilerCallCount=@($taskCalls|Where-Object{$_.kind-in@('PROJECTION_COMPILE','BRIDGE_COMPILE','NATIVE_COMPILE_LINK')}).Count
  actualPreprocessCallCount=@($taskCalls|Where-Object{$_.kind-ceq'PREPROCESS_EP'}).Count;actualDumpbinCallCount=@($taskCalls|Where-Object{$_.kind-in@('EXPORTS','IMPORTS')}).Count
  compilerExecuted=(@($taskCalls|Where-Object{$_.kind-in@('PROJECTION_COMPILE','BRIDGE_COMPILE','NATIVE_COMPILE_LINK')-and$null-ne$_.actualPid}).Count-gt0)
  shaderCompilerExecuted=$false;managedCompilerExecuted=$false;originalShaderPsIncludeSha256='BBDCF77EDB0985AB4856C70FFDFF8908D09C4FA9DA4171BF5668AEBDF6A8E480'
  newCompilerDefine=$false;BreproAdded=$false;PETimeStampNormalizationPerformed=$false;defaultBinaryByteExactRequired=$false
  defaultBinaryRawByteEqualityObserved=@($taskVariants|Where-Object{$_.variant-ceq'DEFAULT'}|ForEach-Object{$_.rawDllByteEqualToBaseline})
  originalSourceForwardByteExact=$a.forwardByteExact;originalSourceInverseByteExact=$a.inverseByteExact;pinsStable=$taskStable;failure=$taskFailure
  pass=(!$taskFailure-and$taskStable-and$taskCalls.Count-eq22-and$taskOutputs.Count-eq24-and$taskVariants.Count-eq4-and$null-ne$taskEP-and$taskEP.codeProjectionEqual)
  dllLoaded=$false;outputsExecuted=$false;SDKInvoked=$false;GPUInvoked=$false;XRInvoked=$false;SIMInvoked=$false;hardwareExecuted=$false;installed=$false
  runtimeAdmission=$false;runtimeAllowed=$false;readyForRun=$false;actualPerformanceProven=$false
 }
 TaskNewUtf8 (Join-Path $BuildRoot 'actual-native-compile-only-receipt.json') ($receipt|ConvertTo-Json -Depth 30)
}
$receipt|Select-Object pass,pinsStable,failure,actualNativeCompilerCallCount,actualPreprocessCallCount,actualDumpbinCallCount,runtimeAllowed|ConvertTo-Json -Depth 4
if(!$receipt.pass){throw 'Actual native offline compile/ABI/DEFAULT projection failed; retain all actual evidence'}
