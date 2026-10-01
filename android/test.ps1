param([Parameter(Mandatory=$true)][string]$Jdk,[string]$Output="$PSScriptRoot/build/tests")
$ErrorActionPreference='Stop'
$Jdk=(Resolve-Path -LiteralPath $Jdk).Path
$Output=[IO.Path]::GetFullPath($Output)
$root=Join-Path $Output ([Guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $root | Out-Null
$sources=@("$PSScriptRoot/src/ca/myfamilyapps/hyperlink/Signing.java","$PSScriptRoot/src/ca/myfamilyapps/hyperlink/PinnedWire.java","$PSScriptRoot/tests/SigningCheck.java","$PSScriptRoot/tests/WireCheck.java")
$sources += @("$PSScriptRoot/src/ca/myfamilyapps/hyperlink/ReplyInbox.java","$PSScriptRoot/tests/ReplyInboxCheck.java")
$sources += @("$PSScriptRoot/src/ca/myfamilyapps/hyperlink/WakePacket.java","$PSScriptRoot/tests/WakePacketCheck.java")
$sources += @("$PSScriptRoot/src/ca/myfamilyapps/hyperlink/MjpegRecording.java","$PSScriptRoot/tests/RecordingCheck.java")
& "$Jdk/bin/javac.exe" --release 8 -Xlint:-options -d $root @sources
if($LASTEXITCODE -ne 0){throw 'Java tests did not compile'}
& "$Jdk/bin/java.exe" -cp $root ca.myfamilyapps.hyperlink.SigningCheck "$root/android-proof.json"
if($LASTEXITCODE -ne 0){throw 'Android signing checks failed'}
& "$Jdk/bin/java.exe" -cp $root ca.myfamilyapps.hyperlink.ReplyInboxCheck
if($LASTEXITCODE -ne 0){throw 'Android session request checks failed'}
& "$Jdk/bin/java.exe" -cp $root ca.myfamilyapps.hyperlink.WakePacketCheck
if($LASTEXITCODE -ne 0){throw 'Android wake packet checks failed'}
& "$Jdk/bin/java.exe" '-Djava.awt.headless=true' -cp $root ca.myfamilyapps.hyperlink.RecordingCheck $root
if($LASTEXITCODE -ne 0){throw 'Android recording checks failed'}
. "$PSScriptRoot/../scripts/AudioDependencies.ps1"
$audioReferences=@(Get-HyperlinkAudioReferences)
Copy-HyperlinkAudioRuntime $root
$native=@(Get-ChildItem "$PSScriptRoot/../src" -Filter *.cs | ForEach-Object {$_.FullName})
$native+=(Resolve-Path -LiteralPath "$PSScriptRoot/tests/InteropHost.cs").Path
$fixture=Join-Path $root 'InteropHost.exe'
& "$env:WINDIR/Microsoft.NET/Framework64/v4.0.30319/csc.exe" /nologo /target:exe /main:Hyperlink.InteropHost "/out:$fixture" /r:System.Windows.Forms.dll /r:System.Drawing.dll /r:System.Web.Extensions.dll /r:System.IO.Compression.dll /r:System.IO.Compression.FileSystem.dll @audioReferences @native
if($LASTEXITCODE -ne 0){throw 'Windows interoperability fixture did not compile'}
$hostProcess=Start-Process -FilePath $fixture -ArgumentList ('"'+$root+'"') -WindowStyle Hidden -PassThru
try{
    & "$Jdk/bin/java.exe" -cp $root ca.myfamilyapps.hyperlink.WireCheck $root
    if($LASTEXITCODE -ne 0){throw 'Java / Windows wire checks failed'}
    if(-not $hostProcess.WaitForExit(10000)){throw 'Windows fixture did not finish'}
    if($hostProcess.ExitCode -ne 0){throw 'Windows host enforcement checks failed'}
    Get-Content "$root/result.txt"
    Write-Output "Fresh PHP proof fixture: $root/android-proof.json"
}finally{if(-not $hostProcess.HasExited){Stop-Process -Id $hostProcess.Id}}
