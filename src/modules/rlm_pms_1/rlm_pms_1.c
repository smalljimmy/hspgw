/*
 * rlm_pms_1.c
 *
 * Version:    $Id$
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

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/modpriv.h>
#include <rlm_sql.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <netdb.h>
#include <string.h>
#include <arpa/inet.h>
#include <fcntl.h>        /* for nonblocking */
#include <sys/time.h>
#include <regex.h>
#include <time.h>
#include "rlm_pms_1.h"
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
 *    The instance data for rlm_pms is the list of fake values we are
 *    going to return.
 */

/*
 *    Translate the SQL queries.
 */

static char *allowed_chars = NULL;
static char *guestname_delimiter = NULL;

static size_t sql_escape_func(char *out, size_t outlen, const char *in) {
	size_t len = 0;

	while (in[0]) {
		/*
		 *    Non-printable characters get replaced with their
		 *    mime-encoded equivalents.
		 */
		if ((in[0] < 32) || strchr(allowed_chars, *in) == NULL) {
			/*
			 *    Only 3 or less bytes available.
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
		 *    Only one byte left.
		 */
		if (outlen <= 1) {
			break;
		}

		/*
		 *    Allowed character.
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

static int rlm_pms_detach(void *instance) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	//    PMS_SOCKET_POOL *cur;
	//    PMS_SOCKET_POOL *next;
	//    PMS_SOCKET *pmssocket;

	// before freeing the memory we should disconnect from all connected PMS:
	/*    if (inst->datapool) {
	 for (cur = inst->datapool->socketpool; cur; cur = next) {
	 next = cur->next;
	 pmssocket = cur->sckt;
	 if(pmssocket->state == pmssockconnected)
	 pms_fias_disconnect(pmssocket);
	 }
	 }
	 */
	if (inst->config) {
		int i;

		if (inst->datapool) {
			pms_fias_freeDataPool((void *) inst);
		}

		if (inst->logevents) {
			pms_fias_freeLogEventPool(inst);
		}

		/* try to avoid xlat, since i don't know what it is and how it works:
		 if (inst->config->xlat_name) {
		 xlat_unregister(inst->config->xlat_name,(RAD_XLAT_FUNC)sql_xlat);
		 free(inst->config->xlat_name);
		 }
		 */

		/*
		 *    Free up dynamically allocated string pointers.
		 */
		for (i = 0; module_config[i].name != NULL; i++) {
			char **p;

			if (module_config[i].type != PW_TYPE_STRING_PTR) {
				continue;
			}

			/*
			 *    Treat 'config' as an opaque array of bytes,
			 *    and take the offset into it.  There's a
			 *      (char*) pointer at that offset, and we want
			 *    to point to it.
			 */
			p = (char **) (((char *) inst->config) + module_config[i].offset);
			if (!*p) { /* nothing allocated */
				continue;
			}
			free(*p);
			*p = NULL;
		}
		/*
		 *    Catch multiple instances of the module.
		 */
		if (allowed_chars == inst->config->pms_fias_allowed_chars) {
			allowed_chars = NULL;
		}
		free(inst->config);
		inst->config = NULL;
	}

	if (inst->handle) {
#if 0
		/*
		 *    FIXME: Call the modules 'destroy' function?
		 */
		lt_dlclose(inst->handle); /* ignore any errors */
#endif
	}
	free(inst);

	return 0;
}

/*
 *    Do any per-module initialization that is separate to each
 *    configured instance of the module.  e.g. set up connections
 *    to external databases, read configuration files, set up
 *    dictionary entries, etc.
 *
 *    If configuration information is given in the config section
 *    that must be referenced in later calls, store a handle to it
 *    in *instance otherwise put a null pointer there.
 */

static int pms_instantiate(CONF_SECTION *conf, void **instance) {
	module_instance_t *modinst;
	rlm_pms_1_module_t * inst;

	DEBUG2("Entering PMS 1 Instantiation ...");
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
	 *    If the configuration parameters can't be parsed, then fail.
	 */
	if (cf_section_parse(conf, inst->config, module_config) < 0) {
		rlm_pms_detach(inst);
		return -1;
	}

	if ((inst->config->sql_instance_name == NULL) || (strlen(
			inst->config->sql_instance_name) == 0)) {
		radlog(
				L_ERR,
				"%s : rlm_pms_1 instantiation : The 'sql-instance-name' variable must be set.",
				"SC_HSPGW_CONFIG_ERR");
		rlm_pms_detach(inst);
		return -1;
	}

	modinst = find_module_instance(cf_section_find("modules"),
			inst->config->sql_instance_name, 1);
	if (!modinst) {
		radlog(
				L_ERR,
				"%s : rlm_pms_1 instantiation : Failed to find sql instance named %s",
				"SC_HSPGW_CONFIG_ERR", inst->config->sql_instance_name);
		rlm_pms_detach(inst);
		return -1;
	}

	if (strcmp(modinst->entry->name, "rlm_sql") != 0) {
		radlog(
				L_ERR,
				"%s : rlm_pms_1 instantiation : Module \"%s\" is not an instance of the rlm_sql module",
				"SC_HSPGW_CONFIG_ERR", inst->config->sql_instance_name);
		rlm_pms_detach(inst);
		return -1;
	}

	inst->sql_inst = (SQL_INST *) modinst->insthandle;

	// what ever...
	allowed_chars = inst->config->pms_fias_allowed_chars;
	guestname_delimiter = inst->config->pms_conf_guestname_delimiter;

	// needed, since there are 2 instances of hspgw in pwlan env
	gethostname(inst->hostname, sizeof inst->hostname);

	//Collect ALL log events from the database
	if (pms_fias_initLogEventPool(inst) < 0) {
		rlm_pms_detach(inst);
		return -1;
	}

	// Collect data PMS data from the SPP database.
	// This information is required to create the PMS socket pool.
	if (pms_fias_initDataPool(inst) < 0) {
		rlm_pms_detach(inst);
		return -1;
	}

	*instance = inst;

	// todo: (here if usefull) for each socket start thread, if activegw = me or null
	return 0;
}

/*
 *  Authentication for PMS module
 */

static int pms_authenticate(void *instance, REQUEST *request) {
	VALUE_PAIR *vp, *cai;
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	int ret;
	char *msgtxt = NULL;
	char strtimeout[7]; // See typedef struct pms_data
	char querystr[MAX_QUERY_LEN];
	char *ekey = NULL;
	char strMacAddress[19] = "";
	char strIpAddress[16] = "";
	int free_access, authenticated, session_timeout;
	int current_timestamp = time(NULL);
	int autoLogin = 0;
	int webproxy = 0;
	int trackerId = -1;
	int occupancyId = -1;
	int deadline;
	pms_tracker_data *trckrdata; // Tracker
	pms_logfile_data *logfiledata;
	pms_data *pmsdata = NULL;
	PMS_DATA_LIST *pmsdatalist = NULL, *list = NULL;
	PMS_SOCKET *pmssocket;

	// Allocate memory for Tracker data
	trckrdata = rad_malloc(sizeof(pms_tracker_data));
	memset(trckrdata, 0, sizeof(pms_tracker_data));
	trckrdata->billed = 0;
	strcpy(trckrdata->guestid, "");
	trckrdata->next = NULL;

	// Allocate memory for Logfile data
	logfiledata = rad_malloc(sizeof(pms_logfile_data));
	memset(logfiledata, '\0', sizeof(pms_logfile_data));

	DEBUG2("Entering PMS 1 Authentication ...");
	DEBUG2("===============================");
	// initalize log data
	strcpy(logfiledata->sessid, "n/a");
	strcpy(logfiledata->tariffid, "n/a");
	strcpy(logfiledata->zoneid, "n/a");
	strcpy(logfiledata->username, "n/a");

	/*
	 * Verify the SCS-SPP-StrippedUser, SCS-SPP-ZoneID and User-Password attributes
	 */

	// Extract Acct-Session-Id attribute
	DEBUG2("rlm_pms_1::pms_authenticate: Reading required attributes.");
	vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID);

	strcpy(trckrdata->sessid, vp->vp_strvalue);
	strcpy(logfiledata->sessid, vp->vp_strvalue);

	// Check the SCS-SPP-ZoneID (it was already set in pms_authorize())
	vp = pairfind(request->config_items, PW_SCS_SPP_ZONEID);

	strcpy(trckrdata->zoneid, vp->vp_strvalue);
	strcpy(logfiledata->zoneid, vp->vp_strvalue);

	// added by Hj Lin on 4 July, 2011
	/*    vp = pairfind(request->packet->vps, PW_NAS_IP_ADDRESS);
	 struct in_addr nasip;
	 char nasipstr[16];
	 nasip.s_addr = vp->vp_ipaddr;
	 strncpy(nasipstr, inet_ntoa(nasip), sizeof(nasipstr) - 1);
	 nasipstr[sizeof(nasipstr)-1] = '\0';
	 */

/*	radlog(	L_INFO,
			"SC_HSPGW_AUTH_INCOMING_REQ : req %u : sid %s : zid %s : tid n/a : usr n/a : Processing Access-Request from client <%s>",
			request->number, logfiledata->sessid, logfiledata->zoneid,
			request->client->shortname);
*/

	// Check the stripped user name (it was already set in pms_authorize())
	vp = pairfind(request->config_items, PW_SCS_SPP_STRIPPEDUSER);
	if (!vp) {
		msgtxt
				= "Missing SCS-SPP-StrippedUser attribute in incoming request. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_INTERNAL_ERR", inst,
				logfiledata, NULL);
		pairadd(&request->reply->vps, pairmake("Reply-Message",
				"SC_HSPGW_AUTH_INTERNAL_ERR", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

	DEBUG2(	"rlm_pms_1::pms_authenticate: Found attribute SCS-SPP-StrippedUser: '%s'.", vp->vp_strvalue);

	strcpy(trckrdata->usr, vp->vp_strvalue);
	strcpy(logfiledata->username, vp->vp_strvalue);

	// Extract User-Password attribute
	DEBUG2("rlm_pms_1::pms_authenticate: Extract password from User-Password attribute.");
	vp = pairfind(request->packet->vps, PW_PASSWORD);
	if (!vp) {
		msgtxt = "Missing User-Password attribute in incoming request. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst,
				logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message",
				"SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}
	DEBUG2("rlm_pms_1::pms_authenticate: Found attribute User-Password: '%s'.",
				vp->vp_strvalue);

	radlog(	L_INFO,
			"SC_HSPGW_AUTH_INCOMING_REQ : req %u : sid %s : zid %s : tid %i : usr %s : pwd %s : Processing Access-Request from client <%s>",
			request->number, logfiledata->sessid, logfiledata->zoneid, trckrdata->tariffid, trckrdata->usr, vp->vp_strvalue,
			request->client->shortname);

	// we need to convert the pwd to ascii-7 code in order to compare it with values from DB or PMS
	convert2ascii(trckrdata->pwd, sizeof(trckrdata->pwd), vp->vp_strvalue);
	pairadd(&request->config_items, pairmake("SCS-SPP-DecodedPassword",
			trckrdata->pwd, T_OP_SET));


	// Extract Account-Info Attribut
	vp = pairfind(request->packet->vps, CISCO2ATTR(PW_SCS_ACCOUNT_INFO));
	if (!vp) {
		radlog(L_INFO, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_AUTH_MALFORMED_REQ",
				logfiledata->sessid, logfiledata->zoneid, logfiledata->username, "Missing Cisco-SSG-Account-Info attribute. Re-login cannot be initialized.");
	}
	else
	{
		DEBUG2("rlm_pms_1::pms_authenticate: Found attribute Cisco-SSG-Account-Info: '%s'.",
					vp->vp_strvalue);

		// determine AutoLogin flag
		if(strstr(vp->vp_strvalue, "AutoLogin=1") == NULL)
		{
			radlog(L_INFO, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_AUTH_MALFORMED_REQ",
					logfiledata->sessid, logfiledata->zoneid, logfiledata->username, "Missing AutoLogin flag. Re-login cannot be initialized.");
		}
		else
		{
			autoLogin = 1;
			webproxy = extract_webproxy_from_account_info(vp->vp_strvalue);			
			DEBUG2("rlm_pms_1::pms_authenticate: Found AutoLogin=1 and WebProxy=%d", webproxy);
		}
	}

	if (autoLogin > 0){

		// Extract Framed-IP-Address Attribute.
		// Sample: Framed-IP-Address = 10.15.155.88
		vp = pairfind(request->packet->vps, PW_FRAMED_IP_ADDRESS);
		if (!vp) {
			autoLogin = 0;
			DEBUG2("rlm_pms_1::pms_authenticate: Reset AutoLogin");
			radlog(L_INFO, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_AUTH_MALFORMED_REQ",
					logfiledata->sessid, logfiledata->zoneid, logfiledata->username, "Missing Framed-IP-Address attribute. Re-login cannot be initialized.");
		}
		else
		{
			convertRadiusIpAddress(strIpAddress, vp->vp_strvalue);
			DEBUG2("rlm_pms_1::pms_authenticate: Found attribute Framed-IP-Address: '%s'.",strIpAddress);
		}

		// Extract Calling-Station-Id Attribute
		// Sample: Calling-Station-Id = 844b.f5d2.e967
		vp = pairfind(request->packet->vps, PW_CALLING_STATION_ID);
		if (!vp) {
			autoLogin = 0;
			DEBUG2("rlm_pms_1::pms_authenticate: Reset AutoLogin");
			radlog(L_INFO, "%s : req n/a : sid %s : zid %s : usr %s : %s",  "SC_HSPGW_AUTH_MALFORMED_REQ",
					logfiledata->sessid, logfiledata->zoneid, logfiledata->username, "Missing Calling-Station-Id attribute. Re-login cannot be initialized.");
		}
		else
		{
			DEBUG2("rlm_pms_1::pms_authenticate: Found attribute Calling-Station-Id: '%s'.",
						vp->vp_strvalue);
			strcpy(strMacAddress, vp->vp_strvalue);
		}

	}

	// Purge database table PMS_ACCOUNT_TRACKER
	DEBUG2("rlm_pms_1::pms_authenticate: Purge database tables ACCOUNT_TRACKER_RELOGIN and ACCOUNT_TRACKER.");
	ret = pms_fias_accTrackerPurge(inst, request);
	// DB Failure
	if (ret < 0) {
		msgtxt = "Could not purge ACCOUNT ACCOUNT_TRACKER_RELOGIN and ACCOUNT_TRACKER table. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata,
				request);
		pairadd(&request->reply->vps, pairmake("Reply-Message",
				"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_REJECT;
	}

//	if (autoLogin > 0){
	// Call stored function HSPGW_RELOGIN which updates the relogin tables.
//		proc_relogin(inst, pmsdata, strMacAddress, strIpAddress);
//	}

	// hm/am, 20130319: prepare Cisco account-info (cai) with bandwidth profile ID.
	// The id is stored in the vendor-specific attribute "Cisco-Account-Info".
	// The attribute value is set to "roaming@hspgw-n" with n = bandwidth profile ID,
	// or "roaming@hspgw" if there is no BW profile.

	parse_repl(querystr, sizeof(querystr), inst->config->pms_config_query,
			request, sql_escape_func);

	// get pms data
	if (!get_pmsdatalist(&pmsdatalist, inst, logfiledata, querystr, "AUTH")) {
		msgtxt = "Could not allocate PMS configuration data. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata,
				request);
		pairadd(&request->reply->vps,
				pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_REJECT;
	}

	int bwProfileId = pmsdatalist->pmsdata->bw_profile_id;
	int tariffid = pmsdatalist->pmsdata->tariffid;
	char cai_value[25];
	if (bwProfileId == 0) {
		sprintf(cai_value, "roaming@hspgw");
	} else {
		sprintf(cai_value, "roaming@hspgw-%d", bwProfileId);
	}
	cai = pairmake("Cisco-Account-Info", cai_value, T_OP_EQ);
	DEBUG2("Set Cisco-Account-Info='%s'.", cai_value);

	trackerId = pms_fias_accTrackerFind(inst, request, &deadline);
	if (trackerId == -1) {
		// DB Failure
		msgtxt = "Database select query failed while querying account tracker. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata,
				request);
		pairadd(&request->reply->vps,
				pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_REJECT;
	} else if (trackerId == -2) {
		msgtxt = "Missing SCS-SPP-DecodedPassword attribute. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_MALFORMED_REQ", inst,
				logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message",
				"SC_HSPGW_AUTH_MALFORMED_REQ", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_REJECT;
	}
	// Valid authentication already exists in the database
	// Recalculate Session Timeout value
	else if (trackerId > 0 && strcmp(pmsdatalist->pmsdata->tariff_key, "PER_TIME") == 0) {
		// Access accepted
		msgtxt = "Found valid authentication in ACCOUNT_TRACKER table. Authentication succeeded.";
		pms_fias_infoLog(msgtxt, "SC_HSPGW_AUTH_BY_DB", inst, logfiledata, request);
		// Calculate new Session-Timeout
		session_timeout = deadline - current_timestamp;
		if (session_timeout < inst->config->pms_conf_sesto_threshold) {
			session_timeout += inst->config->pms_conf_sesto_threshold;
		}

		if (autoLogin > 0){
			// insert MAC address into HSPGW_ACCTR_RELOGIN
			ret = pms_fias_accTrackerReloginInsert(inst, request, &trckrdata, trackerId, strMacAddress, strIpAddress, pmsdatalist->pmsdata->group, webproxy);
			if (ret < 0) {
				msgtxt = "Could not insert account data into ACCOUNT_TRACKER_RELOGIN table. Authentication aborted.";
				pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
						logfiledata, request);
				pairadd(&request->reply->vps, pairmake("Reply-Message",
						"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
				free(trckrdata);
				free(logfiledata);
				freepl(pmsdatalist);
				return RLM_MODULE_REJECT;
			}
		}

		sprintf(strtimeout, "%d", session_timeout);
		pairadd(&request->reply->vps, pairmake("Session-Timeout", strtimeout, T_OP_EQ));

		// hm/am, 20130319: add cisco account-info (cai)
		pairadd(&request->reply->vps, cai);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}
	else {
		// (ret == 0):
		// New Authentication request, proceed to authenticate via PMS socket
		// Insert new record into database table
		DEBUG2(
					"rlm_pms_1::pms_authenticate: This is a new authentication request. Alternative authentication required.");
	}

	// authenticate by table hspgw_occupancy
	occupancyId = pms_checkedin(inst, request, &trckrdata);

	if (occupancyId == -1) { // db error

		msgtxt = "Database select query failed while querying occupancy. Authentication aborted.";
		pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata, request);
		pairadd(&request->reply->vps, pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_REJECT;
	} else if (occupancyId > 0) {

		DEBUG2("rlm_pms_1::pms_authenticate: Session Deadline value: %d",
					trckrdata->deadline);
		ret = pms_fias_accTrackerInsert(inst, request, trckrdata);
		// DB Failure
		if (ret < 0) {
			msgtxt
					= "Could not insert account data into ACCOUNT_TRACKER table. Authentication aborted.";
			pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
					logfiledata, request);
			pairadd(&request->reply->vps, pairmake("Reply-Message",
					"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
			free(trckrdata);
			free(logfiledata);
			freepl(pmsdatalist);
			return RLM_MODULE_REJECT;
		}

		trackerId = pms_fias_accTrackerFind(inst, request, &deadline);

		// insert MAC address into HSPGW_ACCTR_RELOGIN
		if (autoLogin > 0){
			ret = pms_fias_accTrackerReloginInsert(inst, request, &trckrdata, trackerId, strMacAddress, strIpAddress, pmsdatalist->pmsdata->group, webproxy);
			if (ret < 0) {
				msgtxt = "Could not insert account data into ACCOUNT_TRACKER_RELOGIN table. Authentication aborted.";
				pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
						logfiledata, request);
				pairadd(&request->reply->vps, pairmake("Reply-Message",
						"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
				free(trckrdata);
				free(logfiledata);
				freepl(pmsdatalist);
				return RLM_MODULE_REJECT;
			}

			if (strcmp(pmsdatalist->pmsdata->amount, "0") == 0) {
				pms_fias_occupancyReloginInsert(inst, request, &trckrdata, occupancyId, strMacAddress, strIpAddress, pmsdatalist->pmsdata->group, webproxy);
			}
		}

		msgtxt = "Authentication by occupancy table. Authentication succeeded.";
		pms_fias_infoLog(msgtxt, "SC_HSPGW_AUTH_BY_DB", inst, logfiledata,
				request);
		session_timeout = atoi(trckrdata->sesto);
		// + inst->config->pms_conf_sesto_threshold; // add threshold to sesto
		sprintf(strtimeout, "%d", session_timeout);
		pairadd(&request->reply->vps, pairmake("Session-Timeout", strtimeout, T_OP_EQ));

		// hm/am, 20130319: add cisco account-info (cai)
		pairadd(&request->reply->vps, cai);

		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	// reorder zone list so the tracker zone id comes first
	reorder_pmsdatalist(&pmsdatalist, trckrdata->zoneid);

	/*
	 * Check zone id
	 */
	for (list = pmsdatalist; list != NULL; list = list->next) {

		pmsdata = list->pmsdata;

		strcpy(logfiledata->hsrmid, pmsdata->hsrmid);
		strcpy(logfiledata->zoneid, pmsdata->zoneid);
		strcpy(trckrdata->zoneid, pmsdata->zoneid);
		strcpy(trckrdata->sesto, pmsdata->sesto);
		trckrdata->tariffid = pmsdata->tariffid;
		strcpy(trckrdata->tariff_key, pmsdata->tariff_key);

		// Get go through value
		free_access = pmsdata->go_through;

		// todo active_gw is obsolete since use of FmCS!!
		// we have two nodes. set this node as active
		if (strcmp(inst->hostname, pmsdata->active_gw)) {

			if (!upd_active_gw(inst, request, pmsdata)) {
				msgtxt = "Could not update active hspgw on SPP DB. Authentication aborted.";
				pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
						logfiledata, request);
				pairadd(&request->reply->vps, pairmake("Reply-Message",
						"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
				free(trckrdata);
				free(logfiledata);
				freepl(pmsdatalist);
				return RLM_MODULE_REJECT;
			}
		}
		// todo end

		// get socket from pool
		pmssocket = pms_fias_find_SocketData(inst->datapool->socketpool,
				pmsdata->ipaddr, pmsdata->port);

		if (pmssocket == NULL) {
			DEBUG2(
						"rlm_pms_1::pms_authenticate: find new IP %s:%s (new PMS?), update the socket pool",
						pmsdata->ipaddr, pmsdata->port);

			//radlog(L_INFO,"create new socket and start new thread with request %u.", request->number);

			pmssocket = pms_fias_insertSocketPool(inst, pmsdata);

			//start separate thread to handle incoming messages
			changeSockState(pmssocket, pending, pmssocket->shutdown);
			//        pmssocket->shutdown = 0;
			pms_fias_startthread(inst, pmssocket, pmsdata);
			//wait a little until we send the PR request (up to 3sec cz of the init sequence)
			sleep(PMS_INIT_TIME + 1); // just for certification: wait a little longer
			if ((pmssocket->state == pmssockconnected) && is_obsolete(pmsdata->ecot)) {
				radlog(L_INFO,"socket connected. sending DR.%u, ecot: %i", request->number, pmsdata->ecot);
				sendDR(pmssocket); // db synch
			}
		} else {
			// i guess here we got the deadlock:
			if (pmssocket->state == pmssockunconnected) {
				if (pmssocket->thread != 0) {
					radlog(L_INFO,"there is a unconnected socket inclusive thread. notify thread to restart with request %u.", request->number);
					//set signal to restart
					changeSockState(pmssocket, pending, pmssocket->shutdown);
					pms_fias_notifyThreadRestart(pmssocket);
					radlog(L_INFO,"thread should have restarted with request %u.", request->number);

				} else {
					radlog(L_INFO,"there is a unconnected socket without thread. create a new thread with request %u.", request->number);
					changeSockState(pmssocket, pending, 0);
					pms_fias_startthread(inst, pmssocket, pmsdata);
				}
				//wait a little until we send the PR request (up to 3sec cz of the init sequence)
				sleep(PMS_INIT_TIME);
				if ((pmssocket->state == pmssockconnected) && is_obsolete(pmsdata->ecot)) {
					radlog(L_INFO,"socket connected. sending DR.%u, ecot: %i", request->number, pmsdata->ecot);
					sendDR(pmssocket); // db synch
				}
			}
		}

		// last socket test here before it is used
		if (pmssocket == NULL || pmssocket->state != pmssockconnected
				|| pmssocket->thread == 0 || pmssocket->shutdown == 1) {

			if (pmssocket == NULL) {
				msgtxt = "PMS socket not available. Cannot process request.";
				ekey = "SC_HSPGW_AUTH_RESOURCE_BUSY";
			} else if (pmssocket->state == pending) {
				//radlog(L_INFO, "SOCKET IS PENDING, req: %u", request->number);
				msgtxt
						= "PMS socket not connected: SOCKET IS PENDING. Cannot process request. Authentication aborted.";
				ekey = "SC_HSPGW_AUTH_RESOURCE_BUSY";
			} else if (pmssocket->state == pmssockunconnected) {
				//radlog(L_INFO, "SOCKET IS NOT CONNECTED, req: %u", request->number);
				msgtxt
						= "PMS socket not connected. Cannot process request. Authentication aborted.";
				ekey = "SC_HSPGW_AUTH_PMS_DICONNECTED";
			} else if (pmssocket->shutdown == 1 || pmssocket->thread == 0) {
				msgtxt = "PMS socket state is going down. Cannot process request.";
				ekey = "SC_HSPGW_AUTH_RESOURCE_BUSY";
			}

			pms_fias_errLog(msgtxt, ekey, inst, logfiledata, request);
			continue;
		}

		//radlog(L_INFO,"Ready with socket tests. there should be one.");


		// if not authenticated yet, query the pms
		// Get PMS socket handle, send request, receive data
		// Request data from PMS server and verify password
		DEBUG2("connect with PMS Server");
		//authenticated = pms_fias_verify_password(inst, &trckrdata, pmsdata, &guestidlist);
		authenticated = pms_fias_verify_password(inst, &trckrdata, pmsdata,
				pmssocket);

		// SOCKET_ERROR
		if (authenticated < 0) {

			if (!free_access) {
				//  Free Access is not allowed for this location: Failure

				switch (authenticated) {
				/*            case -1:
				 msgtxt = "No socket found in socket pool. Authentication aborted.";
				 ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
				 break; */
				case -2:
					msgtxt = "Connection to PMS is lost (socket not connected). Authentication aborted.";
					ekey = "SC_HSPGW_AUTH_PMS_DICONNECTED";
					break;
				case -3:
					msgtxt = "Cannot lock send queue. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
					break;
				case -4:
					msgtxt = "Cannot unlock send queue. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
					break;
				case -5:
					msgtxt = "Cannot lock receive mutex. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
					break;
				case -6:
					msgtxt
							= "Got unexpected message from PMS. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_UNEXPECTED_MSG";
					break;
				case -7:
					msgtxt
							= "Could not receive response from PMS. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_TIMEOUT";
					break;
				default:
					msgtxt = "Unknown error. Authentication aborted";
					ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
					break;
				}

				pms_fias_errLog(msgtxt, ekey, inst, logfiledata, request);
				continue;
			}

			//  Free Access is allowed: Access accepted if deadline not expired
			else {
				DEBUG2(
							"The PMS is not reachable, but go-through is enabled with period = %d sec",
							pmsdata->go_through_period);

				if (pmsdata->go_through_deadline > 0
						&& pmsdata->go_through_deadline <= current_timestamp) {

					switch (authenticated) {
					case -1:
						msgtxt
								= "No socket found in socket pool and free-access deadline is exceeded. Authentication aborted.";
						ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
						break;
					case -2:
						msgtxt
								= "Connection to PMS is lost (socket not connected) and free-access deadline is exceeded. Authentication aborted.";
						ekey = "SC_HSPGW_AUTH_PMS_DICONNECTED";
						break;
					case -3:
						msgtxt
								= "Cannot lock send queue and free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
						break;
					case -4:
						msgtxt
								= "Cannot unlock send queue and free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
						break;
					case -5:
						msgtxt
								= "Cannot lock receive mutex and free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
						break;
					case -6:
						msgtxt
								= "Got unexpected message from PMS and free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_UNEXPECTED_MSG";
						break;
					case -7:
						msgtxt
								= "Could not receive response from PMSand free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_TIMEOUT";
						break;
					default:
						msgtxt
								= "Unknown error and free-access deadline is exceeded. Authentication aborted";
						ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
						break;
					}

					pms_fias_errLog(msgtxt, ekey, inst, logfiledata, request);
					continue;

				} else if (pmsdata->go_through_deadline > 0
						&& pmsdata->go_through_deadline > current_timestamp) {

					switch (authenticated) {
					case -1:
						msgtxt
								= "No socket found in socket pool but free-access with time limit is granted.";
						break;
					case -2:
						msgtxt
								= "Connection to PMS is lost (socket not connected) but free-access with time limit is granted.";
						break;
					case -3:
						msgtxt
								= "Cannot lock send queue but free-access with time limit is granted.";
						break;
					case -4:
						msgtxt
								= "Cannot unlock send queue but free-access with time limit is granted.";
						break;
					case -5:
						msgtxt
								= "Cannot lock receive mutex but free-access with time limit is granted.";
						break;
					case -6:
						msgtxt
								= "Got unexpected message from PMS but free-access with time limit is granted.";
						break;
					case -7:
						msgtxt
								= "Could not receive response from PMS but free-access with time limit is granted.";
						break;
					default:
						msgtxt
								= "Unknown error but free-access with time limit is granted.";
						break;
					}

				} else if (pmsdata->go_through_deadline == 0) {
					// No deadline has been set yet, therefore set it.
					int deadline = current_timestamp + pmsdata->go_through_period;
					ret = spp_set_gothrough_deadline(inst, deadline, pmsdata);
					//                ret = spp_set_gothrough_deadline(inst, request, deadline);
					if (ret < 0) {
						msgtxt = "Could not update HSRM_CONFIGURATION table. Authentication aborted.";
						pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR",
								inst, logfiledata, request);
						pairadd(&request->reply->vps, pairmake("Reply-Message",
								"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
						free(trckrdata);
						free(logfiledata);
						freepl(pmsdatalist);
						return RLM_MODULE_REJECT;
					}

					switch (authenticated) {
					case -1:
						msgtxt
								= "No socket found in socket pool. Time limited free-access will be granted.";
						break;
					case -2:
						msgtxt
								= "Connection to PMS is lost (socket not connected). Time limited free-access will be granted.";
						break;
					case -3:
						msgtxt
								= "Cannot lock send queue. Time limited free-access will be granted.";
						break;
					case -4:
						msgtxt
								= "Cannot unlock send queue. Time limited free-access will be granted.";
						break;
					case -5:
						msgtxt
								= "Cannot lock receive mutex. Time limited free-access will be granted.";
						break;
					case -6:
						msgtxt
								= "Got unexpected message from PMS. Time limited free-access will be granted.";
						break;
					case -7:
						msgtxt
								= "Could not receive response from PMS but free-access with time limit will be granted.";
						break;
					default:
						msgtxt = "Unknown error.Time limit will be granted.";
						break;
					}

				}

				trckrdata->deadline = current_timestamp + atoi(pmsdata->sesto);
				DEBUG2("rlm_pms::pms_authenticate: Session Deadline value: %d",
							trckrdata->deadline);
				ret = pms_fias_accTrackerInsert(inst, request, trckrdata); // not authenticated => guestidlist=NULL, trckrdata->guestd = ""
				// DB Failure
				if (ret < 0) {
					msgtxt = "Could not insert into access deadline ACCOUNT_TRACKER table. Authentication aborted.";
					pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
							logfiledata, request);
					pairadd(&request->reply->vps, pairmake("Reply-Message",
							"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
					free(trckrdata);
					free(logfiledata);
					freepl(pmsdatalist);
					return RLM_MODULE_REJECT;
				}

				// Settig Session-Timeout
				pms_fias_infoLog(msgtxt, "SC_HSPGW_AUTH_GRANTED_NOT_VERIFIED",
						inst, logfiledata, request);

				//radlog(L_INFO, "sesto after go through: %s", pmsdata->sesto);
				pairadd(&request->reply->vps, pairmake("Session-Timeout", pmsdata->sesto, T_OP_EQ));

				// hm/am, 20130319: add cisco account-info (cai)
				pairadd(&request->reply->vps, cai);

				free(trckrdata);
				free(pmsdata);
				free(logfiledata);
				return RLM_MODULE_OK;
			}
		} //if authenticated < 0

		//NON-SOCKET ERROR
		if (authenticated > 0) {
			switch (authenticated) {
			/*            case -1:
			 msgtxt = "No socket found in socket pool. Authentication aborted.";
			 ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
			 break; */
			case 1:
				msgtxt = "Unknown user. Authentication failed.";
				ekey = "SC_HSPGW_AUTH_INVALID_USER";
				break;
			case 2:
				msgtxt = "User cannot be granted due to no post flag. Authentication failed.";
				ekey = "SC_HSPGW_AUTH_NO_POST_ACTIVE";
				break;
			case 3:
				msgtxt = "Invalid password. Authentication failed.";
				ekey = "SC_HSPGW_AUTH_INVALID_PWD";
				break;
			default:
				msgtxt = "Unknown error. Authentication aborted";
				ekey = "SC_HSPGW_AUTH_INTERNAL_ERR";
				break;
			}

			pms_fias_errLog(msgtxt, ekey, inst, logfiledata, request);

			// manually triggered db swap: just set the ecot value on db to 888
			// the next request with invalid credetials will initiate it:
			if(pmsdata->ecot == 888) {
				radlog(L_INFO,"%s : req n/a : sid n/a : zid %s : usr n/a : %s",
						"SC_HSPGW_PMS_MANUAL_DB_SWAP", logfiledata->zoneid, "Manually triggered db swap.");
				pms_set_ecot(inst, pmsdata, time(NULL));
				sendDR(pmssocket);
			}

			continue;
		} //if authenticated > 0

		// If we came here, the password was verified by the PMS (authenticated == 0)
		trckrdata->deadline = current_timestamp + atoi(pmsdata->sesto);
		DEBUG2("rlm_pms_1::pms_authenticate: Session Deadline value: %d",
					trckrdata->deadline);
		ret = pms_fias_accTrackerInsert(inst, request, trckrdata);
		trackerId = pms_fias_accTrackerFind(inst, request, &deadline);
		if (trackerId < 0) {
			// DB Failure
			msgtxt = "Could not insert into ACCOUNT_TRACKER table. Authentication aborted.";
			pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst, logfiledata,
					request);
			pairadd(&request->reply->vps,
					pairmake("Reply-Message", "SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
			free(trckrdata);
			free(logfiledata);
			free(pmsdata);
			return RLM_MODULE_REJECT;
		}

		//Insert into occupancy table
		pms_fias_occupancyInsert(inst, trckrdata, pmsdata);

		// insert MAC address into HSPGW_ACCTR_RELOGIN
		if (autoLogin > 0){
			ret = pms_fias_accTrackerReloginInsert(inst, request, &trckrdata, trackerId, strMacAddress, strIpAddress, pmsdatalist->pmsdata->group, webproxy);
			if (ret < 0) {
				msgtxt = "Could not insert account data into ACCOUNT_TRACKER_RELOGIN table. Authentication aborted.";
				pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_ORA_ERROR", inst,
						logfiledata, request);
				pairadd(&request->reply->vps, pairmake("Reply-Message",
						"SC_HSPGW_AUTH_ORA_ERROR", T_OP_SET));
				free(trckrdata);
				free(logfiledata);
				freepl(pmsdatalist);
				return RLM_MODULE_REJECT;
			}

			occupancyId = pms_checkedin(inst, request, &trckrdata);

			//insert into OCCUPANCY_RELOGIN if zero tarif
			if (strcmp(pmsdatalist->pmsdata->amount, "0") == 0) {
				pms_fias_occupancyReloginInsert(inst, request, &trckrdata, occupancyId, strMacAddress, strIpAddress, pmsdatalist->pmsdata->group, webproxy);
			}
		}

		// Access accepted
		msgtxt = "Password verified by PMS. Authentication succeed.";
		pms_fias_infoLog(msgtxt, "SC_HSPGW_AUTH_BY_PMS", inst, logfiledata, request);

		// Settig Session-Timeout
		session_timeout = atoi(pmsdata->sesto)
				+ inst->config->pms_conf_sesto_threshold; // add threshold to sesto
		sprintf(strtimeout, "%d", session_timeout);

		//radlog(L_INFO, "sesto after pms auth: %s", strtimeout);

		pairadd(&request->reply->vps, pairmake("Session-Timeout", strtimeout, T_OP_EQ));
		// hm/am, 20130303: add cisco account-info (cai)
		pairadd(&request->reply->vps, cai);

		free(trckrdata);
		free(logfiledata);
		free(pmsdata);
		return RLM_MODULE_OK;

	} //end for


	//authenticate failed
	pairadd(&request->reply->vps, pairmake("Reply-Message", ekey, T_OP_SET));

	// Free memory
	free(trckrdata);
	free(logfiledata);
	freepl(pmsdatalist);

	return RLM_MODULE_REJECT;

}

/*
 *  Accounting for PMS module
 */

static int pms_accounting(void *instance, REQUEST *request) {

	VALUE_PAIR *vp;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	char querystr[MAX_QUERY_LEN];
	int ret;
	char *msgtxt = NULL;
	char *ekey = NULL;
	int trf = 0; // indicates a transfer to pms
	PMS_DATA_LIST *pmsdatalist = NULL;
	pms_data *pmsdata = NULL;
	pms_data *pmsdata_checkin = NULL;
	pms_tracker_data *trckrdata; // Tracker
	pms_logfile_data *logfiledata;
	int checked_by_pms = 0;
	PMS_SOCKET *pmssocket;
	//int rcode = 0;

	// Allocate memory for Tracker data
	trckrdata = rad_malloc(sizeof(pms_tracker_data));
	memset(trckrdata, 0, sizeof(pms_tracker_data));

	// Allocate memory for Logfile data
	logfiledata = rad_malloc(sizeof(pms_logfile_data));
	memset(logfiledata, '\0', sizeof(pms_logfile_data));

	DEBUG2("Entering PMS Accounting ...");
	DEBUG2("===========================");

	// initalize log data
	strcpy(logfiledata->sessid, "n/a");
	strcpy(logfiledata->tariffid, "n/a");
	strcpy(logfiledata->zoneid, "n/a");
	strcpy(logfiledata->username, "n/a");

	/*
	 * Verify the Acct-Session-Id, SCS-SPP-ZoneID and  SCS-SPP-StrippedUser
	 */

	// Extract Acct-Session-Id attribute
	DEBUG2("rlm_pms_1::pms_accouting: Checking required attributes...");
	vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID);

	strcpy(trckrdata->sessid, vp->vp_strvalue);
	strcpy(logfiledata->sessid, vp->vp_strvalue);

	// Check the SCS-SPP-ZoneID (it was already set in pms_authorize())
	vp = pairfind(request->config_items, PW_SCS_SPP_ZONEID);

	strcpy(trckrdata->zoneid, vp->vp_strvalue);
	strcpy(logfiledata->zoneid, vp->vp_strvalue);

	// Check the stripped user name (it was already set in pms_authorize())
	vp = pairfind(request->config_items, PW_SCS_SPP_STRIPPEDUSER);
	if (!vp) {
		msgtxt = "Missing SCS-SPP-StrippedUser attribute. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_INTERNAL_ERR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_fias_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_USER", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	strcpy(trckrdata->usr, vp->vp_strvalue);
	strcpy(logfiledata->username, vp->vp_strvalue);

	parse_repl(querystr, sizeof(querystr), inst->config->pms_config_query,
			request, sql_escape_func);

	// get pms data
	if (!get_pmsdatalist(&pmsdatalist, inst, logfiledata, querystr, "ACCT")) {
		msgtxt	= "Could not allocate PMS configuration data. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_ORA_ERROR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	// Build the PMS data
	pmsdata = pmsdatalist->pmsdata;

	// Check the struct
	if (pmsdata == NULL) {
		msgtxt = "Could not allocate PMS data. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_INTERNAL_ERR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_fias_errLog(msgtxt, "SC_HSPGW_ACCT_INVALID_USER", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	// Debug XXL!: this will print the selected node
	//    pms_printConfigData(pmsdata);

	// Check account_tracker table for existing transfer
	if (strcmp(pmsdata->tariff_key, PMS_TARIFF_PER_SESSION) == 0) {
		parse_repl(querystr, sizeof(querystr),
				inst->config->pms_acc_qry_by_sessid, request, sql_escape_func);
	} else {
		parse_repl(querystr, sizeof(querystr),
				inst->config->pms_acc_qry_by_usrloc, request, sql_escape_func);
	}
	DEBUG2("rlm_pms_1::pms_accounting: Tariff id is %d, type is %s.", pmsdata->tariffid, pmsdata->tariff_key);

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	DEBUG2("sql_get_socket %d at %s:%d", sqlsocket->id, __FILE__,__LINE__);
	if (sqlsocket == NULL) {
		msgtxt = "Connection to data base failed. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_SPPDB_CONN_ERR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		free(trckrdata);
		free(logfiledata);
		return RLM_MODULE_OK;
	}

	// Execute the select query
	// Get Configuration Data from database
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt = "Database select query failed. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_ORA_ERROR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_fias_errLog(msgtxt, "SC_HSPGW_ACCT_ORA_ERROR", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	// Fetch row
	ret = inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst);
	if (ret == -1) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		msgtxt
				= "Could not fetch row from account tracker query. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_ORA_ERROR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		//pms_fias_errLog(msgtxt, "SC_HSPGW_ACCT_ORA_ERROR", inst, logfiledata, NULL);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	row = sqlsocket->row;
	if (row == NULL) {
		// user was billed already --> just ok
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		DEBUG("User was billed already. Silently discarding accounting process.");
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	//DEBUG2("rlm_pms_1::pms_accounting: account tracker query delivered %d, %s, %s, %s", ret, row[0], row[1], row[2]);

	// Add to Tracker data
	trckrdata->billed = 0;
	trckrdata->next = NULL;
	// ACCOUNT_TRACKER_DEADLINE, ACCOUNT_TRACKER_PWD, ACCOUNT_TRACKER_GUEST_ID
	trckrdata->deadline = atoi(row[0]);
	strcpy(trckrdata->pwd, row[1]);
	// Guest-ID can be null in case the pms wasn't reachable but is configured as go_through
	if (row[2]) {
		strcpy(trckrdata->guestid, row[2]);
	}
	strcpy(trckrdata->zoneid, row[3]);
	strcpy(logfiledata->zoneid, trckrdata->zoneid);

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	radlog(
			L_INFO,
			"SC_HSPG_ACCT_INCOMING_REQ : sid %s : zid %s : Processing incoming Accounting-Request from client <%s>",
			logfiledata->sessid, logfiledata->zoneid,
			request->client->shortname);

	// checkin zone may be different from login zone.
	// we have to find the right PMS entry here!
	pmsdata_checkin = NULL;
	while (pmsdatalist != NULL) {
		DEBUG2("pms zone id='%s', tracker zone id='%s'", pmsdatalist->pmsdata->zoneid, trckrdata->zoneid);
		if (strcmp (pmsdatalist->pmsdata->zoneid, trckrdata->zoneid) == 0) {
			// found!
			pmsdata_checkin = pmsdatalist->pmsdata;
			DEBUG2("found checkin zone id %s", pmsdata_checkin->zoneid);
			break;
		}
		pmsdatalist = pmsdatalist->next;
	}

	if (pmsdata_checkin == NULL ) {
		msgtxt = "No matching configuration returned. Accounting aborted.";
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
				"SC_HSPGW_ACCT_ORA_ERROR", logfiledata->sessid,
				logfiledata->zoneid, logfiledata->username, msgtxt);
		free(trckrdata);
		free(logfiledata);
		freepl(pmsdatalist);
		return RLM_MODULE_OK;
	}

	strcpy(logfiledata->hsrmid, pmsdata_checkin->hsrmid);

	// only amounts greater than 0 will be transfered:
	msgtxt = "";
	if (atoi(pmsdata_checkin->amount) > 0) {
		// Send PR-Posting to PMS
		DEBUG(
					"rlm_pms_1::pms_accounting: posting amount of CHF %s to PMS %s:%s...",
					pmsdata_checkin->amount, pmsdata_checkin->ipaddr, pmsdata_checkin->port);

		// we have two nodes. set this node as active
		if (strcmp(inst->hostname, pmsdata_checkin->active_gw)) {

			if (!upd_active_gw(inst, request, pmsdata_checkin)) {
				msgtxt
						= "Update of active GW to data base failed. Accounting aborted.";
				radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",
						"SC_HSPGW_ACCT_ORA_ERROR", request->number,
						logfiledata->sessid, logfiledata->zoneid,
						logfiledata->username, msgtxt);
				//pms_fias_errLog(msgtxt, "SC_HSPGW_ACCT_ORA_ERROR", inst, logfiledata, request);
				free(trckrdata);
				free(logfiledata);
				freepl(pmsdatalist);
				return RLM_MODULE_OK;
			}
		}

		//// moved block start
		//radlog(L_INFO,"get socket from pool with request %u.", request->number);
		// get socket from pool
		pmssocket = pms_fias_find_SocketData(inst->datapool->socketpool,
				pmsdata_checkin->ipaddr, pmsdata_checkin->port);

		if (pmssocket == NULL) {
			DEBUG2(
						"rlm_pms_1::pms_accounting : find new IP %s (new PMS?), update the socket pool",
						pmsdata_checkin->ipaddr);

			radlog(
					L_INFO,
					"acct: create new socket and start new thread with request %u.",
					request->number);

			pmssocket = pms_fias_insertSocketPool(inst, pmsdata_checkin);
			changeSockState(pmssocket, pending, pmssocket->shutdown);
			//start separate thread to handle incoming messages
			pms_fias_startthread(inst, pmssocket, pmsdata_checkin);
			//wait a little until we send the PR request (up to 3sec cz of the init sequence)
			sleep(PMS_INIT_TIME);
			if ((pmssocket->state == pmssockconnected) && is_obsolete(pmsdata_checkin->ecot)) {
				sendDR(pmssocket); // db synch
			}
		} else {

			if (pmssocket->state == pmssockunconnected) {
				if (pmssocket->thread != 0) {
					radlog(
							L_INFO,
							"acct: there is a unconnected socket inclusive thread. notify thread to restart with request %u.",
							request->number);
					//set signal to restart
					changeSockState(pmssocket, pending, pmssocket->shutdown);
					pms_fias_notifyThreadRestart(pmssocket);
					//radlog(L_INFO,"acct: thread should have restarted with request %u.", request->number);
				} else {
					radlog(
							L_INFO,
							"acct: there is a unconnected socket without thread. create a new thread with request %u.",
							request->number);
					changeSockState(pmssocket, pending, 0);
					pms_fias_startthread(inst, pmssocket, pmsdata_checkin);
				}

				//wait a little until we send the PR request (up to 3sec cz of the init sequence)
				sleep(PMS_INIT_TIME);
				if ((pmssocket->state == pmssockconnected) && is_obsolete(pmsdata_checkin->ecot)) {
					sendDR(pmssocket); // db synch
				}
			}
		}
		///// moved block end

		// in this case there was no PR request yet. According to FIAS there must be a preceding PR inquiry
		// before posting usage fee to PMS. So do it, although it is not necessary:

		// last socket test here before it is used
		if (pmssocket == NULL || pmssocket->state != pmssockconnected
				|| pmssocket->thread == 0 || pmssocket->shutdown == 1) {

			if (pmssocket == NULL) {
				msgtxt = "PMS socket not available. Cannot process request. Accounting aborted.";
				ekey = "SC_HSPGW_ACCT_RESOURCE_BUSY";
			} else if (pmssocket->state == pending) {
				//radlog(L_INFO, "SOCKET IS PENDING, req: %u", request->number);
				msgtxt = "PMS socket not connected: SOCKET IS PENDING. Cannot process request. Accounting aborted.";
				ekey = "SC_HSPGW_ACCT_RESOURCE_BUSY";
			} else if (pmssocket->state == pmssockunconnected) {
				//radlog(L_INFO, "SOCKET IS NOT CONNECTED, req: %u", request->number);
				msgtxt = "PMS socket not connected. Cannot process request. Accounting aborted.";
				ekey = "SC_HSPGW_ACCT_PMS_DICONNECTED";
			} else if (pmssocket->shutdown == 1 || pmssocket->thread == 0) {
				msgtxt = "PMS socket state is going down. Cannot process request. Accounting aborted.";
				ekey = "SC_HSPGW_ACCT_RESOURCE_BUSY";
			}

			radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s", ekey,
					request->number, logfiledata->sessid, logfiledata->zoneid,
					logfiledata->username, msgtxt);
			free(trckrdata);
			freepl(pmsdatalist);
			free(logfiledata);
			return RLM_MODULE_OK;
		}

		checked_by_pms = pms_fias_verify_password(inst, &trckrdata, pmsdata_checkin,
				pmssocket);

		if (checked_by_pms != 0) {

			switch (checked_by_pms) {
			case -1:
				msgtxt = "No socket found in socket pool. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
				break;
			case -2:
				msgtxt
						= "Connection to PMS is lost (socket not connected). Accounting aborted";
				ekey = "SC_HSPGW_ACCT_PMS_DICONNECTED";
				break;
			case -3:
				msgtxt = "Cannot lock send queue. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
				break;
			case -4:
				msgtxt = "Cannot unlock send queue. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
				break;
			case -5:
				msgtxt = "Cannot lock receive mutex. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
				break;
			case -6:
				msgtxt = "Got unexpected message from PMS. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_UNEXPECTED_MSG";
				break;
			case -7:
				msgtxt
						= "Could not receive response from PMS. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_TIMEOUT";
				break;
			default:
				msgtxt = "Not authenticated. Accounting aborted";
				ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
				break;
			}
			radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s", ekey,
					request->number, logfiledata->sessid, logfiledata->zoneid,
					logfiledata->username, msgtxt);

			free(trckrdata);
			freepl(pmsdatalist);
			free(logfiledata);
			return RLM_MODULE_OK;
		}

		//update account tracker data since it could be NULL (PMS not reachable during authentication)
		ret = pms_fias_accUpdateGid(inst, request, trckrdata);
		// added by HJ on 26 August, need to be review
		if (ret < 0) {
			msgtxt
					= "Could not update guest-id in HSPGW_ACCOUNT_TRACKER table. Accounting aborted.";
			radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",
					"SC_HSPGW_ACCT_ORA_ERROR", request->number,
					logfiledata->sessid, logfiledata->zoneid,
					logfiledata->username, msgtxt);
			free(trckrdata);
			freepl(pmsdatalist);
			free(logfiledata);
			return RLM_MODULE_OK;
		}

		// now we can post it
		ret = pms_fias_proc_PR_posting(inst, &trckrdata, pmsdata_checkin, pmssocket);

		switch (ret) {
		case 1:
			trf = 1;
			msgtxt = "Successfully posted usage fee to PMS.";
			pms_fias_infoLog(msgtxt, "SC_HSPGW_ACCT_TRANSMITTED", inst,
					logfiledata, request);
			break;
		case 0:
			msgtxt = "Posting of usage fee rejected by PMS. Accounting failed.";
			ekey = "SC_HSPGW_ACCT_REJECTED";
			break;
		case -1:
			msgtxt
					= "No socket found in socket pool while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
			break;
		case -2:
			msgtxt
					= "Connection to PMS is lost (socket not connected) while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_PMS_DICONNECTED";
			break;
		case -3:
			msgtxt
					= "Cannot lock send queue for posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
			break;
		case -4:
			msgtxt
					= "Cannot unlock send queue while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
			break;
		case -5:
			msgtxt
					= "Cannot lock receive mutex  while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
			break;
		case -6:
			msgtxt
					= "Got unexpected response from PMS while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_UNEXPECTED_MSG";
			break;
		case -7:
			msgtxt
					= "Could not receive response from PMS while posting usage fee. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_TIMEOUT";
			break;
		default:
			// edited by HJ on 26 August
			msgtxt = "Unknown error. Accounting aborted";
			ekey = "SC_HSPGW_ACCT_INTERNAL_ERR";
			break;
		}

		if (ret <= 0) {
			pms_fias_errLog(msgtxt, ekey, inst, logfiledata, request);
			// Free memory
			free(trckrdata);
			free(logfiledata);
			freepl(pmsdatalist);
			return RLM_MODULE_OK;

		}
	} else {
		msgtxt = "Usage fee is CHF 0.00. Treat it as billed.";
		pms_fias_infoLog(msgtxt, "SC_HSPGW_ACCT_TRANSMITTED", inst,
				logfiledata, request);
		DEBUG(
					"rlm_pms_1::pms_accounting: charge amount is CHF 0.00. Treat it as billed.");
		trf = 1;
	}

	// Update the billed flag in the account tracker table
	if (trf) {
		DEBUG("rlm_pms_1::pms_accounting: setting billed-flag ...");

		pms_fias_accUpdBilled(inst, request, pmsdata_checkin);
		//todo: IF UPDATE FAILS, WE HAVE A PROBLEM
		// cz next accounting request will bill the guest again
		// possible solution: set the flag first. if the posting fails, the we can roll back the flag
		DEBUG("rlm_pms_1::pms_accounting: flag set.");
	}

	// Free memory
	free(trckrdata);
	free(logfiledata);
	freepl(pmsdatalist);
	return RLM_MODULE_OK;
}

static int sendDR(PMS_SOCKET *pmssocket) {
	time_t now;
	struct tm *tm_now;
	char dbuff[BUFSIZ];
	char tbuff[BUFSIZ];
	char message[MAX_FIAS_BUFFER_SIZE];
	//int i;
	now = time(NULL);
	tm_now = localtime(&now);
	strftime(dbuff, sizeof dbuff, "%y%m%d", tm_now);
	strftime(tbuff, sizeof tbuff, "%H%M%S", tm_now);

	// Format DR message
	sprintf(message, PROTEL_FIAS_DR, dbuff, tbuff);

	//i =
	return (pms_fias_send_message(pmssocket, message));
	//radlog(L_INFO, "return value is %u", i);
}

/*************************************************************************
 *
 *    Function: pms_verify_password
 *
 *    Purpose: Request user data from Protel PMS and verify it against user request
 *  @ PMS_SOCKET *pmssocket: PMS Socket
 *  @ pms_tracker_data *tracker: Account Tracker data
 *  Return -2 on timeout, -1 on failure, 1 on user not found, 2 on no posting flag, 3 on invalid password,  0 on valid password
 *  If return 0, the guestId is overwritten
 *  Author: Juan Vasquez
 *  Last changed: 13.01.2010
 *************************************************************************/
static int pms_fias_verify_password(void *instance, pms_tracker_data **tracker,
		pms_data *pmsdata, PMS_SOCKET *pmssocket) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	time_t now;
	struct tm *tm_now;
	char dbuff[BUFSIZ];
	char tbuff[BUFSIZ];
	char message[MAX_FIAS_BUFFER_SIZE];

	pms_tracker_data *trckr = *tracker;
	int valid_passwd, seqnr, ret;
	PMS_FIAS_PACKET *packet = NULL;
	PMS_FIAS_GUESTLIST *guestlist;
	int i = 0;
	int n = 0;
	char guestnames[10][PMS_PASSWORD_MAX_LEN + 1];

//	radlog(L_INFO, "pms_fias_verify_password (%s : %s): pusrfld: '%s', tusr: '%s', tpw: '%s'", pmsdata->ipaddr, pmsdata->port, pmsdata->usrfld, trckr->usr, trckr->pwd);

	now = time(NULL);
	tm_now = localtime(&now);
	strftime(dbuff, sizeof dbuff, "%y%m%d", tm_now);
	strftime(tbuff, sizeof tbuff, "%H%M%S", tm_now);

	// Get new sequence number
	seqnr = pms_fias_getSeqNr(pmssocket->seqnumber);
	pmssocket->seqnumber = seqnr;

	// Format PR message "PR|P#%d|PI%s|WS%s|DA%s|TI%s|"
	sprintf(message, PROTEL_FIAS_PR_RN, seqnr, trckr->usr, inst->hostname,
			dbuff, tbuff);

	// Send PR message
	DEBUG3("pms_fias::pms_verify_password: Send packet type 'Posting Request'");

	ret = pms_fias_send_message(pmssocket, message);
	if (ret < 0) {
		DEBUG3("pms_fias::pms_fias_verify_password: Could not send message");
		return ret;
	}

	// 'Posting Answer' or 'Posting List' message is expected from PMS server after sending PR message
	ret = pms_fias_get_message(pmssocket, message, seqnr, NULL);
	if (ret < 0) {
		DEBUG3("pms_fias::pms_fias_verify_password: Could not get message");
		return ret;
	}
	radlog(L_INFO, "SC_HSPGW_PMS_RCV_MSG : sid %s : zid %s : pms %s:%s : %s",
			trckr->sessid, trckr->zoneid, pmsdata->ipaddr, pmsdata->port, message);

	// just for test purposes
	// packet = pms_fias_splitPacket(message);
	// pms_fias_validatePacket(message, packet);

	if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PA) < 0)
			&& (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PL) < 0)) {
		DEBUG3("pms_fias::pms_verify_password: Unexpected message record type.");
		return -6;
	}

	DEBUG("pms_fias::pms_verify_password: Message record type is correct.");
	packet = pms_fias_splitPacket(message);
	if (pms_fias_validatePacket(message, packet) < 0) {
		DEBUG3("pms_fias_verify_password: invalid PMS message.");
		free(packet);
		return -7;
	}

	// Negative case: room is unoccupied or unknown
	if (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PA) == 0
			|| packet->guestlist->guestname == NULL) {
		//radlog(L_ERR,"pms_fias : %s : Protel PMS server response: '%s'.", "SC_HSPGW_AUTH_INVALID_USER", packet->cleartxt);
		free(packet);
		return 1;
	}

	if (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PL) == 0
			&& packet->nopost == 1) {
		//radlog(L_ERR,"pms_fias : %s : Protel PMS server response: '%s'.", "SC_HSPGW_AUTH_INVALID_USER", packet->cleartxt);
		free(packet);
		return 2;
	}

	valid_passwd = 0;
	guestlist = packet->guestlist;

	while (guestlist && !valid_passwd) {
		// (for comparison with pwd)
		DEBUG3("pms_fias_verify_password: guestname=%s,roomnumber=%s",
			guestlist->guestname, guestlist->roomnumber);

		// hm/24.04.2014
		n = convertToGuestlist (guestnames, guestlist->guestname, guestname_delimiter);
		if( n > 0 ){
			for (i=0; i<n; i++) {
				if (pms_fias_findPassword(guestnames[i], trckr->pwd, 1)
						&& pms_fias_findPassword(guestlist->roomnumber, trckr->usr, 1)) {
					valid_passwd = 1;
					// Copy guestid to pms_tracker_data
					strcpy(trckr->guestid, guestlist->guestid);
					convert2ascii(trckr->pwd2, sizeof(trckr->pwd2), guestlist->guestname2);
//					strcpy(trckr->pwd2, guestlist->guestname2);
				}
			}
		}
		else{
			if (pms_fias_findPassword(guestlist->guestname, trckr->pwd, 1)
							&& pms_fias_findPassword(guestlist->roomnumber, trckr->usr, 1)) {
						valid_passwd = 1;
						// Copy guestid to pms_tracker_data
						strcpy(trckr->guestid, guestlist->guestid);
						convert2ascii(trckr->pwd2, sizeof(trckr->pwd2), guestlist->guestname2);
//						strcpy(trckr->pwd2, guestlist->guestname2);
			}
		}

		/*
		if (pms_fias_findPassword(guestlist->guestname, trckr->pwd, 1)
				&& pms_fias_findPassword(guestlist->roomnumber, trckr->usr, 1)) {
			valid_passwd = 1;
			// Copy guestid to pms_tracker_data
			strcpy(trckr->guestid, guestlist->guestid);
			strcpy(trckr->pwd2, guestlist->guestname2);



		}*/
		// hm end


		guestlist = guestlist->next;
	}
	//    }

	// Free memory
	free(packet);

	// 3: invalid password
	if (!valid_passwd) {
		DEBUG3("pms_fias_verify_password: invalid pw");
		return 3;
	}

	//0: valid password
	DEBUG3("pms_fias_verify_password: valid pw");
	return 0;

}

/*************************************************************************
 *
 *    Function: pms_proc_PR_posting
 *
 *    Purpose: posts a total amount to pms
 *  @ pms_tracker_data *tracker: Account Tracker data
 *  Return -2 on timeout, -1 on failure, 0 on pms rejection, 1 on billing success
 *
 *  Author: Hagen Muench
 *  Last changed: 18.01.2010
 *************************************************************************/
static int pms_fias_proc_PR_posting(void *instance, pms_tracker_data **tracker,
		pms_data *pmsdata, PMS_SOCKET *pmssocket) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	time_t now;
	struct tm *tm_now;
	char dbuff[BUFSIZ];
	char tbuff[BUFSIZ];
	pms_tracker_data *trckr = *tracker;
	char message[MAX_FIAS_BUFFER_SIZE];
	PMS_FIAS_PACKET *packet = NULL;
	char *tok;
	int seqnr = 0;
	int ret;
	char ctmsgs[1024];

	// Get socket connection to PMS server and also lock the socket
	DEBUG2(
				"pms_fias::pms_proc_PR_posting: Get socket connection to PMS server with SCS-SPP-ZoneID %s.",
				pmsdata->zoneid);

	now = time(NULL);
	tm_now = localtime(&now);
	strftime(dbuff, sizeof dbuff, "%y%m%d", tm_now);
	strftime(tbuff, sizeof tbuff, "%H%M%S", tm_now);

	// Get new sequence number
	seqnr = pms_fias_getSeqNr(pmssocket->seqnumber);
	pmssocket->seqnumber = seqnr;
	// Format PR message PROTEL_FIAS_PR_POS     "PR|P#%d|RN%s|GN%s|G#%s|TA%s|PTC|CT%s|WS%s|DA%s|TI%s|"
	//    sprintf(message, PROTEL_FIAS_PR_POS, seqnr, trckr->usr, trckr->guestid, pmsdata->amount, "PWLAN usage",dbuff, tbuff);
	sprintf(message, PROTEL_FIAS_PR_POS, seqnr, trckr->usr, trckr->pwd2,
			trckr->guestid, pmsdata->amount, "PWLAN usage", inst->hostname,
			dbuff, tbuff);

	ret = pms_fias_send_message(pmssocket, message);
	if (ret < 0) {
		//radlog(L_ERR,"pms_fias : %s : Socket error by send message.", "SC_HSPGW_SOCKET_ERR");
		return ret;
	}

	// 'Posting Answer' or 'Posting List' message is expected from PMS server after sending PR message
	ret = get_posting_answer(pmssocket, message, seqnr, NULL);
	if (ret < 0) {
		//        radlog(L_ERR,"pms_fias : %s : Socket error by get message.", "SC_HSPGW_SOCKET_ERR");
		return ret;
	}
	radlog(L_INFO, "SC_HSPGW_PMS_RCV_MSG : sid %s : zid %s :pms %s:%s : %s",
			trckr->sessid, trckr->zoneid, pmsdata->ipaddr, pmsdata->port, message);

	/*
	 // Send PR message
	 DEBUG("pms_fias::pms_proc_PR_posting: Send packet type 'PR Posting'");
	 if (pms_fias_send_message(pmssocket, message) < 0) {
	 radlog(L_ERR,"pms_fias : %s : Socket error.", "SC_HSPGW_SOCKET_ERR");
	 return -1;
	 }

	 // 'Posting Answer' or 'Posting List' message is expected from PMS server after sending PR message
	 if (pms_fias_get_message(pmssocket, message, seqnr, NULL) < 0) {
	 radlog(L_ERR,"pms_fias : %s : Socket error.", "SC_HSPGW_SOCKET_ERR");
	 return -1;
	 }
	 */

	// Check RecordID LA
	if (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PA) < 0) {
		DEBUG2("pms_fias::pms_proc_PR_posting: Unexpected message record type.");
		// Here store the packet somewhere???
		return -6;
	}
	// Got expected value
	DEBUG("pms_fias::pms_proc_PR_posting: Message record type is correct.");
	// We need to parse the packet
	DEBUG2("pms_fias::pms_proc_PR_posting: Extracting data from message.");
	packet = pms_fias_splitPacket(message);
	if (pms_fias_validatePacket(message, packet) < 0) {
		DEBUG2("pms_proc_PR_posting: invalid PMS message.");
		free(packet);
		return -7;
	}

	// For postings we get always a PA message
	// Positive case: message contains "ASOK|CTGuest transfer accepted"
	// check me: cleartext
	// ugly workaround: the two pms protel and fidelio have different behaviour:
	// protel msg contains "ASOK|CTGuest transfer accepted"
	// fidelio msg contains "ASOK|CTPWLAN usage"
	//if(!strcmp(packet->answstatus, PROTEL_FIAS_AS_OK) && (!strcmp(packet->cleartxt, "Guest transfer accepted") || !strcmp(packet->cleartxt, "PWLAN usage")))
	// pms_1 specific
	if (!strcmp(packet->answstatus, PROTEL_FIAS_AS_OK)) {
		strcpy(ctmsgs, inst->config->pms_conf_asok_msgs);

		for (tok = strtok(ctmsgs, inst->config->pms_conf_ctmsg_del); tok; tok
				= strtok(NULL, inst->config->pms_conf_ctmsg_del)) {
			if (!strncmp(packet->cleartxt, tok, strlen(tok))) {
				DEBUG(
							"pms_fias::pms_proc_PR_posting: PR Posting successful: Amount: %s, Room: %s, Guest-Id: %s",
							pmsdata->amount, trckr->usr, trckr->guestid);
				return 1;
			}
		}
		//DEBUG("pms_fias::pms_proc_PR_posting: PR Posting successfull: Amount: %s, Room: %s, Guest-Id: %s", pmsdata->amount, trckr->usr, trckr->guestid);
		//return 1;
	}

	DEBUG(
				"pms_fias : %s : %s : PR Posting rejected by PMS: Room: %s, Guest-Id: %s, AS: %s, Msg: %s",
				trckr->sessid, trckr->zoneid, trckr->usr, trckr->guestid,
				packet->answstatus, packet->cleartxt);

	free(packet);
	return 0;

}

