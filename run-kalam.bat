@echo off
setlocal

set "KALAM_ROOT=%~dp0"
set "KALAM_SCENE=%KALAM_ROOT%examples\cube.scene.json"

if not "%~1"=="" (
    set "KALAM_SCENE=%~f1"
)

if not exist "%KALAM_ROOT%hot" (
    mkdir "%KALAM_ROOT%hot"
)

"%KALAM_ROOT%nveditor.exe" ^
    --source-root "%KALAM_ROOT%" ^
    --build-directory "%KALAM_ROOT%" ^
    --module "%KALAM_ROOT%game_module.dll" ^
    --hot-directory "%KALAM_ROOT%hot" ^
    --runtime-directory "%KALAM_ROOT%nvim-runtime" ^
    --neovim "%KALAM_ROOT%neovim\bin\nvim.exe" "%KALAM_SCENE%"
