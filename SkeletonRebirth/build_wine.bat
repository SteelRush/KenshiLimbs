@echo off
set VC=C:\Program Files (x86)\Microsoft Visual Studio 10.0\VC
set SDK=C:\Program Files\Microsoft SDKs\Windows\v7.1
if "%DEPS%"=="" set DEPS=Z:\path\to\KenshiLib_Examples_deps
set KLIB=%DEPS%\KenshiLib
set BOOST=%DEPS%\boost_1_60_0
set RAPIDJSON=%DEPS%\rapidjson\include

set INCLUDE=%VC%\include;%SDK%\Include;%KLIB%\Include;%KLIB%\Include\ogre;%BOOST%;%RAPIDJSON%
set LIB=%VC%\lib\amd64;%SDK%\Lib\x64;%KLIB%\Libraries;%BOOST%\stage\lib
set PATH=%VC%\bin\amd64;%PATH%

echo ===compiling SkeletonRebirthDiagnostics.dll===
cl.exe /nologo /EHsc /GL /LD /MD /DUNICODE /D_UNICODE SkeletonRebirthDiagnostics.cpp KenshiLib.lib OgreMain_x64.lib MyGUIEngine_x64.lib /link /out:SkeletonRebirthDiagnostics.dll
echo ===EXITCODE %ERRORLEVEL%===