/*************************************************************************
 *
 *    Function: pms_fias_accTrackerFind
 *
 *    Purpose: Internal function to find a row from PMS_ACCOUNT_TRACKER table
 *  @ void *instance: the module instance
 *  @REQUEST *request: the module request
 *  @PMS_DATA_LIST *pmsdatalist
 *  Return -1 on failure, 0 on not found, HAT_TRACKER_ID on found
 *
 *  Author: Juan Vasquez
 *  Last changed: 10.11.2009
 *************************************************************************/
static int pms_fias_accTrackerFind(void *instance, REQUEST *request, int *deadline) {
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	VALUE_PAIR *vp;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char selectquery[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];
	int trackerId = 0;
	int ret;

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		//radlog(L_ERR, "rlm_pms_1 : %s : Connection to data base failed.", "SC_HSPGW_AUTH_SPPDB_CONN_ERR");
		return -1;
	}

	//  find decoded passwd
	vp = pairfind(request->config_items, PW_SCS_SPP_DECODEDPW);
	if (!vp) {
		//radlog(L_ERR, "rlm_pms : %s : %s : Missing SCS-SPP-DecodedPassword attribute.", logfiledata->sessid, logfiledata->locationid);
		//radlog(L_ERR, "rlm_pms_1 : Missing SCS-SPP-DecodedPassword attribute.");
		return -2;
	}
	// Replace placeholders in querystr pms_config_query
	dupquotes(modstr, vp->vp_strvalue);
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_query,
			request, sql_escape_func);
	sprintf(selectquery, querystr, modstr);
	DEBUG2("expand: %s", selectquery);

	// Execute the select
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, selectquery)) {
		//radlog(L_ERR, "rlm_pms_1 : %s : Database select from query failed", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == -1) {
		//radlog(L_ERR, "rlm_pms_1 : %s : Could not fetch row from select query.", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	row = sqlsocket->row;
	if (row == NULL) {

		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}

	// Found row, return the PMS_ACCOUNT_TRACKER_DEADLINE value
	// row[0] : HAT_TRACKER_DEADLINE
	// row[1] : HAT_TRACKER_BILLED
	// row[2] : HAT_TRACKER_ID

	*deadline = atoi(row[0]);
	trackerId = atoi(row[2]);
	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return trackerId;
}

