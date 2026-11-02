#!/bin/bash
# usage: fetch.sh MATCH_ID DIR -> match.zip, summary.json, replay, layout.txt, rounds.txt
id=$(echo $1 | tr -d '\r'); d=$2; mkdir -p $d
cambc matches download $id -o $d/match.zip </dev/null >/dev/null 2>&1
cambc matches summary $id --json -o $d/summary.json </dev/null >/dev/null 2>&1
(cd $d && python -I -c "import zipfile; z=zipfile.ZipFile('match.zip'); [z.extract(n) for n in z.namelist()]" && ls)
r=$(ls $d/*.repl26a 2>/dev/null | head -1)
(cd scripts/analysis && node layout.mjs "../../$r" > "../../$d/layout.txt" 2>/dev/null && node rounds.mjs "../../$r" > "../../$d/rounds.txt" 2>/dev/null)
wc -l $d/rounds.txt
