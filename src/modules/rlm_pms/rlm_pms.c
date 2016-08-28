/*
 * rlm_pms.c
 *
 * Version:	$Id$
 *
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 *
 * Copyright 2000,2006  The FreeRADIUS server project
 */

#include <sys/resource.h>
#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/modpriv.h>
#include <rlm_sql.h>
#include "rlm_pms.h"

// Function to extract sub-attributes from Vendor WISPr
// VENDOR   WISPr   14122   ietf
#define WISPR2ATTR(x) ((14122 << 16) | (x))

static char *allowed_chars = NULL;

static void dbg_free(void *ptr)
{
	struct rusage usage;
	free (ptr);
	getrusage(RUSAGE_SELF, &usage);
	DEBUG2("dbg_free ru_maxrss=%ld ptr=%p", usage.ru_maxrss, ptr);
}

static void *dbg_malloc(size_t size, int line, char* file, char* func)
{
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	void *ptr = rad_malloc(size);
	DEBUG2("dbg_malloc ru_maxrss=%ld ptr=%p size=%d at %s:%s(%d)", usage.ru_maxrss, ptr, size, file, func, line);
	return ptr;
}

static void printMemUsage(char* func, int line) {
	struct rusage usage;
	getrusage(RUSAGE_SELF, &usage);
	DEBUG2("%s(%d): MemUsage: ru_maxrss=%ld", func,line, usage.ru_maxrss);
}

/*
 *	The instance data for rlm_pms is the list of fake values we are
 *	going to return.
 */

static int rlm_pms_detach(void *instance) {
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char mqs[MAX_QUERY_LEN];
	SQLSOCK *sqlsocket = NULL;
	int	now = time(NULL);

	// possibly not needed
	//	paircompare_unregister(PW_SQL_GROUP, sql_groupcmp);

	radlog(L_INFO,"SC_HSPGW_RDEAMON_SHUTDOWN : HSPGW exciting normally");
	//replace the wild cards
//radlog(L_INFO, "%s : %s, %u", inst->shutdownqry, inst->hostname, now );
	sprintf(mqs, inst->shutdownqry, inst->hostname, now);
//	radlog(L_INFO, "pms_gw_shutdown query: %s", mqs);

	// call the shutdown function on spp db
	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "%s : rlm_pms detach : Connection to data base failed.", "SC_HSPGW_SPPDB_CONN_ERR");
	}
	// set ECOT if necessary
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)){
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : rlm_pms detach : Cannot set online state of %s in SPP data base", inst->hostname);
	}
	// Free the DB handles
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);



	if (inst->config) {
		int i;

		if (inst->logevents) {
			pms_freeLogEventPool(inst);
		}

		/* try to avoid xlat, since i don't know what it is and how it works:
		 if (inst->config->xlat_name) {
		 xlat_unregister(inst->config->xlat_name,(RAD_XLAT_FUNC)sql_xlat);
		 free(inst->config->xlat_name);
		 }
		 */

		/*
		 *	Free up dynamically allocated string pointers.
		 */
		for (i = 0; module_config[i].name != NULL; i++) {
			char **p;
			if (module_config[i].type != PW_TYPE_STRING_PTR) {
				continue;
			}

			/*
			 *	Treat 'config' as an opaque array of bytes,
			 *	and take the offset into it.  There's a
			 *      (char*) pointer at that offset, and we want
			 *	to point to it.
			 */
			p = (char **) (((char *) inst->config) + module_config[i].offset);
			if (!*p) { /* nothing allocated */
				continue;
			}
			free(*p);
			*p = NULL;
		}
		/*
		 *	Catch multiple instances of the module.
		 */
		if (allowed_chars == inst->config->allowed_chars) {
			allowed_chars = NULL;
		}
		free(inst->config);
		inst->config = NULL;
	}

	if (inst->handle) {
#if 0
		/*
		 *	FIXME: Call the modules 'destroy' function?
		 */
		lt_dlclose(inst->handle); /* ignore any errors */
#endif
	}
	free(inst);

	return 0;
}

/*
 *	Do any per-module initialization that is separate to each
 *	configured instance of the module.  e.g. set up connections
 *	to external databases, read configuration files, set up
 *	dictionary entries, etc.
 *
 *	If configuration information is given in the config section
 *	that must be referenced in later calls, store a handle to it
 *	in *instance otherwise put a null pointer there.
 */

static int pms_instantiate(CONF_SECTION *conf, void **instance) {
	module_instance_t *modinst;
	rlm_pms_module_t * inst;

	DEBUG2("Entering PMS Instantiation ...");
	DEBUG2("============================");

	// Set up a storage area for instance data
	// rad_malloc() is a FreeRADIUS function and has its own error handling
	inst = rad_malloc(sizeof(*inst));
	if (!inst)
		return -1;
	memset(inst, 0, sizeof(*inst));
	// PMS Config
	inst->config = rad_malloc(sizeof(PMS_CONFIG));
	memset(inst->config, 0, sizeof(PMS_CONFIG));

	/*
	 *	If the configuration parameters can't be parsed, then fail.
	 */
	if (cf_section_parse(conf, inst->config, module_config) < 0) {
		rlm_pms_detach(inst);
		return -1;
	}

	if ((inst->config->sql_instance_name == NULL) || (strlen(inst->config->sql_instance_name) == 0)) {
		radlog(L_ERR, "%s : rlm_pms instantiation : The 'sql-instance-name' variable must be set.", "SC_HSPGW_CONFIG_ERR");
		rlm_pms_detach(inst);
		return -1;
	}

	modinst = find_module_instance(cf_section_find("modules"), inst->config->sql_instance_name, 1);
	if (!modinst) {
		radlog(L_ERR, "%s : rlm_pms instantiation : Failed to find sql instance named %s", "SC_HSPGW_CONFIG_ERR", inst->config->sql_instance_name);
		rlm_pms_detach(inst);
		return -1;
	}

	if (strcmp(modinst->entry->name, "rlm_sql") != 0) {
		radlog(L_ERR, "%s : rlm_pms instantiation : Module \"%s\" is not an instance of the rlm_sql module",
				"SC_HSPGW_CONFIG_ERR",
				inst->config->sql_instance_name);
		rlm_pms_detach(inst);
		return -1;
	}

	inst->sql_inst = (SQL_INST *) modinst->insthandle;

	// what ever...
	allowed_chars = inst->config->allowed_chars;

	// mark this gw as online in db

	// needed, since there are 2 instances of hspgw in pwlan env
	gethostname(inst->hostname, sizeof inst->hostname);

	//hack cz config not available in detach function:
	strcpy(inst->shutdownqry, inst->config->pms_gw_shutdown);

	//init some db related stuff
	if (pms_init(inst) < 0) {
		rlm_pms_detach(inst);
		return -1;
	}
	radlog(L_INFO,"SC_HSPGW_RDEAMON_STARTUP : HSPGW is starting up.");

	*instance = inst;
	return 0;
}

