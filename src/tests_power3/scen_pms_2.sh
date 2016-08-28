#!/bin/bash
# -------------------------------------------------------------------------
# Testscenarios for PMS 2 (Hogatex)
# -------------------------------------------------------------------------

_DEBUG=off
#_DEBUG=on

. $(dirname $0)/utils.sh

# do a dummy transaction to wake up system
dummy2 () {
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 50 1690 105 abati 201
}

scen21 () {
	initTestScenario 21 "Per-time tariff, no grouping"
	
	initTestCase 211a "Authentication with PMS request"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 50 1690 105 abati 211
	testHspgwResponse Cisco-Account-Info roaming@hspgw-27
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1690,105,abati,50,0
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='abati'" abati
	testPMSRequest '10.10.10.73.*PR|.*|PI105|'
	
	initTestCase 211b "Accounting - expect entry billed"
	runHspgw acct 50 1690 105 abati 211
	testPMSRequest '10.10.10.73.*PR|.*|RN105|.*|TA100|'
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 212a "Authentication by ocuupancy"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'	
	runHspgw auth 50 1690 105 abati 212
	testNoPMSRequest   # PMS is not requested
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1690,105,abati,50,0
	testLog 'Authentication by occupancy table'

	initTestCase 213a "Authentication by tracker"
	runHspgw auth 50 1690 105 abati 213
	testNoPMSRequest   # PMS is not requested
	testLog 'authentication in ACCOUNT_TRACKER table'

}

scen22 () {
	initTestScenario 22 "TODO"
}

scen23 () {
	initTestScenario 23 "TODO"
}

scen24 () {
	initTestScenario 24  "Per-session tarif"

	initTestCase 241a "Authentication - expect entry with zoneid 1680, tariff 43"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 53 1690 105 abati 241
	testLog 'Password verified by PMS'
	testHspgwResponse Cisco-Account-Info roaming@hspgw-28	
	testPMSRequest '10.10.10.73.*PR|.*|PI105|'
	
	runDbQuery 'select HAT_TRACKER_ZONEID,HAT_TRACKER_USR, HAT_TRACKER_PWD, HTC_TARIFF_ID, HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1690,105,abati,53,0
	runDbQuery "select HHO_GN from HSPGW_HSP_OCCUPANCY where HHO_GN='abati'" abati

	initTestCase 241b "Accounting - expect entry billed"
	runHspgw acct 53 1690 105 abati 241
	testLog 'Treat it as billed'
	testNoPMSRequest   # PMS is not requested - free tariff
	runDbQuery 'select HAT_TRACKER_BILLED from HSPGW_ACCOUNT_TRACKER' 1

	initTestCase 242a "Authentication with lookup in ocuupancy - no PMS request"
	runHspgw auth 53 1690 105 abati 242
	testLog 'Authentication by occupancy table'
	testNoPMSRequest   # PMS is not requested
	
}

scen25 () {
	initTestScenario 25 "Per-time tariff, access after checkout"

	initTestCase 251a "Initialization: Create an entry in HSPGW_HSP_OCCUPANCY"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 50 1690 105 abati 251
	testLog 'Password verified by PMS'
	runDbQuery 'select HHO_GN from HSPGW_HSP_OCCUPANCY' abati  # make sure the db entry is there

	initTestCase 252a "Authentication by occupancy, deadline not expired"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_CO_DATE=$(getDbTime)-10000"  # force a checkout in the past
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+20000"  # force a deadline in the future
	runHspgw auth 50 1690 105 abati 252
	testLog 'Authentication by occupancy table'

	initTestCase 253a "Authentication by occupancy, deadline expired"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+1000"  # force a deadline in the past
	runDbQuery 'select count(*) from HSPGW_OCCUPANCY_VIEW' 0 # occupancy not here anymore
	runHspgw auth 50 1690 105 abati 253
	testLog 'Password verified by PMS'
	
}

scen26 () {
	initTestScenario 26 "Per-session tariff, access after checkout"

	initTestCase 261a "Initialization: Create an entry in HSPGW_HSP_OCCUPANCY"
	runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
	runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'
	runHspgw auth 53 1690 105 abati 261
	testLog 'Password verified by PMS'
	runDbQuery 'select HHO_GN from HSPGW_HSP_OCCUPANCY' abati  # make sure the db entry is there

	initTestCase 262a "Authentication by occupancy, deadline not expired"
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_CO_DATE=$(getDbTime)-10000"  # force a checkout in the past
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+20000"  # force a deadline in the future
	runHspgw auth 53 1690 105 abati 261
	testLog 'Authentication by occupancy table'

	initTestCase 263a "Authentication - occupancy, deadline expired"
	runDbQuery "update HSPGW_HSP_OCCUPANCY set HHO_ACC_DEADLINE=HHO_CO_DATE+1000"  # force a deadline in the past
	runDbQuery 'select count(*) from HSPGW_OCCUPANCY_VIEW' 0 # occupancy not here anymore
	runHspgw auth 53 1690 105 abati 261
	testLog 'Password verified by PMS'
	
}
