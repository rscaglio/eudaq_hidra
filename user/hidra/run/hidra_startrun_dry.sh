#!/usr/bin/env sh



BINPATH=../../../bin
mkdir -p out_data logs
$BINPATH/euRun -n HidraRunControl & 
sleep 1
$BINPATH/euLog > $HOME/temp.eudaq.log &
sleep 1
$BINPATH/euCliCollector -n HidraDataCollector -t HidraDataCollector &
sleep 1
$BINPATH/euCliProducer -n HidraDryFERSProducer -t DryFERSProducer &
$BINPATH/euCliProducer -n HidraDryXDCProducer -t DryXDCProducer
