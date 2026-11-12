# Build docs/report/report.pdf from report.html with Microsoft Edge (headless).
#
# Usage (PowerShell, from anywhere):
#   .\docs\report\build.ps1                                  # author info from author.local.json
#   .\docs\report\build.ps1 -Name "..." -StudentId "..."     # or pass it directly
#
# The repository is public, so report.html keeps the {{NAME}} / {{STUDENT_ID}} placeholders.
# They are filled into a temporary copy only when the PDF is built; author.local.json and the
# temporary copy are listed in .gitignore.
param(
    [string]$Name,
    [string]$StudentId
)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$html = Join-Path $here 'report.html'
$tmp  = Join-Path $here '.build.html'      # same folder, so relative image paths still work
$pdf  = Join-Path $here 'report.pdf'

$authorFile = Join-Path $here 'author.local.json'
if ((-not $Name -or -not $StudentId) -and (Test-Path $authorFile)) {
    $author = Get-Content $authorFile -Raw -Encoding UTF8 | ConvertFrom-Json
    if (-not $Name)      { $Name = $author.name }
    if (-not $StudentId) { $StudentId = $author.student_id }
}
if (-not $Name -or -not $StudentId) {
    Write-Warning 'No author info: the cover will show {{NAME}} / {{STUDENT_ID}}.'
}

$edge = @(
    "${env:ProgramFiles(x86)}\Microsoft\Edge\Application\msedge.exe",
    "$env:ProgramFiles\Microsoft\Edge\Application\msedge.exe"
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $edge) { throw 'Microsoft Edge not found.' }

# Fill placeholders into the temporary copy (UTF-8 without BOM)
$content = [System.IO.File]::ReadAllText($html, [System.Text.Encoding]::UTF8)
if ($Name)      { $content = $content.Replace('{{NAME}}', [System.Net.WebUtility]::HtmlEncode($Name)) }
if ($StudentId) { $content = $content.Replace('{{STUDENT_ID}}', [System.Net.WebUtility]::HtmlEncode($StudentId)) }
[System.IO.File]::WriteAllText($tmp, $content, (New-Object System.Text.UTF8Encoding $false))

try {
    $uri = ([System.Uri]$tmp).AbsoluteUri
    if (Test-Path $pdf) { Remove-Item $pdf }
    # Start-Process: Edge writes harmless warnings to stderr, which Windows PowerShell 5.1
    # would otherwise turn into a terminating error under ErrorActionPreference = Stop.
    $edgeArgs = @('--headless=new', '--disable-gpu', '--no-pdf-header-footer',
                  '--run-all-compositor-stages-before-draw', '--virtual-time-budget=5000',
                  "--print-to-pdf=`"$pdf`"", $uri)
    Start-Process -FilePath $edge -ArgumentList $edgeArgs -Wait -WindowStyle Hidden
}
finally {
    Remove-Item $tmp -ErrorAction SilentlyContinue
}
if (-not (Test-Path $pdf)) { throw 'PDF was not created.' }
Write-Host "Wrote $pdf ($([math]::Round((Get-Item $pdf).Length / 1KB)) KB)"
