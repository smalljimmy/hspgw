#!/bin/sh
CONFIGPATH="/opt/hspgw/etc/raddb/"
TMPNM="modules"
TMPNM1="site-available"
BASEFOLDER="./../"

#determine enviroment by argument

CONFIGFOLDER="prod"

if [ $1 = "s" ];then
  CONFIGFOLDER="staging"
fi
if [ $1 = "t" ];then
  CONFIGFOLDER="test"
fi
echo "processing post installation for $CONFIGFOLDER environment..."


#change to configuration directory based on enviroment
TMPNMC="config/"
TMPNM2=$BASEFOLDER$TMPNMC$CONFIGFOLDER

#copy config files to specified folders
if [ -d $CONFIGPATH$TMPNM ];
  then
	echo "continue..." > /dev/null 2>&1
  else 
	mkdir -p $CONFIGPATH$TMPNM
fi

# pms and pms_1 are environment independent now
# sqlrelay configuration is in sql-conf
cp $TMPNM2/sql.conf $CONFIGPATH
chown root:root $CONFIGPATH/sql.conf
chmod 0640 $CONFIGPATH/sql.conf


if [ -d $CONFIGPATH ];
  then
	echo "continue..." > /dev/null 2>&1
  else 
	mkdir -p $CONFIGPATH
fi

cp $TMPNM2/clients.conf $CONFIGPATH

# nur linux
ldconfig

#solaris
#crle -c /var/ld/ld.config -l /lib:/usr/lib:/usr/local/lib:/opt/hspgw/lib:/opt/firstworks/lib:/opt/oracle/instantclient_11_1

echo "post installation done."
exit 0

