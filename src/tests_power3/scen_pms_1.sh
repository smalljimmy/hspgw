#!/bin/bash
# -------------------------------------------------------------------------
# Testscenarios for PMS 1 (Fidelio)
# -------------------------------------------------------------------------

_DEBUG=off
#_DEBUG=on

. $(dirname $0)/utils.sh

# do a dummy transaction to wake up system
dummy1 () {
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 40 1680 112 fritsche 111
}

scen11 () {
	initTestScenario 11 "Per-time tariff, no grouping"
	
	initTestCase 111a "Authentication with PMS request"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 40 1680 112 fritsche 111
	testHspgwResponse Cisco-Account-Info roaming@hspgw-21
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1680,112,fritsche,40,0
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='fritsche'" fritsche
	testPMSRequest '10.10.10.75.*PR|.*|PI112|'
	
	initTestCase 111b "Accounting - expect entry billed"
	runHspgw acct 40 1680 112 fritsche 111	
	testPMSRequest '10.10.10.75.*PR|.*|RN112|.*|TA100|'
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 112a "Authentication by ocuupancy"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'	
	runHspgw auth 40 1680 112 fritsche 112
	testNoPMSRequest   # PMS is not requested
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1680,112,fritsche,40,0
	testLog 'Authentication by occupancy table'

	initTestCase 113a "Authentication by tracker"
	runHspgw auth 40 1680 112 fritsche 113
	testNoPMSRequest   # PMS is not requested
	testLog 'authentication in ACCOUNT_TRACKER table'

}

scen12 () {
	initTestScenario 12 "Per-time tariff, grouping, checkin and login zones same"

	initTestCase 121a "Authentication with PMS request"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 42 1682 309 jucker 121 	
	testHspgwResponse Cisco-Account-Info roaming@hspgw-23
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' '1682,309,jucker,42,0'
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='jucker'" jucker
	testPMSRequest '10.10.10.78.*PR|.*|PI309|'
	
	initTestCase 121b "Accounting - expect entry billed"
	runHspgw acct 42 1682 309 jucker 121
	testPMSRequest '10.10.10.78.*PR|.*|RN309|.*|TA300|' 
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 122a "Authentication with lookup in ocuupancy - no PMS request"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'	
	runHspgw auth 42 1682 309 jucker 122
	testNoPMSRequest   # PMS is not requested
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' '1682,309,jucker,42,0'
	testLog 'Authentication by occupancy table'

	initTestCase 123a "Authentication with lookup in HAT - no PMS request"
	runHspgw auth 42 1682 309 jucker 123
	testNoPMSRequest   # PMS is not requested
	testLog 'authentication in ACCOUNT_TRACKER table'

}

scen13 () {
	initTestScenario 13 "Per-time tariff, grouping, checkin and login zones different"

	initTestCase 131a "Authentication with PMS request"

	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 41 1682 112 fritsche 131
	testHspgwResponse Cisco-Account-Info roaming@hspgw-23
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1681,112,fritsche,41,0
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='fritsche'" fritsche
	testPMSRequest '10.10.10.75.*PR|.*|PI112|'
	testPMSRequest '10.10.10.78.*PR|.*|PI112|'
	
	initTestCase 131b "Accounting - expect entry billed"
	runHspgw acct 41 1682 112 fritsche 131
	testPMSRequest '10.10.10.75.*PR|.*|RN112|.*|TA200|' 
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 132a "Authentication with lookup in ocuupancy - no PMS request"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'	
	runHspgw auth 41 1682 112 fritsche 132
	testNoPMSRequest   # PMS is not requested
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1681,112,fritsche,41,0
	testLog 'Authentication by occupancy table'

	initTestCase 133a "Authentication with lookup in HAT - no PMS request"
	runHspgw auth 41 1682 112 fritsche 133
	testNoPMSRequest   # PMS is not requested
	testLog 'authentication in ACCOUNT_TRACKER table'
	
}

scen14 () {
	initTestScenario 14  "Per-session tarif"

	initTestCase 141a "Authentication - expect entry with zoneid 1680, tariff 43"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 43 1680 112 fritsche 141
	testLog 'Password verified by PMS'
	testHspgwResponse Cisco-Account-Info roaming@hspgw-22	
	testPMSRequest '10.10.10.75.*PR|.*|PI112|'
	
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1680,112,fritsche,43,0
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='fritsche'" fritsche

	initTestCase 141b "Accounting - expect entry billed"
	runHspgw acct 43 1680 112 fritsche 141
	testLog 'Treat it as billed'
	testNoPMSRequest   # PMS is not requested - free tariff
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 142a "Authentication with lookup in ocuupancy - no PMS request"
	runHspgw auth 43 1680 112 fritsche 142
	testLog 'Authentication by occupancy table'
	testNoPMSRequest   # PMS is not requested
	
}

scen15 () {
	initTestScenario 15 "Per-time tariff, access after checkout"

	initTestCase 151a "Initialization: Create an entry in HSPGW_HSP_OCCUPANCY"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 40 1680 112 fritsche 151
	testLog 'Password verified by PMS'
	runDbQuery 'select HHO_GN from HSPGW_HSP_OCCUPANCY' fritsche  # make sure the db entry is there

	initTestCase 152a "Authentication by occupancy, deadline not expired"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_CO_DATE=$(getDbTime)-10000"  # force a checkout in the past
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+20000"  # force a deadline in the future
	runHspgw auth 40 1680 112 fritsche 152
	testLog 'Authentication by occupancy table'

	initTestCase 153a "Authentication by occupancy, deadline expired"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+1000"  # force a deadline in the past
	runDbQuery 'select count(*) from HSPGW_OCCUPANCY_VIEW' 0 # occupancy not here anymore
	runHspgw auth 40 1680 112 fritsche 153
	testLog 'Password verified by PMS'
	
}

scen16 () {
	initTestScenario 16 "Per-session tariff, access after checkout"

	initTestCase 161a "Initialization: Create an entry in HSPGW_HSP_OCCUPANCY"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 43 1680 112 fritsche 161
	testLog 'Password verified by PMS'
	runDbQuery 'select HHO_GN from HSPGW_HSP_OCCUPANCY' fritsche  # make sure the db entry is there

	initTestCase 162a "Authentication by occupancy, deadline not expired"
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_CO_DATE=$(getDbTime)-10000"  # force a checkout in the past
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+20000"  # force a deadline in the future
	runHspgw auth 43 1680 112 fritsche 161
	testLog 'Authentication by occupancy table'

	initTestCase 163a "Authentication - occupancy, deadline expired"
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+1000"  # force a deadline in the past
	runDbQuery 'select count(*) from HSPGW_OCCUPANCY_VIEW' 0 # occupancy not here anymore
	runHspgw auth 43 1680 112 fritsche 161
	testLog 'Password verified by PMS'
	
}
