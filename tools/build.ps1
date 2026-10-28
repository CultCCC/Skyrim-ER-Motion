# Build SDSCombat.dll with xmake from a VS developer shell and copy it into mod\SKSE\Plugins.
#   -Mode     xmake mode (releasedbg | debug)
#   -Publish  also copy the whole mod\ folder there (default: $env:SDSCOMBAT_PUBLISH, none if unset)
#   -Drive    xmake's globs choke on [brackets] in a path; such a checkout is built from this drive letter, mapped
#             (subst) to the checkout's parent folder
param(
    [string]$Mode = "releasedbg",
    [string]$Publish = $env:SDSCOMBAT_PUBLISH,
    [string]$Drive = "P:"
)
$ErrorActionPreference = "Stop"

$repo = (Resolve-Path -LiteralPath "$PSScriptRoot\..").ProviderPath
$work = $repo
if ($repo -match '[\[\]]') {
    if (-not (Test-Path "$Drive\")) { subst $Drive (Split-Path $repo -Parent) }
    $work = Join-Path "$Drive\" (Split-Path $repo -Leaf)
}

$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
$env:PATH = "$env:ProgramFiles\xmake;$env:ProgramFiles\Git\cmd;$env:PATH"
Remove-Item Env:XSE_TES5_MODS_PATH, Env:XSE_TES5_GAME_PATH -ErrorAction SilentlyContinue

Set-Location (Join-Path $work "plugin")
xmake f -m $Mode -y
if ($LASTEXITCODE) { throw "xmake configure failed ($LASTEXITCODE)" }
xmake build -y SDSCombat
if ($LASTEXITCODE) { throw "xmake build failed ($LASTEXITCODE)" }

$dest = Join-Path $work "mod\SKSE\Plugins"
New-Item -ItemType Directory -Force $dest | Out-Null
$bin = Get-ChildItem -Recurse -Path (Join-Path $work "plugin\build") -Filter SDSCombat.dll | Select-Object -First 1
Copy-Item $bin.FullName $dest -Force
Copy-Item ($bin.FullName -replace '\.dll$', '.pdb') $dest -Force -ErrorAction SilentlyContinue
Write-Output "built: $($bin.FullName)"

if ($Publish) {
    New-Item -ItemType Directory -Force $Publish | Out-Null
    Copy-Item -Path (Join-Path $work "mod\*") -Destination $Publish -Recurse -Force
    Write-Output "copied to: $Publish"
}