/*************************************************************************
 *
 *    Function: pms_checkedin
 *
 *    Purpose: Internal function to find a row in table HSPGW_OCCUPANCY
 *  @ void *instance: the module instance
 *  @REQUEST *request: the module request
 *  @pms_data *pmsdata: includes the hspgw_config-ID
 *  @pms_tracker_data **tracker:
 *  Return -1 on failure, 0 on not found,HSPGW_OCCUPANCY.HHO_OID  on found
 *
 *  Author: hm/wdw
 *  Last changed: 2011-07-18 - created
 *************************************************************************/
static int pms_checkedin(void *instance, REQUEST *request, pms_tracker_data **tracker) {

	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	rlm_pms_1_module_t *inst = instance;
	pms_tracker_data *trckr = *tracker;
	VALUE_PAIR *vp;
	char querystr[MAX_QUERY_LEN];
	char selectquery[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];
	char etwas[PMS_PASSWORD_MAX_LEN + 1];
	int ret;
	int occupancyId = 0;


	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		//radlog(L_ERR, "rlm_pms_1 : %s : Connection to data base failed.", "SC_HSPGW_AUTH_SPPDB_CONN_ERR");
		return -1;
	}


	//  find decoded passwd
	vp = pairfind(request->config_items, PW_SCS_SPP_DECODEDPW);
	/*    if (!vp){
	 //radlog(L_ERR, "rlm_pms : %s : %s : Missing SCS-SPP-DecodedPassword attribute.", logfiledata->sessid, logfiledata->locationid);
	 //radlog(L_ERR, "rlm_pms_1 : %s : Missing SCS-SPP-DecodedPassword attribute.", "SC_HSPGW_ACCT_MALFORMED_REQ");
	 return -2;
	 }n */
	strcpy(etwas,"%");
	strcat(etwas, vp->vp_strvalue);
	strncat(etwas, "%", 1);

	// Replace placeholders in querystr pms_occup_by_gnrn
//	dupquotes(modstr, vp->vp_strvalue);
	dupquotes(modstr, etwas);

	parse_repl(querystr, sizeof(querystr), inst->config->pms_occup_by_gnrn,
			request, sql_escape_func);
	// fill parameters in querystr
	sprintf(selectquery, querystr, modstr);
	DEBUG2("expand: %s", selectquery);

	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst,
			selectquery)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Database select from query failed", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	ret = inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst);
	if (ret == -1) {
		radlog(L_ERR, "rlm_pms_1 : %s : Could not fetch row from select query.", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	row = sqlsocket->row;
	if (row == NULL) {
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}

	occupancyId = atoi(row[1]);
	//strcpy(trckr->usr, row[2]);
	//strcpy(trckr->pwd, row[3]);
	strcpy(trckr->guestid, row[4]);
	int currenttime = time(NULL);
	trckr->deadline = currenttime + atoi(row[8]);
	strcpy(trckr->zoneid, row[6]);
	strcpy(trckr->tariff_key, row[7]);
	strcpy(trckr->sesto, row[8]);
	trckr->tariffid = atoi(row[9]);

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return occupancyId;
}


/*************************************************************************
 *
 *    Function: dupquotes
 *
 *    Purpose: Internal function to replace one single quotation marks by
 *             two single quotation marks
 *
 *
 *      Author: hm/wdw
 *      Last changed: 14.03.2011
 *************************************************************************/
static void dupquotes(char *strout, char *strin) {
	int l = strlen(strin);
	int i;
	int j = 0;

	for (i = 0; i < l; i++) {
		if (strin[i] == '\'') {
			strout[j] = '\'';
			j++;
			strout[j] = '\'';
		} else {
			strout[j] = strin[i];
		}
		j++;
	}
	strout[j] = '\0';
	return;
}

/*************************************************************************
 *
 *    Function: pms_fias_accTrackerReloginInsert
 *
 *    Purpose: Inserts into table HSPGW_ACCTR_RELOGIN if no entry exists
 *
 *    Return -1 on failure, 0 on success
 *
 *************************************************************************/
static int pms_fias_accTrackerReloginInsert(void *instance, REQUEST *request, pms_tracker_data *trckrdata, int trackerId, char* mac, char* ip, int group, int webproxy) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char selectquery[MAX_QUERY_LEN];
	char insertquery[MAX_QUERY_LEN];
	SQL_ROW row;
	int ret;
	int found;

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Connection to data base failed.",
				"SC_HSPGW_AUTH_SPPDB_CONN_ERR", trckrdata->sessid,
				trckrdata->zoneid);
		return -1;
	}

	// check if record already exists
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_relogin_query,
			request, sql_escape_func);
	sprintf(selectquery, querystr, trackerId, mac);
	DEBUG2("expand: %s", selectquery);
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst,selectquery)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Database select from query failed", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	ret = inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst);
	if (ret == -1) {
		radlog(L_ERR, "rlm_pms_1 : %s : Could not fetch row from select query.", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	row = sqlsocket->row;
	found = (row != NULL); // need only to know if a record exists
	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	if (found) {
		return 0;
	}

	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_relogin_insert,
			request, sql_escape_func);

	sprintf(insertquery, querystr, trackerId, mac, ip, group, webproxy);
	DEBUG2("expand: %s", insertquery);

	// Execute the insert query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, insertquery)) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Insertion of account tracker relogin data failed.",
				"SC_HSPGW_AUTH_ORA_ERROR", trckrdata->sessid,
				trckrdata->zoneid);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_fias_occupancyReloginInsert
 *
 *    Purpose: Inserts into table HSPGW_OCCUPANCY_RELOGIN if no entry exists
 *
 *    Return -1 on failure, 0 on success
 *
 *************************************************************************/
static int pms_fias_occupancyReloginInsert(void *instance, REQUEST *request, pms_tracker_data *trckrdata, int occupancyId, char *mac, char *ip, int group, int webproxy) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char selectquery[MAX_QUERY_LEN];
	char insertquery[MAX_QUERY_LEN];
	pms_tracker_data *trckr = trckrdata; // Tracker
	char modstr[PMS_PASSWORD_MAX_LEN + 1];	
	SQL_ROW row;
	int ret;
	int found;
	

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Connection to data base failed.",
				"SC_HSPGW_AUTH_SPPDB_CONN_ERR", trckrdata->sessid,
				trckrdata->zoneid);
		return -1;
	}
	
	// check if record already exists
	parse_repl(querystr, sizeof(querystr), inst->config->pms_occupancy_relogin_query,
			request, sql_escape_func);
	sprintf(selectquery, querystr, occupancyId, mac);
	DEBUG2("expand: %s", selectquery);
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst,selectquery)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Database select from query failed", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	ret = inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst);
	if (ret == -1) {
		radlog(L_ERR, "rlm_pms_1 : %s : Could not fetch row from select query.", "SC_HSPGW_AUTH_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	row = sqlsocket->row;
	found = (row != NULL); // need only to know if a record exists
	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	if (found) {
		return 0;
	}

	// Replace placeholders in querystr pms_acctrack_insert
	parse_repl(querystr, sizeof(querystr), inst->config->pms_occupancy_relogin_insert,
			request, sql_escape_func);

	sprintf(insertquery, querystr, occupancyId, mac, ip, group, webproxy);
	DEBUG2("expand: %s", insertquery);

	// Execute the insert query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, insertquery)) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Insertion of occupancy relogin data failed.",
				"SC_HSPGW_AUTH_ORA_ERROR", trckrdata->sessid,
				trckrdata->zoneid);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}


/*************************************************************************
 *
 *    Function: pms_accTrackerInsert
 *
 *    Purpose: Internal function to insert new rows into PMS_ACCOUNT_TRACKER table
 *  @ void *instance: the module instance
 *  @ REQUEST *request: the module request
 *  @ pms_tracker_data *trckrdata: the tracker data
 *  @ pms_data *pmsdata
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 13.01.2010
 *************************************************************************/
static int pms_fias_accTrackerInsert(void *instance, REQUEST *request,
		pms_tracker_data *trckrdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char insertquery[MAX_QUERY_LEN];
	pms_tracker_data *trckr = trckrdata; // Tracker
	char modstr[PMS_PASSWORD_MAX_LEN + 1];

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Connection to data base failed.",
				"SC_HSPGW_AUTH_SPPDB_CONN_ERR", trckrdata->sessid,
				trckrdata->zoneid);
		return -1;
	}

	// Replace placeholders in querystr pms_acctrack_insert
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_insert,
			request, sql_escape_func);

	dupquotes(modstr, trckr->pwd);
	sprintf(insertquery, querystr, atoi(trckr->zoneid), trckr->usr, modstr, trckr->deadline,
			trckr->guestid, trckr->tariff_key, trckr->tariffid);
	DEBUG2("expand: %s", insertquery);

	// Execute the insert query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, insertquery)) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 : Insertion of account tracker data failed.",
				"SC_HSPGW_AUTH_ORA_ERROR", trckrdata->sessid,
				trckrdata->zoneid);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_accUpdateGid - pms specific (just for baur au lac)
 *
 *    Purpose: Internal function to update a guest-id PMS_ACCOUNT_TRACKER table
 *  @ void *instance: the module instance
 *  @ REQUEST *request: the module request
 *  @ pms_tracker_data *trckrdata: the tracker data
 *  Return -1 on failure, 0 on success
 *
 *  Author: hm/wdw
 *  Last changed: 05.12.2009
 *************************************************************************/
