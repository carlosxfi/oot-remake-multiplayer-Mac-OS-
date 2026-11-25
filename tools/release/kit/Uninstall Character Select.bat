@echo off
rem "Uninstall Character Select.bat" takes Snail's mod (Character Select: the - menu's Characters page and its NPC characters) out of
rem the game: the start that follows builds the game again without it. It stays out until "Reinstall Character Select.bat".
rem Close the game first. Your saves, settings and character pick are kept. Then it runs "Start Squadala.bat".
rem All on one line: an update (run by the start) may replace this file while it runs.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\character-select.ps1" -Do uninstall & (if errorlevel 1 (pause & exit /b 1)) & call "%~dp0Start Squadala.bat" %* & exit /b
