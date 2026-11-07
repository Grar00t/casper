@echo off
setlocal EnableExtensions DisableDelayedExpansion
set "ROOT=%~dp0.."
set "VS=C:\Program Files\Microsoft Visual Studio\2022\Community\Common7\Tools\VsDevCmd.bat"
if not exist "%VS%" (
  echo ERROR: Visual Studio 2022 Developer Command Prompt not found.
  exit /b 2
)
call "%VS%" -arch=amd64 -host_arch=amd64 >nul
if errorlevel 1 exit /b 2
cd /d "%ROOT%"
if not exist "build" mkdir "build"
set "FLAGS=/nologo /std:c11 /W4 /WX /wd4996 /O2"
set "BINARY=build\casper_engine.exe"
if /i "%~1"=="--asan" (
  set "FLAGS=/nologo /std:c11 /W4 /WX /wd4996 /Od /Zi /fsanitize=address"
  set "BINARY=build\casper_engine_asan.exe"
)
cl %FLAGS% /I"include" /I"Core_CPP" /Fe:"%BINARY%" Core_CPP\niyah_hybrid_main.c Core_CPP\niyah_core.c Core_CPP\hybrid_reasoner.c Core_CPP\constraint_solver.c Core_CPP\rule_parser.c Core_CPP\rule_source_guard.c Core_CPP\proof_generator.c Core_CPP\khz_q_svd.c Core_CPP\casper_rag.c src\niyah_gguf_map.c tokenizer.c /link winhttp.lib
if errorlevel 1 exit /b 1
if /i "%~1"=="--smoke" (
  "%BINARY%" --smoke
  if errorlevel 1 exit /b 1
  cl /nologo /std:c11 /W4 /WX /wd4996 /O2 /I"Core_CPP" /Fe:"build\make_proof_fixture.exe" tests\make_proof_fixture.c Core_CPP\proof_generator.c
  if errorlevel 1 exit /b 1
  "build\make_proof_fixture.exe" "build\fixture.proof"
  if errorlevel 1 exit /b 1
  "%BINARY%" --verify-proof "build\fixture.proof"
  if errorlevel 1 exit /b 1
  rem Guard source attribution against reordered fields and nested JSON.
  cl /nologo /std:c11 /W4 /WX /wd4996 /O2 /I"Core_CPP" /Fe:"build\test_rag_json.exe" tests\test_rag_json.c Core_CPP\proof_generator.c /link winhttp.lib
  if errorlevel 1 exit /b 1
  "build\test_rag_json.exe"
  if errorlevel 1 exit /b 1
)
if /i "%~1"=="--asan" (
  "%BINARY%" --smoke
  if errorlevel 1 exit /b 1
)
exit /b 0
