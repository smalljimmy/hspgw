/*
 * sql_oracle.c	Oracle (OCI) routines for rlm_sql
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
 * Copyright 2000  David Kerry <davidk@snti.com>
 */

#include <freeradius-devel/ident.h>
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>

#include <sys/stat.h>

#include <sqlrelay/sqlrclientwrapper.h>
#include "rlm_sql.h"

typedef struct rlm_sql_sqlrelay_sock {
	sqlrcon conn;
	sqlrcur cur;
	int rownum;
} rlm_sql_sqlrelay_sock;

/*************************************************************************
 *
 *	Function: sql_error
 *
 *	Purpose: database specific error. Returns error associated with
 *               connection
 *
 *************************************************************************/
static const char *sql_error(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (sqlrelay_sock->conn == NULL) {
		return "rlm_sql_sqlrelay: Socket not connected";
	}

	if (!sqlrelay_sock)
		return "rlm_sql_sqlrelay: no connection to db";

	return sqlrcur_errorMessage(sqlrelay_sock->cur);

}

/*************************************************************************
 *
 *	Function: sql_check_error
 *
 *	Purpose: check the error to see if the server is down
 *
 *************************************************************************/
static int sql_check_error(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	if (sql_error(sqlsocket, config) != NULL && (strstr(sql_error(sqlsocket, config), "no connection to db") || strstr(
			sql_error(sqlsocket, config), "ORA-03114"))) {
		radlog(L_ERR, "rlm_sql_sqlrelay: OCI_SERVER_NOT_CONNECTED");
		return SQL_DOWN;
	} else {
		radlog(L_ERR, "rlm_sql_sqlrelay: OCI_SERVER_NORMAL");
		return -1;
	}
}

/*************************************************************************
 *
 *	Function: sql_close
 *
 *	Purpose: database specific close. Closes an open database
 *               connection and cleans up any open handles.
 *
 *************************************************************************/
