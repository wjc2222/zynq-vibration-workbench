param([Parameter(Mandatory=$true)][string]$OutputDirectory,[Parameter(Mandatory=$true)][string]$VivadoBin)
$ErrorActionPreference='Stop'
$root=[IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$out=[IO.Path]::GetFullPath($OutputDirectory)
if($out -match '[^\x00-\x7f]' -or (Test-Path -LiteralPath $out)){throw 'Use a new ASCII output directory'}
$vivado=Join-Path $VivadoBin 'vivado.bat'
if(!(Test-Path -LiteralPath $vivado)){throw 'Vivado 2025.2 executable not found'}
New-Item -ItemType Directory -Path $out | Out-Null
Copy-Item -LiteralPath (Join-Path $root 'fpga-effective/source') -Destination (Join-Path $out 'source') -Recurse
$fft=Join-Path $out 'fft_ip'
$platform=Join-Path $out 'platform'
$generator=Join-Path $root 'workspace/zynq_vibration_workbench_v7/hardware/analysis_v6/scripts/generate_fft.tcl'
& $vivado -mode batch -nojournal -log (Join-Path $out 'fft-generation.log') -source $generator -tclargs $fft
if($LASTEXITCODE -ne 0){throw 'FFT generation failed'}
& $vivado -mode batch -nojournal -log (Join-Path $out 'create.log') -source (Join-Path $out 'source/combined/scripts/create_combined.tcl') -tclargs $platform --sg
if($LASTEXITCODE -ne 0){throw 'Platform generation failed'}
& $vivado -mode batch -nojournal -log (Join-Path $out 'build.log') -source (Join-Path $out 'source/combined/scripts/build_combined.tcl') -tclargs $platform
if($LASTEXITCODE -ne 0){throw 'Implementation failed'}
