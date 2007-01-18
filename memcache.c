/*
  +----------------------------------------------------------------------+
  | PHP Version 5                                                        |
  +----------------------------------------------------------------------+
  | Copyright (c) 1997-2004 The PHP Group                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.0 of the PHP license,       |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_0.txt.                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Authors: Antony Dovgal <tony2001@phpclub.net>                        |
  |          Mikael Johansson <mikael AT synd DOT info>                  |
  +----------------------------------------------------------------------+
*/

/* $Id$ */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "php.h"
#include "php_ini.h"
#include <stdio.h>
#include <fcntl.h>
#ifdef HAVE_SYS_FILE_H
#include <sys/file.h>
#endif

#if HAVE_MEMCACHE

#include <zlib.h>
#include <time.h>
#include "ext/standard/info.h"
#include "ext/standard/php_string.h"
#include "ext/standard/php_var.h"
#include "ext/standard/php_smart_str.h"
#include "ext/standard/crc32.h"
#include "php_network.h"
#include "php_memcache.h"

#ifndef ZEND_ENGINE_2
#define OnUpdateLong OnUpdateInt
#endif

/* True global resources - no need for thread safety here */
static int le_memcache_pool, le_pmemcache;
static zend_class_entry *memcache_class_entry_ptr;

ZEND_DECLARE_MODULE_GLOBALS(memcache)

/* {{{ memcache_functions[]
 */
zend_function_entry memcache_functions[] = {
	PHP_FE(memcache_connect,		NULL)
	PHP_FE(memcache_pconnect,		NULL)
	PHP_FE(memcache_add_server,		NULL)
	PHP_FE(memcache_set_server_params,		NULL)
	PHP_FE(memcache_get_server_status,		NULL)
	PHP_FE(memcache_get_version,	NULL)
	PHP_FE(memcache_add,			NULL)
	PHP_FE(memcache_set,			NULL)
	PHP_FE(memcache_replace,		NULL)
	PHP_FE(memcache_get,			NULL)
	PHP_FE(memcache_delete,			NULL)
	PHP_FE(memcache_debug,			NULL)
	PHP_FE(memcache_get_stats,		NULL)
	PHP_FE(memcache_get_extended_stats,		NULL)
	PHP_FE(memcache_set_compress_threshold,	NULL)
	PHP_FE(memcache_increment,		NULL)
	PHP_FE(memcache_decrement,		NULL)
	PHP_FE(memcache_close,			NULL)
	PHP_FE(memcache_flush,			NULL)
	{NULL, NULL, NULL}
};

static zend_function_entry php_memcache_class_functions[] = {
	PHP_FALIAS(connect,			memcache_connect,			NULL)
	PHP_FALIAS(pconnect,		memcache_pconnect,			NULL)
	PHP_FALIAS(addserver,		memcache_add_server,		NULL)
	PHP_FALIAS(setserverparams,		memcache_set_server_params,		NULL)
	PHP_FALIAS(getserverstatus,		memcache_get_server_status,		NULL)
	PHP_FALIAS(getversion,		memcache_get_version,		NULL)
	PHP_FALIAS(add,				memcache_add,				NULL)
	PHP_FALIAS(set,				memcache_set,				NULL)
	PHP_FALIAS(replace,			memcache_replace,			NULL)
	PHP_FALIAS(get,				memcache_get,				NULL)
	PHP_FALIAS(delete,			memcache_delete,			NULL)
	PHP_FALIAS(getstats,		memcache_get_stats,			NULL)
	PHP_FALIAS(getextendedstats,		memcache_get_extended_stats,		NULL)
	PHP_FALIAS(setcompressthreshold,	memcache_set_compress_threshold,	NULL)
	PHP_FALIAS(increment,		memcache_increment,			NULL)
	PHP_FALIAS(decrement,		memcache_decrement,			NULL)
	PHP_FALIAS(close,			memcache_close,				NULL)
	PHP_FALIAS(flush,			memcache_flush,				NULL)
	{NULL, NULL, NULL}
};

/* }}} */

/* {{{ memcache_module_entry
 */
zend_module_entry memcache_module_entry = {
#if ZEND_MODULE_API_NO >= 20010901
	STANDARD_MODULE_HEADER,
#endif
	"memcache",
	memcache_functions,
	PHP_MINIT(memcache),
	PHP_MSHUTDOWN(memcache),
	PHP_RINIT(memcache),
	NULL,
	PHP_MINFO(memcache),
#if ZEND_MODULE_API_NO >= 20010901
	NO_VERSION_YET, 			/* Replace with version number for your extension */
#endif
	STANDARD_MODULE_PROPERTIES
};
/* }}} */

#ifdef COMPILE_DL_MEMCACHE
ZEND_GET_MODULE(memcache)
#endif

static PHP_INI_MH(OnUpdateChunkSize) /* {{{ */
{
	long int lval;

	lval = strtol(new_value, NULL, 10);
	if (lval <= 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "memcache.chunk_size must be a positive integer ('%s' given)", new_value);
		return FAILURE;
	}

	return OnUpdateLong(entry, new_value, new_value_length, mh_arg1, mh_arg2, mh_arg3, stage TSRMLS_CC);
}
/* }}} */

static PHP_INI_MH(OnUpdateFailoverAttempts) /* {{{ */
{
	long int lval;

	lval = strtol(new_value, NULL, 10);
	if (lval <= 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "memcache.max_failover_attempts must be a positive integer ('%s' given)", new_value);
		return FAILURE;
	}

	return OnUpdateLong(entry, new_value, new_value_length, mh_arg1, mh_arg2, mh_arg3, stage TSRMLS_CC);
}
/* }}} */

/* {{{ PHP_INI */
PHP_INI_BEGIN()
	STD_PHP_INI_ENTRY("memcache.allow_failover",	"1",		PHP_INI_ALL, OnUpdateLong,		allow_failover,	zend_memcache_globals,	memcache_globals)
	STD_PHP_INI_ENTRY("memcache.max_failover_attempts",	"20",	PHP_INI_ALL, OnUpdateFailoverAttempts,		max_failover_attempts,	zend_memcache_globals,	memcache_globals)
	STD_PHP_INI_ENTRY("memcache.default_port",		"11211",	PHP_INI_ALL, OnUpdateLong,		default_port,	zend_memcache_globals,	memcache_globals)
	STD_PHP_INI_ENTRY("memcache.chunk_size",		"8192",		PHP_INI_ALL, OnUpdateChunkSize,	chunk_size,		zend_memcache_globals,	memcache_globals)
PHP_INI_END()
/* }}} */

/* {{{ macros */
#define MMC_PREPARE_KEY(key, key_len) \
	php_strtr(key, key_len, "\t\r\n ", "____", 4); \

#if ZEND_DEBUG

#define MMC_DEBUG(info) \
{\
	mmc_debug info; \
}\

#else

#define MMC_DEBUG(info) \
{\
}\

#endif


/* }}} */

/* {{{ internal function protos */
static void _mmc_pool_list_dtor(zend_rsrc_list_entry * TSRMLS_DC);
static void _mmc_pserver_list_dtor(zend_rsrc_list_entry * TSRMLS_DC);

static void mmc_server_free(mmc_t * TSRMLS_DC);
static void mmc_server_disconnect(mmc_t * TSRMLS_DC);
static void mmc_server_deactivate(mmc_t * TSRMLS_DC);
static int mmc_server_store(mmc_t *, const char *, int TSRMLS_DC);

static unsigned int mmc_hash(const char *, int);
static int mmc_compress(char **, unsigned long *, const char *, int TSRMLS_DC);
static int mmc_uncompress(char **, unsigned long *, const char *, int);
static int mmc_get_pool(zval *, mmc_pool_t ** TSRMLS_DC);
static int mmc_open(mmc_t *, int, char **, int * TSRMLS_DC);
static int mmc_close(mmc_t * TSRMLS_DC);
static int mmc_readline(mmc_t * TSRMLS_DC);
static char * mmc_get_version(mmc_t * TSRMLS_DC);
static int mmc_str_left(char *, char *, int, int);
static int mmc_sendcmd(mmc_t *, const char *, int TSRMLS_DC);
static int mmc_parse_response(char *, int, char **, int *, int *, int *);
static int mmc_exec_retrieval_cmd_multi(mmc_pool_t *, zval *, zval ** TSRMLS_DC);
static int mmc_read_value(mmc_t *, char **, int *, zval ** TSRMLS_DC);
static int mmc_flush(mmc_t *, int TSRMLS_DC);
static void php_mmc_store(INTERNAL_FUNCTION_PARAMETERS, char *, int);
static int mmc_get_stats(mmc_t *, char *, int, int, zval * TSRMLS_DC);
static int mmc_incr_decr(mmc_t *, int, char *, int, int, long * TSRMLS_DC);
static void php_mmc_incr_decr(INTERNAL_FUNCTION_PARAMETERS, int);
static void php_mmc_connect(INTERNAL_FUNCTION_PARAMETERS, int);
/* }}} */

/* {{{ php_memcache_init_globals()
*/
static void php_memcache_init_globals(zend_memcache_globals *memcache_globals_p TSRMLS_DC)
{
	MEMCACHE_G(debug_mode)		  = 0;
	MEMCACHE_G(num_persistent)	  = 0;
	MEMCACHE_G(compression_level) = Z_DEFAULT_COMPRESSION;
}
/* }}} */

/* {{{ PHP_MINIT_FUNCTION
 */
