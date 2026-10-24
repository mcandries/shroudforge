$ErrorActionPreference = 'Stop'
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$output = Join-Path $root 'build/x64/overlay'
$work = Join-Path $root 'build/game-overlay'
$source = Join-Path $root 'modules/game-overlay'
New-Item -ItemType Directory -Force -Path $output | Out-Null
$visualStudio = & (Join-Path $PSScriptRoot 'find_visual_studio.ps1')
$cmake = Join-Path $visualStudio 'Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe'
& $cmake -S $source -B $work -G 'Visual Studio 17 2022' -A x64
if ($LASTEXITCODE -ne 0) { throw 'ShroudForge overlay configure failed.' }
& $cmake --build $work --config Release
if ($LASTEXITCODE -ne 0) { throw 'ShroudForge overlay build failed.' }
Copy-Item -LiteralPath (Join-Path $work 'Release/shroudforge_overlay.dll'),(Join-Path $source 'shroudforge_overlay.json') -Destination $output -Force
