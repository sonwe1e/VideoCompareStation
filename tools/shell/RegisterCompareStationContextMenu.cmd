@echo off
rem Pure ASCII on purpose: a .cmd file holding non-ASCII text is parsed in the console OEM code
rem page, so Chinese (or any other non-ASCII) copy here gets mangled on double-click. Every message
rem that reaches the user comes from RegisterCompareStationContextMenu.ps1 through pwsh instead.
rem
rem Double-click entry point for an unpacked CompareStation release: find PowerShell 7, hand over to
rem the script that does the per-user registration, forward any arguments (-Uninstall) untouched, and
rem keep the window open when it was opened by a double-click.
setlocal
cd /d "%~dp0."

set "REGISTER_SCRIPT=%~dp0RegisterCompareStationContextMenu.ps1"
if exist "%REGISTER_SCRIPT%" goto find_pwsh

set "REGISTER_SCRIPT=%~dp0..\..\tools\shell\RegisterCompareStationContextMenu.ps1"
if exist "%REGISTER_SCRIPT%" goto find_pwsh

echo [CompareStation] RegisterCompareStationContextMenu.ps1 was not found.
echo [CompareStation] Unpack the whole release ZIP and run this file from inside that folder.
pause
exit /b 1

:find_pwsh
set "PS_EXE="
rem DVS_PWSH_OVERRIDE pins the interpreter, following this repository's DVS_* environment-override
rem convention. It exists so the "PowerShell 7 is missing" path can be tested without uninstalling
rem PowerShell, and it fails closed: an override that does not point at a file stops here instead of
rem silently falling back to an interpreter the caller did not ask for.
if defined DVS_PWSH_OVERRIDE (
    if exist "%DVS_PWSH_OVERRIDE%" (
        set "PS_EXE=%DVS_PWSH_OVERRIDE%"
    ) else (
        echo [CompareStation] DVS_PWSH_OVERRIDE is set but points at no file:
        echo [CompareStation]     %DVS_PWSH_OVERRIDE%
        echo [CompareStation] Unset it, or point it at pwsh.exe.
        pause
        exit /b 1
    )
)
if not defined PS_EXE if exist "%ProgramFiles%\PowerShell\7\pwsh.exe" set "PS_EXE=%ProgramFiles%\PowerShell\7\pwsh.exe"
if not defined PS_EXE for /f "delims=" %%p in ('where pwsh 2^>nul') do if not defined PS_EXE set "PS_EXE=%%p"

if not defined PS_EXE goto no_pwsh

rem %* has to reach the script unchanged: without it "-Uninstall" was silently dropped and the
rem documented uninstall command registered the command again instead of removing it.
"%PS_EXE%" -NoProfile -ExecutionPolicy Bypass -File "%REGISTER_SCRIPT%" %*
set "REGISTER_EXIT=%ERRORLEVEL%"
if not "%REGISTER_EXIT%"=="0" goto failed

pause
exit /b 0

:no_pwsh
echo.
echo [CompareStation] PowerShell 7 (pwsh) is required and was not found on this PC.
echo [CompareStation] Windows PowerShell 5.1 cannot run the registration script.
echo.
echo [CompareStation] Install PowerShell 7 first (no administrator rights needed):
echo [CompareStation]     winget install --id Microsoft.PowerShell
echo [CompareStation] Then double-click this file again.
pause
exit /b 1

:failed
echo.
echo [CompareStation] Registration failed. The message above says why.
pause
exit /b 1

rem Every exit path pauses so a double-clicked window stays readable. That is safe for scripted
rem callers too: pause reads stdin, so a redirected stdin hits end-of-input and returns at once.
rem An earlier version guarded the pause with "if not exist con" plus "call )" and the window still
rem closed - the quoted device name tests as missing, so the guard itself exited - which is why the
rem unconditional pause is deliberate rather than an oversight.