PHP_MINIT_FUNCTION(memcache)
{
	zend_class_entry memcache_class_entry;
	INIT_CLASS_ENTRY(memcache_class_entry, "Memcache", php_memcache_class_functions);
	memcache_class_entry_ptr = zend_register_internal_class(&memcache_class_entry TSRMLS_CC);

	le_memcache_pool = zend_register_list_destructors_ex(_mmc_pool_list_dtor, NULL, "memcache connection", module_number);
	le_pmemcache = zend_register_list_destructors_ex(NULL, _mmc_pserver_list_dtor, "persistent memcache connection", module_number);

#ifdef ZTS
	ts_allocate_id(&memcache_globals_id, sizeof(zend_memcache_globals), (ts_allocate_ctor) php_memcache_init_globals, NULL);
#else
	php_memcache_init_globals(&memcache_globals TSRMLS_CC);
#endif

	REGISTER_LONG_CONSTANT("MEMCACHE_COMPRESSED", MMC_COMPRESSED, CONST_CS | CONST_PERSISTENT);
	REGISTER_INI_ENTRIES();

#if HAVE_MEMCACHE_SESSION
	REGISTER_LONG_CONSTANT("MEMCACHE_HAVE_SESSION", 1, CONST_CS | CONST_PERSISTENT);
	php_session_register_module(ps_memcache_ptr);
#else
	REGISTER_LONG_CONSTANT("MEMCACHE_HAVE_SESSION", 0, CONST_CS | CONST_PERSISTENT);
#endif

	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MSHUTDOWN_FUNCTION
 */
PHP_MSHUTDOWN_FUNCTION(memcache)
{
	UNREGISTER_INI_ENTRIES();
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_RINIT_FUNCTION
 */
PHP_RINIT_FUNCTION(memcache)
{
	MEMCACHE_G(debug_mode) = 0;
	return SUCCESS;
}
/* }}} */

/* {{{ PHP_MINFO_FUNCTION
 */
PHP_MINFO_FUNCTION(memcache)
{
	char buf[MAX_LENGTH_OF_LONG + 1];

	sprintf(buf, "%ld", MEMCACHE_G(num_persistent));

	php_info_print_table_start();
	php_info_print_table_header(2, "memcache support", "enabled");
	php_info_print_table_row(2, "Active persistent connections", buf);
	php_info_print_table_row(2, "Revision", "$Revision$");
	php_info_print_table_end();

	DISPLAY_INI_ENTRIES();
}
/* }}} */

/* ------------------
   internal functions
   ------------------ */

#if ZEND_DEBUG
static void mmc_debug(const char *format, ...) /* {{{ */
{
	TSRMLS_FETCH();

	if (MEMCACHE_G(debug_mode)) {
		char buffer[1024];
		va_list args;

		va_start(args, format);
		vsnprintf(buffer, sizeof(buffer)-1, format, args);
		va_end(args);
		buffer[sizeof(buffer)-1] = '\0';
		php_printf("%s<br />\n", buffer);
	}
}
/* }}} */
#endif

static void _mmc_pool_list_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC) /* {{{ */
{
	mmc_pool_free((mmc_pool_t *)rsrc->ptr TSRMLS_CC);
}
/* }}} */

static void _mmc_pserver_list_dtor(zend_rsrc_list_entry *rsrc TSRMLS_DC) /* {{{ */
{
	mmc_server_free((mmc_t *)rsrc->ptr TSRMLS_CC);
}
/* }}} */

mmc_t *mmc_server_new(char *host, int host_len, unsigned short port, int persistent, int timeout, int retry_interval TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;

	if (persistent) {
		mmc = malloc(sizeof(mmc_t));
		mmc->host = malloc(host_len + 1);
	}
	else {
		mmc = emalloc(sizeof(mmc_t));
		mmc->host = emalloc(host_len + 1);
	}

	mmc->stream = NULL;
	mmc->status = MMC_STATUS_DISCONNECTED;
	memset(&(mmc->outbuf), 0, sizeof(smart_str));

	memcpy(mmc->host, host, host_len);
	mmc->host[host_len] = '\0';
	mmc->port = port;

	mmc->persistent = persistent;
	if (persistent) {
		MEMCACHE_G(num_persistent)++;
	}

	mmc->timeout = timeout;
	mmc->retry_interval = retry_interval;
	mmc->failure_callback = NULL;

	return mmc;
}
/* }}} */

static void mmc_server_free(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	if (mmc->failure_callback != NULL) {
		zval_ptr_dtor(&(mmc->failure_callback));
	}

	if (mmc->persistent) {
		free(mmc->host);
		free(mmc);
		MEMCACHE_G(num_persistent)--;
	}
	else {
		if (mmc->stream != NULL) {
			php_stream_close(mmc->stream);
		}
		efree(mmc->host);
		efree(mmc);
	}
}
/* }}} */

int mmc_server_failure(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	switch (mmc->status) {
		case MMC_STATUS_DISCONNECTED:
			return 0;

		/* attempt reconnect of sockets in unknown state */
		case MMC_STATUS_UNKNOWN:
			mmc->status = MMC_STATUS_DISCONNECTED;
			return 0;
	}

	mmc_server_deactivate(mmc TSRMLS_CC);
	return 1;
}
/* }}} */

static int mmc_server_store(mmc_t *mmc, const char *request, int request_len TSRMLS_DC) { /* {{{ */
	int response_len;

	if (php_stream_write(mmc->stream, request, request_len) != request_len || (response_len = mmc_readline(mmc TSRMLS_CC)) < 0) {
		return -1;
	}

	if(mmc_str_left(mmc->inbuf, "STORED", response_len, sizeof("STORED") - 1)) {
		return 1;
	}

	/* return FALSE */
	if(mmc_str_left(mmc->inbuf, "NOT_STORED", response_len, sizeof("NOT_STORED") - 1)) {
		return 0;
	}

	/* return FALSE without failover */
	if (mmc_str_left(mmc->inbuf, "SERVER_ERROR out of memory", response_len, sizeof("SERVER_ERROR out of memory") - 1)) {
		return 0;
	}

	return -1;
}
/* }}} */

mmc_pool_t *mmc_pool_new() /* {{{ */
{
	mmc_pool_t *pool = emalloc(sizeof(mmc_pool_t));
	pool->num_servers = 0;
	pool->num_buckets = 0;
	pool->compress_threshold = 0;
	pool->min_compress_savings = MMC_DEFAULT_SAVINGS;
	return pool;
}
/* }}} */

void mmc_pool_free(mmc_pool_t *pool TSRMLS_DC) /* {{{ */
{
	int i;
	for (i=0; i<pool->num_servers; i++) {
		if (!pool->servers[i]->persistent) {
			mmc_server_free(pool->servers[i] TSRMLS_CC);
		}
		else if (pool->servers[i]->failure_callback != NULL) {
			zval_ptr_dtor(&(pool->servers[i]->failure_callback));
			pool->servers[i]->failure_callback = NULL;
		}
	}

	if (pool->num_servers) {
		efree(pool->servers);
		efree(pool->requests);
	}

	if (pool->num_buckets) {
		efree(pool->buckets);
	}

	efree(pool);
}
/* }}} */

void mmc_pool_add(mmc_pool_t *pool, mmc_t *mmc, unsigned int weight) /* {{{ */
{
	int i;

	/* add server and a preallocated request pointer */
	if (pool->num_servers) {
		pool->servers = erealloc(pool->servers, sizeof(mmc_t *) * (pool->num_servers + 1));
		pool->requests = erealloc(pool->requests, sizeof(mmc_t *) * (pool->num_servers + 1));
	}
	else {
		pool->servers = emalloc(sizeof(mmc_t *));
		pool->requests = emalloc(sizeof(mmc_t *));
	}

	pool->servers[pool->num_servers] = mmc;
	pool->num_servers++;

	/* add weight number of buckets for this server */
	if (pool->num_buckets) {
		pool->buckets = erealloc(pool->buckets, sizeof(mmc_t *) * (pool->num_buckets + weight));
	}
	else {
		pool->buckets = emalloc(sizeof(mmc_t *) * (pool->num_buckets + weight));
	}

	for (i=0; i<weight; i++) {
		pool->buckets[pool->num_buckets + i] = mmc;
	}
	pool->num_buckets += weight;
}
/* }}} */

mmc_t *mmc_pool_find(mmc_pool_t *pool, const char *key, int key_len TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;

	if (pool->num_servers > 1) {
		unsigned int hash = mmc_hash(key, key_len), i;
		mmc = pool->buckets[hash % pool->num_buckets];

		/* perform failover if needed */
		for (i=0; !mmc_open(mmc, 0, NULL, NULL TSRMLS_CC) && MEMCACHE_G(allow_failover) && i<MEMCACHE_G(max_failover_attempts); i++) {
			char *next_key = emalloc(key_len + MAX_LENGTH_OF_LONG + 1);
			int next_len = sprintf(next_key, "%d%s", i+1, key);
			MMC_DEBUG(("mmc_pool_find: failed to connect to server '%s:%d' status %d, trying next", mmc->host, mmc->port, mmc->status));

			hash += mmc_hash(next_key, next_len);
			mmc = pool->buckets[hash % pool->num_buckets];

			efree(next_key);
		}
	}
	else {
		mmc = pool->servers[0];
		mmc_open(mmc, 0, NULL, NULL TSRMLS_CC);
	}

	return mmc->status != MMC_STATUS_FAILED ? mmc : NULL;
}
/* }}} */

int mmc_pool_store(mmc_pool_t *pool, const char *command, int command_len, const char *key, int key_len, int flags, int expire, const char *value, int value_len TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;
	char *request;
	int request_len, result = -1;
	char *key_copy = NULL, *data = NULL;

	if (key_len > MMC_KEY_MAX_SIZE) {
		key = key_copy = estrndup(key, MMC_KEY_MAX_SIZE);
		key_len = MMC_KEY_MAX_SIZE;
	}

	/* autocompress large values */
	if (pool->compress_threshold && value_len >= pool->compress_threshold) {
		flags |= MMC_COMPRESSED;
	}

	if (flags & MMC_COMPRESSED) {
		unsigned long data_len;

		if (!mmc_compress(&data, &data_len, value, value_len TSRMLS_CC)) {
			return -1;
		}

		/* was enough space saved to motivate uncompress processing on get */
		if (data_len < value_len * (1 - pool->min_compress_savings)) {
			value = data;
			value_len = data_len;
		}
		else {
			flags &= ~MMC_COMPRESSED;
			efree(data);
			data = NULL;
		}
	}

	request = emalloc(
		command_len
		+ 1				/* space */
		+ key_len
		+ 1				/* space */
		+ MAX_LENGTH_OF_LONG
		+ 1 			/* space */
		+ MAX_LENGTH_OF_LONG
		+ 1 			/* space */
		+ MAX_LENGTH_OF_LONG
		+ sizeof("\r\n") - 1
		+ value_len
		+ sizeof("\r\n") - 1
		+ 1
		);

	request_len = sprintf(request, "%s %s %d %d %d\r\n", command, key, flags, expire, value_len);

	memcpy(request + request_len, value, value_len);
	request_len += value_len;

	memcpy(request + request_len, "\r\n", sizeof("\r\n") - 1);
	request_len += sizeof("\r\n") - 1;

	request[request_len] = '\0';

	while (result < 0 && (mmc = mmc_pool_find(pool, key, key_len TSRMLS_CC)) != NULL) {
		if ((result = mmc_server_store(mmc, request, request_len TSRMLS_CC)) < 0 && mmc_server_failure(mmc TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", mmc->host, mmc->port);
		}
	}

	if (key_copy != NULL) {
		efree(key_copy);
	}

	if (data != NULL) {
		efree(data);
	}

	efree(request);

	return result;
}
/* }}} */

static int mmc_compress(char **result, unsigned long *result_len, const char *data, int data_len TSRMLS_DC) /* {{{ */
{
	int status, level = MEMCACHE_G(compression_level);

	*result_len = data_len + (data_len / 1000) + 25 + 1; /* some magic from zlib.c */
	*result = (char *) emalloc(*result_len);

	if (!*result) {
		return 0;
	}

	if (level >= 0) {
		status = compress2((unsigned char *) *result, result_len, (unsigned const char *) data, data_len, level);
	} else {
		status = compress((unsigned char *) *result, result_len, (unsigned const char *) data, data_len);
	}

	if (status == Z_OK) {
		*result = erealloc(*result, *result_len + 1);
		(*result)[*result_len] = '\0';
		return 1;
	}

	switch (status) {
		case Z_MEM_ERROR:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "not enough memory to perform compression");
			break;
		case Z_BUF_ERROR:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "not enough room in the output buffer to perform compression");
			break;
		case Z_STREAM_ERROR:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid compression level");
			break;
		default:
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "unknown error during compression");
			break;
	}

	efree(*result);
	return 0;
}
/* }}}*/

