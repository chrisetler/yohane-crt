# Builds the release DLL and packages it as dist\YohaneCRT-<version>.zip, plus SHA256SUMS.txt.
# The zip's contents go straight into the game folder:
#   D3DCOMPILER_43.dll, crt\crt.hlsl, crt\crt-geom.hlsl,
#   YohaneCRT-README.txt, YohaneCRT-LICENSE.txt, YohaneCRT-source\ (full source, for the GPL)
#
# usage: powershell -ExecutionPolicy Bypass -File package.ps1 [-Url https://github.com/<you>/<repo>]
param(
    [string]$Url = "(see the mod's GitHub or Nexus page)"
)
$ErrorActionPreference = "Stop"
$here = $PSScriptRoot

$version = (Select-String -Path "$here\shaderlog\shaderlog.cpp" -Pattern '#define CRT_VERSION "([^"]+)"').Matches[0].Groups[1].Value
Write-Host "Yohane CRT $version"

cmd /c "`"$here\shaderlog\build.bat`"" | Out-Null
if ($LASTEXITCODE -ne 0) { throw "build failed" }
$dll = "$here\shaderlog\release\D3DCOMPILER_43.dll"
if (-not (Test-Path $dll)) { throw "release DLL missing" }

$name = "YohaneCRT-$version"
$dist = "$here\dist"
$stage = "$dist\$name"
if (Test-Path $stage) { Remove-Item -Recurse -Force $stage }
New-Item -ItemType Directory -Force "$stage\crt", "$stage\YohaneCRT-source" | Out-Null

Copy-Item $dll $stage
Copy-Item "$here\crt\crt.hlsl", "$here\crt\crt-geom.hlsl" "$stage\crt"
Copy-Item "$here\LICENSE.txt" "$stage\YohaneCRT-LICENSE.txt"

# source: everything needed to rebuild, minus build outputs
$src = "$stage\YohaneCRT-source"
New-Item -ItemType Directory -Force "$src\shaderlog", "$src\crt\compare" | Out-Null
Copy-Item "$here\shaderlog\shaderlog.cpp", "$here\shaderlog\shaderlog.def", "$here\shaderlog\build.bat" "$src\shaderlog"
Copy-Item "$here\crt\*.hlsl", "$here\crt\crtpreview.cpp" "$src\crt"
Copy-Item "$here\crt\compare\*.hlsl", "$here\crt\compare\*.hlsli", "$here\crt\compare\crtcompare.cpp" "$src\crt\compare"
Copy-Item "$here\README.md", "$here\DEVELOPMENT.md", "$here\LICENSE.txt", "$here\package.ps1", "$here\dist-readme.txt" $src

$hash = (Get-FileHash $dll -Algorithm SHA256).Hash.ToLower()
$readme = (Get-Content -Raw "$here\dist-readme.txt").Replace("{VERSION}", $version).Replace("{URL}", $Url).Replace("{SHA256}", $hash)
[IO.File]::WriteAllText("$stage\YohaneCRT-README.txt", $readme.Replace("`r`n", "`n").Replace("`n", "`r`n"))

$zip = "$dist\$name.zip"
if (Test-Path $zip) { Remove-Item $zip }
Compress-Archive -Path "$stage\*" -DestinationPath $zip

$zipHash = (Get-FileHash $zip -Algorithm SHA256).Hash.ToLower()
[IO.File]::WriteAllText("$dist\SHA256SUMS.txt", "$hash  D3DCOMPILER_43.dll`r`n$zipHash  $name.zip`r`n")

Write-Host "zip:  $zip"
Write-Host "dll:  $hash"
Write-Host "zip:  $zipHash"
