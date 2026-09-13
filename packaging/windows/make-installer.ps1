# Build the two things a user can be handed, from a DLL that already exists:
#   dist\iso-recorder-<version>-windows-x64.zip           unzip, double-click Install.cmd
#   dist\iso-recorder-<version>-windows-x64-setup.exe     double-click, done
#
# The .exe is an IExpress self-extracting package, which ships with Windows, so
# nothing has to be installed to make it. It runs Install.cmd elevated, which is
# what writing to ProgramData needs.
#
# Build the DLL first:  .\build-win.ps1 -ObsSrc <checkout> -QtPrefix <qt>;<obs-libs>

param(
    [string]$Dll = "$PSScriptRoot\..\..\build-win\iso-recorder.dll",
    [string]$OutDir = "$PSScriptRoot\..\..\dist",
    [string]$Version = "0.3.0"
)

$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot\..\..").Path

if (-not (Test-Path $Dll)) {
    throw "No DLL at $Dll - build it first with build-win.ps1"
}

$work = Join-Path $env:TEMP "iso-recorder-pkg"
$payload = Join-Path $work "payload"
Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force -Path $payload, $OutDir | Out-Null

Copy-Item $Dll                                        "$payload\iso-recorder.dll"
Copy-Item "$PSScriptRoot\Install.cmd"                 "$payload\Install.cmd"
Copy-Item "$PSScriptRoot\README.txt"                  "$payload\README.txt"
Copy-Item "$repo\LICENSE"                             "$payload\LICENSE.txt"

$stem = "iso-recorder-$Version-windows-x64"
$zip = Join-Path $OutDir "$stem.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
Compress-Archive -Path "$payload\*" -DestinationPath $zip -CompressionLevel Optimal

$exe = Join-Path $OutDir "$stem-setup.exe"
Remove-Item $exe -Force -ErrorAction SilentlyContinue

$sed = Join-Path $work "installer.sed"
@"
[Version]
Class=IEXPRESS
SEDVersion=3
[Options]
PackagePurpose=InstallApp
ShowInstallProgramWindow=0
HideExtractAnimation=1
UseLongFileName=1
InsideCompressed=0
CAB_FixedSize=0
CAB_ResvCodeSigning=0
RebootMode=N
InstallPrompt=
DisplayLicense=
FinishMessage=
TargetName=$exe
FriendlyName=ISO Recorder $Version
AppLaunched=Install.cmd
PostInstallCmd=<None>
AdminQuietInstCmd=Install.cmd
UserQuietInstCmd=
SourceFiles=SourceFiles
[SourceFiles]
SourceFiles0=$payload\
[SourceFiles0]
iso-recorder.dll=
Install.cmd=
README.txt=
LICENSE.txt=
"@ | Set-Content -Path $sed -Encoding ASCII

# IExpress is picky about the SED path and about quotes; run it from its own folder.
& "$env:SystemRoot\System32\iexpress.exe" /N /Q $sed
Start-Sleep -Seconds 2

foreach ($f in @($zip, $exe)) {
    if (Test-Path $f) {
        $item = Get-Item $f
        Write-Host ("{0}  {1:N0} bytes" -f $item.Name, $item.Length)
    } else {
        Write-Host ("MISSING: {0}" -f $f)
    }
}
