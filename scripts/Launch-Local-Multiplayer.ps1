[CmdletBinding()]
param(
    [string]$SourceInstallRoot = '',
    [string]$BuildDirectory = '..\build\skate3-custom-engine-layer-release',
    [ValidateRange(2, 100)]
    [int]$Clients = 2,
    [string]$CacAssetRoot = '',
    [switch]$NoDirectBoot,
    # Retail single-player world content is suppressed by default here,
    # because this script stages an ONLINE session: Skate 3's missions write
    # progress into the player's save, and its AI skaters land tricks that a
    # shared game mode has no way to attribute. Pass this to get the retail
    # world back for comparison.
    [switch]$KeepRetailContent
)

$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $PSScriptRoot '..')
)
if ([string]::IsNullOrWhiteSpace($SourceInstallRoot)) {
    $SourceInstallRoot = Join-Path (
        Split-Path $repoRoot -Parent
    ) 'Skate3CustomEngineLayer-Player'
}
$SourceInstallRoot = [System.IO.Path]::GetFullPath($SourceInstallRoot)
$buildRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot $BuildDirectory)
)
$clientRoot = [System.IO.Path]::GetFullPath(
    (Join-Path $repoRoot 'out\local-multiplayer')
)

function Test-CompleteCacAssetRoot {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Container)) {
        return $false
    }
    $createACaracterRoot = Split-Path (
        Split-Path $Path -Parent
    ) -Parent
    $textureRoot = Join-Path $createACaracterRoot 'texture'
    if (-not (Test-Path -LiteralPath $textureRoot -PathType Container)) {
        return $false
    }

    # The old memory-snapshot catalogue padded every file to 128 KiB and
    # omitted the lower half of 512x512 clothing atlases. A complete archive
    # extraction contains many larger RX2 files.
    $largeTexture = Get-ChildItem -LiteralPath $textureRoot `
        -File -Filter '*.rx2' |
        Where-Object Length -GT 131072 |
        Select-Object -First 1
    return $null -ne $largeTexture
}

if ([string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    $documentsRoot = Split-Path (
        Split-Path $repoRoot -Parent
    ) -Parent
    $cacheRoot = Join-Path $repoRoot 'out\createacharacter-full'
    $candidate = Join-Path $cacheRoot (
        'data\content\' +
        'createacharacter\model\cas_db'
    )
    if (-not (Test-CompleteCacAssetRoot $candidate)) {
        $archive = Join-Path $SourceInstallRoot (
            'game\data\content\createacharacter.big'
        )
        $extractor = Join-Path $documentsRoot (
            'Skate3Research\UTT-1.1.7\assets\bigfile.exe'
        )
        if ((Test-Path -LiteralPath $archive -PathType Leaf) -and
            (Test-Path -LiteralPath $extractor -PathType Leaf)) {
            Write-Host (
                'Extracting the complete Create-a-Skater catalogue once...'
            )
            New-Item -ItemType Directory -Path $cacheRoot -Force |
                Out-Null
            Push-Location $cacheRoot
            try {
                & $extractor $archive -x 2>&1 | Out-Null
                if ($LASTEXITCODE -ne 0) {
                    throw (
                        "bigfile.exe failed with exit code $LASTEXITCODE"
                    )
                }
            } finally {
                Pop-Location
            }
        }
    }
    if (Test-CompleteCacAssetRoot $candidate) {
        $CacAssetRoot = $candidate
    }
}
if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    $CacAssetRoot = [System.IO.Path]::GetFullPath($CacAssetRoot)
    if (-not (Test-CompleteCacAssetRoot $CacAssetRoot)) {
        throw (
            'Create-a-Skater asset root is missing complete RX2 texture ' +
            "payloads: $CacAssetRoot"
        )
    }
}

$builtExecutable = Join-Path $buildRoot 'skate3.exe'
$builtRuntime = Join-Path $buildRoot 'rexruntime.dll'
$sourceExecutable = Join-Path $SourceInstallRoot 'skate3.exe'
$sourceRuntime = Join-Path $SourceInstallRoot 'rexruntime.dll'
$executable = if (Test-Path -LiteralPath $builtExecutable -PathType Leaf) {
    $builtExecutable
} else {
    $sourceExecutable
}
$runtime = if (Test-Path -LiteralPath $builtRuntime -PathType Leaf) {
    $builtRuntime
} else {
    $sourceRuntime
}
$gameRoot = Join-Path $SourceInstallRoot 'game'
$mapsRoot = Join-Path $SourceInstallRoot 'maps'
$objectsRoot = Join-Path $SourceInstallRoot 'objects'

foreach ($required in @(
        $executable, $runtime, $gameRoot, $mapsRoot, $objectsRoot
    )) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Local multiplayer source is missing: $required"
    }
}

$running = Get-Process -Name 'skate3' -ErrorAction SilentlyContinue
if ($null -ne $running) {
    throw 'Close existing skate3.exe processes before staging two clients.'
}

function Ensure-Junction {
    param(
        [Parameter(Mandatory)][string]$Path,
        [Parameter(Mandatory)][string]$Target
    )
    if (Test-Path -LiteralPath $Path) {
        $item = Get-Item -LiteralPath $Path -Force
        if (-not ($item.Attributes -band
                  [System.IO.FileAttributes]::ReparsePoint)) {
            throw "Expected a junction but found a real item: $Path"
        }
        return
    }
    New-Item -ItemType Junction -Path $Path -Target $Target | Out-Null
}

New-Item -ItemType Directory -Path $clientRoot -Force | Out-Null
$seedRoot = Join-Path $env:APPDATA 'skate3'
$stagedClients = @()
foreach ($role in 1..$Clients) {
    $root = Join-Path $clientRoot "client$role"
    New-Item -ItemType Directory -Path $root -Force | Out-Null
    Copy-Item -LiteralPath $executable -Destination (
        Join-Path $root 'skate3.exe'
    ) -Force
    Copy-Item -LiteralPath $runtime -Destination (
        Join-Path $root 'rexruntime.dll'
    ) -Force
    # The CEF runtime, if this build has one. skate3.exe IMPORTS libcef.dll,
    # so a staged client without these files either refuses to start or comes
    # up with no in-game UI at all - the dev console and every NUI page are
    # CEF surfaces. Copied from beside the build output rather than from the
    # SDK, so a client always gets the same CEF the exe was linked against.
    $cefSource = Split-Path $executable -Parent
    foreach ($cefFile in @(
            'libcef.dll', 'chrome_elf.dll', 'd3dcompiler_47.dll',
            'dxcompiler.dll', 'dxil.dll', 'libEGL.dll', 'libGLESv2.dll',
            'v8_context_snapshot.bin', 'snapshot_blob.bin',
            'vk_swiftshader.dll', 'vk_swiftshader_icd.json', 'vulkan-1.dll',
            'icudtl.dat', 'chrome_100_percent.pak', 'chrome_200_percent.pak',
            'resources.pak'
        )) {
        $from = Join-Path $cefSource $cefFile
        if (Test-Path -LiteralPath $from -PathType Leaf) {
            Copy-Item -LiteralPath $from -Destination (
                Join-Path $root $cefFile
            ) -Force
        }
    }
    $localesSource = Join-Path $cefSource 'locales'
    if (Test-Path -LiteralPath $localesSource -PathType Container) {
        Copy-Item -LiteralPath $localesSource -Destination $root `
            -Recurse -Force
    }
    New-Item -ItemType File -Path (
        Join-Path $root 'portable.txt'
    ) -Force | Out-Null
    Ensure-Junction -Path (Join-Path $root 'game') -Target $gameRoot
    Ensure-Junction -Path (Join-Path $root 'maps') -Target $mapsRoot
    Ensure-Junction -Path (Join-Path $root 'objects') -Target $objectsRoot

    # Seed each isolated portable client with the user's existing profile once.
    # The two copies can then save concurrently without sharing writable data.
    if (Test-Path -LiteralPath $seedRoot -PathType Container) {
        Get-ChildItem -LiteralPath $seedRoot -Directory |
            Where-Object Name -Match '^[0-9A-Fa-f]{16}$' |
            ForEach-Object {
                $destination = Join-Path $root $_.Name
                if (-not (Test-Path -LiteralPath $destination)) {
                    Copy-Item -LiteralPath $_.FullName -Destination $destination `
                        -Recurse
                }
            }
    }

    $arguments = @(
        '--fullscreen=false',
        '--window_width=1120',
        '--window_height=630',
        '--draw_resolution_scale_x=1',
        '--draw_resolution_scale_y=1',
        '--skate3_input_lab=false',
        '--skate3_multiplayer_local_visuals=true',
        '--skate3_multiplayer_local_lane_spacing=0',
        "--skate3_multiplayer_local_client=$role",
        "--skate3_multiplayer_local_peer_count=$Clients"
    )
    if (-not $NoDirectBoot) {
        $arguments += '--skate3_direct_boot=true'
    }
    if (-not $KeepRetailContent) {
        # Both need to be set before the world loads, which a command-line
        # flag is: they are marked as requiring a restart precisely because
        # flipping them in the console mid-session does nothing for content
        # that has already streamed in.
        $arguments += '--skate3_no_retail_missions=true'
        $arguments += '--skate3_no_ai_skaters=true'
    }
    if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
        $arguments += (
            '--skate3_multiplayer_cac_asset_root={0}' -f
            $CacAssetRoot
        )
    }
    $stagedClients += [pscustomobject]@{
        Role = $role
        Root = $root
        Executable = Join-Path $root 'skate3.exe'
        Arguments = $arguments
    }
}

$cefStaged = Test-Path -LiteralPath (
    Join-Path $stagedClients[0].Root 'libcef.dll'
) -PathType Leaf
if (-not $cefStaged) {
    Write-Warning (
        'No libcef.dll was staged: these clients will have no dev console ' +
        'and no NUI. That is expected for a build configured without ' +
        '-DSKATE3_ENABLE_CEF=ON, and a bug otherwise.'
    )
}

foreach ($client in $stagedClients) {
    Write-Host (
        "Launching local multiplayer client {0} from {1}" -f
        $client.Role, $client.Root
    )
    Start-Process -FilePath $client.Executable `
        -WorkingDirectory $client.Root `
        -ArgumentList $client.Arguments
}

Write-Host ''
Write-Host (
    "$Clients clients use the same game, maps, and object library " +
    'read-only through junctions.'
)
Write-Host 'Their settings, caches, logs, and saves are isolated under:'
Write-Host "  $clientRoot"
Write-Host 'Client 1 is the logical host; nearby peers use the real animated skater and board.'
if (-not [string]::IsNullOrWhiteSpace($CacAssetRoot)) {
    Write-Host "CAC bind assets: $CacAssetRoot"
}
Write-Host 'Remote collision is disabled.'
if ($KeepRetailContent) {
    Write-Host (
        'Retail missions and AI skaters are ENABLED (-KeepRetailContent): ' +
        'expect mission markers, and AI skater tricks in the world.'
    )
} else {
    Write-Host (
        'Retail missions and AI skaters are suppressed. Pass ' +
        '-KeepRetailContent to restore them.'
    )
}
