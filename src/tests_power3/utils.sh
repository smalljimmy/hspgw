#!/bin/bash
_DEBUG=off
#_DEBUG=on

cachedir=/tmp/hspgw_test/cache
mkdir -p $cachedir
rm -f $cachedir/*
#echo cache dir is $cachedir.

SQLR_ID=hspgw
mySecret=testing123
RETRIES=10
TIMEOUT=5
radlog_orig=/opt/hspgw/var/log/radius/radius.log
radlog=$cachedir/radius.log

function DEBUG()
{
 [ "$_DEBUG" == "on" ] &&  $@
}

# runs a dbquery using the SQLRelay and optionally tests against an expected result
# Usage:
#	dbquery 'querystring' [ expected result ]
# Example:
#	dbquery 'select HAT_TRACKER_ZONEID from HSPGW_ACCOUNT_TRACKER' '"1681"'
# 
function runDbQuery (){
	#echo dbquery $*
	query=$1
	DEBUG set -o xtrace
	sqlr-query -id $SQLR_ID -query "$query" | sed 's/"//g' >$cachedir/query.out
	DEBUG set +o xtrace
	#echo $query "==>" >$cachedir/query.result
	#cat $cachedir/query.out >>$cachedir/query.result
	if [ -n "$2" ]; then
		rm -f $cachedir/query.expected
		shift
		IFSSave=$IFS
		#IFS=''
		#echo $*
		for line in $*; do
			echo $line | sed 's/"//g' >>$cachedir/query.expected
		done		
		#IFS=$IFSSave
		diff -w -W 160 -y $cachedir/query.expected $cachedir/query.out >$cachedir/query.diff
		RCODE=$?
		[ $RCODE = 0 ] || (echo $query "\n"; echo "DIFFERENT: expected / actual:" ; cat $cachedir/query.diff; failed dbquery $query $*)
		[ $RCODE = 0 ] && (passed)
	else
		cat $cachedir/query.out
	fi
}

# runs the Radius test client with HSPGW specific parameters and tests the "Cisco account Info"
# Usage:
# 	runHspgw type tariffId zoneID roomNO userName sessionID [caiExpected]
# Example:
# 	runHspgw auth 40 1682 112 fritsche A0000043
function runHspgw () {
	DEBUG echo runhspgw $*
	typeset radcommand=$1
	typeset tariffID=$2
	typeset zoneID=$3
	typeset roomNO=$4
	typeset userName=$5
	typeset sessionID=$6
	typeset caiExpected=$7
	cat >$cachedir/test.in  <<END
WISPr-Location-Name="SCS:CH,$zoneID"
Acct-Session-Id=$sessionID
User-Name="${roomNO}#${tariffID}@hspgw"
User-Password=$userName
END
	
	DEBUG set -o xtrace
	#startHSPGW
	
	lc1=$(cat $radlog_orig | wc -l)
	 
	/opt/hspgw/bin/radclient -f $cachedir/test.in -d /opt/hspgw/etc/raddb -c 1 -r $RETRIES -t $TIMEOUT localhost $radcommand testing123 > $cachedir/test.out
	DEBUG set +o xtrace
	#DEBUG cat $cachedir/test.out
	
	RCODE=1
	#set -o xtrace
	for i in {1..60}; do
		sleep 1
		lc2=$(cat $radlog_orig | wc -l)
		lcLast=$(($lc2 - $lc1))
		touch $radlog
		tail -n $lcLast $radlog_orig >$radlog
		grep 'Cleaning up request' $radlog >/dev/null
		if [ "$?" -eq "0" ]; then
			RCODE=0; break 
		fi
		DEBUG echo runHspgw: Waiting $i...
	done
	#set +o xtrace
	[ $RCODE = 0 ] || echo HSPGW failed to finish.	

	if [ -n "$caiExpected" ]; then
		testHSPGWResponse "$caiExpected"
	fi
}

# tests last response
function testHspgwResponse () {
	#DEBUG set -o xtrace
	typeset caiExpectedKey=$1
	typeset caiExpectedValue=$2

	RCODE=0
	#set -o xtrace
	grep $caiExpectedKey $cachedir/test.out | grep "\"$caiExpectedValue\"" >>/dev/null
	RCODE=$?
	#set +o xtrace
	
	[ $RCODE = 0 ] || (echo FAILED testHSPGWResponse ; cat $cachedir/test.in; cat $cachedir/test.out; failed testHSPGWResponse $* )
	[ $RCODE = 0 ] && (passed)
	#DEBUG set +o xtrace
}

# tests if a PMS Request has been sent
# Usage: testPMSRequest 'PR|P#2|RN112|GNFritsche'
function testPMSRequest () {
	if [ -n "$1" ]; then
		typeset expected=$1
		grep SC_HSPGW_PMS_SND_MSG $radlog | grep "$1" >/dev/null
		RCODE=$?
		[ $RCODE = 0 ] || (echo FAILED testPMSRequest $* ; failed testPMSRequest $* )
		[ $RCODE = 0 ] && (passed)
	else
		grep SC_HSPGW_PMS_SND_MSG $radlog
	fi
}

# tests if NO PMS Request has been sent
function testNoPMSRequest () {
	grep -v SC_HSPGW_PMS_SND_MSG $radlog >/dev/null
	RCODE=$?
	[ $RCODE = 0 ] || (echo FAILED testNoPMSRequest; failed testNoPMSRequest )
	[ $RCODE = 0 ] && (passed)
}


# tests if a Debug message has been sent.
# Usage: testLog ????
function testLog () {
	#set -o xtrace
	if [ -n "$1" ]; then
		typeset expected=$1
		grep -e Info -e Debug $radlog | grep "$1" >/dev/null
		RCODE=$?
		[ $RCODE = 0 ] || (echo FAILED testLog $* ; failed testLog $* )
		[ $RCODE = 0 ] && (passed)
	else
		grep -e Info -e Debug $radlog
	fi
	#set +o xtrace
}



# initializes test statistics
function initTestStatistics () {
	rm -f $cachedir/passed; touch $cachedir/passed
	rm -f $cachedir/failed; touch $cachedir/failed
}

# marks a test as passed
function passed () {
	echo "$testCaseId PASSED">>$cachedir/passed
}

# marks a test as failed
function failed () {
	echo "$testCaseId FAILED - $*">>$cachedir/failed
}

# shows test statistics
function showTestStatistics () {
	total=$(cat $cachedir/passed $cachedir/failed | wc -l)
	passed=$(cat $cachedir/passed | wc -l)
	failed=$(cat $cachedir/failed | wc -l)
	cat <<END
Summary
Tests run      : $total
Tests succeded : $passed
Tests failed   : $failed
	
END

	if [ "$failed" == "0" ]; then
		echo "All tests PASSED"
		rc=0
	else
		echo "Failed tests:"
		cat $cachedir/failed | sort -u 
		rc=1
	fi
}

# sets test scenario
function initTestScenario () {
	testScenarioId=$1
	testScenarioDescription=$2
	echo ============================================================
	echo == Test scenario: $testScenarioId
	if [ -n "$testScenarioDescription" ]; then
		echo == Description: $testScenarioDescription
	fi
	echo ============================================================
}

# sets test case
function initTestCase () {
	testCaseId=$1
	testCaseDescription=$2
	echo ------- Test case $testCaseId - $testCaseDescription
}

# get current time as epoch time
function getTime () {
	date '+%s'
}

# gets current database time as epoch time
function getDbTime () {
	runDbQuery "select cast(to_number(sysdate - to_date('01-01-1970','DD-mm-YYYY'))*24*60*60 as int) from dual"
}

# kills HSPGW
function killHSPGW () 
{ 
	#type rc.radiusd || (echo rc.radiusd missing; exit 1)
	rc.radiusd stop >/dev/null 2>&1
	for i in {1..20}; do
		sleep 1
		if [ -z "$(ps -ef | grep radiusd | grep -v grep |  awk '{print $2}')" ];then
			break
		fi
		echo Waiting $i...
	done

	sleep 1
    for pid in $(ps -ef | grep radiusd | grep -v grep |  awk '{print $2}');
    do
        DEBUG echo Brutally Killing radiusd, pid=$pid ...;
        kill -9 $pid >/dev/null 2>&1
    done
	sleep 1
}

# starts HSPGW
function startHSPGW () {
	killHSPGW
	sleep 1
	# rc.radiusd start
	rm -f $radlog_orig
	radiusd -xxx &
	RCODE=1
	for i in {1..10}; do
		sleep 1
		grep 'Ready to process requests' $radlog_orig  >/dev/null
		if [ "$?" -eq "0" ]; then
			RCODE=0; break 
		fi
		echo Waiting $i...
	done
	[ $RCODE = 0 ] || echo HSPGW failed to start.
	[ $RCODE = 0 ] && echo HSPGW started.	
}

# kills SQLRelay
function killSQLRelay () 
{ 
	type sqlr-stop || (echo sqlr-stop missing; exit 1)
	sqlr-stop
	sleep 1	
    for pid in $(ps ax | grep sqlr- | grep -v grep | awk '{print $2}');
    do
        echo Brutally Killing sqlr, pid=$pid ...;
        kill -9 $pid;
    done
}

# starts SQLRelay
function startSQLRelay () {
	sqlr-status -id hspgw || sqlr-start -id hspgw # only start if needed
	echo SQLRelay started.
}

# starts SQLRelay - experimental
function startSQLRelayForced () {
	#set -o xtrace

	sqlr-stop
	for i in {1..60}; do
		sleep 1
		killall -9 -r sqlr-.*
		pids=$(ps -Ao '%U %p %c' | grep sqlr- | grep -v grep | awk '{print $2}')
		[ -z "$pids" ] && break;
		echo "Wait for completion of sqlr-stop ..."
	done

	tmpfile=/tmp/sqlr-start-$$.log
	
	for i in {1..60}; do
		sqlr-start -id hspgw >$tmpfile 2>&1
		grep -v 'failed' $tmpfile && (echo SQLRelay started; break)  # everything OK
		sqlr-stop
		sleep 1
		killall -9 -r sqlr-.*
		#sqlr-query -id hspgw -query 'select 1-1 from dual'		
		cat $tmpfile
		echo "SQL Relay failed to start ..."

		id=$(grep 'id [0-9]' $tmpfile | tr -cd "[:digit:]") # get id out of error message
		if [ -n "$id" ]; then
			echo "Try to remove semaphore or shared memory id: /$id/"
			ipcrm -s $id >/dev/null 2>&1
			ipcrm -m $id >/dev/null 2>&1	
		fi
		pidfile=$(grep 'The pid file .* exists.' $tmpfile | sed 's/The pid file \(.*\) exists./\1/')
		if [ -n "$pidfile" ]; then
			echo "Try to remove pidfile: /$pidfile/"
			rm $pidfile
		fi		
		
		echo "Retry start SQL Relay ..."
		sleep 1
	done
	echo Done.
}


# starts / restarts ALL
function startAll () {
#	killHSPGW
#	killSQLRelay
	startSQLRelay 		
	startHSPGW	
}

# expeimental ...
function statusAll () {
	statusSqlRelay=OK
	sqlr-status -id hspgw || statusSqlRelay=MISSING 
}