#Requires -Version 5.1

param(
    [switch]$NoGitPull,
    [switch]$BackupFiles,
    [string]$BuildFolder
)
$DEFAULT_BUILD_FOLDER = "build" # build-zig
$ErrorActionPreference = "Stop"

# ============================================================
# Daytona Arcade Recomp - Windows / Zig setup and build
#
# This script:
#   - Requires Administrator privileges
#   - Updates the main Git repository
#   - Installs Git, Python, CMake if missing
#   - Installs Ninja if missing
#   - Installs Zig 0.17.0 if missing
#   - Detects the actual installed tool locations
#   - Generates the Zig CMake toolchain files
#   - Backs up existing generated files with timestamps
#   - Fetches the pinned external repositories
#   - Performs the native Windows Zig build if a ROM exists
#   - Enable Powershell scripts with powershell -ExecutionPolicy Bypass -File zig-setup.ps1 or  Set-ExecutionPolicy unrestricted
#
# External repositories remain pinned to known commits.
# Only the main Daytona repository is updated with git pull.
# ============================================================


# ------------------------------------------------------------
# Error Handling
# ------------------------------------------------------------

function Finish-Script {
    param(
        [int]$ExitCode = 0
    )

    Write-Host
    Write-Host "Log file:"
    Write-Host "  $logFile"
    Write-Host

    Stop-Transcript

    Write-Host
    Write-Host "Press ENTER to close..."
    Read-Host

    exit $ExitCode
}

# ------------------------------------------------------------
# Paths
# ------------------------------------------------------------

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Definition

Set-Location $SCRIPT_DIR

Write-Host
Write-Host "============================================================"
Write-Host " Daytona Arcade Recomp - Windows Zig Setup"
Write-Host "============================================================"
Write-Host
Write-Host "Project directory:"
Write-Host "  $SCRIPT_DIR"
Write-Host


# ============================================================
# Require Administrator privileges
#
# Do this BEFORE starting the transcript so that only the
# elevated PowerShell process creates the log file.
# ============================================================

$currentIdentity = [Security.Principal.WindowsIdentity]::GetCurrent()

$principal = New-Object Security.Principal.WindowsPrincipal(
    $currentIdentity
)

