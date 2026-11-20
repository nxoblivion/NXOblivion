@echo off
rem Build the SDK's PPC instruction tests (Release only) into out/build/tests and run them.
cd /d "%~dp0..\thirdparty\rexglue-sdk" || exit /b 1
call "%~dp0vsenv.bat" cmake -S . -B out/build/tests -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ "-DCMAKE_CXX_FLAGS=-march=x86-64-v2" "-DCMAKE_C_FLAGS=-march=x86-64-v2" -DREXGLUE_BUILD_TESTS=ON || exit /b 1
call "%~dp0vsenv.bat" cmake --build out/build/tests --target ppc_tests || exit /b 1
