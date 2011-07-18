#include <ngx_core.h>
#include <ngx_string.h>
#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <mhash.h>
#include <openssl/md5.h>
#include <ctype.h>

#define FOLDER_MODE 0
#define FILE_MODE 1

#define URL_FORMAT_NGINX 1
#define URL_FORMAT_LIGHTTPD 0

typedef struct {
  const char *timestamp;
  const char *md5;
  const char *path;
  int path_len;
  int path_to_hash_len;
} ngx_http_secure_download_split_uri_t;

static ngx_int_t ngx_http_secure_download_split_uri (ngx_http_request_t*, ngx_http_secure_download_split_uri_t*);
static ngx_int_t ngx_http_secure_download_check_hash(ngx_http_request_t*, ngx_http_secure_download_split_uri_t*, ngx_str_t*);
static void * ngx_http_secure_download_create_loc_conf(ngx_conf_t*);
static char * ngx_http_secure_download_merge_loc_conf (ngx_conf_t*, void*, void*);
static ngx_int_t ngx_http_secure_download_add_variables(ngx_conf_t *cf);
static ngx_int_t ngx_http_secure_download_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data);
static char * ngx_conf_set_path_mode(ngx_conf_t*, ngx_command_t*, void*);
static char * ngx_conf_set_url_format(ngx_conf_t*, ngx_command_t*, void*);

static char *ngx_http_secure_download_secret(ngx_conf_t *cf, void *post, void *data);
static ngx_conf_post_handler_pt  ngx_http_secure_download_secret_p =
    ngx_http_secure_download_secret;

static char *ngx_http_secure_download_prefix(ngx_conf_t *cf, void *post, void *data);
static ngx_conf_post_handler_pt  ngx_http_secure_download_prefix_p =
    ngx_http_secure_download_prefix;

typedef struct {
  ngx_flag_t enable;
  ngx_flag_t path_mode;
  ngx_flag_t url_format;
  ngx_str_t secret;
  ngx_str_t prefix;
  ngx_uint_t period;
  ngx_array_t  *secret_lengths;
  ngx_array_t  *secret_values;
  ngx_array_t  *prefix_lengths;
  ngx_array_t  *prefix_values;
} ngx_http_secure_download_loc_conf_t;

static ngx_command_t ngx_http_secure_download_commands[] = {
  {
    ngx_string("secure_download"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, enable),
    NULL
  },
  {
    ngx_string("secure_download_path_mode"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_path_mode,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, path_mode),
    NULL
  },
  {
    ngx_string("secure_download_url_format"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_url_format,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, url_format),
    NULL
  },
  {
    ngx_string("secure_download_secret"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, secret),
    &ngx_http_secure_download_secret_p
  },
  {
    ngx_string("secure_download_prefix"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, prefix),
    &ngx_http_secure_download_prefix_p
  },
  {
    ngx_string("secure_download_period"),
    NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_num_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_secure_download_loc_conf_t, period),
    NULL
  }
};

static ngx_http_module_t ngx_http_secure_download_module_ctx = {
  ngx_http_secure_download_add_variables,
  NULL,

  NULL,
  NULL,

  NULL,
  NULL,

  ngx_http_secure_download_create_loc_conf,
  ngx_http_secure_download_merge_loc_conf
};

ngx_module_t ngx_http_secure_download_module = {
  NGX_MODULE_V1,
  &ngx_http_secure_download_module_ctx,
  ngx_http_secure_download_commands,
  NGX_HTTP_MODULE,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NULL,
  NGX_MODULE_V1_PADDING
};

static ngx_str_t  ngx_http_secure_download = ngx_string("secure_download");

static char * ngx_conf_set_path_mode(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_str_t *d = cf->args->elts;
  ngx_http_secure_download_loc_conf_t *sdlc = conf;
  if ((d[1].len == 6) && (strncmp((char*)d[1].data, "folder", 6) == 0))
  {
    sdlc->path_mode = FOLDER_MODE;
  }
  else if((d[1].len == 4) && (strncmp((char*)d[1].data, "file", 4) == 0))
  {
    sdlc->path_mode = FILE_MODE;
  }
  else
  {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "secure_download_path_mode should be folder or file", 0);
    return NGX_CONF_ERROR;
  }
  return NGX_CONF_OK;
}

