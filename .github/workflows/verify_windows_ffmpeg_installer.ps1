param(
	[Parameter(Mandatory = $true)]
	[string] $Workspace
)

$ErrorActionPreference = "Stop"

$ffmpegBin = Join-Path $Workspace "3rdparty/ffmpeg/bin"
$installerDirectory = Join-Path $Workspace "build/installer/client"
$msi = Get-ChildItem $installerDirectory -Filter "mumble_client-*.msi" -File |
	Sort-Object LastWriteTime -Descending |
	Select-Object -First 1

if (-not $msi) {
	throw "The Windows client MSI was not produced."
}

$expected = @(Get-ChildItem $ffmpegBin -Filter "*.dll" -File | ForEach-Object Name)
if ($expected.Count -eq 0) {
	throw "The configured FFmpeg bundle contains no runtime DLLs."
}

$extractDirectory = Join-Path $installerDirectory "ffmpeg-payload-check"
New-Item -ItemType Directory -Force $extractDirectory | Out-Null

& msiexec.exe /a $msi.FullName /qn "TARGETDIR=$extractDirectory"
if ($LASTEXITCODE -ne 0) {
	throw "Administrative MSI extraction failed with exit code $LASTEXITCODE."
}

$installed = @(Get-ChildItem $extractDirectory -Filter "*.dll" -File -Recurse | ForEach-Object Name)
$missing = @($expected | Where-Object { $_ -notin $installed })
if ($missing.Count -ne 0) {
	throw "The MSI is missing FFmpeg runtime DLLs: $($missing -join ', ')"
}

Write-Host "Verified all $($expected.Count) FFmpeg runtime DLLs in $($msi.Name)."
