param(
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build'),
    [string]$Board = 'waveshare_rp2040_zero'
)

$projectDirectory = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$buildDirectoryFull = [IO.Path]::GetFullPath($BuildDirectory)
$sdkInstallCandidates = @(
    'C:\Program Files\Raspberry Pi\Pico SDK v1.5.1',
    'C:\Program Files\Raspberry Pi\Pico SDK'
)

if (-not $env:PICO_SDK_PATH) {
    foreach ($candidate in $sdkInstallCandidates) {
        if (Test-Path "$candidate\pico-sdk") {
            $env:PICO_SDK_PATH = "$candidate\pico-sdk"
            $sdkInstall = $candidate
            break
        }
    }
}

if (-not $env:PICO_SDK_PATH) {
    throw 'PICO_SDK_PATH environment variable is not set. Please set PICO_SDK_PATH to your Pico SDK directory (e.g., $env:PICO_SDK_PATH="C:\path\to\pico-sdk") before running this script.'
}

$cmake = 'cmake'
$configureArguments = @('-S', $projectDirectory, '-B', $buildDirectoryFull,
                        "-DPICO_BOARD=$Board", '-DCMAKE_BUILD_TYPE=Release')

if (Test-Path "$sdkInstall\cmake\bin\cmake.exe") {
    $cmake = "$sdkInstall\cmake\bin\cmake.exe"
}

if (Test-Path "$sdkInstall\ninja\ninja.exe") {
    $configureArguments += @('-G', 'Ninja',
        "-DCMAKE_MAKE_PROGRAM=$sdkInstall\ninja\ninja.exe")
}

if (-not (Get-Command arm-none-eabi-gcc -ErrorAction SilentlyContinue) -and
    (Test-Path "$sdkInstall\gcc-arm-none-eabi")) {
    $configureArguments +=
        "-DPICO_TOOLCHAIN_PATH=$sdkInstall\gcc-arm-none-eabi"
}

& $cmake @configureArguments
if ($LASTEXITCODE -ne 0) {
    exit $LASTEXITCODE
}

& $cmake --build $buildDirectoryFull --parallel
exit $LASTEXITCODE
