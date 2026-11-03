param(
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string]$Vivado = 'vivado',
    [string]$Python = 'python'
)
# Requires PowerShell 7, Python with NumPy, and licensed Vivado 2025.2 XFFT IP.
# After RTL completion, use sim/run_fft_compare.py against this work root for
# independent golden AMD-model comparison; simulator completion alone is not
# full numerical acceptance. See README.md for the exact command/arguments.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stage_analysis.ps1')
$stage = New-PublicAnalysisStage -WorkDirectory $WorkDirectory
$sourceRoot = $stage.Analysis
$workRoot = $stage.Work
$vectorRoot = Join-Path $workRoot 'sim'
$fftRoot = Join-Path $workRoot 'fft_ip'
& $Python (Join-Path $sourceRoot 'tests/make_vectors.py') $vectorRoot
if ($LASTEXITCODE -ne 0) { throw "Vector generation failed: $LASTEXITCODE" }
Push-Location $workRoot
try {
    & $Vivado -mode batch -source (Join-Path $sourceRoot 'scripts/generate_fft.tcl') -tclargs $fftRoot
    if ($LASTEXITCODE -ne 0) { throw "FFT IP generation failed: $LASTEXITCODE" }
    & $Vivado -mode batch -source (Join-Path $sourceRoot 'scripts/simulate.tcl') -tclargs $workRoot $sourceRoot
    if ($LASTEXITCODE -ne 0) { throw "XSim failed: $LASTEXITCODE" }
} finally { Pop-Location }
$testLog = Join-Path $workRoot 'sim_project/sim_v6.sim/sim_1/behav/xsim/simulate.log'
if (-not (Test-Path -LiteralPath $testLog) -or -not (Select-String -LiteralPath $testLog -Pattern 'RTL_PASS profiles=4 sample_checks=32768' -Quiet)) {
    throw 'Full raw-to-FFT testbench did not record its exact completion marker.'
}
Write-Output 'XSim completed the source testbench checks. Full AMD FFT C-model comparison remains a separate environment-specific step.'
