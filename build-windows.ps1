[CmdletBinding()]
param(
    [ValidateSet('Release','Debug')] [string]$Configuration = 'Release',
    [switch]$AddressSanitizer,
    [switch]$Analyze,
    [switch]$Package
)
$ErrorActionPreference = 'Stop'
if ($Package -and ($Configuration -ne 'Release' -or $AddressSanitizer -or $Analyze)) {
    throw '-Package requires a regular Release build.'
}

$suffix = if ($AddressSanitizer) { '-asan' } elseif ($Analyze) { '-analyze' } else { '' }
if ($AddressSanitizer -and $Analyze) { throw 'Run ASan and static analysis separately.' }
$build = Join-Path $PSScriptRoot "build/windows$suffix"
$asan = if ($AddressSanitizer) { 'ON' } else { 'OFF' }
$analysis = if ($Analyze) { 'ON' } else { 'OFF' }

function Invoke-Checked([string]$Program, [string[]]$Arguments) {
    & $Program @Arguments
    if ($LASTEXITCODE -ne 0) { throw "$Program exited with code $LASTEXITCODE" }
}

function Get-OrdinaryItem([string]$Path, [bool]$Directory) {
    try { $item = Get-Item -Force -LiteralPath $Path -ErrorAction Stop }
    catch [System.Management.Automation.ItemNotFoundException] { return }
    if ($item.PSIsContainer -ne $Directory -or ($item.Attributes -band [IO.FileAttributes]::ReparsePoint)) {
        $type = if ($Directory) { 'directory' } else { 'file' }
        throw "Ordinary ${type} required: $Path"
    }
    $item
}

if ($Package) {
    $null = Get-OrdinaryItem (Join-Path $PSScriptRoot 'build') $true
    $null = Get-OrdinaryItem $build $true
    $null = Get-OrdinaryItem (Join-Path $build 'Release') $true
    $null = Get-OrdinaryItem (Join-Path $PSScriptRoot 'LICENSES') $true
}

Invoke-Checked cmake @('-S', $PSScriptRoot, '-B', $build, '-G', 'Visual Studio 17 2022', '-A', 'x64',
    "-DFFF_ASAN=$asan", "-DFFF_ANALYZE=$analysis", '-DBUILD_TESTING=ON')
Invoke-Checked cmake @('--build', $build, '--config', $Configuration, '--parallel')
Invoke-Checked ctest @('--test-dir', $build, '-C', $Configuration, '--output-on-failure', '--no-tests=error')

