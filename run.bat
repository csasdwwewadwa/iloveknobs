@echo off
setlocal
set "PUBLISHER=%~1"
set "PASSWORD=%~2"
if not defined PUBLISHER set "PUBLISHER=A90 Private Test Publisher"
if not defined PASSWORD set "PASSWORD=a90-test"
set "EXE=%~dp0x64\Release\A90Native.exe"
set "PFX=%~dp0a90-private-test.pfx"
if not exist "%EXE%" (
    echo Build x64 Release first: "%EXE%"
    exit /b 1
)

set "SIGNTOOL="
for /r "%ProgramFiles(x86)%\Windows Kits\10\bin" %%F in (signtool.exe) do if /i "%%~dpF"=="%ProgramFiles(x86)%\Windows Kits\10\bin\10.0.26100.0\x64\" set "SIGNTOOL=%%F"
if not defined SIGNTOOL for /r "%ProgramFiles(x86)%\Windows Kits\10\bin" %%F in (signtool.exe) do if not defined SIGNTOOL echo %%~dpF | findstr /i "\\x64\\$" >nul && set "SIGNTOOL=%%F"
if not defined SIGNTOOL (
    echo Windows SDK x64 signtool.exe was not found.
    exit /b 1
)

"%SIGNTOOL%" verify /pa /all "%EXE%" >nul 2>&1
if errorlevel 1 (
    echo Executable is not publicly trusted; creating a private test signature...
    set "A90_PUBLISHER=%PUBLISHER%"
    set "A90_PASSWORD=%PASSWORD%"
    set "A90_EXE=%EXE%"
    set "A90_PFX=%PFX%"
    set "A90_SIGNTOOL=%SIGNTOOL%"
    powershell.exe -NoProfile -ExecutionPolicy Bypass -Command "$ErrorActionPreference='Stop'; $publisher=$env:A90_PUBLISHER; $password=[string]$env:A90_PASSWORD; $exe=$env:A90_EXE; $pfx=$env:A90_PFX; $tool=$env:A90_SIGNTOOL; $cert=Get-ChildItem Cert:\\CurrentUser\\My | Where-Object { $_.Subject -eq ('CN='+$publisher) -and $_.HasPrivateKey } | Select-Object -First 1; if (-not $cert) { $cert=New-SelfSignedCertificate -Type CodeSigningCert -Subject ('CN='+$publisher) -FriendlyName $publisher -CertStoreLocation Cert:\\CurrentUser\\My -HashAlgorithm SHA256 -NotAfter (Get-Date).AddYears(2) }; $secure=ConvertTo-SecureString $password -AsPlainText -Force; Export-PfxCertificate -Cert $cert -FilePath $pfx -Password $secure | Out-Null; if (-not (Get-ChildItem Cert:\\CurrentUser\\TrustedPublisher | Where-Object Thumbprint -eq $cert.Thumbprint)) { $cer=Join-Path $env:TEMP 'a90-private-test.cer'; Export-Certificate -Cert $cert -FilePath $cer | Out-Null; Import-Certificate -FilePath $cer -CertStoreLocation Cert:\\CurrentUser\\TrustedPublisher | Out-Null }; & $tool sign /fd SHA256 /f $pfx /p $password $exe; if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }; exit 0"
    if errorlevel 1 exit /b 1
) else (
    echo Executable signature already verified.
)

start "A-90 Native" "%EXE%"
endlocal
