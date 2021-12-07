#! /bin/bash

#ZSIM=/vagrant/build/opt/zsim
ZSIM=./../zsim_simulator/build/opt/zsim

cd ../heap_spray/

rm stats_time.out
cp heap_spray.cfg /tmp/heap_spray.cfg
cat /tmp/heap_spray.cfg | sed "s/server [0-8]/server 1/" | sed "s/attacker [0-8]/attacker 1/" > heap_spray.cfg
$ZSIM heap_spray.cfg # > /dev/null
grep cycles zsim.out | head -n 2 | sed 's/[^0-9]*//g' >> stats_time.out

