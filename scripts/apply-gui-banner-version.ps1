#requires -Version 5.1
[CmdletBinding()]
param(
    [Parameter(Mandatory = $false)]
    [ValidatePattern('^\d+\.\d+\.\d+-gui\d+(?:\.\d+)?(?:-[a-z0-9]+)?$')]
    [string]$Version,

    [string]$OutputPath
)

$ErrorActionPreference = 'Stop'
Add-Type -AssemblyName System.Drawing

$assetDirectory = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\3ds\res'))
$templatePath = Join-Path $assetDirectory 'banner-template.png'
if ([string]::IsNullOrWhiteSpace($Version)) {
    $diagnosticsHeader = Join-Path $PSScriptRoot '..\src\diagnostics.h'
    $headerText = [System.IO.File]::ReadAllText($diagnosticsHeader)
    $match = [regex]::Match($headerText, '(?m)^#define DIAG_BUILD_VERSION "([^"]+)"\r?$')
    if (-not $match.Success) {
        throw "DIAG_BUILD_VERSION not found in $diagnosticsHeader."
    }
    $Version = $match.Groups[1].Value
}
if ($Version -notmatch '^\d+\.\d+\.\d+-gui\d+(?:\.\d+)?(?:-[a-z0-9]+)?$') {
    throw "Unsupported GUI version: $Version"
}
if ([string]::IsNullOrWhiteSpace($OutputPath)) {
    $OutputPath = Join-Path $assetDirectory 'banner.png'
}
$outputFullPath = [System.IO.Path]::GetFullPath($OutputPath)

if (-not (Test-Path -LiteralPath $templatePath -PathType Leaf)) {
    throw "Banner template not found: $templatePath"
}
if ([string]::Equals($outputFullPath, [System.IO.Path]::GetFullPath($templatePath), [StringComparison]::OrdinalIgnoreCase)) {
    throw 'OutputPath must not overwrite banner-template.png.'
}

$bitmap = [System.Drawing.Bitmap]::new($templatePath)
$graphics = $null
$font = $null
$brush = $null
$format = $null
try {
    if ($bitmap.Width -ne 256 -or $bitmap.Height -ne 128) {
        throw "Expected a 256x128 banner template; found $($bitmap.Width)x$($bitmap.Height)."
    }

    $graphics = [System.Drawing.Graphics]::FromImage($bitmap)
    $graphics.TextRenderingHint = [System.Drawing.Text.TextRenderingHint]::AntiAliasGridFit
    $font = [System.Drawing.Font]::new('Arial', 11.0, [System.Drawing.FontStyle]::Regular, [System.Drawing.GraphicsUnit]::Pixel)
    $brush = [System.Drawing.SolidBrush]::new([System.Drawing.Color]::White)
    $format = [System.Drawing.StringFormat]::new()
    $format.Alignment = [System.Drawing.StringAlignment]::Center
    $format.LineAlignment = [System.Drawing.StringAlignment]::Center

    # The fixed text field centers the version under the Moonlight wordmark.
    $textField = [System.Drawing.RectangleF]::new(115.0, 107.5, 99.0, 16.0)
    $graphics.DrawString($Version, $font, $brush, $textField, $format)

    $outputDirectory = [System.IO.Path]::GetDirectoryName($outputFullPath)
    if (-not (Test-Path -LiteralPath $outputDirectory -PathType Container)) {
        New-Item -ItemType Directory -Path $outputDirectory -Force | Out-Null
    }
    $bitmap.Save($outputFullPath, [System.Drawing.Imaging.ImageFormat]::Png)
}
finally {
    if ($format) { $format.Dispose() }
    if ($brush) { $brush.Dispose() }
    if ($font) { $font.Dispose() }
    if ($graphics) { $graphics.Dispose() }
    if ($bitmap) { $bitmap.Dispose() }
}

Write-Output "Wrote $outputFullPath (256x128, $Version)."
