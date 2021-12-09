#! /bin/bash

#ZSIM=/vagrant/build/opt/zsim
ZSIM=./../zsim_simulator/build/opt/zsim

cd ../buffer_overflow/

rm stats_time.out
cp buffer_overflow.cfg /tmp/buffer_overflow.cfg
cat /tmp/buffer_overflow.cfg | sed "s/server [0-8]/server 1/" | sed "s/attacker [0-8]/attacker 1/" > buffer_overflow.cfg
$ZSIM buffer_overflow.cfg # > /dev/null
grep cycles zsim.out | head -n 2 | sed 's/[^0-9]*//g' >> stats_time.out

