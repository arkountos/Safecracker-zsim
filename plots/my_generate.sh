#! /bin/bash

#ZSIM=/vagrant/build/opt/zsim
ZSIM=./../zsim_simulator/build/opt/zsim

cd ../heap_spray/

rm stats_time.out
#for n in 0 1 2 3 4 5 6; do
cp heap_spray.cfg /tmp/heap_spray.cfg
cat /tmp/heap_spray.cfg | sed "s/server [0-8]/server 2/" | sed "s/attacker [0-8]/attacker 2/" > heap_spray.cfg
$ZSIM heap_spray.cfg # > /dev/null
grep cycles zsim.out | head -n 2 | sed 's/[^0-9]*//g' >> stats_time.out
#done

#cd ../plots/
#python plot_heap_spray.py
#
#cd ../buffer_overflow/
#
#rm stats_time.out
#for n in 0 1 2 3 4 5 6 7 8; do
#    cp buffer_overflow.cfg /tmp/buffer_overflow.cfg
#    cat /tmp/buffer_overflow.cfg | sed "s/server [0-8]/server $n/" | sed "s/attacker [0-8]/attacker $n/" > buffer_overflow.cfg
#    $ZSIM buffer_overflow.cfg # > /dev/null
#    grep cycles zsim.out | head -n 2 | sed 's/[^0-9]*//g' >> stats_time.out
#done

#cd ../plots/
#python plot_buffer_overflow.py 