static char * ngx_conf_set_url_format(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
  ngx_str_t *d = cf->args->elts;
  ngx_http_secure_download_loc_conf_t *sdlc = conf;
  if ((d[1].len == 5) && (strncmp((char*)d[1].data, "nginx", 5) == 0))
  {
    sdlc->url_format = URL_FORMAT_NGINX;
  }
  else if((d[1].len == 8) && (strncmp((char*)d[1].data, "lighttpd", 8) == 0))
  {
    sdlc->url_format = URL_FORMAT_LIGHTTPD;
  }
  else
  {
    ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "secure_download_url_format should be nginx or lighttpd", 0);
    return NGX_CONF_ERROR;
  }
  return NGX_CONF_OK;
}

static void * ngx_http_secure_download_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_secure_download_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_secure_download_loc_conf_t));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }
  conf->enable = NGX_CONF_UNSET;
  conf->path_mode = NGX_CONF_UNSET;
  conf->url_format = NGX_CONF_UNSET;
  conf->secret.data = NULL;
  conf->secret.len = 0;
  conf->prefix.data = NULL;
  conf->prefix.len = 0;
  conf->period = NGX_CONF_UNSET;
  return conf;
}

static char * ngx_http_secure_download_merge_loc_conf (ngx_conf_t *cf, void *parent, void *child)
{
  ngx_http_secure_download_loc_conf_t *prev = parent;
  ngx_http_secure_download_loc_conf_t *conf = child;

  ngx_conf_merge_value(conf->enable, prev->enable, 0);
  ngx_conf_merge_value(conf->path_mode, prev->path_mode, FOLDER_MODE);
  ngx_conf_merge_value(conf->url_format, prev->url_format, URL_FORMAT_LIGHTTPD);
  ngx_conf_merge_str_value(conf->secret, prev->secret, "");
  ngx_conf_merge_str_value(conf->prefix, prev->prefix, "");
  ngx_conf_merge_uint_value(conf->period, prev->period, 10*60*60);

  if (conf->enable == 1) {
      if (conf->secret.len == 0) {
          ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
               "no secure_download_secret specified");
          return NGX_CONF_ERROR;
      }

      if (conf->url_format == URL_FORMAT_LIGHTTPD && conf->prefix.len == 0) {
          ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
               "no secure_download_prefix specified");
          return NGX_CONF_ERROR;
      }
  }
  return NGX_CONF_OK;
}

static ngx_int_t ngx_http_secure_download_variable(ngx_http_request_t *r, ngx_http_variable_value_t *v, uintptr_t data) {
  unsigned timestamp;
  unsigned remaining_time = 0;
  ngx_http_secure_download_loc_conf_t *sdc;
  ngx_http_secure_download_split_uri_t sdsu;
  ngx_str_t rel_path;
  ngx_str_t secret;
  int value = 0;
  
  sdc = ngx_http_get_module_loc_conf(r, ngx_http_secure_download_module);
  if (sdc->enable != 1)
  {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "securedownload: module not enabled");
      value = -3;
      goto finish;
  }
  
  if (!sdc->secret_lengths || !sdc->secret_values) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "securedownload: module enabled, but secret key not configured!");
      value = -3;
      goto finish;
  }

  if (!sdc->prefix_lengths || !sdc->prefix_values) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
          "securedownload: module enabled, but prefix not configured!");
      value = -3;
      goto finish;
  }

  if (ngx_http_secure_download_split_uri(r, &sdsu) == NGX_ERROR)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "received an error from ngx_http_secure_download_split_uri", 0);
    value = -3;
    goto finish;
  }

  if (sscanf(sdsu.timestamp, "%08X", &timestamp) != 1)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "error in timestamp hex-dec conversion", 0);
    value = -3;
    goto finish;
  }
  
  remaining_time = sdc->period + timestamp - (unsigned) time(NULL);
  if ((int)remaining_time <= 0)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "expired timestamp", 0);
    value = -1;
    goto finish;
  }
  
  if (ngx_http_script_run(r, &secret, sdc->secret_lengths->elts, 0, sdc->secret_values->elts) == NULL) {
      ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
          "securedownload: evaluation failed");
      value = -3;
      goto finish;
  }
  
  ngx_log_error(NGX_LOG_INFO, r->connection->log, 0,
    "securedownload: evaluated value of secret: \"%V\"", &secret);
    
  if (ngx_http_secure_download_check_hash(r, &sdsu, &secret) != NGX_OK)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "bad hash", 0);
    value = -2;
    goto finish;
  }
  
  rel_path.data = r->uri.data;
  rel_path.len = sdsu.path_len;
  
  finish: 
  
  v->not_found = 0;
  v->valid = 1;
  v->no_cacheable = 0;
  v->not_found = 0;
  if (value == 0)
  {
    v->data = ngx_pcalloc(r->pool, sizeof(char) * 12);
    if (v->data == NULL) {
        return NGX_ERROR;
    }
    v->len = (int) sprintf((char *)v->data, "%i", remaining_time);
    //printf("valid, %i\n", remaining_time);
  } else {
    v->data = ngx_pcalloc(r->pool, sizeof(char) * 3);
    if (v->data == NULL) {
        return NGX_ERROR;
    }
    v->len = (int) sprintf((char*)v->data, "%i", value);
    //printf("problem %i\n", value);
  }
  
  return NGX_OK;
}

