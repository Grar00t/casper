@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "ROOT=%~dp0.."
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSROOT="
if not exist "%VSWHERE%" (
  echo ERROR: vswhere not installed
  exit /b 2
)
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSROOT=%%I"
if not defined VSROOT (
  echo ERROR: No MSVC C compiler installation found
  exit /b 2
)
call "%VSROOT%\Common7\Tools\VsDevCmd.bat" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b 2
cd /d "%ROOT%"
if not exist "build\release" mkdir "build\release"
set "CFLAGS=/nologo /std:c11 /W4 /WX /wd4996 /MT /O2"
cl %CFLAGS% /I"include" /I"Core_CPP" /Fe:"build\release\casper_engine.exe" Core_CPP\niyah_hybrid_main.c Core_CPP\niyah_core.c Core_CPP\hybrid_reasoner.c Core_CPP\constraint_solver.c Core_CPP\rule_parser.c Core_CPP\rule_source_guard.c Core_CPP\proof_generator.c Core_CPP\khz_q_svd.c Core_CPP\casper_rag.c src\niyah_gguf_map.c tokenizer.c /link winhttp.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /LD /I"include" /Fe:"build\release\casper.dll" src\casper_ffi.c
if errorlevel 1 exit /b 1
cl %CFLAGS% /Fe:"build\release\test_ffi_host.exe" tests\test_ffi_host.c
if errorlevel 1 exit /b 1
cl %CFLAGS% /I"Core_CPP" /Fe:"build\release\test_rag_json.exe" tests\test_rag_json.c Core_CPP\proof_generator.c /link winhttp.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /I"Core_CPP" /Fe:"build\release\make_proof_fixture.exe" tests\make_proof_fixture.c Core_CPP\proof_generator.c
if errorlevel 1 exit /b 1
cl %CFLAGS% /I"Core_CPP" /Fe:"build\release\test_proof_v1.exe" tests\test_proof_v1.c Core_CPP\proof_generator.c
if errorlevel 1 exit /b 1
cl %CFLAGS% /I"include" /I"Core_CPP" /Fe:"build\release\casper_cli.exe" Core_CPP\casper_cli.c Core_CPP\casper_rag.c Core_CPP\rule_parser.c Core_CPP\rule_source_guard.c Core_CPP\proof_generator.c Core_CPP\khz_q_svd.c /link winhttp.lib
if errorlevel 1 exit /b 1
cl %CFLAGS% /I"include" /I"Core_CPP" /Fe:"build\release\test_gguf_infer.exe" tests\test_gguf_infer.c Core_CPP\niyah_core.c src\niyah_gguf_map.c
if errorlevel 1 exit /b 1
if /i not "%~1"=="--smoke" exit /b 0
"build\release\casper_engine.exe" --smoke
if errorlevel 1 exit /b 1
"build\release\make_proof_fixture.exe" "build\release\fixture.proof" "build\release\v1_fixture.proof" "build\release\v1_rules_fixture.bin"
if errorlevel 1 exit /b 1
"build\release\casper_engine.exe" --verify-proof "build\release\fixture.proof"
if errorlevel 1 exit /b 1
"build\release\test_rag_json.exe"
if errorlevel 1 exit /b 1
"build\release\test_ffi_host.exe"
if errorlevel 1 exit /b 1
"build\release\test_gguf_infer.exe"
if errorlevel 1 exit /b 1
"build\release\test_proof_v1.exe"
if errorlevel 1 exit /b 1
"build\release\casper_engine.exe" --verify-proof "build\release\v1_fixture.proof" "build\release\v1_rules_fixture.bin"
if errorlevel 1 exit /b 1
"build\release\casper_cli.exe" --self-check
if errorlevel 1 exit /b 1
"build\release\casper_cli.exe" --verify "build\release\v1_fixture.proof" "build\release\v1_rules_fixture.bin"
if errorlevel 1 exit /b 1
echo RELEASE_SMOKE=PASS
exit /b 0
