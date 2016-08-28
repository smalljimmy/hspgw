/* 
 * File:   rlm_pms.h
 * Author: hagen
 *
 * Created on 2. September 2009, 15:06
 *
 * Last changed: 12.01.2009 by Juan
 */

#ifndef _PMS_DL_H
#define	_PMS_DL_H

#ifdef HAVE_PTHREAD_H
#include <pthread.h>
#endif


#include <freeradius-devel/ident.h>
RCSIDH(other_h, "$Id$")

#ifdef	__cplusplus
extern "C" {
#endif
#include	<ltdl.h>
#ifdef	__cplusplus
}
#endif

// PMS Attributes
#define PW_LOCATION_NAME			2		// ATTRIBUTE WISPr-Location-Name 2 
#define PW_SCS_SPP_ZONEID			3000
#define PW_SCS_SPP_STRIPPEDUSER		3001	// Stripped user from attribute User-Name
#define PW_SCS_HSPGW_TARIFFID       3003    // Stripped tariff id from attribute User-Name

#define PMS_ZONEID_MAX_LEN		5
#define PMS_SESSIONID_MAX_LEN		40
#define PMS_GUESTID_MAX_LEN		16
#define PMS_USERNAME_MAX_LEN		40
#define PMS_TARIFFID_MAX_LEN		40
#define PMS_PASSWORD_MAX_LEN		40
#define PMS_SEARCHPATTERN_MAX_LEN	40
#define PMS_HSRMID_MAX_LEN		5
#define PMS_AUT_ACC_TYPE_MAX_LEN	10	   // "PMS_" + PMS_HSRMID_MAX_LEN + '\0' = 10

#define PMS_TARIFF_PER_SESSION		"PER_SESSION"
#define PMS_MAX_REPLY_MSG_LEN		64

#define ASCEND_PORT_HACK
#define ASCEND_CHANNELS_PER_LINE    23
#define CISCO_ACCOUNTING_HACK


// PMS FIAS specific
#define BYTE unsigned char
#define PROTEL_FIAS_SEARCH_PATTERN  "RN"

typedef struct pms_config {
	char   *sql_instance_name;
	// General PMS
	char	*pms_all_config_query;
	char	*pms_all_events_query;
	char	*pms_config_query;
//	char	*pms_acctrack_query;
//	char 	*pms_acc_qry_by_sessid;
//	char 	*pms_acc_qry_by_usrloc;
//	char	*pms_acctrack_insert;
//	char	*pms_acc_upd_gid;
//	char	*pms_acc_upd_billed;
//	char	*pms_acctrack_purge;
	char	*pms_logger_insert;
	char    *pms_sim_access;
	char	*pms_gw_set_state;
	char	*pms_gw_shutdown;
	
	char	*pms_conf_locid_delimiter;
	char	*pms_conf_realm_delimiter;
	char    *pms_conf_tariff_delimiter;
	char	*pms_conf_allowed_realm;
	char	*pms_conf_type_prefix;
	char	*pms_snmptrap_command;

	int    pms_conf_sckt_conn_timeout;
	int    pms_conf_sckt_recv_ntimeout;
	int    pms_conf_sckt_recv_utimeout;
	int    pms_conf_sckt_max_trials;
	int    pms_conf_sckt_max_timeouts;

	int		pms_connect_failure_retry_delay;

	int    sqltrace;
	int    do_clients;
	char   *tracefile;
	char   *xlat_name;
	int    deletestalesessions;
	int    num_sql_socks;
	int    lifetime;
	int    max_queries;
	int    connect_failure_retry_delay;
//	char   *postauth_query;
	char   *allowed_chars;
	int    query_timeout;
	/* individual driver config */
	void	*localcfg;
} PMS_CONFIG;


/*
 *	A mapping of configuration file names to internal variables.
 *
 *	Note that the string is dynamically allocated, so it MUST
 *	be freed.  When the configuration file parse re-reads the string,
 *	it free's the old one, and strdup's the new one, placing the pointer
 *	to the strdup'd string into 'config.string'.  This gets around
 *	buffer over-flows.
 */
static const CONF_PARSER module_config[] = {
	{"sql-instance-name",PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,sql_instance_name), NULL, "sql"},
	{"pms_all_config_query", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_all_config_query), NULL, ""},
	{"pms_config_query", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_config_query), NULL, ""},
