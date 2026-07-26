# CEF Bridge Build

This directory builds the runtime-loaded CEF bridge used by the game when `ENABLE_BROWSER=1`.

Windows outputs:
- `lib/cef/win64/bridge/browser_backend_cef.dll`
- `lib/cef/win64/bridge/browser_subprocess.dll`

Linux outputs:
- `lib/cef/linux64/bridge/browser_backend_cef.so`
- `lib/cef/linux64/bridge/browser_subprocess`

Build on Windows with Visual Studio 2022 x64:

```powershell
cmake -S tools/browser_backend_cef -B build/browser_backend_cef -G "Visual Studio 17 2022" -A x64
cmake --build build/browser_backend_cef --config Release
```

Build on Linux:

```bash
cmake -S tools/browser_backend_cef -B build/browser_backend_cef -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/browser_backend_cef
```

The normal game build then copies the matching CEF `Release/*`, `Resources/*`, bridge library, and subprocess into `build/us_pc/cef_resources`.
