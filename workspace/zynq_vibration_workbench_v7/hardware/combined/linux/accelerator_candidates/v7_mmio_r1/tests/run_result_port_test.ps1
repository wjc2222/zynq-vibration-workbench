param([string]$WorkDirectory='E:\zynq_workbench_v7_build\driver_result_unit_r1',[string]$VivadoBin='D:\vivado\2025.2\Vivado\bin')
$ErrorActionPreference='Stop'
$driverSource=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..')).Path
$analysisSource=(Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..\..\..\..\analysis_v6')).Path
$expectedRtl='D510D6DBD7904F4FE967323A064C4ED189F12F89732EC924FB356FEBF4C8EEAE'
if((Get-FileHash -Algorithm SHA256 -LiteralPath (Join-Path $analysisSource 'rtl\detection_accel.sv')).Hash -ne $expectedRtl){throw 'Sealed RTL differs'}
New-Item -ItemType Directory -Path $WorkDirectory -Force | Out-Null
foreach($item in @('rtl/detection_accel.sv','tests/fir_unit_fft_stub.sv','data/fir_all.mem','data/hann_8192.mem')){Copy-Item -LiteralPath (Join-Path $analysisSource $item) -Destination $WorkDirectory}
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'v7_mmio_result_tb.sv') -Destination $WorkDirectory
Push-Location -LiteralPath $WorkDirectory
try{
 & (Join-Path $VivadoBin 'xvlog.bat') --sv detection_accel.sv fir_unit_fft_stub.sv v7_mmio_result_tb.sv
 if($LASTEXITCODE -ne 0){throw 'Compile failed'}
 & (Join-Path $VivadoBin 'xelab.bat') v7_mmio_result_tb -s v7_mmio_result --debug typical --mt 2 --timescale 1ns/1ps
 if($LASTEXITCODE -ne 0){throw 'Elaborate failed'}
 & (Join-Path $VivadoBin 'xsim.bat') v7_mmio_result -runall -log v7-mmio-result.log
 if($LASTEXITCODE -ne 0 -or -not (Select-String -LiteralPath 'v7-mmio-result.log' -Pattern 'V7_LO32_RESULT_PORT_PASS transactions=37345' -Quiet)){throw 'LO32 RTL port regression failed'}
}finally{Pop-Location}
