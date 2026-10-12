@echo off
rem Regenerate the recompiled C from the dqix-decomp checkout one folder up.
cd /d "%~dp0"
py -3 recomp\build.py --extract ..\extract\usa --config ..\config\usa\arm9 --out ..\build\recomp --hle hle.txt
if errorlevel 1 (echo RECOMPILE FAILED) else (echo Done: ..\build\recomp)
if "%1"=="" pause