//	{"pms_acctrack_query", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acctrack_query), NULL, ""},
//	{"pms_acctrack_insert", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acctrack_insert), NULL, ""},
//	{"pms_acctrack_purge", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acctrack_purge), NULL, ""},
	{"pms_logger_insert", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_logger_insert), NULL, ""},
	{"pms_sim_access", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_sim_access), NULL, ""},
	{"pms_all_events_query", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_all_events_query), NULL, ""},
//	{"pms_acc_qry_by_sessid", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acc_qry_by_sessid), NULL, ""},
//	{"pms_acc_qry_by_usrloc", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acc_qry_by_usrloc), NULL, ""},
//	{"pms_acc_upd_gid", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acc_upd_gid), NULL, ""},
//	{"pms_acc_upd_billed", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_acc_upd_billed), NULL, ""},
	{"pms_gw_set_state", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_gw_set_state), NULL, ""},
	{"pms_gw_shutdown", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_gw_shutdown), NULL, ""},


	{"safe-characters", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,allowed_chars), NULL,"@çèéabcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789.-_: /"},

	{"pms_conf_locid_delimiter", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_conf_locid_delimiter), NULL, ","},
	{"pms_conf_realm_delimiter", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_conf_realm_delimiter), NULL, "@"},
	{"pms_conf_tariff_delimiter", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_conf_tariff_delimiter), NULL, "/"},
	{"pms_conf_allowed_realm", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_conf_allowed_realm), NULL, "hspgw"},
	{"pms_conf_type_prefix", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_conf_type_prefix), NULL, "PMS_"},

	{"pms_conf_sckt_conn_timeout", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,pms_conf_sckt_conn_timeout), NULL, "3"},
	{"pms_conf_sckt_recv_ntimeout", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,pms_conf_sckt_recv_ntimeout), NULL, "0"},
	{"pms_conf_sckt_recv_utimeout", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,pms_conf_sckt_recv_utimeout), NULL, "100000"},
	{"pms_conf_sckt_max_trials", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,pms_conf_sckt_max_trials), NULL, "30"},
	{"pms_conf_sckt_max_timeouts", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,pms_conf_sckt_max_timeouts), NULL, "3"},

	{"pms_snmptrap_command", PW_TYPE_STRING_PTR,offsetof(PMS_CONFIG,pms_snmptrap_command), NULL, ""},


	/*
	 *	This only works for a few drivers.
	 */
	{"query_timeout", PW_TYPE_INTEGER,offsetof(PMS_CONFIG,query_timeout), NULL, NULL},
  { NULL, -1, 0, NULL, NULL }		/* end the list */
};

// Configuration Data
typedef struct pms_data {
	char	zoneid[PMS_ZONEID_MAX_LEN + 1];  // i.e. max. ID '9999' + '\0': 4 + 1 = 5
	char	ipaddr[16]; // Length of Oracle database column + 1 for '\0': 15 + 1 = 16
	char	port[7];	// Length of Oracle database column + 1 for '\0': 6 + 1 = 7
	int		protocol;
	char	sesto[7];		// Sufficient for about 10 days ('864000' sec) + 1 for '\0': 6 + 1 = 7
	char   amount[8];		// Max. amount (in cents) '9999999' + '\0': 7 + 1 = 8
	char	usrfld[11];		// Field_Name_1 column length + 1 for '\0': 10 + 1 = 11
	char	pwfld[11];		// Field_Name_2 column length + 1 for '\0': 10 + 1 = 11
	char	tariff_key[21]; // Length of Oracle database column + 1 for '\0': 20 + 1 = 21
	int		go_through;
	char	hsrmid[PMS_HSRMID_MAX_LEN + 1];		// i.e. max. ID '99999' + '\0': 5 + 1 = 6
	int		is_enabled;
} pms_data;