static int pms_fias_accUpdateGid(void *instance, REQUEST *request,
		pms_tracker_data *trckrdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char updquery[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];

	pms_tracker_data *trckr = trckrdata; // Tracker

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 accUpdateGid : Connection to data base failed.",
				"SC_HSPGW_AUTH_SPPDB_CONN_ERR", trckrdata->sessid,
				trckrdata->zoneid);
		return -1;
	}

	// Replace placeholders in querystr pms_acc_upd_gid
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acc_upd_gid,
			request, sql_escape_func);
	dupquotes(modstr, trckr->pwd);
	sprintf(updquery, querystr, trckr->guestid, atoi(trckr->zoneid), modstr);
	DEBUG2("expand: %s", updquery);

	// Execute the update query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, updquery)) {
		//        radlog(L_ERR, "rlm_pms_1 : %s : Update failed: %s", "SC_HSPGW_ORA_ERROR", updquery);
		radlog(
				L_ERR,
				"%s : sid %s : zid %s : rlm_pms_1 accUpdateGid : Update of guest-ID in account tracker failed.",
				"SC_HSPGW_AUTH_ORA_ERROR", trckrdata->sessid, trckrdata->zoneid);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_accUpdBilled
 *
 *    Purpose: Internal function to update a guest-id PMS_ACCOUNT_TRACKER table
 *  @ void *instance: the module instance
 *  @ REQUEST *request: the module request
 *  Return -1 on failure, 0 on success
 *
 *  Author: hm/wdw
 *  Last changed: 05.12.2009
 *************************************************************************/
static int pms_fias_accUpdBilled(void *instance, REQUEST *request, pms_data *pmsdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "rlm_pms_1 : %s : Connection to data base failed.",
				"SC_HSPGW_ORA_ERROR");
		return -1;
	}
	// Replace placeholders in querystr pms_acc_upd_billed_ses
	if (strcmp(pmsdata->tariff_key, PMS_TARIFF_PER_SESSION) == 0) {
		// only mark the session as billed
		parse_repl(querystr, sizeof(querystr),
				inst->config->pms_acc_upd_billed_ses, request, sql_escape_func);
	} else {
		// mark all sessions for a room as billed
		parse_repl(querystr, sizeof(querystr),
				inst->config->pms_acc_upd_billed, request, sql_escape_func);
	}

	// Execute the update query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, querystr)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Update failed: %s",
				"SC_HSPGW_ORA_ERROR", querystr);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}

/*************************************************************************
 *
 *    Function:     upd_active_gw
 *
 *    Purpose:     Internal function to update the active gateway in
 *                HSPGW_CONFIGURATION
 *  @ void *instance: the module instance
 *  @ hostname
 *  Return -1 on failure, 0 on success
 *
 *  Author: hm/wdw
 *  Last changed: 2011-07-01
 *************************************************************************/

static int upd_active_gw(void *instance, REQUEST *request, pms_data *pmsdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	pms_data *lpmsdata = pmsdata;
	char *msgtxt = NULL;
	char querystr[MAX_QUERY_LEN];
	char updquery[MAX_QUERY_LEN];

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		msgtxt
				= "SC_HSPGW_AUTH_SPPDB_CONN_ERR : rlm_pms_1 upd_active_gw : Connection to data base failed. Authentication aborted.";
		//        pms_fias_errLog(msgtxt, "SC_HSPGW_AUTH_SPPDB_CONN_ERR", inst, logfiledata, NULL);
		radlog(L_ERR, msgtxt);
		return 0;
	}
	// Replace placeholders in querystr pms_upd_activ_gw
	parse_repl(querystr, sizeof(querystr), inst->config->pms_upd_activ_gw,
			request, sql_escape_func);
	sprintf(updquery, querystr, inst->hostname, lpmsdata->config_id);
	DEBUG2("expand: %s", updquery);


	// Execute the update query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, updquery)) {
		msgtxt
				= "SC_HSPGW_AUTH_ORA_ERROR : rlm_pms_1 upd_active_gw : Update of active GW to data base failed. Authentication aborted.";
		radlog(L_ERR, msgtxt);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 1;
}

static void freepl(PMS_DATA_LIST *pl) {
	PMS_DATA_LIST *nl = NULL;
	pms_data *pmsdata = NULL;
	while (pl != NULL) {
		nl = pl;
		pmsdata = pl->pmsdata;
		pl = pl->next;
		free(pmsdata);
		free(nl);
	}
}

/*************************************************************************
 *
 *    Function: get_pms_data
 *
 *    Purpose: Internal function for the PMS configuration data
 *  @ pms_data **pmsdata: reference to pms_data
 *  @ void *inst: pms_1 module instance
 *  Return int
 *
 *  Author: hm/wdw
 *  Last changed: 2011-07-01
 *************************************************************************/

static int get_pms_data(pms_data **pmsdata, void *instance, char *qs) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	//    pms_data     *pd = *pmsdata;
	char querystr[MAX_QUERY_LEN];

	/*
	 * Get PMS Configuration from data pool
	 */
	strcpy(querystr, qs);
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		return 0;
	}

	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}

	if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == -1) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}

	// Get the values
	row = sqlsocket->row;

	if (row == NULL) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return 0;
	}
	// Build the PMS Configuration Data
	*pmsdata = pms_fias_createConfigData(row);

	// Free the DB handles
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return 1;
}

/*************************************************************************
 *
 *    Function: pms_accTrackerPurge
 *
 *    Purpose: Internal function to purge table PMS_ACCOUNT_TRACKER
 *  @ void *instance: the module instance
 *  @REQUEST *request: the module request
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 10.11.2009
 *************************************************************************/
static int pms_fias_accTrackerPurge(void *instance, REQUEST *request) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char deletequery[MAX_QUERY_LEN];
	int current_time = time(NULL);

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "rlm_pms_1 : %s : Connection to data base failed.",
				"SC_HSPGW_ORA_ERROR");
		return -1;
	}

	// first delete account_tracker_relogin
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_relogin_purge,
			request, sql_escape_func);
	sprintf(deletequery, querystr, current_time);
	DEBUG2("expand: %s", deletequery);

	// Execute the delete query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, deletequery)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Delete from table failed.",
				"SC_HSPGW_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	// then delete account_tracker
	parse_repl(querystr, sizeof(querystr), inst->config->pms_acctrack_purge,
			request, sql_escape_func);
	sprintf(deletequery, querystr, current_time);
	DEBUG2("expand: %s", deletequery);

	// Execute the delete query
	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, deletequery)) {
		radlog(L_ERR, "rlm_pms_1 : %s : Delete from table failed.",
				"SC_HSPGW_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_dbLogger
 *
 *    Purpose: Internal function to insert a new row into PMS_LOG table
 *  @ void *instance: the module instance
 *  @ REQUEST *request: the module request
 *  @ pms_logger_data *loggerdata: the tracker data
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 02.12.2009
 *************************************************************************/
static int pms_fias_dbLogger(void *instance, REQUEST *request,
		pms_logger_data *loggerdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = instance;
	char querystr[MAX_QUERY_LEN];
	char insertquery[MAX_QUERY_LEN];
	char sid[PMS_SESSIONID_MAX_LEN + 1];
	int ret;
	pms_logger_data *logdata = loggerdata; // Logger
	VALUE_PAIR *vp;

	/*
	 **********************added by Hj Lin on 13 Jan, 2011*****************************************
	 */
	vp = pairfind(request->packet->vps, PW_ACCT_SESSION_ID);
	if (!vp) {

		strcpy(sid, "n/a");
		//sid = "n/a";
	} else {
		strcpy(sid, vp->vp_strvalue);

	}

	/****************************************************/
	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %d : rlm_pms_1 dbLogger : Connection to data base failed.",
				"SC_HSPGW_SPPDB_CONN_ERR", sid, logdata->zoneid);
		return -1;
	}

	// Replace placeholders in querystr pms_logger_insert
	parse_repl(querystr, sizeof(querystr), inst->config->pms_logger_insert,
			request, sql_escape_func);
	sprintf(insertquery, querystr, logdata->zoneid, logdata->hsrmid,
			logdata->logeventid, logdata->logdescr);
	DEBUG2("expand: %s", insertquery);
	// Execute the insert query
	ret = rlm_sql_query(sqlsocket, inst->sql_inst, insertquery);
	if (ret != 0) {
		radlog(
				L_ERR,
				"%s : sid %s : zid %d : rlm_pms_1 dbLogger : Insert into log table failed.",
				"SC_HSPGW_ORA_ERROR", sid, logdata->zoneid);
		sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	// Free memory
	free(logdata);
	return 0;
}

/*************************************************************************
 *
 *    Function: spp_set_gothrough_deadline
 *
 *    Purpose: Set go_through_deadline in HSPGW_CONFIGURATION
 *  @ void *instance: PMS instance
 *
 *  Return -1 on failure, 1 on success
 *
 *  Author: Thomas Wichser
 *  Last changed: 24.06.2010
 *************************************************************************/
static int spp_set_gothrough_deadline(void *instance, int deadline, pms_data *pmsdata) {
	SQLSOCK *sqlsocket = NULL;
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	char updquery[MAX_QUERY_LEN];

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "rlm_pms : %s : Connection to data base failed.",
				"SC_HSPGW_ORA_ERROR");
		return -1;
	}

	radlog(
			L_INFO,
			"SC_HSPGW_GO_THROUGH_INFO : zid %s : Setting go-through deadline for %s:%s to %d",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, deadline);

	//radlog(L_INFO, "from config: %s", inst->config->pms_set_deadline);
	//    parse_repl(querystr, sizeof(querystr), inst->config->pms_set_deadline, request, sql_escape_func);
	//    sprintf(updquery, querystr, deadline);
	sprintf(updquery, inst->config->pms_set_deadline, deadline,
			pmsdata->config_id);
	DEBUG2("expand: %s", updquery);

	//radlog(L_INFO, "after setting deadline: %s", updquery);

	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, updquery)) {
		radlog(L_ERR, "rlm_pms : %s : Update failed: %s", "SC_HSPGW_ORA_ERROR",
				updquery);
		(inst->sql_inst->module->sql_finish_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	(inst->sql_inst->module->sql_finish_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
	return 1;
}

/*************************************************************************
 *
 *    Function: pms_insertSocketPool
 *
 *    Purpose: Internal functions to insert a new node into the socket pool
 *  @ PMS_SOCKET_POOL *socketpool: the socket pool
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return pointer to the inserted node
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static PMS_SOCKET* pms_fias_insertSocketPool(void *instance, pms_data *pmsdata) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	PMS_SOCKET_POOL *current;
	PMS_SOCKET_POOL *last = NULL;
	PMS_SOCKET_POOL *next_socketpool;
	PMS_SOCKET* pmssocket = NULL;

	// Proceed to insert the data
	pmssocket = pms_fias_createSocketData(pmsdata);
	pmssocket->timeouts = inst->config->pms_conf_sckt_recv_ntimeout;

	// Allocate memory for socket pool
	next_socketpool = rad_malloc(sizeof(PMS_SOCKET_POOL));
	memset(next_socketpool, 0, sizeof(PMS_SOCKET_POOL));
	next_socketpool->sckt = pmssocket;

	// Pointer to Next struct node
	next_socketpool->next = NULL;
	current = inst->datapool->socketpool;

	while (current) {
		last = current;
		current = current->next;
	}

	// Here we create the node and allocate memory
	// calling the function pms_createSocketData()
	if (last == NULL) {
		inst->datapool->socketpool = next_socketpool;
	} else {
		last->next = next_socketpool;
	}

	return pmssocket;
}

/*************************************************************************
 *
 *    Function: pms_createSocketData
 *
 *    Purpose: Internal function to create a new node in the PMS data pool
 *  @ SQL_ROW row: the result of the SQL SELECT query
 *  Return pointer to the new node
 *
 *  Author: Juan Vasquez
 *  Last changed: 12.01.2010
 *************************************************************************/
static PMS_SOCKET *pms_fias_createSocketData(pms_data *pmsdata) {
	PMS_SOCKET *pmssocket;
	int rcode;

	// Allocate memory for PMS socket
	pmssocket = rad_malloc(sizeof(PMS_SOCKET));
	memset(pmssocket, 0, sizeof(PMS_SOCKET));
	pmssocket->state = pmssockunconnected;
	pmssocket->shutdown = 0;
	pmssocket->sockfd = 0;
	pmssocket->seqnumber = 0;
	pmssocket->timeouts = 0;
	pmssocket->thread = 0;
	pmssocket->snd_msg_queue = NULL;
	pmssocket->rcv_msg_queue = NULL;
	strcpy(pmssocket->ipaddr, pmsdata->ipaddr);
	strcpy(pmssocket->port, pmsdata->port);
#ifdef HAVE_PTHREAD_H
	rcode = pthread_cond_init(&pmssocket->cond_rcvmsg, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init cond_rcvmsg of PMS socket: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_cond_init(&pmssocket->cond_rstthread, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init cond_rcvmsg of PMS socket: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_mutex_init(&pmssocket->socket_mutex, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init lock of PMS socket: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_mutex_init(&pmssocket->sndqueue_mutex, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init lock of PMS socket: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_mutex_init(&pmssocket->rcvqueue_mutex, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init lock of PMS socket: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_mutex_init(&pmssocket->cond_rcvmsg_mutex, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init lock of cond_rcvmsg_mutex: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}

	rcode = pthread_mutex_init(&pmssocket->cond_rstthread_mutex, NULL);
	if (rcode != 0) {
		free(pmssocket);
		radlog(
				L_ERR,
				"%s : sid n/a : zid %s : pms %s:%s : createSocketData : Failed to init lock of cond_rstthread_mutex: %s",
				"SC_HSPGW_INTERNAL_ERR", pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(rcode));
		return NULL;
	}
#endif

	return pmssocket;

}

/*************************************************************************
 *
 *    Function: pms_find_ipSocket
 *
 *    Purpose: Internal function to find the address of a node containing the given hsrmid
 *  @ PMS_SOCKET_POOL *socketpool: the socket pool
 *  @ char *ip: the ip of pms
 *  Return NULL if not found, pointer to the node if found
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static PMS_SOCKET *pms_fias_find_SocketData(PMS_SOCKET_POOL *socketpool, char *ip, char *port) {
	PMS_SOCKET_POOL *found = NULL;
	PMS_SOCKET_POOL *current = socketpool;

	while (current) {
		if (strcmp(current->sckt->ipaddr, ip) == 0 && strcmp(current->sckt->port, port) == 0) {
			found = current;
			return found->sckt;
		}
		current = current->next;
	}

	return NULL;
}

/*************************************************************************
 *
 *    Function: pms_initDataPool
 *
 *    Purpose: Internal function initialise the data pool
 *  @ void *instance: PMS instance
 *
 *  Return -1 on failure, 0 on mutex failure, 1 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 06.01.2010
 *************************************************************************/
static int pms_fias_initDataPool(void *instance) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	PMS_DATA_POOL *datapool = NULL;
	pms_data *pmsdata = NULL;
	PMS_SOCKET *pmssocket = NULL;
	int rcode;

	inst->datapool = NULL;

	// Allocate memory
	datapool = rad_malloc(sizeof(struct pms_data_pool));
	memset(datapool, 0, sizeof(datapool));
	datapool->socketpool = NULL;

#ifdef HAVE_PTHREAD_H
	rcode = pthread_mutex_init(&datapool->mutex, NULL);
	if (rcode != 0) {
		radlog(    L_ERR,
				"%s : n/a : n/a : init : Failed to initialize lock of data pool: %s", "SC_HSPGW_INTERNAL_ERR",
				strerror(rcode));
		free(datapool);
		return 0;
	}
#endif

	// Add the data pool to the instance
	inst->datapool = datapool;

	// Collect data from database and create the socket pool
	DEBUG2("pms_fias::pms_initDataPool: Collecting PMS Data from SPP database.");
	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "%s : init : Connection to data base failed while initializing data pool.", "SC_HSPGW_SPPDB_CONN_ERR");
		free(datapool);
		return -1;
	}
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, inst->config->pms_all_config_query)) {
		radlog(L_ERR, "%s : init : Database query failed while querying pms data.", "SC_HSPGW_ORA_ERROR");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		free(datapool);
		return -1;
	}

	// Fetch rows
	while (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == 0 && sqlsocket->row != NULL) {

		row = sqlsocket->row;

		if (!pms_fias_containsSocketData(datapool->socketpool, row)) {
			// Build the PMS Configuration Data
			pmsdata = pms_fias_createConfigData(row);
			// Insert the node into the data struct
			// this will allocate memory for each node!
			pmssocket = pms_fias_insertSocketPool(inst, pmsdata);

		}
	}

	// Free the DB handles
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return 1;
}

/*************************************************************************
 *
 *    Function: pms_containsSocketData
 *
 *    Purpose: Internal function to determine if the current SQL row is contained in the data pool
 *  @ struct pms_data *list: the data pool
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return 0 on not found, 1 on found
 *
 *  Author: Juan Vasquez
 *  Last changed: 04.01.2010
 *************************************************************************/
static int pms_fias_containsSocketData(PMS_SOCKET_POOL *socketpool, SQL_ROW row) {

	if (socketpool == NULL) {
		return 0;
	}
	while (socketpool) {

		if (socketpool->zoneid == atoi(row[0])) {
			return 1;
		}

		if (strcmp(socketpool->sckt->ipaddr, row[1]) == 0 && strcmp(socketpool->sckt->port, row[2]) == 0) {
			return 1;
		}

		socketpool = socketpool->next;
	}
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_containsLogEvent
 *
 *    Purpose: Internal function to determine if the current SQL row is contained in the log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return 0 on not found, 1 on found
 *
 *  Author: Juan Vasquez
 *  Last changed: 04.12.2009
 *************************************************************************/
static int pms_fias_containsLogEvent(PMS_LOGEVENTS *list, SQL_ROW row) {
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
 *    Function: pms_createConfigData
 *
 *    Purpose: Internal function for the PMS configuration data
 *  @ SQL_ROW row: the result of the SQL SELECT query
 *  Return pointer to the struct
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
static struct pms_data *pms_fias_createConfigData(SQL_ROW row) {
	pms_data *list;

	list = rad_malloc(sizeof(struct pms_data));
	memset(list, 0, sizeof(struct pms_data));
	// zone ID is primary key
	strcpy(list->zoneid, row[0]);
	// IP Address
	strcpy(list->ipaddr, row[1]);
	// Port number
	strcpy(list->port, row[2]);
	// Protocol ID
	list->protocol = atoi(row[3]);
	// Session Time Out
	strcpy(list->sesto, row[4]);
	// Amount per Unit
	strcpy(list->amount, row[5]);
	// Attribute filed name 1
	strcpy(list->usrfld, row[6]);
	// Attribute filed name 2
	strcpy(list->pwfld, row[7]);
	// Tariff id
	list->tariffid = atoi(row[23]);
	// Tariff key
	strcpy(list->tariff_key, row[8]);
	// Go Through
	list->go_through = atoi(row[9]);
	// DF_LHSPID_HRSM
	strcpy(list->hsrmid, row[10]);
	// Is Enabled
	list->is_enabled = atoi(row[11]);
	// Go Through Period
	list->go_through_period = atoi(row[12]);
	// Go Through Deadline
	list->go_through_deadline = atoi(row[13]);
	// configuration id
	strcpy(list->config_id, row[14]);
	// max simultaneous sessions
	list->max_sim_sess = atoi(row[15]);
	// active gateway node
	strcpy(list->active_gw, row[16]);
	// latest check-out time
	list->ecot = atoi(row[17]);
	// group ID
	list->group = atoi(row[26]);
	// bandwidth profile id
	list->bw_profile_id = atoi(row[27]);

	return list;
}

/*************************************************************************
 *
 *    Function: pms_createLogEvent
 *
 *    Purpose: Internal function to create a new node in the log events struct
 *  @ SQL_ROW row: the result of the SQL SELECT query
 *  Return pointer to the new node
 *
 *  Author: Juan Vasquez
 *  Last changed:14.12.2009
 *************************************************************************/
static PMS_LOGEVENTS *pms_fias_createLogEvent(SQL_ROW row) {

	PMS_LOGEVENTS *list;

	// Allocate memory
	list = rad_malloc(sizeof(PMS_LOGEVENTS));
	memset(list, 0, sizeof(PMS_LOGEVENTS));

	list->id = atoi(row[0]);
	strcpy(list->key, row[1]);
	strcpy(list->type, row[2]);
	strcpy(list->level, row[3]);
	strcpy(list->postcond, row[4]);
	strcpy(list->format, row[1]);

	list->next = NULL;
	return list;
}

/*************************************************************************
 *
 * Function: pms_snmpTrap
 *
 * Purpose: send message per snmp trap
 *
 * Input:    message text
 *            instance
 *            request
 *
 *  Last changed: 03.03.2010
 *************************************************************************/

static void pms_fias_snmpTrap(char *zoneId, char *eventKey, char *msgtxt, void *instance, REQUEST *request) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	char command[512];
	int result;

	if (strcmp(inst->config->pms_snmptrap_command, "") == 0) {
		return;
	}

	sprintf(command, inst->config->pms_snmptrap_command, msgtxt, zoneId, eventKey);
	DEBUG2(command);

	result = system(command);

	if (result != 0) {
		RDEBUG2("snmptrap failed.");
	}
}

/*************************************************************************
 *
 *    Function: pms_sizeLogEvent
 *
 *    Purpose: Internal function to calculate the number of nodes in log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  Return the size
 *
 *  Author: Juan Vasquez
 *  Last changed: 04.12.2009
 *************************************************************************/
static int pms_fias_sizeLogEvent(PMS_LOGEVENTS *list) {
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
 *  Author: Juan Vasquez
 *  Last changed: 25.01.2010
 *************************************************************************/

static void pms_fias_infoLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *ld, REQUEST *request) {

	pms_logger_data *logdata = NULL;
	PMS_LOGEVENTS *the_event = NULL;
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;

	if (ld) {
		if (request) {
			//radlog(L_INFO, "%s : %u : %s : %s : %s : %s : %s",  eventkey, request->number, ld->sessid, ld->zoneid, ld->username, ld->tariffid, msgtxt);
			radlog(L_INFO, "%s : req %u : sid %s : zid %s : usr %s : %s",  eventkey, request->number, ld->sessid, ld->zoneid, ld->username, msgtxt);
			the_event = pms_fias_findLogEventKey(inst->logevents, eventkey);
			if (!the_event) {
				radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : Invalid LogEventKey '%s'. Database logger will not work!",  "SC_HSPGW_INVALID_LOGKEY", request->number, ld->sessid, ld->zoneid, ld->username, eventkey);
			} else {
				pms_fias_prepareLogData(&logdata, ld->zoneid, ld->hsrmid, the_event->id, msgtxt);
				pms_fias_dbLogger(inst, request, logdata);
			}
		} else {
			radlog(L_INFO, "%s : req n/a : sid %s : zid %s : usr %s : %s",  eventkey, ld->sessid, ld->zoneid, ld->username, msgtxt);
			//radlog(L_INFO, "%s : n/a : %s : %s : %s : %s : %s",  eventkey, ld->sessid, ld->zoneid, ld->username, ld->tariffid, msgtxt);

		} // if (request)
	} else {
		radlog(L_INFO, "%s : req n/a : sid n/a : zid n/a : usr n/a : %s", eventkey, msgtxt);
	}
}

/*************************************************************************
 *
 * Function: pms_fias_errLog
 *
 * Purpose: event loggin (radlog, snmp trap and optional in SPP-DB)
 *
 * Input:    message text
 *            event key
 *             instance
 *            logfile data
 *            request: if NULL then no db-log
 *
 *  Last changed: 03.03.2010
 *************************************************************************/

static void pms_fias_errLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *ld, REQUEST *request) {

	pms_logger_data *logdata = NULL;
	PMS_LOGEVENTS *the_event = NULL;
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;

	if (ld) {

		if (request) {
			//radlog(L_ERR, "%s : %u : %s : %s : %s : %s : %s",  eventkey, request->number, ld->sessid, ld->zoneid, ld->username, ld->tariffid, msgtxt);
			radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : %s",  eventkey, request->number, ld->sessid, ld->zoneid, ld->username, msgtxt);

			the_event = pms_fias_findLogEventKey(inst->logevents, eventkey);

			if (!the_event) {
				radlog(L_ERR, "%s : req %u : sid %s : zid %s : usr %s : Invalid LogEventKey '%s'. Database logger will not work!",  "SC_HSPGW_INVALID_LOGKEY", request->number, ld->sessid, ld->zoneid, ld->username, eventkey);
			} else {
				pms_fias_prepareLogData(&logdata, ld->zoneid, ld->hsrmid, the_event->id, msgtxt);
				pms_fias_dbLogger(inst, request, logdata);

				if (strcmp(the_event->level, "Critical") == 0) {
					pms_fias_snmpTrap(ld->zoneid, the_event->key, msgtxt, instance, request);
				}
			}
		} else {
			//radlog(L_ERR, "%s : n/a : %s : %s : %s : %s : %s",  eventkey, ld->sessid, ld->zoneid, ld->username, ld->tariffid, msgtxt);
			radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",  eventkey, ld->sessid, ld->zoneid, ld->username, msgtxt);
		} // if (request)
	} else {
		radlog(L_ERR, "%s : req n/a : sid n/a : zid n/a : usr n/a : %s", eventkey, msgtxt);
	}
}

/*************************************************************************
 *
 *    Function: pms_printConfigData
 *
 *    Purpose: Internal function to print a PMS data pool node
 *  @ struct pms_data *list: pointer to struct
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
static void pms_fias_printConfigData(struct pms_data *list) {
	if (list) {
		DEBUG("___Config_Data_Beginn___");
		DEBUG("Zone ID: %s", list->zoneid);
		DEBUG("IP Address: %s", list->ipaddr);
		DEBUG("Port number: %s", list->port);
		DEBUG("Protocol ID: %d", list->protocol);
		DEBUG("Session Time Out: %s", list->sesto);
		DEBUG("Amount per Unit: %s", list->amount);
		DEBUG("Attribute filed name 1: %s", list->usrfld);
		DEBUG("Attribute filed name 2: %s", list->pwfld);
		DEBUG("Tariff ID: %d", list->tariffid);
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
 *    Function: pms_prepareLogData
 *
 *    Purpose: Internal function to prepare the logger data
 *  @ pms_logger_data *loggerdata: the tracker data
 *  Return 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 01.12.2009
 *************************************************************************/
static int pms_fias_prepareLogData(pms_logger_data **loggerdata, char *zoneid,
		char *hsrmid, int logeventid, char *logdescr) {
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
 *    Function: pms_freeLogEventPool
 *
 *    Purpose: Internal functions to free the event pool
 *  @ void *instance: PMS instance
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
static void pms_fias_freeLogEventPool(void *instance) {
	rlm_pms_1_module_t *inst = instance;
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
 *    Function: pms_initLogEventPool
 *
 *    Purpose: Internal function initialise the log event pool
 *  @ void *instance: PMS instance
 *
 *  Return -1 on failure, 1 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 17.12.2009
 *************************************************************************/
static int pms_fias_initLogEventPool(void *instance) {
	rlm_pms_1_module_t *inst = instance;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	PMS_LOGEVENTS *logevents = NULL;
	int ret;

	// Collect data from database and create the socket pool
	DEBUG2("pms_fias::pms_initLogEventPool: Initialize event pool.");

	// Initialize the sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : init : Connection to data base failed while initializing log event pool.");
		return -1;
	}
	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, inst->config->pms_all_events_query)) {
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : init : Database query failed while querying log events.");
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}
	// Fetch rows
	while (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == 0 && sqlsocket->row != NULL) {

		row = sqlsocket->row;
		// Insert the node into the data struct
		// this will allocate memory for each event node!
		logevents = pms_fias_insertLogEvent(logevents, row);

	}

	ret = pms_fias_sizeLogEvent(logevents);
	// Error if could not collect log events data from database (database is empty?)
	if (!ret) {
		radlog(L_ERR, "SC_HSPGW_ORA_NO_DATA :  Could not retrieve any log events");
		(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		return -1;
	}

	// Free the DB handles
	(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	// Add the log event pool to the instance
	inst->logevents = logevents;

	return 1;
}

/*************************************************************************
 *
 *    Function: pms_insertLogEvent
 *
 *    Purpose: Internal functions to insert a new node into the log events struct
 *  @ PMS_LOGEVENTS *list: log events struct
 *  @ SQL_ROW row: the current row of the SQL SELECT query
 *  Return pointer to the inserted node
 *
 *  Author: Juan Vasquez
 *  Last changed: 14.12.2009
 *************************************************************************/
static PMS_LOGEVENTS *pms_fias_insertLogEvent(PMS_LOGEVENTS *list, SQL_ROW row) {
	PMS_LOGEVENTS *last = NULL;
	PMS_LOGEVENTS *current = list;

	if (!pms_fias_containsLogEvent(list, row)) {

		while (current) {
			last = current;
			current = current->next;
		}

		// Allocate memory for each node calling pms_createLogEvent()
		if (last == NULL) {
			list = pms_fias_createLogEvent(row);
		} else {
			last->next = pms_fias_createLogEvent(row);
		}
	}
	return list;
}

/*************************************************************************
 *
 *    Function: pms_findLogEventKey
 *
 *    Purpose: Internal function to find a node containing the given event key
 *  @ PMS_LOGEVENTS *list: the data pool
 *  @ char *eventkey: the log event key
 *  Return pointer to the selected node if found one, NULL if not found
 *
 *  Author: Juan Vasquez
 *  Last changed: 04.12.2009
 *************************************************************************/
static PMS_LOGEVENTS *pms_fias_findLogEventKey(PMS_LOGEVENTS *list, char *eventkey) {
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
 *    Function: pms_fias_findPassword
 *
 *    Purpose: Internal function to find a given password (the Last Name) in a
 *             string (the Full Name). The GuestName field has the length as defined in FIAS.
 *  @ char *name: the name (GuestName field), e.g. "Obama"
 *  @ char *password: string (Password) to find in the full name, e.g. "Obama"
 *  @ int ignorecase: "obama" or "Obama" are valid and will return 1
 *  Return 0 on not found, 1 on found
 *
 *  Author: Juan Vasquez
 *  Last changed: 24.02.2009 -shan
 *************************************************************************/
static int pms_fias_findPassword(char *fullname, char *password, int ignorecase) {
	char thename[PMS_USERNAME_MAX_LEN + 1];
	char thepasswd[PMS_PASSWORD_MAX_LEN + 1]; // see cloumn PMS_ACCOUNT_TRACKER_PWD in table PMS_ACCOUNT_TRACKER
	int i;

	if (ignorecase > 0) {
		i = 0;
		while (*fullname != '\0') {
			thename[i] = toupper(*fullname);
			fullname++;
			i++;
		}
		thename[i] = '\0';
		i = 0;
		while (*password != '\0') {
			thepasswd[i] = toupper(*password);
			password++;
			i++;
		}
		thepasswd[i] = '\0';
	} else {
		strcpy(thename, fullname);
		strcpy(thepasswd, password);
	}

	trim(thename);
	DEBUG3( "pms_fias_findPassword: thename = '%s' thepasswd= '%s'", thename, thepasswd);
	if (strcmp(thename, thepasswd) == 0) {
		return 1;
	} else {
		return 0;
	}

}


/*************************************************************************
 *
 *    Function: trim
 *
 *    Purpose: trims a string (remove leading / trailing whitespaces)
 *
 *  Author: Internet
 *  Last changed: 30.01.2015 am
 *************************************************************************/
static void trim (char *s)
{
	int i;
	char *t = s;

	while (isspace (*s)) s++;   // skip left side white spaces
	for (i = strlen (s) - 1; (isspace (s[i])); i--) ;   // skip right side white spaces
	s[i+1] = '\0';
	memmove(t, s, i+2);
}

/*************************************************************************
 *
 *    Function: trim
 *
 *    Purpose: convert RADIUS IP address to format aaabbbcccddd
 *
 *  Author: am
 *
 *************************************************************************/
static void convertRadiusIpAddress (char* dest, char *source)
{
	sprintf(dest, "%03u%03u%03u%03u",
			(unsigned char) source[0],
			(unsigned char) source[1],
			(unsigned char) source[2],
			(unsigned char) source[3]
			);
}


/*************************************************************************
 *
 *    Function: pms_fias_splitPacket
 *
 *    Purpose: Internal function to split a Protel PMS packet according to
 *             the FIAS Protocol (field length as defined in FIAS)
 *  @ char *message: the packet
 *  Return pointer to the created struct packet
 *
 *  Author: Juan Vasquez
 *  Last changed: 	16.12.2009
 *  				24.04.2014 hm/goco: inserted name splitting
 *************************************************************************/
static PMS_FIAS_PACKET *pms_fias_splitPacket(char *message) {
	PMS_FIAS_GUESTLIST *guestlist = NULL;
	PMS_FIAS_PACKET *packet;
	char packetbuffer[MAX_FIAS_BUFFER_SIZE];
	char *ptr;
	char *stok;
	char *fieldvalue;
	int isfirst, foundroom, len;
	BYTE newtriplet = '\x07';
	BYTE triplet = '\x00';
	char *roomnumber = "";
	char *guestid = "";
	char *guestname = "";
//	int i;
//	int n = 0;
//	char guestnames[10][PMS_PASSWORD_MAX_LEN + 1];
//	char guestnames[10][PMS_PASSWORD_MAX_LEN];

	// Allocate memory for packet
	packet = (PMS_FIAS_PACKET *) rad_malloc(sizeof(struct pms_fias_packet));
	memset(packet, '\0', sizeof(packet));
	strcpy(packet->answstatus, "");
	strcpy(packet->cleartxt, "");
	strcpy(packet->ronumber, "");
	packet->isdbsynch = 0; // no db synch
	packet->shared = 0; // not shared

	// Length of string without '\0'
	len = strlen(message);

	// Trim last "|" from packet
	--len;
	strncpy(packetbuffer, message, len);

	// Add '\0'
	packetbuffer[len] = '\0';

	// Split trimmed packet
	//ptr = strtok(packetbuffer, PROTEL_FIAS_SPRTR);
	ptr = strtok_r(packetbuffer, PROTEL_FIAS_SPRTR, &stok);
	isfirst = 1;
	foundroom = 0;
	while (ptr != NULL) {
		fieldvalue = ptr;
		fieldvalue += 2;
		if (isfirst) {
			strncpy(packet->recordtype, ptr, 2);
			isfirst = 0;
		} else if (strncmp(ptr, "P#", 2) == 0) {
			packet->seqnumber = atoi(fieldvalue);
		} else if (strncmp(ptr, "DA", 2) == 0) {
			strcpy(packet->date, fieldvalue);
		} else if (strncmp(ptr, "TI", 2) == 0) {
			strcpy(packet->time, fieldvalue);
		} else if (strncmp(ptr, "AS", 2) == 0) {
			strcpy(packet->answstatus, fieldvalue);
		} else if (strncmp(ptr, "CT", 2) == 0) {
			strcpy(packet->cleartxt, fieldvalue);
		} else if (strncmp(ptr, "SF", 2) == 0) {
			packet->isdbsynch = 1;
		}
		else if (strncmp(ptr, "RO", 2) == 0) {
			strcpy(packet->ronumber, fieldvalue);
		}
		else if (strncmp(ptr, "WS", 2) == 0) {
			strcpy(packet->wsid, fieldvalue);
		}
		else if (strncmp(ptr, "GS", 2) == 0) {
			if (strncmp(ptr + 2, "Y", 1) == 0)
				packet->shared = 1;
		}
		else if (strncmp(ptr, "NP", 2) == 0) {
			if (strncmp(ptr + 2, "Y", 1) == 0)
				packet->nopost = 1;
		}
		// According to FIAS, we can get here multiple triplets
		else if (strncmp(ptr, "RN", 2) == 0) {
			roomnumber = fieldvalue;
			triplet += 4;
			foundroom = 1;
		} else if (strncmp(ptr, "G#", 2) == 0) {
			guestid = fieldvalue;
			triplet += 2;
		} else if (strncmp(ptr, "GN", 2) == 0) {
			guestname = fieldvalue;
			triplet += 1;
		} else {
			if (packet->recordtype[0] != 'L' ) {
				// DEBUG2("pms_fias_splitPacket: Got strange field '%s' in message '%s'", ptr, message);
			}
		}
		// New struct
		if (triplet == newtriplet) {
			triplet = '\x00';
			DEBUG2("Create guest list triplet: roomnumber=%s, guestid=%s, guestname=%s", roomnumber, guestid, guestname);

			/*
			// are there more then one name in GN?
			// not for GI, GO, GC:
			if (strncmp(packet->recordtype, "PL", 2) == 0){
				n = convertToGuestlist (guestnames, guestname, guestname_delimiter);
			}

			if( n > 0 ){
				for (i=0; i<n; i++) {
					guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestnames[i]);
				}
			}
			else{
				guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestname);
			} */
			guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestname);
		}
		//ptr = strtok(NULL, PROTEL_FIAS_SPRTR);
		ptr = strtok_r(NULL, PROTEL_FIAS_SPRTR, &stok);
	} // while (ptr != NULL)


	// Some packets like PA may only contain a Room Number
	// not relevant for PL type
	if (foundroom && (guestlist == NULL) ) {
		DEBUG2( "pms_fias::pms_fias_splitPacket: Found a Room Number only. No triplet.");
		// are there more then one name in GN?
/*		n = convertToGuestlist (guestnames, guestname, guestname_delimiter);
		if( n > 0 ){
			for (i=0; i<n; i++) {
				guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestnames[i]);
			}
		}
		else{
			guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestname);
		}  */
		guestlist = pms_fias_insertGuestData(guestlist, roomnumber, guestid, guestname);

	}
	// Add guest list to packet
	packet->guestlist = guestlist;
	return packet;
}
/*************************************************************************
 *
 *    Function: pms_fias_validatePacket
 *
 *    Purpose: Internal function to validate a PMS packet according to
 *             the FIAS Protocol (field length as defined in FIAS)
 *  @ char *message: the orginal message, for information purposes
 *  @ PMS_FIAS_PACKET *packet: the split packet
 *  returns 0 - OK <0 - syntax errors
 *  Author: Andreas Meyer
 *  Last changed: 	24.04.2014 hm/goco: fiex several bugs
 *************************************************************************/
static int pms_fias_validatePacket(char *message, PMS_FIAS_PACKET *packet) {

	char err_msg[256] = "";
	char *err_msg_rectype =    " No valid record type found.";
	char *err_msg_answstatus = " No answer status found.";
	char *err_msg_guestlist =  " No guest list found.";
	char *err_msg_cleartext =  " No cleartext found.";
	char *err_msg_ronumber =   " No old room number found.";
	char *err_msg_seqnumber =  " No sequence number found.";
	char *err_msg_roomnumber = " No room number found.";
	char *err_msg_guestname =  " No guest name found.";
	char *err_msg_guestid   =  " No guest ID found.";
	int ret = 0;

	if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_LS) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_LD) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_LE) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_LR) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_LA) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_PA) == 0) {
		if (strlen(packet->answstatus)== 0) {
			strcat(err_msg, err_msg_answstatus);
		}
		if (strlen(packet->cleartxt)== 0 ) {
			strcat(err_msg, err_msg_cleartext);
		}
		if (packet->seqnumber == 0) {
			strcat(err_msg, err_msg_seqnumber);
		}
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_PL) == 0) {
		if (packet->seqnumber == 0) {
			strcat(err_msg, err_msg_seqnumber);
		}
		if (packet->guestlist == NULL) {
			strcat(err_msg, err_msg_guestlist);
		}
		PMS_FIAS_GUESTLIST *guestlist = packet->guestlist;
		while (guestlist) {
			if (strlen(guestlist->roomnumber) == 0) {
				strcat(err_msg, err_msg_roomnumber);
			}
			if (strlen(guestlist->guestid) == 0) {
				strcat(err_msg, err_msg_guestid);
			}
			if (strlen(guestlist->guestname) == 0) {
				strcat(err_msg, err_msg_guestname);
			}
			guestlist = guestlist->next;
		}
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_DS) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_DE) == 0) {
		// nothing to test
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_GI) == 0) {
		if (packet->guestlist == NULL) {
			strcat(err_msg, err_msg_guestlist);
		} else {
			PMS_FIAS_GUESTLIST *guestlist = packet->guestlist;
			while (guestlist) {
				if (strlen(guestlist->roomnumber) == 0) {
					strcat(err_msg, err_msg_roomnumber);
				}
				if (strlen(guestlist->guestid) == 0) {
					strcat(err_msg, err_msg_guestid);
				}
				if (strlen(guestlist->guestname) == 0) {
					strcat(err_msg, err_msg_guestname);
				}
				guestlist = guestlist->next;
			}
		}
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_GO) == 0) {
//	if (packet->isdbsynch == 0 && packet->guestlist == NULL && strlen(packet->cleartxt)== 0) {  // complete nonsense?
	if (packet->guestlist == NULL) {  // complete nonsense?
			strcat(err_msg, err_msg_guestlist);
		} else {
			PMS_FIAS_GUESTLIST *guestlist = packet->guestlist;
			while (guestlist) {
				if (strlen(guestlist->roomnumber) == 0) {
					strcat(err_msg, err_msg_roomnumber);
				}
				// guestid mandatory in real-time GO
				if (packet->isdbsynch == 0 && strlen(guestlist->guestid) == 0) {
//				if (packet->isdbsynch == 0) {
//					if (strlen(guestlist->guestid) == 0) {
					strcat(err_msg, err_msg_cleartext);
//					}
				}
				guestlist = guestlist->next;
			}
		}
	} else if (strcmp(packet->recordtype, PROTEL_FIAS_RECID_GC) == 0) {
//		if (strlen(packet->ronumber) == 0 && packet->guestlist == NULL) {
		if (packet->guestlist == NULL) {
			strcat(err_msg, err_msg_guestlist);
		} else {
			PMS_FIAS_GUESTLIST *guestlist = packet->guestlist;
			while (guestlist) {

				if (strlen(guestlist->roomnumber) == 0) {
					// if there is no RN
					strcat(err_msg, err_msg_roomnumber);
				}
				// G# is mandatory
				if (strlen(guestlist->guestid) == 0) {
					strcat(err_msg, err_msg_guestid);
				}
				// GN mandatory for HSPGW. Not mandatory according FIAS!
				if (strlen(guestlist->guestname) == 0) {
					strcat(err_msg, err_msg_guestname);
				}

/*	for HSPGW GN is always needed. It is not mandatory according FIAS though.
			// guestname not mandatory when old roomnumber given
				if (strlen(packet->ronumber) == 0) {
					if (strlen(guestlist->guestname) == 0) {
						strcat(err_msg, err_msg_ronumber);
						strcat(err_msg, err_msg_guestname);
					}
				}
*/
				guestlist = guestlist->next;
			}
		}
	} else {
		strcat(err_msg, err_msg_rectype);
	}

	if (strlen(err_msg)) {
		radlog(L_ERR, "SC_HSPGW_PMS_INVALID_FORMAT : PMS message '%s' is invalid. Details: %s",
				message, err_msg);
		ret = -1;
	}
	return ret;
}