if (-not $principal.IsInRole(
        [Security.Principal.WindowsBuiltInRole]::Administrator)) {

    Write-Host
    Write-Host "Administrator privileges are required."
    Write-Host "Restarting PowerShell as Administrator..."
    Write-Host

    $arguments = @(
        "-NoProfile"
        "-ExecutionPolicy"
        "Bypass"
        "-File"
        "`"$PSCommandPath`""
    )

    # Preserve command-line switches when elevating.
    if ($NoGitPull) {
        $arguments += "-NoGitPull"
    }
	
	if ($BackupFiles) {
		$arguments += "-BackupFiles"
	}
	
	if (-not [string]::IsNullOrWhiteSpace($BuildFolder)) {
		$arguments += "-BuildFolder"
		$arguments += $BuildFolder
	}
	
	

    Start-Process `
        -FilePath "powershell.exe" `
        -Verb RunAs `
        -ArgumentList $arguments

    exit
}


# ============================================================
# Logging
# ============================================================

$SCRIPT_DIR = Split-Path -Parent $MyInvocation.MyCommand.Definition

$logDir = Join-Path $SCRIPT_DIR "logs"

New-Item `
    -ItemType Directory `
    -Path $logDir `
    -Force | Out-Null

$timestamp = Get-Date -Format "dd_MM_yyyy_HH_mm_ss"

$logFile = Join-Path `
    $logDir `
    "zig-setup_$timestamp.log"

Start-Transcript `
    -Path $logFile `
    -Append

Write-Host
Write-Host "Log file:"
Write-Host "  $logFile"
Write-Host

# ============================================================
# Startup
# ============================================================

Write-Host
Write-Host "Log file:"
Write-Host "  $logFile"
Write-Host

Write-Host "============================================================"
Write-Host " Daytona Arcade Recomp - Windows Zig Setup"
Write-Host "============================================================"
Write-Host

Write-Host "Project directory:"
Write-Host "  $SCRIPT_DIR"
Write-Host

if ($NoGitPull) {
    Write-Host "Git pull:"
    Write-Host "  DISABLED (-NoGitPull)"
    Write-Host
}
else {
    Write-Host "Git pull:"
    Write-Host "  ENABLED"
    Write-Host
}

# ------------------------------------------------------------
# Helper: refresh PATH
# ------------------------------------------------------------

function Refresh-Path {
    $machinePath = [Environment]::GetEnvironmentVariable(
        "Path",
        "Machine"
    )

    $userPath = [Environment]::GetEnvironmentVariable(
        "Path",
        "User"
    )

    $env:Path = "$machinePath;$userPath"
}


# ------------------------------------------------------------
# Helper: add directory to machine PATH
# ------------------------------------------------------------

function Add-MachinePath([string]$PathToAdd) {

    $machinePath = [Environment]::GetEnvironmentVariable(
        "Path",
        "Machine"
    )

    $entries = $machinePath -split ';' |
        Where-Object {
            $_ -and $_.Trim() -ne ""
        }

    $normalised = $PathToAdd.TrimEnd('\')

    $exists = $entries |
        Where-Object {
            $_.TrimEnd('\') -ieq $normalised
        }

    if (-not $exists) {

        Write-Host "Adding to machine PATH:"
        Write-Host "  $PathToAdd"

        $newPath =
            $machinePath.TrimEnd(';') +
            ";" +
            $PathToAdd

        [Environment]::SetEnvironmentVariable(
            "Path",
            $newPath,
            "Machine"
        )
    }
}


# ------------------------------------------------------------
# Helper: test whether a command exists
# ------------------------------------------------------------

function Test-CommandExists([string]$Command) {

    $result = Get-Command $Command -ErrorAction SilentlyContinue

    return ($null -ne $result)
}


# ------------------------------------------------------------
# Helper: locate an executable
# ------------------------------------------------------------

function Find-Executable([string]$Name) {

    $command = Get-Command $Name -ErrorAction SilentlyContinue

    if ($null -eq $command) {
        return $null
    }

    return $command.Source
}


# ------------------------------------------------------------
# Helper: backup an existing generated file
# ------------------------------------------------------------

function Backup-IfExists([string]$Path) {
	
	 if (-not $BackupFiles) {
        return
    }

    if (-not (Test-Path $Path -PathType Leaf)) {
        return
    }

    $directory = Split-Path -Parent $Path
    $filename  = Split-Path -Leaf $Path

    $timestamp = Get-Date -Format "dd_MM_yyyy_HH_mm_ss"

    $backup = Join-Path `
        $directory `
        "${filename}_${timestamp}"

    # Avoid collisions if the script runs more than once
    # during the same second.
    $counter = 1

    while (Test-Path $backup) {

        $backup = Join-Path `
            $directory `
            "${filename}_${timestamp}_${counter}"

        $counter++
    }

    Write-Host
    Write-Host "Backing up existing file:"
    Write-Host "  $Path"
    Write-Host "  -> $backup"

    Move-Item `
        -LiteralPath $Path `
        -Destination $backup
}


# ------------------------------------------------------------
# Helper: clone a pinned repository
# ------------------------------------------------------------

function Clone-PinnedRepository(
    [string]$Name,
    [string]$Url,
    [string]$Directory,
    [string]$Commit
) {

    Write-Host
    Write-Host "------------------------------------------------------------"
    Write-Host "Repository: $Name"
    Write-Host "------------------------------------------------------------"

    if (-not (Test-Path $Directory)) {

        Write-Host "Cloning $Url"
        Write-Host

        git clone $Url $Directory

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to clone $Name."
        }
    }

    Push-Location $Directory

    try {

        Write-Host "Fetching pinned commit:"
        Write-Host "  $Commit"
        Write-Host

        git fetch --depth 1 origin $Commit

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to fetch $Name commit $Commit."
        }

        git checkout --detach $Commit

        if ($LASTEXITCODE -ne 0) {
            throw "Failed to checkout $Name commit $Commit."
        }

    }
    finally {
        Pop-Location
    }
}


# ------------------------------------------------------------
# Update main Daytona repository
# ------------------------------------------------------------

if ($NoGitPull) {

    Write-Host
    Write-Host "============================================================"
    Write-Host " Skipping Git pull"
    Write-Host "============================================================"
    Write-Host
    Write-Host "Git pull disabled by -NoGitPull."
    Write-Host

}
else {

    Write-Host
    Write-Host "============================================================"
    Write-Host " Updating Daytona source repository"
    Write-Host "============================================================"
    Write-Host

    if (-not (Test-Path (Join-Path $SCRIPT_DIR ".git"))) {
        throw "$SCRIPT_DIR is not a Git repository."
    }

    Push-Location $SCRIPT_DIR

    try {

        git pull --ff-only

        if ($LASTEXITCODE -ne 0) {
            throw "git pull --ff-only failed."
        }

    }
    finally {
        Pop-Location
    }

    Write-Host
    Write-Host "Daytona repository is up to date."
    Write-Host
}


# ------------------------------------------------------------
# Check / install Git
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking Git"
Write-Host "============================================================"
Write-Host

Refresh-Path

if (-not (Test-CommandExists "git.exe")) {

    Write-Host "Git was not found."
    Write-Host "Installing Git using winget..."
    Write-Host

    winget install `
        --id Git.Git `
        --exact `
        --accept-source-agreements `
        --accept-package-agreements

    if ($LASTEXITCODE -ne 0) {
        throw "Git installation failed."
    }

    Refresh-Path
}

$gitExe = Find-Executable "git.exe"

Write-Host "Git:"
Write-Host "  $gitExe"


# ------------------------------------------------------------
# Check / install Python
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking Python"
Write-Host "============================================================"
Write-Host

Refresh-Path

if (-not (Test-CommandExists "python.exe")) {

    Write-Host "Python was not found."
    Write-Host "Installing Python 3.12 using winget..."
    Write-Host

    winget install `
        --id Python.Python.3.12 `
        --exact `
        --accept-source-agreements `
        --accept-package-agreements

    if ($LASTEXITCODE -ne 0) {
        throw "Python installation failed."
    }

    Refresh-Path
}

$pythonExe = Find-Executable "python.exe"

Write-Host "Python:"
Write-Host "  $pythonExe"


# ------------------------------------------------------------
# Check / install CMake
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking CMake"
Write-Host "============================================================"
Write-Host

$CMAKE_VERSION = "4.4.4"
$CMAKE_MSI_URL = "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VERSION/cmake-$CMAKE_VERSION-windows-x86_64.msi"

Refresh-Path

if (-not (Test-CommandExists "cmake.exe")) {

    Write-Host "CMake was not found."
    Write-Host
    Write-Host "Installing CMake $CMAKE_VERSION using the official Kitware MSI..."
    Write-Host

    $tempDir = Join-Path $env:TEMP "daytona-cmake-install"
    $cmakeMsi = Join-Path $tempDir "cmake-$CMAKE_VERSION-windows-x86_64.msi"

    New-Item `
        -ItemType Directory `
        -Path $tempDir `
        -Force | Out-Null

    try {

        Write-Host "Downloading:"
        Write-Host "  $CMAKE_MSI_URL"
        Write-Host

        Invoke-WebRequest `
            -Uri $CMAKE_MSI_URL `
            -OutFile $cmakeMsi `
            -UseBasicParsing

        if (-not (Test-Path $cmakeMsi)) {
            throw "CMake MSI download failed."
        }

        Write-Host "Installing CMake MSI..."
        Write-Host

        $msiProcess = Start-Process `
            -FilePath "msiexec.exe" `
            -ArgumentList @(
                "/i"
                "`"$cmakeMsi`""
                "/quiet"
                "/norestart"
            ) `
            -Wait `
            -PassThru

        if ($msiProcess.ExitCode -ne 0) {
            throw "CMake MSI installation failed with exit code $($msiProcess.ExitCode)."
        }

        Write-Host "CMake MSI installation completed."

    }
    catch {

        Write-Warning "CMake MSI installation failed."
        Write-Warning $_.Exception.Message
        Write-Host

        # ----------------------------------------------------
        # Fall back to WinGet
        # ----------------------------------------------------

        if (Test-CommandExists "winget.exe") {

            Write-Host "Falling back to WinGet..."
            Write-Host

            winget install `
                --id Kitware.CMake `
                --exact `
                --accept-source-agreements `
                --accept-package-agreements

            if ($LASTEXITCODE -ne 0) {
                throw "CMake installation failed using both the official MSI and WinGet."
            }

        }
        else {
            throw @"
CMake installation failed using the official MSI, and WinGet is not available.

MSI:
  $CMAKE_MSI_URL

Please install CMake manually and rerun this script.
"@
        }
    }

    Refresh-Path
}

