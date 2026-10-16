# ladder.ps1 — per-map win-rate matrix for one bot against several opponents.
#
# The event format is 5 maps, best-of-5, drawn from the fixed 9-map pool, so the
# metric that matters is the per-map win rate. With a fixed pool a 7/9 record
# means at most 2 losing maps can appear in any 5-map set, so no Bo5 can be lost.
#
# Usage:
#   pwsh ladder.ps1 -Bot db12-comeback -Build
#   pwsh ladder.ps1 -Bot db12-comeback -Opponents dumb-bot-6,dumb-bot-5
param(
    [string]$Bot = 'db12-comeback',
    [string[]]$Opponents = @(
        'dumb-bot-6', 'dumb-bot-5', 'dumb-bot-4', 'dumb-bot-3',
        'dumb-bot-9', 'db10-macro', 'db11-looter'
    ),
    [string[]]$Maps = @('collision', 'corners', 'elkring', 'expanse', 'interlock', 'lanes', 'monolith', 'packs', 'rows'),
    [switch]$Build,
    [string]$Out = (Join-Path $env:TEMP 'ladder')
)

$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
New-Item -ItemType Directory -Force -Path $Out | Out-Null

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
        opponent  = $o
        wins      = "$wins/$($Maps.Count)"
        rate      = '{0:P0}' -f ($wins / $Maps.Count)
        bo5margin = $wins -ge 7
        ourGems   = $gemsUs
        theirGems = $gemsThem
        lostMaps  = ($lost -join ',')
    }
    Write-Host ("{0,-14} {1}/{2}  gems {3} vs {4}  lost: {5}" -f $o, $wins, $Maps.Count, $gemsUs, $gemsThem, ($lost -join ','))
}

Write-Host ""
Write-Host "================= $Bot per-map win rate ================="
$rows | Format-Table -AutoSize

Write-Host "per-map result across all opponents:"
foreach ($m in $perMap.Keys) {
    $r = $perMap[$m]
    $w = ($r | Where-Object { $_ -eq 'W' }).Count
    Write-Host ("  {0,-10} {1}/{2}  {3}" -f $m, $w, $r.Count, ($r -join ''))
}