/**********************************************************************************
 *
 *    Function: pms_fias_insertGuestData
 *
 *    Purpose: Internal function to insert a new node into the guest list
 *  @ PMS_FIAS_GUESTLIST *guestlist: the guest list
 *  @ char *roomnumber: the room number
 *  @ char *guestid: the guest id
 *  @ char *guestname: the guest name
 *  Return pointer to the inserted node
 *
 *  Author: Juan Vasquez
 *  Last changed: 	13.11.2009
 *  				24.04.2014 hm/goco: moved name splitting to pms_fias_splitPacket
 ***********************************************************************************/

/* rolled back to the old version
static PMS_FIAS_GUESTLIST *pms_fias_insertGuestData(PMS_FIAS_GUESTLIST *guestlist, char *roomnumber, char *guestid, char *guestname) {
	PMS_FIAS_GUESTLIST *last;
	int i;

	last = pms_fias_findLastGuestData(guestlist);
	char guestnames[10][PMS_PASSWORD_MAX_LEN];

	DEBUG3( "pms_fias::pms_fias_insertGuestData: GN = %s, RN = %s, G# = %s", guestname, roomnumber, guestid );
	//DEBUG3( "pms_fias::pms_fias_insertGuestData: guestname = %s", "test");
		int n = convertToGuestlist (guestnames, guestname, guestname_delimiter);
DEBUG3( "pms_fias::pms_fias_insertGuestData: Num of names: %i", n );
	for (i=0; i<n; i++) {
		DEBUG3( "pms_fias::pms_fias_insertGuestData: guestnames[%d] = %s", i,guestnames[i]);
		if (last == NULL) {
			guestlist = pms_fias_createGuestData(roomnumber, guestid, guestnames[i]);
			last = pms_fias_findLastGuestData(guestlist);
		} else {
			last->next = pms_fias_createGuestData(roomnumber, guestid, guestnames[i]);
		}
	}

	return guestlist;
}
*/

static PMS_FIAS_GUESTLIST *pms_fias_insertGuestData(PMS_FIAS_GUESTLIST *guestlist, char *roomnumber, char *guestid, char *guestname) {
	PMS_FIAS_GUESTLIST *last;
	last = pms_fias_findLastGuestData(guestlist);

	if(strlen(guestname) > PMS_PASSWORD_MAX_LEN)
		guestname[PMS_PASSWORD_MAX_LEN] = '\0';
	if (last == NULL) {
		guestlist = pms_fias_createGuestData(roomnumber, guestid, guestname);
	} else {
		last->next = pms_fias_createGuestData(roomnumber, guestid, guestname);
	}
	return guestlist;
}

/*************************************************************************
 *
 *    Function: convertToGuestlist
 *
 *    Purpose: Internal function to separate a guest list into different names
 *  @ char guestnames[][PMS_PASSWORD_MAX_LEN]: the resulting list, must be predefined
 *  @ char *guestname: the original guest name
 *  @ char *delim: the delimiter
 *  Return number of guests found
 *
 *  Author: Andreas Meyer
 *  Last changed: 	13.01.2014
 *  				25.04.2014 hm/goco
 *************************************************************************/
static int convertToGuestlist (char guestnames[][PMS_PASSWORD_MAX_LEN + 1], char *guestname, char *delim) {

	regex_t rx;
	regmatch_t pm;
	int a;
	int cnt = 0;
	int offset = 0;

	DEBUG3("test pattern='%s' string='%s'\n", guestname_delimiter, guestname);
	if (regcomp(&rx, guestname_delimiter, 0) != 0) {
		DEBUG3("Invalid regular expression '%s'\n", guestname_delimiter);
		return 1;
	}
	a = regexec(&rx, guestname, 1, &pm, REG_EXTENDED);

	// hm/25.04.2014
	if(a && strlen(guestname) > 0){
		// only one name
		strcpy(guestnames[cnt], guestname);
		guestnames[cnt][PMS_PASSWORD_MAX_LEN + 1] = '\0';
		DEBUG3("0 guestname[%i]: %s",cnt, guestnames[cnt]);
		cnt++;
	}
	else {

		while (a == 0)
		{
			DEBUG3("match at %d\n",pm.rm_eo);
			strncpy(guestnames[cnt], guestname + offset, pm.rm_so);
			if( pm.rm_so > PMS_PASSWORD_MAX_LEN ){
				guestnames[cnt][PMS_PASSWORD_MAX_LEN + 1] = '\0'; // cut after 40 chars according FIAS 2.20
			}
			else{
				guestnames[cnt][pm.rm_so] = '\0';
			}
			offset += pm.rm_eo;
			cnt++;
			a = regexec(&rx, guestname + offset, 1, &pm, 0);
		}

		// if splitting found also add full guestname
		if (offset < strlen(guestname)) {
			strcpy(guestnames[cnt], guestname + offset);
			cnt++;
		}
	}

	regfree(&rx);

/*	if (cnt > 1) {
		strcpy(guestnames[cnt], guestname); // no null-termination?
DEBUG3("II guestname[%i]: %s",cnt, guestnames[cnt]);
		cnt++;
	}
*/
	return cnt;
}


/*************************************************************************
 *
 *    Function: pms_fias_createGuestData
 *
 *    Purpose: Internal function to create a new node in the guest list
 *  @ char *roomnumber: the room number
 *  @ char *guestid: the guest id
 *  @ char *guestname: the guest name
 *  Return pointer to the new node
 *
 *  Author: Juan Vasquez
 *  Last changed: 13.11.2009
 *************************************************************************/
static PMS_FIAS_GUESTLIST *pms_fias_createGuestData( char *roomnumber, char *guestid, char *guestname) {
	PMS_FIAS_GUESTLIST *guestlist;
	// Allocate memory
	// rad_malloc is a FreeRADIUS function and has its own error handling
	guestlist = (PMS_FIAS_GUESTLIST *) rad_malloc(
			sizeof(struct pms_fias_guestlist));
	memset(guestlist, 0, sizeof(guestlist));

	// room number
	strcpy(guestlist->roomnumber, roomnumber);
	// guest id
	strcpy(guestlist->guestid, guestid);
	// guest name
	// the guest name must only consist of ASCII code
	// (for comparison with pwd)
DEBUG3("pms_fias_createGuestData::guestname: %s", guestname);
	convert2ascii(guestlist->guestname, sizeof(guestlist->guestname), guestname);
	strcpy(guestlist->guestname2, guestname);

	// Pointer to Next struct node
	guestlist->next = NULL;
	return guestlist;
}

/*************************************************************************
 *
 *    Function: pms_fias_findLastGuestData
 *
 *    Purpose: Internal function to determine the last node of the guest list
 *  @ PMS_FIAS_GUESTLIST *guestlist: the guest list
 *  Return pointer to the last node
 *
 *  Author: Juan Vasquez
 *  Last changed: 13.11.2009
 *************************************************************************/
static PMS_FIAS_GUESTLIST *pms_fias_findLastGuestData(PMS_FIAS_GUESTLIST *guestlist) {
	PMS_FIAS_GUESTLIST *last = guestlist;
	while (guestlist) {
		last = guestlist;
		guestlist = guestlist->next;
	}
	return last;
}

/*************************************************************************
 *
 *    Function: pms_fias_startthread
 *
 *    Purpose: Internal function to start a listener thread on a socket
 *  @ void *instance: the module's instance
 *  @ PMS_SOCKET *pmssocket: Socket handle
 *  @ pms_data *pmsdata: structure that contains the pms data
 *
 *  Author:     sz/wdw
 *  Changes:    2011-06-20: created
 *************************************************************************/
static void pms_fias_startthread(void *instance, PMS_SOCKET *pmssocket, pms_data *pmsdata) {
	PMS_THREAD_ARG *thread_arg = (PMS_THREAD_ARG *) rad_malloc(
			sizeof(struct pms_thread_arg));
	memset(thread_arg, '\0', sizeof(thread_arg));
	thread_arg->instance = instance;
	thread_arg->pmssocket = pmssocket;
	thread_arg->pmsdata = pmsdata;

	pthread_t thread_id;

	pthread_create(&thread_id, NULL, pms_fias_thread_proc, (void *) thread_arg);

	pmssocket->thread = thread_id;

	//pthread_detach(thread_id);
}

/*************************************************************************
 *
 *    Function: pms_fias_printPacket
 *
 *    Purpose: Internal function to print a Protel FIAS Packet
 *  @ PMS_FIAS_PACKET *packet: the packet
 *
 *  Author: Juan Vasquez
 *  Last changed: 13.11.2009
 *************************************************************************/