$cmakeExe = Find-Executable "cmake.exe"

if (-not $cmakeExe) {
    throw "CMake could not be located after installation."
}

$cmakeDir = Split-Path -Parent $cmakeExe

Write-Host "CMake:"
Write-Host "  $cmakeExe"


# ------------------------------------------------------------
# Check / install Ninja
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking Ninja"
Write-Host "============================================================"
Write-Host

Refresh-Path

if (-not (Test-CommandExists "ninja.exe")) {

    Write-Host "Ninja was not found."
    Write-Host "Downloading Ninja 1.13.2..."
    Write-Host

    $ninjaUrl =
        "https://github.com/ninja-build/ninja/releases/download/" +
        "v1.13.2/ninja-win.zip"

    $ninjaZip = Join-Path $env:TEMP "ninja-win.zip"
    $ninjaTemp = Join-Path $env:TEMP "ninja-win"

    if (Test-Path $ninjaTemp) {
        Remove-Item `
            -Recurse `
            -Force `
            $ninjaTemp
    }

    Invoke-WebRequest `
        -Uri $ninjaUrl `
        -OutFile $ninjaZip

    Expand-Archive `
        -Path $ninjaZip `
        -DestinationPath $ninjaTemp `
        -Force

    $ninjaSource = Join-Path $ninjaTemp "ninja.exe"

    if (-not (Test-Path $ninjaSource)) {
        throw "Ninja executable was not found in downloaded archive."
    }

    $ninjaDestination = Join-Path $cmakeDir "ninja.exe"

    Write-Host "Installing Ninja:"
    Write-Host "  $ninjaDestination"

    Copy-Item `
        -Force `
        $ninjaSource `
        $ninjaDestination

    Add-MachinePath $cmakeDir

    Refresh-Path
}

$ninjaExe = Find-Executable "ninja.exe"

if (-not $ninjaExe) {
    throw "Ninja could not be located."
}

Write-Host "Ninja:"
Write-Host "  $ninjaExe"


# ------------------------------------------------------------
# Check / install Zig 0.17.0
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking Zig"
Write-Host "============================================================"
Write-Host

$zigVersion = "0.17.0"

Refresh-Path

$zigExe = Find-Executable "zig.exe"

if ($zigExe) {

    $installedVersion = (& $zigExe version).Trim()

    Write-Host "Found Zig:"
    Write-Host "  $zigExe"
    Write-Host "  Version: $installedVersion"
    Write-Host

    if ($installedVersion -ne $zigVersion) {

        Write-Warning `
            "Zig $installedVersion is installed; expected $zigVersion."

        Write-Host
        Write-Host "The existing Zig installation will be replaced."
        Write-Host
    }
}