/*
 *      Authorization for PMS Module
 */

static int pms_authorize(void *instance, REQUEST *request) {
	VALUE_PAIR *vp;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	rlm_pms_module_t * inst = (rlm_pms_module_t *) instance;
	pms_logfile_data *logfiledata;
	int ret;
	int max_sessions;
	char *msgtxt;
	char strippeduser[MAX_STRING_LEN];
	char strippedtariffid[MAX_STRING_LEN];
	char zoneid[PMS_ZONEID_MAX_LEN + 1];
	char auth_type[PMS_AUT_ACC_TYPE_MAX_LEN];
	char querystr[MAX_QUERY_LEN];
	// Allocate memory for Logfile data
	logfiledata = rad_malloc(sizeof(pms_logfile_data));
	memset(logfiledata, '\0', sizeof(pms_logfile_data));

	DEBUG2("Entering PMS Authorization ...");
	DEBUG2("==============================");
	// initalize log data
	strcpy(logfiledata->sessid, "n/a");
	strcpy(logfiledata->tariffid, "n/a");
	strcpy(logfiledata->zoneid, "n/a");
	strcpy(logfiledata->username, "n/a");


	// Extract Acct-Session-Id attribute
	DEBUG2("rlm_pms::pms_authorize: Extract session ID from Acct-Session-Id attribute.");
	vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID);
	if (!vp){
		msgtxt = "Missing Acct-Session-Id attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, NULL);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}
	DEBUG2("rlm_pms::pms_authorize: Found attribute Acct-Session-Id attribute: '%s'.", vp->vp_strvalue);

	// Check  Acct-Session-Id length
	if (strlen(vp->vp_strvalue) > PMS_SESSIONID_MAX_LEN) {
		DEBUG2("rlm_pms::authorize: Invalid length of Acct-Session-Id attribute. Max. allowed length is %d.", PMS_SESSIONID_MAX_LEN);
		msgtxt = "Invalid length of Acct-Session-Id attribute. Authorization aborted.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, NULL);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// Acct-Session-Id is OK
	strcpy(logfiledata->sessid, vp->vp_strvalue);


	//added by Hj Lin on 1 July, 2011
/*
	vp = pairfind(request->packet->vps, PW_NAS_IP_ADDRESS);
	if (!vp){
		msgtxt = "Missing NAS-IP-Address attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	struct in_addr nasip;
	char nasipstr[16];
	nasip.s_addr = vp->vp_ipaddr;
	strncpy(nasipstr, inet_ntoa(nasip), sizeof(nasipstr) - 1);
	nasipstr[sizeof(nasipstr)] = 0;
*/

	/*
	 * Check zone ID
	 */
	// Extract SCS-SPP-ZoneID from Location-Name attribute PW_SCS_SPP_ZONEID PW_LOCATION_NAME
	// For example, from a Location-Name attribute:   SCS:CH,1234
	// the extracted PMS ZoneID will be:  1234
	DEBUG2("rlm_pms::pms_authorize: Extract PW_SCS_SPP_ZONEID from WISPr-Location-Name attribute.");
	vp = pairfind(request->packet->vps, WISPR2ATTR(PW_LOCATION_NAME));
	if (!vp) {
		msgtxt = "Missing WISPr-Location-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// Get the stripped SCS-SPP-ZoneID
	if (pms_strip_zoneid(inst, vp->vp_strvalue, zoneid) < 0) {
		msgtxt	= "Cannot parse SCS-SPP-ZoneID in WISPr-Location-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}
	DEBUG2("rlm_pms::pms_authorize: SCS-SPP-ZoneID: %s", zoneid);
	strcpy(logfiledata->zoneid, zoneid);

	radlog(L_INFO, "%s : req %u : sid %s : zid %s : tid n/a : usr n/a : Incoming Access-Request from client <%s>.",
				"SC_HSPGW_AUTH_INCOMING_REQ",
				request->number,
				logfiledata->sessid,
				logfiledata->zoneid,
				request->client->shortname);
	//			nasipstr);

	// Set the request attribute PW_SCS_SPP_ZONEID
	// it's mandatory for the place holder replacement in pms-conf
	if (pairfind(request->config_items, PW_SCS_SPP_ZONEID) != NULL) {
		DEBUG2("rlm_pms::pms_authorize: SCS-SPP-ZoneID already set.  Not setting to %s", zoneid);
	} else {
		DEBUG("rlm_pms::pms_authorize: Setting 'SCS-SPP-ZoneID := %s'", zoneid);
		pairadd(&request->config_items, pairmake("SCS-SPP-ZoneID", zoneid, T_OP_SET));
	}

	/*
	 * Check Recipient (user@pmsgw) and get stripped user name
	 */
	// Validate realm and extract user name
	vp = pairfind(request->packet->vps, PW_USER_NAME);
	if (!vp) {
		msgtxt = "Missing User-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}
	DEBUG2("rlm_pms::pms_authorize: User-Name attribute: '%s'.", vp->vp_strvalue);

	// If realm is valid, get stripped username,
	DEBUG2("rlm_pms::pms_authorize: Validate realm and extract user name");
	ret = pms_validate_realm(inst, vp->vp_strvalue, strippeduser);
	if (ret != 0) {
		msgtxt = "Invalid realm in User-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_INVALID_REALM", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_INVALID_REALM", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// Check stripped username
	if (!strlen(strippeduser)) {
		msgtxt = "Could not extract user name from User-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// Check stripped user name length
	if (strlen(strippeduser) > PMS_USERNAME_MAX_LEN) {
		DEBUG2("rlm_pms::authorize: Invalid length of stripped user name '%s'. Max. allowed length is %d.", strippeduser, PMS_USERNAME_MAX_LEN);
		msgtxt = "The length of the stripped user name is invalid. Authorization aborted.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	DEBUG2("rlm_pms::pms_authorize: Got stripped user name: '%s'.", strippeduser);
	strcpy(logfiledata->username, strippeduser);

	// Set the request attribute PW_SCS_SPP_STRIPPEDUSER
	if (pairfind(request->config_items, PW_SCS_SPP_STRIPPEDUSER) != NULL) {
		DEBUG2("rlm_pms::pms_authorize: SCS-SPP-StrippedUser already set.  Not setting to %s", strippeduser);
	} else {
		DEBUG2("rlm_pms::pms_authorize: Setting 'SCS-SPP-StrippedUser := %s'", strippeduser);
		pairadd(&request->config_items, pairmake("SCS-SPP-StrippedUser", strippeduser, T_OP_SET));
	}



	/***********************added by hj on 6 junly, 2011******************************/
	ret = pms_strip_tariffid(inst, vp->vp_strvalue, strippedtariffid);

	if (ret != 0) {
		msgtxt = "Failed to parse tariff id in User-Name attribute. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// Check stripped user name length
	if (strlen(strippedtariffid) > PMS_TARIFFID_MAX_LEN) {
		DEBUG2("rlm_pms::authorize: Invalid length of stripped tariff id '%s'. Max. allowed length is %d.", strippedtariffid, PMS_TARIFFID_MAX_LEN);
		msgtxt = "The length of the parsed tariff id is invalid. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_INTERNAL_ERR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_INTERNAL_ERR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	DEBUG2("rlm_pms::pms_authorize: Got stripped tariff id: '%s'.", strippedtariffid);
	strcpy(logfiledata->tariffid, strippedtariffid);

	// Set the request attribute PW_SCS_SPP_STRIPPEDUSER
	if (pairfind(request->config_items, PW_SCS_HSPGW_TARIFFID) != NULL) {
		DEBUG2("rlm_pms::pms_authorize: SCS-HSPGW-TARIFFID already set.  Not setting to %s", strippedtariffid);
	} else {
		DEBUG2("rlm_pms::pms_authorize: Setting 'SCS-HSPGW-TARIFFID := %s'", strippedtariffid);
		pairadd(&request->config_items, pairmake("SCS-HSPGW-TARIFFID", strippedtariffid, T_OP_SET));
	}



	/**
	 * Get configuration Data from DB
	 */
	// Get Configuration Data from database
	DEBUG2("rlm_pms::pms_authorize: Find SCS-SPP-ZoneID '%s' in the SPP database.", zoneid);
	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		msgtxt = "Connection to data base failed. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_SPPDB_CONN_ERR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_FAIL;
	}

	// Replace placeholders in querystr pms_config_query
	parse_repl(querystr, sizeof(querystr), inst->config->pms_config_query, request, sql_escape_func);
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Select query failed while querying 3rd party config data. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_FAIL;
	}

	if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == -1) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Could not get data from fetch call while querying 3rd party config data. Authorization aborted.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_FAIL;
	}
		// Get the values
	row = sqlsocket->row;
	if (row == NULL) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Could not find zone ID/tariff ID combination in database. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ZONE_TARIFF_NOT_FOUND", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ZONE_TARIFF_NOT_FOUND", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	DEBUG2("rlm_pms::pms_authorize: Found SCS-SPP-ZoneID in the database.");



	// Finally set the Authentication Type to protocol id (=row[3])
	sprintf(auth_type, inst->config->pms_conf_type_prefix, row[3]);

	//store the max access session number to max_sessions, for later comparation
	max_sessions = atoi(row[15]);

	// must finish before new query
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);

	/*******added by hj on 6 July, 2011************/
	sprintf(querystr, inst->config->pms_sim_access, zoneid, strippeduser);
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Database select query failed while querying simultaneous sessions. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_FAIL;
	}

	if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == -1) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Could not get data from fetch call for simultaneous sessions. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_FAIL;
	}

	// Get the values
	row = sqlsocket->row;
	if (row == NULL) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Cannot get the number of simultaneous sessions from database. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_NO_DATA", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_NO_DATA", T_OP_SET));
		free(logfiledata);
		//return RLM_MODULE_REJECT; // do not reject
	}

	DEBUG2("rlm_pms::pms_authorize: String max sim sess is %s.", row[0]);
	ret = atoi(row[0]);

	DEBUG2("rlm_pms::pms_authorize: number max sim sess is %d.", ret);
	if( max_sessions > 0 && ret != 0 && ret >= max_sessions ){
		// finish query and release sql socket
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Simultaneous sessions exceeded the maximum. Authorization failed.";
		pms_errLog(msgtxt, "SC_HSPGW_AUTH_MAX_SESS_EXCEEDED", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_MAX_SESS_EXCEEDED", T_OP_SET));
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	// To be changed
	if (pairfind(request->config_items, PW_AUTHTYPE) != NULL) {
		// finish query and release sql socket
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		DEBUG2("WARNING: Auth-Type already set.  Not setting to PMS");
		return RLM_MODULE_NOOP;
	}
	DEBUG2("rlm_pms::pms_authorize: Setting 'Auth-Type := %s'", auth_type);

	// Delegate
	pairadd(&request->config_items, pairmake("Auth-Type", auth_type, T_OP_EQ));

	// finish query and release sql socket
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	// Free memory
	free(logfiledata);

	return RLM_MODULE_OK;
}

/*
 *      Preaccounting by pms
 */

static int pms_preaccounting(void *instance, REQUEST *request) {
	VALUE_PAIR *vp;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char querystr[MAX_QUERY_LEN];
	int ret;
	char *msgtxt;
	char strippeduser[MAX_STRING_LEN];
	char strippedtariffid[MAX_STRING_LEN];
	char zoneid[PMS_ZONEID_MAX_LEN + 1];
	pms_tracker_data *trckrdata; // Tracker
	pms_logfile_data *logfiledata;
	char acct_type[PMS_AUT_ACC_TYPE_MAX_LEN];

	// Allocate memory for Tracker data
	trckrdata = rad_malloc(sizeof(pms_tracker_data));
	memset(trckrdata, 0, sizeof(pms_tracker_data));

	// Allocate memory for Logfile data
	logfiledata = rad_malloc(sizeof(pms_logfile_data));
	memset(logfiledata, '\0', sizeof(pms_logfile_data));

	DEBUG2("Entering PMS Preaccounting ...");
	DEBUG2("===========================");

	// initalize log data
	strcpy(logfiledata->sessid, "n/a");
	strcpy(logfiledata->tariffid, "n/a");
	strcpy(logfiledata->zoneid, "n/a");
	strcpy(logfiledata->username, "n/a");

	// Extract Acct-Session-Id attribute
	DEBUG2("rlm_pms::pms_preaccounting: Extract session ID from Acct-Session-Id attribute.");
	vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID);
	if (!vp){
		msgtxt = "Missing Acct-Session-Id attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}
	DEBUG2("rlm_pms::pms_preaccounting: Found attribute Acct-Session-Id attribute: '%s'.", vp->vp_strvalue);

	// Check  Acct-Session-Id length
	if (strlen(vp->vp_strvalue) > PMS_SESSIONID_MAX_LEN) {
		DEBUG2("rlm_pms::pms_preaccounting: Invalid length of Acct-Session-Id attribute. Max. allowed length is %d.", PMS_SESSIONID_MAX_LEN);
		msgtxt = "Invalid length of Acct-Session-Id attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Acct-Session-Id is OK
	strcpy(trckrdata->sessid, vp->vp_strvalue);
	strcpy(logfiledata->sessid, vp->vp_strvalue);

	/*
	 * Check PMS Location ID
	 */
	DEBUG2("rlm_pms::pms_preaccounting: Extracting SCS-SPP-ZoneID from WISPr-Location-Name attribute...");
	vp = pairfind(request->packet->vps, WISPR2ATTR(PW_LOCATION_NAME));
	if (!vp) {
		msgtxt = "Missing WISPr-Location-Name attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Get the stripped SCS-SPP-ZoneID
	if (pms_strip_zoneid(inst, vp->vp_strvalue, zoneid) < 0) {
		msgtxt = "Cannot parse zone ID in Location-Name attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	DEBUG("rlm_pms::pms_preaccounting: ZoneID extracted from Location-Name attribute: %s", zoneid);
	strcpy(trckrdata->zoneid, zoneid);
	strcpy(logfiledata->zoneid, zoneid);


	// Set the request attribute PW_SCS_SPP_ZONEID
	// it's mandatory for the place holder replacement in pms-conf
	if (pairfind(request->config_items, PW_SCS_SPP_ZONEID) != NULL) {
		DEBUG2("rlm_pms::pms_preaccounting: SCS-SPP-ZoneID already set. Not setting to %s", zoneid);
	} else {
		DEBUG("rlm_pms::pms_preaccounting: Setting 'SCS-SPP-ZoneID := %s'", zoneid);
		pairadd(&request->config_items, pairmake("SCS-SPP-ZoneID", zoneid, T_OP_SET));
	}

	// Validate realm and extract user name
	vp = pairfind(request->packet->vps, PW_USER_NAME);
	if (!vp) {
		msgtxt = "Missing User-Name attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}
	DEBUG("rlm_pms::pms_preaccounting: User-Name attribute: '%s'.", vp->vp_strvalue);

	// If realm is valid, get stripped username,
	DEBUG("rlm_pms::pms_preaccounting: Validate realm and extract user name");
	ret = pms_validate_realm(inst, vp->vp_strvalue, strippeduser);
	if (ret != 0) {
		msgtxt = "Invalid realm in User-Name attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_INVALID_REALM", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_REALM", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Stripped username
	if (!strlen(strippeduser)) {
		msgtxt = "Could not extract user name from User-Name attribute. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Check stripped user name length
	if (strlen(strippeduser) > PMS_USERNAME_MAX_LEN) {
		DEBUG2("rlm_pms::pms_preaccounting: Invalid length of stripped user name '%s'. Max. allowed length is %d.", strippeduser, PMS_USERNAME_MAX_LEN);
		msgtxt = "The length of the stripped user name is invalid. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	DEBUG2("rlm_pms::pms_preaccounting: Got stripped user name: '%s'.", strippeduser);
	strcpy(trckrdata->usr, strippeduser);
	strcpy(logfiledata->username, strippeduser);

	// Set the request attribute PW_SCS_SPP_STRIPPEDUSER
	if (pairfind(request->config_items, PW_SCS_SPP_STRIPPEDUSER) != NULL) {
		DEBUG2("rlm_pms::pms_preaccounting: SCS-SPP-StrippedUser already set.  Not setting to %s", trckrdata->usr);
	} else {
		DEBUG("rlm_pms::pms_preaccounting: Setting 'SCS-SPP-StrippedUser := %s'", trckrdata->usr);
		pairadd(&request->config_items, pairmake("SCS-SPP-StrippedUser", trckrdata->usr, T_OP_SET));
	}

	/***********************added by hj on 6 junly, 2011******************************/
		// Validate realm and extract user name
/* that vp still exists
	vp = pairfind(request->packet->vps, PW_USER_NAME);
		if (!vp) {
			msgtxt = "Missing User-Name attribute. Accounting aborted.";
			//pms_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_REALM", inst, logfiledata, NULL);
			free(trckrdata);
			free(logfiledata);
			return RLM_MODULE_OK;

		}
		DEBUG("rlm_pms::pms_preaccounting: User-Name attribute: '%s'.", vp->vp_strvalue);
*/
	ret = pms_strip_tariffid(inst, vp->vp_strvalue, strippedtariffid);

	if (ret != 0) {
		msgtxt = "Failed to parse tariff id in User-Name attribute. Pre-accounting failed.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_MALFORMED_REQ", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_MALFORMED_REQ", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Check stripped tariff id
	/* lentgh was checked already in pms_strip_tariffid()
	if (!strlen(strippedtariffid)) { // what is, if strlen == 0?  => !0 => true to be fixed
		msgtxt = "Could not extract tariff id from User-Name attribute. Pre-accounting aborted.";
		pms_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_TARIFFID", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}
*/

	// Check stripped user name length
	if (strlen(strippedtariffid) > PMS_TARIFFID_MAX_LEN || strlen(strippedtariffid) < 1) {
		DEBUG2("rlm_pms::authorize: Invalid length of stripped tariff id '%s'. Max. allowed length is %d.", strippedtariffid, PMS_TARIFFID_MAX_LEN);
		msgtxt = "The length of the stripped tariff id is invalid. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_INTERNAL_ERR", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_TARIFFID", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	DEBUG2("rlm_pms::pms_authorize: Got stripped tariff id: '%s'.", strippedtariffid);
	strcpy(logfiledata->tariffid, strippedtariffid);

	// Set the request attribute PW_SCS_SPP_STRIPPEDUSER
	if (pairfind(request->config_items, PW_SCS_HSPGW_TARIFFID) != NULL) {
		DEBUG2("rlm_pms::pms_authorize: SCS-HSPGW-TARIFFID already set.  Not setting to %s", strippedtariffid);
	} else {
		DEBUG2("rlm_pms::pms_authorize: Setting 'SCS-HSPGW-TARIFFID := %s'", strippedtariffid);
		pairadd(&request->config_items, pairmake("SCS-HSPGW-TARIFFID", strippedtariffid, T_OP_SET));
	}
	/*****************************************************/


	// Get Configuration Data from database
	DEBUG2("rlm_pms::pms_preaccounting: Find SCS-SPP-ZoneID in the SPP database.");
	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		msgtxt = "Connection to data base failed. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_SPPDB_CONN_ERR", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_errLog(msgtxt, "SC_HSPGW_ACCT_SPPDB_CONN_ERR", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Replace placeholders in querystr pms_config_query
	parse_repl(querystr, sizeof(querystr), inst->config->pms_config_query, request, sql_escape_func);
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Select query failed  while querying 3rd party config data. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_ORA_ERROR", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == -1) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Could not fetch row from select query while querying 3rd party config data. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_ORA_ERROR", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		pms_errLog(msgtxt, "SC_HSPGW_ACCT_ORA_ERROR", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Get the values
	row = sqlsocket->row;
	if (row == NULL) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Could not retrieve data while querying 3rd party config data. Pre-accounting aborted.";
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_ACCT_ZONE_TARIFF_NOT_FOUND", request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->username, msgtxt);
		pms_errLog(msgtxt, "SC_HSPGW_ACCT_ZONE_TARIFF_NOT_FOUND", inst, logfiledata, request);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}


	// Finally set the Authentication Type to protocol id
	sprintf(acct_type, inst->config->pms_conf_type_prefix, row[3]);

	// Release the mutex of socket
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	// To be changed
	if (pairfind(request->config_items, PW_ACCTTYPE) != NULL) {
		DEBUG2("rlm_pms::pms_accounting: Acct-Type already set. Not setting to PMS");
		return RLM_MODULE_OK;
	}

	DEBUG2("rlm_pms::pms_preaccouting: Setting 'Acct-Type := %s'", acct_type);

	// Delegate
	pairadd(&request->config_items, pairmake("Acct-Type", acct_type, T_OP_EQ));

	// Free memory
	free(trckrdata);
	free(logfiledata);

	return RLM_MODULE_OK;

}

/*
 *	Translate the SQL queries.
 */
static size_t sql_escape_func(char *out, size_t outlen, const char *in) {
	size_t len = 0;

	while (in[0]) {
		/*
		 *	Non-printable characters get replaced with their
		 *	mime-encoded equivalents.
		 */
		if ((in[0] < 32) || strchr(allowed_chars, *in) == NULL) {
			/*
			 *	Only 3 or less bytes available.
			 */
			if (outlen <= 3) {
				break;
			}

			snprintf(out, outlen, "=%02X", (unsigned char) in[0]);
			in++;
			out += 3;
			outlen -= 3;
			len += 3;
			continue;
		}

		/*
		 *	Only one byte left.
		 */
		if (outlen <= 1) {
			break;
		}

		/*
		 *	Allowed character.
		 */
		*out = *in;
		out++;
		in++;
		outlen--;
		len++;
	}
	*out = '\0';
	return len;
}

/*************************************************************************
 *
 *	Function: pms_strip_zoneid
 *
 *	Purpose: Internal function to strip the zoneid contained in Location-Name attribute
 *
 *  Input:
 *		locationname: string containing the Location-Name attribute (e.g. 'SCS:CH,476')
 *		strippedzoneid: the stripped zoneid (e.g. '476')
 *
 *	Return -1 on failure, 0 on success
 *
 *  Author: Juan V��squez
 *  Last changed: 16.12.2009
 *************************************************************************/
int pms_strip_zoneid(void *instance, char *locationname, char *outzoneid) {
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char *zoneid = NULL;
	char *delimiter = inst->config->pms_conf_locid_delimiter;
	int max_zid_len = PMS_ZONEID_MAX_LEN;
	int stripzidlen = 0;

	//	DEBUG2("rlm_pms::pms_strip_zoneid: Searching for delimiter '%s' in Location-Name attribute '%s'.", delimiter, locationname);
	zoneid = strstr(locationname, delimiter);
	if (!zoneid) {
		DEBUG("rlm_pms::pms_strip_zoneid: Could not find '%s' in '%s'.", delimiter, locationname);
		return -1;
	}
	zoneid++;
	stripzidlen = strlen(zoneid);
	if ((stripzidlen < 1) || (stripzidlen > max_zid_len)) {
		DEBUG("rlm_pms::pms_strip_zoneid: Invalid length of SCS-SPP-ZoneID.");
		return -1;
	}
	strcpy(outzoneid, zoneid);
	//	DEBUG2("rlm_pms::pms_strip_zoneid: Stripped SCS-SPP-ZoneID: '%s'.", strippedzoneid);
	return 0;
}

/*************************************************************************
 *
 *	Function: pms_strip_tariffid
 *
 *	Purpose: Internal function to strip the tariff id contained in the line 'username/tariffid@realm'.
 *
 *  Input:
 *		username: string containing the username, tariffid and realm (e.g. 'juan/1@pmswg')
 *		strippedtariffid: the stripped tariffid (e.g. '1') or NULL
 *
 *	Return -1 on strip failed, 0 on strip successed
 *
 *  Author: Hongjuan Lin
 *  Last changed: 6.07.2011
 *************************************************************************/
int pms_strip_tariffid(void *instance, char *username, char *strippedtariffid){

	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char *tariffdelimiter = inst->config->pms_conf_tariff_delimiter;
	char *realmdelimiter = inst->config->pms_conf_realm_delimiter;
	char *tmptariffid=NULL;
	int combinusernamelen = 0;
	int tariffidlen = 0;
	int is_strip_success = -1;

	char *realmname = NULL;
	char combinusername[MAX_STRING_LEN]; // before stripp the tarrif id, e.g.username/tariffid

	// Initialise strippedtariffid to avoid pointer issues
	strippedtariffid[0] = '\0';
	combinusername[0] = '\0';

	realmname=strstr(username,realmdelimiter);

	if(realmname){
		realmname++;
		if (strcmp(realmname, "\0") == 0) {

			DEBUG("rlm_pms :: pms_strip_tariffid : Could not find realm in '%s'.",username);

		}else{

			combinusernamelen = realmname - username - 1;

			if(combinusernamelen > 0){

				//strip username/tariffid to combinusername fisrt
				strncpy(combinusername, username, combinusernamelen);
				combinusername[combinusernamelen] = '\0';

				//locates tariffid and return pointer to strippedstariffid
				tmptariffid = strpbrk(combinusername, tariffdelimiter) + 1;

				tariffidlen = strlen(combinusername) - (tmptariffid - combinusername);
				if( tariffidlen > 0){

					strcpy(strippedtariffid, tmptariffid);
					strippedtariffid[tariffidlen] = '\0';
					is_strip_success = 0;

				}else{

					DEBUG("rlm_pms :: pms_strip_tariffid : Could not find tariff delimiter '%s' in '%s'.",tariffdelimiter, username);

				}

			}else {

				DEBUG("rlm_pms :: pms_strip_tariffid : Could not strip a username/id from '%s'.",username);

			}
		}

	}else {

		DEBUG("rlm_pms :: pms_strip_tariffid : Could not find realm delimiter '%s' in '%s'.",realmdelimiter, username);

	}

	return is_strip_success;
}


/*************************************************************************
 *
 *	Function: pms_validate_realm
 *
 *	Purpose: Internal function to verify the realm and
 *  strip the user contained in the line 'username@realm'.
 *
 *  Input:
 *		username: string containing the username and realm (e.g. 'juan@pmswg')
 *		strippeduser: the stripped user (e.g. 'juan') or NULL
 *
 *	Return -1 on invalid realm, 0 on valid realm
 *
 *  Author: Juan V��squez
 *  Last changed: 16.12.2009
 *************************************************************************/
int pms_validate_realm(void *instance, char *username, char *strippeduser) {
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char *realmname = NULL;
	char *delimiter = inst->config->pms_conf_realm_delimiter;
	char *tariffdelimiter = inst->config->pms_conf_tariff_delimiter;
	char *validrealm = inst->config->pms_conf_allowed_realm;
	int strippeduserlength = 0;
//	int tariffdelimiteridx = 0;
	int is_valid_realm = -1;

	// Initialise strippeduser to avoid pointer issues
	strippeduser[0] = '\0';

	//	DEBUG("rlm_pms::pms_validate_realm: Searching for delimiter '%s' in username '%s'.", delimiter, username);
	realmname = strstr(username, delimiter);
	if (realmname) {
		realmname++;
		if (strcmp(realmname, "\0") == 0) {
			DEBUG("rlm_pms::pms_validate_realm: Could not find realm in '%s'.", username);
		} else {
			//			DEBUG("rlm_pms::pms_validate_realm: Found realm '%s'.", realmname);
			if (strcmp(realmname, validrealm) == 0) {

				is_valid_realm = 0;
				DEBUG2("rlm_pms::pms_validate_realm: Realm '%s' is valid.",realmname);
				DEBUG2("rlm_pms::pms_validate_realm: tariffdelimiter is %s.",tariffdelimiter);

				strippeduserlength = strcspn(username, tariffdelimiter);
				DEBUG2("rlm_pms::pms_validate_realm: strippeduserlength is %d.",strippeduserlength);

				if (strippeduserlength > 0) {

					strncpy(strippeduser, username, strippeduserlength);
					strippeduser[strippeduserlength] = '\0';
					DEBUG2("rlm_pms::pms_validate_realm: strippeduser is %s.",strippeduser);

				} else {
					DEBUG2("rlm_pms::pms_validate_realm: Could not strip a username from '%s'.", username);
				}
			} else {
				DEBUG2("rlm_pms::pms_validate_realm: Realm '%s' is invalid.", realmname);
			}
		}
	} else { //(realmname)
		DEBUG("rlm_pms::pms_validate_realm: Could not find delimiter '%s' in '%s'.", delimiter, username);
	}
	return is_valid_realm;
}


/*************************************************************************
 *
 *	Function: pms_printConfigData
 *
 *	Purpose: Internal function to print a PMS data pool node
 *  @ struct pms_data *list: pointer to struct
 *
 *  Author: Juan Vassquez
 *  Last changed: 17.12.2009
 *************************************************************************/
static void pms_printConfigData(struct pms_data *list) {
	if (list) {
		DEBUG("___Config_Data_Beginn___");
		DEBUG("Location ID: %s", list->zoneid);
		DEBUG("IP Address: %s", list->ipaddr);
		DEBUG("Port number: %s", list->port);
		DEBUG("Protocol ID: %d", list->protocol);
		DEBUG("Session Time Out: %s", list->sesto);
		DEBUG("Amount per Unit: %s", list->amount);
		DEBUG("Attribute filed name 1: %s", list->usrfld);
		DEBUG("Attribute filed name 2: %s", list->pwfld);
		DEBUG("Tariff key: %s", list->tariff_key);
		DEBUG("Go Through: %d", list->go_through);
		DEBUG("DF_LHSPID_HRSM: %s", list->hsrmid);
		DEBUG("Is enabled: %d", list->is_enabled);
		DEBUG("___Config_Data_End___");
		//if (recursive) pms_printConfigData(list->next, recursive);
	}
}

/*************************************************************************
 *
 *	Function: pms_prepareLogData
 *
 *	Purpose: Internal function to prepare the logger data
 *  @ pms_logger_data *loggerdata: the tracker data
 *  Return 0 on success
 *
 *  Author: Juan V��squez
 *  Last changed: 01.12.2009
 *************************************************************************/
int pms_prepareLogData(pms_logger_data **loggerdata, char *zoneid, char *hsrmid, int logeventid, char *logdescr) {
	pms_logger_data *logdata;

	// Allocate memory for logger
	// Allocated memory will be freed in pms_dbLogger()
	logdata = rad_malloc(sizeof(pms_logger_data));
	memset(logdata, 0, sizeof(pms_logger_data));
	logdata->zoneid = atoi(zoneid);
	logdata->hsrmid = atoi(hsrmid);
	logdata->logeventid = logeventid;
	strcpy(logdata->logdescr, logdescr);
	*loggerdata = logdata;
	return 0;
}

/*************************************************************************
 *
 *	Function: pms_dbLogger
 *
 *	Purpose: Internal function to insert a new row into PMS_LOG table
 *  @ void *instance: the module instance
 *  @ REQUEST *request: the module request
 *  @ pms_logger_data *loggerdata: the tracker data
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan V��squez
 *  Last changed: 02.12.2009
 *************************************************************************/
int pms_dbLogger(void *instance, REQUEST *request, pms_logger_data *loggerdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	char querystr[MAX_QUERY_LEN];
	char insertquery[MAX_QUERY_LEN];
	int ret;
	pms_logger_data *logdata = loggerdata; // Logger


	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "%s : rlm_pms : zid %d : Connection to data base failed while running db log.", "SC_HSPGW_SPPDB_CONN_ERR", logdata->zoneid);
		return -1;
	}

	// Replace placeholders in querystr pms_config_query
	parse_repl(querystr, sizeof(querystr), inst->config->pms_logger_insert, request, sql_escape_func);
	sprintf(insertquery, querystr, logdata->zoneid, logdata->hsrmid, logdata->logeventid, logdata->logdescr);
	//DEBUG2("The STATEMENT: %s", insertquery);
	// Execute the insert query
	ret = inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, insertquery);
	if (ret != 0) {
		radlog(L_ERR, "%s : zid %d : Insert into log table failed while running db log.", "SC_HSPGW_ORA_ERROR", logdata->zoneid);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	// Free memory
	free(logdata);
	return 0;
}

/*************************************************************************
 *
 *	Function: pms_init
 *
 *	Purpose: Internal function initialise some db related stuff
 *  @ void *instance: PMS instance
 *
 *  Return -1 on failure, 1 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
int pms_init(void *instance) {
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	PMS_LOGEVENTS *logevents = NULL;
	char mqs[MAX_QUERY_LEN];
	int ret;

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "%s : Connection to data base failed.", "SC_HSPGW_SPPDB_CONN_ERR");
		return -1;
	}

	// Initialize logevents
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, inst->config->pms_all_events_query)) {
		radlog(L_ERR, "%s : rlm_pms initLogEventPool : Database query failed", "SC_HSPGW_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	// Fetch rows
	while (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == 0 && sqlsocket->row != NULL){
		row = sqlsocket->row;
		// Insert the node into the data struct
		// this will allocate memory for each event node!
		logevents = pms_insertLogEvent(logevents, row);

	}
	ret = pms_sizeLogEvent(logevents);

	// Error if could not collect log events data from database (database is empty?)
	DEBUG2("rlm_pms::pms_initLogEventPool: Total row number of collected log events: %d", ret);

	if (!ret) {
		radlog(L_ERR, "%s : rlm_pms init : LOGEVENTS_VIEW is empty.", "SC_HSPGW_ORA_NO_DATA");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	// Add the log event pool to the instance
	inst->logevents = logevents;

	// set online state in db
	//replace the wild cards
	sprintf(mqs, inst->config->pms_gw_set_state, "1", inst->hostname);
	DEBUG3("pms_gw_set_state query: %s", mqs);
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)){
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : rlm_pms init : Cannot set online state of %s in SPP data base", inst->hostname);
	}

	// Free the DB handles
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);


	return 1;
}

/*************************************************************************
 *
 *	Function: pms_freeLogEventPool
 *
 *	Purpose: Internal functions to free the event pool
 *  @ void *instance: PMS instance
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
void pms_freeLogEventPool(void *instance) {
	rlm_pms_module_t *inst = instance;
	PMS_LOGEVENTS *cur;
	PMS_LOGEVENTS *next;

	for (cur = inst->logevents; cur; cur = next) {
		next = cur->next;
		// Free memory
		free(cur);
	}

	inst->logevents = NULL;
}

/*************************************************************************
 *
 *	Function: pms_insertLogEvent
 *
 *	Purpose: Internal functions to insert a new node into the log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return pointer to the inserted node
 *
 *  Author: Juan V��squez
 *  Last changed: 14.12.2009
 *************************************************************************/
PMS_LOGEVENTS *pms_insertLogEvent(PMS_LOGEVENTS *list, SQL_ROW row) {
	PMS_LOGEVENTS *last;

	if (!pms_containsLogEvent(list, row)) {
		last = pms_find_lastLogEvent(list);
		// Allocate memory for each node calling pms_createLogEvent()
		if (last == NULL) {
			list = pms_createLogEvent(row);
		} else {
			last->next = pms_createLogEvent(row);
		}
	}
	return list;
}

/*************************************************************************
 *
 *	Function: pms_containsLogEvent
 *
 *	Purpose: Internal function to determine if the current SQL row is contained in the log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return 0 on not found, 1 on found
 *
 *  Author: Juan V��squez
 *  Last changed: 04.12.2009
 *************************************************************************/
int pms_containsLogEvent(PMS_LOGEVENTS *list, SQL_ROW row) {
	if (list == NULL) {
		return 0;
	}
	while (list) {
		if (list->id == atoi(row[0])) {
			return 1;
		}
		list = list->next;
	}
	return 0;
}

/*************************************************************************
 *
 *	Function: pms_findLogEventKey
 *
 *	Purpose: Internal function to find a node containing the given event key
 *  @ PMS_LOGEVENTS *list: the data pool
 *  @ char *eventkey: the log event key
 *  Return pointer to the selected node if found one, NULL if not found
 *
 *  Author: Juan V��squez
 *  Last changed: 04.12.2009
 *************************************************************************/
PMS_LOGEVENTS *pms_findLogEventKey(PMS_LOGEVENTS *list, char *eventkey) {
	PMS_LOGEVENTS *found = NULL;

	while (list) {
		if (strcmp(list->key, eventkey) == 0) {
			found = list;
			return found;
		}
		list = list->next;
	}
	return found;
}

/*************************************************************************
 *
 *	Function: pms_createLogEvent
 *
 *	Purpose: Internal function to create a new node in the log events struct
 *  @ SQL_ROW row: the result of the SQL SELECT query
 *  Return pointer to the new node
 *
 *  Author: Juan V��squez
 *  Last changed:14.12.2009
 *************************************************************************/
PMS_LOGEVENTS *pms_createLogEvent(SQL_ROW row) {
	PMS_LOGEVENTS *list;

	// Allocate memory
	list = rad_malloc(sizeof(PMS_LOGEVENTS));
	memset(list, 0, sizeof(PMS_LOGEVENTS));

	list->id		= atoi(row[0]);
	strcpy(list->key, row[1]);
	strcpy(list->type, row[2]);
	strcpy(list->level, row[3]);
	strcpy(list->postcond, row[4]);
	strcpy(list->format, row[1]);

	list->next		= NULL;
	return list;
}

/*************************************************************************
 *
 *	Function: pms_find_lastLogEvent
 *
 *	Purpose: Internal function to determine the last node of the log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  Return pointer to the last node
 *
 *  Author: Juan V��squez
 *  Last changed: 04.12.2009
 *************************************************************************/
PMS_LOGEVENTS *pms_find_lastLogEvent(PMS_LOGEVENTS *list) {
	PMS_LOGEVENTS *last = list;
	while (list) {
		last = list;
		list = list->next;
	}
	return last;
}

/*************************************************************************
 *
 *	Function: pms_sizeLogEvent
 *
 *	Purpose: Internal function to calculate the number of nodes in log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  Return the size
 *
 *  Author: Juan V��squez
 *  Last changed: 04.12.2009
 *************************************************************************/
int pms_sizeLogEvent(PMS_LOGEVENTS *list) {
	int count = 0;
	for (; list; list = list->next, count++) {
	}
	return count;
}


/*************************************************************************
 *
 *  Function: pms_infoLog
 *
 *  Purpose: event loggin in the database (SPP-DB)
 *
 *  @ char *msgtxt: message text
 *  @ char *eventkey: event key
 *  @ void *instance: instance
 *  @ pms_data *pmsdata: pms_data
 *  @ REQUEST *request: request
 *
 *  Author: Juan V��squez
 *  Last changed: 	25.01.2010
 *  				2011-07-12 hm/wdw: change locationid to zoneid
 *************************************************************************/

void pms_infoLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *logfiledata, REQUEST *request) {
	pms_logger_data *logdata = NULL;
	PMS_LOGEVENTS *the_event = NULL;
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;

	//DEBUG("rlm_pms: %s, %s, %s, %s : %s", logfiledata->sessid, logfiledata->zoneid, logfiledata->username, eventkey, msgtxt);
	//radlog(L_INFO, "%s : %s : %s : %s : %s : %s", eventkey, logfiledata->sessid, logfiledata->zoneid, logfiledata->tariffid, logfiledata->username, msgtxt);

	if (request) {
		radlog(L_INFO, "%s : req %u : sid %s : zid %s : tid %s : usr %s : %s", eventkey, request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->tariffid, logfiledata->username, msgtxt);
		the_event = pms_findLogEventKey(inst->logevents, eventkey);
		if (!the_event) {
			radlog(L_ERR, "%s : authorize : LogEventKey '%s' does not exist in DB. DB logger will not work!", "SC_HSPGW_INVALID_LOGKEY", eventkey);
		}
		else {
			pms_prepareLogData(&logdata, logfiledata->zoneid, logfiledata->hsrmid, the_event->id, msgtxt);
			pms_dbLogger(inst, request, logdata);
		}
	} else {
		radlog(L_INFO, "%s : req n/a : sid%s : zid %s : tid %s : usr %s : %s", eventkey, logfiledata->sessid, logfiledata->zoneid, logfiledata->tariffid, logfiledata->username, msgtxt);
	}

	// if (request)
}

/*************************************************************************
 *
 * Function: pms_errLog
 *
 * Purpose: event loggin (radlog, snmp trap and optional in SPP-DB)
 *
 * Input:	message text
 *			event key
 * 			instance
 *			logfile data
 *			request: if NULL then no db-log
 *
 *  Last changed: 03.03.2010
 *************************************************************************/

void pms_errLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *logfiledata, REQUEST *request) {

	pms_logger_data *logdata = NULL;
	PMS_LOGEVENTS *the_event = NULL;
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;


	if (request) {
		radlog(L_ERR, "%s : req %u : sid %s : zid %s : tid %s : usr %s : %s", eventkey, request->number, logfiledata->sessid, logfiledata->zoneid, logfiledata->tariffid, logfiledata->username, msgtxt);
		the_event = pms_findLogEventKey(inst->logevents, eventkey);
		if (!the_event) {
			radlog(L_ERR, "rlm_pms : %s : LogEventKey '%s' does not exist in DB. DB logger will not work!", "SC_HSPGW_INVALID_LOGKEY", eventkey);
		}
		else {
			pms_prepareLogData(&logdata, logfiledata->zoneid, logfiledata->hsrmid, the_event->id, msgtxt);
			pms_dbLogger(inst, request, logdata);
/*			if (strcmp(the_event->level, "Critical") == 0){
				pms_snmpTrap(msgtxt,instance,request);
			}
*/
		}
	} else {
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : tid %s : usr %s : %s", eventkey, logfiledata->sessid, logfiledata->zoneid, logfiledata->tariffid, logfiledata->username, msgtxt);
	}// if (request)
}


/*************************************************************************
 *
 * Function: pms_snmpTrap
 *
 * Purpose: send message per snmp trap
 *
 * Input:	message text
 *			instance
 *			request
 *
 *  Last changed: 03.03.2010
 *************************************************************************/

void pms_snmpTrap(char *msgtxt, void *instance, REQUEST *request) {
	rlm_pms_module_t *inst = (rlm_pms_module_t *) instance;

	char command[256];
	int	result;
	char	buffer[256];

	if (strcmp(inst->config->pms_snmptrap_command,"")==0){
		return;
	}

	sprintf(command,inst->config->pms_snmptrap_command,msgtxt);

	result = radius_exec_program(command, request,
					 TRUE, /* wait */
					 buffer, sizeof(buffer),
					 NULL, NULL, 1);
	if (result != 0) {
		DEBUG2("snmptrap failed.");
	}
}



module_t rlm_pms = {
	RLM_MODULE_INIT,
	"pms",
	RLM_TYPE_CHECK_CONFIG_SAFE, /* type */
	pms_instantiate, /* instantiation */
	rlm_pms_detach, /* detach */
	{ 	NULL, /* authentication */
		pms_authorize, /* authorization */
		pms_preaccounting, /* preaccounting */
		NULL, /* accounting */
		NULL, /*pms_checksimul,*//* checksimul */
		NULL, /* pre-proxy */
		NULL, /* post-proxy */
		NULL /* post-auth */
#ifdef WITH_COA
		, NULL /* recv-coa */
		, NULL /* send-coa */
#endif
	},
};
