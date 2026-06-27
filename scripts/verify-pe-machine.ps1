param(
    [Parameter(Mandatory=$true)]
    [string]$Path,

    [Parameter(Mandatory=$true)]
    [string]$ExpectedMachine
)

$ErrorActionPreference = "Stop"

$expected = [Convert]::ToUInt16($ExpectedMachine.Replace("0x", ""), 16)
$resolvedPath = Resolve-Path -LiteralPath $Path
$cleanupPath = $null

if ((Get-Item -LiteralPath $resolvedPath).PSIsContainer) {
    $root = $resolvedPath
}
elseif ([System.IO.Path]::GetExtension($resolvedPath.Path) -ieq ".zip") {
    $cleanupPath = Join-Path ([System.IO.Path]::GetTempPath()) ("moonlight-pe-check-" + [System.Guid]::NewGuid().ToString("N"))
    New-Item -ItemType Directory -Path $cleanupPath | Out-Null
    Expand-Archive -LiteralPath $resolvedPath -DestinationPath $cleanupPath -Force
    $root = Resolve-Path -LiteralPath $cleanupPath
}
else {
    throw "Path must be a directory or .zip file: $Path"
}
$badFiles = @()

function Get-PeMachine {
    param([Parameter(Mandatory=$true)][string]$FilePath)

    $stream = [System.IO.File]::OpenRead($FilePath)
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        $stream.Seek(0x3C, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $reader.ReadInt32()
        $stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $signature = $reader.ReadUInt32()
        if ($signature -ne 0x00004550) {
            throw "Invalid PE signature"
        }
        return $reader.ReadUInt16()
    }
    finally {
        $stream.Dispose()
    }
}

try {
    Get-ChildItem -LiteralPath $root -Recurse -File |
        Where-Object { $_.Extension -in ".exe", ".dll" } |
        ForEach-Object {
            $machine = Get-PeMachine -FilePath $_.FullName
            if ($machine -ne $expected) {
                $relativePath = $_.FullName.Substring($root.Path.Length + 1)
                $badFiles += [pscustomobject]@{
                    Machine = ("0x{0:X4}" -f $machine)
                    Path = $relativePath
                }
            }
        }

    if ($badFiles.Count -gt 0) {
        Write-Error ("Found {0} binaries that do not match expected machine 0x{1:X4}:`n{2}" -f $badFiles.Count, $expected, ($badFiles | Format-Table -AutoSize | Out-String))
    }

    Write-Host ("All PE binaries under {0} match machine 0x{1:X4}" -f $Path, $expected)
}
finally {
    if ($cleanupPath) {
        Remove-Item -LiteralPath $cleanupPath -Recurse -Force -ErrorAction SilentlyContinue
    }
}