if (
    (-not $zigExe) -or
    (($installedVersion -ne $zigVersion) -and
     ($null -ne $installedVersion))
) {

    $zigUrl =
        "https://ziglang.org/download/0.17.0/" +
        "zig-x86_64-windows-0.17.0.zip"

    $zigZip = Join-Path `
        $env:TEMP `
        "zig-x86_64-windows-0.17.0.zip"

    $zigTemp = Join-Path `
        $env:TEMP `
        "zig-x86_64-windows-0.17.0"

    Write-Host "Downloading Zig $zigVersion..."
    Write-Host

    Invoke-WebRequest `
        -Uri $zigUrl `
        -OutFile $zigZip

    if (Test-Path $zigTemp) {
        Remove-Item `
            -Recurse `
            -Force `
            $zigTemp
    }

    Expand-Archive `
        -Path $zigZip `
        -DestinationPath $zigTemp `
        -Force

    $zigExtractedDir =
        Join-Path `
            $zigTemp `
            "zig-x86_64-windows-0.17.0"

    if (-not (Test-Path $zigExtractedDir)) {
        throw "Zig archive did not contain the expected directory."
    }

    $zigInstallDir = "C:\Program Files\Zig"

    if (Test-Path $zigInstallDir) {

        Write-Host "Removing existing Zig installation:"
        Write-Host "  $zigInstallDir"

        Remove-Item `
            -Recurse `
            -Force `
            $zigInstallDir
    }

    Write-Host "Installing Zig:"
    Write-Host "  $zigInstallDir"

    New-Item `
        -ItemType Directory `
        -Path $zigInstallDir `
        -Force | Out-Null

    Copy-Item `
        -Recurse `
        -Force `
        (Join-Path $zigExtractedDir "*") `
        $zigInstallDir

    Add-MachinePath $zigInstallDir

    Refresh-Path

    $zigExe = Join-Path `
        $zigInstallDir `
        "zig.exe"
}

# Locate Zig again after installation.
$zigExe = Find-Executable "zig.exe"

if (-not $zigExe) {
    throw "Zig could not be located."
}

$installedVersion = (& $zigExe version).Trim()

if ($installedVersion -ne $zigVersion) {
    throw "Expected Zig $zigVersion but found Zig $installedVersion."
}

Write-Host
Write-Host "Zig:"
Write-Host "  $zigExe"
Write-Host "  Version: $installedVersion"


# ------------------------------------------------------------
# Generate Zig CMake toolchain
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Generating Zig CMake toolchain"
Write-Host "============================================================"
Write-Host

$zigCmakePath = $zigExe -replace '\\', '/'

$toolchainFile = Join-Path `
    $SCRIPT_DIR `
    "zig-windows.cmake"

Backup-IfExists $toolchainFile

@"
set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_SYSTEM_PROCESSOR x86_64)

set(ZIG "${zigCmakePath}")

set(CMAKE_C_COMPILER "`${ZIG}")
set(CMAKE_CXX_COMPILER "`${ZIG}")

set(CMAKE_C_COMPILER_ARG1 "cc")
set(CMAKE_CXX_COMPILER_ARG1 "c++")

set(CMAKE_C_COMPILER_TARGET "x86_64-windows-gnu")
set(CMAKE_CXX_COMPILER_TARGET "x86_64-windows-gnu")

set(CMAKE_AR "`${CMAKE_CURRENT_LIST_DIR}/zig-ar.cmd"
    CACHE FILEPATH "Zig archiver")

set(CMAKE_RANLIB "`${CMAKE_CURRENT_LIST_DIR}/zig-ranlib.cmd"
    CACHE FILEPATH "Zig ranlib")

set(CMAKE_C_COMPILER_AR "`${ZIG}"
    CACHE FILEPATH "Zig compiler archiver")

set(CMAKE_CXX_COMPILER_AR "`${ZIG}"
    CACHE FILEPATH "Zig CXX compiler archiver")

set(CMAKE_LINKER "`${ZIG}"
    CACHE FILEPATH "Zig linker")

set(CMAKE_C_FLAGS_INIT "-fno-sanitize=all")
set(CMAKE_CXX_FLAGS_INIT "-fno-sanitize=all")
"@ | Set-Content `
    -Path $toolchainFile `
    -Encoding ASCII

Write-Host "Generated:"
Write-Host "  $toolchainFile"


# ------------------------------------------------------------
# Generate Zig archiver wrapper
# ------------------------------------------------------------

$zigCmdPath = $zigExe.Replace('"', '""')

$arFile = Join-Path `
    $SCRIPT_DIR `
    "zig-ar.cmd"

Backup-IfExists $arFile

@"
@echo off
"$zigCmdPath" ar %*
"@ | Set-Content `
    -Path $arFile `
    -Encoding ASCII

Write-Host "Generated:"
Write-Host "  $arFile"


# ------------------------------------------------------------
# Generate Zig ranlib wrapper
# ------------------------------------------------------------

$ranlibFile = Join-Path `
    $SCRIPT_DIR `
    "zig-ranlib.cmd"

Backup-IfExists $ranlibFile

@"
@echo off
"$zigCmdPath" ranlib %*
"@ | Set-Content `
    -Path $ranlibFile `
    -Encoding ASCII

Write-Host "Generated:"
Write-Host "  $ranlibFile"


# ------------------------------------------------------------
# Optional compiler wrappers
#
# These are generated for compatibility/manual use, but the
# CMake toolchain above does NOT require them because
# CMAKE_C_COMPILER_ARG1 and CMAKE_CXX_COMPILER_ARG1 are used.
# ------------------------------------------------------------

$ccFile = Join-Path `
    $SCRIPT_DIR `
    "zig-cc.cmd"

Backup-IfExists $ccFile

@"
@echo off
"$zigCmdPath" cc %*
"@ | Set-Content `
    -Path $ccFile `
    -Encoding ASCII

Write-Host "Generated:"
Write-Host "  $ccFile"


$cxxFile = Join-Path `
    $SCRIPT_DIR `
    "zig-c++.cmd"

Backup-IfExists $cxxFile

@"
@echo off
"$zigCmdPath" c++ %*
"@ | Set-Content `
    -Path $cxxFile `
    -Encoding ASCII

Write-Host "Generated:"
Write-Host "  $cxxFile"


# ------------------------------------------------------------
# External repositories
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking external repositories"
Write-Host "============================================================"
Write-Host

$externDir = Join-Path `
    $SCRIPT_DIR `
    "extern"

New-Item `
    -ItemType Directory `
    -Path $externDir `
    -Force | Out-Null


# ------------------------------------------------------------
# Berkeley SoftFloat
# ------------------------------------------------------------

Clone-PinnedRepository `
    "softfloat" `
    "https://github.com/ucb-bar/berkeley-softfloat-3.git" `
    (Join-Path $externDir "softfloat") `
    "a0c6494cdc11865811dec815d5c0049fba9d82a8"


# ------------------------------------------------------------
# SDL3
# ------------------------------------------------------------

Clone-PinnedRepository `
    "sdl3" `
    "https://github.com/libsdl-org/SDL.git" `
    (Join-Path $externDir "sdl3") `
    "fa2c02bb6e21974a89ea9824bc53c9932abe5f9c"


# ------------------------------------------------------------
# Dear ImGui
# ------------------------------------------------------------

Clone-PinnedRepository `
    "imgui" `
    "https://github.com/ocornut/imgui.git" `
    (Join-Path $externDir "imgui") `
    "f1cc2ae15e53a861a874c3034aae6798fde194ab"


# ------------------------------------------------------------
# YMFM
# ------------------------------------------------------------

Clone-PinnedRepository `
    "ymfm" `
    "https://github.com/aaronsgiles/ymfm.git" `
    (Join-Path $externDir "ymfm") `
    "81aec25ccbb98f4873a255f7551ac4dadac59b4a"


# ------------------------------------------------------------
# LZMA / 7-Zip
# ------------------------------------------------------------

Clone-PinnedRepository `
    "lzma" `
    "https://github.com/ip7z/7zip.git" `
    (Join-Path $externDir "lzma") `
    "0766b733fe3e06dd2a7f9a3cfbf2108ac73abd17"


# ------------------------------------------------------------
# MAME
#
# MAME is deliberately sparse checked out because the project
# only needs a small subset of the source tree.
# ------------------------------------------------------------

$mameDir = Join-Path `
    $externDir `
    "mame"

$mameCommit =
    "dddd73680656e355bb2b5beecab1167c9f07bf81"

Write-Host
Write-Host "------------------------------------------------------------"
Write-Host "Repository: mame"
Write-Host "------------------------------------------------------------"

if (-not (Test-Path $mameDir)) {

    Write-Host "Cloning MAME sparse repository..."

    git clone `
        --filter=blob:none `
        --no-checkout `
        "https://github.com/mamedev/mame.git" `
        $mameDir

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to clone MAME."
    }
}

