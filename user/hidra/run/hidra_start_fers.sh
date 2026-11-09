!/usr/bin/env sh
BINPATH=../../../bin
$BINPATH/euRun -n HidraRunControl &
$BINPATH/euCliProducer -n HidraFERS2Producer -t FERS2Producer  
