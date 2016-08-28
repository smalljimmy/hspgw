#!/bin/sh
if [ -z "$4" ]; then
	echo Usage: $0 zone_id tariff_id room_no user_name
	exit
fi

RETRIES=3
TIMEOUT=6
SESSION_ID=$$

zone_id=$1
tariff_id=$2
room_no=$3
user_name=$4
infile=test_hspgw_${zone_id}_${tariff_id}_${room_no}_${user_name/ /_}.in
cat > $infile <<END
WISPr-Location-Name="SCS:CH,${zone_id}"
Acct-Session-Id=AS${SESSION_ID}
User-Name="${room_no}#${tariff_id}@hspgw"
User-Password="${user_name}"
END

cat $infile
echo Authentification...
/opt/hspgw/bin/radclient -d /opt/hspgw/etc/raddb -c 1 -f $infile -r $RETRIES -t $TIMEOUT localhost auth testing123
echo Accounting...
/opt/hspgw/bin/radclient -d /opt/hspgw/etc/raddb -c 1 -f $infile -r $RETRIES -t $TIMEOUT localhost acct testing123

