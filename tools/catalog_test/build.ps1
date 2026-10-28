# Build the standalone AnimCatalog test (no CommonLib): catalog_test.exe file.hkx ... | catalog_test.exe --build <Data\meshes>
$ErrorActionPreference = "Stop"
$vs = & "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe" -latest -products * -property installationPath
& "$vs\Common7\Tools\Launch-VsDevShell.ps1" -Arch amd64 -HostArch amd64 -SkipAutomaticLocation | Out-Null
Set-Location -LiteralPath $PSScriptRoot
cl /nologo /std:c++latest /EHsc /O2 /utf-8 main.cpp ..\..\plugin\src\AnimCatalog.cpp /Fe:catalog_test.exe
if ($LASTEXITCODE) { throw "cl failed ($LASTEXITCODE)" }