// Account Tracker data
typedef struct pms_tracker_data {
	char	zoneid[PMS_ZONEID_MAX_LEN + 1];  // i.e. max. ID '9999' + '\0': 4 + 1 = 5
	char	usr[PMS_USERNAME_MAX_LEN + 1];			// ACCOUNT_TRACKER_USR column length  + 1 for '\0'
	char	pwd[PMS_PASSWORD_MAX_LEN + 1];			// ACCOUNT_TRACKER_PWD column length  + 1 for '\0'
	char 	pwd2[PMS_PASSWORD_MAX_LEN + 1];			// ACCOUNT_TRACKER_PWD column length  + 1 for '\0'
	char	sessid[PMS_SESSIONID_MAX_LEN + 1];		// ACCOUNT_TRACKER_SESS_ID column length  + 1 for '\0'
	char	sesto[7];		// Sufficient for about 10 days ('864000' sec) + 1 for '\0': 6 + 1 = 7
	int     deadline;
	int		billed;
	char	guestid[PMS_GUESTID_MAX_LEN + 1];	// Length of Oracle database column + 1 for '\0': 16 + 1 = 17
	struct pms_tracker_data *next;
} pms_tracker_data;


// Logger data
typedef struct pms_logger_data {
	int		zoneid;
	int		hsrmid;
	int		logeventid;		// Log Event ID
	char	logdescr[101];	// Length of Oracle database column + 1 for '\0': 100 + 1 = 101
} pms_logger_data;


// Log file data
typedef struct pms_logfile_data {
	char	sessid[PMS_SESSIONID_MAX_LEN + 1];
	char	zoneid[PMS_ZONEID_MAX_LEN + 1];
	char	username[PMS_USERNAME_MAX_LEN + 1];
	char    tariffid[PMS_TARIFFID_MAX_LEN + 1];
	char	hsrmid[PMS_HSRMID_MAX_LEN + 1];		// i.e. max. ID '99999' + '\0': 5 + 1 = 6
} pms_logfile_data;


// Log events
typedef struct pms_log_events {
	int		id;  
	char	key[33];	// Length of Oracle database column + 1 for '\0': 32 + 1 = 33
	char	type[21];
	char	level[21];
	char	postcond[65];
	char	format[43];  // (key) + (appendix) + '\0': 43
	struct pms_log_events *next;
} PMS_LOGEVENTS;

// The instance
typedef struct rlm_pms_module_t {
	SQL_INST *sql_inst;

	char				*rcode_str;
	int					rcode;

	SQLSOCK				*sqlpool;
	SQLSOCK				*last_used;

	lt_dlhandle			handle;
	PMS_CONFIG			*config;

	PMS_LOGEVENTS		*logevents;
	size_t (*sql_escape_func)(char *out, size_t outlen, const char *in);
	char				hostname[40];
	char				shutdownqry[MAX_QUERY_LEN];

} rlm_pms_module_t;


static size_t sql_escape_func(char *out, size_t outlen, const char *in);

int pms_strip_zoneid(void *instance, char *locationname, char *outzoneid);
static int pms_strip_tariffid(void *instance, char *username, char *strippedtariffid);
int pms_validate_realm(void *instance, char *username, char *strippeduser);

int pms_dbLogger(void *instance, REQUEST *request, pms_logger_data *loggerdata);
int pms_prepareLogData(pms_logger_data **loggerdata, char *zoneid, char *hsrmid, int logeventid, char *logdescr);

int pms_init(void *instance);
void pms_freeLogEventPool(void *instance);
PMS_LOGEVENTS *pms_insertLogEvent(PMS_LOGEVENTS *list, SQL_ROW row);
int pms_containsLogEvent(PMS_LOGEVENTS *list, SQL_ROW row);
PMS_LOGEVENTS *pms_findLogEventKey(PMS_LOGEVENTS *list, char *eventkey);
PMS_LOGEVENTS *pms_createLogEvent(SQL_ROW row);
PMS_LOGEVENTS *pms_find_lastLogEvent(PMS_LOGEVENTS *list);
int pms_sizeLogEvent(PMS_LOGEVENTS *list);
void pms_infoLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *logfiledata, REQUEST *request);
void pms_errLog(char *msgtxt, char *eventkey, void *instance, pms_logfile_data *logfiledata, REQUEST *request);
int parse_repl(char *out, int outlen, const char *fmt, REQUEST *request, RADIUS_ESCAPE_STRING func);

void pms_snmpTrap(char *msgtxt, void *instance, REQUEST *request);
#endif	/* _PMS_DL_H */
