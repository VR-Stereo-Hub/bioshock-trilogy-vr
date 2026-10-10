# End-to-end tests of the built EXE. All destinations are a new scratch tree.
param([string]$Exe='', [string]$OutDir='')
$ErrorActionPreference='Stop'
$repo=Split-Path -Parent $PSScriptRoot
if (-not $Exe) { $version=([regex]::Match((Get-Content -LiteralPath "$repo\CMakeLists.txt" -Raw),'project\(BioshockVR\s+VERSION\s+([0-9.]+)')).Groups[1].Value; $Exe="$repo\build\src\RelWithDebInfo\BioShockVR-Launcher-v$version.exe" }
$Exe=(Resolve-Path -LiteralPath $Exe).Path
if (-not $OutDir) { $OutDir="$repo\build\launcher-e2e-$([guid]::NewGuid().ToString('N'))" }
$root=[IO.Path]::GetFullPath($OutDir)
if (Test-Path -LiteralPath $root) { throw 'Use a new scratch directory' }
New-Item -ItemType Directory -Path $root | Out-Null
$game=Join-Path $root 'game\Build\Final'; $data=Join-Path $root 'data'; $ini=Join-Path $root 'config\Bioshock.ini'
New-Item -ItemType Directory -Path $game,$data,(Split-Path -Parent $ini) | Out-Null
# A nonexecutable PE header fixture, never a game copy and never started.
$fake=New-Object byte[] 512; $fake[0]=77; $fake[1]=90; $fake[60]=128; $fake[128]=80; $fake[129]=69; $fake[132]=76; $fake[133]=1; $fake[152]=11; $fake[153]=1
[IO.File]::WriteAllBytes((Join-Path $game 'BioshockHD.exe'),$fake)
$oldIni="; player file`r`n[WinDrv.WindowsClient]`r`nWindowedViewportX=1280`r`nWindowedViewportY=720`r`nFullscreenViewportX=1920`r`nFullscreenViewportY=1080`r`nStartupFullscreen=False`r`n[Other]`r`nvalue=kept`r`n"
[IO.File]::WriteAllText($ini,$oldIni,[Text.UTF8Encoding]::new($false))
$preferences="# keep this comment`r`nP SnapTurn -1 - 0`r`nP PlaceForward 1 Pistol 2.5`r`nD HandMode -1 - 3`r`nP FutureSetting -1 - 12`r`n"
[IO.File]::WriteAllText((Join-Path $data 'menu-settings.ini'),$preferences,[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $data 'hands.ini'),"player hand calibration`r`n",[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $data 'xr.ini'),"[runtime]`r`nmode=auto`r`ncustom=keep`r`n",[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $game 'xinput1_3.dll'),'original third-party file',[Text.UTF8Encoding]::new($false))
[IO.File]::WriteAllText((Join-Path $game 'NeverTouch.sav'),'save sentinel',[Text.UTF8Encoding]::new($false))
$script:checks=0
function Check([bool]$Passed,[string]$Label) { $script:checks++; if (-not $Passed) { throw "FAILED: $Label" } }
function Run([string]$Name,[string]$Options) {
    $result=Join-Path $root ($Name+'.json')
    $args=$Options+' --game-dir "'+$game+'" --data-dir "'+$data+'" --game-ini "'+$ini+'" --result "'+$result+'"'
    $p=Start-Process -FilePath $Exe -ArgumentList $args -WindowStyle Hidden -Wait -PassThru
    Check ($p.ExitCode -eq 0) ($Name+' exit status')
    Get-Content -LiteralPath $result -Raw | ConvertFrom-Json
}
$first=Run 'before' '--inspect'; Check $first.found 'fixture discovered'; Check $first.payloadOk 'real embedded payload validated'
$install=Run 'install' '--apply --op install'; Check $install.ok 'install report'
Check ([IO.File]::ReadAllText($ini) -ceq $oldIni) 'install preserves complete game INI'
Check ([IO.File]::ReadAllText((Join-Path $data 'menu-settings.ini')) -ceq $preferences) 'install preserves complete F10 file'
$installed=Run 'installed' '--inspect'; Check $installed.matchesPayload 'all four installed hashes match resources'
foreach($f in $installed.payload.PSObject.Properties) { Check ((Get-FileHash -LiteralPath (Join-Path $game $f.Name) -Algorithm SHA256).Hash.ToLower() -eq $f.Value) ('installed '+$f.Name) }
$settings=Run 'settings' '--apply --op settings --runtime steamvr --size 2750x2850 --set SnapTurn=1'
Check $settings.ok 'settings report'
$expected=$oldIni.Replace('X=1280','X=2750').Replace('Y=720','Y=2850').Replace('X=1920','X=2750').Replace('Y=1080','Y=2850')
Check ([IO.File]::ReadAllText($ini) -ceq $expected) 'whole game INI diff contains precisely four edits with CRLF'
Check ([IO.File]::ReadAllText((Join-Path $data 'menu-settings.ini')) -ceq $preferences.Replace('P SnapTurn -1 - 0','P SnapTurn -1 - 1')) 'whole F10 diff contains precisely one edit'
Check ([IO.File]::ReadAllText((Join-Path $data 'xr.ini')) -ceq "[runtime]`r`nmode=steamvr`r`ncustom=keep`r`n") 'runtime diff preserves unknown key and CRLF'
$disabled=Run 'disable' '--apply --op disable'; Check $disabled.ok 'disable report'; Check (-not (Test-Path -LiteralPath (Join-Path $game 'bioshockvr.dll'))) 'mod parked'
$enabled=Run 'enable' '--apply --op enable'; Check $enabled.ok 'enable report'
$support=Run 'support' ('--support --support-out "'+(Join-Path $root 'support')+'"'); Check $support.ok 'embedded support collector report'
$zip=@(Get-ChildItem -LiteralPath (Join-Path $root 'support') -Filter *.zip); Check ($zip.Count -eq 1) 'support ZIP exists'
Add-Type -AssemblyName System.IO.Compression.FileSystem
$archive=[IO.Compression.ZipFile]::OpenRead($zip[0].FullName)
try { $names=@($archive.Entries | ForEach-Object FullName); Check ($names -contains 'game-Bioshock.ini') 'game config collected'; Check ($names -contains 'hands.ini') 'hand calibration collected'; Check (@($names | Where-Object {$_ -match '\.(dll|exe|sav)$'}).Count -eq 0) 'support excludes binaries and saves' } finally {$archive.Dispose()}
$uninstall=Run 'uninstall' '--apply --op uninstall'; Check $uninstall.ok 'uninstall report'
Check ([IO.File]::ReadAllText((Join-Path $game 'xinput1_3.dll')) -ceq 'original third-party file') 'original third party file restored'
Check ([IO.File]::ReadAllText((Join-Path $data 'hands.ini')) -ceq "player hand calibration`r`n") 'calibration untouched through lifecycle'
Check ([IO.File]::ReadAllText((Join-Path $game 'NeverTouch.sav')) -ceq 'save sentinel') 'save untouched'
Check (-not (Test-Path -LiteralPath (Join-Path $game 'bioshockvr-install.json'))) 'ownership record removed'
[ordered]@{ Checks=$script:checks; Failures=0; Exe=$Exe; SHA256=(Get-FileHash -LiteralPath $Exe -Algorithm SHA256).Hash; Scratch=$root } | ConvertTo-Json | Set-Content -LiteralPath (Join-Path $root 'summary.json')
Write-Output "Launcher EXE tests: $script:checks checks, 0 failures. Evidence: $root"