static void pms_fias_printPacket(PMS_FIAS_PACKET *packet) {
	PMS_FIAS_GUESTLIST *guestlist = NULL;
	if (packet) {
		DEBUG("___Packet_Beginn___");
		DEBUG("Record Type: %s", packet->recordtype);
		DEBUG("Sequence number: %d", packet->seqnumber);
		DEBUG("Date: %s", packet->date);
		DEBUG("Time: %s", packet->time);
		DEBUG("Answer Status: %s", packet->answstatus);
		DEBUG("Clear Text: %s", packet->cleartxt);
		DEBUG("---");
		guestlist = packet->guestlist;
		while (guestlist) {
			DEBUG("Room Number: %s", guestlist->roomnumber);
			DEBUG("Guest ID: %s", guestlist->guestid);
			DEBUG("Guest Name: %s", guestlist->guestname);
			DEBUG("---");
			guestlist = guestlist->next;
		}
		DEBUG("___Packet_End___");
	}
}

/*************************************************************************
 0: success
 -1 : error
 *************************************************************************/
static int pms_fias_connect(void *instance, PMS_SOCKET *pmssocket) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	int ret;

	// Connect the socket, if required
	if (pmssocket->state == pending) {
		// Create socket
		pms_fias_lockSocket(pmssocket);
		ret = pms_fias_create_socket(&pmssocket->sockfd);

		if (ret != 0) {
			radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : sid n/a : zid n/a : pms %s:%s : Could not open a socket for TCP communication: %s",
				pmssocket->ipaddr, pmssocket->port, strerror(ret));
			// Unlock the socket
			pms_fias_unlockSocket(pmssocket);
			return -1;
		}

		radlog(L_INFO,"sid n/a : zid n/a : pms %s:%s : sockfd  %d is created", pmssocket->ipaddr, pmssocket->port, pmssocket->sockfd);

		// Make connection
		if (pms_fias_connect_on_socket(inst, pmssocket) != 0) {
			radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : %s : Could not make a connection on the created socket.", pmssocket->ipaddr);
			// Close the socket and unlock the socket
			pms_fias_close_socket(pmssocket);
			pms_fias_unlockSocket(pmssocket);
			return -1;
		}

		pms_fias_unlockSocket(pmssocket);

		// At this point, socket is connected // except of sometimes, grrr....
		changeSockState(pmssocket, pmssockconnected, pmssocket->shutdown);
		//pmssocket->state = pmssockconnected;

	}

	return 0;
}

static void* pms_fias_thread_proc(void *arg) {
	PMS_THREAD_ARG *thread_arg = (PMS_THREAD_ARG *) arg;
	rlm_pms_1_module_t *inst = thread_arg->instance;
	PMS_SOCKET *pmssocket = thread_arg->pmssocket;
	pms_data *pmsdata = thread_arg->pmsdata;
	//SQLSOCK *sqlsckt = NULL;
	int rcode = 0;
	int ret = 0;
	int loop = inst->config->pms_conf_sckt_max_trials;
	char mqs[MAX_QUERY_LEN];

	// make sure it's me that has to connect
	// get query string
	sprintf(mqs, inst->config->pms_config_query_zid, pmsdata->zoneid);

	if (!get_pms_data(&pmsdata, inst, mqs)) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid %s : pms %s:%s : Cannot read pms data from SPPDB. Listener thread won't start.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		// not my turn ?
		loop = 0;
	}

	// connection loop
	while (loop-- && !strcmp(inst->hostname, pmsdata->active_gw) && pmssocket->shutdown == 0) {

		//radlog(L_INFO, "loop: %u", loop);
		DEBUG2("pms_fias::pms_thread_proc: Get socket connection to PMS server %s:%s for location %s.",
					pmsdata->ipaddr, pmsdata->port, pmsdata->zoneid);

		//better here pending?
		changeSockState(pmssocket, pending, pmssocket->shutdown);
		//pmssocket->state = pending;


		// Initialize connection with PMS server
		// may be it's helpful if we invent a flag what marks the last round
		rcode = pms_fias_connect(inst, pmssocket);

		if (rcode < 0) {

			pms_fias_releaseSocket(pmssocket);

		} else {
			radlog(L_INFO, "SC_HSPGW_PMS_CONNECTED : sid n/a : zid %s : pms %s:%s : Connected on socket for PMS interface.",
				pmsdata->zoneid, pmssocket->ipaddr, pmssocket->port);

			// if there is a go through deadline > 0 it should be reset to zero here
			if (pmsdata->go_through_deadline > 0) {
				DEBUG2("Go-through deadline was set but PMS is reachable. Resetting go-though deadline");
				ret = spp_set_gothrough_deadline(inst, 0, pmsdata);
				if (ret < 0) {
					radlog(L_ERR, "SC_HSPGW_AUTH_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Could not reset go-through deadline.",
						pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
				}
			}

			// listener loop
			while (pmssocket->shutdown == 0) {
				//cancel point: obsolete
				//pthread_testcancel();

				//send pending messages
				// Lock the socket before proceeding
				pms_fias_lockSendQueue(pmssocket);

				SEND_MESSAGE_QUEUE *current = pmssocket->snd_msg_queue;
				SEND_MESSAGE_QUEUE *tmp = NULL;
				while (current != NULL) {

					rcode = pms_fias_send_packet(pmssocket, current->message, pmsdata);
					if (rcode < 0) {
						//pmssocket->state = pmssockunconnected;
						changeSockState(pmssocket, pmssockunconnected, pmssocket->shutdown);
						pms_fias_releaseSocket(pmssocket);
						break;
					}

					tmp = current;
					//remove current node
					free(current);
					current = tmp->next;
				}
				if (pmssocket->state == pmssockunconnected) {
					break;
				}

				pmssocket->snd_msg_queue = NULL;
				pms_fias_unlockSendQueue(pmssocket);

				//receive message
				//
				pms_fias_removeInvalidateMessages(pmssocket);

				rcode = pms_fias_recprocmgs(inst, pmssocket, pmsdata);

				if (rcode == -2 || rcode == -3) { //time out or invalid regexp
					continue;
				}

				if (rcode == -1) { // socket state already set to unconnected
					pms_fias_releaseSocket(pmssocket);
					break;
				}

				loop = inst->config->pms_conf_sckt_max_trials - 1;

			}
		}

		// if code reach here, the socket is broken

		radlog(L_ERR, "SC_HSPGW_PMS_DISCONNECTED : sid n/a : zid %s : pms %s:%s : PMS is disconnected.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		DEBUG2("Reconnect round %d in %d seconds",
					(inst->config->pms_conf_sckt_max_trials - loop),
					(inst->config->pms_conf_sckt_max_trials - loop) * 10);

		//last round should not wait
		if(inst->config->pms_conf_sckt_max_trials != inst->config->pms_conf_sckt_max_trials - loop && pmssocket->shutdown == 0){
			if(pms_fias_waitThreadRestart(pmssocket, inst->config->pms_conf_sckt_max_trials - loop) != ETIMEDOUT){
				//if there are a many siultaneous request-threads it causes the listener to shut down often.
				// that is why we add 1 to loop, if the condition was a broadcast.
				loop++;
			}
			// reread pms data to make sure it's me that has to connect
			if (!get_pms_data(&pmsdata, inst, mqs)) {
				radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid %s : pms %s:%s : Cannot read pms data from SPPDB. Listener thread won't start.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
				loop = 0;
			}
			// save ECOT to DB.
			if (!pmsdata->ecot || pmsdata->ecot == 0) {
				//fixme: set  pmsdata->ecot = time(NULL)
				pms_set_ecot(inst, pmsdata, time(NULL));
			}
		}
		//another cancel point obsolete
		// pthread_testcancel();
	}

	if (pmssocket->shutdown == 1) {
		radlog(L_INFO, "SC_HSPGW_PMS_DISCONNECTED : n/a : %s : %s:%s : Shutting down listener thread and releasing the socket.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
	}
	else {
		radlog(L_INFO, "SC_HSPGW_PMS_DISCONNECTED : n/a : %s:%s : %s : Could not reconnect on socket after %u trials. Releasing the socket.",
		pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, inst->config->pms_conf_sckt_max_trials);
	}

	changeSockState(pmssocket, pending, 1);
	pms_fias_disconnect(pmssocket);
	pms_fias_releaseSocket(pmssocket);
	pmssocket->thread = 0;

	free(arg);

	return NULL;
}

static int pms_fias_notifyReceiveMessage(PMS_SOCKET *pmssocket) {
	int rcode = 0;

	//DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: try to lock the cond_rcvmsg_mutex");
	rcode = pthread_mutex_lock(&pmssocket->cond_rcvmsg_mutex);
	if (rcode != 0) {
		DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: Failed to lock the cond_rcvmsg_mutex");
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: try to broadcast the cond_rcvmsg");
	pthread_cond_broadcast(&pmssocket->cond_rcvmsg);

	//DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: try to unlock the cond_rcvmsg_mutex");
	rcode = pthread_mutex_unlock(&pmssocket->cond_rcvmsg_mutex);
	if (rcode != 0) {
		DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: Failed to unlock the cond_rcvmsg_mutex");
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_notifyReceiveMessage: done.");
	return 0;
}

static int pms_fias_notifyThreadRestart(PMS_SOCKET *pmssocket) {
	int rcode = 0;

	DEBUG3("pms_fias::pms_fias_notifyThreadRestart: try to lock the cond_rstthread_mutex");
	rcode = pthread_mutex_lock(&pmssocket->cond_rstthread_mutex);

	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Failed to lock the cond_rstthread_mutex: %s.",
			pmssocket->ipaddr, pmssocket->port, strerror(rcode));
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_notifyThreadRestart: try to broadcast the cond_rstthread");
	rcode = pthread_cond_broadcast(&pmssocket->cond_rstthread);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Failed to broadcast the cond_rstthread: %s.",
			pmssocket->ipaddr, pmssocket->port, strerror(rcode));
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_notifyThreadRestart: try to unlock the cond_rstthread_mutex");
	rcode = pthread_mutex_unlock(&pmssocket->cond_rstthread_mutex);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Failed to unlock the cond_rstthread_mutex: %s.", pmssocket->ipaddr, pmssocket->port, strerror(rcode));
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_notifyThreadRestart: done.");
	return 0;
}

static int pms_fias_waitThreadRestart(PMS_SOCKET *pmssocket, int round) {
	int result = 0;

	struct timeval now;
	struct timespec timeout;

	gettimeofday(&now, NULL);
	//    timeout.tv_sec = now.tv_sec + round * 10;
	timeout.tv_sec = now.tv_sec + round * 2;
	timeout.tv_nsec = now.tv_usec * 1000;

	int rcode = 0;

	DEBUG3("pms_fias::pms_fias_waitThreadRestart: try to lock the cond_rstthread_mutex");
	rcode = pthread_mutex_lock(&pmssocket->cond_rstthread_mutex);
	if (rcode != 0) {
		DEBUG3("pms_fias::pms_fias_waitThreadRestart: Failed to lock the cond_rstthread_mutex");
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_waitThreadRestart: try to wait for the cond_rstthread on cond_rstthread_mutex");
	result = pthread_cond_timedwait(&pmssocket->cond_rstthread,
			&pmssocket->cond_rstthread_mutex, &timeout);

	DEBUG3("pms_fias::pms_fias_waitThreadRestart: try to unlock the cond_rstthread_mutex");
	rcode = pthread_mutex_unlock(&pmssocket->cond_rstthread_mutex);
	if (rcode != 0) {
		DEBUG3("pms_fias::pms_fias_waitThreadRestart: Failed to unlock the cond_rstthread_mutex");
		return -1;
	}

	DEBUG3("pms_fias::pms_fias_waitThreadRestart: return %d", result);
	return result;
}

static int pms_fias_lockSocket(PMS_SOCKET *pmssocket) {

	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_lockSocket: try to lock the socket_mutex");
	rcode = pthread_mutex_lock(&sckt->socket_mutex);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not lock the socket: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}

	//DEBUG3("pms_fias::pms_fias_lockSocket: the socket_mutex is locked");
	return 0;
}

static int pms_fias_lockSendQueue(PMS_SOCKET *pmssocket) {
	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_lockSendQueue: try to lock the sndqueue_mutex");
	rcode = pthread_mutex_lock(&sckt->sndqueue_mutex);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not lock the sndqueue_mutex: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}

	//DEBUG3("pms_fias::pms_fias_lockSendQueue: the sndqueue_mutex is locked");
	return 0;
}

static int pms_fias_lockReceiveQueue(PMS_SOCKET *pmssocket) {

	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_lockReceiveQueue: try to lock the rcvqueue_mutex");
	rcode = pthread_mutex_lock(&sckt->rcvqueue_mutex);
	if (rcode != 0) {
		radlog(L_ERR,"SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not lock the rcvqueue_mutex: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}
	//DEBUG3("pms_fias::pms_fias_lockReceiveQueue: the rcvqueue_mutex is locked");
	return 0;
}

static int pms_fias_unlockSocket(PMS_SOCKET *pmssocket) {
	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_unlockSocket: try to unlock the socket_mutex");
	rcode = pthread_mutex_unlock(&sckt->socket_mutex);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not unlock the socket: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}
	//DEBUG3("pms_fias::pms_fias_unlockSocket: the socket_mutex is unlocked");
	return 0;
}

static int pms_fias_unlockSendQueue(PMS_SOCKET *pmssocket) {
	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_unlockSendQueue: try to unlock the sndqueue_mutex");
	rcode = pthread_mutex_unlock(&sckt->sndqueue_mutex);
	if (rcode != 0) {
		radlog(L_ERR,"SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not unlock the sndqueue_mutex: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}
	//DEBUG3("pms_fias::pms_fias_unlockSendQueue: the sndqueue_mutex is unlocked");
	return 0;
}

static int pms_fias_unlockReceiveQueue(PMS_SOCKET *pmssocket) {
	PMS_SOCKET *sckt = pmssocket;
	int rcode;

	//DEBUG3("pms_fias::pms_fias_unlockReceiveQueue: try to unlock the rcvqueue_mutex");
	rcode = pthread_mutex_unlock(&sckt->rcvqueue_mutex);
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Could not unlock the rcvqueue_mutex: %s",
			sckt->ipaddr, sckt->port, strerror(rcode));
		return -1;
	}
	//DEBUG3("pms_fias::pms_fias_unlockReceiveQueue: the rcvqueue_mutex is unlocked");
	return 0;
}

static void changeSockState(PMS_SOCKET *pmssocket, enum State sockstate, int shutdown)
{

	pms_fias_lockSocket(pmssocket);

	pmssocket->state = sockstate;
	pmssocket->shutdown = shutdown;

	pms_fias_unlockSocket(pmssocket);

}

/*************************************************************************
 *
 *    Function: pms_fias_releaseSocket
 *
 *    Purpose: Close and free a PMS socket
 *  @ void *instance: PMS instance
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static void pms_fias_releaseSocket(PMS_SOCKET *pmssocket) {

	int ret;
	DEBUG2("pms_fias::pms_fias_releaseSocket: Close and free a PMS socket.");

	/*    else if (pmssocket->state == pmssockunconnected) {
	 radlog(L_INFO,"socket unconnected.");
	 }else if (pmssocket->state == pending) {
	 radlog(L_INFO,"socket pending.");
	 }
	 */
	pms_fias_close_socket(pmssocket);

	// Unlock the PMS socket
#ifdef HAVE_PTHREAD_H

	ret = pthread_mutex_unlock(&pmssocket->socket_mutex);
	if (ret != 0)
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Cannot unlock socket mutex: %s",
			pmssocket->ipaddr, pmssocket->port, strerror(ret));
	ret = pthread_mutex_unlock(&pmssocket->rcvqueue_mutex);
	if (ret != 0)
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Cannot unlock rcvqueue_mutex: %s",
			pmssocket->ipaddr, pmssocket->port, strerror(ret));
	ret = pthread_mutex_unlock(&pmssocket->sndqueue_mutex);
	if (ret != 0)
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Cannot unlock sndqueue_mutex: %s",
			pmssocket->ipaddr, pmssocket->port, strerror(ret));
	ret = pthread_mutex_unlock(&pmssocket->cond_rstthread_mutex);
	if (ret != 0)
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s:%s : Cannot unlock cond_rstthread_mutex: %s",
			pmssocket->ipaddr, pmssocket->port, strerror(ret));

	pms_fias_freeSendMessageQueue(pmssocket);
	pms_fias_freeReceiveMessageQueue(pmssocket);

#endif

	pmssocket->state = pmssockunconnected;
	pmssocket->snd_msg_queue = NULL;
	pmssocket->rcv_msg_queue = NULL;
	pmssocket->sockfd = 0;
	pmssocket->seqnumber = 0;
	pmssocket->shutdown = 0;

	//unlock pmssocket
	pms_fias_unlockSocket(pmssocket);

}

/*************************************************************************
 *
 *    Function: pms_send_config_records
 *
 *    Purpose: Send configuration records to PMS server
 *  @ int sockfd: socket file descriptor
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static int pms_fias_send_config_records(PMS_SOCKET *pmssocket, pms_data *pmsdata) {
	time_t now;
	struct tm *tm_now;
	char dbuff[BUFSIZ];
	char tbuff[BUFSIZ];
	char message[MAX_FIAS_BUFFER_SIZE];

	now = time(NULL);
	tm_now = localtime(&now);
	strftime(dbuff, sizeof dbuff, "%y%m%d", tm_now);
	strftime(tbuff, sizeof tbuff, "%H%M%S", tm_now);

	// Send "LD|IFWW|V#PWLAN Gateway x.y|"
	sprintf(message, PROTEL_FIAS_LD_IFWW, "2.0", dbuff, tbuff);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Description'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}
	// Send "LR|RIPR|FLP#RNPIG#PTSOTACTWSDATI|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIPR);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}
	// Send "LR|RIPA|FLP#RNASCTWSDATI|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIPA);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}
	// Send "LR|RIPL|FLP#DATIRNG#GN|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIPL);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}

	// Send "LR|RIGI|FLRNG#GSGNDATI|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIGI);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}

	// Send "LR|RIGO|FLRNG#GSDATI|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIGO);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}

	// Send "LR|RIGC|FLRNG#GSGNDATI|"
	sprintf(message, "%s", PROTEL_FIAS_LR_RIGC);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}

	/*
	 // Send "LR|RIDR|FIDATI|"
	 sprintf(message, PROTEL_FIAS_LR_RIDR);
	 DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	 if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
	 return -1;
	 }

	 // Send "LR|RIDS|FIDATI|"
	 sprintf(message, PROTEL_FIAS_LR_RIDS);
	 DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	 if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
	 return -1;
	 }

	 // Send "LR|RIDE|FIDATI|"
	 sprintf(message, PROTEL_FIAS_LR_RIDE);
	 DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Record'");
	 if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
	 return -1;
	 }
	 */
	// Send "LA|DAyymmdd|TIhhmmss|"
	sprintf(message, PROTEL_FIAS_LA, dbuff, tbuff);
	DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Link Alive'");
	if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
		return -1;
	}

	// Send "DR|DRyymmdd|TIhhmmss|"
	/*    sprintf(message, PROTEL_FIAS_DR, dbuff, tbuff);
	 sleep(2); // get a LA back can need some seconds
	 DEBUG2("pms_fias::pms_send_config_records: Send packet type 'Database Resync'");
	 if (pms_fias_send_packet(pmssocket, message, pmsdata) == -1) {
	 return -1;
	 }
	 */
	return 0;
}

/************************************************************************
 * Function:    updates the time stamp of last db synch
 */
static void pms_fias_upd_dbs_ts(void *instance, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	char mqs[MAX_QUERY_LEN];
	SQLSOCK *sqlsocket;

	sprintf(mqs, inst->config->pms_udp_dbsynch_ts, time(NULL), pmsdata->config_id);
	DEBUG(mqs);

	// get sql socket
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, mqs)) {
		radlog(L_ERR, "pms_fias::pms_fias_upd_dbs_ts: Update failed: %s", mqs);
		//       return -1; --> danger
	}

	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return;
}

/************************************************************************
 * Function:    process a PMS GO message
 */

static void proc_GO(void *instance, PMS_FIAS_PACKET *pkt, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQL_ROW row = NULL;
	SQLSOCK *sqlsocket;
	char mqs[MAX_QUERY_LEN];
	char gid[10] = "0";

	if (pkt->isdbsynch == 0) {
		strcpy(gid, pkt->guestlist->guestid);
	}

	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	//radlog(L_INFO, "creting query");
	sprintf(mqs, inst->config->pms_proc_go, pmsdata->config_id, pkt->guestlist->roomnumber, gid, pkt->isdbsynch);
	//radlog(L_INFO, mqs);
	DEBUG3(mqs);

	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Processing of GO on SPP data base failed.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
	} else {
		if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) != 0 || sqlsocket->row == NULL) {
			//can happen:
			DEBUG2("SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Could not fetch row while executing pms_proc_go on SPP data base.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		} else {
			// Get the values
			row = sqlsocket->row;
			if (row == NULL) {
				//can happen
				DEBUG2("SC_HSPGW_ORA_NO_DATA : sid n/a : zid %s : pms %s:%s : No data while executing pms_proc_go on SPP data base.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
			}
		}

	}

	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return;
}

/************************************************************************
 * Function:    process a PMS GO message
 */

static void proc_GI(void *instance, PMS_FIAS_PACKET *pkt, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQL_ROW row = NULL;
	SQLSOCK *sqlsocket;
	char mqs[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];
	char guestname[PMS_PASSWORD_MAX_LEN + 1];

	if(pkt->guestlist == NULL || pkt->guestlist->guestname == NULL) {
		radlog(L_ERR, "SC_HSPGW_PMS_UNEXPECTED_MSG : sid n/a : zid %s : pms %s:%s : Missing GN field in GI message. Suspending execution of proc_gc",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
				return;
	}

//should be done in splipackage:
//	convert2ascii(guestname, sizeof(guestname), pkt->guestlist->guestname);

//	dupquotes(modstr, guestname);
	dupquotes(modstr, pkt->guestlist->guestname);

	sprintf(mqs, inst->config->pms_proc_gi, pmsdata->config_id, pkt->guestlist->roomnumber, modstr, pkt->guestlist->guestid, pkt->isdbsynch);
	DEBUG3(mqs);
	//radlog(L_INFO, "GI query: %s", mqs);

	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Processing of GI on SPP data base failed.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
	} else {
		if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) != 0 || sqlsocket->row == NULL) {
			//can happen:
			DEBUG2("SC_HSPGW_ORA_ERROR : n/a : %s : %s:%s : Could not fetch row while executing pms_proc_gi on SPP data base.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		} else {
			// Get the values
			row = sqlsocket->row;
			if (row == NULL) {
				//can happen
				DEBUG2("SC_HSPGW_ORA_NO_DATA : n/a : %s : %s:%s : No data while executing pms_proc_gi on SPP data base.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
			}
		}
	}

	// finish query and release sql socket
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return;
}

static void pms_fias_occupancyInsert(void *instance, pms_tracker_data *trckrdata, pms_data *pmsdata){
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQL_ROW row = NULL;
	SQLSOCK *sqlsocket;
	char mqs[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];

//	dupquotes(modstr, trckrdata->pwd);
	dupquotes(modstr, trckrdata->pwd2); // hm/24.04.2014
	sprintf(mqs, inst->config->pms_proc_gi, pmsdata->config_id, trckrdata->usr, modstr, trckrdata->guestid, 0);
	DEBUG3(mqs);
	//radlog(L_INFO, "GI query: %s", mqs);

	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {
		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Processing of GI on SPP data base failed.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
	} else {
		if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) != 0 || sqlsocket->row == NULL) {
			//can happen:
			DEBUG2("SC_HSPGW_ORA_ERROR : n/a : %s : %s:%s : Could not fetch row while executing pms_proc_gi on SPP data base.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		} else {
			// Get the values
			row = sqlsocket->row;
			if (row == NULL) {
				//can happen
				DEBUG2("SC_HSPGW_ORA_NO_DATA : n/a : %s : %s:%s : No data while executing pms_proc_gi on SPP data base.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
			}
		}
	}

	// finish query and release sql socket
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	return;

}

/*************************************************************************
 *
 *    Function: pms_fias_getSeqNr
 *
 *    Purpose: Send a packet to Protel PMS server
 *  @ int seqnumber: sequence number
 *  Return the next sequence number
 *
 *  Author: Juan Vasquez
 *  Last changed: 24.11.2009
 *************************************************************************/
static int pms_fias_getSeqNr(int seqnumber) {
	if (++seqnumber > MAX_FIAS_SEQUENCENUMBER) {
		seqnumber = 1;
	}
	return seqnumber;
}

/*************************************************************************
 *
 *    Function: pms_fias_send_packet
 *
 *    Purpose: Send a packet to Protel PMS server
 *  @ void *instance: instance
 *  @ char *msg: the packet
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static int pms_fias_send_packet(PMS_SOCKET *pmssocket, char *msg, pms_data *pmsdata) {
	char *errmsg;
	int msg_length, packet_length, i, ret;
	char *packet, *thepacket;
	int sockfd = pmssocket->sockfd;
	char message[128];
	//char ip[16];
	char zid[PMS_ZONEID_MAX_LEN + 1];

	strcpy(message, msg);
	// Allocate memory
	msg_length = strlen(msg);
	packet_length = msg_length + 3;
	packet = rad_malloc(packet_length);
	memset(packet, '\0', packet_length);
	thepacket = packet;

	// Start of Text
	*packet = PROTEL_FIAS_STX;
	packet++;
	for (i = 0; i < msg_length; i++) {
		*packet++ = *msg++;
	}
	// End of Text
	*packet++ = PROTEL_FIAS_ETX;
	*packet = '\0';

	if (pmsdata != NULL) {
		strcpy(zid, pmsdata->zoneid);
	} else {
		strcpy(zid, "n/a");
	}

	// Send the packet
	DEBUG2("pms_fias::pms_fias_send_packet: Sending packet: '%s'", thepacket);
	radlog(L_INFO, "SC_HSPGW_PMS_SND_MSG : sid n/a : zid %s : pms %s:%s : %s",
		zid, pmssocket->ipaddr, pmssocket->port, message);

	pms_fias_lockSocket(pmssocket);
	ret = send(sockfd, thepacket, strlen(thepacket), 0);
	errmsg = strerror(errno);
	pms_fias_unlockSocket(pmssocket);

	//radlog(L_INFO, "SC_HSPGW_PMS_SND_MSG : %d, %d, % s", ret, errno, errmsg);
	if (ret == -1) {
		//if (ret != strlen(thepacket)) {
		radlog(L_INFO, "SC_HSPGW_PMS_SND_ERR : sid n/a : zid %s : pms %s:%s : Error sending message to PMS: %s",
		zid, pmssocket->ipaddr, pmssocket->port, errmsg);
		// Free memory => glibc detected munmap_chunk(): invalid pointer !!!
		free(thepacket);
		thepacket = NULL;
		return -1;
	}

	// Free memory
	free(thepacket);
	thepacket = NULL;
	return 0;
}

/*************************************************************************
 *
 *    Function: send_last_packet
 *
 *    Purpose: Send a packet to Protel PMS server
 *  @ void *instance: instance
 *  @ char *msg: the packet
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 05.01.2010
 *************************************************************************/
