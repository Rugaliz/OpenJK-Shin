<#
.SYNOPSIS
    Builds every game in this repository on Windows.

.DESCRIPTION
    Builds
      Jedi Outcast  single player   (openjo_sp)
      Jedi Outcast  multiplayer     (openjo client, openjoded dedicated server)
      Jedi Academy  single player   (openjk_sp)
      Jedi Academy  multiplayer     (openjk client, openjkded dedicated server, game/cgame/ui modules, renderers)

    It checks that what the build needs is installed (and says how to install what is missing),
    configures with CMake, builds with Visual Studio, and copies the result to a folder laid out like
    the game folders, ready to be copied into the GameData folder of each game.

    zlib, libpng and libjpeg come from the copies in lib/. SDL3 is downloaded and built by CMake the
    first time, which needs an internet connection.

.PARAMETER Only
    What to build: any of jo (Jedi Outcast single player), jo-mp (Jedi Outcast multiplayer),
    ja-sp (Jedi Academy single player), ja-mp (Jedi Academy multiplayer). Default: all of them.

.PARAMETER Type
    Release (default), Debug, RelWithDebInfo or MinSizeRel.

.PARAMETER Arch
    x64 (default), x86 or arm64.

.PARAMETER Jobs
    Number of parallel jobs (default: all processors).

.PARAMETER BuildDir
    Where to build (default: .\build).

.PARAMETER Output
    Where to copy the result (default: .\dist).

.PARAMETER Generator
    CMake generator. Default: the newest Visual Studio with the C++ tools that is installed.

.PARAMETER NoRend2
    Leave out the experimental multiplayer rend2 renderer.

.PARAMETER NoInstall
    Only build, do not copy the result to the output folder.

.PARAMETER Clean
    Delete the build folder first.

.PARAMETER Check
    Only check that everything needed is installed, then stop.

.PARAMETER CMakeArgs
    Extra arguments passed to CMake when configuring.

.EXAMPLE
    .\build.ps1
    Builds everything in Release.

.EXAMPLE
    .\build.ps1 -Only jo,ja-sp -Jobs 8
    Builds the two single player games with 8 jobs.

.EXAMPLE
    .\build.ps1 -Check
    Shows what is missing without building.

.NOTES
    If scripts are blocked on your system, run it as:
        powershell -ExecutionPolicy Bypass -File .\build.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet('jo', 'jo-mp', 'ja-sp', 'ja-mp', 'all')]
    [string[]]$Only = @('all'),

    [ValidateSet('Release', 'Debug', 'RelWithDebInfo', 'MinSizeRel')]
    [string]$Type = 'Release',

    [ValidateSet('x64', 'x86', 'arm64')]
    [string]$Arch = 'x64',

    [int]$Jobs = 0,
    [string]$BuildDir = '',
    [string]$Output = '',
    [string]$Generator = '',
    [switch]$NoRend2,
    [switch]$NoInstall,
    [switch]$Clean,
    [switch]$Check,
    [string[]]$CMakeArgs = @()
)

Set-StrictMode -Version 2.0
$ErrorActionPreference = 'Stop'

$SourceDir = $PSScriptRoot
if (-not $BuildDir) { $BuildDir = Join-Path $SourceDir 'build' }
if (-not $Output)   { $Output   = Join-Path $SourceDir 'dist' }
if ($Jobs -le 0)    { $Jobs = [Environment]::ProcessorCount }

# ----------------------------------------------------------------------------------------------
# Output
# ----------------------------------------------------------------------------------------------

function Write-Step([string]$Text) { Write-Host ''; Write-Host "==> $Text" -ForegroundColor Cyan }
function Write-Ok([string]$Text)   { Write-Host '  [ ok ] ' -ForegroundColor Green -NoNewline; Write-Host $Text }
function Write-Warn([string]$Text) { Write-Host '  [warn] ' -ForegroundColor Yellow -NoNewline; Write-Host $Text }
function Write-Miss([string]$Text) { Write-Host '  [MISS] ' -ForegroundColor Red -NoNewline; Write-Host $Text }
function Write-Info([string]$Text) { Write-Host "  $Text" -ForegroundColor DarkGray }

