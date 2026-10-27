# ladder.ps1 — per-map win-rate matrix for one bot against several opponents.
#
# The event format is 5 maps, best-of-5, drawn from a fixed pool, so the metric
# that matters is the per-map win rate. With a fixed pool a 7/9 record means at
# most 2 losing maps can appear in any 5-map set, so no Bo5 can be lost.
#
# The pool is read from maps/*.map26a, so `cambc maps pull` is picked up
# automatically -- the published pool changes during the competition.
#
# Usage (from any directory):
#   pwsh scripts/ladder.ps1 -Build
#   pwsh scripts/ladder.ps1 -Bot test-bot -Opponents starter-cpp,baseline
# Opponents are bot folder names under bots/.
param(
    [string]$Bot = 'test-bot',
    [string[]]$Opponents = @('starter-cpp'),
    # Empty = every map in the current pool.
    [string[]]$Maps = @(),
    [switch]$Build,
    [string]$Out = (Join-Path $env:TEMP 'ladder')
)

$ErrorActionPreference = 'Continue'
# cambc resolves bots and maps relative to the repository root.
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if ($Maps.Count -eq 0) {
    $Maps = @(Get-ChildItem (Join-Path $root 'maps') -Filter *.map26a | ForEach-Object { $_.BaseName })
    Write-Host "pool: $($Maps.Count) maps"
}

if ($Build) {
    Write-Host "building $Bot ..."
    & cambc build $Bot --release 2>$null | Out-Null
}

# Every map is played from both seats (the seats' results differ on about a
# quarter of the maps), in release mode as on the platform. The summary is
# read as JSON: match.outcome names the winning team, teams[] the tributed gems.
$rows = @()
$perMap = [ordered]@{}
foreach ($o in $Opponents) {
    $wins = 0; $gemsUs = 0; $gemsThem = 0; $lost = @()
    foreach ($m in $Maps) {
        foreach ($seat in @('A', 'B')) {
            $stat = Join-Path $Out "$Bot-vs-$o-$m-$seat.json"
            if ($seat -eq 'A') {
                & cambc run $Bot $o --map $m --release --statistics $stat --json -o $Out 2>$null | Out-Null
            } else {
                & cambc run $o $Bot --map $m --release --statistics $stat --json -o $Out 2>$null | Out-Null
            }
            $ours = "team$seat"
            $j = Get-Content $stat -Raw | ConvertFrom-Json
            $won = $j.match.outcome -eq $ours
            if ($won) { $wins++ } else { $lost += "$m($seat)" }
            $gemsUs += [int](($j.teams | Where-Object { $_.team -eq $ours }).submittedGems)
            $gemsThem += [int](($j.teams | Where-Object { $_.team -ne $ours }).submittedGems)
            if (-not $perMap.Contains($m)) { $perMap[$m] = @() }
            $perMap[$m] += $(if ($won) { 'W' } else { 'L' })
        }
    }
    $games = 2 * $Maps.Count
    $rows += [pscustomobject]@{
        bot       = $Bot
        opponent  = $o
        botWins   = "$wins/$games"
        rate      = '{0:P0}' -f ($wins / $games)
        # 7/9 per-map rate is the event bar (any 5-map subset then holds <= 2 losses).
        clearsBar = (($wins / $games) -ge (7 / 9))
        ourGems   = $gemsUs
        theirGems = $gemsThem
        botLostMaps = ($lost -join ',')
    }
    Write-Host ("{0} vs {1,-14} {2}/{3}  gems {4} vs {5}  lost: {6}" -f $Bot, $o, $wins, $games, $gemsUs, $gemsThem, ($lost -join ','))
}

Write-Host ""
Write-Host "================= $Bot per-map win rate (both seats) ================="
$rows | Format-Table -AutoSize

Write-Host "per-map result across all opponents:"
foreach ($m in $perMap.Keys) {
    $r = $perMap[$m]
    $w = ($r | Where-Object { $_ -eq 'W' }).Count
    Write-Host ("  {0,-10} {1}/{2}  {3}" -f $m, $w, $r.Count, ($r -join ''))
}

Pop-Location