//////////////////////
static char *
ngx_http_secure_download_compile_secret(ngx_conf_t *cf, ngx_http_secure_download_loc_conf_t *sdc)
{

    ngx_http_script_compile_t   sc;
    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    sc.cf = cf;
    sc.source = &sdc->secret;
    sc.lengths = &sdc->secret_lengths;
    sc.values = &sdc->secret_values;
    sc.variables = ngx_http_script_variables_count(&sdc->secret);
    sc.complete_lengths = 1;
    sc.complete_values = 1;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_secure_download_secret(ngx_conf_t *cf, void *post, void *data)
{
    ngx_http_secure_download_loc_conf_t *sdc =
	    ngx_http_conf_get_module_loc_conf(cf, ngx_http_secure_download_module);

    return ngx_http_secure_download_compile_secret(cf, sdc);
}
////////////////////////

//////////////////////
static char *
ngx_http_secure_download_compile_prefix(ngx_conf_t *cf, ngx_http_secure_download_loc_conf_t *sdc)
{

    ngx_http_script_compile_t   sc;
    ngx_memzero(&sc, sizeof(ngx_http_script_compile_t));

    sc.cf = cf;
    sc.source = &sdc->prefix;
    sc.lengths = &sdc->prefix_lengths;
    sc.values = &sdc->prefix_values;
    sc.variables = ngx_http_script_variables_count(&sdc->prefix);
    sc.complete_lengths = 1;
    sc.complete_values = 1;

    if (ngx_http_script_compile(&sc) != NGX_OK) {
        return NGX_CONF_ERROR;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_secure_download_prefix(ngx_conf_t *cf, void *post, void *data)
{
    ngx_http_secure_download_loc_conf_t *sdc =
	    ngx_http_conf_get_module_loc_conf(cf, ngx_http_secure_download_module);

    return ngx_http_secure_download_compile_prefix(cf, sdc);
}
////////////////////////

static ngx_int_t ngx_http_secure_download_check_hash(ngx_http_request_t *r, ngx_http_secure_download_split_uri_t *sdsu, ngx_str_t *secret)
{
  int i;
  unsigned char generated_hash[16];
  char hash[33];
  MHASH td;
  char *hash_data, *str;
  int data_len;
  ngx_http_secure_download_loc_conf_t *sdc;


  static const char xtoc[] = "0123456789abcdef";

  sdc = ngx_http_get_module_loc_conf(r, ngx_http_secure_download_module);

  /* rel_path_to_hash/secret/timestamp\0 */

  if(sdc->url_format == URL_FORMAT_LIGHTTPD) {
      data_len = sdsu->path_to_hash_len + secret->len + 8;
  } else {
      data_len = sdsu->path_to_hash_len + secret->len + 10;
  }

  /* debug */
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hashing string \"%s\" with len %i", sdsu->path, sdsu->path_to_hash_len);

  hash_data = malloc(data_len + 1);
  if (hash_data == NULL)
  {
    ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "error in allocating memory for string_to_hash.data", 0);
    return NGX_ERROR;
  }

  if(sdc->url_format == URL_FORMAT_LIGHTTPD) {
      str = hash_data;
      memcpy(str, secret->data, secret->len);
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "str is \"%s\", lenght is %d", str, secret->len);

      str += secret->len;
      //*str++ = '/';
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "str is \"%s\", lenght is %d", str, secret->len);

      memcpy(str, sdsu->path, sdsu->path_to_hash_len);

      str += sdsu->path_to_hash_len;
      //*str++ = '/';
      memcpy(str, sdsu->timestamp, 8);
      str[8] = 0;
  
  } else {

      str = hash_data;
      memcpy(str, sdsu->path, sdsu->path_to_hash_len);

      str += sdsu->path_to_hash_len;
      *str++ = '/';
      memcpy(str, secret->data, secret->len);
      str += secret->len;
      *str++ = '/';
      memcpy(str, sdsu->timestamp, 8);
      str[8] = 0;
  }

  td = mhash_init(MHASH_MD5);

  if (td == MHASH_FAILED)
  {
	free(hash_data);
    return NGX_ERROR;
  }
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hashing string \"%s\" with len %i", hash_data, data_len);
  mhash(td, hash_data, data_len);
  mhash_deinit(td, generated_hash);

  free(hash_data);

  for (i = 0; i < 16; ++i) {
	  hash[2 * i + 0] = xtoc[generated_hash[i] >> 4];
	  hash[2 * i + 1] = xtoc[generated_hash[i] & 0xf];
  }
  
  hash[32] = 0; //because %.32 doesn't work
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "computed hash: %32s", hash); 
  // ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hash from uri: %.32s", sdsu->md5);

  if(memcmp(hash, sdsu->md5, 32) != 0) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "hash mismatch", 0); 	  
	  return NGX_ERROR;
  }

  return NGX_OK;
}

