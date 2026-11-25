#!/bin/bash

njobs=4
if [ $# -gt 0 ]; then
		njobs=$1
fi

i=0

while [ $i -lt $njobs ]; do
		../../cmake-build-release/src/dolphin/dolphin --stats examples/metar.dcl /tmp/load.txt > /dev/null &
		i=$((i+1))
done
