@echo off
rem AMD FSR 4 and its native AA for the co-op game (AMD Radeon graphics cards, RX 6000 and newer). Longshot has FSR 4 only
rem in a game built with it and with AMD's FSR runtime beside it: this asks you to accept AMD's license once, then builds
rem the game again with FSR 4 and AMD's runtime it downloads (much shorter than the first build). Every later build of
rem this kit keeps FSR 4. Then: in the game, Graphics, Upscaler: AMD FSR 4 (or AMD FSR 4 Native AA).
rem Off again: "AMD FSR 4.bat" -NoFsr4
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0scripts\hookshot-build.ps1" -Fsr4 %*
pause
