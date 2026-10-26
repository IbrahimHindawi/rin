<#
.SYNOPSIS
    Install rin syntax highlighting into Visual Studio 2022 or later.

.DESCRIPTION
    Visual Studio colours a language from TextMate grammars placed under
    %USERPROFILE%\.vs\Extensions\<name>\Syntaxes\. This copies
    editors\textmate\rin.tmLanguage.json there (and language-configuration.json
    next to it). The grammar's "fileTypes" maps *.rin to it. Restart Visual
    Studio afterwards.

.PARAMETER Uninstall
    Remove the installed folder instead.

.PARAMETER DryRun
    Show what would happen; write nothing.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File editors\visualstudio\install.ps1
#>
param(
    [switch]$Uninstall,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'

$textmate = Join-Path (Split-Path -Parent $PSScriptRoot) 'textmate'
$target = Join-Path $env:USERPROFILE '.vs\Extensions\rin'
$syntaxes = Join-Path $target 'Syntaxes'

$files = @(
    @{ From = (Join-Path $textmate 'rin.tmLanguage.json'); To = (Join-Path $syntaxes 'rin.tmLanguage.json') },
    @{ From = (Join-Path $textmate 'language-configuration.json'); To = (Join-Path $target 'language-configuration.json') }
)

Write-Host "target: $target"

if ($Uninstall) {
    if (Test-Path $target) {
        Write-Host "  remove $target"
        if (-not $DryRun) { Remove-Item -Recurse -Force -Confirm:$false $target }
    } else {
        Write-Host '  nothing installed'
    }
    if ($DryRun) { Write-Host 'dry run only, nothing was removed' }
    exit 0
}

foreach ($f in $files) {
    if (-not (Test-Path $f.From)) {
        Write-Error "missing $($f.From)"
    }
}

foreach ($f in $files) {
    Write-Host "  copy $($f.From) -> $($f.To)"
    if (-not $DryRun) {
        New-Item -ItemType Directory -Force (Split-Path -Parent $f.To) | Out-Null
        Copy-Item -Force $f.From $f.To
    }
}

if ($DryRun) {
    Write-Host 'dry run only, nothing was written'
} else {
    Write-Host 'installed. Restart Visual Studio, then open a .rin file.'
}
