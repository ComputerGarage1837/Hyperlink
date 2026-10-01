param(
    [Parameter(Mandatory=$true)][string]$WindowsPackage,
    [Parameter(Mandatory=$true)][string]$AndroidPackage,
    [Parameter(Mandatory=$true)][string]$KeyPath,
    [Parameter(Mandatory=$true)][string]$Version,
    [Parameter(Mandatory=$true)][int]$Sequence,
    [Parameter(Mandatory=$true)][int]$AndroidCode,
    [Parameter(Mandatory=$true)][string]$Output
)
$ErrorActionPreference='Stop'
if($Version -notmatch '^\d{1,5}(\.\d{1,5}){2,3}$' -or $Sequence -lt 1 -or $AndroidCode -lt 1){throw 'Invalid release identifiers'}
Add-Type -AssemblyName System.Security
Add-Type -AssemblyName System.IO.Compression.FileSystem
$windows=Get-Item -LiteralPath $WindowsPackage; $android=Get-Item -LiteralPath $AndroidPackage
if($windows.Length -gt 32MB -or $android.Length -gt 32MB){throw 'Release exceeds update size limit'}
$manifest=[ordered]@{product='Hyperlink';version=$Version;sequence=$Sequence;expires=[DateTime]::UtcNow.AddDays(365).ToString('o');windowsSha256=(Get-FileHash -LiteralPath $windows.FullName -Algorithm SHA256).Hash.ToLowerInvariant();windowsSize=$windows.Length;androidSha256=(Get-FileHash -LiteralPath $android.FullName -Algorithm SHA256).Hash.ToLowerInvariant();androidSize=$android.Length;androidCode=$AndroidCode}
$payload=[Text.Encoding]::UTF8.GetBytes(($manifest|ConvertTo-Json -Compress))
$clear=[Security.Cryptography.ProtectedData]::Unprotect([IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $KeyPath).Path),$null,[Security.Cryptography.DataProtectionScope]::CurrentUser)
$key=New-Object Security.Cryptography.RSACryptoServiceProvider
$key.PersistKeyInCsp=$false
try {$key.FromXmlString([Text.Encoding]::UTF8.GetString($clear));$signature=$key.SignData($payload,[Security.Cryptography.CryptoConfig]::MapNameToOID('SHA256'))}
finally {[Array]::Clear($clear,0,$clear.Length);$key.Dispose()}
$envelope=[ordered]@{payload=[Convert]::ToBase64String($payload);signature=[Convert]::ToBase64String($signature)}|ConvertTo-Json -Compress
New-Item -ItemType Directory -Force -Path $Output | Out-Null
[IO.File]::WriteAllText((Join-Path $Output 'updates.json'),$envelope,(New-Object Text.UTF8Encoding($false)))
$zip=[IO.Compression.ZipFile]::OpenRead($windows.FullName)
try {$entry=$zip.GetEntry('release.json');if(!$entry -or $entry.Length -gt 32768){throw 'Windows package lacks signed release metadata'};$reader=New-Object IO.StreamReader($entry.Open());try{$metadata=$reader.ReadToEnd()}finally{$reader.Dispose()}}
finally{$zip.Dispose()}
$signed=$metadata|ConvertFrom-Json;$info=[Text.Encoding]::UTF8.GetString([Convert]::FromBase64String($signed.payload))|ConvertFrom-Json
if($info.product -ne 'Hyperlink' -or $info.sequence -ne $Sequence -or ([Version]$info.version) -ne ([Version]$Version)){throw 'Windows release identifiers do not match the update feed'}
New-Item -ItemType Directory -Force -Path (Join-Path $Output 'releases/windows') | Out-Null
[IO.File]::WriteAllText((Join-Path $Output 'releases/windows/stable.json'),$metadata,(New-Object Text.UTF8Encoding($false)))
Copy-Item -LiteralPath $windows.FullName -Destination (Join-Path $Output "releases/windows/$Sequence.hup")
Copy-Item -LiteralPath $android.FullName -Destination (Join-Path $Output 'android.apk')
Write-Output 'Signed release feeds prepared. Publish packages first and feed metadata last; keep the signing key offline.'