function Stop-Build([string]$Text) {
    Write-Host ''
    Write-Host "error: $Text" -ForegroundColor Red
    exit 1
}

# ----------------------------------------------------------------------------------------------
# What to build
# ----------------------------------------------------------------------------------------------

$buildJO = $false; $buildJOMP = $false; $buildJASP = $false; $buildJAMP = $false
foreach ($item in $Only) {
    switch ($item) {
        'all'   { $buildJO = $true; $buildJOMP = $true; $buildJASP = $true; $buildJAMP = $true }
        'jo'    { $buildJO = $true }
        'jo-mp' { $buildJOMP = $true }
        'ja-sp' { $buildJASP = $true }
        'ja-mp' { $buildJAMP = $true }
    }
}

# ----------------------------------------------------------------------------------------------
# Checking what is installed
# ----------------------------------------------------------------------------------------------

$script:missingRequired = $false
$script:installHints = New-Object System.Collections.Generic.List[string]

function Add-Hint([string]$Text) {
    if ($script:installHints -notcontains $Text) { $script:installHints.Add($Text) }
}

function Get-VisualStudio {
    # The newest Visual Studio (or Build Tools) with the C++ compiler, found with vswhere
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    if (-not $pf86) { return $null }
    $vswhere = Join-Path $pf86 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (-not (Test-Path $vswhere)) { return $null }

    $json = & $vswhere -latest -products '*' -requires 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' -format json 2>$null
    if (-not $json) { return $null }
    $found = @(($json -join "`n") | ConvertFrom-Json)
    if ($found.Count -eq 0) { return $null }
    return $found[0]
}

function Get-VsGenerator($vs) {
    $major = [int]($vs.installationVersion.Split('.')[0])
    switch ($major) {
        18      { return 'Visual Studio 18 2026' }
        17      { return 'Visual Studio 17 2022' }
        16      { return 'Visual Studio 16 2019' }
        default { return $null }
    }
}

function Find-CMake($vs) {
    $cmd = Get-Command cmake -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    if ($vs) {
        # Visual Studio can come with its own CMake
        $bundled = Join-Path $vs.installationPath 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe'
        if (Test-Path $bundled) { return $bundled }
    }
    $default = Join-Path ([Environment]::GetEnvironmentVariable('ProgramFiles')) 'CMake\bin\cmake.exe'
    if (Test-Path $default) { return $default }
    return $null
}

function Test-Internet {
    try {
        $request = [System.Net.WebRequest]::Create('https://github.com')
        $request.Method = 'HEAD'
        $request.Timeout = 8000
        $response = $request.GetResponse()
        $response.Close()
        return $true
    } catch {
        return $false
    }
}