static int mmc_uncompress(char **result, unsigned long *result_len, const char *data, int data_len) /* {{{ */
{
	int status;
	unsigned int factor = 1, maxfactor = 16;
	char *tmp1 = NULL;

	do {
		*result_len = (unsigned long)data_len * (1 << factor++);
		*result = (char *) erealloc(tmp1, *result_len);
		status = uncompress((unsigned char *) *result, result_len, (unsigned const char *) data, data_len);
		tmp1 = *result;
	} while (status == Z_BUF_ERROR && factor < maxfactor);

	if (status == Z_OK) {
		*result = erealloc(*result, *result_len + 1);
		(*result)[*result_len] = '\0';
		return 1;
	}

	efree(*result);
	return 0;
}
/* }}}*/

/**
 * New style crc32 hash, compatible with other clients
 */
static unsigned int mmc_hash(const char *key, int key_len) { /* {{{ */
	unsigned int crc = ~0;
	int i;

	for (i=0; i<key_len; i++) {
		CRC32(crc, key[i]);
	}

	crc = (~crc >> 16) & 0x7fff;
  	return crc ? crc : 1;
}
/* }}} */

static int mmc_get_pool(zval *id, mmc_pool_t **pool TSRMLS_DC) /* {{{ */
{
	zval **connection;
	int resource_type;

	if (Z_TYPE_P(id) != IS_OBJECT || zend_hash_find(Z_OBJPROP_P(id), "connection", sizeof("connection"), (void **) &connection) == FAILURE) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "cannot find connection identifier");
		return 0;
	}

	*pool = (mmc_pool_t *) zend_list_find(Z_LVAL_PP(connection), &resource_type);

	if (!*pool || resource_type != le_memcache_pool) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "connection identifier not found");
		return 0;
	}

	return Z_LVAL_PP(connection);
}
/* }}} */

static int _mmc_open(mmc_t *mmc, char **error_string, int *errnum TSRMLS_DC) /* {{{ */
{
	struct timeval tv;
	char *hostname = NULL, *hash_key = NULL, *errstr = NULL;
	int	hostname_len, err = 0;

	/* close open stream */
	if (mmc->stream != NULL) {
		mmc_server_disconnect(mmc TSRMLS_CC);
	}

	tv.tv_sec = mmc->timeout;
	tv.tv_usec = 0;

	if (mmc->port) {
		hostname_len = spprintf(&hostname, 0, "%s:%d", mmc->host, mmc->port);
	}
	else {
		hostname_len = spprintf(&hostname, 0, "%s", mmc->host);
	}

	if (mmc->persistent) {
		spprintf(&hash_key, 0, "memcache:%s", hostname);
	}

#if PHP_API_VERSION > 20020918
	mmc->stream = php_stream_xport_create( hostname, hostname_len,
										   ENFORCE_SAFE_MODE | REPORT_ERRORS,
										   STREAM_XPORT_CLIENT | STREAM_XPORT_CONNECT,
										   hash_key, &tv, NULL, &errstr, &err);
#else
	if (mmc->persistent) {
		switch(php_stream_from_persistent_id(hash_key, &(mmc->stream) TSRMLS_CC)) {
			case PHP_STREAM_PERSISTENT_SUCCESS:
				if (php_stream_eof(mmc->stream)) {
					php_stream_pclose(mmc->stream);
					mmc->stream = NULL;
					break;
				}
			case PHP_STREAM_PERSISTENT_FAILURE:
				break;
		}
	}

	if (!mmc->stream) {
		int socktype = SOCK_STREAM;
		mmc->stream = php_stream_sock_open_host(mmc->host, mmc->port, socktype, &tv, hash_key);
	}

#endif

	efree(hostname);
	if (mmc->persistent) {
		efree(hash_key);
	}

	if (!mmc->stream) {
		MMC_DEBUG(("_mmc_open: can't open socket to host"));
		mmc_server_deactivate(mmc TSRMLS_CC);

		if (errstr) {
			if (error_string) {
				*error_string = errstr;
			}
			else {
				efree(errstr);
			}
		}
		if (errnum) {
			*errnum = err;
		}

		return 0;
	}

	php_stream_auto_cleanup(mmc->stream);
	php_stream_set_option(mmc->stream, PHP_STREAM_OPTION_READ_TIMEOUT, 0, &tv);
	php_stream_set_option(mmc->stream, PHP_STREAM_OPTION_WRITE_BUFFER, PHP_STREAM_BUFFER_NONE, NULL);
	php_stream_set_chunk_size(mmc->stream, MEMCACHE_G(chunk_size));

	mmc->status = MMC_STATUS_CONNECTED;
	return 1;
}
/* }}} */

static int mmc_open(mmc_t *mmc, int force_connect, char **error_string, int *errnum TSRMLS_DC) /* {{{ */
{
	switch (mmc->status) {
		case MMC_STATUS_DISCONNECTED:
			return _mmc_open(mmc, error_string, errnum TSRMLS_CC);

		case MMC_STATUS_CONNECTED:
			return 1;

		case MMC_STATUS_UNKNOWN:
			/* check connection if needed */
			if (force_connect) {
				char *version;
				if ((version = mmc_get_version(mmc TSRMLS_CC)) == NULL && !_mmc_open(mmc, error_string, errnum TSRMLS_CC)) {
					break;
				}
				if (version) {
					efree(version);
				}
				mmc->status = MMC_STATUS_CONNECTED;
			}
			return 1;

		case MMC_STATUS_FAILED:
			if (mmc->retry_interval >= 0 && (long)time(NULL) >= mmc->failed + mmc->retry_interval) {
				if (_mmc_open(mmc, error_string, errnum TSRMLS_CC) /*&& mmc_flush(mmc, 0 TSRMLS_CC) > 0*/) {
					return 1;
				}
				mmc_server_deactivate(mmc TSRMLS_CC);
			}
			break;
	}
	return 0;
}
/* }}} */

static void mmc_server_disconnect(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	if (mmc->stream != NULL) {
		if (mmc->persistent) {
			php_stream_pclose(mmc->stream);
		}
		else {
			php_stream_close(mmc->stream);
		}
		mmc->stream = NULL;
	}
	mmc->status = MMC_STATUS_DISCONNECTED;
}
/* }}} */

