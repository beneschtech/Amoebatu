../../cmake-build-release/src/dolphin/dolphin --stats --threads 1  examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 1 --fixed-thresholds examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 2  examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 2 --fixed-thresholds examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 4  examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 4 --fixed-thresholds examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 8  examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 8 --fixed-thresholds examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 16  examples/metar.dol /tmp/load.txt > /dev/null &
../../cmake-build-release/src/dolphin/dolphin --stats --threads 16 --fixed-thresholds examples/metar.dol /tmp/load.txt > /dev/null &
