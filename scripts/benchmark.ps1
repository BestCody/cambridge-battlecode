# benchmark.ps1 — parameter grid search over strategy.h `policy` constants.
#
# Usage:
#   pwsh scripts/benchmark.ps1 -Bot test-bot -Opponent starter-cpp -Maps contest,elkring
#   pwsh scripts/benchmark.ps1 -GridSpec "commit_bonus=1.0f,1.08f,1.2f"
#
# The grid is defined below. For every combination it:
#   1. patches the constant(s) in bots/<Bot>/src/strategy.h
#   2. runs `cambc build <Bot> --release`
#   3. runs each map against <Opponent> from both seats, in release mode, with
#      a JSON --statistics summary
#   4. records win/loss + both gem totals
# The original strategy.h is restored afterwards. Works from any directory.
param(
    [string]$Bot = 'test-bot',
    [string[]]$Opponent = @('starter-cpp'),
    # Empty = every map in the current pool (maps/*.map26a). The published pool
    # changes during the competition, so it is read rather than hard-coded.
    [string[]]$Maps = @(),
    [string]$Out = (Join-Path $env:TEMP 'bench'),
    # Optional grid spec: "name:type=v1,v2;name2:type=v1,v2" (type defaults to float).
    # When empty, the $grid literal below is used.
    # NOTE: must NOT be named $Grid -- PowerShell variable names are case-insensitive,
    # so it would collide with the $grid literal below.
    [string]$GridSpec = ''
)

$ErrorActionPreference = 'Continue'
# cambc resolves bots and maps relative to the repository root.
$root = Split-Path $PSScriptRoot -Parent
Push-Location $root
$strategyPath = "$root\bots\$Bot\src\strategy.h"
# Read and written as UTF-8 without a byte-order mark, so the file keeps its
# exact text and line endings.
$utf8 = New-Object System.Text.UTF8Encoding($false)
$original = [System.IO.File]::ReadAllText($strategyPath, $utf8)
New-Item -ItemType Directory -Force -Path $Out | Out-Null

if ($Maps.Count -eq 0) {
    $Maps = @(Get-ChildItem (Join-Path $root 'maps') -Filter *.map26a | ForEach-Object { $_.BaseName })
    Write-Host "pool: $($Maps.Count) maps"
}

# ---------------------------------------------------------------- grid ----
# Each entry: @(name, type, value1, value2, ...) -> cartesian product.
$grid = @(
    ,@('late_gold_value', 'float', '0.10f', '0.25f', '0.40f')
)

$types = @{}
if ($GridSpec.Trim()) {
    $grid = @()
    foreach ($part in ($GridSpec -split ';')) {
        if (-not $part.Trim()) { continue }
        $kv = $part -split '=', 2
        $head = $kv[0] -split ':'
        $name = $head[0].Trim()
        $type = if ($head.Count -gt 1) { $head[1].Trim() } else { 'float' }
        $entry = @($name, $type)
        foreach ($v in ($kv[1] -split ',')) { $entry += $v.Trim() }
        $grid += ,$entry
    }
}
foreach ($e in $grid) { $types[$e[0]] = $e[1] }

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

# Every map is played from both seats, in release mode as on the platform.
$total = 2 * $Maps.Count * $Opponent.Count
Write-Host "grid: $($combos.Count) combos x $($Maps.Count) maps x $($Opponent.Count) opponents = $($combos.Count * $total) matches"

$results = @()
$i = 0
try {
    foreach ($combo in $combos) {
        $i++
        $content = $original
        $label = ''
        foreach ($k in $combo.Keys) {
            $content = Set-Constant $content $k $types[$k] $combo[$k]
            $label += "$k=$($combo[$k]) "
        }
        $label = $label.Trim()

        [System.IO.File]::WriteAllText($strategyPath, $content, $utf8)
        Write-Host "[$i/$($combos.Count)] $label : building..."
        & cambc build $Bot --release 2>$null | Out-Null

        $wins = 0; $gemsUs = 0; $gemsThem = 0; $detail = @(); $oppWins = @()
        foreach ($o in $Opponent) {
            $oWins = 0
            foreach ($m in $Maps) {
                foreach ($seat in @('A', 'B')) {
                    $stat = Join-Path $Out "$($label -replace ' ','_')-$o-$m-$seat.json"
                    if ($seat -eq 'A') {
                        & cambc run $Bot $o --map $m --release --statistics $stat --json -o $Out 2>$null | Out-Null
                    } else {
                        & cambc run $o $Bot --map $m --release --statistics $stat --json -o $Out 2>$null | Out-Null
                    }
                    $ours = "team$seat"
                    $j = Get-Content $stat -Raw | ConvertFrom-Json
                    if ($j.match.outcome -eq $ours) { $wins++; $oWins++ }
                    $gemsUs += [int](($j.teams | Where-Object { $_.team -eq $ours }).submittedGems)
                    $gemsThem += [int](($j.teams | Where-Object { $_.team -ne $ours }).submittedGems)
                }
            }
            $oppWins += $oWins
            $detail += "$($o -replace 'dumb-bot-','db'):$oWins/$(2 * $Maps.Count)"
        }
        $detailStr = $detail -join ' '
        # The event format is 5 maps best-of-5 from the fixed pool, so the metric
        # that matters is the per-map rate, and the binding constraint is the
        # WORST opponent. minWins is that worst-case per-map win count.
        $minWins = ($oppWins | Measure-Object -Minimum).Minimum
        $maxWins = ($oppWins | Measure-Object -Maximum).Maximum
        $results += [pscustomobject]@{ combo = $label; minWins = $minWins; maxWins = $maxWins; wins = $wins; total = $total; ourGems = $gemsUs; theirGems = $gemsThem; detail = $detailStr }
        Write-Host ("  -> wins {0}/{1}  worst {2}/{3}  gems {4} vs {5}   [{6}]" -f $wins, $total, $minWins, (2 * $Maps.Count), $gemsUs, $gemsThem, $detailStr)
    }
}
finally {
    [System.IO.File]::WriteAllText($strategyPath, $original, $utf8)
    Write-Host "restored $strategyPath"
    Pop-Location
}

Write-Host ""
Write-Host "============ RESULTS (sorted by worst-opponent per-map wins, then total) ============"
$results | Sort-Object minWins, wins, ourGems -Descending | Format-Table -AutoSize
