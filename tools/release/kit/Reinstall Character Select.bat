@echo off
rem "Reinstall Character Select.bat" downloads Snail's mod (Character Select: the - menu's Characters page and its NPC characters) and
rem makes it again, in place of the copy there (also a broken or half-made one). It asks whether to include the NPC
rem characters: this is the way to get them later.
rem Close the game first. Your saves, settings and character pick are kept. Then it runs "Start Squadala.bat".
rem All on one line: an update (run by the start) may replace this file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\character-select.ps1" -Do reinstall & (if errorlevel 1 (pause & exit /b 1)) & call "%~dp0Start Squadala.bat" %* & exit /b
