@echo off
set VC=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC
set SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1
set DEPS=Z:\tmp\claude-1000\-home-bryan-Git-RE-Kenshi\907f67df-a82b-47ba-83ea-dece40268f59\scratchpad\KenshiLib_Examples_deps
set KLIB=%DEPS%\KenshiLib
set BOOST=%DEPS%\boost_1_60_0

set INCLUDE=%VC%\include;%SDK%\Include;%KLIB%\Include;%BOOST%
set LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KLIB%\Libraries;%BOOST%\stage\lib
set PATH=%VC%\bin\amd64;%PATH%

echo ===compiling RobotLimbRaceLock.dll===
cl.exe /nologo /EHsc /GL /LD /DUNICODE /D_UNICODE RobotLimbRaceLock.cpp KenshiLib.lib OgreMain_x64.lib /link /out:RobotLimbRaceLock.dll
echo ===EXITCODE %ERRORLEVEL%===
