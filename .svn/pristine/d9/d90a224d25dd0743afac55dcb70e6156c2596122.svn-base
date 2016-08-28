#!/bin/sh

if [ -f /opt/hspgw/var/run/radiusd/radiusd.sock ];
then
   rm /opt/hspgw/var/run/radiusd/radiusd.sock
fi

rm -r ./src
mkdir ./src
mkdir -p ./src/firstworks
cp -r /opt/hspgw ./src/hspgw

# tar cf ./src/sqlrelay/sqlrlibs.tar /opt/firstworks/lib
cp -r /opt/firstworks/lib ./src/firstworks

if [ -f ./src/hspgw/var/log/radius/radius.log ];
then
   rm ./src/hspgw/var/log/radius/radius.log
fi