function Test-Dependencies {
    Write-Step 'Checking what the build needs'

    $os = [Environment]::OSVersion.VersionString
    Write-Info "System: $os, $Arch build"

    if (-not [Environment]::Is64BitOperatingSystem -and $Arch -ne 'x86') {
        Write-Warn "a 32-bit Windows can only build for x86 (use -Arch x86)"
    }

    # --- Visual Studio ---
    $vs = Get-VisualStudio
    $script:vsGenerator = $null
    if ($Generator) {
        $script:vsGenerator = $Generator
        Write-Ok "generator: $Generator"
        if ($vs) { Write-Ok "$($vs.displayName) with the C++ tools" }
    } elseif ($vs) {
        $script:vsGenerator = Get-VsGenerator $vs
        if ($script:vsGenerator) {
            Write-Ok "$($vs.displayName) with the C++ tools ($($script:vsGenerator))"
        } else {
            Write-Miss "$($vs.displayName) is too new or too old for this script (version $($vs.installationVersion)); pass -Generator"
            $script:missingRequired = $true
        }
    } else {
        Write-Miss 'Visual Studio with the "Desktop development with C++" workload (or the Build Tools)'
        $script:missingRequired = $true
        Add-Hint 'winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--passive --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"'
        Add-Hint 'or install Visual Studio Community from https://visualstudio.microsoft.com/ and tick "Desktop development with C++"'
    }

    # --- Windows SDK (part of the C++ workload, checked in case it was unticked) ---
    $pf86 = [Environment]::GetEnvironmentVariable('ProgramFiles(x86)')
    $sdkInclude = if ($pf86) { Join-Path $pf86 'Windows Kits\10\Include' } else { '' }
    if ($sdkInclude -and (Test-Path $sdkInclude) -and (Get-ChildItem $sdkInclude -Directory -ErrorAction SilentlyContinue)) {
        Write-Ok 'Windows SDK'
    } elseif ($vs) {
        Write-Miss 'Windows SDK (open the Visual Studio Installer and add a "Windows 10/11 SDK" to the C++ workload)'
        $script:missingRequired = $true
    }

    # --- CMake ---
    $script:cmakePath = Find-CMake $vs
    if ($script:cmakePath) {
        $versionLine = (& $script:cmakePath --version | Select-Object -First 1)
        $version = [version](($versionLine -split ' ')[-1] -replace '[^0-9.].*$', '')
        if ($version -ge [version]'3.16') {
            Write-Ok "cmake $version"
        } else {
            Write-Miss "cmake $version (3.16 or newer is needed)"
            $script:missingRequired = $true
            Add-Hint 'winget upgrade Kitware.CMake'
        }
    } else {
        Write-Miss 'cmake'
        $script:missingRequired = $true
        Add-Hint 'winget install Kitware.CMake'
    }

    # --- libraries ---
    foreach ($lib in 'zlib', 'libpng', 'jpeg-9a', 'minizip') {
        if (-not (Test-Path (Join-Path $SourceDir "lib\$lib"))) {
            Write-Miss "lib\$lib is missing from the source tree (is the checkout complete?)"
            $script:missingRequired = $true
        }
    }
    Write-Ok 'zlib, libpng, libjpeg: the copies in lib\ are used'

    # --- SDL3 is downloaded by CMake ---
    if (Test-Internet) {
        Write-Ok 'internet connection (SDL3 is downloaded and built the first time)'
    } elseif (Test-Path (Join-Path $BuildDir '_deps\sdl3-src')) {
        Write-Warn 'no internet connection, using the SDL3 downloaded earlier'
    } else {
        Write-Miss 'internet connection to github.com (needed to download SDL3 the first time)'
        $script:missingRequired = $true
        Add-Hint 'check your connection or proxy, or build once on a machine that is online and copy the build folder'
    }

    # --- source tree ---
    if ((Test-Path (Join-Path $SourceDir 'CMakeLists.txt')) -and (Test-Path (Join-Path $SourceDir 'codemp')) -and (Test-Path (Join-Path $SourceDir 'code'))) {
        Write-Ok "source tree: $SourceDir"
    } else {
        Write-Miss 'source tree (run this script from the root of the repository)'
        $script:missingRequired = $true
    }

    if ($script:missingRequired) {
        Write-Host ''
        Write-Host 'Something the build needs is missing (see the lines marked [MISS]).' -ForegroundColor Red
        if ($script:installHints.Count -gt 0) {
            Write-Host ''
            Write-Host 'To install what is missing:' -ForegroundColor White
            foreach ($hint in $script:installHints) { Write-Host "  $hint" }
            Write-Host '  (open a new terminal afterwards so the new programs are found)'
        }
        return $false
    }

    Write-Host ''
    Write-Host 'Everything needed is installed.' -ForegroundColor Green
    return $true
}

if (-not (Test-Dependencies)) { exit 1 }
if ($Check) { exit 0 }

# ----------------------------------------------------------------------------------------------
# Build
# ----------------------------------------------------------------------------------------------

function OnOff([bool]$Value) { if ($Value) { return 'ON' } else { return 'OFF' } }

function Invoke-CMake([string[]]$Arguments, [string]$FailureText) {
    & $script:cmakePath @Arguments
    if ($LASTEXITCODE -ne 0) { Stop-Build $FailureText }
}

Write-Step "Configuring ($Type, $($script:vsGenerator), $Arch, $Jobs jobs)"

if ($Clean -and (Test-Path $BuildDir)) {
    if (-not (Test-Path (Join-Path $BuildDir 'CMakeCache.txt'))) {
        Stop-Build "'$BuildDir' does not look like a CMake build folder, not deleting it"
    }
    Write-Info "deleting $BuildDir"
    Remove-Item -Recurse -Force $BuildDir
}

