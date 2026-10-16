# benchmark.ps1 — parameter grid search over strategy.h `policy` constants.
#
# Usage:
#   pwsh benchmark.ps1 -Bot dumb-bot-6 -Opponent dumb-bot-3 -Maps collision,corners,elkring
#
# The grid is defined below. For every combination it:
#   1. patches the constant(s) in <Bot>/src/strategy.h
#   2. runs `cambc build <Bot>`
#   3. runs each map against <Opponent> with --statistics
#   4. records win/loss + both gem totals
# The original strategy.h is restored afterwards.
param(
    [string]$Bot = 'dumb-bot-6',
    [string]$Opponent = 'dumb-bot-3',
    [string[]]$Maps = @('collision', 'corners', 'elkring', 'expanse', 'interlock', 'lanes', 'monolith', 'packs', 'rows'),
    [string]$Out = (Join-Path $env:TEMP 'bench')
)

$ErrorActionPreference = 'Continue'
$root = (Get-Location).Path
$strategyPath = "$root\bots\$Bot\src\strategy.h"
$original = Get-Content $strategyPath -Raw
New-Item -ItemType Directory -Force -Path $Out | Out-Null

# ---------------------------------------------------------------- grid ----
# Each entry: @(name, type, value1, value2, ...) -> cartesian product.
$grid = @(
    @('commit_bonus', 'float', '0.98f', '1.03f', '1.08f'),
    @('bank_base',    'float', '30.0f', '42.0f', '56.0f')
)

function Set-Constant([string]$content, [string]$name, [string]$type, [string]$value) {
    $p = "inline constexpr $type $([regex]::Escape($name))\s*=\s*[^;]+;"
    if ($content -notmatch $p) { throw "constant not found: $name ($type)" }
    return ($content -replace $p, "inline constexpr $type $name = $value;")
}

# Cartesian product of the grid.
$combos = @([ordered]@{})
foreach ($entry in $grid) {
    $name = $entry[0]; $type = $entry[1]; $values = $entry[2..($entry.Count - 1)]
    $next = @()
    foreach ($c in $combos) {
        foreach ($v in $values) {
            $nc = [ordered]@{}
            foreach ($k in $c.Keys) { $nc[$k] = $c[$k] }
            $nc[$name] = $v
            $next += $nc
        }
    }
    $combos = $next
}

Write-Host "grid: $($combos.Count) combos x $($Maps.Count) maps = $($combos.Count * $Maps.Count) matches"

$results = @()
$i = 0
try {
    foreach ($combo in $combos) {
        $i++
        $content = $original
        $label = ''
        foreach ($k in $combo.Keys) {
            $content = Set-Constant $content $k 'float' $combo[$k]
            $label += "$k=$($combo[$k]) "
        }
        $label = $label.Trim()

        Set-Content -Path $strategyPath -Value $content -Encoding ASCII
        Write-Host "[$i/$($combos.Count)] $label : building..."
        & cambc build $Bot 2>$null | Out-Null

        $wins = 0; $gemsUs = 0; $gemsThem = 0
        foreach ($m in $Maps) {
            $stat = Join-Path $Out "$($label -replace ' ','_')-$m.json"
            & cambc run $Bot $Opponent --map $m --statistics $stat -o $Out 2>$null | Out-Null
            $j = Get-Content $stat -Raw | ConvertFrom-Json
            if ($j.outcome -eq 'teamA') { $wins++ }
            $gemsUs += $j.teams[0].submittedGems
            $gemsThem += $j.teams[1].submittedGems
        }
        $results += [pscustomobject]@{ combo = $label; wins = $wins; ourGems = $gemsUs; theirGems = $gemsThem }
        Write-Host ("  -> wins {0}/{1}  gems {2} vs {3}" -f $wins, $Maps.Count, $gemsUs, $gemsThem)
    }
}
finally {
    Set-Content -Path $strategyPath -Value $original -Encoding ASCII
    Write-Host "restored $strategyPath"
}

Write-Host ""
Write-Host "================= RESULTS (sorted by wins desc) ================="
$results | Sort-Object wins, ourGems -Descending | Format-Table -AutoSize
