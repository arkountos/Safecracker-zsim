#!/bin/bash

for n in 1 2 3 4;do
counter=$(($n * 1000))
echo $counter
cp heap_spray.cfg /tmp/heap_spray.cfg
cat /tmp/heap_spray.cfg | sed "s/server [0-9 ]*/server 2 $counter/" | sed "s/attacker [0-9 ]*/attacker 200/" > heap_spray.cfg
done
