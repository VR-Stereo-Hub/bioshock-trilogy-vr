@echo off
setlocal enabledelayedexpansion
rem ===========================================================================
rem  BioShock Remastered VR - graphics setup
rem
rem  The mod cannot set these itself. They live in the GAME's own Bioshock.ini,
rem  which the game reads at startup and rewrites whenever the player touches
rem  the in-game video options - so a value set by hand does not survive, and
rem  this has to be re-run after any visit to that menu.
rem
rem  What it writes, and why each one (all four measured on a live install,
rem  2026-08-22):
rem
rem    HorizontalFOV=100 + both FOV locks
rem        The game ships 130-ish and the lock off. 100 looks near identical in
rem        a headset and runs noticeably better, because the game stops
rem        rendering side content that never reaches the display.
rem
rem    LevelOfAnisotropy=16 in THREE D3D sections
rem        The value exists in five sections. Engine.RenderConfig alone is not
rem        enough - on this machine it read 16 there while all three D3D device
rem        sections read 4, which is what the renderer actually consumes. The
rem        PS4/GNM section is deliberately skipped.
rem
rem    Windowed AND Fullscreen AND Menu viewports
rem        The game resets WindowedViewportX/Y to 640x480 when the player
rem        changes any graphics setting, and StartupFullscreen is False, so the
rem        WINDOWED viewport is the live one. Setting only the fullscreen pair
rem        leaves the mod rendering at 640x480 after one visit to the options.
rem
rem  Modelled on the BRVR mod's Setup.bat, which solved this first.
rem
rem  This file must stay CRLF. cmd tracks its position in a batch file by byte
rem  offset, so LF-only endings make it resume in the wrong place.
rem ===========================================================================

set "RESX=2750"
set "RESY=2850"
set "FOV=100"
set "ANISO=16"

set "INI=%APPDATA%\BioshockHD\Bioshock\Bioshock.ini"
if not "%~1"=="" set "INI=%~1"

echo.
echo  BioShock Remastered VR - graphics setup
echo  ---------------------------------------
echo   Config:  %INI%
echo   Writing: %RESX% x %RESY%, FOV %FOV%, anisotropy x%ANISO%
echo.

if not exist "%INI%" (
    echo  ERROR: Bioshock.ini not found.
    echo.
    echo  Run the game once so it creates its config, then run this again.
    echo  If your config lives somewhere else, pass the full path:
    echo      Setup-BS1.bat "C:\path\to\Bioshock.ini"
    echo.
    pause
    exit /b 1
)

rem  Back up once, and only once, so re-running never overwrites the player's
rem  original with a copy of our own edits.
if not exist "%INI%.vrbackup" (
    copy /y "%INI%" "%INI%.vrbackup" >nul
    echo   Backed up to Bioshock.ini.vrbackup
)

rem  WritePrivateProfileString, not a text rewrite: it edits the named key in
rem  the named section and leaves every other line, comment and section alone.
powershell -NoProfile -ExecutionPolicy Bypass -Command "$d='[DllImport(\"kernel32\")] public static extern bool WritePrivateProfileString(string a, string b, string c, string d);'; Add-Type -MemberDefinition $d -Name Ini -Namespace W32 | Out-Null; $f='%INI%'; $w='WinDrv.WindowsClient'; $u='ShockGame.ShockUserSettings'; $r='Engine.RenderConfig'; $set=@(@($w,'WindowedViewportX','%RESX%'),@($w,'WindowedViewportY','%RESY%'),@($w,'FullscreenViewportX','%RESX%'),@($w,'FullscreenViewportY','%RESY%'),@($w,'MenuViewportX','%RESX%'),@($w,'MenuViewportY','%RESY%'),@($u,'HorizontalFOV','%FOV%'),@($u,'bHorizontalFOVLock','True'),@($r,'HorizontalFOVLock','True'),@($r,'LevelOfAnisotropy','%ANISO%'),@('D3DDrv.D3DRenderDevice','LevelOfAnisotropy','%ANISO%'),@('D3DDrv10.D3DRenderDevice10','LevelOfAnisotropy','%ANISO%'),@('D3DDrv11.D3DRenderDevice11','LevelOfAnisotropy','%ANISO%')); foreach($k in $set){ [void][W32.Ini]::WritePrivateProfileString($k[0],$k[1],$k[2],$f) }; [void][W32.Ini]::WritePrivateProfileString($null,$null,$null,$f)"

echo.
echo   Result:
powershell -NoProfile -ExecutionPolicy Bypass -Command "$f='%INI%'; $s=''; Get-Content $f | ForEach-Object { if($_ -match '^\[(.+)\]'){ $s=$matches[1] }; if($_ -match '^(HorizontalFOV|bHorizontalFOVLock|HorizontalFOVLock|LevelOfAnisotropy|WindowedViewportX|FullscreenViewportX)='){ '{0,-32} {1}' -f $s, $_ } }"

echo.
echo   Done. Re-run this after ANY visit to the in-game video options -
echo   the game resets the viewport to 640x480 and anisotropy to x4 there.
echo.
pause
