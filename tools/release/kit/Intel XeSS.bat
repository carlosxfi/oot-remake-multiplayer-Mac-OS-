@echo off
rem Intel XeSS and its native AA for the co-op game (Intel Arc, and most AMD and NVIDIA graphics cards too). Every game
rem has Intel XeSS; it needs only Intel's library (libxess.dll) beside the game: this asks you to accept Intel's license
rem once, then downloads it into the game folder (no game build). Every later build of this kit keeps it.
rem Then: in the game, Graphics, Upscaler: Intel XeSS (or Intel XeSS Native AA).
rem Off again: "Intel XeSS.bat" -NoXess
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" -Xess %*
pause
