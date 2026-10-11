#requires -Version 7.5
param([Parameter(Mandatory)][string]$PlanPath,[Parameter(Mandatory)][string]$PlanSha256)
$ErrorActionPreference='Stop'
function Sha([string]$p){(Get-FileHash -LiteralPath $p).Hash}
function Pin([string]$p,[string]$s){if((Sha $p)-cne$s){throw ('Pin drift: '+$p)}}
function Fresh([string]$p,[string]$v){$b=[Text.UTF8Encoding]::new($false).GetBytes($v);$f=[IO.File]::Open($p,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write);try{$f.Write($b,0,$b.Length)}finally{$f.Dispose()}}
Pin $PlanPath $PlanSha256
$taskPlan=Get-Content -LiteralPath $PlanPath -Raw|ConvertFrom-Json -DateKind String
if($taskPlan.schema-cne'OWNER_PHASE_OBSERVATION_ROOT_CSC_PLAN_V1'-or!$taskPlan.compileOnlyAdmission-or$taskPlan.runtimeAllowed-or$taskPlan.sources.Count-ne20){throw 'Compile-only plan topology refused'}
Pin $taskPlan.reviewPath $taskPlan.reviewSha256
Pin $taskPlan.baseRecipePath $taskPlan.baseRecipeSha256
Pin $PSCommandPath $taskPlan.scriptSha256
$taskRecipe=Get-Content -LiteralPath $taskPlan.baseRecipePath -Raw|ConvertFrom-Json -DateKind String
if($taskRecipe.calls.Count-ne4){throw 'Four original variant recipes required'}
Pin $taskRecipe.csc.path $taskRecipe.csc.sha256
foreach($taskRef in $taskRecipe.references){Pin $taskRef.path $taskRef.sha256}
foreach($taskSrc in $taskPlan.sources){Pin $taskSrc.path $taskSrc.sha256}
if($taskPlan.calls.Count-ne7){throw 'Four unchanged variants, phase ON, and two rejected combinations required'}
$taskExpectedNames=@('DEFAULT','QA_OFF','QA_ON','QA_OUTPUT4096','PHASE_ON','PHASE_BAD_NO_OUTPUT','PHASE_BAD_NO_POLL')
if(($taskPlan.calls.name-join'|')-cne($taskExpectedNames-join'|')){throw 'CSC variant order changed'}
for($taskN=0;$taskN-lt7;$taskN++){
 $taskC=$taskPlan.calls[$taskN]
 $taskExpectedBase=if($taskN-lt4){$taskExpectedNames[$taskN]}else{'QA_OUTPUT4096'}
 $taskExpectedAdd=if($taskN-lt4){''}else{'VRSIM_QA_OWNER_PHASE_CPU_WALL'}
 $taskExpectedRemove=if($taskN-eq5){'VRSIM_QA_OUTPUT_4096'}elseif($taskN-eq6){'VRSIM_QA_D9_BEGIN_POLL'}else{''}
 if($taskC.baseVariant-cne$taskExpectedBase-or($taskC.addDefines-join',')-cne$taskExpectedAdd-or($taskC.removeDefines-join',')-cne$taskExpectedRemove-or[bool]$taskC.expectReject-ne($taskN-ge5)){throw 'CSC derived macro contract changed'}
}
if(Test-Path -LiteralPath $taskPlan.buildRoot){throw 'Fresh build root required'}
[void][IO.Directory]::CreateDirectory($taskPlan.buildRoot)
$taskFrozen=Join-Path $taskPlan.buildRoot 'frozen-source'
[void][IO.Directory]::CreateDirectory($taskFrozen)
$taskSourceMap=@{}
foreach($taskSrc in $taskPlan.sources){
 if($taskSourceMap.ContainsKey($taskSrc.relative)){throw 'Duplicate source relative name'}
 $taskDest=Join-Path $taskFrozen $taskSrc.relative
 [void][IO.Directory]::CreateDirectory((Split-Path -Parent $taskDest))
 [IO.File]::Copy($taskSrc.path,$taskDest,$false)
 Pin $taskDest $taskSrc.sha256
 (Get-Item -LiteralPath $taskDest).IsReadOnly=$true
 $taskSourceMap[$taskSrc.relative]=$taskDest
}
$taskActual=[Collections.Generic.List[object]]::new()
$taskError=$null
$taskCompilerStartedCount=0
try{
 foreach($taskCall in $taskPlan.calls){
  $taskArgs=[Collections.Generic.List[string]]::new()
  $taskVariantDir=Join-Path $taskPlan.buildRoot $taskCall.name
  [void][IO.Directory]::CreateDirectory($taskVariantDir)
  $taskOutPath=Join-Path $taskVariantDir 'UX.VRSIM.Sim.Plugin.SharedGPU.dll'
  $taskBaseline=@($taskRecipe.calls|Where-Object variant -CEQ $taskCall.baseVariant)
  if($taskBaseline.Count-ne1){throw 'Unknown original variant'}
  $taskMapped=0;$taskDefines=0;$taskOutputs=0
  foreach($taskArg in $taskBaseline[0].arguments){
   if($taskArg.StartsWith('/out:',[StringComparison]::Ordinal)){$taskArgs.Add('/out:'+$taskOutPath);$taskOutputs++}
   elseif($taskArg.StartsWith($taskRecipe.originalFrozenSourceRoot+'\',[StringComparison]::Ordinal)){
    $taskRel=$taskArg.Substring($taskRecipe.originalFrozenSourceRoot.Length+1)
    if(!$taskSourceMap.ContainsKey($taskRel)){throw 'Original source outside fixed twenty'}
    $taskArgs.Add($taskSourceMap[$taskRel]);$taskMapped++
   }elseif($taskArg.StartsWith('/define:',[StringComparison]::Ordinal)){
    $taskFlags=@($taskArg.Substring(8).Split(','))
    foreach($taskRemove in $taskCall.removeDefines){if($taskRemove-cnotin$taskFlags){throw 'Removed macro not in original'};$taskFlags=@($taskFlags|Where-Object {$_-cne$taskRemove})}
    foreach($taskAdd in $taskCall.addDefines){if($taskAdd-cin$taskFlags){throw 'Added macro already present'};$taskFlags+=,$taskAdd}
    $taskArgs.Add('/define:'+($taskFlags-join','));$taskDefines++
   }else{$taskArgs.Add([string]$taskArg)}
  }
  if($taskMapped-ne20-or$taskDefines-ne1-or$taskOutputs-ne1){throw 'CSC literal mapping incomplete'}
  $taskStdout=Join-Path $taskPlan.buildRoot ($taskCall.name+'.stdout.raw')
  $taskStderr=Join-Path $taskPlan.buildRoot ($taskCall.name+'.stderr.raw')
  Fresh (Join-Path $taskPlan.buildRoot ($taskCall.name+'.argv.json')) ($taskArgs.ToArray()|ConvertTo-Json)
  $taskO=[IO.File]::Open($taskStdout,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
  $taskE=[IO.File]::Open($taskStderr,[IO.FileMode]::CreateNew,[IO.FileAccess]::Write,[IO.FileShare]::Read)
  $taskP=[Diagnostics.Process]::new();$taskP.StartInfo=[Diagnostics.ProcessStartInfo]::new()
  $taskP.StartInfo.FileName=$taskRecipe.csc.path;$taskP.StartInfo.WorkingDirectory=$taskPlan.buildRoot
  $taskP.StartInfo.UseShellExecute=$false;$taskP.StartInfo.CreateNoWindow=$true
  $taskP.StartInfo.RedirectStandardOutput=$true;$taskP.StartInfo.RedirectStandardError=$true
  foreach($taskA in $taskArgs){$taskP.StartInfo.ArgumentList.Add($taskA)}
  $taskExit=$null;$taskTimeout=$false
  try{if(!$taskP.Start()){throw 'CSC did not start'};$taskCompilerStartedCount++;$taskOt=$taskP.StandardOutput.BaseStream.CopyToAsync($taskO);$taskEt=$taskP.StandardError.BaseStream.CopyToAsync($taskE);if(!$taskP.WaitForExit(60000)){$taskTimeout=$true;$taskP.Kill();$taskP.WaitForExit()};[void]$taskOt.GetAwaiter().GetResult();[void]$taskEt.GetAwaiter().GetResult();$taskExit=$taskP.ExitCode}finally{$taskP.Dispose();$taskO.Dispose();$taskE.Dispose()}
  $taskDiagnostic=[IO.File]::ReadAllText($taskStdout)+[IO.File]::ReadAllText($taskStderr)
  $taskDll=if(Test-Path -LiteralPath $taskOutPath){[ordered]@{path=$taskOutPath;sha256=Sha $taskOutPath;bytes=(Get-Item -LiteralPath $taskOutPath).Length}}else{$null}
  $taskAccepted=if($taskCall.expectReject){(!$taskTimeout-and$taskExit-ne0-and$null-eq$taskDll-and$taskDiagnostic.Contains('VRSIM_QA_OWNER_PHASE_CPU_WALL requires VRSIM_QA_OUTPUT_4096 and VRSIM_QA_D9_BEGIN_POLL'))}else{(!$taskTimeout-and$taskExit-eq0-and$null-ne$taskDll)}
  $taskActual.Add([ordered]@{name=$taskCall.name;baseVariant=$taskCall.baseVariant;arguments=$taskArgs.ToArray();exitCode=$taskExit;timedOut=$taskTimeout;expectedReject=$taskCall.expectReject;expectationMet=$taskAccepted;stdoutPath=$taskStdout;stdoutSha256=Sha $taskStdout;stderrPath=$taskStderr;stderrSha256=Sha $taskStderr;dll=$taskDll})
  if(!$taskAccepted){throw ('CSC expectation failed: '+$taskCall.name)}
 }
}catch{$taskError=$_.Exception.ToString()}
$taskDrift=@()
$taskPostPinError=$null
try{
 $taskDrift=@(foreach($taskSrc in $taskPlan.sources){if((Sha $taskSrc.path)-cne$taskSrc.sha256-or(Sha $taskSourceMap[$taskSrc.relative])-cne$taskSrc.sha256){$taskSrc.relative}})
 foreach($taskRef in $taskRecipe.references){Pin $taskRef.path $taskRef.sha256}
 Pin $taskRecipe.csc.path $taskRecipe.csc.sha256
 Pin $taskPlan.baseRecipePath $taskPlan.baseRecipeSha256
 Pin $taskPlan.reviewPath $taskPlan.reviewSha256
 Pin $PlanPath $PlanSha256
 Pin $PSCommandPath $taskPlan.scriptSha256
}catch{$taskPostPinError=$_.Exception.ToString()}
$taskReceipt=[ordered]@{schema='OWNER_PHASE_OBSERVATION_ACTUAL_CSC_ONLY_V1';utc=[datetime]::UtcNow.ToString('o');planPath=$PlanPath;planSha256=$PlanSha256;sourcePins=$taskPlan.sources;reviewPath=$taskPlan.reviewPath;reviewSha256=$taskPlan.reviewSha256;scriptSha256=$taskPlan.scriptSha256;compiler=$taskRecipe.csc;referencePins=$taskRecipe.references;calls=$taskActual.ToArray();sourceDrift=$taskDrift;error=$taskError;postPinError=$taskPostPinError;actualCompilerStartedCount=$taskCompilerStartedCount;compilerExecuted=($taskCompilerStartedCount-gt0);assemblyLoaded=$false;SDKInvoked=$false;nativeInvoked=$false;XRInvoked=$false;installed=$false;actualPerformanceProven=$false;runtimeAllowed=$false;pass=(!$taskError-and!$taskPostPinError-and$taskDrift.Count-eq0-and$taskActual.Count-eq7-and$taskCompilerStartedCount-eq7)}
Fresh (Join-Path $taskPlan.buildRoot 'actual-compile-only-receipt.json') ($taskReceipt|ConvertTo-Json -Depth 15)
[pscustomobject]$taskReceipt|Select-Object pass,error,sourceDrift,assemblyLoaded|ConvertTo-Json
if(!$taskReceipt.pass){exit 1}