static void mmc_server_deactivate(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	mmc_server_disconnect(mmc TSRMLS_CC);
	mmc->status = MMC_STATUS_FAILED;
	mmc->failed = (long)time(NULL);

	if (mmc->failure_callback != NULL) {
		zval *retval;
		zval **params[2];
		zval *host, *port;

		params[0] = &host;
		params[1] = &port;

		MAKE_STD_ZVAL(host);
		MAKE_STD_ZVAL(port);

		ZVAL_STRING(host, mmc->host, 1);
		ZVAL_LONG(port, mmc->port);

		call_user_function_ex(EG(function_table), NULL, mmc->failure_callback, &retval, 2, params, 0, NULL TSRMLS_CC);

		zval_ptr_dtor(&host);
		zval_ptr_dtor(&port);
		zval_ptr_dtor(&retval);
	}
}
/* }}} */

static int mmc_close(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	MMC_DEBUG(("mmc_close: closing connection to server"));
	if (!mmc->persistent) {
		mmc_server_disconnect(mmc TSRMLS_CC);
	}
	return 1;
}
/* }}} */

static int mmc_readline(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	char *response;
	size_t response_len;

	if (mmc->stream == NULL) {
		MMC_DEBUG(("mmc_readline: socket is already closed"));
		return -1;
	}

	response = php_stream_get_line(mmc->stream, mmc->inbuf, MMC_BUF_SIZE, &response_len);
	if (response) {
		MMC_DEBUG(("mmc_readline: read data:"));
		MMC_DEBUG(("mmc_readline:---"));
		MMC_DEBUG(("%s", response));
		MMC_DEBUG(("mmc_readline:---"));
		return response_len;
	}

	MMC_DEBUG(("mmc_readline: cannot read a line from the server"));
	return -1;
}
/* }}} */

static char *mmc_get_version(mmc_t *mmc TSRMLS_DC) /* {{{ */
{
	char *version_str;
	int response_len;

	if (mmc_sendcmd(mmc, "version", sizeof("version") - 1 TSRMLS_CC) < 0) {
		return NULL;
	}

	if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0) {
		return NULL;
	}

	if (mmc_str_left(mmc->inbuf, "VERSION ", response_len, sizeof("VERSION ") - 1)) {
		version_str = estrndup(mmc->inbuf + sizeof("VERSION ") - 1, response_len - (sizeof("VERSION ") - 1) - (sizeof("\r\n") - 1) );
		return version_str;
	}

	MMC_DEBUG(("mmc_get_version: data is not valid version string"));
	return NULL;
}
/* }}} */

static int mmc_str_left(char *haystack, char *needle, int haystack_len, int needle_len) /* {{{ */
{
	char *found;

	found = php_memnstr(haystack, needle, needle_len, haystack + haystack_len);
	if ((found - haystack) == 0) {
		return 1;
	}
	return 0;
}
/* }}} */

static int mmc_sendcmd(mmc_t *mmc, const char *cmd, int cmdlen TSRMLS_DC) /* {{{ */
{
	char *command;
	int command_len;

	if (!mmc || !cmd) {
		return -1;
	}

	MMC_DEBUG(("mmc_sendcmd: sending command '%s'", cmd));

	command = emalloc(cmdlen + sizeof("\r\n"));
	memcpy(command, cmd, cmdlen);
	memcpy(command + cmdlen, "\r\n", sizeof("\r\n") - 1);
	command_len = cmdlen + sizeof("\r\n") - 1;
	command[command_len] = '\0';

	if (php_stream_write(mmc->stream, command, command_len) != command_len) {
		MMC_DEBUG(("mmc_sendcmd: write failed"));
		efree(command);
		return -1;
	}
	efree(command);

	return 1;
}
/* }}}*/

static int mmc_parse_response(char *response, int response_len, char **key, int *key_len, int *flags, int *value_len) /* {{{ */
{
	int i=0, n=0;
	int spaces[3];

	if (!response || response_len <= 0) {
		return -1;
	}

	MMC_DEBUG(("mmc_parse_response: got response '%s'", response));

	for (i=0, n=0; i < response_len && n < 3; i++) {
		if (response[i] == ' ') {
			spaces[n++] = i;
		}
	}

	MMC_DEBUG(("mmc_parse_response: found %d spaces", n));

	if (n < 3) {
		return -1;
	}

	if (key != NULL) {
		int len = spaces[1] - spaces[0] - 1;

		*key = emalloc(len + 1);
		*key_len = len;

		memcpy(*key, response + spaces[0] + 1, len);
		(*key)[len] = '\0';
	}

	*flags = atoi(response + spaces[1]);
	*value_len = atoi(response + spaces[2]);

	if (*flags < 0 || *value_len < 0) {
		return -1;
	}

	MMC_DEBUG(("mmc_parse_response: 1st space is at %d position", spaces[1]));
	MMC_DEBUG(("mmc_parse_response: 2nd space is at %d position", spaces[2]));
	MMC_DEBUG(("mmc_parse_response: flags = %d", *flags));
	MMC_DEBUG(("mmc_parse_response: value_len = %d ", *value_len));

	return 1;
}
/* }}} */

int mmc_exec_retrieval_cmd(mmc_pool_t *pool, const char *key, int key_len, zval **return_value TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;
	char *command;
	int result = -1, command_len, response_len;

	MMC_DEBUG(("mmc_exec_retrieval_cmd: key '%s'", key));

	command_len = spprintf(&command, 0, "get %s", key);

	while (result < 0 && (mmc = mmc_pool_find(pool, key, key_len TSRMLS_CC)) != NULL) {
		MMC_DEBUG(("mmc_exec_retrieval_cmd: found server '%s:%d' for key '%s'", mmc->host, mmc->port, key));

		/* send command and read value */
		if ((result = mmc_sendcmd(mmc, command, command_len TSRMLS_CC)) > 0 &&
			(result = mmc_read_value(mmc, NULL, NULL, return_value TSRMLS_CC)) >= 0) {

			/* not found */
			if (result == 0) {
				ZVAL_FALSE(*return_value);
			}
			/* read "END" */
			else if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0 || !mmc_str_left(mmc->inbuf, "END", response_len, sizeof("END")-1)) {
				result = -1;
			}
		}

		if (result < 0 && mmc_server_failure(mmc TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", mmc->host, mmc->port);
		}
	}

	efree(command);
	return result;
}
/* }}} */

static int mmc_exec_retrieval_cmd_multi(mmc_pool_t *pool, zval *keys, zval **return_value TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;
	HashPosition pos;
	zval **key, *value;
	char *result_key, *str_key;
	int	i = 0, j, num_requests, result, result_status, result_key_len, key_len;

	array_init(*return_value);

	/* until no retrival errors or all servers have failed */
	do {
		result_status = num_requests = 0;
		zend_hash_internal_pointer_reset_ex(Z_ARRVAL_P(keys), &pos);

		/* first pass to build requests for each server */
		while (zend_hash_get_current_data_ex(Z_ARRVAL_P(keys), (void **) &key, &pos) == SUCCESS) {
			if (Z_TYPE_PP(key) == IS_STRING) {
				key_len = Z_STRLEN_PP(key);
				str_key = estrndup(Z_STRVAL_PP(key), key_len);
			} else {
				zval *tmp;

				ALLOC_ZVAL(tmp);
				*tmp = **key;
				zval_copy_ctor(tmp);
				INIT_PZVAL(tmp);
				convert_to_string(tmp);

				key_len = Z_STRLEN_P(tmp);
				str_key = estrndup(Z_STRVAL_P(tmp), key_len);
				zval_dtor(tmp);
				FREE_ZVAL(tmp);
			}

			MMC_PREPARE_KEY(str_key, Z_STRLEN_PP(key));

			/* schedule key if first round or if missing from result */
			if ((!i || !zend_hash_exists(Z_ARRVAL_PP(return_value), str_key, key_len)) &&
				(mmc = mmc_pool_find(pool, str_key, key_len TSRMLS_CC)) != NULL) {
				if (!(mmc->outbuf.len)) {
					smart_str_appendl(&(mmc->outbuf), "get", sizeof("get")-1);
					pool->requests[num_requests++] = mmc;
				}

				smart_str_appendl(&(mmc->outbuf), " ", 1);
				smart_str_appendl(&(mmc->outbuf), str_key, key_len);
				MMC_DEBUG(("mmc_exec_retrieval_cmd_multi: scheduled key '%s' for '%s:%d' request length '%d'", str_key, mmc->host, mmc->port, mmc->outbuf.len));
			}
			efree(str_key);
			zend_hash_move_forward_ex(Z_ARRVAL_P(keys), &pos);
		}

		/* second pass to send requests in parallel */
		for (j=0; j<num_requests; j++) {
			smart_str_0(&(pool->requests[j]->outbuf));

			if ((result = mmc_sendcmd(pool->requests[j], pool->requests[j]->outbuf.c, pool->requests[j]->outbuf.len TSRMLS_CC)) < 0) {
				if (mmc_server_failure(pool->requests[j] TSRMLS_CC)) {
					php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->requests[j]->host, pool->requests[j]->port);
				}
				result_status = result;
			}
		}

		/* third pass to read responses */
		for (j=0; j<num_requests; j++) {
			if (pool->requests[j]->status != MMC_STATUS_FAILED) {
				for (value = NULL; (result = mmc_read_value(pool->requests[j], &result_key, &result_key_len, &value TSRMLS_CC)) > 0; value = NULL) {
					add_assoc_zval_ex(*return_value, result_key, result_key_len + 1, value);
					efree(result_key);
				}

				/* check for server failure */
				if (result < 0) {
					if (mmc_server_failure(pool->requests[j] TSRMLS_CC)) {
						php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->requests[j]->host, pool->requests[j]->port);
					}
					result_status = result;
				}
			}

			smart_str_free(&(pool->requests[j]->outbuf));
		}
	} while (result_status < 0 && MEMCACHE_G(allow_failover) && i++ < MEMCACHE_G(max_failover_attempts));

	return result_status;
}
/* }}} */

