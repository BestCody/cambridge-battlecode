#!/bin/bash
tag=$1; ids=$(awk '{print $2}' analysis/pg10/groups_$tag.txt | tr -d '\r' | tr '\n' ' ')
for i in $(seq 1 60); do o=$(python -I analysis/pg8/show.py $ids </dev/null 2>&1); echo "$o" | grep -q "running\|queued" || break; sleep 20; done
echo "$o" > analysis/pg10/results_$tag.txt; echo "$o" | grep -v finished; date
