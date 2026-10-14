<#
.SYNOPSIS
    Prepares PowerShell to build an Android app with the Gradle wrapper (no Android Studio
    import needed), checks the prerequisites, then runs the debug build.

.DESCRIPTION
    Run it from the Android project folder (the one containing gradlew.bat), or pass -ProjectDir.

    1. Finds Android Studio, sets JAVA_HOME to its bundled JDK (JBR) and checks the version.
    2. Finds the Android SDK and sets ANDROID_HOME.
    3. Reads the project: Gradle wrapper version, Android Gradle Plugin (AGP) version, and from
       app\build.gradle the compileSdk, ndkVersion, CMake version (and buildToolsVersion if pinned).
       Those versions decide which SDK packages are required, so the check always matches the project.
    4. Uses sdkmanager --list_installed to confirm those packages are installed.
    5. If everything is met: sets ANDROID_NDK_HOME, runs gradlew --stop, then
       gradlew :app:assembleDebug --refresh-dependencies.

    If anything is missing the script stops before building and says how to fix it.
    Environment variables are process-wide, so JAVA_HOME, ANDROID_HOME and PATH stay set in your
    current session afterwards. Exit code: 0 = all checks passed (and the build succeeded),
    otherwise non-zero.

.PARAMETER ProjectDir
    Android project folder. Defaults to the current directory.

.PARAMETER AppBuildFile
    App module build file, relative to ProjectDir (app\build.gradle.kts is tried if needed).

.PARAMETER DefaultBuildTools
    Build-tools version to require when the build file does not pin buildToolsVersion
    (AGP then chooses its own default, which for AGP 9.0 is expected to be 36.0.0).

.PARAMETER ExtraPackages
    Additional SDK package ids to require, as sdkmanager names them.

.PARAMETER ExpectedAgp
    Required AGP version prefix (9.0 accepts 9.0, 9.0.0, 9.0.1 ...).

.PARAMETER ExpectedGradle
    Required Gradle wrapper version.

.PARAMETER InstallMissing
    Install missing SDK packages with sdkmanager (interactive: you accept the licences).

.PARAMETER NoBuild
    Only set up and check; do not run Gradle.

.PARAMETER NoRefresh
    Leave out --refresh-dependencies (much faster once dependencies are cached).

.PARAMETER Persist
    Also save JAVA_HOME and ANDROID_HOME to your user environment variables.

.EXAMPLE
    cd C:\Path\To\daytona-arcade-recomp\platform\mobile\android
    powershell -ExecutionPolicy Bypass -File .\Setup-AndroidEnv.ps1 -NoRefresh -InstallMissing

#>
[CmdletBinding()]
param(
    [string]$ProjectDir = (Get-Location).Path,
    [string]$AppBuildFile = 'app\build.gradle',
    [string]$StudioPath,
    [string]$SdkPath,
    [int]$MinJavaMajor = 17,
    [string]$DefaultBuildTools = '36.0.0',
    [string[]]$ExtraPackages = @(),
    [string]$ExpectedAgp = '9.0',
    [string]$ExpectedGradle = '9.1.0',
    [switch]$InstallMissing,
    [switch]$NoBuild,
    [switch]$NoRefresh,
    [switch]$Persist
)

$script:failed     = $false
$script:missing    = @()
$script:required   = @()
$script:ndkVersion = $null
function Write-Pass($m)    { Write-Host "[ OK ] $m" -ForegroundColor Green }
function Write-Caution($m) { Write-Host "[WARN] $m" -ForegroundColor Yellow }
function Write-Fail($m)    { Write-Host "[FAIL] $m" -ForegroundColor Red; $script:failed = $true }
function Write-Step($m)    { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }
function Add-Required($pkg) { if ($pkg -and ($script:required -notcontains $pkg)) { $script:required += $pkg } }

# Returns the first non-comment line matching $pattern as @{ Value = group 1; Line = line number }
function Find-Value($lines, $pattern) {
    for ($i = 0; $i -lt $lines.Count; $i++) {
        if ($lines[$i] -match '^\s*(//|#)') { continue }
        if ($lines[$i] -match $pattern) {
            return [pscustomobject]@{ Value = $Matches[1]; Line = $i + 1 }
        }
    }
    return $null
}

