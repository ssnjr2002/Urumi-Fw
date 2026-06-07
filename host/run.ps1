# run.ps1 - PowerShell-compatible pipeline wrapper
# PowerShell 5 corrupts binary data in pipes between native processes.
# This script uses a temp file as the transport so no binary crosses a PS pipe.
#
# Usage:
#   .\run.ps1 ..\fish.svg
#   .\run.ps1 ..\fish.svg -Plot
#   .\run.ps1 ..\fish.svg -Plot -V
#   .\run.ps1 ..\fish.svg -Serial COM3

param(
    [Parameter(Mandatory=$true, Position=0)]
    [string]$Svg,

    [switch]$Plot,
    [string]$Serial = "",
    [switch]$V
)

$tmp = [System.IO.Path]::GetTempFileName() + ".bin"

try {
    uv run --quiet python svg_to_packets.py $Svg --out $tmp
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

    $verifyArgs = @("--in", $tmp)
    if ($Plot)   { $verifyArgs += "--plot" }
    if ($Serial) { $verifyArgs += "--serial", $Serial }
    if ($V)      { $verifyArgs += "-v" }

    uv run --quiet python verify_packets.py @verifyArgs
    exit $LASTEXITCODE
}
finally {
    if (Test-Path $tmp) { Remove-Item $tmp }
}