static int mmc_read_value(mmc_t *mmc, char **key, int *key_len, zval **value TSRMLS_DC) /* {{{ */
{
	int response_len, flags, data_len, i, size;
	char *data;

	/* read "VALUE <key> <flags> <bytes>\r\n" header line */
	if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0) {
		MMC_DEBUG(("failed to read the server's response"));
		return -1;
	}

	/* reached the end of the data */
	if (mmc_str_left(mmc->inbuf, "END", response_len, sizeof("END") - 1)) {
		return 0;
	}

	if (mmc_parse_response(mmc->inbuf, response_len, key, key_len, &flags, &data_len) < 0) {
		return -1;
	}

	MMC_DEBUG(("mmc_read_value: data len is %d bytes", data_len));

	/* data_len + \r\n + \0 */
	data = emalloc(data_len + 3);

	for (i=0; i<data_len+2; i+=size) {
		if ((size = php_stream_read(mmc->stream, data + i, data_len + 2 - i)) == 0) {
			MMC_DEBUG(("incomplete data block (expected %d, got %d)", (data_len + 2), i));
			if (key) {
				efree(*key);
			}
			efree(data);
			return -1;
		}
	}

	data[data_len] = '\0';
	if (!data) {
		if (*value == NULL) {
			MAKE_STD_ZVAL(*value);
		}
		ZVAL_EMPTY_STRING(*value);
		efree(data);
		return 1;
	}

	if (flags & MMC_COMPRESSED) {
		char *result_data;
		unsigned long result_len = 0;

		if (!mmc_uncompress(&result_data, &result_len, data, data_len)) {
			MMC_DEBUG(("Error when uncompressing data"));
			if (key) {
				efree(*key);
			}
			efree(data);
			return -1;
		}

		efree(data);
		data = result_data;
		data_len = result_len;
	}

	MMC_DEBUG(("mmc_read_value: data '%s'", data));

	if (*value == NULL) {
		MAKE_STD_ZVAL(*value);
	}

	if (flags & MMC_SERIALIZED) {
		const char *tmp = data;
		php_unserialize_data_t var_hash;
		PHP_VAR_UNSERIALIZE_INIT(var_hash);

		if (!php_var_unserialize(value, (const unsigned char **)&tmp, (const unsigned char *)(tmp + data_len), &var_hash TSRMLS_CC)) {
			MMC_DEBUG(("Error at offset %d of %d bytes", tmp - data, data_len));
			PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
			if (key) {
				efree(*key);
			}
			efree(data);
			return -1;
		}

		PHP_VAR_UNSERIALIZE_DESTROY(var_hash);
		efree(data);
	}
	else {
		ZVAL_STRINGL(*value, data, data_len, 0);
	}

	return 1;
}
/* }}} */

int mmc_delete(mmc_t *mmc, const char *key, int key_len, int time TSRMLS_DC) /* {{{ */
{
	char *command;
	int command_len, response_len;

	command_len = spprintf(&command, 0, "delete %s %d", key, time);

	MMC_DEBUG(("mmc_delete: trying to delete '%s'", key));

	if (mmc_sendcmd(mmc, command, command_len TSRMLS_CC) < 0) {
		efree(command);
		return -1;
	}
	efree(command);

	if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0){
		MMC_DEBUG(("failed to read the server's response"));
		return -1;
	}

	MMC_DEBUG(("mmc_delete: server's response is '%s'", mmc->inbuf));

	if(mmc_str_left(mmc->inbuf,"DELETED", response_len, sizeof("DELETED") - 1)) {
		return 1;
	}

	if(mmc_str_left(mmc->inbuf,"NOT_FOUND", response_len, sizeof("NOT_FOUND") - 1)) {
		return 0;
	}

	MMC_DEBUG(("failed to delete item"));
	return -1;
}
/* }}} */

static int mmc_flush(mmc_t *mmc, int timestamp TSRMLS_DC) /* {{{ */
{
	char *command;
	int command_len, response_len;

	MMC_DEBUG(("mmc_flush: flushing the cache"));

	if (timestamp) {
		command_len = spprintf(&command, 0, "flush_all %d", timestamp);
	}
	else {
		command_len = spprintf(&command, 0, "flush_all");
	}

	if (mmc_sendcmd(mmc, command, command_len TSRMLS_CC) < 0) {
		efree(command);
		return -1;
	}
	efree(command);

	/* get server's response */
	if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0){
		return -1;
	}

	MMC_DEBUG(("mmc_flush: server's response is '%s'", mmc->inbuf));

	if(mmc_str_left(mmc->inbuf, "OK", response_len, sizeof("OK") - 1)) {
		return 1;
	}

	MMC_DEBUG(("failed to flush server's cache"));
	return -1;
}
/* }}} */

/*
 * STAT 6:chunk_size 64
 */
static int mmc_stats_parse_stat(char *start, char *end, zval *result TSRMLS_DC)  /* {{{ */
{
	char *space, *colon, *key;
	long index = 0;

	/* find space delimiting key and value */
	if ((space = php_memnstr(start, " ", 1, end)) == NULL) {
		return 0;
	}

	/* find colon delimiting subkeys */
	if ((colon = php_memnstr(start, ":", 1, space - 1)) != NULL) {
		zval *element, **elem;
		key = estrndup(start, colon - start);

		/* find existing or create subkey array in result */
		if ((is_numeric_string(key, colon - start, &index, NULL, 0) &&
			zend_hash_index_find(Z_ARRVAL_P(result), index, (void **) &elem) != FAILURE) ||
			zend_hash_find(Z_ARRVAL_P(result), key, colon - start + 1, (void **) &elem) != FAILURE) {
			element = *elem;
		}
		else {
			MAKE_STD_ZVAL(element);
			array_init(element);
			add_assoc_zval_ex(result, key, colon - start + 1, element);
		}

		efree(key);
		return mmc_stats_parse_stat(colon + 1, end, element TSRMLS_CC);
	}

	/* no more subkeys, add value under last subkey */
	key = estrndup(start, space - start);
	add_assoc_stringl_ex(result, key, space - start + 1, space + 1, end - space, 1);
	efree(key);

	return 1;
}
/* }}} */

/*
 * ITEM test_key [3 b; 1157099416 s]
 */
static int mmc_stats_parse_item(char *start, char *end, zval *result TSRMLS_DC)  /* {{{ */
{
	char *space, *value, *value_end, *key;
	zval *element;

	/* find space delimiting key and value */
	if ((space = php_memnstr(start, " ", 1, end)) == NULL) {
		return 0;
	}

	MAKE_STD_ZVAL(element);
	array_init(element);

	/* parse each contained value */
	for (value = php_memnstr(space, "[", 1, end); value != NULL && value <= end; value = php_memnstr(value + 1, ";", 1, end)) {
		do {
			value++;
		} while (*value == ' ' && value <= end);

		if (value <= end && (value_end = php_memnstr(value, " ", 1, end)) != NULL && value_end <= end) {
			add_next_index_stringl(element, value, value_end - value, 1);
		}
	}

	/* add parsed values under key */
	key = estrndup(start, space - start);
	add_assoc_zval_ex(result, key, space - start + 1, element);
	efree(key);

	return 1;
}
/* }}} */

static int mmc_stats_parse_generic(char *start, char *end, zval *result TSRMLS_DC)  /* {{{ */
{
	char *space, *key;

	/* "stats maps" returns "\n" delimited lines, other commands uses "\r\n" */
	if (*end == '\r') {
		end--;
	}

	if (start <= end) {
		if ((space = php_memnstr(start, " ", 1, end)) != NULL) {
			key = estrndup(start, space - start);
			add_assoc_stringl_ex(result, key, space - start + 1, space + 1, end - space, 1);
			efree(key);
		}
		else {
			add_next_index_stringl(result, start, end - start, 1);
		}
	}

	return 1;
}
/* }}} */

