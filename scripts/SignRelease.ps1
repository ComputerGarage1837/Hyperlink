param(
    [Parameter(Mandatory=$true)][string]$InputDirectory,
    [Parameter(Mandatory=$true)][string]$KeyPath,
    [Parameter(Mandatory=$true)][ValidateRange(1,2147483647)][int]$Sequence,
    [Parameter(Mandatory=$true)][string]$Package
)
$ErrorActionPreference='Stop'
Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.IO.Compression.FileSystem
$inputRoot=(Resolve-Path -LiteralPath $InputDirectory).Path
$keyFile=(Resolve-Path -LiteralPath $KeyPath).Path
$packageFile=[IO.Path]::GetFullPath($Package)
$stageRoot=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../build/release-work'))
New-Item -ItemType Directory -Path $stageRoot -Force | Out-Null
$stage=[IO.Path]::GetFullPath((Join-Path $stageRoot ([Guid]::NewGuid().ToString('N'))))
New-Item -ItemType Directory -Path $stage | Out-Null
$rsa=New-Object Security.Cryptography.RSACryptoServiceProvider
$rsa.PersistKeyInCsp=$false
try {
    $plain=[Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes($keyFile),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
    try {$rsa.FromXmlString([Text.Encoding]::UTF8.GetString($plain))}finally{[Array]::Clear($plain,0,$plain.Length)}
    if($rsa.KeySize -ne 3072){throw 'Expected the offline 3072-bit release key'}
    $source=[IO.File]::ReadAllText((Join-Path $PSScriptRoot '../src/ReleaseKey.cs'))
    if(-not $source.Contains($rsa.ToXmlString($false))){throw 'The signing key does not match the embedded release public key'}
    $names=@('Hyperlink.exe','NAudio.Core.dll','NAudio.Wasapi.dll','NAudio-MIT.txt','README.md','self-test.txt')
    $optional=@('ffmpeg.exe','ffmpeg-LGPL.txt','ffmpeg-source.txt')
    if(Test-Path -LiteralPath (Join-Path $inputRoot 'ffmpeg.exe')){$names+=$optional}
    $files=@(foreach($name in $names){
        $path=Join-Path $inputRoot $name
        $file=Get-Item -LiteralPath $path
        if(($file.Attributes -band [IO.FileAttributes]::ReparsePoint) -ne 0){throw 'Linked release files are not permitted'}
        if($file.Length -lt 1 -or $file.Length -gt 192MB){throw 'Release file exceeds size limits'}
        Copy-Item -LiteralPath $path -Destination (Join-Path $stage $name)
        [ordered]@{name=$name;size=$file.Length;sha256=(Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant()}
    })
    $version=[Reflection.AssemblyName]::GetAssemblyName((Join-Path $inputRoot 'Hyperlink.exe')).Version.ToString()
    $payload=[Text.Encoding]::UTF8.GetBytes(([ordered]@{product='Hyperlink';version=$version;sequence=$Sequence;files=$files}|ConvertTo-Json -Depth 5 -Compress))
    $signature=$rsa.SignData($payload,'SHA256')
    $envelope=[ordered]@{payload=[Convert]::ToBase64String($payload);signature=[Convert]::ToBase64String($signature)}|ConvertTo-Json -Compress
    [IO.File]::WriteAllText((Join-Path $stage 'release.json'),$envelope,(New-Object Text.UTF8Encoding($false)))
    if(Test-Path -LiteralPath $packageFile){throw 'Refusing to replace an existing signed release package'}
    [IO.Compression.ZipFile]::CreateFromDirectory($stage,$packageFile)
    Write-Output "Signed release $version, sequence $Sequence"
} finally {
    $rsa.Clear()
    $resolvedStage=[IO.Path]::GetFullPath($stage)
    if($resolvedStage.StartsWith($stageRoot+[IO.Path]::DirectorySeparatorChar,[StringComparison]::OrdinalIgnoreCase) -and [regex]::IsMatch([IO.Path]::GetFileName($resolvedStage),'\A[a-f0-9]{32}\z')){
        Remove-Item -LiteralPath $resolvedStage -Recurse -Force
    }else{throw 'Release cleanup path escaped the staging directory'}
}
