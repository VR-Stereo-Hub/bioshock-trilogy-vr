# external-refs.ps1 - clone the third-party reference trees this repo reads (never commits)
# into the gitignored external\, each at the commit docs/MODDING_SDKS.md was written against.
#
#   .\tools\external-refs.ps1            # clone what is missing, report what is there
#   .\tools\external-refs.ps1 -Latest    # also fetch and report how far each is behind upstream
#
# Never runs anything from the trees it clones. The Unofficial BioShock Editor ships an
# installer (BioShockSDK-*-Setup.exe) and carries no licence: read it, do not run it on a
# machine you care about (docs/MODDING_SDKS.md says why it is not needed for Remastered).
# NOTE: keep this file pure ASCII (PowerShell 5.1 misreads BOM-less UTF-8).
param([switch]$Latest)
$ErrorActionPreference = 'Stop'
$root = Join-Path (Split-Path $PSScriptRoot -Parent) "external"
New-Item -ItemType Directory -Force -Path $root | Out-Null

$refs = @(
    @{ Name = "Unofficial-BioShock-Editor"; Url = "https://github.com/bio4554/Unofficial-BioShock-Editor.git";
       Branch = "development"; Commit = "de8b3c72287428fb70f996d835904733c81d7277" },
    @{ Name = "CodeRed-Generator"; Url = "https://github.com/CodeRedModding/CodeRed-Generator.git";
       Branch = "main"; Commit = "ce9c43803b4d03d9cc72b2c762a66ddd5b5695e4" }
)

foreach ($r in $refs) {
    $dir = Join-Path $root $r.Name
    if (-not (Test-Path -LiteralPath (Join-Path $dir ".git"))) {
        Write-Output "cloning $($r.Name) ..."
        git clone --quiet --branch $r.Branch $r.Url $dir
        if ($LASTEXITCODE -ne 0) { throw "git clone $($r.Url) failed" }
    }
    $head = (git -C $dir rev-parse HEAD).Trim()
    $note = if ($head -eq $r.Commit) { "at the documented commit" } else { "NOT the documented commit $($r.Commit.Substring(0, 7))" }
    Write-Output ("{0,-28} {1}  {2}" -f $r.Name, $head.Substring(0, 7), $note)
    if ($Latest) {
        git -C $dir fetch --quiet origin $r.Branch
        $behind = (git -C $dir rev-list --count "HEAD..origin/$($r.Branch)").Trim()
        Write-Output ("{0,-28} {1} commit(s) behind origin/{2}" -f "", $behind, $r.Branch)
    }
}
