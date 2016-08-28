#!/bin/bash
_DEBUG=off
#_DEBUG=on

. $(dirname $0)/utils.sh

startSQLRelay

. $(dirname $0)/testInitialData.sh

startHSPGW


initTestStatistics

. $(dirname $0)/scen_pms_1.sh

dummy1
scen11
scen12
scen13
scen14
scen15
scen16

. $(dirname $0)/scen_pms_2.sh

dummy2
scen21
scen22
scen23
scen24
scen25
scen26

showTestStatistics

killHSPGW

#killSQLRelay

echo Returns: $rc
exit $rc
