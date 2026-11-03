param(
    [string]$WorkDirectory = 'E:\zynq_workbench_v7_build\fir_unit',
    [string]$VivadoBin = 'D:\vivado\2025.2\Vivado\bin',
    [switch]$BusOnly,
    [switch]$DirectFir
)
$ErrorActionPreference='Stop'
$analysisUnitRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
New-Item -ItemType Directory -Force -Path $WorkDirectory | Out-Null
$analysisUnitWork = (Resolve-Path -LiteralPath $WorkDirectory).Path
if ($analysisUnitWork.StartsWith($analysisUnitRoot,[StringComparison]::OrdinalIgnoreCase)) {
    throw 'Use a build directory outside the source directory.'
}
foreach ($analysisUnitFile in @('rtl/detection_accel.sv','tests/fir_unit_fft_stub.sv','tests/result_port_unit_tb.sv','tests/fir_unit_tb.sv','data/fir_all.mem','data/hann_8192.mem')) {
    Copy-Item -LiteralPath (Join-Path $analysisUnitRoot $analysisUnitFile) -Destination $analysisUnitWork
}
if (-not $BusOnly) {
    & python (Join-Path $PSScriptRoot 'fir_unit_generate.py') (Join-Path $analysisUnitRoot 'data') $analysisUnitWork
    if ($LASTEXITCODE -ne 0) { throw 'Independent integer FIR fixture generation failed.' }
}
Push-Location -LiteralPath $analysisUnitWork
try {
    & (Join-Path $VivadoBin 'xvlog.bat') --sv detection_accel.sv fir_unit_fft_stub.sv result_port_unit_tb.sv fir_unit_tb.sv
    if ($LASTEXITCODE -ne 0) { throw 'Isolated unit compilation failed.' }
    & (Join-Path $VivadoBin 'xelab.bat') result_port_unit_tb -s result_port_unit --debug typical --mt 4 --timescale 1ns/1ps
    if ($LASTEXITCODE -ne 0) { throw 'Result-port unit elaboration failed.' }
    & (Join-Path $VivadoBin 'xsim.bat') result_port_unit -runall -log result-port-unit.log
    if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath 'result-port-unit.log' -Pattern 'RESULT_PORT_UNIT_PASS' -Quiet)) {
        throw 'Result-port unit did not pass.'
    }
    if (-not $BusOnly) {
        $analysisFirTop = if ($DirectFir) { 'fir_unit_direct_tb' } else { 'fir_unit_tb' }
        & (Join-Path $VivadoBin 'xelab.bat') $analysisFirTop -s fir_unit --debug typical --mt 4 --timescale 1ns/1ps
        if ($LASTEXITCODE -ne 0) { throw 'FIR-only unit elaboration failed.' }
        & (Join-Path $VivadoBin 'xsim.bat') fir_unit -runall -log fir-unit.log
        $analysisExpectedCases = if ($DirectFir) { 4 } else { 28 }
        if ($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath 'fir-unit.log' -Pattern "FIR_UNIT_PASS cases=$analysisExpectedCases " -Quiet)) {
            throw "FIR-only unit did not pass all $analysisExpectedCases requested cases."
        }
    }
} finally { Pop-Location }