$generator = $script:vsGenerator
$cacheFile = Join-Path $BuildDir 'CMakeCache.txt'
if (Test-Path $cacheFile) {
    # A build folder stays tied to the generator it was created with
    $line = Select-String -Path $cacheFile -Pattern '^CMAKE_GENERATOR:INTERNAL=(.*)$' | Select-Object -First 1
    if ($line) {
        $existing = $line.Matches[0].Groups[1].Value
        if ($existing -ne $generator) {
            Write-Warn "$BuildDir was made with '$existing', using that (use -Clean to start over)"
            $generator = $existing
        }
    }
}

$archFlag = switch ($Arch) { 'x64' { 'x64' } 'x86' { 'Win32' } 'arm64' { 'ARM64' } }

$configureArgs = @(
    '-S', $SourceDir, '-B', $BuildDir, '-G', $generator,
    "-DCMAKE_BUILD_TYPE=$Type",
    "-DBuildJK2SPEngine=$(OnOff $buildJO)",
    "-DBuildJK2SPGame=$(OnOff $buildJO)",
    "-DBuildJK2SPRdVanilla=$(OnOff $buildJO)",
    "-DBuildJK2MPEngine=$(OnOff $buildJOMP)",
    "-DBuildJK2MPDed=$(OnOff $buildJOMP)",
    "-DBuildJK2MPRdVanilla=$(OnOff $buildJOMP)",
    "-DBuildSPEngine=$(OnOff $buildJASP)",
    "-DBuildSPGame=$(OnOff $buildJASP)",
    "-DBuildSPRdVanilla=$(OnOff $buildJASP)",
    "-DBuildMPEngine=$(OnOff $buildJAMP)",
    "-DBuildMPDed=$(OnOff $buildJAMP)",
    "-DBuildMPGame=$(OnOff $buildJAMP)",
    "-DBuildMPCGame=$(OnOff $buildJAMP)",
    "-DBuildMPUI=$(OnOff $buildJAMP)",
    "-DBuildMPRdVanilla=$(OnOff $buildJAMP)",
    "-DBuildMPRend2=$(OnOff ($buildJAMP -and -not $NoRend2))",
    '-DBuildTests=OFF',
    '-DUseInternalSDL3=ON'
)
if ($generator -like 'Visual Studio*') {
    $configureArgs += @('-A', $archFlag)
}
$configureArgs += $CMakeArgs

Invoke-CMake $configureArgs 'CMake could not configure the project (see the messages above)'

Write-Step 'Building'
$timer = [System.Diagnostics.Stopwatch]::StartNew()
Invoke-CMake @('--build', $BuildDir, '--config', $Type, '--parallel', "$Jobs") 'the build failed (see the messages above)'
Write-Ok ("built in {0:N0} s" -f $timer.Elapsed.TotalSeconds)

if (-not $NoInstall) {
    Write-Step "Copying the result to $Output"
    Invoke-CMake @('--install', $BuildDir, '--config', $Type, '--prefix', $Output) "could not copy the result to $Output"
}

Write-Step 'Done'
if (-not $NoInstall) {
    foreach ($name in 'JediOutcast', 'JediAcademy') {
        $gameDir = Join-Path $Output $name
        if (-not (Test-Path $gameDir)) { continue }
        Write-Host "  $name" -ForegroundColor White
        Get-ChildItem $gameDir -Recurse -File | ForEach-Object {
            Write-Host ('      ' + $_.FullName.Substring($gameDir.Length + 1))
        }
    }
    Write-Host ''
    Write-Host 'Copy the contents of each folder into the matching game''s GameData folder (next to the base folder),'
    $suffix = switch ($Arch) { 'x64' { 'x86_64' } 'x86' { 'x86' } 'arm64' { 'arm64' } }
    Write-Host "then run the executable: openjo_sp.$suffix.exe / openjo.$suffix.exe for Jedi Outcast, openjk_sp.$suffix.exe / openjk.$suffix.exe for Jedi Academy."

} else {
    Write-Host "  The programs are in $BuildDir"
}
