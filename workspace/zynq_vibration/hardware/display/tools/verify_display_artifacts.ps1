param([string]$EvidenceRoot = (Join-Path $PSScriptRoot '..\evidence\20261025_011856'))
$ErrorActionPreference = 'Stop'
$EvidenceRoot = (Resolve-Path -LiteralPath $EvidenceRoot).Path
$displayRoot = Split-Path (Split-Path $EvidenceRoot -Parent) -Parent
$artifacts = @(Get-Content -LiteralPath (Join-Path $EvidenceRoot 'sha256.json') -Raw | ConvertFrom-Json)
$sources = @(Get-Content -LiteralPath (Join-Path $EvidenceRoot 'source_sha256.json') -Raw | ConvertFrom-Json)
foreach ($group in @(@{root=$EvidenceRoot; entries=$artifacts}, @{root=$displayRoot; entries=$sources})) {
    foreach ($entry in $group.entries) {
        $path = [IO.Path]::GetFullPath((Join-Path $group.root $entry.path))
        if (-not $path.StartsWith($group.root + [IO.Path]::DirectorySeparatorChar, [StringComparison]::OrdinalIgnoreCase)) {
            throw "Manifest path outside expected root: $($entry.path)"
        }
        if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash.ToLowerInvariant() -ne $entry.sha256) {
            throw "Hash mismatch: $path"
        }
    }
}
$embedded = @(Get-Content -LiteralPath (Join-Path $EvidenceRoot 'xsa_embedded_verification.json') -Raw | ConvertFrom-Json)
$xsa = [IO.Compression.ZipFile]::OpenRead((Join-Path $EvidenceRoot 'export\ax7z020_display.xsa'))
try {
    foreach ($item in $embedded) {
        $entry = $xsa.GetEntry($item.entry)
        if (-not $entry) { throw "XSA missing $($item.entry)" }
        $stream = $entry.Open()
        $algorithm = [Security.Cryptography.SHA256]::Create()
        try { $actual = [Convert]::ToHexString($algorithm.ComputeHash($stream)).ToLowerInvariant() }
        finally { $algorithm.Dispose(); $stream.Dispose() }
        if ($actual -ne $item.sha256) { throw "XSA embedded hash mismatch: $($item.entry)" }
        $exportName = if ($item.entry -eq 'ax7z020_display.bit') {'ax7z020_display_720p60.bit'} else {$item.entry}
        if ((Get-FileHash -LiteralPath (Join-Path $EvidenceRoot "export\$exportName") -Algorithm SHA256).Hash.ToLowerInvariant() -ne $actual) {
            throw "XSA/export content mismatch: $exportName"
        }
    }
} finally { $xsa.Dispose() }
[ordered]@{
    status = 'PASS'
    evidence = $EvidenceRoot
    artifact_hashes = $artifacts.Count
    fpga_source_hashes = $sources.Count
    embedded_xsa_exports = $embedded.Count
    hardware_access = $false
    scope = 'Artifact/source consistency only; not kernel compilation, board execution or optical acceptance'
} | ConvertTo-Json