static int mmc_get_stats(mmc_t *mmc, char *type, int slabid, int limit, zval *result TSRMLS_DC) /* {{{ */
{
	char *command;
	int command_len, response_len;

	if (slabid) {
		command_len = spprintf(&command, 0, "stats %s %d %d", type, slabid, limit);
	}
	else if (type) {
		command_len = spprintf(&command, 0, "stats %s", type);
	}
	else {
		command_len = spprintf(&command, 0, "stats");
	}

	if (mmc_sendcmd(mmc, command, command_len TSRMLS_CC) < 0) {
		efree(command);
		return -1;
	}

	efree(command);
	array_init(result);

	while ((response_len = mmc_readline(mmc TSRMLS_CC)) >= 0) {
		if (mmc_str_left(mmc->inbuf, "ERROR", response_len, sizeof("ERROR") - 1) ||
			mmc_str_left(mmc->inbuf, "CLIENT_ERROR", response_len, sizeof("CLIENT_ERROR") - 1) ||
			mmc_str_left(mmc->inbuf, "SERVER_ERROR", response_len, sizeof("SERVER_ERROR") - 1)) {

			zend_hash_destroy(Z_ARRVAL_P(result));
			FREE_HASHTABLE(Z_ARRVAL_P(result));

			ZVAL_FALSE(result);
			return 0;
		}
		else if (mmc_str_left(mmc->inbuf, "RESET", response_len, sizeof("RESET") - 1)) {
			zend_hash_destroy(Z_ARRVAL_P(result));
			FREE_HASHTABLE(Z_ARRVAL_P(result));

			ZVAL_TRUE(result);
			return 1;
		}
		else if (mmc_str_left(mmc->inbuf, "ITEM ", response_len, sizeof("ITEM ") - 1)) {
			if (!mmc_stats_parse_item(mmc->inbuf + (sizeof("ITEM ") - 1), mmc->inbuf + response_len - sizeof("\r\n"), result TSRMLS_CC)) {
				zend_hash_destroy(Z_ARRVAL_P(result));
				FREE_HASHTABLE(Z_ARRVAL_P(result));
				return -1;
			}
		}
		else if (mmc_str_left(mmc->inbuf, "STAT ", response_len, sizeof("STAT ") - 1)) {
			if (!mmc_stats_parse_stat(mmc->inbuf + (sizeof("STAT ") - 1), mmc->inbuf + response_len - sizeof("\r\n"), result TSRMLS_CC)) {
				zend_hash_destroy(Z_ARRVAL_P(result));
				FREE_HASHTABLE(Z_ARRVAL_P(result));
				return -1;
			}
		}
		else if (mmc_str_left(mmc->inbuf, "END", response_len, sizeof("END") - 1)) {
			break;
		}
		else if (!mmc_stats_parse_generic(mmc->inbuf, mmc->inbuf + response_len - sizeof("\n"), result TSRMLS_CC)) {
			zend_hash_destroy(Z_ARRVAL_P(result));
			FREE_HASHTABLE(Z_ARRVAL_P(result));
			return -1;
		}
	}

	if (response_len < 0) {
		zend_hash_destroy(Z_ARRVAL_P(result));
		FREE_HASHTABLE(Z_ARRVAL_P(result));
		return -1;
	}

	return 1;
}
/* }}} */

static int mmc_incr_decr(mmc_t *mmc, int cmd, char *key, int key_len, int value, long *number TSRMLS_DC) /* {{{ */
{
	char *command;
	int  command_len, response_len;

	if (cmd > 0) {
		command_len = spprintf(&command, 0, "incr %s %d", key, value);
	}
	else {
		command_len = spprintf(&command, 0, "decr %s %d", key, value);
	}

	if (mmc_sendcmd(mmc, command, command_len TSRMLS_CC) < 0) {
		efree(command);
		return -1;
	}
	efree(command);

	if ((response_len = mmc_readline(mmc TSRMLS_CC)) < 0) {
		MMC_DEBUG(("failed to read the server's response"));
		return -1;
	}

	MMC_DEBUG(("mmc_incr_decr: server's answer is: '%s'", mmc->inbuf));
	if (mmc_str_left(mmc->inbuf, "NOT_FOUND", response_len, sizeof("NOT_FOUND") - 1)) {
		MMC_DEBUG(("failed to %sement variable - item with such key not found", cmd > 0 ? "incr" : "decr"));
		return 0;
	}
	else if (mmc_str_left(mmc->inbuf, "ERROR", response_len, sizeof("ERROR") - 1)) {
		MMC_DEBUG(("failed to %sement variable - an error occured", cmd > 0 ? "incr" : "decr"));
		return -1;
	}
	else if (mmc_str_left(mmc->inbuf, "CLIENT_ERROR", response_len, sizeof("CLIENT_ERROR") - 1)) {
		MMC_DEBUG(("failed to %sement variable - client error occured", cmd > 0 ? "incr" : "decr"));
		return -1;
	}
	else if (mmc_str_left(mmc->inbuf, "SERVER_ERROR", response_len, sizeof("SERVER_ERROR") - 1)) {
		MMC_DEBUG(("failed to %sement variable - server error occured", cmd > 0 ? "incr" : "decr"));
		return -1;
	}

	*number = (long)atol(mmc->inbuf);
	return 1;
}
/* }}} */

static void php_mmc_store(INTERNAL_FUNCTION_PARAMETERS, char *command, int command_len) /* {{{ */
{
	mmc_pool_t *pool;
	zval *var, *mmc_object = getThis();

	int result, value_len, key_len;
	char *value, *key_tmp, *key;
	long flags = 0, expire = 0;

	php_serialize_data_t var_hash;
	smart_str buf = {0};

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Osz|ll", &mmc_object, memcache_class_entry_ptr, &key_tmp, &key_len, &var, &flags, &expire) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "sz|ll", &key_tmp, &key_len, &var, &flags, &expire) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC) || !pool->num_servers) {
		RETURN_FALSE;
	}

	switch (Z_TYPE_P(var)) {
		case IS_STRING:
			value = Z_STRVAL_P(var);
			value_len = Z_STRLEN_P(var);
			break;

		case IS_LONG:
		case IS_DOUBLE:
		case IS_BOOL:
			convert_to_string(var);
			value = Z_STRVAL_P(var);
			value_len = Z_STRLEN_P(var);
			break;

		default:
			PHP_VAR_SERIALIZE_INIT(var_hash);
			php_var_serialize(&buf, &var, &var_hash TSRMLS_CC);
			PHP_VAR_SERIALIZE_DESTROY(var_hash);

			if (!buf.c) {
				/* you're trying to save null or something went really wrong */
				RETURN_FALSE;
			}

			value = buf.c;
			value_len = buf.len;
			flags |= MMC_SERIALIZED;
			break;
	}

	key = estrndup(key_tmp, key_len);
	MMC_PREPARE_KEY(key, key_len);
	
	result = mmc_pool_store(pool, command, command_len, key, key_len, flags, expire, value, value_len TSRMLS_CC);
	efree(key);

	if (flags & MMC_SERIALIZED) {
		smart_str_free(&buf);
	}

	if (result > 0) {
		RETURN_TRUE;
	}

	RETURN_FALSE;
}
/* }}} */

static void php_mmc_incr_decr(INTERNAL_FUNCTION_PARAMETERS, int cmd) /* {{{ */
{
	mmc_t *mmc;
	mmc_pool_t *pool;
	int result = -1, key_len;
	long value = 1, number;
	char *key;
	zval *mmc_object = getThis();

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Os|l", &mmc_object, memcache_class_entry_ptr, &key, &key_len, &value) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &key, &key_len, &value) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC) || !pool->num_servers) {
		RETURN_FALSE;
	}

	MMC_PREPARE_KEY(key, key_len);

	while (result < 0 && (mmc = mmc_pool_find(pool, key, key_len TSRMLS_CC)) != NULL) {
		if ((result = mmc_incr_decr(mmc, cmd, key, key_len, value, &number TSRMLS_CC)) < 0 && mmc_server_failure(mmc TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", mmc->host, mmc->port);
		}
	}

	if (result > 0) {
		RETURN_LONG(number);
	}
	RETURN_FALSE;
}
/* }}} */

static void php_mmc_connect (INTERNAL_FUNCTION_PARAMETERS, int persistent) /* {{{ */
{
	zval **connection, *mmc_object = getThis();
	mmc_t *mmc = NULL;
	mmc_pool_t *pool;
	int resource_type, host_len, errnum = 0, list_id;
	char *host, *error_string = NULL;
	long port = MEMCACHE_G(default_port), timeout = MMC_DEFAULT_TIMEOUT;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|ll", &host, &host_len, &port, &timeout) == FAILURE) {
		return;
	}

	/* initialize and connect server struct */
	if (persistent) {
		mmc = mmc_find_persistent(host, host_len, port, timeout, MMC_DEFAULT_RETRY TSRMLS_CC);
	}
	else {
		MMC_DEBUG(("php_mmc_connect: creating regular connection"));
		mmc = mmc_server_new(host, host_len, port, 0, timeout, MMC_DEFAULT_RETRY TSRMLS_CC);
	}

	if (!mmc_open(mmc, 1, &error_string, &errnum TSRMLS_CC)) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "Can't connect to %s:%ld, %s (%d)", host, port, error_string ? error_string : "Unknown error", errnum);
		if (!persistent) {
			mmc_server_free(mmc TSRMLS_CC);
		}
		if (error_string) {
			efree(error_string);
		}
		RETURN_FALSE;
	}

	/* initialize pool and object if need be */
	if (!mmc_object) {
		pool = mmc_pool_new();
		mmc_pool_add(pool, mmc, 1);

		object_init_ex(return_value, memcache_class_entry_ptr);
		list_id = zend_list_insert(pool, le_memcache_pool);
		add_property_resource(return_value, "connection", list_id);
	}
	else if (zend_hash_find(Z_OBJPROP_P(mmc_object), "connection", sizeof("connection"), (void **) &connection) != FAILURE) {
		pool = (mmc_pool_t *) zend_list_find(Z_LVAL_PP(connection), &resource_type);
		if (!pool || resource_type != le_memcache_pool) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "connection identifier not found");
			RETURN_FALSE;
		}

		mmc_pool_add(pool, mmc, 1);
		RETURN_TRUE;
	}
	else {
		pool = mmc_pool_new();
		mmc_pool_add(pool, mmc, 1);

		list_id = zend_list_insert(pool, le_memcache_pool);
		add_property_resource(mmc_object, "connection", list_id);
		RETURN_TRUE;
	}
}
/* }}} */

/* ----------------
   module functions
   ---------------- */

/* {{{ proto object memcache_connect( string host [, int port [, int timeout ] ])
   Connects to server and returns a Memcache object */
