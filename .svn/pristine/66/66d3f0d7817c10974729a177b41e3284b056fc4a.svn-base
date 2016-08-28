# start HSPGW
cd ../..
sqlr-stop
sqlr-start -id hspgw

killall -9 radiusd ; rm /opt/hspgw/var/log/radius/radius.log
radiusd -xxx -d raddb/
tail -f /opt/hspgw/var/log/radius/radius.log
cd -

. ./utils.sh
# =========================================================================
# PMS 1 
# =========================================================================
runDbQuery 'select distinct PROTOCOL_ID from HSPGW_CONFIG_VIEW WHERE ZONE_ID=1682' 1

# -------------------------------------------------------
# Free tariff
runDbQuery 'update HSPGW_TARIFF set HTC_AMOUNT_PER_UNIT =0 WHERE HTC_TARIFF_ID=42'
runDbQuery 'select HTC_AMOUNT_PER_UNIT, HGT_TARIFF_TYPE_ID from HSPGW_TARIFF WHERE HTC_TARIFF_ID=42' 0,2

# by PMS, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1682 42 309 jucker 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1  #  have OCC-RL
runDbQuery 'select COUNT(*) from HSPGW_HSP_OCCUPANCY' 1
runDbQuery 'select HAR_WEBPROXY from HSPGW_ACCTR_RELOGIN' 12345
runDbQuery 'select HOR_WEBPROXY from HSPGW_OCCUPANCY_RELOGIN' 12345

# by OCC, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1682 42 309 jucker 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# by PMS, No AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1682 42 309 jucker 0

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 0
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# by OCC, No AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1682 42 309 jucker 0

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 0
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# No AutoLogin - error message
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

killall -9 radiusd ; rm /opt/hspgw/var/log/radius/radius.log

radiusd -xxx -d raddb/
../../test_hspgw.sh 1682 42 309 jucker 1 12345

grep 'Re-login cannot be initialized.' /opt/hspgw/var/log/radius/radius.log  # should issue 2 warnings

../../test_hspgw.sh 1682 42 309 jucker 0

grep 'Re-login cannot be initialized.' /opt/hspgw/var/log/radius/radius.log  # should issue 1 warnings

# -------------------------------------------------------
# Paid tariff
runDbQuery 'update HSPGW_TARIFF set HTC_AMOUNT_PER_UNIT =300, HGT_TARIFF_TYPE_ID=2 WHERE HTC_TARIFF_ID=42'
runDbQuery 'select HTC_AMOUNT_PER_UNIT, HGT_TARIFF_TYPE_ID from HSPGW_TARIFF WHERE HTC_TARIFF_ID=42' 300,2


# by PMS, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1682 42 309 jucker 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0   #  no  OCC-RL 
runDbQuery 'select COUNT(*) from HSPGW_HSP_OCCUPANCY' 1

# by HAT, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1682 42 309 jucker 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0  # no OCC-RL
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# by OCC, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1682 42 309 jucker 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0   #  no OCC-RL 
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# by PMS, No AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1682 42 309 jucker 0

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 0
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0 
runDbQuery 'select COUNT(*) from HSPGW_HSP_OCCUPANCY' 1

# by HAT, No AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1682 42 309 jucker 0

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 0
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0  # no OCC-RL
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1


# =========================================================================
# PMS 2
# =========================================================================
runDbQuery 'select distinct PROTOCOL_ID from HSPGW_CONFIG_VIEW WHERE ZONE_ID=1692' 2

# Free tariff
runDbQuery 'update HSPGW_TARIFF set HTC_AMOUNT_PER_UNIT =0 WHERE HTC_TARIFF_ID=52'
runDbQuery 'select HTC_AMOUNT_PER_UNIT, HGT_TARIFF_TYPE_ID from HSPGW_TARIFF WHERE HTC_TARIFF_ID=52' 0,2

# by PMS, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1692 52 105 abati 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1  #  have OCC-RL
runDbQuery 'select COUNT(*) from HSPGW_HSP_OCCUPANCY' 1

# by OCC, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1692 52 105 abati 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 1
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# by PMS, No AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1692 52 105 abati 0

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 0
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

# No AutoLogin - error message
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1692 52 105 abati 1

grep 'Re-login cannot be initialized.' /opt/hspgw/var/log/radius/radius.log  # should issue 2 messages "Missing ..."

../../test_hspgw.sh 1692 52 105 abati 0

grep 'Re-login cannot be initialized.' /opt/hspgw/var/log/radius/radius.log  # should issue 1 message "Missing ..."

# -------------------------------------------------------
# Paid tariff
runDbQuery 'update HSPGW_TARIFF set HTC_AMOUNT_PER_UNIT =300 WHERE HTC_TARIFF_ID=52'
runDbQuery 'select HTC_AMOUNT_PER_UNIT, HGT_TARIFF_TYPE_ID from HSPGW_TARIFF WHERE HTC_TARIFF_ID=52' 300,2

# by PMS, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'delete from HSPGW_ACCOUNT_TRACKER'											
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'delete from HSPGW_HSP_OCCUPANCY'											

../../test_hspgw.sh 1692 52 105 abati 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0 #  no OCC-RL 
runDbQuery 'select COUNT(*) from HSPGW_HSP_OCCUPANCY' 1

# by HAT, AutoLogin
runDbQuery 'delete from HSPGW_ACCTR_RELOGIN'
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0
runDbQuery 'delete from HSPGW_OCCUPANCY_RELOGIN'											
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

../../test_hspgw.sh 1692 52 105 abati 1 12345 a.b.c 1.1.1.1

runDbQuery 'select COUNT(*) from HSPGW_ACCTR_RELOGIN' 1
runDbQuery 'select COUNT(*) from HSPGW_ACCOUNT_TRACKER' 1
runDbQuery 'select COUNT(*) from HSPGW_OCCUPANCY_RELOGIN' 0  # no OCC-RL
runDbQuery 'select COUNT(*)  from HSPGW_HSP_OCCUPANCY' 1