# ------------------------------------------------------------------ 1. JAVA_HOME
function Find-AndroidStudio {
    $candidates = @()
    if ($StudioPath) { $candidates += $StudioPath }
    $candidates += "$env:ProgramFiles\Android\Android Studio"
    $candidates += "${env:ProgramFiles(x86)}\Android\Android Studio"
    $candidates += "$env:LOCALAPPDATA\Programs\Android Studio"

    $keys = 'HKLM:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKLM:\SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall\*',
            'HKCU:\SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall\*'
    foreach ($k in $keys) {
        Get-ItemProperty $k -ErrorAction SilentlyContinue |
            Where-Object { $_.DisplayName -like 'Android Studio*' -and $_.InstallLocation } |
            ForEach-Object { $candidates += $_.InstallLocation }
    }

    # JetBrains Toolbox installs (studio64.exe lives in <install>\bin)
    $toolbox = "$env:LOCALAPPDATA\JetBrains\Toolbox\apps"
    if (Test-Path $toolbox) {
        Get-ChildItem $toolbox -Directory -Filter 'AndroidStudio*' -ErrorAction SilentlyContinue | ForEach-Object {
            Get-ChildItem $_.FullName -Recurse -Depth 3 -Filter studio64.exe -ErrorAction SilentlyContinue |
                ForEach-Object { $candidates += (Split-Path (Split-Path $_.FullName)) }
        }
    }

    foreach ($c in $candidates) {
        if (-not $c) { continue }
        foreach ($jdk in 'jbr', 'jre') {   # older Studio versions used "jre"
            if (Test-Path (Join-Path $c "$jdk\bin\java.exe")) {
                return [pscustomobject]@{ Studio = $c; Jdk = (Join-Path $c $jdk) }
            }
        }
    }
    return $null
}

function Get-JavaVersion($javaHome) {
    $v = $null
    $release = Join-Path $javaHome 'release'
    if (Test-Path $release) {
        $m = Select-String -Path $release -Pattern '^JAVA_VERSION="?([^"]+)"?' | Select-Object -First 1
        if ($m) { $v = $m.Matches[0].Groups[1].Value }
    }
    if (-not $v) {
        $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
        $out = (& (Join-Path $javaHome 'bin\java.exe') -version 2>&1 | Out-String)
        $ErrorActionPreference = $prev
        if ($out -match 'version "([^"]+)"') { $v = $Matches[1] }
    }
    if (-not $v) { return $null }
    $major = 0
    if     ($v -match '^1\.(\d+)') { $major = [int]$Matches[1] }
    elseif ($v -match '^(\d+)')    { $major = [int]$Matches[1] }
    return [pscustomobject]@{ Version = $v; Major = $major }
}