PHP_FUNCTION(memcache_connect)
{
	php_mmc_connect(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto object memcache_pconnect( string host [, int port [, int timeout ] ])
   Connects to server and returns a Memcache object */
PHP_FUNCTION(memcache_pconnect)
{
	php_mmc_connect(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ proto bool memcache_add_server( string host [, int port [, bool persistent [, int weight [, int timeout [, int retry_interval [, bool status [, callback failure_callback ] ] ] ] ] ] ])
   Adds a connection to the pool. The order in which this function is called is significant */
PHP_FUNCTION(memcache_add_server)
{
	zval **connection, *mmc_object = getThis(), *failure_callback = NULL;
	mmc_pool_t *pool;
	mmc_t *mmc;
	long port = MEMCACHE_G(default_port), weight = 1, timeout = MMC_DEFAULT_TIMEOUT, retry_interval = MMC_DEFAULT_RETRY;
	zend_bool persistent = 1, status = 1;
	int resource_type, host_len, list_id;
	char *host, *failure_callback_name;

	if (mmc_object) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lblllbz", &host, &host_len, &port, &persistent, &weight, &timeout, &retry_interval, &status, &failure_callback) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Os|lblllbz", &mmc_object, memcache_class_entry_ptr, &host, &host_len, &port, &persistent, &weight, &timeout, &retry_interval, &status, &failure_callback) == FAILURE) {
			return;
		}
	}

	if (weight < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "weight must be a positive integer");
		RETURN_FALSE;
	}

	if (failure_callback != NULL && Z_TYPE_P(failure_callback) != IS_NULL) {
		if (!zend_is_callable(failure_callback, 0, &failure_callback_name)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid failure callback '%s' passed", failure_callback_name);
			efree(failure_callback_name);
			RETURN_FALSE;
		}
		efree(failure_callback_name);
	}

	/* lazy initialization of server struct */
	if (persistent) {
		mmc = mmc_find_persistent(host, host_len, port, timeout, retry_interval TSRMLS_CC);
	}
	else {
		MMC_DEBUG(("memcache_add_server: initializing regular struct"));
		mmc = mmc_server_new(host, host_len, port, 0, timeout, retry_interval TSRMLS_CC);
	}

	/* add server in failed mode */
	if (!status) {
		mmc->status = MMC_STATUS_FAILED;
	}

	if (failure_callback != NULL && Z_TYPE_P(failure_callback) != IS_NULL) {
		ZVAL_ADDREF(failure_callback);
		mmc->failure_callback = failure_callback;
	}

	/* initialize pool if need be */
	if (zend_hash_find(Z_OBJPROP_P(mmc_object), "connection", sizeof("connection"), (void **) &connection) == FAILURE) {
		pool = mmc_pool_new();
		list_id = zend_list_insert(pool, le_memcache_pool);
		add_property_resource(mmc_object, "connection", list_id);
	}
	else {
		pool = (mmc_pool_t *) zend_list_find(Z_LVAL_PP(connection), &resource_type);
		if (!pool || resource_type != le_memcache_pool) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "connection identifier not found");
			RETURN_FALSE;
		}
	}

	mmc_pool_add(pool, mmc, weight);
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool memcache_set_server_params( string host [, int port [, int timeout [, int retry_interval [, bool status [, callback failure_callback ] ] ] ] ])
   Changes server parameters at runtime */
PHP_FUNCTION(memcache_set_server_params)
{
	zval *mmc_object = getThis(), *failure_callback = NULL;
	mmc_pool_t *pool;
	mmc_t *mmc = NULL;
	long port = MEMCACHE_G(default_port), timeout = MMC_DEFAULT_TIMEOUT, retry_interval = MMC_DEFAULT_RETRY;
	zend_bool status = 1;
	int host_len, i;
	char *host, *failure_callback_name;

	if (mmc_object) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|lllbz", &host, &host_len, &port, &timeout, &retry_interval, &status, &failure_callback) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Os|lllbz", &mmc_object, memcache_class_entry_ptr, &host, &host_len, &port, &timeout, &retry_interval, &status, &failure_callback) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (!strcmp(pool->servers[i]->host, host) && pool->servers[i]->port == port) {
			mmc = pool->servers[i];
			break;
		}
	}

	if (!mmc) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "server not found in pool");
		RETURN_FALSE;
	}

	if (failure_callback != NULL && Z_TYPE_P(failure_callback) != IS_NULL) {
		if (!zend_is_callable(failure_callback, 0, &failure_callback_name)) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "invalid failure callback '%s' passed", failure_callback_name);
			efree(failure_callback_name);
			RETURN_FALSE;
		}
		efree(failure_callback_name);
	}

	mmc->timeout = timeout;
	mmc->retry_interval = retry_interval;

	if (!status) {
		mmc->status = MMC_STATUS_FAILED;
	}
	else if (mmc->status == MMC_STATUS_FAILED) {
		mmc->status = MMC_STATUS_DISCONNECTED;
	}

	if (failure_callback != NULL) {
		if (mmc->failure_callback != NULL) {
			zval_ptr_dtor(&(mmc->failure_callback));
		}

		if (Z_TYPE_P(failure_callback) != IS_NULL) {
			ZVAL_ADDREF(failure_callback);
			mmc->failure_callback = failure_callback;
		}
		else {
			mmc->failure_callback = NULL;
		}
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int memcache_get_server_status( string host [, int port ])
   Returns server status (0 if server is failed, otherwise non-zero) */
PHP_FUNCTION(memcache_get_server_status)
{
	zval *mmc_object = getThis();
	mmc_pool_t *pool;
	mmc_t *mmc = NULL;
	long port = MEMCACHE_G(default_port);
	int host_len, i;
	char *host;

	if (mmc_object) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &host, &host_len, &port) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Os|l", &mmc_object, memcache_class_entry_ptr, &host, &host_len, &port) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (!strcmp(pool->servers[i]->host, host) && pool->servers[i]->port == port) {
			mmc = pool->servers[i];
			break;
		}
	}

	if (!mmc) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "server not found in pool");
		RETURN_FALSE;
	}

	RETURN_LONG(mmc->status);
}
/* }}} */

mmc_t *mmc_find_persistent(char *host, int host_len, int port, int timeout, int retry_interval TSRMLS_DC) /* {{{ */
{
	mmc_t *mmc;
	list_entry *le;
	char *hash_key;
	int hash_key_len;

	MMC_DEBUG(("mmc_find_persistent: seeking for persistent connection"));
	hash_key = emalloc(sizeof("mmc_connect___") - 1 + host_len + MAX_LENGTH_OF_LONG + 1);
	hash_key_len = sprintf(hash_key, "mmc_connect___%s:%d", host, port);

	if (zend_hash_find(&EG(persistent_list), hash_key, hash_key_len+1, (void **) &le) == FAILURE) {
		list_entry new_le;
		MMC_DEBUG(("mmc_find_persistent: connection wasn't found in the hash"));

		mmc = mmc_server_new(host, host_len, port, 1, timeout, retry_interval TSRMLS_CC);
		new_le.type = le_pmemcache;
		new_le.ptr  = mmc;

		/* register new persistent connection */
		if (zend_hash_update(&EG(persistent_list), hash_key, hash_key_len+1, (void *) &new_le, sizeof(list_entry), NULL) == FAILURE) {
			mmc_server_free(mmc TSRMLS_CC);
			mmc = NULL;
		} else {
			zend_list_insert(mmc, le_pmemcache);
		}
	}
	else if (le->type != le_pmemcache || le->ptr == NULL) {
		list_entry new_le;
		MMC_DEBUG(("mmc_find_persistent: something was wrong, reconnecting.."));
		zend_hash_del(&EG(persistent_list), hash_key, hash_key_len+1);

		mmc = mmc_server_new(host, host_len, port, 1, timeout, retry_interval TSRMLS_CC);
		new_le.type = le_pmemcache;
		new_le.ptr  = mmc;

		/* register new persistent connection */
		if (zend_hash_update(&EG(persistent_list), hash_key, hash_key_len+1, (void *) &new_le, sizeof(list_entry), NULL) == FAILURE) {
			mmc_server_free(mmc TSRMLS_CC);
			mmc = NULL;
		}
		else {
			zend_list_insert(mmc, le_pmemcache);
		}
	}
	else {
		MMC_DEBUG(("mmc_find_persistent: connection found in the hash"));
		mmc = (mmc_t *)le->ptr;
		mmc->timeout = timeout;
		mmc->retry_interval = retry_interval;

		/* attempt to reconnect this node before failover in case connection has gone away */
		if (mmc->status == MMC_STATUS_CONNECTED) {
			mmc->status = MMC_STATUS_UNKNOWN;
		}
	}

	efree(hash_key);
	return mmc;
}
/* }}} */

/* {{{ proto string memcache_get_version( object memcache )
   Returns server's version */
PHP_FUNCTION(memcache_get_version)
{
	mmc_pool_t *pool;
	char *version;
	int i;
	zval *mmc_object = getThis();

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &mmc_object, memcache_class_entry_ptr) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (mmc_open(pool->servers[i], 1, NULL, NULL TSRMLS_CC) && (version = mmc_get_version(pool->servers[i] TSRMLS_CC)) != NULL) {
			RETURN_STRING(version, 0);
		}
		else if (mmc_server_failure(pool->servers[i] TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->servers[i]->host, pool->servers[i]->port);
		}
	}

	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool memcache_add( object memcache, string key, mixed var [, int flag [, int expire ] ] )
   Adds new item. Item with such key should not exist. */
PHP_FUNCTION(memcache_add)
{
	php_mmc_store(INTERNAL_FUNCTION_PARAM_PASSTHRU, "add", sizeof("add") - 1);
}
/* }}} */

/* {{{ proto bool memcache_set( object memcache, string key, mixed var [, int flag [, int expire ] ] )
   Sets the value of an item. Item may exist or not */
PHP_FUNCTION(memcache_set)
{
	php_mmc_store(INTERNAL_FUNCTION_PARAM_PASSTHRU, "set", sizeof("set") - 1);
}
/* }}} */

