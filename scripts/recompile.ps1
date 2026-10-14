# Recompile-Daytona.ps1
#
# PowerShell port of recompile.py.
#
# Recompiles the i960, TGP and sound-board programs from ROMs,
# generates C++ sources, and builds the game.
#
# Intended to be dot-sourced by Zig-Setup.ps1 or invoked directly.
#
# Example:
#   . .\scripts\Recompile-Daytona.ps1
#   recompile
#   recompile -Set daytona -BuildDir build-daytona
#   recompile -FastInaccuracy $true
#
# Exit codes:
#   0 = success
#   1 = build or operational failure
#   3 = ROM set rejected by m2import

$script:RecompileRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)


function Invoke-RecompileCommand {
    param(
        [Parameter(Mandatory)]
        [string] $FilePath,

        [string[]] $Arguments = @(),

        [switch] $AllowNonZeroExitCode
    )

    Write-Host ("+ {0} {1}" -f $FilePath, ($Arguments -join ' '))

    & $FilePath @Arguments

    $script:LastRecompileExitCode = $LASTEXITCODE

    if ($script:LastRecompileExitCode -ne 0 -and
        -not $AllowNonZeroExitCode) {
        throw "Command failed with exit code $script:LastRecompileExitCode`: $FilePath"
    }
}

function Get-RecompileTool {
    param(
        [Parameter(Mandatory)]
        [string] $Build,

        [Parameter(Mandatory)]
        [string] $Config,

        [Parameter(Mandatory)]
        [string] $Name
    )

    $exe = if ($env:OS -eq 'Windows_NT') {
        "$Name.exe"
    } else {
        $Name
    }

    $candidates = @(
        (Join-Path $Build $exe)
        (Join-Path (Join-Path $Build $Config) $exe)
    )

    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate -PathType Leaf) {
            return $candidate
        }
    }

    throw "recompile: $Name was not built in $Build"
}

function recompile {
    [CmdletBinding()]
    param(
        [ValidateSet('daytona93', 'daytona')]
        [string] $Set = 'daytona93',

        [string] $BuildDir = 'build',

        [string] $Config = 'Release',

        [bool] $FastInaccuracy = $false
    )

    $root = $script:RecompileRoot
    $build = Join-Path $root $BuildDir
    $cache = Join-Path (Join-Path $build 'rom_cache') $Set

    $roms = @(
        (Join-Path $root "roms\$Set.zip")
        (Join-Path $root "roms\$Set.7z")
    ) | Where-Object { Test-Path -LiteralPath $_ -PathType Leaf }

    $buildCommand = @(
        '--build', $build,
        '--config', $Config
    )

    $oldLocation = Get-Location

    try {
        Set-Location -LiteralPath $root

        # Configure the build for the selected ROM set.
        Invoke-RecompileCommand -FilePath 'cmake' -Arguments @(
            '-S', '.',
            '-B', $build,
            "-DM2_ROMSET=$Set"
        ) | Out-Null

        # Import ROMs if any required cached files are missing.
        $requiredCacheFiles = @(
            'tgp_program.bin',
            'sound_program.bin',
            'pcm1.bin'
        )

        $cacheComplete = $true

        foreach ($file in $requiredCacheFiles) {
            if (-not (Test-Path -LiteralPath (Join-Path $cache $file) -PathType Leaf)) {
                $cacheComplete = $false
                break
            }
        }

        if (-not $cacheComplete) {
            if ($roms.Count -eq 0) {
                Write-Error "recompile: put your ROM set at roms\$Set.zip (or .7z) first"
                return 1
            }

            Invoke-RecompileCommand -FilePath 'cmake' -Arguments (
                $buildCommand + @('--target', 'm2import')
            ) | Out-Null

            $m2import = Get-RecompileTool `
                -Build $build `
                -Config $Config `
                -Name 'm2import'

            
			Invoke-RecompileCommand `
				-FilePath $m2import `
				-Arguments @($roms[0], $cache) `
				-AllowNonZeroExitCode

			if ($script:LastRecompileExitCode -ne 0) {
				Write-Error "recompile: m2import rejected the ROM set."
				return 3
			}
        }

        # Build the three recompilers.
        Invoke-RecompileCommand -FilePath 'cmake' -Arguments (
            $buildCommand + @(
                '--target',
                'm2recomp',
                'm2tgprecomp',
                'm2sndrecomp'
            )
        ) | Out-Null

        $m2recomp = Get-RecompileTool -Build $build -Config $Config -Name 'm2recomp'
        $m2tgprecomp = Get-RecompileTool -Build $build -Config $Config -Name 'm2tgprecomp'
        $m2sndrecomp = Get-RecompileTool -Build $build -Config $Config -Name 'm2sndrecomp'

        # Clear generated i960 sources, as the Python script does.
        $gen = Join-Path (Join-Path $build 'gen') $Set

        if (Test-Path -LiteralPath $gen -PathType Container) {
            Get-ChildItem -LiteralPath $gen -File |
                Remove-Item -Force
        } else {
            New-Item -ItemType Directory -Path $gen -Force | Out-Null
        }

        $recompArgs = @(
            (Join-Path $cache 'program.bin'),
            $gen,
            '--seeds',
            "seeds\$Set.txt"
        )

        $hooks = "seeds\${Set}_hooks.txt"

        if (Test-Path -LiteralPath (Join-Path $root $hooks) -PathType Leaf) {
            $recompArgs += @('--hooks', $hooks)
        }

        if ($FastInaccuracy) {
            $recompArgs += '--fast_inaccuracy'
        }

        Invoke-RecompileCommand -FilePath $m2recomp -Arguments $recompArgs |
            Out-Null

        # Generate TGP C++.
        $tgp = Join-Path (Join-Path $build 'gen') "${Set}_tgp"
        New-Item -ItemType Directory -Path $tgp -Force | Out-Null

        $tgpArgs = @(
            (Join-Path $cache 'tgp_program.bin'),
            (Join-Path $tgp 'tgp_gen.cpp')
        )

        if ($FastInaccuracy) {
            $tgpArgs += '--fast_inaccuracy'
        }

        Invoke-RecompileCommand -FilePath $m2tgprecomp -Arguments $tgpArgs |
            Out-Null

        # Generate sound-board C++.
        $snd = Join-Path (Join-Path $build 'gen') "${Set}_snd"
        New-Item -ItemType Directory -Path $snd -Force | Out-Null

        Invoke-RecompileCommand -FilePath $m2sndrecomp -Arguments @(
            (Join-Path $cache 'sound_program.bin'),
            (Join-Path $snd 'snd_gen.cpp')
        ) | Out-Null

        # Reconfigure to include the newly generated C++ files.
        Invoke-RecompileCommand -FilePath 'cmake' -Arguments @(
            '-S', '.',
            '-B', $build
        ) | Out-Null

        # Build the game and check tools.
        Invoke-RecompileCommand -FilePath 'cmake' -Arguments (
            $buildCommand + @('--parallel')
        ) | Out-Null

        return 0
    }
    catch {
        Write-Error "recompile: $($_.Exception.Message)"
        return 1
    }
    finally {
        Set-Location -LiteralPath $oldLocation
    }
}
```
