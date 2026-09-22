# Add A-90 modifier for any Roblox game

`A90Native.sln` is a Visual Studio x64 Win32 project for the A-90 overlay.

## Build in Visual Studio

1. Open `A90Native.sln` in Visual Studio 2022 or newer.
2. Select `Debug` or `Release` and `x64`.
3. Build the solution.
4. Run `A90Native.exe` with the `assets` folder and `config.txt` beside it.

The project uses only Windows SDK libraries: WIC for WebP decoding, Win32 layered windows for the overlay, and MCI for MP3 playback. No Python or third-party runtime is required.

## Build from a Developer PowerShell

```powershell
msbuild A90Native.sln /m /p:Configuration=Release /p:Platform=x64
```

The executable is written to `x64\Release\A90Native.exe`.

## Smart App Control and distribution

Visual Studio produces an unsigned executable by default. Windows Smart App Control may block unsigned or low-reputation native binaries on other computers. There is no code change that can make an unsigned executable universally trusted.

Sign the compiled .exe before use:

```bat
sign.bat
```

`sign.bat` verifies the Release executable and creates a private self-signed certificate and signs it only when needed. The default test PFX password is `a90-test`; optional arguments are publisher and password.

For public distribution, use a publicly trusted Authenticode certificate or Microsoft Store/MSIX. Self-signed certificates may still be blocked by Smart App Control on some systems.

Do NOT share `a90-private-test.pfx` or its password.

## Controller hub

The native app opens an `A-90 Controller` window with an `A-90 enabled` checkbox. Turning it off hides active overlays and prevents new events. Closing the controller window posts `WM_QUIT` and terminates the native process.