Push-Location $mameDir

try {

    Write-Host "Fetching pinned MAME commit:"
    Write-Host "  $mameCommit"
    Write-Host

    git fetch `
        --depth 1 `
        origin `
        $mameCommit

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to fetch MAME commit."
    }

    git checkout --detach $mameCommit

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to checkout MAME commit."
    }

    git sparse-checkout init --no-cone

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to initialise MAME sparse checkout."
    }

    git sparse-checkout set --no-cone `
        "/src/devices/cpu/i960/" `
        "/src/devices/cpu/mb86233/" `
        "/src/devices/cpu/m68000/m68000.cpp" `
        "/src/devices/cpu/m68000/m68000.h" `
        "/src/mame/sega/model2.cpp" `
        "/src/mame/sega/model2.h" `
        "/src/mame/sega/model2_v.cpp" `
        "/src/mame/sega/model2_m.cpp" `
        "/src/mame/shared/segam1audio.cpp" `
        "/src/mame/shared/segam1audio.h"

    if ($LASTEXITCODE -ne 0) {
        throw "Failed to configure MAME sparse checkout."
    }

}
finally {
    Pop-Location
}


# ------------------------------------------------------------
# Check for ROM
#
# The build only starts when at least one file exists in roms.
# The actual recompiler script determines which ROM belongs
# to the requested set.
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Checking ROM directory"
Write-Host "============================================================"
Write-Host

$romDir = Join-Path `
    $SCRIPT_DIR `
    "roms"

New-Item `
    -ItemType Directory `
    -Path $romDir `
    -Force | Out-Null

$romFiles = @(
    Get-ChildItem `
        -Path $romDir `
        -File `
        -ErrorAction SilentlyContinue
)

if ($romFiles.Count -eq 0) {

    Write-Host "No ROM files were found in:"
    Write-Host "  $romDir"
    Write-Host
    Write-Host "Setup is complete."
    Write-Host
    Write-Host "Place a Daytona ROM in:"
    Write-Host "  $romDir"
    Write-Host
    Write-Host "Then run this script again to perform the build."
    Write-Host

    Finish-Script 0
}

Write-Host "Found ROM file(s):"

foreach ($rom in $romFiles) {
    Write-Host "  $($rom.Name)"
}


# ------------------------------------------------------------
# Configure/build directories
# ------------------------------------------------------------

if ([string]::IsNullOrWhiteSpace($BuildFolder)) {
    $BuildFolder = $DEFAULT_BUILD_FOLDER
}

$buildDir = Join-Path `
    $SCRIPT_DIR `
    $BuildFolder

New-Item `
    -ItemType Directory `
    -Path $buildDir `
    -Force | Out-Null


# ------------------------------------------------------------
# Configure with CMake
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Configuring Windows Zig build"
Write-Host "============================================================"
Write-Host

& $cmakeExe `
    -S $SCRIPT_DIR `
    -B $buildDir `
    -G Ninja `
    "-DCMAKE_MAKE_PROGRAM=$ninjaExe" `
    "-DCMAKE_TOOLCHAIN_FILE=$toolchainFile" `
    "-DCMAKE_BUILD_TYPE=Release" `
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON" `
    "-DM2_ROMSET=daytona93"

if ($LASTEXITCODE -ne 0) {
    throw "CMake configuration failed."
}


# ------------------------------------------------------------
# Build
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Building Windows executable"
Write-Host "============================================================"
Write-Host

& $ninjaExe `
    -C $buildDir `
    -v

if ($LASTEXITCODE -ne 0) {
    throw "Ninja build failed."
}


# ------------------------------------------------------------
# Recompile
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " Running Windows recompiler"
Write-Host "============================================================"
Write-Host

& $pythonExe `
    ".\scripts\recompile.py" `
    --set `
    daytona93 `
    --build-dir `
    $buildDir `
    --config `
    Release

if ($LASTEXITCODE -ne 0) {
    throw "Windows recompiler failed."
}


# ------------------------------------------------------------
# Final result
# ------------------------------------------------------------

Write-Host
Write-Host "============================================================"
Write-Host " BUILD COMPLETE"
Write-Host "============================================================"
Write-Host

$exe = Join-Path `
    $buildDir `
    "daytona.exe"

if (Test-Path $exe) {

    Write-Host "Windows executable:"
    Write-Host "  $exe"

}
else {

    Write-Warning `
        "Build completed but daytona.exe was not found at:"
    Write-Host "  $exe"
}

Write-Host
Write-Host "============================================================"
Write-Host " SCRIPT COMPLETE"
Write-Host "============================================================"
Write-Host
Write-Host "Log file:"
Write-Host "  $logFile"
Write-Host

Stop-Transcript

Write-Host
Write-Host "Press ENTER to close..."
Read-Host