Write-Step "JDK (JAVA_HOME)"
$found = Find-AndroidStudio
if (-not $found) {
    Write-Fail "Android Studio (with a bundled JDK) not found. Re-run with -StudioPath 'C:\path\to\Android Studio'."
} else {
    Write-Pass "Android Studio: $($found.Studio)"
    $env:JAVA_HOME = $found.Jdk

    # JDK first on PATH, without piling up duplicates on repeated runs
    $javaBin = Join-Path $env:JAVA_HOME 'bin'
    $others  = $env:Path -split ';' | Where-Object { $_ -and ($_.TrimEnd('\') -ne $javaBin.TrimEnd('\')) }
    $env:Path = (@($javaBin) + $others) -join ';'
    Write-Pass "JAVA_HOME = $env:JAVA_HOME"

    $jv = Get-JavaVersion $env:JAVA_HOME
    if (-not $jv) {
        Write-Fail "Could not determine the Java version in $env:JAVA_HOME"
    } elseif ($jv.Major -lt $MinJavaMajor) {
        Write-Fail "Java $($jv.Version) is older than the required $MinJavaMajor"
    } else {
        Write-Pass "Java $($jv.Version) (>= $MinJavaMajor)"
    }

    $resolved = (Get-Command java -ErrorAction SilentlyContinue | Select-Object -First 1).Source
    if ($resolved -and ($resolved -ne (Join-Path $javaBin 'java.exe'))) {
        Write-Caution "'java' on PATH resolves to $resolved, not the Studio JDK"
    }
    if ($Persist) {
        [Environment]::SetEnvironmentVariable('JAVA_HOME', $env:JAVA_HOME, 'User')
        Write-Pass "JAVA_HOME saved to user environment"
    }
}

# ---------------------------------------------------------------- 2. ANDROID_HOME
Write-Step "Android SDK (ANDROID_HOME)"
$sdkCandidates = @($SdkPath, $env:ANDROID_HOME, $env:ANDROID_SDK_ROOT, "$env:LOCALAPPDATA\Android\Sdk") |
    Where-Object { $_ }
$sdk = $sdkCandidates | Where-Object { Test-Path (Join-Path $_ 'platforms') } | Select-Object -First 1

if (-not $sdk) {
    Write-Fail "No Android SDK found (looked for a 'platforms' folder). Re-run with -SdkPath 'C:\path\to\Sdk'."
} else {
    $env:ANDROID_HOME = $sdk
    Write-Pass "ANDROID_HOME = $sdk"
    if ($env:ANDROID_SDK_ROOT -and ($env:ANDROID_SDK_ROOT.TrimEnd('\') -ne $sdk.TrimEnd('\'))) {
        Write-Caution "ANDROID_SDK_ROOT ($env:ANDROID_SDK_ROOT) differs from ANDROID_HOME; run: Remove-Item Env:ANDROID_SDK_ROOT"
    }
    if ($Persist) {
        [Environment]::SetEnvironmentVariable('ANDROID_HOME', $sdk, 'User')
        Write-Pass "ANDROID_HOME saved to user environment"
    }
}

# ------------------------------------------------- 3. Project: wrapper, AGP, app build file
Write-Step "Project: Gradle wrapper, AGP and $AppBuildFile"
$ProjectDir = $ProjectDir.TrimEnd('\')
if (-not (Test-Path (Join-Path $ProjectDir 'gradlew.bat'))) {
    Write-Fail "gradlew.bat not found in $ProjectDir. Run from the Android project folder or pass -ProjectDir."
} else {
    Write-Pass "Project: $ProjectDir"

    # Gradle wrapper version
    $wrapperProps = Join-Path $ProjectDir 'gradle\wrapper\gradle-wrapper.properties'
    $gm = $null
    if (Test-Path $wrapperProps) {
        $gm = Select-String -Path $wrapperProps -Pattern 'gradle-([\d\.]+(?:-[\w\.]+)?)-(?:bin|all)\.zip' |
            Select-Object -First 1
    }
    if (-not $gm) {
        Write-Fail "Could not read the Gradle version from $wrapperProps"
    } else {
        $gv = $gm.Matches[0].Groups[1].Value
        if ($gv -eq $ExpectedGradle) { Write-Pass "Gradle wrapper $gv" }
        else { Write-Fail "Gradle wrapper is $gv, expected $ExpectedGradle (edit distributionUrl in $wrapperProps)" }
    }

    # AGP: plugins block (Groovy or Kotlin DSL), buildscript classpath, or version catalog
    $agpPatterns = @(
        'com\.android\.(?:application|library)[\x27\x22]\)?\s+version\s+[\x27\x22]([^\x27\x22]+)[\x27\x22]',
        'com\.android\.tools\.build:gradle:([\w\.\-]+)',
        '^\s*agp\s*=\s*[\x27\x22]([^\x27\x22]+)[\x27\x22]'
    )
    $agp = $null
    foreach ($rel in 'build.gradle', 'build.gradle.kts', 'settings.gradle', 'settings.gradle.kts', 'gradle\libs.versions.toml') {
        $f = Join-Path $ProjectDir $rel
        if (-not (Test-Path $f)) { continue }
        $flines = @(Get-Content $f)
        foreach ($p in $agpPatterns) {
            $hit = Find-Value $flines $p
            if ($hit) { $agp = [pscustomobject]@{ Version = $hit.Value; File = $f; Line = $hit.Line }; break }
        }
        if ($agp) { break }
    }
    if (-not $agp) {
        Write-Caution "Could not find an AGP version in build.gradle, settings.gradle or libs.versions.toml (it may be set elsewhere)"
    } else {
        $v = $agp.Version
        if ($v -eq $ExpectedAgp -or $v.StartsWith("$ExpectedAgp.") -or $v.StartsWith("$ExpectedAgp-")) {
            Write-Pass "Android Gradle Plugin $v  ($($agp.File):$($agp.Line))"
        } else {
            Write-Fail "Android Gradle Plugin is $v, expected $ExpectedAgp.x. Edit $($agp.File) line $($agp.Line)."
        }
    }

    # App module: the SDK, NDK, CMake (and build-tools) versions the build will ask for
    $appFile = Join-Path $ProjectDir $AppBuildFile
    if (-not (Test-Path $appFile) -and (Test-Path "$appFile.kts")) { $appFile = "$appFile.kts" }
    if (-not (Test-Path $appFile)) {
        Write-Fail "App build file not found: $appFile"
    } else {
        $leaf  = Split-Path $appFile -Leaf
        $lines = @(Get-Content $appFile)
        Write-Host "Reading required versions from $appFile"

        $q = '[\x27\x22]'
        $csdk = Find-Value $lines '^\s*compileSdk(?:Version)?\s*=?\s*(\d+(?:\.\d+)?)\b'
        if (-not $csdk) { $csdk = Find-Value $lines 'release\((\d+(?:\.\d+)?)' }
        if ($csdk) {
            Write-Pass "compileSdk $($csdk.Value)  ($leaf`:$($csdk.Line))"
            Add-Required "platforms;android-$($csdk.Value)"
            Add-Required "sources;android-$($csdk.Value)"
        } else {
            Write-Caution "compileSdk not found in $leaf; platform and sources packages not checked"
        }

        $bt = Find-Value $lines "^\s*buildToolsVersion\s*=?\s*$q([^\x27\x22]+)$q"
        if ($bt) {
            Write-Pass "buildToolsVersion $($bt.Value)  ($leaf`:$($bt.Line))"
            Add-Required "build-tools;$($bt.Value)"
        } else {
            Write-Caution "buildToolsVersion is not pinned in $leaf; AGP picks its default, so requiring build-tools $DefaultBuildTools (-DefaultBuildTools to change)"
            Add-Required "build-tools;$DefaultBuildTools"
        }

        $ndk = Find-Value $lines "^\s*ndkVersion\s*=?\s*$q([^\x27\x22]+)$q"
        if ($ndk) {
            Write-Pass "ndkVersion $($ndk.Value)  ($leaf`:$($ndk.Line))"
            Add-Required "ndk;$($ndk.Value)"
            $script:ndkVersion = $ndk.Value
        } else {
            Write-Caution "ndkVersion not found in $leaf; NDK not checked and ANDROID_NDK_HOME will not be set"
        }

        $cm = Find-Value $lines "^\s*version\s*=?\s*$q([\d\.]+)\+?$q"
        if ($cm) {
            Write-Pass "CMake $($cm.Value)  ($leaf`:$($cm.Line))"
            Add-Required "cmake;$($cm.Value)"
        } else {
            Write-Caution "CMake version is not pinned in $leaf; CMake not checked"
        }

        # The native build only defines the 'daytona' target when the generated sources exist
        $gen = Find-Value $lines 'M2_GEN_ROOT=([^\x27\x22]+)'
        if ($gen) {
            $genPath = $gen.Value.Replace('${rootProject.projectDir}', $ProjectDir) -replace '/', '\'
            $genPath = [IO.Path]::GetFullPath($genPath)
            $hasGen  = (Test-Path $genPath) -and
                       (Get-ChildItem $genPath -Recurse -Depth 2 -Filter gen_table.cpp -ErrorAction SilentlyContinue |
                            Select-Object -First 1)
            if ($hasGen) {
                Write-Pass "M2_GEN_ROOT $genPath has generated sources"
            } else {
                Write-Fail "M2_GEN_ROOT resolves to $genPath, which has no generated sources (gen_table.cpp). Without them CMake does not define the 'daytona' target. Point M2_GEN_ROOT ($leaf`:$($gen.Line)) at the folder that holds them."
            }
        }
    }
}

# --------------------------------------------------------------- 4. SDK packages
function Get-InstalledPackages($sm) {
    $result = @{}
    Write-Host "Running sdkmanager --list_installed (can take a few seconds)..."
    $prev = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $raw = & $sm --list_installed 2>&1 | Out-String
    $ErrorActionPreference = $prev
    foreach ($line in ($raw -split "`r?`n")) {
        if ($line -match '^\s*([A-Za-z][\w;\.\-]*)\s*\|\s*([^|]*?)\s*\|') {
            if ($Matches[1] -ne 'Path') { $result[$Matches[1]] = $Matches[2] }
        }
    }
    return $result
}

function Test-Packages($installed, $packages) {
    $script:missing = @()
    foreach ($pkg in $packages) {
        $ok = $false; $detail = ''
        if ($installed.ContainsKey($pkg)) {
            $ok = $true; $detail = "version $($installed[$pkg])"
        } elseif ($installed.Count -eq 0) {
            # sdkmanager unavailable: the package id maps to a folder, e.g. cmake;3.31.6 -> <sdk>\cmake\3.31.6
            if (Test-Path (Join-Path $sdk ($pkg -replace ';', '\'))) { $ok = $true; $detail = 'folder present' }
        }
        if ($ok) { Write-Pass "$pkg ($detail)" }
        else     { Write-Host "[MISS] $pkg" -ForegroundColor Red; $script:missing += $pkg }
    }
}

Write-Step "SDK packages"
$packages = @($script:required) + @($ExtraPackages)
if (-not $sdk) {
    Write-Fail "Skipped: no SDK"
} elseif ($packages.Count -eq 0) {
    Write-Caution "No packages to check (no versions found in the build file)"
} else {
    $sm = Join-Path $sdk 'cmdline-tools\latest\bin\sdkmanager.bat'
    if (-not (Test-Path $sm)) {
        $sm = Get-ChildItem (Join-Path $sdk 'cmdline-tools') -Directory -ErrorAction SilentlyContinue |
            ForEach-Object { Join-Path $_.FullName 'bin\sdkmanager.bat' } |
            Where-Object { Test-Path $_ } | Select-Object -First 1
    }

    $installed = @{}
    if (-not $sm) {
        Write-Caution "sdkmanager not found (install 'Android SDK Command-line Tools' in Studio); checking folders instead"
    } elseif (-not $env:JAVA_HOME) {
        Write-Caution "JAVA_HOME is not set, so sdkmanager cannot run; checking folders instead"
    } else {
        $installed = Get-InstalledPackages $sm
        if ($installed.Count -eq 0) { Write-Caution "sdkmanager returned no package list; checking folders instead" }
    }

    Test-Packages $installed $packages

    if ($script:missing.Count -gt 0 -and $InstallMissing) {
        if ($sm -and $env:JAVA_HOME) {
            Write-Host "Installing with sdkmanager (accept the licences when asked): $($script:missing -join ', ')"
            & $sm @($script:missing)
            $installed = Get-InstalledPackages $sm
            Test-Packages $installed $packages
        } else {
            Write-Caution "-InstallMissing needs sdkmanager and a working JAVA_HOME"
        }
    }

    if ($script:missing.Count -gt 0) {
        Write-Fail "Missing SDK packages: $($script:missing -join ', ')"
        Write-Host ""
        Write-Host "  Install them from Android Studio: Tools > SDK Manager." -ForegroundColor Yellow
        Write-Host "    - 'SDK Platforms' tab: tick 'Show Package Details', then the API platform and 'Sources for Android ...'" -ForegroundColor Yellow
        Write-Host "    - 'SDK Tools' tab: tick 'Show Package Details', then Build-Tools, CMake and NDK (Side by side) at the versions listed above" -ForegroundColor Yellow
        Write-Host "  The SDK Manager is a dialog inside Studio and cannot be launched on its own." -ForegroundColor Yellow
        if ($sm) {
            Write-Host "  The command-line equivalent works without opening Studio:" -ForegroundColor Yellow
            Write-Host "    & '$sm' $(($script:missing | ForEach-Object { "'$_'" }) -join ' ')" -ForegroundColor Yellow
            Write-Host "  or re-run this script with -InstallMissing." -ForegroundColor Yellow
        }
    }
}

# ----------------------------------------------------------------------- summary
Write-Step "Summary"
Write-Host "JAVA_HOME    = $env:JAVA_HOME"
Write-Host "ANDROID_HOME = $env:ANDROID_HOME"
if ($script:failed) {
    Write-Host "One or more prerequisites are not met; not building." -ForegroundColor Red
    exit 1
}
Write-Host "All prerequisites met." -ForegroundColor Green

# ---------------------------------------------------------------------- 5. build
if ($script:ndkVersion) {
    $env:ANDROID_NDK_HOME = Join-Path $sdk "ndk\$($script:ndkVersion)"
    Write-Pass "ANDROID_NDK_HOME = $env:ANDROID_NDK_HOME"
}
if ($NoBuild) { exit 0 }

$code = 1
Push-Location $ProjectDir
try {
    Write-Step ".\gradlew.bat --stop"
    & .\gradlew.bat --stop

    $gradleArgs = @(':app:assembleDebug')
    if (-not $NoRefresh) { $gradleArgs += '--refresh-dependencies' }
    Write-Step ".\gradlew.bat $($gradleArgs -join ' ')"
    & .\gradlew.bat @gradleArgs
    $code = $LASTEXITCODE
} finally {
    Pop-Location
}

if ($code -eq 0) {
    $apk = Join-Path $ProjectDir 'app\build\outputs\apk\debug\app-debug.apk'
    Write-Host ""
    Write-Host "Build succeeded." -ForegroundColor Green
    if (Test-Path $apk) { Write-Host "APK: $apk"; Write-Host "Install: adb install -r `"$apk`"" }
} else {
    Write-Host "Build failed (exit code $code)." -ForegroundColor Red
}
exit $code
