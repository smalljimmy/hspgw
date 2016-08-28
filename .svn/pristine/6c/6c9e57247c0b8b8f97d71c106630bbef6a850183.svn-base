#!/bin/bash
if [ -z "$4" ]; then
        echo Usage: $0 zone_id tariff_id room_no user_name AutoLogin [WebProxy Calling-Station-Id Framed-IP-Address]
        exit
fi

RETRIES=3
TIMEOUT=6
SESSION_ID=$$

zone_id=$1
tariff_id=$2
room_no=$3
user_name=$4 #; echo $user_name ; echo ${user_name// /_}
auto_login=${5:-'0'} ; echo $auto_login
web_proxy=${6:-'0'}
cst_id=$7
fia=$8
user_name_u="${user_name// /_}"
infile=test_hspgw_${zone_id}_${tariff_id}_${room_no}_${user_name_u}.in
cat > $infile <<END
WISPr-Location-Name="SCS:CH,${zone_id}"
Acct-Session-Id=AS${SESSION_ID}
User-Name="${room_no}#${tariff_id}@hspgw"
User-Password=${user_name}
Cisco-Account-Info="key=value1,key2=value2,WebProxy=${web_proxy},AutoLogin=${auto_login}"
END

if [ -n "$7" ]; then
cat >> $infile <<END
Calling-Station-Id=${cst_id}
END
fi

if [ -n "$8" ]; then
cat >> $infile <<END
Framed-IP-Address=${fia}
END
fi


cat $infile
echo Authentification...
/opt/hspgw/bin/radclient -d /opt/hspgw/etc/raddb -c 1 -f $infile -r $RETRIES -t $TIMEOUT localhost auth testing123
echo Accounting...
/opt/hspgw/bin/radclient -d /opt/hspgw/etc/raddb -c 1 -f $infile -r $RETRIES -t $TIMEOUT localhost acct testing123
