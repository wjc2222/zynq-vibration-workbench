param(
    [Parameter(Mandatory=$true)][string]$WorkDirectory,
    [string]$VivadoBin = 'D:/vivado/2025.2/Vivado/bin',
    [string]$Python = 'python',
    [ValidateSet('ResultPort','FIR','All')][string]$Suite = 'All'
)
# FIR generation requires Python with NumPy. Both suites require Vivado 2025.2.
# The FFT model is a stub here; full golden FFT comparison uses run_xsim.ps1
# followed by run_fft_compare.py and the installed AMD C-model ZIP.
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'stage_analysis.ps1')
$stage = New-PublicAnalysisStage -WorkDirectory $WorkDirectory
$runRoot = Join-Path $stage.Work 'isolated'
$null = New-Item -ItemType Directory -Path $runRoot
foreach ($item in @('rtl/detection_accel.sv','tests/fir_unit_fft_stub.sv','tests/fir_unit_tb.sv','tests/result_port_unit_tb.sv','tests/v7_mmio_result_tb.sv','data/fir_all.mem','data/hann_8192.mem')) {
    Copy-Item -LiteralPath (Join-Path $stage.Analysis $item) -Destination $runRoot
}
if ($Suite -in @('FIR','All')) {
    & $Python (Join-Path $stage.Analysis 'tests/fir_unit_generate.py') (Join-Path $stage.Analysis 'data') $runRoot
    if ($LASTEXITCODE -ne 0) { throw 'Independent integer FIR fixture generation failed.' }
}
Push-Location -LiteralPath $runRoot
try {
    & (Join-Path $VivadoBin 'xvlog.bat') --sv detection_accel.sv fir_unit_fft_stub.sv fir_unit_tb.sv result_port_unit_tb.sv v7_mmio_result_tb.sv
    if ($LASTEXITCODE -ne 0) { throw 'Isolated RTL compilation failed.' }
    $cases = @()
    if ($Suite -in @('ResultPort','All')) {
        $cases += @{Top='result_port_unit_tb';Marker='RESULT_PORT_UNIT_PASS transactions=37345'}
        $cases += @{Top='v7_mmio_result_tb';Marker='V7_LO32_RESULT_PORT_PASS transactions=37345'}
    }
    if ($Suite -in @('FIR','All')) {
        $cases += @{Top='fir_unit_tb';Marker='FIR_UNIT_PASS cases=28 exact_sample_reads=688128'}
        $cases += @{Top='fir_unit_direct_tb';Marker='FIR_UNIT_PASS cases=4 exact_sample_reads=98304'}
    }
    foreach ($case in $cases) {
        $snapshot = 'public_' + $case.Top
        & (Join-Path $VivadoBin 'xelab.bat') $case.Top -s $snapshot --debug typical --mt 2 --timescale 1ns/1ps
        if ($LASTEXITCODE -ne 0) { throw "Elaboration failed: $($case.Top)" }
        $log = $snapshot + '.log'
        & (Join-Path $VivadoBin 'xsim.bat') $snapshot -runall -log $log
        if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath $log -SimpleMatch -Pattern $case.Marker -Quiet)) {
            throw "Isolated test did not record its expected completion marker: $($case.Top)"
        }
    }
} finally { Pop-Location }
Write-Output "PASS isolated suite=$Suite. FIR/port scopes only; FFT is stubbed, no full FFT or board acceptance. Logs: $runRoot"
