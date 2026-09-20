param(
    [Parameter(Mandatory = $true)]
    [string]$WorkDir,
    [string]$Profile = "sm_6_2"
)

$ErrorActionPreference = "Stop"
$PSNativeCommandUseErrorActionPreference = $true

$work = [System.IO.Path]::GetFullPath($WorkDir)
$slangArchive = Join-Path $work "slang.zip"
$dxcArchive = Join-Path $work "dxc.zip"
$slangDir = Join-Path $work "slang"
$dxcDir = Join-Path $work "dxc"

New-Item -ItemType Directory -Force -Path $work | Out-Null
Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $slangDir, $dxcDir

Invoke-WebRequest `
    -Uri "https://github.com/shader-slang/slang/releases/download/v2026.9.2/slang-2026.9.2-windows-x86_64.zip" `
    -OutFile $slangArchive
Expand-Archive $slangArchive -DestinationPath $slangDir
Invoke-WebRequest `
    -Uri "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2602/dxc_2026_02_20.zip" `
    -OutFile $dxcArchive
Expand-Archive $dxcArchive -DestinationPath $dxcDir

$slangBin = Join-Path $slangDir "bin"
Copy-Item (Join-Path $dxcDir "bin/x64/dxcompiler.dll") $slangBin
Copy-Item (Join-Path $dxcDir "bin/x64/dxil.dll") $slangBin

$slangc = Join-Path $slangBin "slangc.exe"
$source = "samples/03-first-light/shaders/first_light.slang"
$shadow = Join-Path $work "shadow.dxil"
$vertex = Join-Path $work "forward.dxil"
$fragment = Join-Path $work "fragment.dxil"

# The hosted Basic Render Driver tops out at Shader Model 6.2. This compatibility scene uses no
# 6.6 feature; the engine-wide 6.6 floor remains intact for physical hardware and virtual geometry.
& $slangc $source -target dxil -profile $Profile -entry shadowVertex -stage vertex -o $shadow
& $slangc $source -target dxil -profile $Profile -entry forwardVertex -stage vertex -o $vertex
& $slangc $source -target dxil -profile $Profile -entry forwardFragment -stage fragment -o $fragment
& python samples/03-first-light/shaders/embed_dxil.py `
    samples/03-first-light/shaders/first_light_dxil.h `
    "kFirstLightShadowVertexDxil=$shadow" `
    "kFirstLightForwardVertexDxil=$vertex" `
    "kFirstLightForwardFragmentDxil=$fragment"