if ($Package) {
    $packageRoot = Join-Path $PSScriptRoot 'out/FlashFasaFiso'
    $version = '0.8.0'
    $asset = "ruffle-$version-windows-x86_64.zip"
    $url = "https://github.com/ruffle-rs/ruffle/releases/download/v$version/$asset"
    $expected = '7833834077B76700DD69E01C0DBD8E7FCE735235DA555A59F9E83F27793BCF82'
    $archive = Join-Path $build $asset
    $directories = @((Join-Path $PSScriptRoot 'out'), $packageRoot,
        (Join-Path $packageRoot 'runtime'), (Join-Path $packageRoot 'games'),
        (Join-Path $packageRoot 'LICENSES'))
    foreach ($directory in $directories) { $null = Get-OrdinaryItem $directory $true }

    $staging = Join-Path $build ("ruffle-$version-" + [guid]::NewGuid().ToString('N'))
    $extracted = Join-Path $staging 'extracted'
    $backups = Join-Path $staging 'previous'
    $files = [ordered]@{
        'FlashFasaFiso.exe' = (Join-Path $build 'Release/FlashFasaFiso.exe')
        'runtime/ruffle.exe' = (Join-Path $extracted 'ruffle.exe')
        'LICENSES/Ruffle-LICENSE.md' = (Join-Path $extracted 'LICENSE.md')
        'LICENSES/Ruffle-SOURCE.md' = (Join-Path $PSScriptRoot 'LICENSES/Ruffle-SOURCE.md')
        'LICENSE' = (Join-Path $PSScriptRoot 'LICENSE')
        'README.md' = (Join-Path $PSScriptRoot 'README.md')
        'README.tr.md' = (Join-Path $PSScriptRoot 'README.tr.md')
        'LICENSES/Ruffle-VERSION.txt' = (Join-Path $staging 'Ruffle-VERSION.txt')
    }
    $legacyLicense = Join-Path $packageRoot 'LICENSES/FlashFasaFiso.txt'
    if (Get-OrdinaryItem $legacyLicense $false) { $files['LICENSES/FlashFasaFiso.txt'] = $files['LICENSE'] }
    foreach ($relative in $files.Keys) {
        $null = Get-OrdinaryItem (Join-Path $packageRoot $relative) $false
    }

    New-Item -ItemType Directory -Path $staging | Out-Null
    $preserveStaging = $false
    try {
        if (!(Get-OrdinaryItem $archive $false)) {
            $partial = Join-Path $staging $asset
            Invoke-Checked curl.exe @('--fail', '--silent', '--show-error', '--location', '--output', $partial, $url)
            if ((Get-FileHash -Algorithm SHA256 -LiteralPath $partial).Hash -ne $expected) {
                throw "SHA-256 mismatch for $partial."
            }
            [IO.File]::Move($partial, $archive)
        }
        if ((Get-FileHash -Algorithm SHA256 -LiteralPath $archive).Hash -ne $expected) {
            throw "SHA-256 mismatch for $archive."
        }
        Expand-Archive -LiteralPath $archive -DestinationPath $extracted
        @("Ruffle Desktop v$version (Windows x64)", $url, "ZIP SHA-256 $expected",
          "EXE SHA-256 $((Get-FileHash -Algorithm SHA256 -LiteralPath $files['runtime/ruffle.exe']).Hash)") |
            Set-Content -LiteralPath $files['LICENSES/Ruffle-VERSION.txt'] -Encoding ascii

        $updates = [ordered]@{}
        foreach ($relative in $files.Keys) {
            $source = $files[$relative]
            $destination = Join-Path $packageRoot $relative
            if (!(Get-OrdinaryItem $source $false)) { throw "Missing package input: $source" }
            $previous = Get-OrdinaryItem $destination $false
            if ($previous) {
                if ((Get-FileHash -Algorithm SHA256 -LiteralPath $source).Hash -eq
                    (Get-FileHash -Algorithm SHA256 -LiteralPath $destination).Hash) { continue }
                $stream = [IO.File]::Open($destination, 'Open', 'ReadWrite', 'ReadWrite, Delete')
                $stream.Dispose()
            }
            $candidate = Join-Path $staging ([guid]::NewGuid().ToString('N'))
            [IO.File]::Copy($source, $candidate)
            $updates[$relative] = $candidate
        }
        foreach ($directory in $directories) {
            if (!(Get-OrdinaryItem $directory $true)) { New-Item -ItemType Directory -Path $directory | Out-Null }
        }

        $changes = [Collections.Generic.List[object]]::new()
        try {
            foreach ($relative in $updates.Keys) {
                $destination = Join-Path $packageRoot $relative
                $candidate = $updates[$relative]
                $change = [pscustomobject]@{ Destination = $destination; Backup = $null; Published = $false }
                if (Get-OrdinaryItem $destination $false) {
                    $change.Backup = Join-Path $backups $relative
                    $null = [IO.Directory]::CreateDirectory([IO.Path]::GetDirectoryName($change.Backup))
                    $changes.Add($change)
                    [IO.File]::Replace($candidate, $destination, $change.Backup)
                } else {
                    [IO.File]::Move($candidate, $destination)
                    $changes.Add($change)
                }
                $change.Published = $true
            }
        } catch {
            for ($index = $changes.Count - 1; $index -ge 0; --$index) {
                $change = $changes[$index]
                try {
                    $current = Get-OrdinaryItem $change.Destination $false
                    if (!$change.Backup) {
                        if ($current) { [IO.File]::Delete($change.Destination) }
                    } elseif (Get-OrdinaryItem $change.Backup $false) {
                        if ($current) {
                            $displaced = Join-Path $staging ([guid]::NewGuid().ToString('N'))
                            [IO.File]::Replace($change.Backup, $change.Destination, $displaced)
                        } else {
                            [IO.File]::Move($change.Backup, $change.Destination)
                        }
                    } elseif ($change.Published -or !$current) {
                        throw "Missing package backup: $($change.Backup)"
                    }
                } catch {
                    $preserveStaging = $true
                    Write-Warning -WarningAction Continue "Could not restore $($change.Destination): $_"
                }
            }
            if ($preserveStaging) {
                Write-Warning -WarningAction Continue "Package recovery files kept in $staging"
            }
            throw
        }
    } finally {
        if (!$preserveStaging) {
            $resolvedStage = Get-OrdinaryItem $staging $true
            $resolvedBuild = Get-OrdinaryItem $build $true
            if ($resolvedStage -and $resolvedBuild -and
                $resolvedStage.FullName -eq [IO.Path]::GetFullPath($staging) -and
                $resolvedStage.Parent.FullName -eq $resolvedBuild.FullName) {
                Remove-Item -LiteralPath $resolvedStage.FullName -Recurse -Force
            }
        }
    }
    Write-Host "Portable folder: $packageRoot"
}
