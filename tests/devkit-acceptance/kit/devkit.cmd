@echo off
setlocal DisableDelayedExpansion
set "W64DEVKIT_HOME=%~dp0w64devkit"
set "PATH=%W64DEVKIT_HOME%\bin;%SystemRoot%\System32;%SystemRoot%"
set "CC=gcc"
set "CXX=g++"
if "%~1"=="" goto shell
%*
exit /b %errorlevel%

:shell
"%SystemRoot%\System32\cmd.exe" /d
exit /b %errorlevel%