static int send_last_packet(PMS_SOCKET *pmssocket, char *msg, pms_data *pmsdata) {
	char *errmsg;
	int msg_length, packet_length, i, ret;
	char *packet, *thepacket;
	int sockfd = pmssocket->sockfd;
	char message[128];
	//char ip[16];
	char zid[PMS_ZONEID_MAX_LEN + 1];

	strcpy(message, msg);
	// Allocate memory
	msg_length = strlen(msg);
	packet_length = msg_length + 3;
	packet = rad_malloc(packet_length);
	memset(packet, '\0', packet_length);
	thepacket = packet;

	// Start of Text
	*packet = PROTEL_FIAS_STX;
	packet++;
	for (i = 0; i < msg_length; i++) {
		*packet++ = *msg++;
	}
	// End of Text
	*packet++ = PROTEL_FIAS_ETX;
	*packet = '\0';

	if (pmsdata != NULL) {
		strcpy(zid, pmsdata->zoneid);
	} else {
		strcpy(zid, "n/a");
	}

	// Send the packet
	DEBUG2("pms_fias::send_last_packet: Sending packet: '%s'", thepacket);
	radlog(L_INFO, "SC_HSPGW_PMS_SND_MSG : sid n/a : zid %s : pms %s:%s : %s",
		zid, pmssocket->ipaddr, pmssocket->port, message);

	//    pms_fias_lockSocket(pmssocket);
	ret = send(sockfd, thepacket, strlen(thepacket), 0);
	errmsg = strerror(errno);
	//    pms_fias_unlockSocket(pmssocket);

	if (ret == -1) {
		//if (ret != strlen(thepacket)) {
		radlog(L_INFO, "SC_HSPGW_PMS_SND_ERR : sid n/a : zid %s : pms %s : Error sending message to PMS: %s", zid, pmssocket->ipaddr, errmsg);
		// Free memory => glibc detected munmap_chunk(): invalid pointer !!!
		free(thepacket);
		thepacket = NULL;
		return -1;
	}

	// Free memory
	free(thepacket);
	thepacket = NULL;
	return 0;
}

/**
 * 0:success
 * -1:error
 */
static int pms_fias_send_message(PMS_SOCKET *pmssocket, char message[MAX_FIAS_BUFFER_SIZE]) {
	SEND_MESSAGE_QUEUE *last = NULL;
	SEND_MESSAGE_QUEUE *current = NULL;
	SEND_MESSAGE_QUEUE *newNode = NULL;
	int ret = 0;

	if (pmssocket == NULL || pmssocket->state != pmssockconnected) {
		return -2;
	}

	DEBUG3("pms_fias::pms_fias_send_message: try to lock the SendQueue");
	ret = pms_fias_lockSendQueue(pmssocket);
	if (ret < 0) {
		DEBUG3("pms_fias::pms_fias_send_message: failed to lock the SendQueue");
		return -3;
	}

	// Allocate memory for socket pool
	newNode = rad_malloc(sizeof(SEND_MESSAGE_QUEUE));
	memset(newNode, 0, sizeof(SEND_MESSAGE_QUEUE));
	strcpy(newNode->message, message);
	newNode->next = NULL;

	//if NULL, assign new node
	if (pmssocket->snd_msg_queue == NULL) {
		pmssocket->snd_msg_queue = newNode;
	}

	else {

		current = pmssocket->snd_msg_queue;

		while (current) {
			last = current;
			current = current->next;
		}

		//add new node
		last->next = newNode;

	}

	DEBUG3("pms_fias::pms_fias_send_message: try to unlock the SendQueue");
	ret = pms_fias_unlockSendQueue(pmssocket);
	if (ret < 0) {
		DEBUG3("pms_fias::pms_fias_send_message: failed to unlock the SendQueue");
		return -4;
	}

	DEBUG3("pms_fias::pms_fias_send_message: done.");
	return 0;

}

/*************************************************************************
 *
 *    Function: pms_check_packet_type
 *
 *    Purpose: Check the Protel PMS packet record type
 *  @ char *msg: the packet
 *  @ char *msgtype: the expected record type
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 11.11.2009
 *************************************************************************/
static int pms_fias_check_packet_type(char *msg, char *msgtype) {
	if (strncmp(msg, msgtype, 2) != 0) {
		return -1;
	}
	return 0;
}

/**
 * 1: found
 * -1: error
 * -2: timeout
 */
static int get_posting_answer(PMS_SOCKET *pmssocket, char *message, int seqnumber, char *type) {
	int found = 0;
	int retries = 4;

	while (!found && (retries > 0)) {
		retries--;
		//wait a little longer
		sleep(1);

		pms_fias_lockReceiveQueue(pmssocket);

		RECEIVE_MESSAGE_QUEUE *current = pmssocket->rcv_msg_queue;

		while (current != NULL) {
			// radlog(L_INFO, "currentmsg: %u: %s", current->seqnumber, current->message);

			// ugly workaround: it is possible, that several threads wake up due to the broadcast condition,
			// though the specific message did not come in yet. The result is, that the user gets a timeout
			// and the queue is growing. the workaround just invalidates older sequence numbers:
			if ((current->seqnumber > 0 && current->seqnumber < seqnumber - 10)) {
				current->seqnumber = -1;
			}

			if ((current->seqnumber > 0 && current->seqnumber == seqnumber)
					|| (type != NULL && pms_fias_check_packet_type(current->message, type) == 0)) {

				found = 1;
				strcpy(message, current->message);
				current->seqnumber = -1;
				break;
			}

			current = current->next;
		}

		pms_fias_unlockReceiveQueue(pmssocket);
	}
	//}

	//    pms_fias_unlockReceiveQueue(pmssocket);


	if (found) {
		return 1;
	} else {
		return -7;
	}

}

/**
 * 1: found
 * -1: error
 * -2: timeout
 */
static int pms_fias_get_message(PMS_SOCKET *pmssocket, char *message, int seqnumber, char *type) {
	int rcode = 0;
	int found = 0;
	struct timespec delay;

	delay.tv_sec = 0;
	delay.tv_nsec = 200000000L; //half second in nano

	// Wait until timeout or data receive
	/*
	 if (pmssocket->state == pmssockunconnected) {
	 return -2;
	 }
	 */
//	radlog(L_INFO, "pms_fias_get_message (%s : %s): try to waitReceiveMessage", pmssocket->ipaddr, pmssocket->port);
	rcode = pms_fias_waitReceiveMessage(pmssocket);
//	radlog(L_INFO, "pms_fias_get_message (%s : %s):waitReceiveMessage returned %d", pmssocket->ipaddr, pmssocket->port, rcode);
	//    sleep(1);

	if (rcode == ETIMEDOUT) {
		found = 0;
	} else if (rcode == -1) {
//		radlog(L_INFO, "pms_fias_get_message (%s : %s): returns -4", pmssocket->ipaddr, pmssocket->port);
		return -4;
	} //else {

	if (rcode != ETIMEDOUT) {
		//wait a little longer
		nanosleep(&delay, NULL);
		sleep(1);
	}
	// DEBUG3("pms_fias::pms_fias_get_message: try to lock the ReceiveQueue ...");
	pms_fias_lockReceiveQueue(pmssocket);
	// DEBUG3("pms_fias::pms_fias_get_message: ReceiveQueue is locked.");

	RECEIVE_MESSAGE_QUEUE *current = pmssocket->rcv_msg_queue;

	while (current != NULL) {
		// radlog(L_INFO, "currentmsg: %u: %s", current->seqnumber, current->message);

		// ugly workaround: it is possible, that several threads wake up due to the broadcast condition,
		// though the specific message did not come in yet. The result is, that the user gets a timeout
		// and the queue is growing. the workaround just invalidates older sequence numbers:
		if ((current->seqnumber > 0 && current->seqnumber < seqnumber - 10)) {
			current->seqnumber = -1;
		}

		if ((current->seqnumber > 0 && current->seqnumber == seqnumber)
					|| (type != NULL && pms_fias_check_packet_type(current->message, type) == 0)) {

			found = 1;
			strcpy(message, current->message);
			current->seqnumber = -1;
			break;
		}

		current = current->next;
	}

	//DEBUG3("pms_fias::pms_fias_get_message: try to unlock the ReceiveQueue ...");
	pms_fias_unlockReceiveQueue(pmssocket);
	//DEBUG3("pms_fias::pms_fias_get_message: ReceiveQueue is unlocked.");

	//}

	//    pms_fias_unlockReceiveQueue(pmssocket);


	// DEBUG3("pms_fias::pms_fias_get_message: found=%d", found);
	if (found) {
		rcode= 1;
	} else {
		rcode = -7;
	}
//	radlog(L_INFO, "pms_fias_get_message (%s : %s): returns %d", pmssocket->ipaddr, pmssocket->port, rcode);
	return rcode;
}

static int pms_fias_waitReceiveMessage(PMS_SOCKET *pmssocket) {
	struct timeval now;
	struct timespec timeout;
	int rcode = 0;

	gettimeofday(&now, NULL);
	timeout.tv_sec = now.tv_sec + pmssocket->timeouts; // original value: 4
	timeout.tv_nsec = now.tv_usec * 1000;

	//DEBUG3("pms_fias_waitReceiveMessage: try to lock cond_rcvmsg_mutex");
	rcode = pthread_mutex_lock(&pmssocket->cond_rcvmsg_mutex);
	DEBUG3("pms_fias_waitReceiveMessage: cond_rcvmsg_mutex is locked");
	if (rcode != 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms %s : Failed to lock cond_rcvmsg_mutex: %s",
				pmssocket->ipaddr,
				strerror(rcode));
		return -1;
	}

//	radlog(L_INFO, "pms_fias_waitReceiveMessage (%s : %s): wait for cond_rcvmsg_mutex", pmssocket->ipaddr, pmssocket->port);
	rcode = pthread_cond_timedwait(&pmssocket->cond_rcvmsg, &pmssocket->cond_rcvmsg_mutex, &timeout);
//	radlog(L_INFO, "pms_fias_waitReceiveMessage (%s : %s): wait for cond_rcvmsg_mutex returned %d", pmssocket->ipaddr, pmssocket->port, rcode);
	if (rcode == ETIMEDOUT) {
		DEBUG3("pms_fias_waitReceiveMessage: Time out");
	} else {
		DEBUG3("pms_fias_waitReceiveMessage: notified");
	}

	pthread_mutex_unlock(&pmssocket->cond_rcvmsg_mutex);
	DEBUG3("pms_fias_waitReceiveMessage: cond_rcvmsg_mutex is unlocked");

	return rcode;
}

/**
 * return received bytes
 * -2 : timeout
 * -1: error
 */
static int pms_fias_recprocmgs(void *instance, PMS_SOCKET *pmssocket, pms_data *pmsdata) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	// recv
	int lastchar;
	fd_set rset;
	struct timeval tval;
	int n, nsec;
	int sockfd = pmssocket->sockfd;
	// Set time out in seconds
	nsec = pmssocket->timeouts;

	// Set up the file descriptor set
	FD_ZERO(&rset);
	FD_SET(sockfd, &rset);
	// Set up the struct timeval for the timeout
	tval.tv_sec = 1; // last original value: 2
	tval.tv_usec = 10000;

	char buffer[MAX_FIAS_BUFFER_SIZE] = "";
	char message[MAX_FIAS_BUFFER_SIZE * 100] = "";

	regex_t rx;
	int ret;
	char *msgp = NULL;
	char *smp = NULL;
	char *delim = "\x02"; //PROTEL_FIAS_STX;
	char *pattern = "^[a-z,A-Z][a-z,A-Z]|[a-z,A-Z].+\|\x03"; // stable?...

	pms_fias_lockSocket(pmssocket);

	// Wait until timeout or data received
	while ((n = select(sockfd + 1, &rset, NULL, NULL, &tval)) < 0) {

		if (errno != EINTR) {
			radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : sid n/a : zid %s : pms %s:%s : Error while select on socket: %s",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port, strerror(errno));
			//DEBUG2("pms_fias::pms_fias_receive_packet: Select error for sockfd %d (%s)", sockfd, strerror(errno));
			pms_fias_close_socket(pmssocket);
			pmssocket->state = pmssockunconnected;
			pms_fias_unlockSocket(pmssocket);
			return -1;
		}
	}
	//    pms_fias_unlockSocket(pmssocket);
	if (n == 0) {
		pms_fias_unlockSocket(pmssocket);
		return -2; // Timeout!
	}

	if (regcomp(&rx, pattern, REG_EXTENDED | REG_NOSUB) != 0) {
		pms_fias_unlockSocket(pmssocket);
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid n/a : pms n/a : Invalid regular expression in pms_fias_recprocmgs.");
		ret = -3;
	}

	// Data must be here, so do a normal recv()
	while((ret = recv(sockfd, buffer, MAX_FIAS_BUFFER_SIZE, 0)) > 0 && strlen(message) < MAX_FIAS_BUFFER_SIZE*99) {

		lastchar = (ret == MAX_FIAS_BUFFER_SIZE) ? (ret - 1) : ret;
		buffer[lastchar] = '\0';
		strcat(message, buffer);
	}
	pms_fias_unlockSocket(pmssocket);

	//msgp = strtok(message, delim);
	msgp = strtok_r(message, delim, &smp);

	while (msgp != NULL && strlen(msgp) > 0) {
		if (regexec(&rx, msgp, (size_t) 0, NULL, 0) == 0) { // at least one complete message

			// work on it
			msgp[strlen(msgp) - 1] = '\0';
			if ((pms_fias_check_packet_type(msgp, PROTEL_FIAS_RECID_PA) == 0)
			|| (pms_fias_check_packet_type(msgp, PROTEL_FIAS_RECID_PL) == 0)) {
				pms_fias_insertReceiveMessage(msgp, pmssocket);
			}
			pms_fias_handle_message(inst, pmssocket, msgp, pmsdata);
			msgp = strtok_r(NULL, delim, &smp);
		}
	}

	//free(matches);
	regfree(&rx);

	ret = 0;
	return ret;
}



/*************************************************************************
 *
 *    Function: pms_receive_packet
 *
 *    Purpose: Receive a packet from Protel PMS server
 *  @ void *instance: instance
 *  @ char *buffer: buffer
 *  Return -2 on timeout, -1 on failure, 0 on closed, packet length
 *
 *  Author: Juan Vasquez
 *  Last changed: 04.01.2010
 *************************************************************************/
static int pms_fias_receive(PMS_SOCKET *pmssocket, char *buffer) {

	int ret, lastchar, error, len;
	fd_set rset, wset;
	struct timeval tval;
	int n, nsec;
	int sockfd = pmssocket->sockfd;

	// Set time out in seconds
	nsec = pmssocket->timeouts;

	// Set up the file descriptor set
	FD_ZERO(&rset);
	FD_SET(sockfd, &rset);
	wset = rset;
	// Set up the struct timeval for the timeout
	//tval.tv_sec = 2;
	tval.tv_sec = nsec;
	tval.tv_usec = 0;

	pms_fias_lockSocket(pmssocket);

	// Wait until timeout or data received
	while ((n = select(sockfd + 1, &rset, NULL, NULL, &tval)) < 0) {

		if (errno != EINTR) {
			DEBUG2("pms_fias::pms_fias_receive_packet: Select error for sockfd %d (%s)", sockfd, strerror(errno));

			pms_fias_close_socket(pmssocket);
			pmssocket->state = pmssockunconnected;
			pms_fias_unlockSocket(pmssocket);
			return -1;

		}
	}

	if (n == 0) {
		pms_fias_unlockSocket(pmssocket);
		return -2; // Timeout!
	}

	// new (necessay?)

	if (FD_ISSET(pmssocket->sockfd, &rset) || FD_ISSET(pmssocket->sockfd, &wset)) {
		len = sizeof(int);
		error = 0;
		if (getsockopt(pmssocket->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
			DEBUG2( "pms_fias::pms_connect_on_socket(%s : %s): getsockopt failed %s ",
						pmssocket->ipaddr, pmssocket->port, strerror(errno));
			pms_fias_unlockSocket(pmssocket);
			return (-1); // Solaris pending error
		}

		if (error > 0) {
			DEBUG2("pms_fias::pms_connect_on_socket(%s : %s): pms_fias_fcntl_socket failed %s ",
						pmssocket->ipaddr, pmssocket->port, strerror(error));
			pms_fias_close_socket(pmssocket);
			errno = error;
			pms_fias_unlockSocket(pmssocket);
			return (-1);
		}
	} else {
		DEBUG("pms_fias::pms_connect_on_socket: Select error: sockfd not set");
		return (-1);
	}
	// end new

	// Data must be here, so do a normal recv()
	ret = recv(sockfd, buffer, MAX_FIAS_BUFFER_SIZE, 0);

	if (ret == 0) {
		DEBUG2( "pms_fias::pms_receive_packet: The remote side has closed the connection.");
		pms_fias_close_socket(pmssocket);
		pmssocket->state = pmssockunconnected;
	}

	if (ret < 0) {

		DEBUG2("pms_fias::pms_receive_packet: socket error by receive %s", strerror(ret));
		pms_fias_close_socket(pmssocket);
		pmssocket->state = pmssockunconnected;
	}

	pms_fias_unlockSocket(pmssocket);

	if (ret > 0) {
		lastchar = (ret == MAX_FIAS_BUFFER_SIZE) ? (ret - 1) : ret;
		buffer[lastchar] = '\0';
	}

	return ret;
}

/*************************************************************************
 *
 *    Function: pms_get_socket
 *
 *    Purpose: Request for socket
 *  @ int *sockfd: pointer to socket file descriptor
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 10.11.2009
 *************************************************************************/
//int pms_fias_create_socket(PMS_SOCKET **pmssocket) {
static int pms_fias_create_socket(int *sockfd) {
	*sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (*sockfd < 0) {
		//radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : %s :Could not open a socket for TCP communication: %s.", pmssocket->ipaddr, strerror(errno));
		return errno;
	}
	return 0;
}

/*************************************************************************
 *
 *    Function: pms_fias_ist_connected
 *
 *    Purpose: Primitive check for socket connection
 *  @ int *sockfd: pointer to socket file descriptor
 *  Return 0 is connected, otherwise error code

 *************************************************************************/
static int pms_fias_ist_connected(int sockfd) {

	int error = 0;
	socklen_t len = sizeof(error);
	int retval = getsockopt(sockfd, SOL_SOCKET, SO_ERROR, &error, &len);
	return retval;

}

/*************************************************************************
 *
 *    Function: pms_connect_on_socket
 *
 *    Purpose:    Open connection on the socket to Protel PMS server.
 *                Non-blocking connect as described in UNIX Network Programming
 *  @ int sockfd: socket file descriptor
 *  @ pms_data *list: PMS data pool
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 16.12.2009
 *************************************************************************/
static int pms_fias_connect_on_socket(void *instance, PMS_SOCKET *pmssocket) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	struct sockaddr_in sa;
	// For non-blocking
	int flags, retVal, error, nsec;
	socklen_t len;
	fd_set rset, wset;
	struct timeval tval;

	// Allocate memory for struct
	memset((char *) &sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons((u_short) atoi(pmssocket->port));
	sa.sin_addr.s_addr = inet_addr(pmssocket->ipaddr);

	// Set time out in seconds
	nsec = inst->config->pms_conf_sckt_conn_timeout;

	// Set file status flags to non-block
	flags = pms_fias_fcntl_socket(pmssocket->sockfd, F_GETFL, 0);
	pms_fias_fcntl_socket(pmssocket->sockfd, F_SETFL, flags | O_NONBLOCK);

	// First we call connect
	DEBUG2("pms_fias::pms_connect_on_socket: Open connection to '%s' using port '%s' ...", pmssocket->ipaddr, pmssocket->port);

	if ((retVal = connect(pmssocket->sockfd, (struct sockaddr *) &sa, sizeof(sa)))< 0) {
		// When doing a connect() on a non-blocking socket, it will immediately
		// return EINPROGRESS to let you know that the operation is in progress
		if (errno != EINPROGRESS) {
			DEBUG2("pms_fias::pms_connect_on_socket(%s : %s): connect failed: %s ", pmssocket->ipaddr, pmssocket->port, strerror(errno));
			radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : asynch : sid n/a : zid n/a : pms %s : pms_connect_on_socket : connection failed: %s ", pmssocket->ipaddr, strerror(errno));
			return (-1);
		}
	}

	// Do whatever we want while the connect is taking place
	DEBUG2("pms_fias::pms_connect_on_socket: ... connect operation is in progress");

	// Connect completed immediately
	//todo: check whether this is necessary:
	/*    if (retVal == 0) {
	 goto done;
	 }
	 */

	FD_ZERO(&rset);
	FD_SET(pmssocket->sockfd, &rset);
	wset = rset;
	tval.tv_sec = nsec;
	tval.tv_usec = 0;

	// Wait until timeout or data received
	if ((retVal = select(pmssocket->sockfd + 1, &rset, &wset, NULL, &tval)) == 0) {
		pms_fias_close_socket(pmssocket);
		errno = ETIMEDOUT; //fixme: for what set errno here, if we just return -1???????
		radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : asynch : sid n/a : zid n/a : pms %s : select on socket failed: %s ", pmssocket->ipaddr, strerror(errno));
		DEBUG("pms_fias::pms_connect_on_socket: Timeout");
		return (-1);
	}

	if (retVal < 0) {
		DEBUG("pms_fias::pms_connect_on_socket: Select error %s", strerror(errno));
		radlog(L_ERR,"SC_HSPGW_PMS_SOCKET_ERR : asynch : sid n/a : zid n/a : pms %s : select on socket failed: %s ", pmssocket->ipaddr, strerror(errno));
		return (-1);

	}

	if (FD_ISSET(pmssocket->sockfd, &rset) || FD_ISSET(pmssocket->sockfd, &wset)) {
		len = sizeof(error);
		error = 0;
		if (getsockopt(pmssocket->sockfd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
			DEBUG2( "pms_fias::pms_connect_on_socket(%s : %s): getsockopt failed %s ",
						pmssocket->ipaddr, pmssocket->port, strerror(errno));
			return (-1); // Solaris pending error
		}

		if (error > 0) {
			DEBUG2("pms_fias::pms_connect_on_socket(%s : %s): pms_fias_fcntl_socket failed %s ",
						pmssocket->ipaddr, pmssocket->port, strerror(error));
			pms_fias_close_socket(pmssocket);
			errno = error;
			return (-1);
		}
	} else {
		DEBUG("pms_fias::pms_connect_on_socket: Select error: sockfd not set");
		return (-1);
	}

	//done:
	// Restore file status flags
	pms_fias_fcntl_socket(pmssocket->sockfd, F_SETFL, flags | O_NONBLOCK);
	// Just in case


	return (0);
}

/*************************************************************************
 *
 *    Function: pms_close_socket
 *
 *    Purpose: Close the socket to Protel PMS server
 *  @ PMS_SOCKET *pmssocket
 *  Return -1 on failure, 0 on success
 *
 *  Author: Juan Vasquez
 *  Last changed: 23.11.2009
 *************************************************************************/
static int pms_fias_close_socket(PMS_SOCKET *pmssocket) {

	if (pmssocket->sockfd > 0) {
		if (close(pmssocket->sockfd) < 0) {
			 radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid  n/a : pms %s:%s  : Error closing sockfd %d : %s",
				pmssocket->ipaddr, pmssocket->port, pmssocket->sockfd, strerror(errno));
			pmssocket->sockfd = 0;
			return -1;
		} else {
			 radlog(L_INFO, "sid n/a : zid  n/a : pms %s:%s  : The sockfd %d is closed.",
				pmssocket->ipaddr, pmssocket->port, pmssocket->sockfd);
			pmssocket->sockfd = 0;
		}

	}

	return 0;

}

/*
 *  Control socket descriptors
 *  Parameter fd is the socket descriptor you wish to operate on, cmd should be set to F_SETFL,
 *  and arg can be one of the following commands: O_NONBLOCK , O_ASYNC
 */
static int pms_fias_fcntl_socket(int fd, int cmd, int arg) {

	int n;

	if ((n = fcntl(fd, cmd, arg)) == -1) {
		if (errno != EINPROGRESS) {
			DEBUG2("pms_fias::pms_fias_fcntl_socket: fcntrl error %s ", strerror(errno));
			return (-1);
		} else {
			return 0;
		}
	}
	return (n);
}

static void pms_fias_insertReceiveMessage(char *message, PMS_SOCKET *pmssocket) {

	PMS_FIAS_PACKET *packet = pms_fias_splitPacket(message);
	// just for test purposes
	// pms_fias_validatePacket(message, packet);

	pms_fias_lockReceiveQueue(pmssocket);

	//add to receive queue
	RECEIVE_MESSAGE_QUEUE *last = NULL;
	RECEIVE_MESSAGE_QUEUE *newNode = NULL;
	// Allocate memory for socket pool
	newNode = rad_malloc(sizeof(RECEIVE_MESSAGE_QUEUE));
	memset(newNode, 0, sizeof(RECEIVE_MESSAGE_QUEUE));
	strcpy(newNode->message, message);
	newNode->seqnumber = packet->seqnumber;
	newNode->next = NULL;
	//if NULL, assign new node
	if (pmssocket->rcv_msg_queue == NULL) {
		pmssocket->rcv_msg_queue = newNode;
	} else {
		//get last node
		last = pmssocket->rcv_msg_queue;
		while (last->next != NULL) {
			last = last->next;
		}
		//add new node
		last->next = newNode;
	}

	pms_fias_unlockReceiveQueue(pmssocket);

	free(packet);
}

static void pms_fias_rekur_removeInvalideMessage(RECEIVE_MESSAGE_QUEUE *head) {
	if (head == NULL) {
		return;
	}

	RECEIVE_MESSAGE_QUEUE *node = head ->next;

	if (node == NULL)
		return;

	if (node->seqnumber == -1) {
		head->next = node->next;
		free(node);
	}

	pms_fias_rekur_removeInvalideMessage(head->next);
}

static int pms_fias_removeInvalidateMessages(PMS_SOCKET* pmssocket) {

	pms_fias_lockReceiveQueue(pmssocket);

	RECEIVE_MESSAGE_QUEUE *tmp = rad_malloc(sizeof(RECEIVE_MESSAGE_QUEUE));
	memset(tmp, 0, sizeof(RECEIVE_MESSAGE_QUEUE));
	tmp->next = pmssocket->rcv_msg_queue;

	pms_fias_rekur_removeInvalideMessage(tmp);

	pmssocket->rcv_msg_queue = tmp->next;

	free(tmp);

	pms_fias_unlockReceiveQueue(pmssocket);

	return 0;
}

static int pms_fias_invalidateMessage(PMS_SOCKET* pmssocket, int seqnumber, char* type) {
	RECEIVE_MESSAGE_QUEUE *current = pmssocket->rcv_msg_queue;

	while (current != NULL) {
		if ((current->seqnumber > 0 && current->seqnumber == seqnumber) ||
			((type != NULL && pms_fias_check_packet_type(current->message, type) == 0) && (current->seqnumber != -1))) {
			current->seqnumber = -1;
			break;
		}

		current = current->next;
	}

	return 0;
}

/*************************************************************************
 *
 *    Function: pms_fias_consolidate_DB
 *
 *    Purpose: Internal functions to consolidate the table HSPGW_HSPGW_OCCUPANCY in SPP DB at the end of DB synch.
 *  @ void *instance: PMS instance
 *  Return 0 on success, -1 on failure
 *
 *  Author: Hongjuan Lin
 *  Last changed: 11.07.2011
 *************************************************************************/
static void pms_fias_consolidate_DB(void *instance, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQLSOCK *sqlsocket;
	SQL_ROW row = NULL;
	char mqs[MAX_QUERY_LEN];

	//replace the wildcard with value got from pmsdata
	sprintf(mqs, inst->config->pms_consolidate_db, pmsdata->config_id);
	DEBUG3("pms_consolidate_db query string: %s", mqs);

	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {

		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Consolidate HSPGW_OCCUPANCY on SPP database failed.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

	} else {
		if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) != 0 || sqlsocket->row == NULL) {

			DEBUG2("SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Could not fetch row while executing pms_consolidate_db on SPP data base.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		} else {

			// Get the values
			row = sqlsocket->row;
			if (row == NULL) {

				DEBUG2("SC_HSPGW_ORA_NO_DATA : sid n/a : zid %s : pms %s:%s : No data while executing pms_consolidate_db on SPP data base.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
			}
		}

	}

	// finish query
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);

	// clear timestamp
	//replace the wildcard with value got from pmsdata
	sprintf(mqs, inst->config->pms_clear_timestamp, pmsdata->config_id);
	DEBUG3("pms_clear_timestamp query: %s", mqs);

	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, mqs)) {

		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Cannot update dbsync time stamp.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

	}
	// finish query
	(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);

	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

}

