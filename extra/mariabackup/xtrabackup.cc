/******************************************************
MariaBackup: hot backup tool for InnoDB
(c) 2009-2017 Percona LLC and/or its affiliates
Originally Created 3/3/2009 Yasufumi Kinoshita
Written by Alexey Kopytov, Aleksandr Kuzminsky, Stewart Smith, Vadim Tkachenko,
Yasufumi Kinoshita, Ignacio Nin and Baron Schwartz.
(c) 2017, MariaDB Corporation.
Portions written by Marko Mäkelä.

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA

*******************************************************

This file incorporates work covered by the following copyright and
permission notice:

Copyright (c) 2000, 2011, MySQL AB & Innobase Oy. All Rights Reserved.

This program is free software; you can redistribute it and/or modify it under
the terms of the GNU General Public License as published by the Free Software
Foundation; version 2 of the License.

This program is distributed in the hope that it will be useful, but WITHOUT
ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License along with
this program; if not, write to the Free Software Foundation, Inc., 59 Temple
Place, Suite 330, Boston, MA 02111-1307 USA

*******************************************************/

//#define XTRABACKUP_TARGET_IS_PLUGIN

#include <my_global.h>
#include <my_config.h>
#include <unireg.h>
#include <mysql_version.h>
#include <my_base.h>
#include <my_getopt.h>
#include <mysql_com.h>
#include <my_default.h>
#include <mysqld.h>

#include <fcntl.h>
#include <string.h>

#ifdef __linux__
# include <sys/prctl.h>
#include <sys/resource.h>
#endif


#include <btr0sea.h>
#include <dict0priv.h>
#include <lock0lock.h>
#include <log0recv.h>
#include <log0crypt.h>
#include <row0mysql.h>
#include <row0quiesce.h>
#include <srv0start.h>
#include "trx0sys.h"
#include <buf0dblwr.h>

#include <list>
#include <sstream>
#include <set>
#include <mysql.h>

#define G_PTR uchar*

#include "common.h"
#include "datasink.h"

#include "xb_regex.h"
#include "fil_cur.h"
#include "write_filt.h"
#include "xtrabackup.h"
#include "ds_buffer.h"
#include "ds_tmpfile.h"
#include "xbstream.h"
#include "changed_page_bitmap.h"
#include "read_filt.h"
#include "backup_wsrep.h"
#include "innobackupex.h"
#include "backup_mysql.h"
#include "backup_copy.h"
#include "backup_mysql.h"
#include "xb0xb.h"
#include "encryption_plugin.h"
#include <sql_plugin.h>
#include <srv0srv.h>
#include <crc_glue.h>
#include <log.h>

int sys_var_init();

/* === xtrabackup specific options === */
char xtrabackup_real_target_dir[FN_REFLEN] = "./xtrabackup_backupfiles/";
char *xtrabackup_target_dir= xtrabackup_real_target_dir;
static my_bool xtrabackup_version;
my_bool xtrabackup_backup;
my_bool xtrabackup_prepare;
my_bool xtrabackup_copy_back;
my_bool xtrabackup_move_back;
my_bool xtrabackup_decrypt_decompress;
my_bool xtrabackup_print_param;

my_bool xtrabackup_export;

longlong xtrabackup_use_memory;

uint opt_protocol;
long xtrabackup_throttle; /* 0:unlimited */
static lint io_ticket;
static os_event_t wait_throttle;
static os_event_t log_copying_stop;

char *xtrabackup_incremental;
lsn_t incremental_lsn;
lsn_t incremental_to_lsn;
lsn_t incremental_last_lsn;
xb_page_bitmap *changed_page_bitmap;

char *xtrabackup_incremental_basedir; /* for --backup */
char *xtrabackup_extra_lsndir; /* for --backup with --extra-lsndir */
char *xtrabackup_incremental_dir; /* for --prepare */

char xtrabackup_real_incremental_basedir[FN_REFLEN];
char xtrabackup_real_extra_lsndir[FN_REFLEN];
char xtrabackup_real_incremental_dir[FN_REFLEN];


char *xtrabackup_tmpdir;

char *xtrabackup_tables;
char *xtrabackup_tables_file;
char *xtrabackup_tables_exclude;

typedef std::list<regex_t> regex_list_t;
static regex_list_t regex_include_list;
static regex_list_t regex_exclude_list;

static hash_table_t* tables_include_hash = NULL;
static hash_table_t* tables_exclude_hash = NULL;

char *xtrabackup_databases = NULL;
char *xtrabackup_databases_file = NULL;
char *xtrabackup_databases_exclude = NULL;
static hash_table_t* databases_include_hash = NULL;
static hash_table_t* databases_exclude_hash = NULL;

static hash_table_t* inc_dir_tables_hash;

struct xb_filter_entry_struct{
	char*		name;
	ibool		has_tables;
	hash_node_t	name_hash;
};
typedef struct xb_filter_entry_struct	xb_filter_entry_t;

lsn_t checkpoint_lsn_start;
lsn_t checkpoint_no_start;
static lsn_t log_copy_scanned_lsn;
static bool log_copying;
static bool log_copying_running;
static bool io_watching_thread_running;

int xtrabackup_parallel;

char *xtrabackup_stream_str = NULL;
xb_stream_fmt_t xtrabackup_stream_fmt = XB_STREAM_FMT_NONE;
ibool xtrabackup_stream = FALSE;

const char *xtrabackup_compress_alg = NULL;
ibool xtrabackup_compress = FALSE;
uint xtrabackup_compress_threads;
ulonglong xtrabackup_compress_chunk_size = 0;

/* sleep interval beetween log copy iterations in log copying thread
in milliseconds (default is 1 second) */
ulint xtrabackup_log_copy_interval = 1000;
static ulong max_buf_pool_modified_pct;

/* Ignored option (--log) for MySQL option compatibility */
static char*	log_ignored_opt;

/* === metadata of backup === */
#define XTRABACKUP_METADATA_FILENAME "xtrabackup_checkpoints"
char metadata_type[30] = ""; /*[full-backuped|log-applied|incremental]*/
lsn_t metadata_from_lsn;
lsn_t metadata_to_lsn;
lsn_t metadata_last_lsn;

static ds_file_t*	dst_log_file;

static char mysql_data_home_buff[2];

const char *defaults_group = "mysqld";

/* === static parameters in ha_innodb.cc */

#define HA_INNOBASE_ROWS_IN_TABLE 10000 /* to get optimization right */
#define HA_INNOBASE_RANGE_COUNT	  100

ulong 	innobase_large_page_size = 0;

/* The default values for the following, type long or longlong, start-up
parameters are declared in mysqld.cc: */

long innobase_buffer_pool_awe_mem_mb = 0;
long innobase_file_io_threads = 4;
long innobase_read_io_threads = 4;
long innobase_write_io_threads = 4;
long innobase_log_buffer_size = 1024*1024L;

longlong innobase_page_size = (1LL << 14); /* 16KB */
char*	innobase_buffer_pool_filename = NULL;

longlong innobase_buffer_pool_size = 8*1024*1024L;

/* The default values for the following char* start-up parameters
are determined in innobase_init below: */

static char*	innobase_ignored_opt;
char*	innobase_data_home_dir;
char*	innobase_data_file_path;
/* The following has a misleading name: starting from 4.0.5, this also
affects Windows: */
char*	innobase_unix_file_flush_method;

my_bool innobase_use_doublewrite;
my_bool innobase_use_large_pages;
my_bool	innobase_file_per_table;
my_bool innobase_locks_unsafe_for_binlog;
my_bool innobase_rollback_on_timeout;
my_bool innobase_create_status_file;

/* The following counter is used to convey information to InnoDB
about server activity: in selects it is not sensible to call
srv_active_wake_master_thread after each fetch or search, we only do
it every INNOBASE_WAKE_INTERVAL'th step. */

#define INNOBASE_WAKE_INTERVAL	32
ulong	innobase_active_counter	= 0;

#ifndef _WIN32
static char *xtrabackup_debug_sync = NULL;
#endif

my_bool xtrabackup_incremental_force_scan = FALSE;

/* The flushed lsn which is read from data files */
lsn_t	flushed_lsn= 0;

ulong xb_open_files_limit= 0;
char *xb_plugin_dir;
char *xb_plugin_load;
my_bool xb_close_files;

/* Datasinks */
ds_ctxt_t       *ds_data     = NULL;
ds_ctxt_t       *ds_meta     = NULL;
ds_ctxt_t       *ds_redo     = NULL;

static bool	innobackupex_mode = false;

/* String buffer used by --print-param to accumulate server options as they are
parsed from the defaults file */
static std::ostringstream print_param_str;

/* Set of specified parameters */
std::set<std::string> param_set;

static ulonglong global_max_value;

extern "C" sig_handler handle_fatal_signal(int sig);
extern LOGGER logger;

my_bool opt_galera_info = FALSE;
my_bool opt_slave_info = FALSE;
my_bool opt_no_lock = FALSE;
my_bool opt_safe_slave_backup = FALSE;
my_bool opt_rsync = FALSE;
my_bool opt_force_non_empty_dirs = FALSE;
my_bool opt_noversioncheck = FALSE;
my_bool opt_no_backup_locks = FALSE;
my_bool opt_decompress = FALSE;
my_bool opt_remove_original;

my_bool opt_lock_ddl_per_table = FALSE;

static const char *binlog_info_values[] = {"off", "lockless", "on", "auto",
					   NullS};
static TYPELIB binlog_info_typelib = {array_elements(binlog_info_values)-1, "",
				      binlog_info_values, NULL};
ulong opt_binlog_info;

char *opt_incremental_history_name;
char *opt_incremental_history_uuid;

char *opt_user;
char *opt_password;
char *opt_host;
char *opt_defaults_group;
char *opt_socket;
uint opt_port;
char *opt_log_bin;

const char *query_type_names[] = { "ALL", "UPDATE", "SELECT", NullS};

TYPELIB query_type_typelib= {array_elements(query_type_names) - 1, "",
	query_type_names, NULL};

ulong opt_lock_wait_query_type;
ulong opt_kill_long_query_type;

uint opt_kill_long_queries_timeout = 0;
uint opt_lock_wait_timeout = 0;
uint opt_lock_wait_threshold = 0;
uint opt_debug_sleep_before_unlock = 0;
uint opt_safe_slave_backup_timeout = 0;

const char *opt_history = NULL;

#if defined(HAVE_OPENSSL)
my_bool opt_ssl_verify_server_cert = FALSE;
#endif

char mariabackup_exe[FN_REFLEN];
char orig_argv1[FN_REFLEN];

/* Whether xtrabackup_binlog_info should be created on recovery */
static bool recover_binlog_info;

/* Simple datasink creation tracking...add datasinks in the reverse order you
want them destroyed. */
#define XTRABACKUP_MAX_DATASINKS	10
static	ds_ctxt_t	*datasinks[XTRABACKUP_MAX_DATASINKS];
static	uint		actual_datasinks = 0;
static inline
void
xtrabackup_add_datasink(ds_ctxt_t *ds)
{
	xb_ad(actual_datasinks < XTRABACKUP_MAX_DATASINKS);
	datasinks[actual_datasinks] = ds; actual_datasinks++;
}


typedef void (*process_single_tablespace_func_t)(const char *dirname, const char *filname, bool is_remote);
static dberr_t enumerate_ibd_files(process_single_tablespace_func_t callback);


/* ======== Datafiles iterator ======== */
struct datafiles_iter_t {
	fil_space_t	*space;
	fil_node_t	*node;
	ibool		started;
	pthread_mutex_t	mutex;
};

/* ======== Datafiles iterator ======== */
static
datafiles_iter_t *
datafiles_iter_new()
{
	datafiles_iter_t *it;

	it = static_cast<datafiles_iter_t *>(malloc(sizeof(datafiles_iter_t)));
	pthread_mutex_init(&it->mutex, NULL);

	it->space = NULL;
	it->node = NULL;
	it->started = FALSE;

	return it;
}

static
fil_node_t *
datafiles_iter_next(datafiles_iter_t *it)
{
	fil_node_t *new_node;

	pthread_mutex_lock(&it->mutex);

	if (it->node == NULL) {
		if (it->started)
			goto end;
		it->started = TRUE;
	} else {
		it->node = UT_LIST_GET_NEXT(chain, it->node);
		if (it->node != NULL)
			goto end;
	}

	it->space = (it->space == NULL) ?
		UT_LIST_GET_FIRST(fil_system.space_list) :
		UT_LIST_GET_NEXT(space_list, it->space);

	while (it->space != NULL &&
	       (it->space->purpose != FIL_TYPE_TABLESPACE ||
		UT_LIST_GET_LEN(it->space->chain) == 0))
		it->space = UT_LIST_GET_NEXT(space_list, it->space);
	if (it->space == NULL)
		goto end;

	it->node = UT_LIST_GET_FIRST(it->space->chain);

end:
	new_node = it->node;
	pthread_mutex_unlock(&it->mutex);

	return new_node;
}

static
void
datafiles_iter_free(datafiles_iter_t *it)
{
	pthread_mutex_destroy(&it->mutex);
	free(it);
}

#ifndef DBUG_OFF
struct dbug_thread_param_t
{
	MYSQL *con;
	const char *query;
	int expect_err;
	int expect_errno;
	os_event_t done_event;
};


/* Thread procedure used in dbug_start_query_thread. */
extern "C"
os_thread_ret_t
DECLARE_THREAD(dbug_execute_in_new_connection)(void *arg)
{
	mysql_thread_init();
	dbug_thread_param_t *par= (dbug_thread_param_t *)arg;
	int err = mysql_query(par->con, par->query);
	int err_no = mysql_errno(par->con);
	DBUG_ASSERT(par->expect_err == err);
	if (err && par->expect_errno)
		DBUG_ASSERT(err_no == par->expect_errno);
	mysql_close(par->con);
	mysql_thread_end();
	os_event_t done = par->done_event;
	delete par;
	os_event_set(done);
	os_thread_exit();
	return os_thread_ret_t(0);
}

/*
Execute query from a new connection, in own thread.

@param query - query to be executed
@param wait_state - if not NULL, wait until query from new connection
	reaches this state (value of column State in I_S.PROCESSLIST)
@param expected_err - if 0, query is supposed to finish successfully,
	otherwise query should return error.
@param expected_errno - if not 0, and query finished with error,
	expected mysql_errno()
*/
static os_event_t dbug_start_query_thread(
	const char *query,
	const char *wait_state,
	int expected_err,
	int expected_errno)

{
	dbug_thread_param_t *par = new dbug_thread_param_t;
	par->query = query;
	par->expect_err = expected_err;
	par->expect_errno = expected_errno;
	par->done_event = os_event_create(0);
	par->con =  xb_mysql_connect();
	os_thread_create(dbug_execute_in_new_connection, par, 0);

	if (!wait_state)
		return par->done_event;

	char q[256];
	snprintf(q, sizeof(q),
		"SELECT 1 FROM INFORMATION_SCHEMA.PROCESSLIST where ID=%lu"
		" AND Command='Query' AND State='%s'",
		mysql_thread_id(par->con), wait_state);
	for (;;) {
		MYSQL_RES *result = xb_mysql_query(mysql_connection,q, true, true);
		if (mysql_fetch_row(result)) {
			goto end;
		}
		msg_ts("Waiting for query '%s' on connection %lu to "
			" reach state '%s'", query, mysql_thread_id(par->con),
			wait_state);
		my_sleep(1000);
	}
end:
	msg_ts("query '%s' on connection %lu reached state '%s'", query,
	mysql_thread_id(par->con), wait_state);
	return par->done_event;
}

os_event_t dbug_alter_thread_done;
#endif

void mdl_lock_all()
{
	mdl_lock_init();
	datafiles_iter_t *it = datafiles_iter_new();
	if (!it)
		return;

	while (fil_node_t *node = datafiles_iter_next(it)){
		if (fil_is_user_tablespace_id(node->space->id)
			&& check_if_skip_table(node->space->name))
			continue;

		mdl_lock_table(node->space->id);
	}
	datafiles_iter_free(it);

	DBUG_EXECUTE_IF("check_mdl_lock_works",
		dbug_alter_thread_done =
		  dbug_start_query_thread("ALTER TABLE test.t ADD COLUMN mdl_lock_column int",
			 "Waiting for table metadata lock",1, ER_QUERY_INTERRUPTED););
}

/** Check if the space id belongs to the table which name should
be skipped based on the --tables, --tables-file and --table-exclude
options.
@param[in]	space_id	space id to check
@return true if the space id belongs to skip table/database list. */
static bool backup_includes(space_id_t space_id)
{
	datafiles_iter_t *it = datafiles_iter_new();
	if (!it)
		return true;

	while (fil_node_t *node = datafiles_iter_next(it)){
		if (space_id == 0
		    || (node->space->id == space_id
			&& !check_if_skip_table(node->space->name))) {

			msg("mariabackup: Unsupported redo log detected "
			"and it belongs to %s\n",
			space_id ? node->name: "the InnoDB system tablespace");

			msg("mariabackup: ALTER TABLE or OPTIMIZE TABLE "
			"was being executed during the backup.\n");

			if (!opt_lock_ddl_per_table) {
				msg("mariabackup: Use --lock-ddl-per-table "
				"parameter to lock all the table before "
				"backup operation.\n");
			}

			datafiles_iter_free(it);
			return false;
		}
	}

	datafiles_iter_free(it);
	return true;
}

/* ======== Date copying thread context ======== */

typedef struct {
	datafiles_iter_t 	*it;
	uint			num;
	uint			*count;
	pthread_mutex_t		count_mutex;
	os_thread_id_t		id;
} data_thread_ctxt_t;

/* ======== for option and variables ======== */

enum options_xtrabackup
{
  OPT_XTRA_TARGET_DIR = 1000,     /* make sure it is larger
                                     than OPT_MAX_CLIENT_OPTION */
  OPT_XTRA_BACKUP,
  OPT_XTRA_PREPARE,
  OPT_XTRA_EXPORT,
  OPT_XTRA_PRINT_PARAM,
  OPT_XTRA_USE_MEMORY,
  OPT_XTRA_THROTTLE,
  OPT_XTRA_LOG_COPY_INTERVAL,
  OPT_XTRA_INCREMENTAL,
  OPT_XTRA_INCREMENTAL_BASEDIR,
  OPT_XTRA_EXTRA_LSNDIR,
  OPT_XTRA_INCREMENTAL_DIR,
  OPT_XTRA_TABLES,
  OPT_XTRA_TABLES_FILE,
  OPT_XTRA_DATABASES,
  OPT_XTRA_DATABASES_FILE,
  OPT_XTRA_PARALLEL,
  OPT_XTRA_STREAM,
  OPT_XTRA_COMPRESS,
  OPT_XTRA_COMPRESS_THREADS,
  OPT_XTRA_COMPRESS_CHUNK_SIZE,
  OPT_LOG,
  OPT_INNODB,
  OPT_INNODB_DATA_FILE_PATH,
  OPT_INNODB_DATA_HOME_DIR,
  OPT_INNODB_ADAPTIVE_HASH_INDEX,
  OPT_INNODB_DOUBLEWRITE,
  OPT_INNODB_FILE_PER_TABLE,
  OPT_INNODB_FLUSH_LOG_AT_TRX_COMMIT,
  OPT_INNODB_FLUSH_METHOD,
  OPT_INNODB_LOCKS_UNSAFE_FOR_BINLOG,
  OPT_INNODB_LOG_GROUP_HOME_DIR,
  OPT_INNODB_MAX_DIRTY_PAGES_PCT,
  OPT_INNODB_MAX_PURGE_LAG,
  OPT_INNODB_ROLLBACK_ON_TIMEOUT,
  OPT_INNODB_STATUS_FILE,
  OPT_INNODB_AUTOEXTEND_INCREMENT,
  OPT_INNODB_BUFFER_POOL_SIZE,
  OPT_INNODB_COMMIT_CONCURRENCY,
  OPT_INNODB_CONCURRENCY_TICKETS,
  OPT_INNODB_FILE_IO_THREADS,
  OPT_INNODB_IO_CAPACITY,
  OPT_INNODB_READ_IO_THREADS,
  OPT_INNODB_WRITE_IO_THREADS,
  OPT_INNODB_USE_NATIVE_AIO,
  OPT_INNODB_PAGE_SIZE,
  OPT_INNODB_BUFFER_POOL_FILENAME,
  OPT_INNODB_LOCK_WAIT_TIMEOUT,
  OPT_INNODB_LOG_BUFFER_SIZE,
  OPT_INNODB_LOG_FILE_SIZE,
  OPT_INNODB_LOG_FILES_IN_GROUP,
  OPT_INNODB_OPEN_FILES,
  OPT_XTRA_DEBUG_SYNC,
  OPT_INNODB_CHECKSUM_ALGORITHM,
  OPT_INNODB_UNDO_DIRECTORY,
  OPT_INNODB_UNDO_TABLESPACES,
  OPT_INNODB_LOG_CHECKSUMS,
  OPT_XTRA_INCREMENTAL_FORCE_SCAN,
  OPT_DEFAULTS_GROUP,
  OPT_OPEN_FILES_LIMIT,
  OPT_PLUGIN_DIR,
  OPT_PLUGIN_LOAD,
  OPT_INNODB_ENCRYPT_LOG,
  OPT_CLOSE_FILES,
  OPT_CORE_FILE,

  OPT_COPY_BACK,
  OPT_MOVE_BACK,
  OPT_GALERA_INFO,
  OPT_SLAVE_INFO,
  OPT_NO_LOCK,
  OPT_SAFE_SLAVE_BACKUP,
  OPT_RSYNC,
  OPT_FORCE_NON_EMPTY_DIRS,
  OPT_NO_VERSION_CHECK,
  OPT_NO_BACKUP_LOCKS,
  OPT_DECOMPRESS,
  OPT_INCREMENTAL_HISTORY_NAME,
  OPT_INCREMENTAL_HISTORY_UUID,
  OPT_REMOVE_ORIGINAL,
  OPT_LOCK_WAIT_QUERY_TYPE,
  OPT_KILL_LONG_QUERY_TYPE,
  OPT_HISTORY,
  OPT_KILL_LONG_QUERIES_TIMEOUT,
  OPT_LOCK_WAIT_TIMEOUT,
  OPT_LOCK_WAIT_THRESHOLD,
  OPT_DEBUG_SLEEP_BEFORE_UNLOCK,
  OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
  OPT_BINLOG_INFO,
  OPT_XB_SECURE_AUTH,

  OPT_XTRA_TABLES_EXCLUDE,
  OPT_XTRA_DATABASES_EXCLUDE,
  OPT_PROTOCOL,
  OPT_LOCK_DDL_PER_TABLE
};