static ngx_int_t ngx_http_secure_download_split_uri(ngx_http_request_t *r, ngx_http_secure_download_split_uri_t *sdsu)
{
  int md5_len = 0;
  int tstamp_len = 0;
  int pos = 0;
  int len = r->uri.len;
  const char *uri = (char*)r->uri.data;
  
  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "uri: %s", uri);

  ngx_http_secure_download_loc_conf_t *sdc = ngx_http_get_module_loc_conf(r, ngx_http_secure_download_module);

  if(sdc->url_format == URL_FORMAT_LIGHTTPD) {
      ngx_str_t *prefix = &sdc->prefix;
      pos = prefix->len;
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "prefix value is %s,  size is: %d", prefix->data, prefix->len);

      sdsu->md5 = uri + pos + 1;
      while(len && uri[++pos] != '/')
          ++md5_len;
      if(md5_len != 32) {
	      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "md5 size mismatch: %d", md5_len);
	      return NGX_ERROR;
      }
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "md5 value is %s,  size is: %d", sdsu->md5, md5_len);

      sdsu->timestamp = uri + pos + 1;
      while(len && uri[++pos] != '/')
          ++tstamp_len;
      if(tstamp_len != 8) {
	      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "timestamp size mismatch: %d", tstamp_len);
	      return NGX_ERROR;
      }
      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "timestamp value is %s,  size is: %d, at %d", sdsu->timestamp, tstamp_len, pos);

      if(len == pos) {
	      ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "bad path", 0);
	      return NGX_ERROR;
      }

      /* Fix-it */
      sdsu->path = uri + pos;
      sdsu->path_len = len - pos;

      if(sdc->path_mode == FOLDER_MODE) {
	      while(len && uri[--len] != '/');
      }
      sdsu->path_to_hash_len = len - pos;

      return NGX_OK;
  }

  while(len && uri[--len] != '/')
	  ++tstamp_len;
  if(tstamp_len != 8) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "timestamp size mismatch: %d", tstamp_len);
	  return NGX_ERROR;
  }
  sdsu->timestamp = uri + len + 1;

  while(len && uri[--len] != '/')
	  ++md5_len;
  if(md5_len != 32) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "md5 size mismatch: %d", md5_len);
	  return NGX_ERROR;
  }
  sdsu->md5 = uri + len + 1;

  if(len == 0) {
	  ngx_log_debug(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "bad path", 0);
	  return NGX_ERROR;
  }

  sdsu->path = uri;
  sdsu->path_len = len;

  if(sdc->path_mode == FOLDER_MODE) {
	  while(len && uri[--len] != '/');
  }
  sdsu->path_to_hash_len = len;

  return NGX_OK;
}

static ngx_int_t
ngx_http_secure_download_add_variables(ngx_conf_t *cf)
{
    ngx_http_variable_t  *var;

    var = ngx_http_add_variable(cf, &ngx_http_secure_download, NGX_HTTP_VAR_NOHASH);
    if (var == NULL) {
        return NGX_ERROR;
    }

    var->get_handler = ngx_http_secure_download_variable;

    return NGX_OK;
}