/*************************************************************************
 *
 *    Function: pms_reset_ecot(inst, pmsdata, sqlsckt)
 *
 *    Purpose: Internal functions to reset ecot in table HSPGW_HSPGW_CONFIGURATION
 *    after DB synch.
 *  @ void *instance: PMS instance
 *  @ pms_data *pmsdata: pms configuration
 *  Return void
 *
 *  Author: hm/wdw
 *  History:     2011-07-27 hm/wdw: created
 *              2011-08-02 hm/wdw: added ecot param
 *************************************************************************/
static void pms_set_ecot(void *instance, pms_data *pmsdata, int ecot) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQLSOCK *sqlsocket;
	char mqs[MAX_QUERY_LEN];

	char ecotstr[11];

	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

	// convert the int
	if( ecot == NULL || ecot == 0) {
		strncpy(ecotstr,"null",4);
		ecotstr[4] = '\0';
	}
	else {
		sprintf(ecotstr, "%i", ecot);
		ecotstr[11] = '\0';
	}

	//replace the wildcard with value got from pmsdata
	sprintf(mqs, inst->config->pms_set_ecot, ecotstr, pmsdata->config_id);
	DEBUG3("pms_reset_ecot query: %s", mqs);

	if (inst->sql_inst->sql_query(sqlsocket, inst->sql_inst, mqs)) {

		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Cannot set ECOT on SPP data base",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
	}
	// finish query and release sql socket
	(inst->sql_inst->module->sql_finish_query)(sqlsocket, inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

}

/*************************************************************************
 *
 *    Function: get_pms_data
 *
 *    Purpose: Internal function for the PMS configuration data
 *  @ PMS_DATA_LIST **pmsdatalist: reference to pms_data_list
 *  @ void *inst: pms_1 module instance
 *  Return int
 *
 *  Author: shan/wdw
 *  Last changed: 2012-09-10
 *************************************************************************/

static int get_pmsdatalist(PMS_DATA_LIST **pmsdatalist, void *instance,	pms_logfile_data *logfiledata, char *qs, char *scope) {

	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQLSOCK *sqlsocket = NULL;
	SQL_ROW row;
	pms_data *pmsdata = NULL;
	PMS_DATA_LIST *list = NULL;
	PMS_DATA_LIST *last = NULL;

	char querystr[MAX_QUERY_LEN];
	char *msgtxt = NULL;
	char msg[MAX_QUERY_LEN];
	char key[64];

	strcpy(key, "SC_HSPGW_");

	if (scope == NULL) {
		msgtxt = "";
		scope = "";
	} else if (strcmp(scope, "AUTH") == 0) {
		msgtxt = "Authentication aborted.";
	} else if (strcmp(scope, "ACCT") == 0) {
		msgtxt = "Accounting aborted.";
	}

	strcat(key, scope);

	/*
	 * Get PMS Configuration from data pool
	 */
	strcpy(querystr, qs);
	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		strcpy(msg, "Connection to data base failed. ");
		strcat(msg, msgtxt);
		strcat(key, "_SPPDB_CONN_ERR"); // "SC_HSPGW_AUTH_SPPDB_CONN_ERR"
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s", key,
				logfiledata->sessid, logfiledata->zoneid,
				logfiledata->username, msg);
		return 0;
	}

	// Execute the select query
	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst,
			querystr)) {
		(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
				inst->sql_inst->config);
		inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
		strcpy(msg,
				"Database select query failed while querying pms data. ");
		strcat(msg, msgtxt);
		strcat(key, "_ORA_ERROR"); // "SC_HSPGW_AUTH_SPPDB_CONN_ERR"
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s", key,
				logfiledata->sessid, logfiledata->zoneid,
				logfiledata->username, msg);
		return 0;
	}

	// Fetch rows
	while (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) == 0) {

		// Get the values
		row = sqlsocket->row;

		if (row == NULL) {
			(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
					inst->sql_inst->config);
			inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
			strcpy(msg, "Could not get data while querying pms data. ");
			strcat(msg, msgtxt);
			strcat(key, "_ORA_NO_DATA");
			radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s",
					key, logfiledata->sessid, logfiledata->zoneid,
					logfiledata->username, msg);
			return 0;
		}

		// Build the PMS Configuration Data
		pmsdata = pms_fias_createConfigData(row);

		// Add the pmsdata to the List
		list = rad_malloc(sizeof(PMS_DATA_LIST));
		memset(list, 0, sizeof(PMS_DATA_LIST));
		list->pmsdata = pmsdata;
		list->next = NULL;

		if (last == NULL) {
			last = list;
			*pmsdatalist = last;
		} else {
			last->next = list;
			last = last->next;
		}
	}

	// Free the DB handles
	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket,
			inst->sql_inst->config);
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

	if (last == NULL) {
		strcpy(msg,
				"Could not fetch row from select query while querying pms data. ");
		strcat(msg, msgtxt);
		strcat(key, "_ORA_ERROR"); // "SC_HSPGW_AUTH_ORA_ERROR"
		radlog(L_ERR, "%s : req n/a : sid %s : zid %s : usr %s : %s", key,
				logfiledata->sessid, logfiledata->zoneid,
				logfiledata->username, msg);
		*pmsdatalist = NULL;
		return 0;
	}

	return 1;
}

/*************************************************************************
 *
 *    Function: reorder_pmsdatalist
 *
 *    Purpose:reorder a list of PMS so that the one with a given Zone ID
 *    comes first.
 *
 *  @ PMS_DATA_LIST *pmsdatalist_ptr: reference to pms_data_list
 *  @ char *zoneid_start            : zone id which has to come first
 *
 *  Author: 	am/wdw
 *  History:    2013-04-19 am/wdw: created
 *************************************************************************/
static void reorder_pmsdatalist(PMS_DATA_LIST **pmsdatalist_ptr, char *zoneid_start) {
	PMS_DATA_LIST *list;
	PMS_DATA_LIST *list_prev = NULL;
	for (list = *pmsdatalist_ptr; list != NULL; list = list->next) {
		// DEBUG2("pms zone id='%s', start zone id='%s'", list->pmsdata->zoneid, zoneid_start);
		if (strcmp (list->pmsdata->zoneid, zoneid_start) == 0) {
			// found!
			DEBUG2("found start zone id %s", list->pmsdata->zoneid);
			// if not already first element swap with first element
			if (list_prev != NULL) {
				list_prev->next = list->next;   // n-1 has n+1 as next
				list->next = *pmsdatalist_ptr;  // n has 1 as next
				*pmsdatalist_ptr = list;		// n is first now
			}
			break;
		}
		list_prev = list;
	}

}


/*************************************************************************
 *
 *    Function: is_obsolete(int ecot)
 *
 *    Purpose: check whether the date of ecot is at least one day in the past
 *    This is used as condition to trigger DB sync
 *
 *  @ time_t ecot:  timestamp of ecot
 *  Return 1: ecot is null or at least one day in the past
 *         0: otherwise
 *
 *  Author: zs/wdw
 *  History:     2012-07-09 zs/wdw: created
 *************************************************************************/
static int is_obsolete(time_t ecot) {
	struct tm *tm_now, *tm_ecot;


	if (ecot > 0) {

		time_t now = time(NULL);
		tm_now = localtime(&now);
		tm_ecot = localtime(&ecot);

		if(now-ecot < 86400)
			return 0;
		/*
		if (tm_now->tm_year == tm_ecot->tm_year ||
				tm_now->tm_mon == tm_ecot->tm_mon ||
				tm_now->tm_mday == tm_ecot->tm_mday){
			return 0;
		} */
	}

	return 1;
}

/*************************************************************************
 *
 *    Function: proc_GC
 *
 *    Purpose: Internal functions to handle GC message.
 *  @ void *instance: PMS instance
 *
 *  Author: Hongjuan Lin
 *  Last changed: 11.07.2011
 *************************************************************************/
static void proc_GC(void *instance, PMS_FIAS_PACKET *pkt, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	SQL_ROW row = NULL;
	SQLSOCK *sqlsocket;
	char mqs[MAX_QUERY_LEN];
	char modstr[PMS_PASSWORD_MAX_LEN + 1];
	char guestname[PMS_PASSWORD_MAX_LEN + 1];






	if(pkt->guestlist == NULL || pkt->guestlist->guestname == NULL) {
		radlog(L_ERR, "SC_HSPGW_PMS_UNEXPECTED_MSG : sid n/a : zid %s : pms %s:%s : Missing GN field in GC message. Suspending execution of proc_gc",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
				return;
	}


	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
	if (sqlsocket == NULL) {
		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : sid n/a : zid %s : pms %s:%s : Cannot get socket for SPP data base.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
		return;
	}

//	convert2ascii(guestname, sizeof(guestname), pkt->guestlist->guestname);
	//dupquotes(modstr, guestname);
	dupquotes(modstr, pkt->guestlist->guestname);
	sprintf(mqs, inst->config->pms_proc_gc, pmsdata->config_id, pkt->guestlist->roomnumber, pkt->ronumber, modstr, pkt->guestlist->guestid, pkt->isdbsynch);
	DEBUG3("pms_proc_gc query is %s ", mqs);

	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {

		radlog(L_ERR, "SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Processing of GC on SPP data base failed.",
			pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

	} else {

		if (inst->sql_inst->sql_fetch_row(sqlsocket, inst->sql_inst) != 0 || sqlsocket->row == NULL) {

			DEBUG2("SC_HSPGW_ORA_ERROR : sid n/a : zid %s : pms %s:%s : Could not fetch row while executing pms_proc_gc on SPP data base.",
				pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);

		} else {

			// Get the values
			row = sqlsocket->row;
			if (row == NULL) {
				DEBUG2("SC_HSPGW_ORA_NO_DATA : sid n/a : zid %s : pms %s:%s : No data while executing pms_proc_gc on SPP data base.",
					pmsdata->zoneid, pmsdata->ipaddr, pmsdata->port);
			}

		}

	}

	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
	// release sql socket
	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);

}


///*************************************************************************
// *
// *    Function: proc_relogin
// *
// *    Purpose: Wrapper for stored function HSPGW_RELOGIN
// *  @ void *instance: PMS instance
// *
// *************************************************************************/
//static void proc_relogin(void *instance, char *p_mac_address, char *p_client_ip) {
//	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
//	SQL_ROW row = NULL;
//	SQLSOCK *sqlsocket;
//	char mqs[MAX_QUERY_LEN];
//	char modstr[PMS_PASSWORD_MAX_LEN + 1];
//
//	sqlsocket = inst->sql_inst->sql_get_socket(inst->sql_inst);
//	if (sqlsocket == NULL) {
//		radlog(L_ERR, "SC_HSPGW_SPPDB_CONN_ERR : Cannot get socket for SPP data base.");
//		return;
//	}
//
//	sprintf(mqs, inst->config->pms_relogin, p_mac_address, p_client_ip);
//	DEBUG3("expand: %s", mqs);
//
//	if (inst->sql_inst->sql_select_query(sqlsocket, inst->sql_inst, mqs)) {
//		radlog(L_ERR, "SC_HSPGW_ORA_ERROR :  Processing of HSPGW_RELOGIN (mac_address=%s, client_ip=%)on SPP data base failed.",
//				p_mac_address, p_client_ip);
//
//	}
//
//	(inst->sql_inst->module->sql_finish_select_query)(sqlsocket, inst->sql_inst->config);
//	inst->sql_inst->sql_release_socket(inst->sql_inst, sqlsocket);
//
//}


/*************************************************************************
 *
 *    Function: methods for asynchronous communication with Socket
 *
 *  0 success
 *  -1 error
 *************************************************************************/

static int pms_fias_handle_message(void *instance, PMS_SOCKET* pmssocket, char* message, pms_data *pmsdata) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;

	// asynch messages (log here)
	radlog(L_INFO, "SC_HSPGW_PMS_RCV_MSG : sid n/a : zid %s : pms %s:%s : %s",
		pmsdata->zoneid, pmssocket->ipaddr, pmsdata->port, message);

	// expected requested messages
	if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PA) == 0)
			|| (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_PL) == 0)) {

		// just for test purposes
		// PMS_FIAS_PACKET *packet = pms_fias_splitPacket(message);
		// pms_fias_validatePacket(message, packet);

		// reply messages for requested PRs (not handled here)
		pms_fias_notifyReceiveMessage(pmssocket);

	} else {

		PMS_FIAS_PACKET *packet = pms_fias_splitPacket(message);

		if (pms_fias_validatePacket(message, packet) < 0) {
			DEBUG2("pms_fias_handle_message: invalid PMS message.");
			free(packet);
			return -1;
		}
		// link start record -> send init records to pms
		if (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_LS) == 0) {
			if (pms_fias_send_config_records(pmssocket, pmsdata) < 0) {
				radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid %s : pms %s:%s : Could not send init records over socket.",
					pmsdata->zoneid, pmssocket->ipaddr, pmsdata->port);
				return -1;
			}

		} else if (pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_LE) == 0) {
			// store ecot to ctime now
			pms_set_ecot(inst, pmsdata, time(NULL));
			//close socket
			changeSockState(pmssocket, pmssockunconnected, 1);

		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_DS) == 0)) {
			radlog(L_INFO, "SC_HSPGW_PMS_DBSYNCH : sid n/a : zid %s : pms %s:%s : Start processing data base synch...",
				pmsdata->zoneid, pmssocket->ipaddr, pmsdata->port);
			// store db synch timestamp
			//todo: if this fails, then all msgs will be treated as realtime <- change stored func!!
			pms_fias_upd_dbs_ts(inst, pmsdata);
		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_DE) == 0)) {
			pms_fias_consolidate_DB(inst, pmsdata); // consolidate HSPGW_OCCUPANCY
			//pms_fias_clear_timestamp(inst, pmsdata); // reset dbs_ts to NULL
			//pms_reset_ecot(inst, pmsdata); // set ecot = NULL
			pms_set_ecot(inst, pmsdata, NULL);
			radlog(L_INFO, "SC_HSPGW_PMS_DBSYNCH : sid n/a : zid %s : pms %s:%s : Data base synch process done.",
				pmsdata->zoneid, pmssocket->ipaddr, pmsdata->port);
		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_GO) == 0)) {
			//
			//
			//radlog(L_INFO, "starting processing GO......");
			proc_GO(inst, packet, pmsdata);
			//radlog(L_INFO, "GO processed.");
		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_GI) == 0)) {
			//
			//
			proc_GI(inst, packet, pmsdata);
		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_GC) == 0)) {
			//

			proc_GC(inst, packet, pmsdata);
		} else if ((pms_fias_check_packet_type(message, PROTEL_FIAS_RECID_LA) == 0)){
			//todo: LA? LE? sendDR(pmssocket);
			//sendDR(pmssocket);
		}
		else {
			//todo: LA? LE? sendDR(pmssocket);
		}
	}
	return 0;

}

static void pms_fias_freeSocketData(PMS_SOCKET *pmssocket) {

	//radlog(L_INFO,"calling release socket");
	pms_fias_releaseSocket(pmssocket);
	//radlog(L_INFO,"socket released");


	pthread_cond_destroy(&pmssocket->cond_rcvmsg);
	pthread_cond_destroy(&pmssocket->cond_rstthread);
	pthread_mutex_destroy(&pmssocket->rcvqueue_mutex);
	pthread_mutex_destroy(&pmssocket->sndqueue_mutex);
	pthread_mutex_destroy(&pmssocket->socket_mutex);
	pthread_mutex_destroy(&pmssocket->cond_rcvmsg_mutex);
	pthread_mutex_destroy(&pmssocket->cond_rstthread_mutex);

	// Free memory
	free(pmssocket);
}

/*************************************************************************
 *
 *    Function: pms_freeDataPool
 *
 *    Purpose: Internal functions to free the data pool
 *  @ void *instance: PMS instance
 *  Return 0 on success, -1 on failure
 *
 *  Author: Juan Vasquez
 *  Last changed: 06.01.2010
 *************************************************************************/
static void pms_fias_freeDataPool(void *instance) {
	rlm_pms_1_module_t *inst = (rlm_pms_1_module_t *) instance;
	PMS_DATA_POOL *datapool;
	PMS_SOCKET_POOL *cur;
	PMS_SOCKET_POOL *next;
	PMS_SOCKET *pmssocket;
	int rcode;

	datapool = inst->datapool;

	// Here we free the socket pool too!
	//radlog(L_INFO,"looping socket pool");
	for (cur = inst->datapool->socketpool; cur; cur = next) {
		next = cur->next;
		pmssocket = cur->sckt;
		//radlog(L_INFO,"cancel thread");
		//pms_fias_cancel_thread(&pmssocket);

		if (pmssocket->thread != 0) {

			pthread_t thread = pmssocket->thread;
			pmssocket->shutdown = 1;
			pthread_join(thread, NULL);

			//the freeSocketData will be handled by thread itself
		} else {
			//radlog(L_INFO,"freeing socket data");
			pms_fias_freeSocketData(pmssocket);
		}

	}
	//radlog(L_INFO,"ended looping socket pool");


#ifdef HAVE_PTHREAD_H

	rcode = pthread_mutex_destroy(&datapool->mutex);
	if (rcode != 0) {
		DEBUG(
					"pms_fias::pms_freeDataPool: Failed to destroy the data pool lock: %s",
					strerror(rcode));
	}
#endif

	// Free memory
	DEBUG("freeing the data pool");

	free(datapool);
	DEBUG("data pool free");
	inst->datapool = NULL;
	DEBUG("data pool null");

}

/**
 * 0: success
 * -1: error
 */
static int pms_fias_freeSendMessageQueue(PMS_SOCKET *pmssocket) {
	struct message_queue *cur;
	struct message_queue *next;

	pms_fias_lockSendQueue(pmssocket);

	for (cur = pmssocket->snd_msg_queue; cur; cur = next) {
		next = cur->next;
		// Free memory
		free(cur);
	}

	pmssocket->snd_msg_queue = NULL;

	pms_fias_unlockSendQueue(pmssocket);

	return 0;

}

/**
 * 0: success
 * -1: error
 */
static int pms_fias_freeReceiveMessageQueue(PMS_SOCKET *pmssocket) {
	struct message_queue *cur;
	struct message_queue *next;

	pms_fias_lockReceiveQueue(pmssocket);

	for (cur = pmssocket->rcv_msg_queue; cur; cur = next) {
		next = cur->next;

		// Free memory
		free(cur);
	}

	pmssocket->rcv_msg_queue = NULL;

	pms_fias_unlockReceiveQueue(pmssocket);

	return 0;

}

/*************************************************************************
 *
 *      Function: pms_fias_disconnect
 *
 *      Purpose:  Sends a LE request to FIAS server to signal a system
 *                shutdown. This request is called only in the detach
 *                function of the rlm_pms_1 module
 *
 ************************************************************************/

static int pms_fias_disconnect(PMS_SOCKET *pmssocket) {
	time_t now;
	struct tm *tm_now;
	char dbuff[BUFSIZ];
	char tbuff[BUFSIZ];
	char message[MAX_FIAS_BUFFER_SIZE];
	char packetbuffer[MAX_FIAS_BUFFER_SIZE];
	int ret;
	int sockfd;
	sockfd = pmssocket->sockfd;

	if (pms_fias_ist_connected(pmssocket->sockfd) != 0) {
		//no need to disconnect
		return 0;
	}

	now = time(NULL);
	tm_now = localtime(&now);
	strftime(dbuff, sizeof dbuff, "%y%m%d", tm_now); //needed?
	strftime(tbuff, sizeof tbuff, "%H%M%S", tm_now); //needed?

	// Format LE message "LE|DA%s|TI%s|"
	sprintf(message, PROTEL_FIAS_LE, dbuff, tbuff);

	// Send LE message
	DEBUG2("pms_fias::pms_disconnect: Send packet type 'Link End'");
	if (send_last_packet(pmssocket, message, NULL) < 0) {
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid  n/a : pms %s:%s : Could not send LE to PMS.",
			pmssocket->ipaddr, pmssocket->port);
		return -1;
	}

	sleep(PMS_INIT_TIME);

	// 'Link End' message is expected from PMS server after sending configuration records
	ret = pms_fias_receive(pmssocket, packetbuffer);

	if (ret < 0) { // Socket failure
		radlog(L_ERR, "SC_HSPGW_INTERNAL_ERR : sid n/a : zid  n/a : pms %s:%s : LE not acknowledged by PMS.",
			pmssocket->ipaddr, pmssocket->port);
		return -1;
	}

	return 0;
}

/*************************************************************************
 *
 *      Function: extract_webproxy_from_account_info
 *
 *      Purpose:  Extracts the webproxy value from the account info string
 *
 ************************************************************************/
static int extract_webproxy_from_account_info (char* string) {
	char *pattern = "WebProxy=\\([[:digit:]]*\\)";
	regex_t regex;
	regmatch_t pmatch[2];
	int result;

	if (regcomp(&regex, pattern, 0) != 0) {
		// printf("Invalid regular expression '%s'\n", pattern);
		return 0;
	}
	if (regexec(&regex, string, 2, pmatch, REG_EXTENDED) == 0) {
		char word[100];
    	int begin, end;
//		  printf("With the whole expression, "
//		             "a matched substring \"%.*s\" is found at position %d to %d.\n",
//		             pmatch[0].rm_eo - pmatch[0].rm_so, &string[pmatch[0].rm_so],
//		             pmatch[0].rm_so, pmatch[0].rm_eo - 1);
//		      printf("With the sub-expression, "
//		             "a matched substring \"%.*s\" is found at position %d to %d.\n",
//		             pmatch[1].rm_eo - pmatch[1].rm_so, &string[pmatch[1].rm_so],
//		             pmatch[1].rm_so, pmatch[1].rm_eo - 1);
        begin = (int)pmatch[1].rm_so;
        end = (int)pmatch[1].rm_eo;
        strncpy(word,string+begin, end - begin);
        word[end - begin] = 0;
        result =atoi(word);
    }
	else
	{
//		printf ("No match\n");
		result = 0;
	}
    regfree(&regex);
    return result;
}

module_t rlm_pms_1 = { RLM_MODULE_INIT, "pms_1", RLM_TYPE_CHECK_CONFIG_SAFE, /* type */
pms_instantiate, /* instantiation */
rlm_pms_detach, /* detach */
{ pms_authenticate, /* authentication */
NULL, /* authorization */
NULL, /* preaccounting */
pms_accounting, /* accounting */
NULL, /*pms_checksimul,*//* checksimul */
NULL, /* pre-proxy */
NULL, /* post-proxy */
NULL /* post-auth */
#ifdef WITH_COA
		, NULL, /* recv-coa */
		NULL /* send-coa */
#endif
		}, };
// if there
