@echo off
rem SPDX-License-Identifier: AGPL-3.0-or-later
rem Windows front end for tools\xsl-run: double-click to be asked for a title folder, or drag a
rem title folder (or .exe) onto this file. Extra arguments are passed through.
setlocal
set "TITLE=%~1"
if "%TITLE%"=="" set /p "TITLE=Title folder or .exe: "
if "%TITLE%"=="" exit /b 1
where python >nul 2>nul || (echo Python 3 is required ^(python.org or scoop^). & pause & exit /b 1)
python "%~dp0xsl-run" "%TITLE%" %2 %3 %4 %5 %6 %7 %8 %9
if errorlevel 1 (echo. & echo xsl-run failed ^(exit %errorlevel%^). & pause)