struct my_option xb_client_options[] =
{
  {"version", 'v', "print xtrabackup version information",
   (G_PTR *) &xtrabackup_version, (G_PTR *) &xtrabackup_version, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},
  {"target-dir", OPT_XTRA_TARGET_DIR, "destination directory", (G_PTR*) &xtrabackup_target_dir,
   (G_PTR*) &xtrabackup_target_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"backup", OPT_XTRA_BACKUP, "take backup to target-dir",
   (G_PTR*) &xtrabackup_backup, (G_PTR*) &xtrabackup_backup,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"prepare", OPT_XTRA_PREPARE, "prepare a backup for starting mysql server on the backup.",
   (G_PTR*) &xtrabackup_prepare, (G_PTR*) &xtrabackup_prepare,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"export", OPT_XTRA_EXPORT, "create files to import to another database when prepare.",
   (G_PTR*) &xtrabackup_export, (G_PTR*) &xtrabackup_export,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"print-param", OPT_XTRA_PRINT_PARAM, "print parameter of mysqld needed for copyback.",
   (G_PTR*) &xtrabackup_print_param, (G_PTR*) &xtrabackup_print_param,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"use-memory", OPT_XTRA_USE_MEMORY, "The value is used instead of buffer_pool_size",
   (G_PTR*) &xtrabackup_use_memory, (G_PTR*) &xtrabackup_use_memory,
   0, GET_LL, REQUIRED_ARG, 100*1024*1024L, 1024*1024L, LONGLONG_MAX, 0,
   1024*1024L, 0},
  {"throttle", OPT_XTRA_THROTTLE, "limit count of IO operations (pairs of read&write) per second to IOS values (for '--backup')",
   (G_PTR*) &xtrabackup_throttle, (G_PTR*) &xtrabackup_throttle,
   0, GET_LONG, REQUIRED_ARG, 0, 0, LONG_MAX, 0, 1, 0},
  {"log", OPT_LOG, "Ignored option for MySQL option compatibility",
   (G_PTR*) &log_ignored_opt, (G_PTR*) &log_ignored_opt, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
  {"log-copy-interval", OPT_XTRA_LOG_COPY_INTERVAL, "time interval between checks done by log copying thread in milliseconds (default is 1 second).",
   (G_PTR*) &xtrabackup_log_copy_interval, (G_PTR*) &xtrabackup_log_copy_interval,
   0, GET_LONG, REQUIRED_ARG, 1000, 0, LONG_MAX, 0, 1, 0},
  {"extra-lsndir", OPT_XTRA_EXTRA_LSNDIR, "(for --backup): save an extra copy of the xtrabackup_checkpoints file in this directory.",
   (G_PTR*) &xtrabackup_extra_lsndir, (G_PTR*) &xtrabackup_extra_lsndir,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"incremental-lsn", OPT_XTRA_INCREMENTAL, "(for --backup): copy only .ibd pages newer than specified LSN 'high:low'. ##ATTENTION##: If a wrong LSN value is specified, it is impossible to diagnose this, causing the backup to be unusable. Be careful!",
   (G_PTR*) &xtrabackup_incremental, (G_PTR*) &xtrabackup_incremental,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"incremental-basedir", OPT_XTRA_INCREMENTAL_BASEDIR, "(for --backup): copy only .ibd pages newer than backup at specified directory.",
   (G_PTR*) &xtrabackup_incremental_basedir, (G_PTR*) &xtrabackup_incremental_basedir,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"incremental-dir", OPT_XTRA_INCREMENTAL_DIR, "(for --prepare): apply .delta files and logfile in the specified directory.",
   (G_PTR*) &xtrabackup_incremental_dir, (G_PTR*) &xtrabackup_incremental_dir,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tables", OPT_XTRA_TABLES, "filtering by regexp for table names.",
   (G_PTR*) &xtrabackup_tables, (G_PTR*) &xtrabackup_tables,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tables_file", OPT_XTRA_TABLES_FILE, "filtering by list of the exact database.table name in the file.",
   (G_PTR*) &xtrabackup_tables_file, (G_PTR*) &xtrabackup_tables_file,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"databases", OPT_XTRA_DATABASES, "filtering by list of databases.",
   (G_PTR*) &xtrabackup_databases, (G_PTR*) &xtrabackup_databases,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"databases_file", OPT_XTRA_DATABASES_FILE,
   "filtering by list of databases in the file.",
   (G_PTR*) &xtrabackup_databases_file, (G_PTR*) &xtrabackup_databases_file,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tables-exclude", OPT_XTRA_TABLES_EXCLUDE, "filtering by regexp for table names. "
  "Operates the same way as --tables, but matched names are excluded from backup. "
  "Note that this option has a higher priority than --tables.",
    (G_PTR*) &xtrabackup_tables_exclude, (G_PTR*) &xtrabackup_tables_exclude,
    0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"databases-exclude", OPT_XTRA_DATABASES_EXCLUDE, "Excluding databases based on name, "
  "Operates the same way as --databases, but matched names are excluded from backup. "
  "Note that this option has a higher priority than --databases.",
    (G_PTR*) &xtrabackup_databases_exclude, (G_PTR*) &xtrabackup_databases_exclude,
    0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"stream", OPT_XTRA_STREAM, "Stream all backup files to the standard output "
   "in the specified format." 
   "Supported format is 'xbstream'."
   ,
   (G_PTR*) &xtrabackup_stream_str, (G_PTR*) &xtrabackup_stream_str, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"compress", OPT_XTRA_COMPRESS, "Compress individual backup files using the "
   "specified compression algorithm. Currently the only supported algorithm "
   "is 'quicklz'. It is also the default algorithm, i.e. the one used when "
   "--compress is used without an argument.",
   (G_PTR*) &xtrabackup_compress_alg, (G_PTR*) &xtrabackup_compress_alg, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

  {"compress-threads", OPT_XTRA_COMPRESS_THREADS,
   "Number of threads for parallel data compression. The default value is 1.",
   (G_PTR*) &xtrabackup_compress_threads, (G_PTR*) &xtrabackup_compress_threads,
   0, GET_UINT, REQUIRED_ARG, 1, 1, UINT_MAX, 0, 0, 0},

  {"compress-chunk-size", OPT_XTRA_COMPRESS_CHUNK_SIZE,
   "Size of working buffer(s) for compression threads in bytes. The default value is 64K.",
   (G_PTR*) &xtrabackup_compress_chunk_size, (G_PTR*) &xtrabackup_compress_chunk_size,
   0, GET_ULL, REQUIRED_ARG, (1 << 16), 1024, ULONGLONG_MAX, 0, 0, 0},

  {"incremental-force-scan", OPT_XTRA_INCREMENTAL_FORCE_SCAN,
   "Perform a full-scan incremental backup even in the presence of changed "
   "page bitmap data",
   (G_PTR*)&xtrabackup_incremental_force_scan,
   (G_PTR*)&xtrabackup_incremental_force_scan, 0, GET_BOOL, NO_ARG,
   0, 0, 0, 0, 0, 0},


  {"close_files", OPT_CLOSE_FILES, "do not keep files opened. Use at your own "
   "risk.", (G_PTR*) &xb_close_files, (G_PTR*) &xb_close_files, 0, GET_BOOL,
   NO_ARG, 0, 0, 0, 0, 0, 0},

  {"core-file", OPT_CORE_FILE, "Write core on fatal signals", 0, 0, 0,
   GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0},


  {"copy-back", OPT_COPY_BACK, "Copy all the files in a previously made "
   "backup from the backup directory to their original locations.",
   (uchar *) &xtrabackup_copy_back, (uchar *) &xtrabackup_copy_back, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"move-back", OPT_MOVE_BACK, "Move all the files in a previously made "
   "backup from the backup directory to the actual datadir location. "
   "Use with caution, as it removes backup files.",
   (uchar *) &xtrabackup_move_back, (uchar *) &xtrabackup_move_back, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"galera-info", OPT_GALERA_INFO, "This options creates the "
   "xtrabackup_galera_info file which contains the local node state at "
   "the time of the backup. Option should be used when performing the "
   "backup of MariaDB Galera Cluster. Has no effect when backup locks "
   "are used to create the backup.",
   (uchar *) &opt_galera_info, (uchar *) &opt_galera_info, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"slave-info", OPT_SLAVE_INFO, "This option is useful when backing "
   "up a replication slave server. It prints the binary log position "
   "and name of the master server. It also writes this information to "
   "the \"xtrabackup_slave_info\" file as a \"CHANGE MASTER\" command. "
   "A new slave for this master can be set up by starting a slave server "
   "on this backup and issuing a \"CHANGE MASTER\" command with the "
   "binary log position saved in the \"xtrabackup_slave_info\" file.",
   (uchar *) &opt_slave_info, (uchar *) &opt_slave_info, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"no-lock", OPT_NO_LOCK, "Use this option to disable table lock "
   "with \"FLUSH TABLES WITH READ LOCK\". Use it only if ALL your "
   "tables are InnoDB and you DO NOT CARE about the binary log "
   "position of the backup. This option shouldn't be used if there "
   "are any DDL statements being executed or if any updates are "
   "happening on non-InnoDB tables (this includes the system MyISAM "
   "tables in the mysql database), otherwise it could lead to an "
   "inconsistent backup. If you are considering to use --no-lock "
   "because your backups are failing to acquire the lock, this could "
   "be because of incoming replication events preventing the lock "
   "from succeeding. Please try using --safe-slave-backup to "
   "momentarily stop the replication slave thread, this may help "
   "the backup to succeed and you then don't need to resort to "
   "using this option.",
   (uchar *) &opt_no_lock, (uchar *) &opt_no_lock, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"safe-slave-backup", OPT_SAFE_SLAVE_BACKUP, "Stop slave SQL thread "
   "and wait to start backup until Slave_open_temp_tables in "
   "\"SHOW STATUS\" is zero. If there are no open temporary tables, "
   "the backup will take place, otherwise the SQL thread will be "
   "started and stopped until there are no open temporary tables. "
   "The backup will fail if Slave_open_temp_tables does not become "
   "zero after --safe-slave-backup-timeout seconds. The slave SQL "
   "thread will be restarted when the backup finishes.",
   (uchar *) &opt_safe_slave_backup,
   (uchar *) &opt_safe_slave_backup,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"rsync", OPT_RSYNC, "Uses the rsync utility to optimize local file "
   "transfers. When this option is specified, innobackupex uses rsync "
   "to copy all non-InnoDB files instead of spawning a separate cp for "
   "each file, which can be much faster for servers with a large number "
   "of databases or tables.  This option cannot be used together with "
   "--stream.",
   (uchar *) &opt_rsync, (uchar *) &opt_rsync,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"force-non-empty-directories", OPT_FORCE_NON_EMPTY_DIRS, "This "
   "option, when specified, makes --copy-back or --move-back transfer "
   "files to non-empty directories. Note that no existing files will be "
   "overwritten. If --copy-back or --nove-back has to copy a file from "
   "the backup directory which already exists in the destination "
   "directory, it will still fail with an error.",
   (uchar *) &opt_force_non_empty_dirs,
   (uchar *) &opt_force_non_empty_dirs,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"no-version-check", OPT_NO_VERSION_CHECK, "This option disables the "
   "version check which is enabled by the --version-check option.",
   (uchar *) &opt_noversioncheck,
   (uchar *) &opt_noversioncheck,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"no-backup-locks", OPT_NO_BACKUP_LOCKS, "This option controls if "
   "backup locks should be used instead of FLUSH TABLES WITH READ LOCK "
   "on the backup stage. The option has no effect when backup locks are "
   "not supported by the server. This option is enabled by default, "
   "disable with --no-backup-locks.",
   (uchar *) &opt_no_backup_locks,
   (uchar *) &opt_no_backup_locks,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"decompress", OPT_DECOMPRESS, "Decompresses all files with the .qp "
   "extension in a backup previously made with the --compress option.",
   (uchar *) &opt_decompress,
   (uchar *) &opt_decompress,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"user", 'u', "This option specifies the MySQL username used "
   "when connecting to the server, if that's not the current user. "
   "The option accepts a string argument. See mysql --help for details.",
   (uchar*) &opt_user, (uchar*) &opt_user, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"host", 'H', "This option specifies the host to use when "
   "connecting to the database server with TCP/IP.  The option accepts "
   "a string argument. See mysql --help for details.",
   (uchar*) &opt_host, (uchar*) &opt_host, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"port", 'P', "This option specifies the port to use when "
   "connecting to the database server with TCP/IP.  The option accepts "
   "a string argument. See mysql --help for details.",
   &opt_port, &opt_port, 0, GET_UINT, REQUIRED_ARG,
   0, 0, 0, 0, 0, 0},

  {"password", 'p', "This option specifies the password to use "
   "when connecting to the database. It accepts a string argument.  "
   "See mysql --help for details.",
   0, 0, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"protocol", OPT_PROTOCOL, "The protocol to use for connection (tcp, socket, pipe, memory).",
   0, 0, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"socket", 'S', "This option specifies the socket to use when "
   "connecting to the local database server with a UNIX domain socket.  "
   "The option accepts a string argument. See mysql --help for details.",
   (uchar*) &opt_socket, (uchar*) &opt_socket, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"incremental-history-name", OPT_INCREMENTAL_HISTORY_NAME,
   "This option specifies the name of the backup series stored in the "
   "PERCONA_SCHEMA.xtrabackup_history history record to base an "
   "incremental backup on. Xtrabackup will search the history table "
   "looking for the most recent (highest innodb_to_lsn), successful "
   "backup in the series and take the to_lsn value to use as the "
   "starting lsn for the incremental backup. This will be mutually "
   "exclusive with --incremental-history-uuid, --incremental-basedir "
   "and --incremental-lsn. If no valid lsn can be found (no series by "
   "that name, no successful backups by that name) xtrabackup will "
   "return with an error. It is used with the --incremental option.",
   (uchar*) &opt_incremental_history_name,
   (uchar*) &opt_incremental_history_name, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"incremental-history-uuid", OPT_INCREMENTAL_HISTORY_UUID,
   "This option specifies the UUID of the specific history record "
   "stored in the PERCONA_SCHEMA.xtrabackup_history to base an "
   "incremental backup on. --incremental-history-name, "
   "--incremental-basedir and --incremental-lsn. If no valid lsn can be "
   "found (no success record with that uuid) xtrabackup will return "
   "with an error. It is used with the --incremental option.",
   (uchar*) &opt_incremental_history_uuid,
   (uchar*) &opt_incremental_history_uuid, 0, GET_STR,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"remove-original", OPT_REMOVE_ORIGINAL, "Remove .qp files after decompression.",
   (uchar *) &opt_remove_original,
   (uchar *) &opt_remove_original,
   0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  {"ftwrl-wait-query-type", OPT_LOCK_WAIT_QUERY_TYPE,
   "This option specifies which types of queries are allowed to complete "
   "before innobackupex will issue the global lock. Default is all.",
   (uchar*) &opt_lock_wait_query_type,
   (uchar*) &opt_lock_wait_query_type, &query_type_typelib,
   GET_ENUM, REQUIRED_ARG, QUERY_TYPE_ALL, 0, 0, 0, 0, 0},

  {"kill-long-query-type", OPT_KILL_LONG_QUERY_TYPE,
   "This option specifies which types of queries should be killed to "
   "unblock the global lock. Default is \"all\".",
   (uchar*) &opt_kill_long_query_type,
   (uchar*) &opt_kill_long_query_type, &query_type_typelib,
   GET_ENUM, REQUIRED_ARG, QUERY_TYPE_SELECT, 0, 0, 0, 0, 0},

  {"history", OPT_HISTORY,
   "This option enables the tracking of backup history in the "
   "PERCONA_SCHEMA.xtrabackup_history table. An optional history "
   "series name may be specified that will be placed with the history "
   "record for the current backup being taken.",
   NULL, NULL, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

  {"kill-long-queries-timeout", OPT_KILL_LONG_QUERIES_TIMEOUT,
   "This option specifies the number of seconds innobackupex waits "
   "between starting FLUSH TABLES WITH READ LOCK and killing those "
   "queries that block it. Default is 0 seconds, which means "
   "innobackupex will not attempt to kill any queries.",
   (uchar*) &opt_kill_long_queries_timeout,
   (uchar*) &opt_kill_long_queries_timeout, 0, GET_UINT,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"ftwrl-wait-timeout", OPT_LOCK_WAIT_TIMEOUT,
   "This option specifies time in seconds that innobackupex should wait "
   "for queries that would block FTWRL before running it. If there are "
   "still such queries when the timeout expires, innobackupex terminates "
   "with an error. Default is 0, in which case innobackupex does not "
   "wait for queries to complete and starts FTWRL immediately.",
   (uchar*) &opt_lock_wait_timeout,
   (uchar*) &opt_lock_wait_timeout, 0, GET_UINT,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"ftwrl-wait-threshold", OPT_LOCK_WAIT_THRESHOLD,
   "This option specifies the query run time threshold which is used by "
   "innobackupex to detect long-running queries with a non-zero value "
   "of --ftwrl-wait-timeout. FTWRL is not started until such "
   "long-running queries exist. This option has no effect if "
   "--ftwrl-wait-timeout is 0. Default value is 60 seconds.",
   (uchar*) &opt_lock_wait_threshold,
   (uchar*) &opt_lock_wait_threshold, 0, GET_UINT,
   REQUIRED_ARG, 60, 0, 0, 0, 0, 0},

  {"debug-sleep-before-unlock", OPT_DEBUG_SLEEP_BEFORE_UNLOCK,
   "This is a debug-only option used by the XtraBackup test suite.",
   (uchar*) &opt_debug_sleep_before_unlock,
   (uchar*) &opt_debug_sleep_before_unlock, 0, GET_UINT,
   REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"safe-slave-backup-timeout", OPT_SAFE_SLAVE_BACKUP_TIMEOUT,
   "How many seconds --safe-slave-backup should wait for "
   "Slave_open_temp_tables to become zero. (default 300)",
   (uchar*) &opt_safe_slave_backup_timeout,
   (uchar*) &opt_safe_slave_backup_timeout, 0, GET_UINT,
   REQUIRED_ARG, 300, 0, 0, 0, 0, 0},

  {"binlog-info", OPT_BINLOG_INFO,
   "This option controls how XtraBackup should retrieve server's binary log "
   "coordinates corresponding to the backup. Possible values are OFF, ON, "
   "LOCKLESS and AUTO. See the XtraBackup manual for more information",
   &opt_binlog_info, &opt_binlog_info,
   &binlog_info_typelib, GET_ENUM, OPT_ARG, BINLOG_INFO_AUTO, 0, 0, 0, 0, 0},

  {"secure-auth", OPT_XB_SECURE_AUTH, "Refuse client connecting to server if it"
    " uses old (pre-4.1.1) protocol.", &opt_secure_auth,
    &opt_secure_auth, 0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},

#include "sslopt-longopts.h"


  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

uint xb_client_options_count = array_elements(xb_client_options);

#ifndef DBUG_OFF
/** Parameters to DBUG */
static const char *dbug_option;
#endif

struct my_option xb_server_options[] =
{
  {"datadir", 'h', "Path to the database root.", (G_PTR*) &mysql_data_home,
   (G_PTR*) &mysql_data_home, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"tmpdir", 't',
   "Path for temporary files. Several paths may be specified, separated by a "
#if defined(__WIN__) || defined(OS2) || defined(__NETWARE__)
   "semicolon (;)"
#else
   "colon (:)"
#endif
   ", in this case they are used in a round-robin fashion.",
   (G_PTR*) &opt_mysql_tmpdir,
   (G_PTR*) &opt_mysql_tmpdir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"parallel", OPT_XTRA_PARALLEL,
   "Number of threads to use for parallel datafiles transfer. "
   "The default value is 1.",
   (G_PTR*) &xtrabackup_parallel, (G_PTR*) &xtrabackup_parallel, 0, GET_INT,
   REQUIRED_ARG, 1, 1, INT_MAX, 0, 0, 0},

   {"log", OPT_LOG, "Ignored option for MySQL option compatibility",
   (G_PTR*) &log_ignored_opt, (G_PTR*) &log_ignored_opt, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

   {"log_bin", OPT_LOG, "Base name for the log sequence",
   &opt_log_bin, &opt_log_bin, 0, GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},

   {"innodb", OPT_INNODB, "Ignored option for MySQL option compatibility",
   (G_PTR*) &innobase_ignored_opt, (G_PTR*) &innobase_ignored_opt, 0,
   GET_STR, OPT_ARG, 0, 0, 0, 0, 0, 0},
#ifdef BTR_CUR_HASH_ADAPT
  {"innodb_adaptive_hash_index", OPT_INNODB_ADAPTIVE_HASH_INDEX,
   "Enable InnoDB adaptive hash index (enabled by default).  "
   "Disable with --skip-innodb-adaptive-hash-index.",
   &btr_search_enabled,
   &btr_search_enabled,
   0, GET_BOOL, NO_ARG, 1, 0, 0, 0, 0, 0},
#endif /* BTR_CUR_HASH_ADAPT */
  {"innodb_autoextend_increment", OPT_INNODB_AUTOEXTEND_INCREMENT,
   "Data file autoextend increment in megabytes",
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   (G_PTR*) &sys_tablespace_auto_extend_increment,
   0, GET_ULONG, REQUIRED_ARG, 8L, 1L, 1000L, 0, 1L, 0},
  {"innodb_buffer_pool_size", OPT_INNODB_BUFFER_POOL_SIZE,
   "The size of the memory buffer InnoDB uses to cache data and indexes of its tables.",
   (G_PTR*) &innobase_buffer_pool_size, (G_PTR*) &innobase_buffer_pool_size, 0,
   GET_LL, REQUIRED_ARG, 8*1024*1024L, 1024*1024L, LONGLONG_MAX, 0,
   1024*1024L, 0},
  {"innodb_data_file_path", OPT_INNODB_DATA_FILE_PATH,
   "Path to individual files and their sizes.", &innobase_data_file_path,
   &innobase_data_file_path, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_data_home_dir", OPT_INNODB_DATA_HOME_DIR,
   "The common part for InnoDB table spaces.", &innobase_data_home_dir,
   &innobase_data_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_doublewrite", OPT_INNODB_DOUBLEWRITE,
   "Enable InnoDB doublewrite buffer during --prepare.",
   (G_PTR*) &innobase_use_doublewrite,
   (G_PTR*) &innobase_use_doublewrite, 0, GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_io_capacity", OPT_INNODB_IO_CAPACITY,
   "Number of IOPs the server can do. Tunes the background IO rate",
   (G_PTR*) &srv_io_capacity, (G_PTR*) &srv_io_capacity,
   0, GET_ULONG, OPT_ARG, 200, 100, ~0UL, 0, 0, 0},
  {"innodb_file_io_threads", OPT_INNODB_FILE_IO_THREADS,
   "Number of file I/O threads in InnoDB.", (G_PTR*) &innobase_file_io_threads,
   (G_PTR*) &innobase_file_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 4, 64, 0,
   1, 0},
  {"innodb_read_io_threads", OPT_INNODB_READ_IO_THREADS,
   "Number of background read I/O threads in InnoDB.", (G_PTR*) &innobase_read_io_threads,
   (G_PTR*) &innobase_read_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 1, 64, 0,
   1, 0},
  {"innodb_write_io_threads", OPT_INNODB_WRITE_IO_THREADS,
   "Number of background write I/O threads in InnoDB.", (G_PTR*) &innobase_write_io_threads,
   (G_PTR*) &innobase_write_io_threads, 0, GET_LONG, REQUIRED_ARG, 4, 1, 64, 0,
   1, 0},
  {"innodb_file_per_table", OPT_INNODB_FILE_PER_TABLE,
   "Stores each InnoDB table to an .ibd file in the database dir.",
   (G_PTR*) &innobase_file_per_table,
   (G_PTR*) &innobase_file_per_table, 0, GET_BOOL, NO_ARG,
   FALSE, 0, 0, 0, 0, 0},

  {"innodb_flush_method", OPT_INNODB_FLUSH_METHOD,
   "With which method to flush data.", (G_PTR*) &innobase_unix_file_flush_method,
   (G_PTR*) &innobase_unix_file_flush_method, 0, GET_STR, REQUIRED_ARG, 0, 0, 0,
   0, 0, 0},

  {"innodb_log_buffer_size", OPT_INNODB_LOG_BUFFER_SIZE,
   "The size of the buffer which InnoDB uses to write log to the log files on disk.",
   (G_PTR*) &innobase_log_buffer_size, (G_PTR*) &innobase_log_buffer_size, 0,
   GET_LONG, REQUIRED_ARG, 1024*1024L, 256*1024L, LONG_MAX, 0, 1024, 0},
  {"innodb_log_file_size", OPT_INNODB_LOG_FILE_SIZE,
   "Ignored for mysqld option compatibility",
   (G_PTR*) &srv_log_file_size, (G_PTR*) &srv_log_file_size, 0,
   GET_ULL, REQUIRED_ARG, 48 << 20, 1 << 20, 512ULL << 30, 0,
   UNIV_PAGE_SIZE_MAX, 0},
  {"innodb_log_files_in_group", OPT_INNODB_LOG_FILES_IN_GROUP,
   "Ignored for mysqld option compatibility",
   &srv_n_log_files, &srv_n_log_files,
   0, GET_LONG, REQUIRED_ARG, 1, 1, 100, 0, 1, 0},
  {"innodb_log_group_home_dir", OPT_INNODB_LOG_GROUP_HOME_DIR,
   "Path to InnoDB log files.", &srv_log_group_home_dir,
   &srv_log_group_home_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
  {"innodb_max_dirty_pages_pct", OPT_INNODB_MAX_DIRTY_PAGES_PCT,
   "Percentage of dirty pages allowed in bufferpool.", (G_PTR*) &srv_max_buf_pool_modified_pct,
   (G_PTR*) &srv_max_buf_pool_modified_pct, 0, GET_ULONG, REQUIRED_ARG, 90, 0, 100, 0, 0, 0},
  {"innodb_use_native_aio", OPT_INNODB_USE_NATIVE_AIO,
   "Use native AIO if supported on this platform.",
   (G_PTR*) &srv_use_native_aio,
   (G_PTR*) &srv_use_native_aio, 0, GET_BOOL, NO_ARG,
   FALSE, 0, 0, 0, 0, 0},
  {"innodb_page_size", OPT_INNODB_PAGE_SIZE,
   "The universal page size of the database.",
   (G_PTR*) &innobase_page_size, (G_PTR*) &innobase_page_size, 0,
   /* Use GET_LL to support numeric suffixes in 5.6 */
   GET_LL, REQUIRED_ARG,
   (1LL << 14), (1LL << 12), (1LL << UNIV_PAGE_SIZE_SHIFT_MAX), 0, 1L, 0},
  {"innodb_buffer_pool_filename", OPT_INNODB_BUFFER_POOL_FILENAME,
   "Ignored for mysqld option compatibility",
   (G_PTR*) &innobase_buffer_pool_filename,
   (G_PTR*) &innobase_buffer_pool_filename,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

#ifndef DBUG_OFF /* unfortunately "debug" collides with existing options */
  {"dbug", '#', "Built in DBUG debugger.",
   &dbug_option, &dbug_option, 0, GET_STR, OPT_ARG,
   0, 0, 0, 0, 0, 0},
#endif
#ifndef __WIN__
  {"debug-sync", OPT_XTRA_DEBUG_SYNC,
   "Debug sync point. This is only used by the xtrabackup test suite",
   (G_PTR*) &xtrabackup_debug_sync,
   (G_PTR*) &xtrabackup_debug_sync,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},
#endif

  {"innodb_checksum_algorithm", OPT_INNODB_CHECKSUM_ALGORITHM,
  "The algorithm InnoDB uses for page checksumming. [CRC32, STRICT_CRC32, "
   "INNODB, STRICT_INNODB, NONE, STRICT_NONE]", &srv_checksum_algorithm,
   &srv_checksum_algorithm, &innodb_checksum_algorithm_typelib, GET_ENUM,
   REQUIRED_ARG, SRV_CHECKSUM_ALGORITHM_INNODB, 0, 0, 0, 0, 0},

  {"innodb_undo_directory", OPT_INNODB_UNDO_DIRECTORY,
   "Directory where undo tablespace files live, this path can be absolute.",
   &srv_undo_dir, &srv_undo_dir, 0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0,
   0},

  {"innodb_undo_tablespaces", OPT_INNODB_UNDO_TABLESPACES,
   "Number of undo tablespaces to use.",
   (G_PTR*)&srv_undo_tablespaces, (G_PTR*)&srv_undo_tablespaces,
   0, GET_ULONG, REQUIRED_ARG, 0, 0, 126, 0, 1, 0},

  {"defaults_group", OPT_DEFAULTS_GROUP, "defaults group in config file (default \"mysqld\").",
   (G_PTR*) &defaults_group, (G_PTR*) &defaults_group,
   0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0},

  {"plugin-dir", OPT_PLUGIN_DIR,
  "Server plugin directory. Used to load encryption plugin during 'prepare' phase."
  "Has no effect in the 'backup' phase (plugin directory during backup is the same as server's)",
  &xb_plugin_dir, &xb_plugin_dir,
  0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

  { "plugin-load", OPT_PLUGIN_LOAD, "encrypton plugin to load during 'prepare' phase.",
  &xb_plugin_load, &xb_plugin_load,
  0, GET_STR, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

  { "innodb-encrypt-log", OPT_INNODB_ENCRYPT_LOG, "encrypton plugin to load",
  &srv_encrypt_log, &srv_encrypt_log,
  0, GET_BOOL, REQUIRED_ARG, 0, 0, 0, 0, 0, 0 },

  {"innodb-log-checksums", OPT_INNODB_LOG_CHECKSUMS,
   "Whether to require checksums for InnoDB redo log blocks",
   &innodb_log_checksums, &innodb_log_checksums,
   0, GET_BOOL, REQUIRED_ARG, 1, 0, 0, 0, 0, 0 },

  {"open_files_limit", OPT_OPEN_FILES_LIMIT, "the maximum number of file "
   "descriptors to reserve with setrlimit().",
   (G_PTR*) &xb_open_files_limit, (G_PTR*) &xb_open_files_limit, 0, GET_ULONG,
   REQUIRED_ARG, 0, 0, UINT_MAX, 0, 1, 0},

  {"lock-ddl-per-table", OPT_LOCK_DDL_PER_TABLE, "Lock DDL for each table "
   "before xtrabackup starts to copy it and until the backup is completed.",
   (uchar*) &opt_lock_ddl_per_table, (uchar*) &opt_lock_ddl_per_table, 0,
   GET_BOOL, NO_ARG, 0, 0, 0, 0, 0, 0},

  { 0, 0, 0, 0, 0, 0, GET_NO_ARG, NO_ARG, 0, 0, 0, 0, 0, 0}
};

uint xb_server_options_count = array_elements(xb_server_options);

#ifndef __WIN__
static int debug_sync_resumed;

static void sigcont_handler(int sig);

static void sigcont_handler(int sig __attribute__((unused)))
{
	debug_sync_resumed= 1;
}
#endif

static inline
void
debug_sync_point(const char *name)
{
#ifndef __WIN__
	FILE	*fp;
	pid_t	pid;
	char	pid_path[FN_REFLEN];

	if (xtrabackup_debug_sync == NULL) {
		return;
	}

	if (strcmp(xtrabackup_debug_sync, name)) {
		return;
	}

	pid = getpid();

	snprintf(pid_path, sizeof(pid_path), "%s/xtrabackup_debug_sync",
		 xtrabackup_target_dir);
	fp = fopen(pid_path, "w");
	if (fp == NULL) {
		msg("mariabackup: Error: cannot open %s\n", pid_path);
		exit(EXIT_FAILURE);
	}
	fprintf(fp, "%u\n", (uint) pid);
	fclose(fp);

	msg("mariabackup: DEBUG: Suspending at debug sync point '%s'. "
	    "Resume with 'kill -SIGCONT %u'.\n", name, (uint) pid);

	debug_sync_resumed= 0;
	kill(pid, SIGSTOP);
	while (!debug_sync_resumed) {
		sleep(1);
	}

	/* On resume */
	msg("mariabackup: DEBUG: removing the pid file.\n");
	my_delete(pid_path, MYF(MY_WME));
#endif
}


static std::vector<std::string> tables_for_export;

static void append_export_table(const char *dbname, const char *tablename, bool is_remote)
{
  if(dbname && tablename && !is_remote)
  {
    char buf[3*FN_REFLEN];
    snprintf(buf,sizeof(buf),"%s/%s",dbname, tablename);
    // trim .ibd
    char *p=strrchr(buf, '.');
    if (p) *p=0;

    tables_for_export.push_back(ut_get_name(0,buf));
  }
}


#define BOOTSTRAP_FILENAME "mariabackup_prepare_for_export.sql"

static int create_bootstrap_file()
{
  FILE *f= fopen(BOOTSTRAP_FILENAME,"wb");
  if(!f)
   return -1;

  fputs("SET NAMES UTF8;\n",f);
  enumerate_ibd_files(append_export_table);
  for (size_t i= 0; i < tables_for_export.size(); i++)
  {
     const char *tab = tables_for_export[i].c_str();
     fprintf(f,
     "BEGIN NOT ATOMIC "
       "DECLARE CONTINUE HANDLER FOR NOT FOUND,SQLEXCEPTION BEGIN END;"
       "FLUSH TABLES %s FOR EXPORT;"
     "END;\n"
     "UNLOCK TABLES;\n",
      tab);
  }
  fclose(f);
  return 0;
}

static int prepare_export()
{
  int err= -1;

  char cmdline[2*FN_REFLEN];
  FILE *outf;

  if (create_bootstrap_file())
    return -1;

  // Process defaults-file , it can have some --lc-language stuff,
  // which is* unfortunately* still necessary to get mysqld up
  if (strncmp(orig_argv1,"--defaults-file=",16) == 0)
  {
    sprintf(cmdline, 
     IF_WIN("\"","") "\"%s\" --mysqld \"%s\" "
      " --defaults-extra-file=./backup-my.cnf --defaults-group-suffix=%s --datadir=."
      " --innodb --innodb-fast-shutdown=0"
      " --innodb_purge_rseg_truncate_frequency=1 --innodb-buffer-pool-size=%llu"
      " --console  --skip-log-error --bootstrap  < "  BOOTSTRAP_FILENAME IF_WIN("\"",""),
      mariabackup_exe, 
      orig_argv1, (my_defaults_group_suffix?my_defaults_group_suffix:""),
      xtrabackup_use_memory);
  }
  else
  {
    sprintf(cmdline,
     IF_WIN("\"","") "\"%s\" --mysqld"
      " --defaults-file=./backup-my.cnf --defaults-group-suffix=%s --datadir=."
      " --innodb --innodb-fast-shutdown=0"
      " --innodb_purge_rseg_truncate_frequency=1 --innodb-buffer-pool-size=%llu"
      " --console  --log-error= --bootstrap  < "  BOOTSTRAP_FILENAME IF_WIN("\"",""),
      mariabackup_exe,
      (my_defaults_group_suffix?my_defaults_group_suffix:""),
      xtrabackup_use_memory);
  }

  msg("Prepare export : executing %s\n", cmdline);
  fflush(stderr);

  outf= popen(cmdline,"r");
  if (!outf)
    goto end;
  
  char outline[FN_REFLEN];
  while(fgets(outline, sizeof(outline)-1, outf))
    fprintf(stderr,"%s",outline);

  err = pclose(outf);
end:
  unlink(BOOTSTRAP_FILENAME);
  return err;
}


static const char *xb_client_default_groups[]=
	{ "xtrabackup", "mariabackup", "client", 0, 0, 0 };

static const char *xb_server_default_groups[]=
	{ "xtrabackup", "mariabackup", "mysqld", 0, 0, 0 };

static void print_version(void)
{
  msg("%s based on MariaDB server %s %s (%s) \n",
      my_progname, MYSQL_SERVER_VERSION, SYSTEM_TYPE, MACHINE_TYPE);
}

static void usage(void)
{
  puts("Open source backup tool for InnoDB and XtraDB\n\
\n\
Copyright (C) 2009-2015 Percona LLC and/or its affiliates.\n\
Portions Copyright (C) 2000, 2011, MySQL AB & Innobase Oy. All Rights Reserved.\n\
\n\
This program is free software; you can redistribute it and/or\n\
modify it under the terms of the GNU General Public License\n\
as published by the Free Software Foundation version 2\n\
of the License.\n\
\n\
This program is distributed in the hope that it will be useful,\n\
but WITHOUT ANY WARRANTY; without even the implied warranty of\n\
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the\n\
GNU General Public License for more details.\n\
\n\
You can download full text of the license on http://www.gnu.org/licenses/gpl-2.0.txt\n");

  printf("Usage: %s [--defaults-file=#] [--backup | --prepare | --copy-back | --move-back] [OPTIONS]\n",my_progname);
  print_defaults("my", xb_server_default_groups);
  my_print_help(xb_client_options);
  my_print_help(xb_server_options);
  my_print_variables(xb_server_options);
  my_print_variables(xb_client_options);
}

#define ADD_PRINT_PARAM_OPT(value)              \
  { \
    print_param_str << opt->name << "=" << value << "\n"; \
    param_set.insert(opt->name); \
  }

/************************************************************************
Check if parameter is set in defaults file or via command line argument
@return true if parameter is set. */
bool
check_if_param_set(const char *param)
{
	return param_set.find(param) != param_set.end();
}

my_bool
xb_get_one_option(int optid,
		  const struct my_option *opt __attribute__((unused)),
		  char *argument)
{
  switch(optid) {
  case 'h':
    strmake(mysql_real_data_home,argument, FN_REFLEN - 1);
    mysql_data_home= mysql_real_data_home;

    ADD_PRINT_PARAM_OPT(mysql_real_data_home);
    break;

  case 't':

    ADD_PRINT_PARAM_OPT(opt_mysql_tmpdir);
    break;

  case OPT_INNODB_DATA_HOME_DIR:

    ADD_PRINT_PARAM_OPT(innobase_data_home_dir);
    break;

  case OPT_INNODB_DATA_FILE_PATH:

    ADD_PRINT_PARAM_OPT(innobase_data_file_path);
    break;

  case OPT_INNODB_LOG_GROUP_HOME_DIR:

    ADD_PRINT_PARAM_OPT(srv_log_group_home_dir);
    break;

  case OPT_INNODB_LOG_FILES_IN_GROUP:
  case OPT_INNODB_LOG_FILE_SIZE:
    break;

  case OPT_INNODB_FLUSH_METHOD:

    ADD_PRINT_PARAM_OPT(innobase_unix_file_flush_method);
    break;

  case OPT_INNODB_PAGE_SIZE:

    ADD_PRINT_PARAM_OPT(innobase_page_size);
    break;

  case OPT_INNODB_UNDO_DIRECTORY:

    ADD_PRINT_PARAM_OPT(srv_undo_dir);
    break;

  case OPT_INNODB_UNDO_TABLESPACES:

    ADD_PRINT_PARAM_OPT(srv_undo_tablespaces);
    break;

  case OPT_INNODB_CHECKSUM_ALGORITHM:

    ut_a(srv_checksum_algorithm <= SRV_CHECKSUM_ALGORITHM_STRICT_NONE);

    ADD_PRINT_PARAM_OPT(innodb_checksum_algorithm_names[srv_checksum_algorithm]);
    break;

  case OPT_INNODB_BUFFER_POOL_FILENAME:

    ADD_PRINT_PARAM_OPT(innobase_buffer_pool_filename);
    break;

  case OPT_XTRA_TARGET_DIR:
    strmake(xtrabackup_real_target_dir,argument, sizeof(xtrabackup_real_target_dir)-1);
    xtrabackup_target_dir= xtrabackup_real_target_dir;
    break;
  case OPT_XTRA_STREAM:
    if (!strcasecmp(argument, "xbstream"))
      xtrabackup_stream_fmt = XB_STREAM_FMT_XBSTREAM;
    else
    {
      msg("Invalid --stream argument: %s\n", argument);
      return 1;
    }
    xtrabackup_stream = TRUE;
    break;
  case OPT_XTRA_COMPRESS:
    if (argument == NULL)
      xtrabackup_compress_alg = "quicklz";
    else if (strcasecmp(argument, "quicklz"))
    {
      msg("Invalid --compress argument: %s\n", argument);
      return 1;
    }
    xtrabackup_compress = TRUE;
    break;
  case OPT_DECOMPRESS:
    opt_decompress = TRUE;
    xtrabackup_decrypt_decompress = true;
    break;
  case (int) OPT_CORE_FILE:
    test_flags |= TEST_CORE_ON_SIGNAL;
    break;
  case OPT_HISTORY:
    if (argument) {
      opt_history = argument;
    } else {
      opt_history = "";
    }
    break;
  case 'p':
    if (argument)
    {
      char *start= argument;
      my_free(opt_password);
      opt_password= my_strdup(argument, MYF(MY_FAE));
      while (*argument) *argument++= 'x';               // Destroy argument
      if (*start)
        start[1]=0 ;
    }
    break;
  case OPT_PROTOCOL:
    if (argument)
    {
      if ((opt_protocol= find_type_with_warning(argument, &sql_protocol_typelib,
                                                opt->name)) <= 0)
      {
        sf_leaking_memory= 1; /* no memory leak reports here */
        exit(1);
      }
    }
    break;
#include "sslopt-case.h"

  case '?':
    usage();
    exit(EXIT_SUCCESS);
    break;
  case 'v':
    print_version();
    exit(EXIT_SUCCESS);
    break;
  default:
    break;
  }
  return 0;
}

static my_bool
innodb_init_param(void)
{
	srv_is_being_started = TRUE;
	/* === some variables from mysqld === */
	memset((G_PTR) &mysql_tmpdir_list, 0, sizeof(mysql_tmpdir_list));

	if (init_tmpdir(&mysql_tmpdir_list, opt_mysql_tmpdir))
		exit(EXIT_FAILURE);
	xtrabackup_tmpdir = my_tmpdir(&mysql_tmpdir_list);
	/* dummy for initialize all_charsets[] */
	get_charset_name(0);

	srv_page_size = 0;
	srv_page_size_shift = 0;
#ifdef BTR_CUR_HASH_ADAPT
	btr_ahi_parts = 1;
#endif /* BTR_CUR_HASH_ADAPT */

	if (innobase_page_size != (1LL << 14)) {
		int n_shift = (int)get_bit_shift((ulint) innobase_page_size);

		if (n_shift >= 12 && n_shift <= UNIV_PAGE_SIZE_SHIFT_MAX) {
			srv_page_size_shift = n_shift;
			srv_page_size = 1 << n_shift;
			msg("InnoDB: The universal page size of the "
			    "database is set to %lu.\n", srv_page_size);
		} else {
			msg("InnoDB: Error: invalid value of "
			    "innobase_page_size: %lld", innobase_page_size);
			exit(EXIT_FAILURE);
		}
	} else {
		srv_page_size_shift = 14;
		srv_page_size = (1 << srv_page_size_shift);
	}

	/* Check that values don't overflow on 32-bit systems. */
	if (sizeof(ulint) == 4) {
		if (xtrabackup_use_memory > UINT_MAX32) {
			msg("mariabackup: use-memory can't be over 4GB"
			    " on 32-bit systems\n");
		}

		if (innobase_buffer_pool_size > UINT_MAX32) {
			msg("mariabackup: innobase_buffer_pool_size can't be "
			    "over 4GB on 32-bit systems\n");

			goto error;
		}
	}

	static char default_path[2] = { FN_CURLIB, 0 };
	fil_path_to_mysql_datadir = default_path;

	/* Set InnoDB initialization parameters according to the values
	read from MySQL .cnf file */

	if (xtrabackup_backup) {
		msg("mariabackup: using the following InnoDB configuration:\n");
	} else {
		msg("mariabackup: using the following InnoDB configuration "
		    "for recovery:\n");
	}

	/*--------------- Data files -------------------------*/

	/* The default dir for data files is the datadir of MySQL */

	srv_data_home = (xtrabackup_backup && innobase_data_home_dir
			 ? innobase_data_home_dir : default_path);
	msg("mariabackup:   innodb_data_home_dir = %s\n", srv_data_home);

	/* Set default InnoDB data file size to 10 MB and let it be
  	auto-extending. Thus users can use InnoDB in >= 4.0 without having
	to specify any startup options. */

	if (!innobase_data_file_path) {
  		innobase_data_file_path = (char*) "ibdata1:10M:autoextend";
	}
	msg("mariabackup:   innodb_data_file_path = %s\n",
	    innobase_data_file_path);

	/* This is the first time univ_page_size is used.
	It was initialized to 16k pages before srv_page_size was set */
	univ_page_size.copy_from(
		page_size_t(srv_page_size, srv_page_size, false));

	srv_sys_space.set_space_id(TRX_SYS_SPACE);
	srv_sys_space.set_name("innodb_system");
	srv_sys_space.set_path(srv_data_home);
	srv_sys_space.set_flags(FSP_FLAGS_PAGE_SSIZE());

	if (!srv_sys_space.parse_params(innobase_data_file_path, true)) {
		goto error;
	}

	/* -------------- Log files ---------------------------*/

	/* The default dir for log files is the datadir of MySQL */

	if (!(xtrabackup_backup && srv_log_group_home_dir)) {
		srv_log_group_home_dir = default_path;
	}
	if (xtrabackup_prepare && xtrabackup_incremental_dir) {
		srv_log_group_home_dir = xtrabackup_incremental_dir;
	}
	msg("mariabackup:   innodb_log_group_home_dir = %s\n",
	    srv_log_group_home_dir);

	os_normalize_path(srv_log_group_home_dir);

	if (strchr(srv_log_group_home_dir, ';')) {

		msg("syntax error in innodb_log_group_home_dir, ");
		goto error;
	}

	srv_adaptive_flushing = FALSE;

	srv_file_flush_method_str = innobase_unix_file_flush_method;

	srv_log_buffer_size = (ulint) innobase_log_buffer_size;

        /* We set srv_pool_size here in units of 1 kB. InnoDB internally
        changes the value so that it becomes the number of database pages. */

	srv_buf_pool_size = (ulint) xtrabackup_use_memory;
	srv_buf_pool_chunk_unit = (ulong)srv_buf_pool_size;
	srv_buf_pool_instances = 1;

	srv_n_file_io_threads = (ulint) innobase_file_io_threads;
	srv_n_read_io_threads = (ulint) innobase_read_io_threads;
	srv_n_write_io_threads = (ulint) innobase_write_io_threads;

	srv_use_doublewrite_buf = (ibool) innobase_use_doublewrite;

	os_use_large_pages = (ibool) innobase_use_large_pages;
	os_large_page_size = (ulint) innobase_large_page_size;
	row_rollback_on_timeout = (ibool) innobase_rollback_on_timeout;

	srv_file_per_table = (my_bool) innobase_file_per_table;

        srv_locks_unsafe_for_binlog = (ibool) innobase_locks_unsafe_for_binlog;

	srv_max_n_open_files = ULINT_UNDEFINED;
	srv_innodb_status = (ibool) innobase_create_status_file;

	srv_print_verbose_log = 1;

	/* Store the default charset-collation number of this MySQL
	installation */

	/* We cannot treat characterset here for now!! */
	data_mysql_default_charset_coll = (ulint)default_charset_info->number;

	ut_a(DATA_MYSQL_BINARY_CHARSET_COLL == my_charset_bin.number);

	//innobase_commit_concurrency_init_default();

	/* Since we in this module access directly the fields of a trx
        struct, and due to different headers and flags it might happen that
	mutex_t has a different size in this module and in InnoDB
	modules, we check at run time that the size is the same in
	these compilation modules. */

	/* On 5.5+ srv_use_native_aio is TRUE by default. It is later reset
	if it is not supported by the platform in
	innobase_start_or_create_for_mysql(). As we don't call it in xtrabackup,
	we have to duplicate checks from that function here. */

#ifdef _WIN32
	srv_use_native_aio = TRUE;

#elif defined(LINUX_NATIVE_AIO)

	if (srv_use_native_aio) {
		ut_print_timestamp(stderr);
		msg(" InnoDB: Using Linux native AIO\n");
	}
#else
	/* Currently native AIO is supported only on windows and linux
	and that also when the support is compiled in. In all other
	cases, we ignore the setting of innodb_use_native_aio. */
	srv_use_native_aio = FALSE;

#endif

	/* Assign the default value to srv_undo_dir if it's not specified, as
	my_getopt does not support default values for string options. We also
	ignore the option and override innodb_undo_directory on --prepare,
	because separate undo tablespaces are copied to the root backup
	directory. */

	if (!srv_undo_dir || !xtrabackup_backup) {
		srv_undo_dir = (char*) ".";
	}

	log_checksum_algorithm_ptr = innodb_log_checksums || srv_encrypt_log
		? log_block_calc_checksum_crc32
		: log_block_calc_checksum_none;

	return(FALSE);

error:
	msg("mariabackup: innodb_init_param(): Error occured.\n");
	return(TRUE);
}

static bool innodb_init()
{
	dberr_t err = innobase_start_or_create_for_mysql();
	if (err != DB_SUCCESS) {
		msg("mariabackup: innodb_init() returned %d (%s).\n",
		    err, ut_strerr(err));
		innodb_shutdown();
		return(TRUE);
	}

	return(FALSE);
}

/* ================= common ================= */

/***********************************************************************
Read backup meta info.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_read_metadata(char *filename)
{
	FILE	*fp;
	my_bool	 r = TRUE;
	int	 t;

	fp = fopen(filename,"r");
	if(!fp) {
		msg("mariabackup: Error: cannot open %s\n", filename);
		return(FALSE);
	}

	if (fscanf(fp, "backup_type = %29s\n", metadata_type)
	    != 1) {
		r = FALSE;
		goto end;
	}
	/* Use UINT64PF instead of LSN_PF here, as we have to maintain the file
	format. */
	if (fscanf(fp, "from_lsn = " UINT64PF "\n", &metadata_from_lsn)
			!= 1) {
		r = FALSE;
		goto end;
	}
	if (fscanf(fp, "to_lsn = " UINT64PF "\n", &metadata_to_lsn)
			!= 1) {
		r = FALSE;
		goto end;
	}
	if (fscanf(fp, "last_lsn = " UINT64PF "\n", &metadata_last_lsn)
			!= 1) {
		metadata_last_lsn = 0;
	}
	/* Optional fields */

	if (fscanf(fp, "recover_binlog_info = %d\n", &t) == 1) {
		recover_binlog_info = (t == 1);
	}
end:
	fclose(fp);

	return(r);
}

/***********************************************************************
Print backup meta info to a specified buffer. */
static
void
xtrabackup_print_metadata(char *buf, size_t buf_len)
{
	/* Use UINT64PF instead of LSN_PF here, as we have to maintain the file
	format. */
	snprintf(buf, buf_len,
		 "backup_type = %s\n"
		 "from_lsn = " UINT64PF "\n"
		 "to_lsn = " UINT64PF "\n"
		 "last_lsn = " UINT64PF "\n"
		 "recover_binlog_info = %d\n",
		 metadata_type,
		 metadata_from_lsn,
		 metadata_to_lsn,
		 metadata_last_lsn,
		 MY_TEST(opt_binlog_info == BINLOG_INFO_LOCKLESS));
}

/***********************************************************************
Stream backup meta info to a specified datasink.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_stream_metadata(ds_ctxt_t *ds_ctxt)
{
	char		buf[1024];
	size_t		len;
	ds_file_t	*stream;
	MY_STAT		mystat;
	my_bool		rc = TRUE;

	xtrabackup_print_metadata(buf, sizeof(buf));

	len = strlen(buf);

	mystat.st_size = len;
	mystat.st_mtime = my_time(0);

	stream = ds_open(ds_ctxt, XTRABACKUP_METADATA_FILENAME, &mystat);
	if (stream == NULL) {
		msg("mariabackup: Error: cannot open output stream "
		    "for %s\n", XTRABACKUP_METADATA_FILENAME);
		return(FALSE);
	}

	if (ds_write(stream, buf, len)) {
		rc = FALSE;
	}

	if (ds_close(stream)) {
		rc = FALSE;
	}

	return(rc);
}

/***********************************************************************
Write backup meta info to a specified file.
@return TRUE on success, FALSE on failure. */
static
my_bool
xtrabackup_write_metadata(const char *filepath)
{
	char		buf[1024];
	size_t		len;
	FILE		*fp;

	xtrabackup_print_metadata(buf, sizeof(buf));

	len = strlen(buf);

	fp = fopen(filepath, "w");
	if(!fp) {
		msg("mariabackup: Error: cannot open %s\n", filepath);
		return(FALSE);
	}
	if (fwrite(buf, len, 1, fp) < 1) {
		fclose(fp);
		return(FALSE);
	}

	fclose(fp);

	return(TRUE);
}

/***********************************************************************
Read meta info for an incremental delta.
@return TRUE on success, FALSE on failure. */
static my_bool
xb_read_delta_metadata(const char *filepath, xb_delta_info_t *info)
{
	FILE*	fp;
	char	key[51];
	char	value[51];
	my_bool	r			= TRUE;

	/* set defaults */
	ulint page_size = ULINT_UNDEFINED, zip_size = 0;
	info->space_id = ULINT_UNDEFINED;

	fp = fopen(filepath, "r");
	if (!fp) {
		/* Meta files for incremental deltas are optional */
		return(TRUE);
	}

	while (!feof(fp)) {
		if (fscanf(fp, "%50s = %50s\n", key, value) == 2) {
			if (strcmp(key, "page_size") == 0) {
				page_size = strtoul(value, NULL, 10);
			} else if (strcmp(key, "zip_size") == 0) {
				zip_size = strtoul(value, NULL, 10);
			} else if (strcmp(key, "space_id") == 0) {
				info->space_id = strtoul(value, NULL, 10);
			}
		}
	}

	fclose(fp);

	if (page_size == ULINT_UNDEFINED) {
		msg("mariabackup: page_size is required in %s\n", filepath);
		r = FALSE;
	} else {
		info->page_size = page_size_t(zip_size ? zip_size : page_size,
					      page_size, zip_size != 0);
	}

	if (info->space_id == ULINT_UNDEFINED) {
		msg("mariabackup: Warning: This backup was taken with XtraBackup 2.0.1 "
			"or earlier, some DDL operations between full and incremental "
			"backups may be handled incorrectly\n");
	}

	return(r);
}

/***********************************************************************
Write meta info for an incremental delta.
@return TRUE on success, FALSE on failure. */
my_bool
xb_write_delta_metadata(const char *filename, const xb_delta_info_t *info)
{
	ds_file_t	*f;
	char		buf[64];
	my_bool		ret;
	size_t		len;
	MY_STAT		mystat;

	snprintf(buf, sizeof(buf),
		 "page_size = " ULINTPF "\n"
		 "zip_size = " ULINTPF " \n"
		 "space_id = " ULINTPF "\n",
		 info->page_size.logical(),
		 info->page_size.is_compressed()
		 ? info->page_size.physical() : 0,
		 info->space_id);
	len = strlen(buf);

	mystat.st_size = len;
	mystat.st_mtime = my_time(0);

	f = ds_open(ds_meta, filename, &mystat);
	if (f == NULL) {
		msg("mariabackup: Error: cannot open output stream for %s\n",
		    filename);
		return(FALSE);
	}

	ret = (ds_write(f, buf, len) == 0);

	if (ds_close(f)) {
		ret = FALSE;
	}

	return(ret);
}

/* ================= backup ================= */
void
xtrabackup_io_throttling(void)
{
	if (xtrabackup_backup && xtrabackup_throttle && (io_ticket--) < 0) {
		os_event_reset(wait_throttle);
		os_event_wait(wait_throttle);
	}
}

static
my_bool regex_list_check_match(
	const regex_list_t& list,
	const char* name)
{
	regmatch_t tables_regmatch[1];
	for (regex_list_t::const_iterator i = list.begin(), end = list.end();
	     i != end; ++i) {
		const regex_t& regex = *i;
		int regres = regexec(&regex, name, 1, tables_regmatch, 0);

		if (regres != REG_NOMATCH) {
			return(TRUE);
		}
	}
	return(FALSE);
}

static
my_bool
find_filter_in_hashtable(
	const char* name,
	hash_table_t* table,
	xb_filter_entry_t** result
)
{
	xb_filter_entry_t* found = NULL;
	HASH_SEARCH(name_hash, table, ut_fold_string(name),
		    xb_filter_entry_t*,
		    found, (void) 0,
		    !strcmp(found->name, name));

	if (found && result) {
		*result = found;
	}
	return (found != NULL);
}

/************************************************************************
Checks if a given table name matches any of specifications given in
regex_list or tables_hash.

@return TRUE on match or both regex_list and tables_hash are empty.*/
static my_bool
check_if_table_matches_filters(const char *name,
	const regex_list_t& regex_list,
	hash_table_t* tables_hash)
{
	if (regex_list.empty() && !tables_hash) {
		return(FALSE);
	}

	if (regex_list_check_match(regex_list, name)) {
		return(TRUE);
	}

	if (tables_hash && find_filter_in_hashtable(name, tables_hash, NULL)) {
		return(TRUE);
	}

	return FALSE;
}

enum skip_database_check_result {
	DATABASE_SKIP,
	DATABASE_SKIP_SOME_TABLES,
	DATABASE_DONT_SKIP,
	DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED,
};

/************************************************************************
Checks if a database specified by name should be skipped from backup based on
the --databases, --databases_file or --databases_exclude options.

@return TRUE if entire database should be skipped,
	FALSE otherwise.
*/
static
skip_database_check_result
check_if_skip_database(
	const char* name  /*!< in: path to the database */
)
{
	/* There are some filters for databases, check them */
	xb_filter_entry_t*	database = NULL;

	if (databases_exclude_hash &&
		find_filter_in_hashtable(name, databases_exclude_hash,
					 &database) &&
		!database->has_tables) {
		/* Database is found and there are no tables specified,
		   skip entire db. */
		return DATABASE_SKIP;
	}

	if (databases_include_hash) {
		if (!find_filter_in_hashtable(name, databases_include_hash,
					      &database)) {
		/* Database isn't found, skip the database */
			return DATABASE_SKIP;
		} else if (database->has_tables) {
			return DATABASE_SKIP_SOME_TABLES;
		} else {
			return DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED;
		}
	}

	return DATABASE_DONT_SKIP;
}

/************************************************************************
Checks if a database specified by path should be skipped from backup based on
the --databases, --databases_file or --databases_exclude options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_database_by_path(
	const char* path /*!< in: path to the db directory. */
)
{
	if (databases_include_hash == NULL &&
		databases_exclude_hash == NULL) {
		return(FALSE);
	}

	const char* db_name = strrchr(path, OS_PATH_SEPARATOR);
	if (db_name == NULL) {
		db_name = path;
	} else {
		++db_name;
	}

	return check_if_skip_database(db_name) == DATABASE_SKIP;
}

/************************************************************************
Checks if a table specified as a name in the form "database/name" (InnoDB 5.6)
or "./database/name.ibd" (InnoDB 5.5-) should be skipped from backup based on
the --tables or --tables-file options.

@return TRUE if the table should be skipped. */
my_bool
check_if_skip_table(
/******************/
	const char*	name)	/*!< in: path to the table */
{
	char buf[FN_REFLEN];
	const char *dbname, *tbname;
	const char *ptr;
	char *eptr;

	if (regex_exclude_list.empty() &&
		regex_include_list.empty() &&
		tables_include_hash == NULL &&
		tables_exclude_hash == NULL &&
		databases_include_hash == NULL &&
		databases_exclude_hash == NULL) {
		return(FALSE);
	}

	dbname = NULL;
	tbname = name;
	while ((ptr = strchr(tbname, '/')) != NULL) {
		dbname = tbname;
		tbname = ptr + 1;
	}

	if (dbname == NULL) {
		return(FALSE);
	}

	strncpy(buf, dbname, FN_REFLEN);
	buf[tbname - 1 - dbname] = 0;

	const skip_database_check_result skip_database =
			check_if_skip_database(buf);
	if (skip_database == DATABASE_SKIP) {
		return (TRUE);
	}

	buf[FN_REFLEN - 1] = '\0';
	buf[tbname - 1 - dbname] = '.';

	/* Check if there's a suffix in the table name. If so, truncate it. We
	rely on the fact that a dot cannot be a part of a table name (it is
	encoded by the server with the @NNNN syntax). */
	if ((eptr = strchr(&buf[tbname - dbname], '.')) != NULL) {

		*eptr = '\0';
	}

	/* For partitioned tables first try to match against the regexp
	without truncating the #P#... suffix so we can backup individual
	partitions with regexps like '^test[.]t#P#p5' */
	if (check_if_table_matches_filters(buf, regex_exclude_list,
					   tables_exclude_hash)) {
		return(TRUE);
	}
	if (check_if_table_matches_filters(buf, regex_include_list,
					   tables_include_hash)) {
		return(FALSE);
	}
	if ((eptr = strstr(buf, "#P#")) != NULL) {
		*eptr = 0;

		if (check_if_table_matches_filters(buf, regex_exclude_list,
						   tables_exclude_hash)) {
			return (TRUE);
		}
		if (check_if_table_matches_filters(buf, regex_include_list,
						   tables_include_hash)) {
			return(FALSE);
		}
	}

	if (skip_database == DATABASE_DONT_SKIP_UNLESS_EXPLICITLY_EXCLUDED) {
		/* Database is in include-list, and qualified name wasn't
		   found in any of exclusion filters.*/
		return (FALSE);
	}

	if (skip_database == DATABASE_SKIP_SOME_TABLES ||
		!regex_include_list.empty() ||
		tables_include_hash) {

		/* Include lists are present, but qualified name
		   failed to match any.*/
		return(TRUE);
	}

	return(FALSE);
}

const char*
xb_get_copy_action(const char *dflt)
{
	const char *action;

	if (xtrabackup_stream) {
		if (xtrabackup_compress) {
			action = "Compressing and streaming";
		} else {
			action = "Streaming";
		}
	} else {
		if (xtrabackup_compress) {
			action = "Compressing";
		} else {
			action = dflt;
		}
	}

	return(action);
}

/* TODO: We may tune the behavior (e.g. by fil_aio)*/

static
my_bool
xtrabackup_copy_datafile(fil_node_t* node, uint thread_n)
{
	char			 dst_name[FN_REFLEN];
	ds_file_t		*dstfile = NULL;
	xb_fil_cur_t		 cursor;
	xb_fil_cur_result_t	 res;
	xb_write_filt_t		*write_filter = NULL;
	xb_write_filt_ctxt_t	 write_filt_ctxt;
	const char		*action;
	xb_read_filt_t		*read_filter;
	my_bool			rc = FALSE;

	/* Get the name and the path for the tablespace. node->name always
	contains the path (which may be absolute for remote tablespaces in
	5.6+). space->name contains the tablespace name in the form
	"./database/table.ibd" (in 5.5-) or "database/table" (in 5.6+). For a
	multi-node shared tablespace, space->name contains the name of the first
	node, but that's irrelevant, since we only need node_name to match them
	against filters, and the shared tablespace is always copied regardless
	of the filters value. */

	const char* const node_name = node->space->name;
	const char* const node_path = node->name;

	if (fil_is_user_tablespace_id(node->space->id)
	    && check_if_skip_table(node_name)) {
		msg("[%02u] Skipping %s.\n", thread_n, node_name);
		return(FALSE);
	}

	if (!changed_page_bitmap) {
		read_filter = &rf_pass_through;
	}
	else {
		read_filter = &rf_bitmap;
	}
	res = xb_fil_cur_open(&cursor, read_filter, node, thread_n);
	if (res == XB_FIL_CUR_SKIP) {
		goto skip;
	} else if (res == XB_FIL_CUR_ERROR) {
		goto error;
	}

	strncpy(dst_name, cursor.rel_path, sizeof(dst_name));

	/* Setup the page write filter */
	if (xtrabackup_incremental) {
		write_filter = &wf_incremental;
	} else {
		write_filter = &wf_write_through;
	}

	memset(&write_filt_ctxt, 0, sizeof(xb_write_filt_ctxt_t));
	ut_a(write_filter->process != NULL);

	if (write_filter->init != NULL &&
	    !write_filter->init(&write_filt_ctxt, dst_name, &cursor)) {
		msg("[%02u] mariabackup: error: "
		    "failed to initialize page write filter.\n", thread_n);
		goto error;
	}

	dstfile = ds_open(ds_data, dst_name, &cursor.statinfo);
	if (dstfile == NULL) {
		msg("[%02u] mariabackup: error: "
		    "cannot open the destination stream for %s\n",
		    thread_n, dst_name);
		goto error;
	}

	action = xb_get_copy_action();

	if (xtrabackup_stream) {
		msg_ts("[%02u] %s %s\n", thread_n, action, node_path);
	} else {
		msg_ts("[%02u] %s %s to %s\n", thread_n, action,
		       node_path, dstfile->path);
	}

	/* The main copy loop */
	while ((res = xb_fil_cur_read(&cursor)) == XB_FIL_CUR_SUCCESS) {
		if (!write_filter->process(&write_filt_ctxt, dstfile)) {
			goto error;
		}
	}

	if (res == XB_FIL_CUR_ERROR) {
		goto error;
	}

	if (write_filter->finalize
	    && !write_filter->finalize(&write_filt_ctxt, dstfile)) {
		goto error;
	}

	/* close */
	msg_ts("[%02u]        ...done\n", thread_n);
	xb_fil_cur_close(&cursor);
	if (ds_close(dstfile)) {
		rc = TRUE;
	}
	if (write_filter && write_filter->deinit) {
		write_filter->deinit(&write_filt_ctxt);
	}
	return(rc);

error:
	xb_fil_cur_close(&cursor);
	if (dstfile != NULL) {
		ds_close(dstfile);
	}
	if (write_filter && write_filter->deinit) {
		write_filter->deinit(&write_filt_ctxt);;
	}
	msg("[%02u] mariabackup: Error: "
	    "xtrabackup_copy_datafile() failed.\n", thread_n);
	return(TRUE); /*ERROR*/

skip:

	if (dstfile != NULL) {
		ds_close(dstfile);
	}
	if (write_filter && write_filter->deinit) {
		write_filter->deinit(&write_filt_ctxt);
	}
	msg("[%02u] mariabackup: Warning: We assume the "
	    "table was dropped during xtrabackup execution "
	    "and ignore the file.\n", thread_n);
	msg("[%02u] mariabackup: Warning: skipping tablespace %s.\n",
	    thread_n, node_name);
	return(FALSE);
}

/** How to copy a redo log segment in backup */
enum copy_logfile {
	/** Initial copying: copy at least one block */
	COPY_FIRST,
	/** Tracking while copying data files */
	COPY_ONLINE,
	/** Final copying: copy until the end of the log */
	COPY_LAST
};

/** Copy redo log blocks to the data sink.
@param[in]	copy		how to copy the log
@param[in]	start_lsn	buffer start LSN
@param[in]	end_lsn		buffer end LSN
@return	last scanned LSN (equals to last copied LSN if copy=COPY_LAST)
@retval	0	on failure */
static
lsn_t
xtrabackup_copy_log(copy_logfile copy, lsn_t start_lsn, lsn_t end_lsn)
{
	lsn_t	scanned_lsn	= start_lsn;
	const byte* log_block = log_sys->buf;
	bool more_data = false;

	for (ulint scanned_checkpoint = 0;
	     scanned_lsn < end_lsn;
	     log_block += OS_FILE_LOG_BLOCK_SIZE) {
		ulint checkpoint = log_block_get_checkpoint_no(log_block);

		if (scanned_checkpoint > checkpoint
		    && scanned_checkpoint - checkpoint >= 0x80000000UL) {
			/* Garbage from a log buffer flush which was made
			before the most recent database recovery */
			break;
		}

		scanned_checkpoint = checkpoint;

		ulint	data_len = log_block_get_data_len(log_block);

		more_data = recv_sys_add_to_parsing_buf(
				log_block,
				scanned_lsn + data_len);

		recv_sys->scanned_lsn = scanned_lsn + data_len;

		if (data_len == OS_FILE_LOG_BLOCK_SIZE) {
			/* We got a full log block. */
			scanned_lsn += data_len;
		} else if (data_len
			   >= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE
			   || data_len <= LOG_BLOCK_HDR_SIZE) {
			/* We got a garbage block (abrupt end of the log). */
			break;
		} else {
			/* We got a partial block (abrupt end of the log). */
			scanned_lsn += data_len;
			break;
		}
	}

	if (more_data && recv_parse_log_recs(0, STORE_NO, false)) {

		msg("mariabackup: copying the log failed \n");

		return(0);
	}

	recv_sys_justify_left_parsing_buf();

	log_sys->log.scanned_lsn = scanned_lsn;

	end_lsn = copy == COPY_LAST
		? ut_uint64_align_up(scanned_lsn, OS_FILE_LOG_BLOCK_SIZE)
		: scanned_lsn & ~lsn_t(OS_FILE_LOG_BLOCK_SIZE - 1);

	if (ulint write_size = ulint(end_lsn - start_lsn)) {
		if (srv_encrypt_log) {
			log_crypt(log_sys->buf, start_lsn, write_size);
		}

		if (ds_write(dst_log_file, log_sys->buf, write_size)) {
			msg("mariabackup: Error: "
			    "write to logfile failed\n");
			return(0);
		}
	}

	return(scanned_lsn);
}

/** Copy redo log until the current end of the log is reached
@param copy	how to copy the log
@return	whether the operation failed */
static bool
xtrabackup_copy_logfile(copy_logfile copy)
{
	ut_a(dst_log_file != NULL);
	ut_ad(recv_sys != NULL);

	lsn_t	start_lsn;
	lsn_t	end_lsn;

	recv_sys->parse_start_lsn = log_copy_scanned_lsn;
	recv_sys->scanned_lsn = log_copy_scanned_lsn;

	start_lsn = ut_uint64_align_down(log_copy_scanned_lsn,
					 OS_FILE_LOG_BLOCK_SIZE);
	/* When copying the first or last part of the log, retry a few
	times to ensure that all log up to the last checkpoint will be
	read. */
	do {
		end_lsn = start_lsn + RECV_SCAN_SIZE;

		xtrabackup_io_throttling();

		log_mutex_enter();

		lsn_t lsn= start_lsn;
		for(int retries= 0; retries < 100; retries++) {
			if (log_group_read_log_seg(log_sys->buf, &log_sys->log,
				&lsn, end_lsn)){
				break;
			}
			msg("Retrying read of a redo log block");
			my_sleep(1000);
		}

		start_lsn = xtrabackup_copy_log(copy, start_lsn, lsn);

		log_mutex_exit();

		if (!start_lsn) {
			ds_close(dst_log_file);
			dst_log_file = NULL;
			msg("mariabackup: Error: xtrabackup_copy_logfile()"
			    " failed.\n");
			return(true);
		}
	} while (start_lsn == end_lsn);

	ut_ad(start_lsn == log_sys->log.scanned_lsn);

	msg_ts(">> log scanned up to (" LSN_PF ")\n", start_lsn);

	/* update global variable*/
	log_copy_scanned_lsn = start_lsn;

	debug_sync_point("xtrabackup_copy_logfile_pause");
	return(false);
}

static os_thread_ret_t DECLARE_THREAD(log_copying_thread)(void*)
{
	/*
	  Initialize mysys thread-specific memory so we can
	  use mysys functions in this thread.
	*/
	my_thread_init();

	do {
		os_event_reset(log_copying_stop);
		os_event_wait_time_low(log_copying_stop,
				       xtrabackup_log_copy_interval * 1000ULL,
				       0);
	} while (log_copying && xtrabackup_copy_logfile(COPY_ONLINE));

	log_copying_running = false;
	my_thread_end();
	os_thread_exit();

	return(0);
}

/* io throttle watching (rough) */
static os_thread_ret_t DECLARE_THREAD(io_watching_thread)(void*)
{
	/* currently, for --backup only */
	ut_a(xtrabackup_backup);

	while (log_copying) {
		os_thread_sleep(1000000); /*1 sec*/
		io_ticket = xtrabackup_throttle;
		os_event_set(wait_throttle);
	}

	/* stop io throttle */
	xtrabackup_throttle = 0;
	os_event_set(wait_throttle);

	io_watching_thread_running = false;

	os_thread_exit();

	return(0);
}

/**************************************************************************
Datafiles copying thread.*/
static
os_thread_ret_t
DECLARE_THREAD(data_copy_thread_func)(
/*==================*/
	void *arg) /* thread context */
{
	data_thread_ctxt_t	*ctxt = (data_thread_ctxt_t *) arg;
	uint			num = ctxt->num;
	fil_node_t*		node;

	/*
	  Initialize mysys thread-specific memory so we can
	  use mysys functions in this thread.
	*/
	my_thread_init();

	debug_sync_point("data_copy_thread_func");

	while ((node = datafiles_iter_next(ctxt->it)) != NULL) {

		/* copy the datafile */
		if(xtrabackup_copy_datafile(node, num)) {
			msg("[%02u] mariabackup: Error: "
			    "failed to copy datafile.\n", num);
			exit(EXIT_FAILURE);
		}
	}

	pthread_mutex_lock(&ctxt->count_mutex);
	(*ctxt->count)--;
	pthread_mutex_unlock(&ctxt->count_mutex);

	my_thread_end();
	os_thread_exit();
	OS_THREAD_DUMMY_RETURN;
}

/************************************************************************
Initialize the appropriate datasink(s). Both local backups and streaming in the
'xbstream' format allow parallel writes so we can write directly.

Otherwise (i.e. when streaming in the 'tar' format) we need 2 separate datasinks
for the data stream (and don't allow parallel data copying) and for metainfo
files (including ib_logfile0). The second datasink writes to temporary
files first, and then streams them in a serialized way when closed. */
static void
xtrabackup_init_datasinks(void)
{
	/* Start building out the pipelines from the terminus back */
	if (xtrabackup_stream) {
		/* All streaming goes to stdout */
		ds_data = ds_meta = ds_redo = ds_create(xtrabackup_target_dir,
						        DS_TYPE_STDOUT);
	} else {
		/* Local filesystem */
		ds_data = ds_meta = ds_redo = ds_create(xtrabackup_target_dir,
						        DS_TYPE_LOCAL);
	}

	/* Track it for destruction */
	xtrabackup_add_datasink(ds_data);

	/* Stream formatting */
	if (xtrabackup_stream) {
		ds_ctxt_t	*ds;

	 ut_a(xtrabackup_stream_fmt == XB_STREAM_FMT_XBSTREAM);
	 ds = ds_create(xtrabackup_target_dir, DS_TYPE_XBSTREAM);

		xtrabackup_add_datasink(ds);

		ds_set_pipe(ds, ds_data);
		ds_data = ds;


		ds_redo = ds_meta = ds_data;
	}

	/* Compression for ds_data and ds_redo */
	if (xtrabackup_compress) {
		ds_ctxt_t	*ds;

		/* Use a 1 MB buffer for compressed output stream */
		ds = ds_create(xtrabackup_target_dir, DS_TYPE_BUFFER);
		ds_buffer_set_size(ds, 1024 * 1024);
		xtrabackup_add_datasink(ds);
		ds_set_pipe(ds, ds_data);
		if (ds_data != ds_redo) {
			ds_data = ds;
			ds = ds_create(xtrabackup_target_dir, DS_TYPE_BUFFER);
			ds_buffer_set_size(ds, 1024 * 1024);
			xtrabackup_add_datasink(ds);
			ds_set_pipe(ds, ds_redo);
			ds_redo = ds;
		} else {
			ds_redo = ds_data = ds;
		}

		ds = ds_create(xtrabackup_target_dir, DS_TYPE_COMPRESS);
		xtrabackup_add_datasink(ds);
		ds_set_pipe(ds, ds_data);
		if (ds_data != ds_redo) {
			ds_data = ds;
			ds = ds_create(xtrabackup_target_dir, DS_TYPE_COMPRESS);
			xtrabackup_add_datasink(ds);
			ds_set_pipe(ds, ds_redo);
			ds_redo = ds;
		} else {
			ds_redo = ds_data = ds;
		}
	}
}

/************************************************************************
Destroy datasinks.

Destruction is done in the specific order to not violate their order in the
pipeline so that each datasink is able to flush data down the pipeline. */
static void xtrabackup_destroy_datasinks(void)
{
	for (uint i = actual_datasinks; i > 0; i--) {
		ds_destroy(datasinks[i-1]);
		datasinks[i-1] = NULL;
	}
	ds_data = NULL;
	ds_meta = NULL;
	ds_redo = NULL;
}

#define SRV_MAX_N_PENDING_SYNC_IOS	100

/** Initialize the tablespace cache subsystem. */
static
void
xb_fil_io_init()
{
	fil_system.create(srv_file_per_table ? 50000 : 5000);
}

static
Datafile*
xb_new_datafile(const char *name, bool is_remote)
{
	if (is_remote) {
		RemoteDatafile *remote_file = new RemoteDatafile();
		remote_file->set_name(name);
		return(remote_file);
	} else {
		Datafile *file = new Datafile();
		file->set_name(name);
		file->make_filepath(".", name, IBD);
		return(file);
	}
}


static
void
xb_load_single_table_tablespace(
	const char *dirname,
	const char *filname,
	bool is_remote)
{
	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA);
	/* Ignore .isl files on XtraBackup recovery. All tablespaces must be
	local. */
	if (is_remote && srv_operation == SRV_OPERATION_RESTORE_DELTA) {
		return;
	}
	if (check_if_skip_table(filname)) {
		return;
	}

	/* The name ends in .ibd or .isl;
	try opening the file */
	char*	name;
	size_t	dirlen		= dirname == NULL ? 0 : strlen(dirname);
	size_t	namelen		= strlen(filname);
	ulint	pathlen		= dirname == NULL ? namelen + 1: dirlen + namelen + 2;
	lsn_t	flush_lsn;
	dberr_t	err;
	fil_space_t	*space;

	name = static_cast<char*>(ut_malloc_nokey(pathlen));

	if (dirname != NULL) {
		snprintf(name, pathlen, "%s/%s", dirname, filname);
		name[pathlen - 5] = 0;
	} else {
		snprintf(name, pathlen, "%s", filname);
		name[pathlen - 5] = 0;
	}

	Datafile *file = xb_new_datafile(name, is_remote);

	if (file->open_read_only(true) != DB_SUCCESS) {
		ut_free(name);
		exit(EXIT_FAILURE);
	}

	err = file->validate_first_page(&flush_lsn);

	if (err == DB_SUCCESS && file->space_id() != SRV_TMP_SPACE_ID) {
		os_offset_t	node_size = os_file_get_size(file->handle());
		os_offset_t	n_pages;

		ut_a(node_size != (os_offset_t) -1);

		n_pages = node_size / page_size_t(file->flags()).physical();

		space = fil_space_create(
			name, file->space_id(), file->flags(),
			FIL_TYPE_TABLESPACE, NULL/* TODO: crypt_data */);

		ut_a(space != NULL);

		if (!fil_node_create(file->filepath(), ulint(n_pages), space,
				     false, false)) {
			ut_error;
		}

		/* by opening the tablespace we forcing node and space objects
		in the cache to be populated with fields from space header */
		space->open();

		if (srv_operation == SRV_OPERATION_RESTORE_DELTA
		    || xb_close_files) {
			space->close();
		}
	}

	ut_free(name);

	delete file;

	if (err != DB_SUCCESS && err != DB_CORRUPTION && xtrabackup_backup) {
		/* allow corrupted first page for xtrabackup, it could be just
		zero-filled page, which we restore from redo log later */
		exit(EXIT_FAILURE);
	}
}

/** Scan the database directories under the MySQL datadir, looking for
.ibd files and determining the space id in each of them.
@return	DB_SUCCESS or error number */

static dberr_t enumerate_ibd_files(process_single_tablespace_func_t callback)
{
	int		ret;
	char*		dbpath		= NULL;
	ulint		dbpath_len	= 100;
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	dberr_t		err		= DB_SUCCESS;
	size_t len;

	/* The datadir of MySQL is always the default directory of mysqld */

	dir = os_file_opendir(fil_path_to_mysql_datadir, true);

	if (dir == NULL) {

		return(DB_ERROR);
	}

	dbpath = static_cast<char*>(ut_malloc_nokey(dbpath_len));

	/* Scan all directories under the datadir. They are the database
	directories of MySQL. */

	ret = fil_file_readdir_next_file(&err, fil_path_to_mysql_datadir, dir,
					 &dbinfo);
	while (ret == 0) {

		/* General tablespaces are always at the first level of the
		data home dir */
		if (dbinfo.type == OS_FILE_TYPE_FILE) {
			bool is_isl = ends_with(dbinfo.name, ".isl");
			bool is_ibd = !is_isl && ends_with(dbinfo.name,".ibd");

			if (is_isl || is_ibd) {
				(*callback)(NULL, dbinfo.name, is_isl);
			}
		}

		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

			goto next_datadir_item;
		}

		/* We found a symlink or a directory; try opening it to see
		if a symlink is a directory */

		len = strlen(fil_path_to_mysql_datadir)
			+ strlen (dbinfo.name) + 2;
		if (len > dbpath_len) {
			dbpath_len = len;

			if (dbpath) {
				ut_free(dbpath);
			}

			dbpath = static_cast<char*>(ut_malloc_nokey(dbpath_len));
		}
		snprintf(dbpath, dbpath_len,
			 "%s/%s", fil_path_to_mysql_datadir, dbinfo.name);
		os_normalize_path(dbpath);

		if (check_if_skip_database_by_path(dbpath)) {
			fprintf(stderr, "Skipping db: %s\n", dbpath);
			goto next_datadir_item;
		}

		/* We want wrong directory permissions to be a fatal error for
		XtraBackup. */
		dbdir = os_file_opendir(dbpath, true);

		if (dbdir != NULL) {

			/* We found a database directory; loop through it,
			looking for possible .ibd files in it */

			for (ret = fil_file_readdir_next_file(&err, dbpath,
							      dbdir,
							      &fileinfo);
			     ret == 0;
			     ret = fil_file_readdir_next_file(&err, dbpath,
							      dbdir,
							      &fileinfo)) {
				if (fileinfo.type == OS_FILE_TYPE_DIR) {
					continue;
				}

				/* We found a symlink or a file */
				if (strlen(fileinfo.name) > 4) {
					bool is_isl= false;
					if (ends_with(fileinfo.name, ".ibd") || ((is_isl = ends_with(fileinfo.name, ".isl"))))
						(*callback)(dbinfo.name, fileinfo.name, is_isl);
				}
			}

			if (0 != os_file_closedir(dbdir)) {
				fprintf(stderr, "InnoDB: Warning: could not"
				 " close database directory %s\n",
					dbpath);

				err = DB_ERROR;
			}

		} else {

			err = DB_ERROR;
			break;

		}

next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						 fil_path_to_mysql_datadir,
						 dir, &dbinfo);
	}

	ut_free(dbpath);

	if (0 != os_file_closedir(dir)) {
		fprintf(stderr,
			"InnoDB: Error: could not close MySQL datadir\n");

		return(DB_ERROR);
	}

	return(err);
}

/** Assign srv_undo_space_id_start variable if there are undo tablespace present.
Read the TRX_SYS page from ibdata1 file and get the minimum space id from
the first slot rollback segments of TRX_SYS_PAGE_NO.
@retval DB_ERROR if file open or page read failed.
@retval DB_SUCCESS if srv_undo_space_id assigned successfully. */
static dberr_t xb_assign_undo_space_start()
{
	ulint		dirnamelen;
	char		name[1000];
	pfs_os_file_t	file;
	byte*		buf;
	byte*		page;
	bool		ret;
	dberr_t		error = DB_SUCCESS;
	ulint		space, page_no __attribute__((unused));

	if (srv_undo_tablespaces == 0) {
		return error;
	}

	os_normalize_path(srv_data_home);
	dirnamelen = strlen(srv_data_home);
	memcpy(name, srv_data_home, dirnamelen);

	if (dirnamelen && name[dirnamelen - 1] != OS_PATH_SEPARATOR) {
		name[dirnamelen++] = OS_PATH_SEPARATOR;
	}

	snprintf(name + dirnamelen, strlen(name) + strlen("ibdata1"),
		 "%s", "ibdata1");

	file = os_file_create(0, name, OS_FILE_OPEN,
			      OS_FILE_NORMAL, OS_DATA_FILE, true, &ret);

	if (!ret) {
		msg("mariabackup: Error in opening %s\n", name);
		return DB_ERROR;
	}

	buf = static_cast<byte*>(ut_malloc_nokey(2 * UNIV_PAGE_SIZE));
	page = static_cast<byte*>(ut_align(buf, UNIV_PAGE_SIZE));

retry:
	if (!os_file_read(IORequestRead, file, page, TRX_SYS_PAGE_NO * UNIV_PAGE_SIZE,
			  UNIV_PAGE_SIZE)) {
		msg("mariabackup: Reading TRX_SYS page failed.\n");
		error = DB_ERROR;
		goto func_exit;
	}

	/* TRX_SYS page can't be compressed or encrypted. */
	if (buf_page_is_corrupted(false, page, univ_page_size)) {
		goto retry;
	}

	/* 0th slot always points to system tablespace.
	1st slot should point to first undotablespace which is minimum. */

	page_no = mach_read_ulint(TRX_SYS + TRX_SYS_RSEGS
				  + TRX_SYS_RSEG_SLOT_SIZE
				  + TRX_SYS_RSEG_PAGE_NO + page, MLOG_4BYTES);
	ut_ad(page_no != FIL_NULL);

	space = mach_read_ulint(TRX_SYS + TRX_SYS_RSEGS
				+ TRX_SYS_RSEG_SLOT_SIZE
				+ TRX_SYS_RSEG_SPACE + page, MLOG_4BYTES);

	srv_undo_space_id_start = space;

func_exit:
	ut_free(buf);
	ret = os_file_close(file);
	ut_a(ret);

	return error;
}

/****************************************************************************
Populates the tablespace memory cache by scanning for and opening data files.
@returns DB_SUCCESS or error code.*/
static
dberr_t
xb_load_tablespaces()
{
	bool	create_new_db;
	dberr_t	err;
	ulint   sum_of_new_sizes;
        lsn_t	flush_lsn;

	ut_ad(srv_operation == SRV_OPERATION_BACKUP
	      || srv_operation == SRV_OPERATION_RESTORE_DELTA);

	err = srv_sys_space.check_file_spec(&create_new_db, 0);

	/* create_new_db must not be true. */
	if (err != DB_SUCCESS || create_new_db) {
		msg("mariabackup: could not find data files at the "
		    "specified datadir\n");
		return(DB_ERROR);
	}

	err = srv_sys_space.open_or_create(false, false, &sum_of_new_sizes,
					   &flush_lsn);

	if (err != DB_SUCCESS) {
		msg("mariabackup: Could not open data files.\n");
		return(err);
	}

	/* Add separate undo tablespaces to fil_system */

	err = xb_assign_undo_space_start();

	if (err != DB_SUCCESS) {
		return err;
	}

	err = srv_undo_tablespaces_init(false);

	if (err != DB_SUCCESS) {
		return(err);
	}

	/* It is important to call xb_load_single_table_tablespaces() after
	srv_undo_tablespaces_init(), because fil_is_user_tablespace_id() *
	relies on srv_undo_tablespaces_open to be properly initialized */

	msg("mariabackup: Generating a list of tablespaces\n");

	err = enumerate_ibd_files(xb_load_single_table_tablespace);
	if (err != DB_SUCCESS) {
		return(err);
	}

	debug_sync_point("xtrabackup_load_tablespaces_pause");

	return(DB_SUCCESS);
}

/************************************************************************
Initialize the tablespace memory cache and populate it by scanning for and
opening data files.
@returns DB_SUCCESS or error code.*/
static
dberr_t
xb_data_files_init()
{
	xb_fil_io_init();

	return(xb_load_tablespaces());
}

/************************************************************************
Destroy the tablespace memory cache. */
static
void
xb_data_files_close()
{
	ut_ad(!os_thread_count);
	fil_close_all_files();
	if (buf_dblwr) {
		buf_dblwr_free();
	}
}

/***********************************************************************
Allocate and initialize the entry for databases and tables filtering
hash tables. If memory allocation is not successful, terminate program.
@return pointer to the created entry.  */
static
xb_filter_entry_t *
xb_new_filter_entry(
/*================*/
	const char*	name)	/*!< in: name of table/database */
{
	xb_filter_entry_t	*entry;
	ulint namelen = strlen(name);

	ut_a(namelen <= NAME_LEN * 2 + 1);

	entry = static_cast<xb_filter_entry_t *>
		(malloc(sizeof(xb_filter_entry_t) + namelen + 1));
	memset(entry, '\0', sizeof(xb_filter_entry_t) + namelen + 1);
	entry->name = ((char*)entry) + sizeof(xb_filter_entry_t);
	strcpy(entry->name, name);
	entry->has_tables = FALSE;

	return entry;
}

/***********************************************************************
Add entry to hash table. If hash table is NULL, allocate and initialize
new hash table */
static
xb_filter_entry_t*
xb_add_filter(
/*========================*/
	const char*	name,	/*!< in: name of table/database */
	hash_table_t**	hash)	/*!< in/out: hash to insert into */
{
	xb_filter_entry_t*	entry;

	entry = xb_new_filter_entry(name);

	if (UNIV_UNLIKELY(*hash == NULL)) {
		*hash = hash_create(1000);
	}
	HASH_INSERT(xb_filter_entry_t,
		name_hash, *hash,
		ut_fold_string(entry->name),
		entry);

	return entry;
}

/***********************************************************************
Validate name of table or database. If name is invalid, program will
be finished with error code */
static
void
xb_validate_name(
/*=============*/
	const char*	name,	/*!< in: name */
	size_t		len)	/*!< in: length of name */
{
	const char*	p;

	/* perform only basic validation. validate length and
	path symbols */
	if (len > NAME_LEN) {
		msg("mariabackup: name `%s` is too long.\n", name);
		exit(EXIT_FAILURE);
	}
	p = strpbrk(name, "/\\~");
	if (p && (uint) (p - name) < NAME_LEN) {
		msg("mariabackup: name `%s` is not valid.\n", name);
		exit(EXIT_FAILURE);
	}
}

/***********************************************************************
Register new filter entry which can be either database
or table name.  */
static
void
xb_register_filter_entry(
/*=====================*/
	const char*	name,	/*!< in: name */
	hash_table_t** databases_hash,
	hash_table_t** tables_hash
	)
{
	const char*		p;
	size_t			namelen;
	xb_filter_entry_t*	db_entry = NULL;

	namelen = strlen(name);
	if ((p = strchr(name, '.')) != NULL) {
		char dbname[NAME_LEN + 1];

		xb_validate_name(name, p - name);
		xb_validate_name(p + 1, namelen - (p - name));

		strncpy(dbname, name, p - name);
		dbname[p - name] = 0;

		if (*databases_hash) {
			HASH_SEARCH(name_hash, (*databases_hash),
					ut_fold_string(dbname),
					xb_filter_entry_t*,
					db_entry, (void) 0,
					!strcmp(db_entry->name, dbname));
		}
		if (!db_entry) {
			db_entry = xb_add_filter(dbname, databases_hash);
		}
		db_entry->has_tables = TRUE;
		xb_add_filter(name, tables_hash);
	} else {
		xb_validate_name(name, namelen);

		xb_add_filter(name, databases_hash);
	}
}

static
void
xb_register_include_filter_entry(
	const char* name
)
{
	xb_register_filter_entry(name, &databases_include_hash,
				 &tables_include_hash);
}

static
void
xb_register_exclude_filter_entry(
	const char* name
)
{
	xb_register_filter_entry(name, &databases_exclude_hash,
				 &tables_exclude_hash);
}

/***********************************************************************
Register new table for the filter.  */
static
void
xb_register_table(
/*==============*/
	const char* name)	/*!< in: name of table */
{
	if (strchr(name, '.') == NULL) {
		msg("mariabackup: `%s` is not fully qualified name.\n", name);
		exit(EXIT_FAILURE);
	}

	xb_register_include_filter_entry(name);
}

static
void
xb_add_regex_to_list(
	const char* regex,  /*!< in: regex */
	const char* error_context,  /*!< in: context to error message */
	regex_list_t* list) /*! in: list to put new regex to */
{
	char			errbuf[100];
	int			ret;

	regex_t compiled_regex;
	ret = regcomp(&compiled_regex, regex, REG_EXTENDED);

	if (ret != 0) {
		regerror(ret, &compiled_regex, errbuf, sizeof(errbuf));
		msg("mariabackup: error: %s regcomp(%s): %s\n",
			error_context, regex, errbuf);
		exit(EXIT_FAILURE);
	}

	list->push_back(compiled_regex);
}

/***********************************************************************
Register new regex for the include filter.  */
static
void
xb_register_include_regex(
/*==============*/
	const char* regex)	/*!< in: regex */
{
	xb_add_regex_to_list(regex, "tables", &regex_include_list);
}

/***********************************************************************
Register new regex for the exclude filter.  */
static
void
xb_register_exclude_regex(
/*==============*/
	const char* regex)	/*!< in: regex */
{
	xb_add_regex_to_list(regex, "tables-exclude", &regex_exclude_list);
}

typedef void (*insert_entry_func_t)(const char*);

/***********************************************************************
Scan string and load filter entries from it.  */
static
void
xb_load_list_string(
/*================*/
	char* list,			/*!< in: string representing a list */
	const char* delimiters,		/*!< in: delimiters of entries */
	insert_entry_func_t ins)	/*!< in: callback to add entry */
{
	char*	p;
	char*	saveptr;

	p = strtok_r(list, delimiters, &saveptr);
	while (p) {

		ins(p);

		p = strtok_r(NULL, delimiters, &saveptr);
	}
}

/***********************************************************************
Scan file and load filter entries from it.  */
static
void
xb_load_list_file(
/*==============*/
	const char* filename,		/*!< in: name of file */
	insert_entry_func_t ins)	/*!< in: callback to add entry */
{
	char	name_buf[NAME_LEN*2+2];
	FILE*	fp;

	/* read and store the filenames */
	fp = fopen(filename, "r");
	if (!fp) {
		msg("mariabackup: cannot open %s\n",
		    filename);
		exit(EXIT_FAILURE);
	}
	while (fgets(name_buf, sizeof(name_buf), fp) != NULL) {
		char*	p = strchr(name_buf, '\n');
		if (p) {
			*p = '\0';
		} else {
			msg("mariabackup: `%s...` name is too long", name_buf);
			exit(EXIT_FAILURE);
		}

		ins(name_buf);
	}

	fclose(fp);
}


static
void
xb_filters_init()
{
	if (xtrabackup_databases) {
		xb_load_list_string(xtrabackup_databases, " \t",
				    xb_register_include_filter_entry);
	}

	if (xtrabackup_databases_file) {
		xb_load_list_file(xtrabackup_databases_file,
				  xb_register_include_filter_entry);
	}

	if (xtrabackup_databases_exclude) {
		xb_load_list_string(xtrabackup_databases_exclude, " \t",
				    xb_register_exclude_filter_entry);
	}

	if (xtrabackup_tables) {
		xb_load_list_string(xtrabackup_tables, ",",
				    xb_register_include_regex);
	}

	if (xtrabackup_tables_file) {
		xb_load_list_file(xtrabackup_tables_file, xb_register_table);
	}

	if (xtrabackup_tables_exclude) {
		xb_load_list_string(xtrabackup_tables_exclude, ",",
				    xb_register_exclude_regex);
	}
}

static
void
xb_filter_hash_free(hash_table_t* hash)
{
	ulint	i;

	/* free the hash elements */
	for (i = 0; i < hash_get_n_cells(hash); i++) {
		xb_filter_entry_t*	table;

		table = static_cast<xb_filter_entry_t *>
			(HASH_GET_FIRST(hash, i));

		while (table) {
			xb_filter_entry_t*	prev_table = table;

			table = static_cast<xb_filter_entry_t *>
				(HASH_GET_NEXT(name_hash, prev_table));

			HASH_DELETE(xb_filter_entry_t, name_hash, hash,
				ut_fold_string(prev_table->name), prev_table);
			free(prev_table);
		}
	}

	/* free hash */
	hash_table_free(hash);
}

static void xb_regex_list_free(regex_list_t* list)
{
	while (list->size() > 0) {
		xb_regfree(&list->front());
		list->pop_front();
	}
}

/************************************************************************
Destroy table filters for partial backup. */
static
void
xb_filters_free()
{
	xb_regex_list_free(&regex_include_list);
	xb_regex_list_free(&regex_exclude_list);

	if (tables_include_hash) {
		xb_filter_hash_free(tables_include_hash);
	}

	if (tables_exclude_hash) {
		xb_filter_hash_free(tables_exclude_hash);
	}

	if (databases_include_hash) {
		xb_filter_hash_free(databases_include_hash);
	}

	if (databases_exclude_hash) {
		xb_filter_hash_free(databases_exclude_hash);
	}
}

/*********************************************************************//**
Creates or opens the log files and closes them.
@return	DB_SUCCESS or error code */
static
ulint
open_or_create_log_file(
/*====================*/
	fil_space_t* space,
	ibool*	log_file_created,	/*!< out: TRUE if new log file
					created */
	ulint	i)			/*!< in: log file number in group */
{
	char	name[10000];
	ulint	dirnamelen;

	*log_file_created = FALSE;

	os_normalize_path(srv_log_group_home_dir);

	dirnamelen = strlen(srv_log_group_home_dir);
	ut_a(dirnamelen < (sizeof name) - 10 - sizeof "ib_logfile");
	memcpy(name, srv_log_group_home_dir, dirnamelen);

	/* Add a path separator if needed. */
	if (dirnamelen && name[dirnamelen - 1] != OS_PATH_SEPARATOR) {
		name[dirnamelen++] = OS_PATH_SEPARATOR;
	}

	sprintf(name + dirnamelen, "%s%lu", "ib_logfile", (ulong) i);

	ut_a(fil_validate());

	ut_a(fil_node_create(name, ulint(srv_log_file_size >> srv_page_size_shift),
			     space, false, false));

	return(DB_SUCCESS);
}

/*********************************************************************//**
Normalizes init parameter values to use units we use inside InnoDB.
@return	DB_SUCCESS or error code */
static
void
xb_normalize_init_values(void)
/*==========================*/
{
	srv_sys_space.normalize();
	srv_log_buffer_size /= UNIV_PAGE_SIZE;
	srv_lock_table_size = 5 * (srv_buf_pool_size / UNIV_PAGE_SIZE);
}

/***********************************************************************
Set the open files limit. Based on set_max_open_files().

@return the resulting open files limit. May be less or more than the requested
value.  */
static uint
xb_set_max_open_files(
/*==================*/
	uint max_file_limit)	/*!<in: open files limit */
{
#if defined(RLIMIT_NOFILE)
	struct rlimit rlimit;
	uint old_cur;

	if (getrlimit(RLIMIT_NOFILE, &rlimit)) {

		goto end;
	}

	old_cur = (uint) rlimit.rlim_cur;

	if (rlimit.rlim_cur == RLIM_INFINITY) {

		rlimit.rlim_cur = max_file_limit;
	}

	if (rlimit.rlim_cur >= max_file_limit) {

		max_file_limit = rlimit.rlim_cur;
		goto end;
	}

	rlimit.rlim_cur = rlimit.rlim_max = max_file_limit;

	if (setrlimit(RLIMIT_NOFILE, &rlimit)) {

		max_file_limit = old_cur;	/* Use original value */
	} else {

		rlimit.rlim_cur = 0;	/* Safety if next call fails */

		(void) getrlimit(RLIMIT_NOFILE, &rlimit);

		if (rlimit.rlim_cur) {

			/* If call didn't fail */
			max_file_limit = (uint) rlimit.rlim_cur;
		}
	}

end:
	return(max_file_limit);
#else
	return(0);
#endif
}

static void stop_backup_threads()
{
	log_copying = false;

	if (log_copying_stop) {
		os_event_set(log_copying_stop);
		msg("mariabackup: Stopping log copying thread.\n");
		while (log_copying_running) {
			msg(".");
			os_thread_sleep(200000); /*0.2 sec*/
		}
		msg("\n");
		os_event_destroy(log_copying_stop);
	}

	if (wait_throttle) {
		/* wait for io_watching_thread completion */
		while (io_watching_thread_running) {
			os_thread_sleep(1000000);
		}
		os_event_destroy(wait_throttle);
	}
}

/** Implement the core of --backup
@return	whether the operation succeeded */
static
bool
xtrabackup_backup_low()
{
	/* read the latest checkpoint lsn */
	{
		ulint	max_cp_field;

		log_mutex_enter();

		if (recv_find_max_checkpoint(&max_cp_field) == DB_SUCCESS
		    && log_sys->log.format != 0) {
			metadata_to_lsn = mach_read_from_8(
				log_sys->checkpoint_buf + LOG_CHECKPOINT_LSN);
			msg("mariabackup: The latest check point"
			    " (for incremental): '" LSN_PF "'\n",
			    metadata_to_lsn);
		} else {
			metadata_to_lsn = 0;
			msg("mariabackup: Error: recv_find_max_checkpoint() failed.\n");
		}
		log_mutex_exit();
	}

	stop_backup_threads();

	if (!dst_log_file || xtrabackup_copy_logfile(COPY_LAST)) {
		return false;
	}

	if (ds_close(dst_log_file)) {
		dst_log_file = NULL;
		return false;
	}

	dst_log_file = NULL;

	if(!xtrabackup_incremental) {
		strcpy(metadata_type, "full-backuped");
		metadata_from_lsn = 0;
	} else {
		strcpy(metadata_type, "incremental");
		metadata_from_lsn = incremental_lsn;
	}
	metadata_last_lsn = log_copy_scanned_lsn;

	if (!xtrabackup_stream_metadata(ds_meta)) {
		msg("mariabackup: Error: failed to stream metadata.\n");
		return false;
	}
	if (xtrabackup_extra_lsndir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_extra_lsndir,
			XTRABACKUP_METADATA_FILENAME);
		if (!xtrabackup_write_metadata(filename)) {
			msg("mariabackup: Error: failed to write metadata "
			    "to '%s'.\n", filename);
			return false;
		}
		sprintf(filename, "%s/%s", xtrabackup_extra_lsndir,
			XTRABACKUP_INFO);
		if (!write_xtrabackup_info(mysql_connection, filename, false)) {
			msg("mariabackup: Error: failed to write info "
			 "to '%s'.\n", filename);
			return false;
		}
	}

	return true;
}

/** Implement --backup
@return	whether the operation succeeded */
static
bool
xtrabackup_backup_func()
{
	MY_STAT			 stat_info;
	uint			 i;
	uint			 count;
	pthread_mutex_t		 count_mutex;
	data_thread_ctxt_t 	*data_threads;

#ifdef USE_POSIX_FADVISE
	msg("mariabackup: uses posix_fadvise().\n");
#endif

	/* cd to datadir */

	if (my_setwd(mysql_real_data_home,MYF(MY_WME)))
	{
		msg("mariabackup: cannot my_setwd %s\n", mysql_real_data_home);
		return(false);
	}
	msg("mariabackup: cd to %s\n", mysql_real_data_home);

	msg("mariabackup: open files limit requested %u, set to %u\n",
	    (uint) xb_open_files_limit,
	    xb_set_max_open_files(xb_open_files_limit));

	mysql_data_home= mysql_data_home_buff;
	mysql_data_home[0]=FN_CURLIB;		// all paths are relative from here
	mysql_data_home[1]=0;

	srv_n_purge_threads = 1;
	srv_read_only_mode = TRUE;

	srv_operation = SRV_OPERATION_BACKUP;

	if (xb_close_files)
		msg("mariabackup: warning: close-files specified. Use it "
		    "at your own risk. If there are DDL operations like table DROP TABLE "
		    "or RENAME TABLE during the backup, inconsistent backup will be "
		    "produced.\n");

	/* initialize components */
        if(innodb_init_param()) {
fail:
		stop_backup_threads();
		if (fil_system.is_initialised()) {
			innodb_shutdown();
		}
		return(false);
	}

	xb_normalize_init_values();


	if (srv_file_flush_method_str == NULL) {
		/* These are the default options */
		srv_file_flush_method = SRV_FSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "fsync")) {
		srv_file_flush_method = SRV_FSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DSYNC")) {
		srv_file_flush_method = SRV_O_DSYNC;

	} else if (0 == ut_strcmp(srv_file_flush_method_str, "O_DIRECT")) {
		srv_file_flush_method = SRV_O_DIRECT;
		msg("mariabackup: using O_DIRECT\n");
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "littlesync")) {
		srv_file_flush_method = SRV_LITTLESYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "nosync")) {
		srv_file_flush_method = SRV_NOSYNC;
	} else if (0 == ut_strcmp(srv_file_flush_method_str, "ALL_O_DIRECT")) {
		srv_file_flush_method = SRV_ALL_O_DIRECT_FSYNC;
		msg("mariabackup: using ALL_O_DIRECT\n");
	} else if (0 == ut_strcmp(srv_file_flush_method_str,
				  "O_DIRECT_NO_FSYNC")) {
		srv_file_flush_method = SRV_O_DIRECT_NO_FSYNC;
		msg("mariabackup: using O_DIRECT_NO_FSYNC\n");
	} else {
		msg("mariabackup: Unrecognized value %s for "
		    "innodb_flush_method\n", srv_file_flush_method_str);
		goto fail;
	}

#ifdef _WIN32
  srv_file_flush_method = SRV_ALL_O_DIRECT_FSYNC;
	srv_use_native_aio = TRUE;
#endif

	if (srv_buf_pool_size >= 1000 * 1024 * 1024) {
                                  /* Here we still have srv_pool_size counted
                                  in kilobytes (in 4.0 this was in bytes)
				  srv_boot() converts the value to
                                  pages; if buffer pool is less than 1000 MB,
                                  assume fewer threads. */
                srv_max_n_threads = 50000;

	} else if (srv_buf_pool_size >= 8 * 1024 * 1024) {

                srv_max_n_threads = 10000;
        } else {
		srv_max_n_threads = 1000;       /* saves several MB of memory,
                                                especially in 64-bit
                                                computers */
        }

	sync_check_init();
	ut_d(sync_check_enable());
	/* Reset the system variables in the recovery module. */
	recv_sys_var_init();
	trx_pool_init();

	ut_crc32_init();
	crc_init();
	recv_sys_init();

#ifdef WITH_INNODB_DISALLOW_WRITES
	srv_allow_writes_event = os_event_create(0);
	os_event_set(srv_allow_writes_event);
#endif

	xb_filters_init();

	{
	ibool	log_file_created;
	ibool	log_created	= FALSE;
	ibool	log_opened	= FALSE;
	ulint	err;
	ulint	i;

	xb_fil_io_init();
	srv_n_file_io_threads = srv_n_read_io_threads;

	os_aio_init(srv_n_read_io_threads, srv_n_write_io_threads,
		    SRV_MAX_N_PENDING_SYNC_IOS);

	log_sys_init();
	log_init(srv_n_log_files);
	fil_space_t*	space = fil_space_create(
		"innodb_redo_log", SRV_LOG_SPACE_FIRST_ID, 0,
		FIL_TYPE_LOG, NULL);

	for (i = 0; i < srv_n_log_files; i++) {
		err = open_or_create_log_file(space, &log_file_created, i);
		if (err != DB_SUCCESS) {
			goto fail;
		}

		if (log_file_created) {
			log_created = TRUE;
		} else {
			log_opened = TRUE;
		}
		if ((log_opened && log_created)) {
			msg(
	"mariabackup: Error: all log files must be created at the same time.\n"
	"mariabackup: All log files must be created also in database creation.\n"
	"mariabackup: If you want bigger or smaller log files, shut down the\n"
	"mariabackup: database and make sure there were no errors in shutdown.\n"
	"mariabackup: Then delete the existing log files. Edit the .cnf file\n"
	"mariabackup: and start the database again.\n");

			goto fail;
		}
	}

	/* log_file_created must not be TRUE, if online */
	if (log_file_created) {
		msg("mariabackup: Something wrong with source files...\n");
		goto fail;
	}

	}

	/* create extra LSN dir if it does not exist. */
	if (xtrabackup_extra_lsndir
		&&!my_stat(xtrabackup_extra_lsndir,&stat_info,MYF(0))
		&& (my_mkdir(xtrabackup_extra_lsndir,0777,MYF(0)) < 0)) {
		msg("mariabackup: Error: cannot mkdir %d: %s\n",
		    my_errno, xtrabackup_extra_lsndir);
		goto fail;
	}

	/* create target dir if not exist */
	if (!xtrabackup_stream_str && !my_stat(xtrabackup_target_dir,&stat_info,MYF(0))
		&& (my_mkdir(xtrabackup_target_dir,0777,MYF(0)) < 0)){
		msg("mariabackup: Error: cannot mkdir %d: %s\n",
		    my_errno, xtrabackup_target_dir);
		goto fail;
	}

        {
	/* definition from recv_recovery_from_checkpoint_start() */
	ulint		max_cp_field;

	/* start back ground thread to copy newer log */
	os_thread_id_t log_copying_thread_id;
	datafiles_iter_t *it;

	/* get current checkpoint_lsn */
	/* Look for the latest checkpoint from any of the log groups */

	log_mutex_enter();

	dberr_t err = recv_find_max_checkpoint(&max_cp_field);

	if (err != DB_SUCCESS) {
log_fail:
		log_mutex_exit();
		goto fail;
	}

	if (log_sys->log.format == 0) {
old_format:
		msg("mariabackup: Error: cannot process redo log"
		    " before MariaDB 10.2.2\n");
		log_mutex_exit();
		goto log_fail;
	}

	ut_ad(!((log_sys->log.format ^ LOG_HEADER_FORMAT_CURRENT)
		& ~LOG_HEADER_FORMAT_ENCRYPTED));

	const byte* buf = log_sys->checkpoint_buf;

reread_log_header:
	checkpoint_lsn_start = log_sys->log.lsn;
	checkpoint_no_start = log_sys->next_checkpoint_no;

	err = recv_find_max_checkpoint(&max_cp_field);

	if (err != DB_SUCCESS) {
		goto log_fail;
	}

	if (log_sys->log.format == 0) {
		goto old_format;
	}

	ut_ad(!((log_sys->log.format ^ LOG_HEADER_FORMAT_CURRENT)
		& ~LOG_HEADER_FORMAT_ENCRYPTED));

	log_group_header_read(&log_sys->log, max_cp_field);

	if (checkpoint_no_start != mach_read_from_8(buf + LOG_CHECKPOINT_NO)) {
		goto reread_log_header;
	}

	log_mutex_exit();

	xtrabackup_init_datasinks();

	if (!select_history()) {
		goto fail;
	}

	/* open the log file */
	memset(&stat_info, 0, sizeof(MY_STAT));
	dst_log_file = ds_open(ds_redo, "ib_logfile0", &stat_info);
	if (dst_log_file == NULL) {
		msg("mariabackup: error: failed to open the target stream for "
		    "'ib_logfile0'.\n");
		goto fail;
	}

	/* label it */
	byte MY_ALIGNED(OS_FILE_LOG_BLOCK_SIZE) log_hdr[OS_FILE_LOG_BLOCK_SIZE];
	memset(log_hdr, 0, sizeof log_hdr);
	mach_write_to_4(LOG_HEADER_FORMAT + log_hdr, log_sys->log.format);
	mach_write_to_8(LOG_HEADER_START_LSN + log_hdr, checkpoint_lsn_start);
	strcpy(reinterpret_cast<char*>(LOG_HEADER_CREATOR + log_hdr),
	       "Backup " MYSQL_SERVER_VERSION);
	log_block_set_checksum(log_hdr,
			       log_block_calc_checksum_crc32(log_hdr));

	/* Write the log header. */
	if (ds_write(dst_log_file, log_hdr, sizeof log_hdr)) {
	log_write_fail:
		msg("mariabackup: error: write to logfile failed\n");
		goto fail;
	}
	/* Adjust the checkpoint page. */
	memcpy(log_hdr, buf, OS_FILE_LOG_BLOCK_SIZE);
	mach_write_to_8(log_hdr + LOG_CHECKPOINT_OFFSET,
			(checkpoint_lsn_start & (OS_FILE_LOG_BLOCK_SIZE - 1))
			| LOG_FILE_HDR_SIZE);
	log_block_set_checksum(log_hdr,
			       log_block_calc_checksum_crc32(log_hdr));
	/* Write checkpoint page 1 and two empty log pages before the
	payload. */
	if (ds_write(dst_log_file, log_hdr, OS_FILE_LOG_BLOCK_SIZE)
	    || !memset(log_hdr, 0, sizeof log_hdr)
	    || ds_write(dst_log_file, log_hdr, sizeof log_hdr)
	    || ds_write(dst_log_file, log_hdr, sizeof log_hdr)) {
		goto log_write_fail;
	}

	/* start flag */
	log_copying = TRUE;

	/* start io throttle */
	if(xtrabackup_throttle) {
		os_thread_id_t io_watching_thread_id;

		io_ticket = xtrabackup_throttle;
		wait_throttle = os_event_create(0);
		io_watching_thread_running = true;

		os_thread_create(io_watching_thread, NULL,
				 &io_watching_thread_id);
	}

	/* Populate fil_system with tablespaces to copy */
	err = xb_load_tablespaces();
	if (err != DB_SUCCESS) {
		msg("mariabackup: error: xb_load_tablespaces() failed with"
		    " error %s.\n", ut_strerr(err));
		goto fail;
	}

	/* copy log file by current position */
	log_copy_scanned_lsn = checkpoint_lsn_start;
	recv_sys->recovered_lsn = log_copy_scanned_lsn;

	if (xtrabackup_copy_logfile(COPY_FIRST))
		goto fail;

	log_copying_stop = os_event_create(0);
	log_copying_running = true;
	os_thread_create(log_copying_thread, NULL, &log_copying_thread_id);

	/* FLUSH CHANGED_PAGE_BITMAPS call */
	if (!flush_changed_page_bitmaps()) {
		goto fail;
	}
	debug_sync_point("xtrabackup_suspend_at_start");

	if (xtrabackup_incremental) {
		if (!xtrabackup_incremental_force_scan) {
			changed_page_bitmap = xb_page_bitmap_init();
		}
		if (!changed_page_bitmap) {
			msg("mariabackup: using the full scan for incremental "
			    "backup\n");
		} else if (incremental_lsn != checkpoint_lsn_start) {
			/* Do not print that bitmaps are used when dummy bitmap
			is build for an empty LSN range. */
			msg("mariabackup: using the changed page bitmap\n");
		}
	}

	ut_a(xtrabackup_parallel > 0);

	if (xtrabackup_parallel > 1) {
		msg("mariabackup: Starting %u threads for parallel data "
		    "files transfer\n", xtrabackup_parallel);
	}

	if (opt_lock_ddl_per_table) {
		mdl_lock_all();
	}

	it = datafiles_iter_new();
	if (it == NULL) {
		msg("mariabackup: Error: datafiles_iter_new() failed.\n");
		goto fail;
	}

	/* Create data copying threads */
	data_threads = (data_thread_ctxt_t *)
		malloc(sizeof(data_thread_ctxt_t) * xtrabackup_parallel);
	count = xtrabackup_parallel;
	pthread_mutex_init(&count_mutex, NULL);

	for (i = 0; i < (uint) xtrabackup_parallel; i++) {
		data_threads[i].it = it;
		data_threads[i].num = i+1;
		data_threads[i].count = &count;
		data_threads[i].count_mutex = count_mutex;
		os_thread_create(data_copy_thread_func, data_threads + i,
				 &data_threads[i].id);
	}

	/* Wait for threads to exit */
	while (1) {
		os_thread_sleep(1000000);
		pthread_mutex_lock(&count_mutex);
		bool stop = count == 0;
		pthread_mutex_unlock(&count_mutex);
		if (stop) {
			break;
		}
	}

	pthread_mutex_destroy(&count_mutex);
	free(data_threads);
	datafiles_iter_free(it);

	if (changed_page_bitmap) {
		xb_page_bitmap_deinit(changed_page_bitmap);
	}
	}

	bool ok = backup_start();

	if (ok) {
		ok = xtrabackup_backup_low();

		backup_release();

		DBUG_EXECUTE_IF("check_mdl_lock_works",
			os_event_wait(dbug_alter_thread_done);
			os_event_destroy(dbug_alter_thread_done);
		);

		if (ok) {
			backup_finish();
		}
	}

	if (!ok) {
		goto fail;
	}

	xtrabackup_destroy_datasinks();

	msg("mariabackup: Redo log (from LSN " LSN_PF " to " LSN_PF
	    ") was copied.\n", checkpoint_lsn_start, log_copy_scanned_lsn);
	xb_filters_free();

	xb_data_files_close();

	/* Make sure that the latest checkpoint was included */
	if (metadata_to_lsn > log_copy_scanned_lsn) {
		msg("mariabackup: error: failed to copy enough redo log ("
		    "LSN=" LSN_PF "; checkpoint LSN=" LSN_PF ").\n",
		    log_copy_scanned_lsn, metadata_to_lsn);
		goto fail;
	}

	innodb_shutdown();
	return(true);
}

/* ================= prepare ================= */

/***********************************************************************
Generates path to the meta file path from a given path to an incremental .delta
by replacing trailing ".delta" with ".meta", or returns error if 'delta_path'
does not end with the ".delta" character sequence.
@return TRUE on success, FALSE on error. */
static
ibool
get_meta_path(
	const char	*delta_path,	/* in: path to a .delta file */
	char 		*meta_path)	/* out: path to the corresponding .meta
					file */
{
	size_t		len = strlen(delta_path);

	if (len <= 6 || strcmp(delta_path + len - 6, ".delta")) {
		return FALSE;
	}
	memcpy(meta_path, delta_path, len - 6);
	strcpy(meta_path + len - 6, XB_DELTA_INFO_SUFFIX);

	return TRUE;
}

/****************************************************************//**
Create a new tablespace on disk and return the handle to its opened
file. Code adopted from fil_create_new_single_table_tablespace with
the main difference that only disk file is created without updating
the InnoDB in-memory dictionary data structures.

@return true on success, false on error.  */
static
bool
xb_space_create_file(
/*==================*/
	const char*	path,		/*!<in: path to tablespace */
	ulint		space_id,	/*!<in: space id */
	ulint		flags,		/*!<in: tablespace flags */
	pfs_os_file_t*	file)		/*!<out: file handle */
{
	bool		ret;
	byte*		buf;
	byte*		page;

	*file = os_file_create_simple_no_error_handling(
		0, path, OS_FILE_CREATE, OS_FILE_READ_WRITE, false, &ret);
	if (!ret) {
		msg("mariabackup: cannot create file %s\n", path);
		return ret;
	}

	ret = os_file_set_size(path, *file,
			       FIL_IBD_FILE_INITIAL_SIZE * UNIV_PAGE_SIZE);
	if (!ret) {
		msg("mariabackup: cannot set size for file %s\n", path);
		os_file_close(*file);
		os_file_delete(0, path);
		return ret;
	}

	buf = static_cast<byte *>(malloc(3 * UNIV_PAGE_SIZE));
	/* Align the memory for file i/o if we might have O_DIRECT set */
	page = static_cast<byte *>(ut_align(buf, UNIV_PAGE_SIZE));

	memset(page, '\0', UNIV_PAGE_SIZE);

	fsp_header_init_fields(page, space_id, flags);
	mach_write_to_4(page + FIL_PAGE_ARCH_LOG_NO_OR_SPACE_ID, space_id);

	const page_size_t page_size(flags);

	if (!page_size.is_compressed()) {
		buf_flush_init_for_writing(NULL, page, NULL, 0);

		ret = os_file_write(IORequestWrite, path, *file, page, 0,
				    UNIV_PAGE_SIZE);
	} else {
		page_zip_des_t	page_zip;
		ulint zip_size = page_size.physical();
		page_zip_set_size(&page_zip, zip_size);
		page_zip.data = page + UNIV_PAGE_SIZE;
		fprintf(stderr, "zip_size = " ULINTPF "\n", zip_size);

#ifdef UNIV_DEBUG
		page_zip.m_start =
#endif /* UNIV_DEBUG */
			page_zip.m_end = page_zip.m_nonempty =
			page_zip.n_blobs = 0;

		buf_flush_init_for_writing(NULL, page, &page_zip, 0);

		ret = os_file_write(IORequestWrite, path, *file,
				    page_zip.data, 0, zip_size);
	}

	free(buf);

	if (!ret) {
		msg("mariabackup: could not write the first page to %s\n",
		    path);
		os_file_close(*file);
		os_file_delete(0, path);
		return ret;
	}

	return TRUE;
}

static fil_space_t* fil_space_get_by_name(const char* name)
{
	ut_ad(mutex_own(&fil_system.mutex));
	for (fil_space_t* space = UT_LIST_GET_FIRST(fil_system.space_list);
	     space != NULL;
	     space = UT_LIST_GET_NEXT(space_list, space))
		if (!strcmp(space->name, name)) return space;
	return NULL;
}

/***********************************************************************
Searches for matching tablespace file for given .delta file and space_id
in given directory. When matching tablespace found, renames it to match the
name of .delta file. If there was a tablespace with matching name and
mismatching ID, renames it to xtrabackup_tmp_#ID.ibd. If there was no
matching file, creates a new tablespace.
@return file handle of matched or created file */
static
pfs_os_file_t
xb_delta_open_matching_space(
	const char*	dbname,		/* in: path to destination database dir */
	const char*	name,		/* in: name of delta file (without .delta) */
	const xb_delta_info_t& info,
	char*		real_name,	/* out: full path of destination file */
	size_t		real_name_len,	/* out: buffer size for real_name */
	bool* 		success)	/* out: indicates error. true = success */
{
	char			dest_dir[FN_REFLEN];
	char			dest_space_name[FN_REFLEN];
	fil_space_t*		fil_space;
	pfs_os_file_t		file;
	xb_filter_entry_t*	table;

	ut_a(dbname != NULL ||
	     !fil_is_user_tablespace_id(info.space_id) ||
	     info.space_id == ULINT_UNDEFINED);

	*success = false;

	if (dbname) {
		snprintf(dest_dir, FN_REFLEN, "%s/%s",
			xtrabackup_target_dir, dbname);
		os_normalize_path(dest_dir);

		snprintf(dest_space_name, FN_REFLEN, "%s/%s", dbname, name);
	} else {
		snprintf(dest_dir, FN_REFLEN, "%s", xtrabackup_target_dir);
		os_normalize_path(dest_dir);

		snprintf(dest_space_name, FN_REFLEN, "%s", name);
	}

	snprintf(real_name, real_name_len,
		 "%s/%s",
		 xtrabackup_target_dir, dest_space_name);
	os_normalize_path(real_name);
	/* Truncate ".ibd" */
	dest_space_name[strlen(dest_space_name) - 4] = '\0';

	/* Create the database directory if it doesn't exist yet */
	if (!os_file_create_directory(dest_dir, FALSE)) {
		msg("mariabackup: error: cannot create dir %s\n", dest_dir);
		return file;
	}

	log_mutex_enter();
	if (!fil_is_user_tablespace_id(info.space_id)) {
found:
		/* open the file and return its handle */

		file = os_file_create_simple_no_error_handling(
			0, real_name,
			OS_FILE_OPEN, OS_FILE_READ_WRITE, false, success);

		if (!*success) {
			msg("mariabackup: Cannot open file %s\n", real_name);
		}
exit:
		log_mutex_exit();
		return file;
	}

	/* remember space name for further reference */
	table = static_cast<xb_filter_entry_t *>
		(malloc(sizeof(xb_filter_entry_t) +
			strlen(dest_space_name) + 1));

	table->name = ((char*)table) + sizeof(xb_filter_entry_t);
	strcpy(table->name, dest_space_name);
	HASH_INSERT(xb_filter_entry_t, name_hash, inc_dir_tables_hash,
			ut_fold_string(table->name), table);

	mutex_enter(&fil_system.mutex);
	fil_space = fil_space_get_by_name(dest_space_name);
	mutex_exit(&fil_system.mutex);

	if (fil_space != NULL) {
		if (fil_space->id == info.space_id
		    || info.space_id == ULINT_UNDEFINED) {
			/* we found matching space */
			goto found;
		} else {

			char	tmpname[FN_REFLEN];

			snprintf(tmpname, FN_REFLEN, "%s/xtrabackup_tmp_#" ULINTPF,
				 dbname, fil_space->id);

			msg("mariabackup: Renaming %s to %s.ibd\n",
				fil_space->name, tmpname);

			if (fil_space->rename(tmpname, NULL, false)
			    != DB_SUCCESS) {
				msg("mariabackup: Cannot rename %s to %s\n",
					fil_space->name, tmpname);
				goto exit;
			}
		}
	}

	if (info.space_id == ULINT_UNDEFINED)
	{
		msg("mariabackup: Error: Cannot handle DDL operation on tablespace "
		    "%s\n", dest_space_name);
		exit(EXIT_FAILURE);
	}
	mutex_enter(&fil_system.mutex);
	fil_space = fil_space_get_by_id(info.space_id);
	mutex_exit(&fil_system.mutex);
	if (fil_space != NULL) {
		char	tmpname[FN_REFLEN];

		strncpy(tmpname, dest_space_name, FN_REFLEN);

		msg("mariabackup: Renaming %s to %s\n",
		    fil_space->name, dest_space_name);

		if (fil_space->rename(tmpname, NULL, false) != DB_SUCCESS)
		{
			msg("mariabackup: Cannot rename %s to %s\n",
				fil_space->name, dest_space_name);
			goto exit;
		}

		goto found;
	}

	/* No matching space found. create the new one.  */
	const ulint flags = info.page_size.is_compressed()
		? get_bit_shift(info.page_size.physical()
				>> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		<< FSP_FLAGS_POS_ZIP_SSIZE
		| FSP_FLAGS_MASK_POST_ANTELOPE
		| FSP_FLAGS_MASK_ATOMIC_BLOBS
		| (info.page_size.logical() == UNIV_PAGE_SIZE_ORIG
		   ? 0
		   : get_bit_shift(info.page_size.logical()
				   >> (UNIV_ZIP_SIZE_SHIFT_MIN - 1))
		   << FSP_FLAGS_POS_PAGE_SSIZE)
		: FSP_FLAGS_PAGE_SSIZE();
	ut_ad(page_size_t(flags).equals_to(info.page_size));

	if (fil_space_create(dest_space_name, info.space_id, flags,
			      FIL_TYPE_TABLESPACE, 0)) {
		*success = xb_space_create_file(real_name, info.space_id,
						flags, &file);
	} else {
		msg("mariabackup: Cannot create tablespace %s\n",
		    dest_space_name);
	}

	goto exit;
}

/************************************************************************
Applies a given .delta file to the corresponding data file.
@return TRUE on success */
static
ibool
xtrabackup_apply_delta(
	const char*	dirname,	/* in: dir name of incremental */
	const char*	dbname,		/* in: database name (ibdata: NULL) */
	const char*	filename,	/* in: file name (not a path),
					including the .delta extension */
	void*		/*data*/)
{
	pfs_os_file_t	src_file;
	pfs_os_file_t	dst_file;
	char	src_path[FN_REFLEN];
	char	dst_path[FN_REFLEN];
	char	meta_path[FN_REFLEN];
	char	space_name[FN_REFLEN];
	bool	success;

	ibool	last_buffer = FALSE;
	ulint	page_in_buffer;
	ulint	incremental_buffers = 0;

	xb_delta_info_t info(univ_page_size, SRV_TMP_SPACE_ID);
	ulint		page_size;
	ulint		page_size_shift;
	byte*		incremental_buffer_base = NULL;
	byte*		incremental_buffer;

	size_t		offset;

	ut_a(xtrabackup_incremental);

	if (dbname) {
		snprintf(src_path, sizeof(src_path), "%s/%s/%s",
			 dirname, dbname, filename);
		snprintf(dst_path, sizeof(dst_path), "%s/%s/%s",
			 xtrabackup_real_target_dir, dbname, filename);
	} else {
		snprintf(src_path, sizeof(src_path), "%s/%s",
			 dirname, filename);
		snprintf(dst_path, sizeof(dst_path), "%s/%s",
			 xtrabackup_real_target_dir, filename);
	}
	dst_path[strlen(dst_path) - 6] = '\0';

	strncpy(space_name, filename, FN_REFLEN);
	space_name[strlen(space_name) -  6] = 0;

	if (!get_meta_path(src_path, meta_path)) {
		goto error;
	}

	os_normalize_path(dst_path);
	os_normalize_path(src_path);
	os_normalize_path(meta_path);

	if (!xb_read_delta_metadata(meta_path, &info)) {
		goto error;
	}

	page_size = info.page_size.physical();
	page_size_shift = get_bit_shift(page_size);
	msg("mariabackup: page size for %s is %zu bytes\n",
	    src_path, page_size);
	if (page_size_shift < 10 ||
	    page_size_shift > UNIV_PAGE_SIZE_SHIFT_MAX) {
		msg("mariabackup: error: invalid value of page_size "
		    "(%zu bytes) read from %s\n", page_size, meta_path);
		goto error;
	}

	src_file = os_file_create_simple_no_error_handling(
		0, src_path,
		OS_FILE_OPEN, OS_FILE_READ_WRITE, false, &success);
	if (!success) {
		os_file_get_last_error(TRUE);
		msg("mariabackup: error: cannot open %s\n", src_path);
		goto error;
	}

	posix_fadvise(src_file, 0, 0, POSIX_FADV_SEQUENTIAL);

	dst_file = xb_delta_open_matching_space(
			dbname, space_name, info,
			dst_path, sizeof(dst_path), &success);
	if (!success) {
		msg("mariabackup: error: cannot open %s\n", dst_path);
		goto error;
	}

	posix_fadvise(dst_file, 0, 0, POSIX_FADV_DONTNEED);

	/* allocate buffer for incremental backup (4096 pages) */
	incremental_buffer_base = static_cast<byte *>
		(malloc((page_size / 4 + 1) * page_size));
	incremental_buffer = static_cast<byte *>
		(ut_align(incremental_buffer_base,
			  page_size));

	msg("Applying %s to %s...\n", src_path, dst_path);

	while (!last_buffer) {
		ulint cluster_header;

		/* read to buffer */
		/* first block of block cluster */
		offset = ((incremental_buffers * (page_size / 4))
			 << page_size_shift);
		success = os_file_read(IORequestRead, src_file,
				       incremental_buffer, offset, page_size);
		if (!success) {
			goto error;
		}

		cluster_header = mach_read_from_4(incremental_buffer);
		switch(cluster_header) {
			case 0x78747261UL: /*"xtra"*/
				break;
			case 0x58545241UL: /*"XTRA"*/
				last_buffer = TRUE;
				break;
			default:
				msg("mariabackup: error: %s seems not "
				    ".delta file.\n", src_path);
				goto error;
		}

		/* FIXME: If the .delta modifies FSP_SIZE on page 0,
		extend the file to that size. */

		for (page_in_buffer = 1; page_in_buffer < page_size / 4;
		     page_in_buffer++) {
			if (mach_read_from_4(incremental_buffer + page_in_buffer * 4)
			    == 0xFFFFFFFFUL)
				break;
		}

		ut_a(last_buffer || page_in_buffer == page_size / 4);

		/* read whole of the cluster */
		success = os_file_read(IORequestRead, src_file,
				       incremental_buffer,
				       offset, page_in_buffer * page_size);
		if (!success) {
			goto error;
		}

		posix_fadvise(src_file, offset, page_in_buffer * page_size,
			      POSIX_FADV_DONTNEED);

		for (page_in_buffer = 1; page_in_buffer < page_size / 4;
		     page_in_buffer++) {
			ulint offset_on_page;

			offset_on_page = mach_read_from_4(incremental_buffer + page_in_buffer * 4);

			if (offset_on_page == 0xFFFFFFFFUL)
				break;

			uchar *buf = incremental_buffer + page_in_buffer * page_size;
			const os_offset_t off = os_offset_t(offset_on_page)*page_size;

			if (off == 0) {
				/* Read tablespace size from page 0,
				and extend the file to specified size.*/
				os_offset_t n_pages = mach_read_from_4(
					buf + FSP_HEADER_OFFSET + FSP_SIZE);
				if (mach_read_from_4(buf
						     + FIL_PAGE_SPACE_ID)) {
					if (!os_file_set_size(
						    dst_path, dst_file,
						    n_pages * page_size))
						goto error;
				} else if (fil_space_t* space
					   = fil_system.sys_space) {
					/* The system tablespace can
					consist of multiple files. The
					first one has full tablespace
					size in page 0, but only the last
					file should be extended. */
					fil_node_t* n = UT_LIST_GET_FIRST(
						space->chain);
					bool fail = !strcmp(n->name, dst_path)
						&& !fil_space_extend(
							space, (ulint)n_pages);
					if (fail) goto error;
				}
			}

			success = os_file_write(IORequestWrite,
						dst_path, dst_file, buf, off, page_size);
			if (!success) {
				goto error;
			}
		}

		/* Free file system buffer cache after the batch was written. */
#ifdef __linux__
		os_file_flush_func(dst_file);
#endif
		posix_fadvise(dst_file, 0, 0, POSIX_FADV_DONTNEED);


		incremental_buffers++;
	}

	free(incremental_buffer_base);
	if (src_file != OS_FILE_CLOSED) {
		os_file_close(src_file);
		os_file_delete(0,src_path);
	}
	if (dst_file != OS_FILE_CLOSED)
		os_file_close(dst_file);
	return TRUE;

error:
	free(incremental_buffer_base);
	if (src_file != OS_FILE_CLOSED)
		os_file_close(src_file);
	if (dst_file != OS_FILE_CLOSED)
		os_file_close(dst_file);
	msg("mariabackup: Error: xtrabackup_apply_delta(): "
	    "failed to apply %s to %s.\n", src_path, dst_path);
	return FALSE;
}

/************************************************************************
Callback to handle datadir entry. Function of this type will be called
for each entry which matches the mask by xb_process_datadir.
@return should return TRUE on success */
typedef ibool (*handle_datadir_entry_func_t)(
/*=========================================*/
	const char*	data_home_dir,		/*!<in: path to datadir */
	const char*	db_name,		/*!<in: database name */
	const char*	file_name,		/*!<in: file name with suffix */
	void*		arg);			/*!<in: caller-provided data */

/************************************************************************
Callback to handle datadir entry. Deletes entry if it has no matching
fil_space in fil_system directory.
@return FALSE if delete attempt was unsuccessful */
static
ibool
rm_if_not_found(
	const char*	data_home_dir,		/*!<in: path to datadir */
	const char*	db_name,		/*!<in: database name */
	const char*	file_name,		/*!<in: file name with suffix */
	void*		arg __attribute__((unused)))
{
	char			name[FN_REFLEN];
	xb_filter_entry_t*	table;

	snprintf(name, FN_REFLEN, "%s/%s", db_name, file_name);
	/* Truncate ".ibd" */
	name[strlen(name) - 4] = '\0';

	HASH_SEARCH(name_hash, inc_dir_tables_hash, ut_fold_string(name),
		    xb_filter_entry_t*,
		    table, (void) 0,
		    !strcmp(table->name, name));

	if (!table) {
		snprintf(name, FN_REFLEN, "%s/%s/%s", data_home_dir,
						      db_name, file_name);
		return os_file_delete(0, name);
	}

	return(TRUE);
}

/************************************************************************
Function enumerates files in datadir (provided by path) which are matched
by provided suffix. For each entry callback is called.
@return FALSE if callback for some entry returned FALSE */
static
ibool
xb_process_datadir(
	const char*			path,	/*!<in: datadir path */
	const char*			suffix,	/*!<in: suffix to match
						against */
	handle_datadir_entry_func_t	func)	/*!<in: callback */
{
	ulint		ret;
	char		dbpath[OS_FILE_MAX_PATH+1];
	os_file_dir_t	dir;
	os_file_dir_t	dbdir;
	os_file_stat_t	dbinfo;
	os_file_stat_t	fileinfo;
	ulint		suffix_len;
	dberr_t		err 		= DB_SUCCESS;
	static char	current_dir[2];

	current_dir[0] = FN_CURLIB;
	current_dir[1] = 0;
	srv_data_home = current_dir;

	suffix_len = strlen(suffix);

	/* datafile */
	dbdir = os_file_opendir(path, FALSE);

	if (dbdir != NULL) {
		ret = fil_file_readdir_next_file(&err, path, dbdir,
							&fileinfo);
		while (ret == 0) {
			if (fileinfo.type == OS_FILE_TYPE_DIR) {
				goto next_file_item_1;
			}

			if (strlen(fileinfo.name) > suffix_len
			    && 0 == strcmp(fileinfo.name + 
					strlen(fileinfo.name) - suffix_len,
					suffix)) {
				if (!func(
					    path, NULL,
					    fileinfo.name, NULL))
				{
					return(FALSE);
				}
			}
next_file_item_1:
			ret = fil_file_readdir_next_file(&err,
							path, dbdir,
							&fileinfo);
		}

		os_file_closedir(dbdir);
	} else {
		msg("mariabackup: Cannot open dir %s\n",
		    path);
	}

	/* single table tablespaces */
	dir = os_file_opendir(path, FALSE);

	if (dir == NULL) {
		msg("mariabackup: Cannot open dir %s\n",
		    path);
	}

		ret = fil_file_readdir_next_file(&err, path, dir,
								&dbinfo);
	while (ret == 0) {
		if (dbinfo.type == OS_FILE_TYPE_FILE
		    || dbinfo.type == OS_FILE_TYPE_UNKNOWN) {

		        goto next_datadir_item;
		}

		snprintf(dbpath, sizeof(dbpath)-1, "%s/%s", path, dbinfo.name);

		os_normalize_path(dbpath);

		dbdir = os_file_opendir(dbpath, FALSE);

		if (dbdir != NULL) {

			ret = fil_file_readdir_next_file(&err, dbpath, dbdir,
								&fileinfo);
			while (ret == 0) {

			        if (fileinfo.type == OS_FILE_TYPE_DIR) {

				        goto next_file_item_2;
				}

				if (strlen(fileinfo.name) > suffix_len
				    && 0 == strcmp(fileinfo.name + 
						strlen(fileinfo.name) -
								suffix_len,
						suffix)) {
					/* The name ends in suffix; process
					the file */
					if (!func(
						    path,
						    dbinfo.name,
						    fileinfo.name, NULL))
					{
						return(FALSE);
					}
				}
next_file_item_2:
				ret = fil_file_readdir_next_file(&err,
								dbpath, dbdir,
								&fileinfo);
			}

			os_file_closedir(dbdir);
		}
next_datadir_item:
		ret = fil_file_readdir_next_file(&err,
						path,
								dir, &dbinfo);
	}

	os_file_closedir(dir);

	return(TRUE);
}

/************************************************************************
Applies all .delta files from incremental_dir to the full backup.
@return TRUE on success. */
static
ibool
xtrabackup_apply_deltas()
{
	return xb_process_datadir(xtrabackup_incremental_dir, ".delta",
		xtrabackup_apply_delta);
}


static
void
innodb_free_param()
{
	srv_sys_space.shutdown();
	free_tmpdir(&mysql_tmpdir_list);
}


/** Store the current binary log coordinates in a specified file.
@param[in]	filename	file name
@param[in]	name		binary log file name
@param[in]	pos		binary log file position
@return whether the operation succeeded */
static bool
store_binlog_info(const char* filename, const char* name, ulonglong pos)
{
	FILE *fp = fopen(filename, "w");

	if (!fp) {
		msg("mariabackup: failed to open '%s'\n", filename);
		return(false);
	}

	fprintf(fp, "%s\t%llu\n", name, pos);
	fclose(fp);

	return(true);
}

/** Implement --prepare
@return	whether the operation succeeded */
static bool
xtrabackup_prepare_func(char** argv)
{
	char			 metadata_path[FN_REFLEN];

	/* cd to target-dir */

	if (my_setwd(xtrabackup_real_target_dir,MYF(MY_WME)))
	{
		msg("mariabackup: cannot my_setwd %s\n",
		    xtrabackup_real_target_dir);
		return(false);
	}
	msg("mariabackup: cd to %s\n", xtrabackup_real_target_dir);

	int argc; for (argc = 0; argv[argc]; argc++) {}
	encryption_plugin_prepare_init(argc, argv);

	xtrabackup_target_dir= mysql_data_home_buff;
	xtrabackup_target_dir[0]=FN_CURLIB;		// all paths are relative from here
	xtrabackup_target_dir[1]=0;
	const lsn_t target_lsn = xtrabackup_incremental
		? incremental_to_lsn : metadata_to_lsn;

	/*
	  read metadata of target
	*/
	sprintf(metadata_path, "%s/%s", xtrabackup_target_dir,
		XTRABACKUP_METADATA_FILENAME);

	if (!xtrabackup_read_metadata(metadata_path)) {
		msg("mariabackup: Error: failed to read metadata from '%s'\n",
		    metadata_path);
		return(false);
	}

	if (!strcmp(metadata_type, "full-backuped")) {
		if (xtrabackup_incremental) {
			msg("mariabackup: error: applying incremental backup "
			    "needs a prepared target.\n");
			return(false);
		}
		msg("mariabackup: This target seems to be not prepared yet.\n");
	} else if (!strcmp(metadata_type, "log-applied")) {
		msg("mariabackup: This target seems to be already prepared.\n");
	} else {
		msg("mariabackup: This target does not have correct metadata.\n");
		return(false);
	}

	bool ok = !xtrabackup_incremental
		|| metadata_to_lsn == incremental_lsn;
	if (!ok) {
		msg("mariabackup: error: This incremental backup seems "
		    "not to be proper for the target.\n"
		    "mariabackup:  Check 'to_lsn' of the target and "
		    "'from_lsn' of the incremental.\n");
		return(false);
	}

	srv_max_n_threads = 1000;
	srv_undo_logs = 1;
	srv_n_purge_threads = 1;

	xb_filters_init();

	srv_log_group_home_dir = NULL;
	srv_thread_concurrency = 1;

	if (xtrabackup_incremental) {
		srv_operation = SRV_OPERATION_RESTORE_DELTA;

		if (innodb_init_param()) {
			goto error_cleanup;
		}

		xb_normalize_init_values();
		sync_check_init();
		ut_d(sync_check_enable());
		ut_crc32_init();
		recv_sys_init();
		log_sys_init();
		recv_recovery_on = true;

#ifdef WITH_INNODB_DISALLOW_WRITES
		srv_allow_writes_event = os_event_create(0);
		os_event_set(srv_allow_writes_event);
#endif
		dberr_t err = xb_data_files_init();
		if (err != DB_SUCCESS) {
			msg("mariabackup: error: xb_data_files_init() failed "
			    "with error %s\n", ut_strerr(err));
			goto error_cleanup;
		}

		inc_dir_tables_hash = hash_create(1000);

		ok = xtrabackup_apply_deltas();

		xb_data_files_close();

		if (ok) {
			/* Cleanup datadir from tablespaces deleted
			between full and incremental backups */

			xb_process_datadir("./", ".ibd", rm_if_not_found);
		}

		xb_filter_hash_free(inc_dir_tables_hash);

		fil_system.close();
#ifdef WITH_INNODB_DISALLOW_WRITES
		os_event_destroy(srv_allow_writes_event);
#endif
		innodb_free_param();
		log_shutdown();
		sync_check_close();
		if (!ok) goto error_cleanup;
	}

	srv_operation = xtrabackup_export
		? SRV_OPERATION_RESTORE_EXPORT : SRV_OPERATION_RESTORE;

	if (innodb_init_param()) {
		goto error_cleanup;
	}

	/* increase IO threads */
	if (srv_n_file_io_threads < 10) {
		srv_n_read_io_threads = 4;
		srv_n_write_io_threads = 4;
	}

	msg("mariabackup: Starting InnoDB instance for recovery.\n"
	    "mariabackup: Using %lld bytes for buffer pool "
	    "(set by --use-memory parameter)\n", xtrabackup_use_memory);

	srv_max_buf_pool_modified_pct = (double)max_buf_pool_modified_pct;

	if (srv_max_dirty_pages_pct_lwm > srv_max_buf_pool_modified_pct) {
		srv_max_dirty_pages_pct_lwm = srv_max_buf_pool_modified_pct;
	}

	if (innodb_init()) {
		goto error_cleanup;
	}

	if (ok) {
		msg("Last binlog file %s, position %lld\n",
		    trx_sys.recovered_binlog_filename,
		    longlong(trx_sys.recovered_binlog_offset));

		/* output to xtrabackup_binlog_pos_innodb and
		   (if backup_safe_binlog_info was available on
		   the server) to xtrabackup_binlog_info. In the
		   latter case xtrabackup_binlog_pos_innodb
		   becomes redundant and is created only for
		   compatibility. */
		ok = store_binlog_info("xtrabackup_binlog_pos_innodb",
				       trx_sys.recovered_binlog_filename,
				       trx_sys.recovered_binlog_offset)
		  && (!recover_binlog_info
		      || store_binlog_info(XTRABACKUP_BINLOG_INFO,
					   trx_sys.recovered_binlog_filename,
					   trx_sys.recovered_binlog_offset));
	}

	/* Check whether the log is applied enough or not. */
	if ((srv_start_lsn || fil_space_get(SRV_LOG_SPACE_FIRST_ID))
	    && srv_start_lsn < target_lsn) {
		msg("mariabackup: error: "
		    "The log was only applied up to LSN " LSN_PF
		    ", instead of " LSN_PF "\n",
		    srv_start_lsn, target_lsn);
		ok = false;
	}
#ifdef WITH_WSREP
	else if (ok) xb_write_galera_info(xtrabackup_incremental);
#endif

	innodb_shutdown();
	innodb_free_param();

	/* output to metadata file */
	if (ok) {
		char	filename[FN_REFLEN];

		strcpy(metadata_type, "log-applied");

		if(xtrabackup_incremental
		   && metadata_to_lsn < incremental_to_lsn)
		{
			metadata_to_lsn = incremental_to_lsn;
			metadata_last_lsn = incremental_last_lsn;
		}

		sprintf(filename, "%s/%s", xtrabackup_target_dir, XTRABACKUP_METADATA_FILENAME);
		if (!xtrabackup_write_metadata(filename)) {

			msg("mariabackup: Error: failed to write metadata "
			    "to '%s'\n", filename);
			ok = false;
		} else if (xtrabackup_extra_lsndir) {
			sprintf(filename, "%s/%s", xtrabackup_extra_lsndir, XTRABACKUP_METADATA_FILENAME);
			if (!xtrabackup_write_metadata(filename)) {
				msg("mariabackup: Error: failed to write "
				    "metadata to '%s'\n", filename);
				ok = false;
			}
		}
	}

	if (ok) ok = apply_log_finish();

	if (ok && xtrabackup_export)
		ok= (prepare_export() == 0);

error_cleanup:
	xb_filters_free();
	return ok;
}

/**************************************************************************
Append group name to xb_load_default_groups list. */
static
void
append_defaults_group(const char *group, const char *default_groups[],
		      size_t default_groups_size)
{
	uint i;
	bool appended = false;
	for (i = 0; i < default_groups_size - 1; i++) {
		if (default_groups[i] == NULL) {
			default_groups[i] = group;
			appended = true;
			break;
		}
	}
	ut_a(appended);
}

bool
xb_init()
{
	const char *mixed_options[4] = {NULL, NULL, NULL, NULL};
	int n_mixed_options;

	/* sanity checks */

	if (opt_slave_info
		&& opt_no_lock
		&& !opt_safe_slave_backup) {
		msg("Error: --slave-info is used with --no-lock but "
			"without --safe-slave-backup. The binlog position "
			"cannot be consistent with the backup data.\n");
		return(false);
	}

	if (xtrabackup_backup && opt_rsync)
	{
		if (xtrabackup_stream_fmt)
		{
			msg("Error: --rsync doesn't work with --stream\n");
			return(false);
		}
		bool have_rsync = IF_WIN(false, (system("rsync --version > /dev/null 2>&1") == 0));
		if (!have_rsync)
		{
			msg("Error: rsync executable not found, cannot run backup with --rsync\n");
			return false;
		}
	}

	n_mixed_options = 0;

	if (opt_decompress) {
		mixed_options[n_mixed_options++] = "--decompress";
	}

	if (xtrabackup_copy_back) {
		mixed_options[n_mixed_options++] = "--copy-back";
	}

	if (xtrabackup_move_back) {
		mixed_options[n_mixed_options++] = "--move-back";
	}

	if (xtrabackup_prepare) {
		mixed_options[n_mixed_options++] = "--apply-log";
	}

	if (n_mixed_options > 1) {
		msg("Error: %s and %s are mutually exclusive\n",
			mixed_options[0], mixed_options[1]);
		return(false);
	}

	if (xtrabackup_backup) {
		if ((mysql_connection = xb_mysql_connect()) == NULL) {
			return(false);
		}

		if (!get_mysql_vars(mysql_connection)) {
			return(false);
		}

		encryption_plugin_backup_init(mysql_connection);
		history_start_time = time(NULL);

	}

	return(true);
}


extern void init_signals(void);

#include <sql_locale.h>

/* Messages . Avoid loading errmsg.sys file */
void setup_error_messages()
{
  static const char *my_msgs[ERRORS_PER_RANGE];
  static const char **all_msgs[] = { my_msgs, my_msgs, my_msgs, my_msgs };
  my_default_lc_messages = &my_locale_en_US;
  my_default_lc_messages->errmsgs->errmsgs = all_msgs;

  /* Populate the necessary error messages */
  struct {
    int id;
    const char *fmt;
  }
  xb_msgs[] =
  {
  { ER_DATABASE_NAME,"Database" },
  { ER_TABLE_NAME,"Table"},
  { ER_PARTITION_NAME, "Partition" },
  { ER_SUBPARTITION_NAME, "Subpartition" },
  { ER_TEMPORARY_NAME, "Temporary"},
  { ER_RENAMED_NAME, "Renamed"},
  { ER_CANT_FIND_DL_ENTRY, "Can't find symbol '%-.128s' in library"},
  { ER_CANT_OPEN_LIBRARY, "Can't open shared library '%-.192s' (errno: %d, %-.128s)" },
  { ER_OUTOFMEMORY, "Out of memory; restart server and try again (needed %d bytes)" },
  { ER_CANT_OPEN_LIBRARY, "Can't open shared library '%-.192s' (errno: %d, %-.128s)" },
  { ER_UDF_NO_PATHS, "No paths allowed for shared library" },
  { ER_CANT_INITIALIZE_UDF,"Can't initialize function '%-.192s'; %-.80s"},
  { ER_PLUGIN_IS_NOT_LOADED,"Plugin '%-.192s' is not loaded" }
  };

  for (int i = 0; i < (int)array_elements(all_msgs); i++)
    all_msgs[0][i] = "Unknown error";

  for (int i = 0; i < (int)array_elements(xb_msgs); i++)
    all_msgs[0][xb_msgs[i].id - ER_ERROR_FIRST] = xb_msgs[i].fmt;
}

void
handle_options(int argc, char **argv, char ***argv_client, char ***argv_server)
{
	/* Setup some variables for Innodb.*/

	srv_operation = SRV_OPERATION_RESTORE;

	files_charset_info = &my_charset_utf8_general_ci;
	check_if_backup_includes = backup_includes;

	setup_error_messages();
	sys_var_init();
	plugin_mutex_init();
	mysql_prlock_init(key_rwlock_LOCK_system_variables_hash, &LOCK_system_variables_hash);
	opt_stack_trace = 1;
	test_flags |=  TEST_SIGINT;
	init_signals();
#ifndef _WIN32
	/* Exit process on SIGINT. */
	my_sigset(SIGINT, SIG_DFL);
#endif

	sf_leaking_memory = 1; /* don't report memory leaks on early exist */

	int i;
	int ho_error;

	char*	target_dir = NULL;
	bool	prepare = false;

	char	conf_file[FN_REFLEN];
	int	argc_client = argc;
	int	argc_server = argc;

	/* scan options for group and config file to load defaults from */
	for (i = 1; i < argc; i++) {

		char *optend = strcend(argv[i], '=');

		if (strncmp(argv[i], "--defaults-group",
			    optend - argv[i]) == 0) {
			defaults_group = optend + 1;
			append_defaults_group(defaults_group,
				xb_server_default_groups,
				array_elements(xb_server_default_groups));
		}

		if (strncmp(argv[i], "--login-path",
			    optend - argv[i]) == 0) {
			append_defaults_group(optend + 1,
				xb_client_default_groups,
				array_elements(xb_client_default_groups));
		}

		if (!strncmp(argv[i], "--prepare",
			     optend - argv[i])) {
			prepare = true;
		}

		if (!strncmp(argv[i], "--apply-log",
			     optend - argv[i])) {
			prepare = true;
		}

		if (!strncmp(argv[i], "--target-dir",
			     optend - argv[i]) && *optend) {
			target_dir = optend + 1;
		}

		if (!*optend && argv[i][0] != '-') {
			target_dir = argv[i];
		}
	}

	snprintf(conf_file, sizeof(conf_file), "my");

	if (prepare && target_dir) {
		snprintf(conf_file, sizeof(conf_file),
			 "%s/backup-my.cnf", target_dir);
			if (!strncmp(argv[1], "--defaults-file=", 16)) {
				/* Remove defaults-file*/
				for (int i = 2; ; i++) {
					if ((argv[i-1]= argv[i]) == 0)
						break;
				}
				argc--;
			}
	}

	*argv_client = argv;
	*argv_server = argv;
	load_defaults_or_exit(conf_file, xb_server_default_groups,
			      &argc_server, argv_server);

	int n;
	for (n = 0; (*argv_server)[n]; n++) {};
	argc_server = n;

	print_param_str <<
		"# This MySQL options file was generated by XtraBackup.\n"
		"[" << defaults_group << "]\n";

	/* We want xtrabackup to ignore unknown options, because it only
	recognizes a small subset of server variables */
	my_getopt_skip_unknown = TRUE;

	/* Reset u_max_value for all options, as we don't want the
	--maximum-... modifier to set the actual option values */
	for (my_option *optp= xb_server_options; optp->name; optp++) {
		optp->u_max_value = (G_PTR *) &global_max_value;
	}


	/* Throw a descriptive error if --defaults-file or --defaults-extra-file
	is not the first command line argument */
	for (int i = 2 ; i < argc ; i++) {
		char *optend = strcend((argv)[i], '=');

		if (optend - argv[i] == 15 &&
                    !strncmp(argv[i], "--defaults-file", optend - argv[i])) {

			msg("mariabackup: Error: --defaults-file "
			    "must be specified first on the command "
			    "line\n");
			exit(EXIT_FAILURE);
		}
                if (optend - argv[i] == 21 &&
		    !strncmp(argv[i], "--defaults-extra-file",
			     optend - argv[i])) {

			msg("mariabackup: Error: --defaults-extra-file "
			    "must be specified first on the command "
			    "line\n");
			exit(EXIT_FAILURE);
		}
	}

	if (argc_server > 0
	    && (ho_error=handle_options(&argc_server, argv_server,
					xb_server_options, xb_get_one_option)))
		exit(ho_error);

	load_defaults_or_exit(conf_file, xb_client_default_groups,
			      &argc_client, argv_client);

	for (n = 0; (*argv_client)[n]; n++) {};
 	argc_client = n;

	if (innobackupex_mode && argc_client > 0) {
		/* emulate innobackupex script */
		innobackupex_mode = true;
		if (!ibx_handle_options(&argc_client, argv_client)) {
			exit(EXIT_FAILURE);
		}
	}

	if (argc_client > 0
	    && (ho_error=handle_options(&argc_client, argv_client,
					xb_client_options, xb_get_one_option)))
		exit(ho_error);

	/* Reject command line arguments that don't look like options, i.e. are
	not of the form '-X' (single-character options) or '--option' (long
	options) */
	for (int i = 0 ; i < argc_client ; i++) {
		const char * const opt = (*argv_client)[i];

		if (strncmp(opt, "--", 2) &&
		    !(strlen(opt) == 2 && opt[0] == '-')) {
			bool server_option = true;

			for (int j = 0; j < argc_server; j++) {
				if (opt == (*argv_server)[j]) {
					server_option = false;
					break;
				}
			}

			if (!server_option) {
				msg("mariabackup: Error:"
				    " unknown argument: '%s'\n", opt);
				exit(EXIT_FAILURE);
			}
		}
	}
}

static int main_low(char** argv);
static int get_exepath(char *buf, size_t size, const char *argv0);

/* ================= main =================== */
int main(int argc, char **argv)
{
	char **client_defaults, **server_defaults;

	if (get_exepath(mariabackup_exe,FN_REFLEN, argv[0]))
    strncpy(mariabackup_exe,argv[0], FN_REFLEN-1);


	if (argc > 1 )
	{
		/* In "prepare export", we need  to start mysqld 
		Since it is not always be installed on the machine,
		we start "mariabackup --mysqld", which acts as mysqld
		*/
		if (strcmp(argv[1], "--mysqld") == 0)
		{
			extern int mysqld_main(int argc, char **argv);
			argc--;
			argv++;
			argv[0]+=2;
			return mysqld_main(argc, argv);
		}
		if(strcmp(argv[1], "--innobackupex") == 0)
		{
			argv++;
			argc--;
			innobackupex_mode = true;
		}
	}
  
	if (argc > 1)
		strncpy(orig_argv1,argv[1],sizeof(orig_argv1) -1);

	init_signals();
	MY_INIT(argv[0]);

	pthread_key_create(&THR_THD, NULL);
	my_pthread_setspecific_ptr(THR_THD, NULL);

	xb_regex_init();

	capture_tool_command(argc, argv);

	if (mysql_server_init(-1, NULL, NULL))
	{
		exit(EXIT_FAILURE);
	}

	system_charset_info = &my_charset_utf8_general_ci;
	key_map_full.set_all();

	logger.init_base();
	logger.set_handlers(LOG_FILE, LOG_NONE, LOG_NONE);
	mysql_mutex_init(key_LOCK_error_log, &LOCK_error_log,
			 MY_MUTEX_INIT_FAST);

	handle_options(argc, argv, &client_defaults, &server_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_SET_INITIAL(dbug_option);
		DBUG_SET(dbug_option);
	}
#endif

	int status = main_low(server_defaults);

	backup_cleanup();

	if (innobackupex_mode) {
		ibx_cleanup();
	}

	free_defaults(client_defaults);
	free_defaults(server_defaults);

#ifndef DBUG_OFF
	if (dbug_option) {
		DBUG_END();
	}
#endif

	if (THR_THD)
		(void) pthread_key_delete(THR_THD);

	logger.cleanup_base();
	mysql_mutex_destroy(&LOCK_error_log);

	if (status == EXIT_SUCCESS) {
		msg_ts("completed OK!\n");
	}

	return status;
}

static int main_low(char** argv)
{
	if (innobackupex_mode) {
		if (!ibx_init()) {
			return(EXIT_FAILURE);
		}
	}

	if (!xtrabackup_print_param && !xtrabackup_prepare
	    && !strcmp(mysql_data_home, "./")) {
		if (!xtrabackup_print_param)
			usage();
		msg("\nmariabackup: Error: Please set parameter 'datadir'\n");
		return(EXIT_FAILURE);
	}

	/* Expand target-dir, incremental-basedir, etc. */

	char cwd[FN_REFLEN];
	my_getwd(cwd, sizeof(cwd), MYF(0));

	my_load_path(xtrabackup_real_target_dir,
		     xtrabackup_target_dir, cwd);
	unpack_dirname(xtrabackup_real_target_dir,
		       xtrabackup_real_target_dir);
	xtrabackup_target_dir= xtrabackup_real_target_dir;

	if (xtrabackup_incremental_basedir) {
		my_load_path(xtrabackup_real_incremental_basedir,
			     xtrabackup_incremental_basedir, cwd);
		unpack_dirname(xtrabackup_real_incremental_basedir,
			       xtrabackup_real_incremental_basedir);
		xtrabackup_incremental_basedir =
			xtrabackup_real_incremental_basedir;
	}

	if (xtrabackup_incremental_dir) {
		my_load_path(xtrabackup_real_incremental_dir,
			     xtrabackup_incremental_dir, cwd);
		unpack_dirname(xtrabackup_real_incremental_dir,
			       xtrabackup_real_incremental_dir);
		xtrabackup_incremental_dir = xtrabackup_real_incremental_dir;
	}

	if (xtrabackup_extra_lsndir) {
		my_load_path(xtrabackup_real_extra_lsndir,
			     xtrabackup_extra_lsndir, cwd);
		unpack_dirname(xtrabackup_real_extra_lsndir,
			       xtrabackup_real_extra_lsndir);
		xtrabackup_extra_lsndir = xtrabackup_real_extra_lsndir;
	}

	/* get default temporary directory */
	if (!opt_mysql_tmpdir || !opt_mysql_tmpdir[0]) {
		opt_mysql_tmpdir = getenv("TMPDIR");
#if defined(__WIN__)
		if (!opt_mysql_tmpdir) {
			opt_mysql_tmpdir = getenv("TEMP");
		}
		if (!opt_mysql_tmpdir) {
			opt_mysql_tmpdir = getenv("TMP");
		}
#endif
		if (!opt_mysql_tmpdir || !opt_mysql_tmpdir[0]) {
			opt_mysql_tmpdir = const_cast<char*>(DEFAULT_TMPDIR);
		}
	}

	/* temporary setting of enough size */
	srv_page_size_shift = UNIV_PAGE_SIZE_SHIFT_MAX;
	srv_page_size = UNIV_PAGE_SIZE_MAX;
	if (xtrabackup_backup && xtrabackup_incremental) {
		/* direct specification is only for --backup */
		/* and the lsn is prior to the other option */

		char* endchar;
		int error = 0;
		incremental_lsn = strtoll(xtrabackup_incremental, &endchar, 10);
		if (*endchar != '\0')
			error = 1;

		if (error) {
			msg("mariabackup: value '%s' may be wrong format for "
			    "incremental option.\n", xtrabackup_incremental);
			return(EXIT_FAILURE);
		}
	} else if (xtrabackup_backup && xtrabackup_incremental_basedir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_incremental_basedir, XTRABACKUP_METADATA_FILENAME);

		if (!xtrabackup_read_metadata(filename)) {
			msg("mariabackup: error: failed to read metadata from "
			    "%s\n", filename);
			return(EXIT_FAILURE);
		}

		incremental_lsn = metadata_to_lsn;
		xtrabackup_incremental = xtrabackup_incremental_basedir; //dummy
	} else if (xtrabackup_prepare && xtrabackup_incremental_dir) {
		char	filename[FN_REFLEN];

		sprintf(filename, "%s/%s", xtrabackup_incremental_dir, XTRABACKUP_METADATA_FILENAME);

		if (!xtrabackup_read_metadata(filename)) {
			msg("mariabackup: error: failed to read metadata from "
			    "%s\n", filename);
			return(EXIT_FAILURE);
		}

		incremental_lsn = metadata_from_lsn;
		incremental_to_lsn = metadata_to_lsn;
		incremental_last_lsn = metadata_last_lsn;
		xtrabackup_incremental = xtrabackup_incremental_dir; //dummy

	} else if (opt_incremental_history_name) {
		xtrabackup_incremental = opt_incremental_history_name;
	} else if (opt_incremental_history_uuid) {
		xtrabackup_incremental = opt_incremental_history_uuid;
	} else {
		xtrabackup_incremental = NULL;
	}

	if (xtrabackup_stream && !xtrabackup_backup) {
		msg("Warning: --stream parameter is ignored, it only works together with --backup.\n");
	}

	if (!xb_init()) {
		return(EXIT_FAILURE);
	}

	/* --print-param */
	if (xtrabackup_print_param) {
		printf("%s", print_param_str.str().c_str());
		return(EXIT_SUCCESS);
	}

	print_version();
	if (xtrabackup_incremental) {
		msg("incremental backup from " LSN_PF " is enabled.\n",
		    incremental_lsn);
	}

	if (xtrabackup_export && innobase_file_per_table == FALSE) {
		msg("mariabackup: auto-enabling --innodb-file-per-table due to "
		    "the --export option\n");
		innobase_file_per_table = TRUE;
	}

	/* cannot execute both for now */
	{
		int num = 0;

		if (xtrabackup_backup) num++;
		if (xtrabackup_prepare) num++;
		if (xtrabackup_copy_back) num++;
		if (xtrabackup_move_back) num++;
		if (xtrabackup_decrypt_decompress) num++;
		if (num != 1) { /* !XOR (for now) */
			usage();
			return(EXIT_FAILURE);
		}
	}

#ifndef __WIN__
	if (xtrabackup_debug_sync) {
		signal(SIGCONT, sigcont_handler);
	}
#endif

	/* --backup */
	if (xtrabackup_backup && !xtrabackup_backup_func()) {
		return(EXIT_FAILURE);
	}

	/* --prepare */
	if (xtrabackup_prepare
	    && !xtrabackup_prepare_func(argv)) {
		return(EXIT_FAILURE);
	}

	if (xtrabackup_copy_back || xtrabackup_move_back) {
		if (!check_if_param_set("datadir")) {
			msg("Error: datadir must be specified.\n");
			return(EXIT_FAILURE);
		}
		if (!copy_back())
			return(EXIT_FAILURE);
	}

	if (xtrabackup_decrypt_decompress && !decrypt_decompress()) {
		return(EXIT_FAILURE);
	}

	return(EXIT_SUCCESS);
}


static int get_exepath(char *buf, size_t size, const char *argv0)
{
#ifdef _WIN32
  DWORD ret = GetModuleFileNameA(NULL, buf, (DWORD)size);
  if (ret > 0)
    return 0;
#elif defined(__linux__)
  ssize_t ret = readlink("/proc/self/exe", buf, size-1);
  if(ret > 0)
    return 0;
#endif

  return my_realpath(buf, argv0, 0);
}
