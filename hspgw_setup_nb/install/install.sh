#!/bin/sh

usage()
{
	echo " "
	echo "Usage:"
	echo "------"
	echo "./install.sh [t|s|p]"
	echo "t: test environment"
	echo "s: staging environment"
	echo "p: production environment"
	echo "use lower case characters"
}

# nur linux:
OSN="Linux"
VER="SMP"
uname -s | grep "$OSN" > /dev/null 2>&1
OSC=$?
uname -r | grep "$SMP" > /dev/null 2>&1
VERC=$?

#nur solaris
#OSN="SunOS"
#VER="5.10"
#uname -s | grep "SunOS" > /dev/null 2>&1
#OSC=$?
#uname -r | grep "5.10" > /dev/null 2>&1
#VERC=$?
# solaris ande

if [ $OSC -ne 0 ];
then
	echo "Error: Wrong operating system, should be $OSN. HSPGW cannot be installed."
	exit 1
fi
if [ $VERC -ne 0 ];
then
	echo "Warning: Wrong os version, should be $OSN $VER. HSPGW cannot be installed."
	#exit 1
fi

if [ $# -ne 1 ]; then 
	usage
	exit 1
fi

if [ $1 = "t" ] || [ $1 = "s" ] || [ $1 = "p" ];	
then
	sh ./preinstall.sh $*
	sh ./postinstall.sh $*
else
	usage
	exit 1
fi

