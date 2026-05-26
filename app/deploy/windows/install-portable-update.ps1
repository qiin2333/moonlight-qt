param(
    [Parameter(Mandatory=$true)][string]$WorkspaceDir,
    [Parameter(Mandatory=$true)][string]$InstallDir,
    [Parameter(Mandatory=$true)][string]$ZipPath,
    [Parameter(Mandatory=$true)][string]$ExePath
)

$ErrorActionPreference = 'Stop'
$extractDir = Join-Path $WorkspaceDir 'extract'
$backupDir = Join-Path $WorkspaceDir 'backup'

function Get-RequiredFreeBytes {
    param([string]$ArchivePath)

    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $archive = [System.IO.Compression.ZipFile]::OpenRead($ArchivePath)
    try {
        $uncompressedBytes = ($archive.Entries | Measure-Object -Property Length -Sum).Sum
        if ($null -eq $uncompressedBytes) {
            $uncompressedBytes = 0
        }

        return [int64]$uncompressedBytes + 64MB
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-FreeSpace {
    param(
        [string]$Path,
        [int64]$RequiredBytes
    )

    if ($RequiredBytes -le 0) {
        return
    }

    $drive = New-Object System.IO.DriveInfo(([System.IO.Path]::GetPathRoot((Resolve-Path -LiteralPath $Path).Path)))
    if ($drive.AvailableFreeSpace -lt $RequiredBytes) {
        throw "Not enough free disk space for the portable update. Need about $([math]::Ceiling($RequiredBytes / 1MB)) MB free."
    }
}

function Wait-ForUnlockedFile {
    param([string]$Path)

    for ($i = 0; $i -lt 120; $i++) {
        try {
            $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::ReadWrite, [System.IO.FileShare]::None)
            if ($stream) {
                $stream.Dispose()
            }
            return
        }
        catch {
            Start-Sleep -Milliseconds 500
        }
    }

    throw 'Timed out waiting for Moonlight to exit.'
}

function Get-ReplacedItems {
    param([string]$Path)

    Get-ChildItem -LiteralPath $Path -Force | Where-Object {
        $_.Name -ne 'Moonlight Game Streaming Project' -and
        $_.Name -notlike 'Moonlight-*.log'
    }
}

try {
    Wait-ForUnlockedFile -Path $ExePath

    if (!(Test-Path -LiteralPath $WorkspaceDir)) {
        New-Item -ItemType Directory -Path $WorkspaceDir | Out-Null
    }

    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }

    if (Test-Path -LiteralPath $backupDir) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }

    New-Item -ItemType Directory -Path $extractDir | Out-Null
    New-Item -ItemType Directory -Path $backupDir | Out-Null

    $requiredFreeBytes = Get-RequiredFreeBytes -ArchivePath $ZipPath
    Assert-FreeSpace -Path $InstallDir -RequiredBytes $requiredFreeBytes

    Expand-Archive -LiteralPath $ZipPath -DestinationPath $extractDir -Force

    $itemsToReplace = @(Get-ReplacedItems -Path $InstallDir)
    foreach ($item in $itemsToReplace) {
        Move-Item -LiteralPath $item.FullName -Destination $backupDir -Force
    }

    Get-ChildItem -LiteralPath $extractDir -Force | ForEach-Object {
        Copy-Item -LiteralPath $_.FullName -Destination $InstallDir -Recurse -Force
    }

    Remove-Item -LiteralPath $backupDir -Recurse -Force
    $backupDir = $null

    Start-Process -FilePath $ExePath -WorkingDirectory $InstallDir | Out-Null
}
catch {
    if ($backupDir -and (Test-Path -LiteralPath $backupDir)) {
        Get-ReplacedItems -Path $InstallDir | ForEach-Object {
            Remove-Item -LiteralPath $_.FullName -Recurse -Force
        }

        Get-ChildItem -LiteralPath $backupDir -Force | ForEach-Object {
            Move-Item -LiteralPath $_.FullName -Destination $InstallDir -Force
        }
    }

    $logPath = Join-Path $InstallDir 'Moonlight-update-error.log'
    $_ | Out-File -LiteralPath $logPath -Append -Encoding utf8
}
finally {
    if (Test-Path -LiteralPath $extractDir) {
        Remove-Item -LiteralPath $extractDir -Recurse -Force
    }

    if ($backupDir -and (Test-Path -LiteralPath $backupDir)) {
        Remove-Item -LiteralPath $backupDir -Recurse -Force
    }

    $workspaceDir = Split-Path -LiteralPath $ZipPath -Parent
    if (Test-Path -LiteralPath $workspaceDir) {
        Remove-Item -LiteralPath $workspaceDir -Recurse -Force
    }
}
