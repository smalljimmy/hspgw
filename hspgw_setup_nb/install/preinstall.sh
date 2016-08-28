#!/bin/sh

	LIBFOLDER="/opt/hspgw/lib"
	SQLRLIBS="/opt/firstworks/lib"
	CONFIGFOLDER="config/"
	SLIB="lib"
	SSRC="src"
	SUTILS="utils"
	FILEFOLDER="/opt"
	BASEFOLDER="./../"
	UTILFOLDER="/opt/hspgw/utils"
	
	preCheck()
	{
		#unzip installation .tar.gz file
		#check operating system version

		#check if the software existed
		if [ -d $FILEFOLDER/hspgw ];
		then
			cp -r $FILEFOLDER/hspgw /tmp
			rm -r $FILEFOLDER/hspgw
		fi

		if [ -d $FILEFOLDER/firstworks ];
		then
			cp -r $FILEFOLDER/firstworks /tmp
			rm -r $FILEFOLDER/firstworks
		fi
		
		
		if [ -d $LIBFOLDER ];
		then
			echo "continue..." > /dev/null 2>&1
		else   
			mkdir -p $LIBFOLDER
		fi

		if [ -d $SQLRLIBS ];
		then
			echo "continue..." > /dev/null 2>&1
		else   
			mkdir -p $SQLRLIBS
		fi


# only linux:
        if [ ! -f /etc/ld.so.conf.d/hspgw.conf ]
		then
			cp $BASEFOLDER$CONFIGFOLDER/hspgw.conf /etc/ld.so.conf.d/
		fi

#only solaris:
#		crle -c /var/ld/ld.config -l /lib:/usr/lib:/usr/local/lib:/opt/hspgw/lib:/opt/firstworks/lib:/opt/oracle/instantclient_11_1

	}

	unzipHspgw()
	{
		#unzip compiled files to /opt/hspgw
		#pwd
		#tar xvf 
		cp -r $BASEFOLDER$SSRC/hspgw $FILEFOLDER
	}

    unzipSqlRelay()
	{
		cp -r $BASEFOLDER$SSRC/firstworks $FILEFOLDER
	}

    installUtils()
	{
		mkdir -p $UTILFOLDER
		rm -f $UTILFOLDER/*
		cp -f $BASEFOLDER$SUTILS/* $UTILFOLDER/
		chmod +x $UTILFOLDER/*sh $UTILFOLDER/*pl
	}
	
echo "processing pre-installtion..."

#check libraries required by installation
preCheck

#unzip src to /opt/hspgw
unzipHspgw

#unzip sqlrelay libraries to /opt/hspgw/lib
unzipSqlRelay

#install utility script files to /opt/hspgw/utils
installUtils

echo "pre-installtion done."
exit 0   