/* {{{ proto bool memcache_replace( object memcache, string key, mixed var [, int flag [, int expire ] ] )
   Replaces existing item. Returns false if item doesn't exist */
PHP_FUNCTION(memcache_replace)
{
	php_mmc_store(INTERNAL_FUNCTION_PARAM_PASSTHRU, "replace", sizeof("replace") - 1);
}
/* }}} */

/* {{{ proto mixed memcache_get( object memcache, mixed key )
   Returns value of existing item or false */
PHP_FUNCTION(memcache_get)
{
	mmc_pool_t *pool;
	zval **key, *mmc_object = getThis();

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "OZ", &mmc_object, memcache_class_entry_ptr, &key) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Z", &key) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC) || !pool->num_servers) {
		RETURN_FALSE;
	}

	if (Z_TYPE_PP(key) != IS_ARRAY) {
		SEPARATE_ZVAL(key);
		convert_to_string_ex(key);
		MMC_PREPARE_KEY(Z_STRVAL_PP(key), Z_STRLEN_PP(key));

		if (mmc_exec_retrieval_cmd(pool, Z_STRVAL_PP(key), Z_STRLEN_PP(key), &return_value TSRMLS_CC) < 0) {
			RETURN_FALSE;
		}
	}
	else {
		if (mmc_exec_retrieval_cmd_multi(pool, *key, &return_value TSRMLS_CC) < 0) {
			RETURN_FALSE;
		}
	}
}
/* }}} */

/* {{{ proto bool memcache_delete( object memcache, string key [, int expire ])
   Deletes existing item */
PHP_FUNCTION(memcache_delete)
{
	mmc_t *mmc;
	mmc_pool_t *pool;
	int result = -1, key_len;
	zval *mmc_object = getThis();
	char *key;
	long time = 0;

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Os|l", &mmc_object, memcache_class_entry_ptr, &key, &key_len, &time) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "s|l", &key, &key_len, &time) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC) || !pool->num_servers) {
		RETURN_FALSE;
	}

	MMC_PREPARE_KEY(key, key_len);

	while (result < 0 && (mmc = mmc_pool_find(pool, key, key_len TSRMLS_CC)) != NULL) {
		if ((result = mmc_delete(mmc, key, key_len, time TSRMLS_CC)) < 0 && mmc_server_failure(mmc TSRMLS_CC)) {
			php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", mmc->host, mmc->port);
		}
	}

	if (result > 0) {
		RETURN_TRUE;
	}
	RETURN_FALSE;
}
/* }}} */

/* {{{ proto bool memcache_debug( bool onoff )
   Turns on/off internal debugging */
PHP_FUNCTION(memcache_debug)
{
#if ZEND_DEBUG
	zend_bool onoff;

	if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "b", &onoff) == FAILURE) {
		return;
	}

	MEMCACHE_G(debug_mode) = onoff ? 1 : 0;

	RETURN_TRUE;
#else
	RETURN_FALSE;
#endif

}
/* }}} */

/* {{{ proto array memcache_get_stats( object memcache [, string type [, int slabid [, int limit ] ] ])
   Returns server's statistics */
PHP_FUNCTION(memcache_get_stats)
{
	mmc_pool_t *pool;
	int i, failures = 0;
	zval *mmc_object = getThis();

	char *type = NULL;
	int type_len = 0;
	long slabid = 0, limit = MMC_DEFAULT_CACHEDUMP_LIMIT;

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|sll", &mmc_object, memcache_class_entry_ptr, &type, &type_len, &slabid, &limit) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sll", &type, &type_len, &slabid, &limit) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (!mmc_open(pool->servers[i], 1, NULL, NULL TSRMLS_CC) || mmc_get_stats(pool->servers[i], type, slabid, limit, return_value TSRMLS_CC) < 0) {
			if (mmc_server_failure(pool->servers[i] TSRMLS_CC)) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->servers[i]->host, pool->servers[i]->port);
			}
			failures++;
		}
		else {
			break;
		}
	}

	if (failures >= pool->num_servers) {
		RETURN_FALSE;
	}
}
/* }}} */

/* {{{ proto array memcache_get_extended_stats( object memcache [, string type [, int slabid [, int limit ] ] ])
   Returns statistics for each server in the pool */
PHP_FUNCTION(memcache_get_extended_stats)
{
	mmc_pool_t *pool;
	char *hostname;
	int i, hostname_len;
	zval *mmc_object = getThis(), *stats;

	char *type = NULL;
	int type_len = 0;
	long slabid = 0, limit = MMC_DEFAULT_CACHEDUMP_LIMIT;

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|sll", &mmc_object, memcache_class_entry_ptr, &type, &type_len, &slabid, &limit) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|sll", &type, &type_len, &slabid, &limit) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	array_init(return_value);
	for (i=0; i<pool->num_servers; i++) {
		MAKE_STD_ZVAL(stats);

		hostname = emalloc(strlen(pool->servers[i]->host) + MAX_LENGTH_OF_LONG + 1 + 1);
		hostname_len = sprintf(hostname, "%s:%d", pool->servers[i]->host, pool->servers[i]->port);

		if (!mmc_open(pool->servers[i], 1, NULL, NULL TSRMLS_CC) || mmc_get_stats(pool->servers[i], type, slabid, limit, stats TSRMLS_CC) < 0) {
			if (mmc_server_failure(pool->servers[i] TSRMLS_CC)) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->servers[i]->host, pool->servers[i]->port);
			}
			ZVAL_FALSE(stats);
		}

		add_assoc_zval_ex(return_value, hostname, hostname_len + 1, stats);
		efree(hostname);
	}
}
/* }}} */

/* {{{ proto array memcache_set_compress_threshold( object memcache, int threshold [, float min_savings ] )
   Set automatic compress threshold */
PHP_FUNCTION(memcache_set_compress_threshold)
{
	mmc_pool_t *pool;
	zval *mmc_object = getThis();
	long threshold;
	double min_savings = MMC_DEFAULT_SAVINGS;

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "Ol|d", &mmc_object, memcache_class_entry_ptr, &threshold, &min_savings) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "l|d", &threshold, &min_savings) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	if (threshold < 0) {
		php_error_docref(NULL TSRMLS_CC, E_WARNING, "threshold must be a positive integer");
		RETURN_FALSE;
	}
	pool->compress_threshold = threshold;

	if (min_savings != MMC_DEFAULT_SAVINGS) {
		if (min_savings < 0 || min_savings > 1) {
			php_error_docref(NULL TSRMLS_CC, E_WARNING, "min_savings must be a float in the 0..1 range");
			RETURN_FALSE;
		}
		pool->min_compress_savings = min_savings;
	}
	else {
		pool->min_compress_savings = MMC_DEFAULT_SAVINGS;
	}

	RETURN_TRUE;
}
/* }}} */

/* {{{ proto int memcache_increment( object memcache, string key [, int value ] )
   Increments existing variable */
PHP_FUNCTION(memcache_increment)
{
	php_mmc_incr_decr(INTERNAL_FUNCTION_PARAM_PASSTHRU, 1);
}
/* }}} */

/* {{{ proto int memcache_decrement( object memcache, string key [, int value ] )
   Decrements existing variable */
PHP_FUNCTION(memcache_decrement)
{
	php_mmc_incr_decr(INTERNAL_FUNCTION_PARAM_PASSTHRU, 0);
}
/* }}} */

/* {{{ proto bool memcache_close( object memcache )
   Closes connection to memcached */
PHP_FUNCTION(memcache_close)
{
	mmc_pool_t *pool;
	int i, failures = 0;
	zval *mmc_object = getThis();

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O", &mmc_object, memcache_class_entry_ptr) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (mmc_close(pool->servers[i] TSRMLS_CC) < 0) {
			if (mmc_server_failure(pool->servers[i] TSRMLS_CC)) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->servers[i]->host, pool->servers[i]->port);
			}
			failures++;
		}
	}

	if (failures && failures >= pool->num_servers) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */

/* {{{ proto bool memcache_flush( object memcache [, int timestamp ] )
   Flushes cache, optionally at the specified time */
PHP_FUNCTION(memcache_flush)
{
	mmc_pool_t *pool;
	int i, failures = 0;
	zval *mmc_object = getThis();
	long timestamp = 0;

	if (mmc_object == NULL) {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "O|l", &mmc_object, memcache_class_entry_ptr, &timestamp) == FAILURE) {
			return;
		}
	}
	else {
		if (zend_parse_parameters(ZEND_NUM_ARGS() TSRMLS_CC, "|l", &timestamp) == FAILURE) {
			return;
		}
	}

	if (!mmc_get_pool(mmc_object, &pool TSRMLS_CC)) {
		RETURN_FALSE;
	}

	for (i=0; i<pool->num_servers; i++) {
		if (!mmc_open(pool->servers[i], 1, NULL, NULL TSRMLS_CC) || mmc_flush(pool->servers[i], timestamp TSRMLS_CC) < 0) {
			if (mmc_server_failure(pool->servers[i] TSRMLS_CC)) {
				php_error_docref(NULL TSRMLS_CC, E_NOTICE, "marked server '%s:%d' as failed", pool->servers[i]->host, pool->servers[i]->port);
			}
			failures++;
		}
	}

	if (failures && failures >= pool->num_servers) {
		RETURN_FALSE;
	}
	RETURN_TRUE;
}
/* }}} */

#endif /* HAVE_MEMCACHE */
/*
 * Local variables:
 * tab-width: 4
 * c-basic-offset: 4
 * End:
 * vim600: noet sw=4 ts=4 fdm=marker
 * vim<600: noet sw=4 ts=4
 */
