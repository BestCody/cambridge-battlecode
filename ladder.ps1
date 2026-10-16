# ladder.ps1 — per-map win-rate matrix for one bot against several opponents.
#
# The event format is 5 maps, best-of-5, drawn from a fixed pool, so the metric
# that matters is the per-map win rate. With a fixed pool a 7/9 record means at
# most 2 losing maps can appear in any 5-map set, so no Bo5 can be lost.
#
# The pool is read from maps/*.map26a, so `cambc maps pull` is picked up
# automatically -- the published pool changes during the competition.
#
# Usage:
#   pwsh ladder.ps1 -Bot db6-1 -Build
#   pwsh ladder.ps1 -Bot db6-1 -Opponents dumb-bot-6,dumb-bot-3
param(
    [string]$Bot = 'db6-1',
    [string[]]$Opponents = @(
        'dumb-bot-6', 'dumb-bot-5', 'dumb-bot-4', 'dumb-bot-3',
        'dumb-bot-9', 'db10-macro', 'db11-looter'
    ),
    # Empty = every map in the current pool.
    [string[]]$Maps = @(),
    [switch]$Build,
    [string]$Out = (Join-Path $env:TEMP 'ladder')
)

$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if ($Maps.Count -eq 0) {
    $Maps = @(Get-ChildItem (Join-Path $root 'maps') -Filter *.map26a | ForEach-Object { $_.BaseName })
    Write-Host "pool: $($Maps.Count) maps"
}

if ($Build) {
    Write-Host "building $Bot ..."
    & cambc build $Bot 2>$null | Out-Null
}

$rows = @()
$perMap = [ordered]@{}
foreach ($o in $Opponents) {
    $wins = 0; $gemsUs = 0; $gemsThem = 0; $lost = @()
    foreach ($m in $Maps) {
        $stat = Join-Path $Out "$Bot-vs-$o-$m.json"
        & cambc run $Bot $o --map $m --statistics $stat -o $Out 2>$null | Out-Null
        $j = Get-Content $stat -Raw | ConvertFrom-Json
        if ($j.outcome -eq 'teamA') { $wins++ } else { $lost += $m }
        $gemsUs += $j.teams[0].submittedGems
        $gemsThem += $j.teams[1].submittedGems
        if (-not $perMap.Contains($m)) { $perMap[$m] = @() }
        $perMap[$m] += $(if ($j.outcome -eq 'teamA') { 'W' } else { 'L' })
    }
    $rows += [pscustomobject]@{
        bot       = $Bot
        opponent  = $o
        botWins   = "$wins/$($Maps.Count)"
        rate      = '{0:P0}' -f ($wins / $Maps.Count)
        # 7/9 per-map rate is the event bar (any 5-map subset then holds <= 2 losses).
        clearsBar = (($wins / $Maps.Count) -ge (7 / 9))
        ourGems   = $gemsUs
        theirGems = $gemsThem
        botLostMaps = ($lost -join ',')
    }
    Write-Host ("{0} (team A) vs {1,-14} {2}/{3}  gems {4} vs {5}  A lost: {6}" -f $Bot, $o, $wins, $Maps.Count, $gemsUs, $gemsThem, ($lost -join ','))
}

Write-Host ""
Write-Host "================= $Bot per-map win rate (team A) ================="
$rows | Format-Table -AutoSize

Write-Host "per-map result across all opponents:"
foreach ($m in $perMap.Keys) {
    $r = $perMap[$m]
    $w = ($r | Where-Object { $_ -eq 'W' }).Count
    Write-Host ("  {0,-10} {1}/{2}  {3}" -f $m, $w, $r.Count, ($r -join ''))
}
