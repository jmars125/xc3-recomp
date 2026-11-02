# Build build\upd\xc3.exe for XC3 2.2.1 with the llvm-mingw toolchain and dependencies of a TotK Recomp setup folder,
# then copy the DLLs and the shader translator (tools/build_xlate.py) next to it. Run from the repository root.
param([string]$Setup = "$PSScriptRoot\..\..\..\totk-pc-recomp",
      [string]$Xlate = "$PSScriptRoot\..\build\xlate")
$ErrorActionPreference = 'Stop'
$Setup = (Resolve-Path $Setup).Path
& "$PSScriptRoot\..\.venv\Scripts\python.exe" "$PSScriptRoot\build_game.py" --version upd `
    --expected "$PSScriptRoot\data\expected-xc3-2.2.1.json" --toolchain "$Setup\llvm" --deps "$Setup\runtime\deps" `
    --ninja "$Setup\bin\ninja.exe" 2>&1 | Select-String -NotMatch 'warning|note:|^\s+\||^\s*\d+ \||^E:/Game|^\s*\^|_Nonnull|_Nullable|^\[\d+/\d+\] CC'
if ($LASTEXITCODE) { exit $LASTEXITCODE }
$out = "$PSScriptRoot\..\build\upd"
Copy-Item "$Setup\bin\SDL3.dll", "$Setup\bin\libopus-0.dll" $out -Force
Copy-Item "$Xlate\xc3_xlate.exe", "$Xlate\xc3_xlate.id" $out -Force
Copy-Item "$Setup\llvm\bin\libc++.dll", "$Setup\llvm\bin\libunwind.dll", "$Setup\llvm\bin\libwinpthread-1.dll" $out -Force