static int sql_close(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (sqlrelay_sock->conn) {
		sqlrcon_free(sqlrelay_sock->conn);
	}

	sqlrelay_sock->conn = NULL;

	sqlrelay_sock->rownum = 0;

	free(sqlrelay_sock);
	sqlsocket->conn = NULL;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_init_socket
 *
 *	Purpose: Establish connection to the db
 *
 *************************************************************************/
static int sql_init_socket(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock;

	if (!sqlsocket->conn) {
		sqlsocket->conn = (rlm_sql_sqlrelay_sock *) rad_malloc(
				sizeof(rlm_sql_sqlrelay_sock));
		if (!sqlsocket->conn) {
			return -1;
		}
	}
	memset(sqlsocket->conn, 0, sizeof(rlm_sql_sqlrelay_sock));

	sqlrelay_sock = sqlsocket->conn;

	sqlrelay_sock->conn = sqlrcon_alloc(config->sql_server,
			atoi(config->sql_port), "", config->sql_login,
			config->sql_password, 0, 1);
	// DEBUG2("sqlrcon_alloc ptr=%p", sqlrelay_sock->conn);

	sqlrelay_sock->rownum = 0;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_destroy_socket
 *
 *	Purpose: Free socket and private connection data
 *
 *************************************************************************/
static int sql_destroy_socket(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	free(sqlsocket->conn);
	sqlsocket->conn = NULL;
	return 0;
}

/*************************************************************************
 *
 *	Function: sql_num_fields
 *
 *	Purpose: database specific num_fields function. Returns number
 *               of columns from query
 *
 *************************************************************************/
static int sql_num_fields(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;
	return sqlrcur_colCount(sqlrelay_sock->cur);
}

/*************************************************************************
 *
 *	Function: sql_query
 *
 *	Purpose: Issue a non-SELECT query (ie: update/delete/insert) to
 *               the database.
 *
 *************************************************************************/
static int sql_query(SQLSOCK *sqlsocket, SQL_CONFIG *config, char *querystr) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (config->sqltrace)
		DEBUG(querystr);
	if (sqlrelay_sock->conn == NULL) {
		radlog(L_ERR, "rlm_sql_sqlrelay: Socket not connected");
		return SQL_DOWN;
	}

	if (sqlrelay_sock->cur != NULL) {
		DEBUG2("WARNING: ptr=%p forgot sql_finish_query?", sqlrelay_sock->cur);
	}
	sqlrelay_sock->cur = sqlrcur_alloc(sqlrelay_sock->conn);
	// DEBUG2("sqlrcur_alloc %s(%d) ptr=%p", __func__,__LINE__, sqlrelay_sock->cur);


	if (!sqlrcur_sendQuery(sqlrelay_sock->cur, querystr)) {
		radlog(L_ERR,
				"rlm_sql_sqlrelay: execute query failed in sql_query: %s",
				sql_error(sqlsocket, config));
		return sql_check_error(sqlsocket, config);
	}

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_select_query
 *
 *	Purpose: Issue a select query to the database
 *
 *************************************************************************/
static int sql_select_query(SQLSOCK *sqlsocket, SQL_CONFIG *config,
		char *querystr) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (config->sqltrace)
		DEBUG(querystr);

	if (sqlrelay_sock->conn == NULL) {
		radlog(L_ERR, "rlm_sql_sqlrelay: Socket not connected");
		return SQL_DOWN;
	}

	if (sqlrelay_sock->cur != NULL) {
		DEBUG2("WARNING: ptr=%p forgot sql_finish_query?", sqlrelay_sock->cur);
	}
	sqlrelay_sock->cur = sqlrcur_alloc(sqlrelay_sock->conn);
	// DEBUG2("sqlrcur_alloc %s(%d) ptr=%p", __func__,__LINE__, sqlrelay_sock->cur);


	if (!sqlrcur_sendQuery(sqlrelay_sock->cur, querystr)) {
		radlog(L_ERR,
				"rlm_sql_sqlrelay: execute query failed in sql_query: %s",
				sql_error(sqlsocket, config));
		return sql_check_error(sqlsocket, config);
	}

	sqlrelay_sock->rownum = 0;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_store_result
 *
 *	Purpose: database specific store_result function. Returns a result
 *               set for the query.
 *
 *************************************************************************/
static int sql_store_result(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	/* Not needed for Oracle */
	return 0;
}

/*************************************************************************
 *
 *	Function: sql_num_rows
 *
 *	Purpose: database specific num_rows. Returns number of rows in
 *               query
 *
 *************************************************************************/
static int sql_num_rows(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	return sqlrcur_rowCount(sqlrelay_sock->cur);
}

/*************************************************************************
 *
 *	Function: sql_fetch_row
 *
 *	Purpose: database specific fetch_row. Returns a SQL_ROW struct
 *               with all the data for the query in 'sqlsocket->row'. Returns
 *		 0 on success, -1 on failure, SQL_DOWN if database is down.
 *
 *************************************************************************/
static int sql_fetch_row(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (sqlrelay_sock->conn == NULL) {
		radlog(L_ERR, "rlm_sql_sqlrelay: Socket not connected");
		return SQL_DOWN;
	}

	if (sqlrcur_rowCount(sqlrelay_sock->cur) == 0) {
		sqlsocket->row = NULL;
	} else if (sqlrelay_sock->rownum >= sql_num_rows(sqlsocket, config)) {
		return -1;
	} else {
		sqlsocket->row = sqlrcur_getRow(sqlrelay_sock->cur,
				sqlrelay_sock->rownum++);
	}

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_free_result
 *
 *	Purpose: database specific free_result. Frees memory allocated
 *               for a result set
 *
 *************************************************************************/
static int sql_free_result(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_finish_query
 *
 *	Purpose: End the query, such as freeing memory
 *
 *************************************************************************/
static int sql_finish_query(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (sqlrelay_sock->conn == NULL) {
		radlog(L_ERR, "rlm_sql_sqlrelay: Socket not connected");
		return SQL_DOWN;
	}

	sqlrcon_endSession(sqlrelay_sock->conn);
	// DEBUG2("sqlrcur_free ptr=%p", sqlrelay_sock->cur);
	sqlrcur_free(sqlrelay_sock->cur);
	sqlrelay_sock->cur = NULL;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_finish_select_query
 *
 *	Purpose: End the select query, such as freeing memory or result
 *
 *************************************************************************/
static int sql_finish_select_query(SQLSOCK *sqlsocket, SQL_CONFIG *config) {
	rlm_sql_sqlrelay_sock *sqlrelay_sock = sqlsocket->conn;

	if (sqlrelay_sock->conn == NULL) {
		radlog(L_ERR, "rlm_sql_sqlrelay: Socket not connected");
		return SQL_DOWN;
	}

	sqlrcon_endSession(sqlrelay_sock->conn);
	// DEBUG2("sqlrcur_free ptr=%p", sqlrelay_sock->cur);
	sqlrcur_free(sqlrelay_sock->cur);
	sqlrelay_sock->cur = NULL;

	return 0;
}

/*************************************************************************
 *
 *	Function: sql_affected_rows
 *
 *	Purpose: Return the number of rows affected by the query (update,
 *               or insert)
 *
 *************************************************************************/
static int sql_affected_rows(SQLSOCK *sqlsocket, SQL_CONFIG *config) {

	return sql_num_rows(sqlsocket, config);
}

/* Exported to rlm_sql */
rlm_sql_module_t rlm_sql_sqlrelay = { "rlm_sql_sqlrelay", sql_init_socket,
		sql_destroy_socket, sql_query, sql_select_query, sql_store_result,
		sql_num_fields, sql_num_rows, sql_fetch_row, sql_free_result,
		sql_error, sql_close, sql_finish_query, sql_finish_select_query,
		sql_affected_rows };
