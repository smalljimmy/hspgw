#!/bin/bash
# utility to copy module and headerfile from pms2 - eases migration
if [ ! -d "../rlm_pms_2" ]; then 
	echo "wrong directory"
	exit 0
fi	

sed -e 's/pms_2/pms_1/gi' -e 's/PMS 2/PMS 1/gi'  ../rlm_pms_2/rlm_pms_2.c >rlm_pms_1_delta.c
sed -e 's/pms_2/pms_1/gi' -e 's/PMS 2/PMS 1/gi'  ../rlm_pms_2/rlm_pms_2.h >rlm_pms_1_delta.h
	

	
