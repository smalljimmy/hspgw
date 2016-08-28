#!/bin/bash
_DEBUG=off
#_DEBUG=on

. $(dirname $0)/utils.sh

# check initial data
function scen_testInitialData () {

	initTestScenario TestInitialData

	# Bandwidth profiles
	runDbQuery 'select  count(*) from spp_bandwidth_tuples where sbt_profile_id in (21,22,23,24,25,26,27,28)' 8

	# Zones
	runDbQuery 'select count(*) from spp_zones where szo_zone_id in (1680,1681,1682,1690,1691,1692)' 6
	
	# Zone-Logintype
	runDbQuery 'select szl_zone_login_type_id, szo_zone_id, slt_login_type_id from spp_zone_logintype where szl_zone_login_type_id in (80,81,82,90,91,92) order by 1' \
80,1680,10 \
81,1681,10 \
82,1682,10 \
90,1690,10 \
91,1691,10 \
92,1692,10

	# Configuration	
	runDbQuery 'select hco_configuration_id, szl_zone_login_type_id, hco_ip_address, hco_port, hgp_group_id from hspgw_configuration where hco_configuration_id in (60,61,62,63,70,71,72,73) order by 1' \
60,80,192.168.2.75,5020, \
61,81,192.168.2.75,5020,2 \
62,82,192.168.2.78,8100,2 \
70,90,192.168.2.73,5555, \
71,91,192.168.2.73,5555,3 \
72,92,192.168.2.77,5555,3

	# Tariff
	runDbQuery 'select htc_tariff_id, hgt_tariff_type_id, htc_amount_per_unit, htc_sess_timeout, hco_configuration_id, sbt_profile_id, hgp_group_id, htc_access_after_co from hspgw_tariff where htc_tariff_id in (40,41,42,43,50,51,52,53) order by 1' \
40,2,100,3600,60,21,,0 \
41,2,200,7200,61,23,2,0 \
42,2,300,86400,61,23,2,0 \
43,1,0,600,60,22,,36000 \
50,2,100,3600,70,27,,0 \
51,2,200,7200,71,28,3,0 \
52,2,300,86400,72,27,3,0 \
53,1,0,600,70,28,,0

	# Views
	runDbQuery 'select TARIFF_ID, AMOUNT_PER_UNIT, BW_PROFILE_ID, CONFIGURATION_ID, PROTOCOL_ID, IP_ADDRESS, PORT_NUMBER, HSPGW_GROUP_ID, ZONE_LOGIN_TYPE_ID, ZONE_ID from HSPGW_CONFIG_VIEW where CONFIGURATION_ID in (60,61,62,63,70,71,72,73) ORDER BY TARIFF_ID, CONFIGURATION_ID' \
40,100,21,60,1,192.168.2.75,5020,,80,1680 \
41,200,23,61,1,192.168.2.75,5020,2,81,1681 \
41,200,23,62,1,192.168.2.78,8100,2,82,1682 \
42,300,23,61,1,192.168.2.75,5020,2,81,1681 \
42,300,23,62,1,192.168.2.78,8100,2,82,1682 \
43,0,22,60,1,192.168.2.75,5020,,80,1680 \
50,100,27,70,2,192.168.2.73,5555,,90,1690 \
51,200,28,71,2,192.168.2.73,5555,3,91,1691 \
51,200,28,72,2,192.168.2.77,5555,3,92,1692 \
52,300,27,71,2,192.168.2.73,5555,3,91,1691 \
52,300,27,72,2,192.168.2.77,5555,3,92,1692 \
53,0,28,70,2,192.168.2.73,5555,,90,1690

}

scen_testInitialData
