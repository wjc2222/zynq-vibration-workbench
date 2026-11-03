function New-PublicAnalysisStage {
    param([Parameter(Mandatory=$true)][string]$WorkDirectory)
    $repository = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
    $workspace = Join-Path $repository 'src/workspace/zynq_vibration_workbench_v7/hardware/analysis_v6'
    $effective = Join-Path $repository 'src/fpga-effective/source/analysis_v6'
    $workRoot = [IO.Path]::GetFullPath($WorkDirectory)
    if ($workRoot -match '[^\x00-\x7F]') { throw 'Select an ASCII-only WorkDirectory for vendor simulator portability.' }
    if (Test-Path -LiteralPath $workRoot) { throw 'WorkDirectory must not exist; select a fresh temporary directory.' }
    foreach ($required in @((Join-Path $workspace 'scripts/simulate.tcl'), (Join-Path $workspace 'tests/fir_unit_tb.sv'), (Join-Path $effective 'rtl/detection_accel.sv'))) {
        if (-not (Test-Path -LiteralPath $required -PathType Leaf)) { throw "Required public source missing: $required" }
    }
    $analysis = Join-Path $workRoot 'source/analysis_v6'
    $null = New-Item -ItemType Directory -Path $analysis
    foreach ($folder in @('scripts','tests','data')) {
        Copy-Item -LiteralPath (Join-Path $workspace $folder) -Destination $analysis -Recurse
    }
    Copy-Item -LiteralPath (Join-Path $effective 'rtl') -Destination $analysis -Recurse
    # Effective coefficients are paired with the effective RTL; preserve exact bytes.
    foreach ($item in Get-ChildItem -LiteralPath (Join-Path $effective 'data') -File) {
        Copy-Item -LiteralPath $item.FullName -Destination (Join-Path $analysis 'data')
    }
    $mmio = Join-Path $repository 'src/workspace/zynq_vibration_workbench_v7/hardware/combined/linux/accelerator_candidates/v7_mmio_r1/tests/v7_mmio_result_tb.sv'
    Copy-Item -LiteralPath $mmio -Destination (Join-Path $analysis 'tests')
    [pscustomobject]@{ Work=$workRoot; Analysis=$analysis }
}
