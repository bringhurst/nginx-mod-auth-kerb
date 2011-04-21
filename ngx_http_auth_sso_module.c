/*
 * Copyright (C) 2009 Michal Kowalski <superflouos{at}gmail[dot]com>
 *
 * Blah, blah, blah...
 *
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
/*
#include <ngx_string.h>
*/

#include <gssapi/gssapi.h>

#include <krb5.h>

/* #include <spnegohelp.h> */
int parseNegTokenInit (const unsigned char *  negTokenInit,
                       size_t                 negTokenInitLength,
                       const unsigned char ** kerberosToken,
                       size_t *               kerberosTokenLength);
int makeNegTokenTarg (const unsigned char *  kerberosToken,
                      size_t                 kerberosTokenLength,
                      const unsigned char ** negTokenTarg,
                      size_t *               negTokenTargLength);

/* Module handler */
static ngx_int_t ngx_http_auth_sso_handler(ngx_http_request_t*);

static void *ngx_http_auth_sso_create_loc_conf(ngx_conf_t*);
static char *ngx_http_auth_sso_merge_loc_conf(ngx_conf_t*, void*, void*);
static ngx_int_t ngx_http_auth_sso_init(ngx_conf_t*);

/* stolen straight from mod_auth_gss_krb5.c except for ngx_ mods */
const char *
get_gss_error(ngx_pool_t *p,
	      OM_uint32 error_status,
	      char *prefix)
{
   OM_uint32 maj_stat, min_stat;
   OM_uint32 msg_ctx = 0;
   gss_buffer_desc status_string;
   char buf[1024];
   size_t len;
   ngx_str_t str;

   /* ngx_fubarprintf... what a hack... %Z inserts '\0' */
   ngx_snprintf((u_char *) buf, sizeof(buf), "%s: %Z", prefix);
   len = ngx_strlen(buf);
   do {
      maj_stat = gss_display_status (&min_stat,
	                             error_status,
				     GSS_C_MECH_CODE,
				     GSS_C_NO_OID,
				     &msg_ctx,
				     &status_string);
      if (sizeof(buf) > len + status_string.length + 1) {
/*
         sprintf(buf, "%s:", (char*) status_string.value);
*/
         ngx_sprintf((u_char *) buf+len, "%s:%Z", (char*) status_string.value);
         len += ( status_string.length + 1);
      }
      gss_release_buffer(&min_stat, &status_string);
   } while (!GSS_ERROR(maj_stat) && msg_ctx != 0);

   /* "include" '\0' */
   str.len = len + 1;
   str.data = (u_char *) buf;
   return (char *)(ngx_pstrdup(p, &str));
}

/* Module per Req/Con CONTEXTUAL Struct */

typedef struct {
  ngx_str_t token; /* decoded Negotiate token */
  ngx_int_t head; /* non-zero flag if headers set */
  ngx_int_t ret; /* current return code */
} ngx_http_auth_sso_ctx_t;

/* Module Configuration Struct(s) (main|srv|loc) */

typedef struct {
  ngx_flag_t protect;
  ngx_str_t realm;
  ngx_str_t keytab;
  ngx_str_t srvcname;
  ngx_flag_t fqun;
} ngx_http_auth_sso_loc_conf_t;

/* Module Directives */

static ngx_command_t ngx_http_auth_sso_commands[] = {

  /*
     { ngx_str_t name;
       ngx_uint_t type;
       char *(*set)(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
       ngx_uint_t conf;
       ngx_uint_t offset;
       void *post; }
  */

  { ngx_string("auth_gss"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_auth_sso_loc_conf_t, protect),
    NULL },

  { ngx_string("auth_gss_realm"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_auth_sso_loc_conf_t, realm),
    NULL },

  { ngx_string("auth_gss_keytab"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_auth_sso_loc_conf_t, keytab),
    NULL },

  { ngx_string("auth_gss_service_name"),
    /* TODO change to NGX_CONF_1MORE for "http", "khttp", besides "HTTP" */
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
    ngx_conf_set_str_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_auth_sso_loc_conf_t, srvcname),
    NULL },

  { ngx_string("auth_gss_format_full"),
    NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_FLAG,
    ngx_conf_set_flag_slot,
    NGX_HTTP_LOC_CONF_OFFSET,
    offsetof(ngx_http_auth_sso_loc_conf_t, fqun),
    NULL },

  ngx_null_command
};

/* Module Context */

static ngx_http_module_t ngx_http_auth_sso_module_ctx = {
  NULL, /* preconf */
  ngx_http_auth_sso_init, /* postconf */

  NULL, /* create main conf (defaults) */
  NULL, /* init main conf (what's in nginx.conf) */

  NULL, /* create server conf */
  NULL, /* merge with main */

  ngx_http_auth_sso_create_loc_conf, /* create location conf */
  ngx_http_auth_sso_merge_loc_conf /* merge with server */
};

/* Module Definition */

/* really ngx_module_s /shrug */
ngx_module_t ngx_http_auth_sso_module = {
  /* ngx_uint_t ctx_index, index, spare{0-3}, version; */
  NGX_MODULE_V1, /* 0, 0, 0, 0, 0, 0, 1 */
  &ngx_http_auth_sso_module_ctx, /* void *ctx */
  ngx_http_auth_sso_commands, /* ngx_command_t *commands */
  NGX_HTTP_MODULE, /* ngx_uint_t type = 0x50545448 */
  NULL, /* ngx_int_t (*init_master)(ngx_log_t *log) */
  NULL, /* ngx_int_t (*init_module)(ngx_cycle_t *cycle) */
  NULL, /* ngx_int_t (*init_process)(ngx_cycle_t *cycle) */
  NULL, /* ngx_int_t (*init_thread)(ngx_cycle_t *cycle) */
  NULL, /* void (*exit_thread)(ngx_cycle_t *cycle) */
  NULL, /* void (*exit_process)(ngx_cycle_t *cycle) */
  NULL, /* void (*exit_master)(ngx_cycle_t *cycle) */
  NGX_MODULE_V1_PADDING /* 0, 0, 0, 0, 0, 0, 0, 0 */
  /* uintptr_t spare_hook{0-7}; */
};

static void *
ngx_http_auth_sso_create_loc_conf(ngx_conf_t *cf)
{
  ngx_http_auth_sso_loc_conf_t *conf;

  conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_auth_sso_loc_conf_t));
  if (conf == NULL) {
    return NGX_CONF_ERROR;
  }

  conf->protect = NGX_CONF_UNSET;
  conf->fqun = NGX_CONF_UNSET;

  /* temporary "debug" */
#if (NGX_DEBUG)
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0,
		     "auth_sso: allocated loc_conf_t (0x%p)", conf);
#endif
  /* TODO find out if there is way to enable it only in debug mode */

  return conf;
}

static char *
ngx_http_auth_sso_merge_loc_conf(ngx_conf_t *cf,
				 void *parent,
				 void *child)
{
  ngx_http_auth_sso_loc_conf_t *prev = parent;
  ngx_http_auth_sso_loc_conf_t *conf = child;

  /* "off" by default */
  ngx_conf_merge_off_value(conf->protect, prev->protect, 0);

  ngx_conf_merge_str_value(conf->realm, prev->realm, "LOCALDOMAIN");
  ngx_conf_merge_str_value(conf->keytab, prev->keytab, "/etc/krb5.keytab");
  ngx_conf_merge_str_value(conf->srvcname, prev->srvcname, "HTTP");

  ngx_conf_merge_off_value(conf->fqun, prev->fqun, 0);

  /* TODO make it only shout in debug */
#if (NGX_DEBUG)
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "auth_sso: protect = %i",
		     conf->protect);
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "auth_sso: realm@0x%p = %s",
		     conf->realm.data, conf->realm.data);
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "auth_sso: keytab@0x%p = %s",
		     conf->keytab.data, conf->keytab.data);
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "auth_sso: srvcname@0x%p = %s",
		     conf->srvcname.data, conf->srvcname.data);
  ngx_conf_log_error(NGX_LOG_INFO, cf, 0, "auth_sso: fqun = %i",
		     conf->fqun);
#endif

  return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_auth_sso_init(ngx_conf_t *cf)
{
  ngx_http_handler_pt *h;
  ngx_http_core_main_conf_t *cmcf;

  cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

  h = ngx_array_push(&cmcf->phases[NGX_HTTP_ACCESS_PHASE].handlers);
  if (h == NULL) {
    return NGX_ERROR;
  }

  *h = ngx_http_auth_sso_handler;

  return NGX_OK;
}

static ngx_int_t
ngx_http_auth_sso_negotiate_headers(ngx_http_request_t *r,
				    ngx_http_auth_sso_ctx_t *ctx,
				    ngx_str_t *token)
{
  ngx_str_t value = ngx_null_string;

  if (token == NULL) {
    value.len = sizeof("Negotiate") - 1;
    value.data = (u_char *) "Negotiate";
  } else {
    value.len = sizeof("Negotiate") + token->len;
    value.data = ngx_pcalloc(r->pool, value.len + 1);
    if (value.data == NULL) {
      return NGX_ERROR;
    }
    ngx_snprintf(value.data, value.len + 1, "Negotiate %V", token);
  }

  r->headers_out.www_authenticate = ngx_list_push(&r->headers_out.headers);
  if (r->headers_out.www_authenticate == NULL) {
    return NGX_ERROR;
  }

  r->headers_out.www_authenticate->hash = 1;
  r->headers_out.www_authenticate->key.len = sizeof("WWW-Authenticate") - 1;
  r->headers_out.www_authenticate->key.data = (u_char *) "WWW-Authenticate";

  r->headers_out.www_authenticate->value.len = value.len;
  r->headers_out.www_authenticate->value.data = value.data;

  ctx->head = 1;

  return NGX_OK;
}

/* sort of like ngx_http_auth_basic_user ... except we store in ctx_t? */
ngx_int_t
ngx_http_auth_sso_token(ngx_http_request_t *r,
			ngx_http_auth_sso_ctx_t *ctx)
{
  /* not copying or decoding anything, just checking if token is present
     and where? NOPE, koz ngx_decode_base64 uses ngx_str_t... so might as well... */
  ngx_str_t token;
  ngx_str_t decoded;

  if (r->headers_in.authorization == NULL) {
    return NGX_DECLINED;
  }
  /* but don't decode second time? */
  if (ctx->token.len) return NGX_OK;

  token = r->headers_in.authorization->value;

  if (token.len < sizeof("Negotiate ") - 1
      || ngx_strncasecmp(token.data, (u_char *) "Negotiate ",
			 sizeof("Negotiate ") - 1) != 0) {
    return NGX_DECLINED;
  }

  token.len -= sizeof("Negotiate ") - 1;
  token.data += sizeof("Negotiate ") - 1;

  while (token.len && token.data[0] == ' ') {
    token.len--;
    token.data++;
  }

  if (token.len == 0) {
    return NGX_DECLINED;
  }

  decoded.len = ngx_base64_decoded_length(token.len);
  decoded.data = ngx_pnalloc(r->pool, decoded.len + 1);
  if (decoded.data == NULL) {
    return NGX_ERROR;
  }

  if (ngx_decode_base64(&decoded, &token) != NGX_OK) {
    return NGX_DECLINED;
  }

  decoded.data[decoded.len] = '\0'; /* hmmm */

/*   ctx = ngx_palloc(r->pool, sizeof(ngx_http_auth_sso_ctx_t)); */
/*   if (ctx == NULL) { */
/*     return NGX_ERROR; */
/*   } */

/*   ngx_http_set_ctx(r, ctx, ngx_http_auth_sso_module); */

  ctx->token.len = decoded.len;
  ctx->token.data = decoded.data;
  /* off by one? hmmm... */
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "Token decoded");

  return NGX_OK;
}

/*
  Because 'remote_user' is assumed to be provided by basic authorization
  (see ngx_http_variable_remote_user) we are forced to create bogus
  non-Negotiate authorization header. This may possibly clobber Negotiate
  token too soon.
*/

ngx_int_t
ngx_http_auth_sso_set_bogus_authorization(ngx_http_request_t *r)
{
  ngx_str_t plain, encoded, final;
  /* jezuz 3 allocs ;( */
  
  if (r->headers_in.user.len == 0) {
    return NGX_DECLINED;
  }

  /* including \0 from sizeof because it's "user:password" */
  plain.len = r->headers_in.user.len + sizeof("bogus");
  plain.data = ngx_pnalloc(r->pool, plain.len);
  if (plain.data == NULL) {
    return NGX_ERROR;
  }

  ngx_snprintf(plain.data, plain.len, "%V:bogus", &r->headers_in.user);

  encoded.len = ngx_base64_encoded_length(plain.len);
  encoded.data = ngx_pnalloc(r->pool, encoded.len);
  if (encoded.data == NULL) {
    return NGX_ERROR;
  }

  ngx_encode_base64(&encoded, &plain);

  final.len = sizeof("Basic ") + encoded.len - 1;
  final.data = ngx_pnalloc(r->pool, final.len);
  if (final.data == NULL) {
    return NGX_ERROR;
  }

  ngx_snprintf(final.data, final.len, "Basic %V", &encoded);

  /* WARNING clobbering authorization header value */
  r->headers_in.authorization->value.len = final.len;
  r->headers_in.authorization->value.data = final.data;

  return NGX_OK;
}

ngx_int_t
ngx_http_auth_sso_auth_user_gss(ngx_http_request_t *r,
				ngx_http_auth_sso_ctx_t *ctx,
				ngx_http_auth_sso_loc_conf_t *alcf)
{
  static unsigned char ntlmProtocol [] = {'N', 'T', 'L', 'M', 'S', 'S', 'P', 0};

  /*
    nginx stuff
  */
  ngx_str_t host_name;
  ngx_int_t ret = NGX_DECLINED;
  int rc;
  int spnego_flag = 0;
  char *p;
  /*
    kerberos stuff
  */
  krb5_context krb_ctx = NULL;
  char *ktname = NULL;
  /* ngx_str_t kerberosToken; ? */
  unsigned char *kerberosToken = NULL;
  size_t kerberosTokenLength = 0;
  /* this izgotten from de-SPNEGGING original token...
     and put into gss_accept_sec_context...
     silly...
   */
  ngx_str_t spnegoToken = ngx_null_string;
  /* unsigned char *spnegoToken = NULL ;
     size_t spnegoTokenLength = 0; */
  /*
    gssapi stuff
  */
  OM_uint32 major_status, minor_status, minor_status2;
  gss_buffer_desc service = GSS_C_EMPTY_BUFFER;
  gss_name_t my_gss_name = GSS_C_NO_NAME;
  gss_cred_id_t my_gss_creds = GSS_C_NO_CREDENTIAL;
  gss_buffer_desc input_token = GSS_C_EMPTY_BUFFER;
  gss_ctx_id_t gss_context = GSS_C_NO_CONTEXT;
  gss_name_t client_name = GSS_C_NO_NAME;
  gss_buffer_desc output_token = GSS_C_EMPTY_BUFFER;
  OM_uint32 ret_flags = 0;
  gss_cred_id_t delegated_cred = GSS_C_NO_CREDENTIAL;

  /* first, see if there is a point in runing */
/*   ctx = ngx_http_get_module_ctx(r, ngx_http_auth_sso_module); */
  /* this really shouldn't 'eppen */
  if (!ctx || ctx->token.len == 0) {
    return ret;
  }
  /* on with the copy cat show */
  ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "GSSAPI authorizing");

  krb5_init_context(&krb_ctx);

  ktname = (char *) ngx_pcalloc(r->pool, sizeof("KRB5_KTNAME=")+alcf->keytab.len);
  if (ktname == NULL) {
    ret = NGX_ERROR;
    goto end;
  }
  ngx_snprintf((u_char *) ktname, sizeof("KRB5_KTNAME=")+alcf->keytab.len,
	       "KRB5_KTNAME=%V%Z", &alcf->keytab);
  putenv(ktname);

  ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "Use keytab %V", &alcf->keytab);

  /* TODECIDE: wherefrom use the hostname value for the service name? */
  host_name = r->headers_in.host->value;
  /* for now using the name client thinks... */
  service.length = alcf->srvcname.len + host_name.len + 2;
  /* @ vel / */
  service.value = ngx_palloc(r->pool, service.length);
  if (service.value == NULL) {
    ret = NGX_ERROR;
    goto end;
  }
  ngx_snprintf(service.value, service.length, "%V@%V%Z",
	       &alcf->srvcname, &host_name);

  ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "Use service principal %V/%V", &alcf->srvcname, &host_name);

  major_status = gss_import_name(&minor_status, &service,
                                 GSS_C_NT_HOSTBASED_SERVICE, &my_gss_name);
  if (GSS_ERROR(major_status)) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		  "%s Used service principal: %s",
		  get_gss_error(r->pool, minor_status,
				"gss_import_name() failed for service principal"),
		  (unsigned char *)service.value);
    ret = NGX_ERROR;
    goto end;
  }

  major_status = gss_acquire_cred(&minor_status,
				  my_gss_name,
				  GSS_C_INDEFINITE,
				  GSS_C_NO_OID_SET,
				  GSS_C_ACCEPT,
				  &my_gss_creds,
				  NULL,
				  NULL);
  if (GSS_ERROR(major_status)) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		  "%s Used service principal: %s",
		  get_gss_error(r->pool, minor_status,
				"gss_acquire_cred() failed"),
		  (unsigned char *)service.value);
    ret = NGX_ERROR;
    goto end;
  }

  /* the MEAT? */
  input_token.length = ctx->token.len + 1;
  input_token.value = (void *) ctx->token.data;
  /* Should check first if SPNEGO token */
  /* but it looks like mit-kerberos version > 1.4.4 DOES include GSSAPI
     code that supports SPNEGO... ("donated by SUN")... */
  if ( (rc = parseNegTokenInit (input_token.value,
			       input_token.length,
			       (const unsigned char **) &kerberosToken,
			       &kerberosTokenLength)) != 0 ) {

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "parseNegTokenInit failed with rc=%d", rc);
    /* 
       Error 1xy -> assume GSSAPI token and continue 
    */
    if ( rc < 100 || rc > 199 ) {
      ret = NGX_DECLINED;
      goto end;
      /* TOTALLY CLUELESS */
    }
    /* feeble NTLM... */
    if ( (input_token.length >= sizeof ntlmProtocol + 1) &&
	 (!ngx_memcmp(input_token.value, ntlmProtocol, sizeof ntlmProtocol)) ) {
      ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		     "received type %d NTLM token",
		     (int) *((unsigned char *)input_token.value + sizeof ntlmProtocol)); /* jeez */
      ret = NGX_DECLINED;
      goto end;
    }
    spnego_flag = 0;
  } else {
    input_token.length = kerberosTokenLength;
    input_token.value = ngx_pcalloc(r->pool, input_token.length);
    if (input_token.value == NULL) {
      ret = NGX_ERROR;
      goto end;
    }
    ngx_memcpy(input_token.value, kerberosToken, input_token.length);
    spnego_flag = 1;
  }

  major_status = gss_accept_sec_context(&minor_status,
					&gss_context,
					my_gss_creds,
					&input_token,
					GSS_C_NO_CHANNEL_BINDINGS,
					&client_name,
					NULL,
					&output_token,
					&ret_flags,
					NULL,
					&delegated_cred);

  if (output_token.length) {
    ngx_str_t token = ngx_null_string;

    if (spnego_flag) {
      if ( (rc = makeNegTokenTarg (output_token.value,
				  output_token.length,
				  (const unsigned char **) &spnegoToken.data,
				  &spnegoToken.len)) != 0 ) {
	ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		       "makeNegTokenTarg failed with rc=%d",rc);
	ret = NGX_DECLINED;
	goto end;
      }
    } else {
      spnegoToken.data = (u_char *) output_token.value;
      spnegoToken.len = output_token.length - 1;
    }
    /* XXX use ap_uuencode() */
    token.len = ngx_base64_encoded_length(spnegoToken.len);
    token.data = ngx_pcalloc(r->pool, token.len + 1);
    if (token.data == NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		    "Not enough memory");
      ret = NGX_ERROR;
      /* ??? */
      gss_release_buffer(&minor_status2, &output_token);
      goto end;
    }
    ngx_encode_base64(&token, &spnegoToken); /* did it work (void) */

    /* ??? */
    gss_release_buffer(&minor_status2, &output_token);

    /* and now here we had to rework ngx_http_auth_sso_negotiate_headers... */

    if ( (ret = ngx_http_auth_sso_negotiate_headers(r, ctx, &token)) == NGX_ERROR ) {
      goto end;
    }
    /*    ap_table_set(r->err_headers_out, "WWW-Authenticate",
	  ap_pstrcat(r->pool, "Negotiate ", token, NULL)); */
  }

  /* theesee two ifs could/SHOULD? as well go before the block above?!?
     headers shouldn't be set if we DECLINE, but i guess there won't be output_token anyway... */
  if (GSS_ERROR(major_status)) {
    ngx_log_debug2(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "%s Used service principal: %s",
		   get_gss_error(r->pool, minor_status,
				 "gss_accept_sec_context() failed"),
		   (unsigned char *)service.value);
    ret = NGX_DECLINED;
    goto end;
  }

  if (major_status & GSS_S_CONTINUE_NEEDED) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "only one authentication iteration allowed");
    ret = NGX_DECLINED;
    goto end;
  }

  /* INFO */
  if ( !(ret_flags & GSS_C_REPLAY_FLAG || ret_flags & GSS_C_SEQUENCE_FLAG) ){
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "GSSAPI Warning: no replay protection !");
  }
  if ( !(ret_flags & GSS_C_SEQUENCE_FLAG) ){
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "GSSAPI Warning: no sequence protection !");
  }

  /* getting user name at the other end of the request */
  major_status = gss_display_name(&minor_status,
				  client_name,
				  &output_token,
				  NULL);
  gss_release_name(&minor_status, &client_name);

  /* hmm... if he is going to ERROR out now we should do it before setting headers... */
  if (GSS_ERROR(major_status)) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0,
		  "%s", get_gss_error(r->pool, minor_status, 
		                      "gss_display_name() failed"));
    ret = NGX_ERROR;
    goto end;
  }

  if (output_token.length) {
    /* TOFIX dirty quick trick for now (no "-1" i.e. include '\0' */
    ngx_str_t user = { output_token.length,
		       (u_char *) output_token.value };

    r->headers_in.user.data = ngx_pstrdup(r->pool, &user);
    /* NULL?!? */
    r->headers_in.user.len = user.len;

    if (alcf->fqun == 0) {
      p = ngx_strchr(r->headers_in.user.data, '@');
      if (p != NULL) {
	if (ngx_strcmp(p+1, alcf->realm.data) == 0) {
	  *p = '\0';
	  r->headers_in.user.len = ngx_strlen(r->headers_in.user.data);
	}
      }
    }

    /* this for the sake of ngx_http_variable_remote_user */
    ngx_http_auth_sso_set_bogus_authorization(r);

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "user is %V", &r->headers_in.user);
  }

  gss_release_buffer(&minor_status, &output_token);

  /* saving creds... LATER, for now debug msg... */
  if (delegated_cred != GSS_C_NO_CREDENTIAL) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		   "Had delegated_cred to save.");
  }

  ret = NGX_OK;
  /* goto end; */

  /* well, alright, the end, my friend */
end:
  if (delegated_cred)
     gss_release_cred(&minor_status, &delegated_cred);

  if (output_token.length) 
     gss_release_buffer(&minor_status, &output_token);

  if (client_name != GSS_C_NO_NAME)
     gss_release_name(&minor_status, &client_name);

  if (gss_context != GSS_C_NO_CONTEXT)
     gss_delete_sec_context(&minor_status, &gss_context, GSS_C_NO_BUFFER);

  krb5_free_context(krb_ctx);
  if (my_gss_name != GSS_C_NO_NAME)
     gss_release_name(&minor_status, &my_gss_name);

  if (my_gss_creds != GSS_C_NO_CREDENTIAL)
     gss_release_cred(&minor_status, &my_gss_creds);

  return ret;
}

static ngx_int_t
ngx_http_auth_sso_handler(ngx_http_request_t *r)
{
  ngx_int_t ret;
  ngx_http_auth_sso_ctx_t *ctx;
  ngx_http_auth_sso_loc_conf_t *alcf;

  alcf = ngx_http_get_module_loc_conf(r, ngx_http_auth_sso_module);

  if (alcf->protect == 0) {
    return NGX_DECLINED;
  }

  /* looks like we need ctx_t "frst" after all, there is URI level
     access phase and filesystem level access phase... */
  ctx = ngx_http_get_module_ctx(r, ngx_http_auth_sso_module);
  if (ctx == NULL) {
    ctx = ngx_palloc(r->pool, sizeof(ngx_http_auth_sso_ctx_t));
    if (ctx == NULL) {
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ctx->token.len = 0;
    ctx->token.data = NULL;
    ctx->head = 0;
    ctx->ret = NGX_HTTP_UNAUTHORIZED;
    ngx_http_set_ctx(r, ctx, ngx_http_auth_sso_module);
  }
  /* nope... local fs req creates new ctx... useless... */

  ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "SSO auth handling IN: token.len=%d, head=%d, ret=%d",
		 ctx->token.len, ctx->head, ctx->ret);

  if (ctx->token.len && ctx->head)
    return ctx->ret;
  if (r->headers_in.user.data != NULL)
    return NGX_OK;

  ret = ngx_http_auth_sso_token(r, ctx);

  if (ret == NGX_OK) {
    /* ok... looks like client sent some Negotiate'ing authorization header... */
    ret = ngx_http_auth_sso_auth_user_gss(r, ctx, alcf);
  }

  if (ret == NGX_DECLINED) {
    /* TODEBATE skip if (ctx->head)... */
    ret = ngx_http_auth_sso_negotiate_headers(r, ctx, NULL);
    if (ret == NGX_ERROR) {
      return (ctx->ret = NGX_HTTP_INTERNAL_SERVER_ERROR);
    }
    return (ctx->ret = NGX_HTTP_UNAUTHORIZED);
  }

  if (ret == NGX_ERROR) {
    return (ctx->ret = NGX_HTTP_INTERNAL_SERVER_ERROR);
  }

  /* else NGX_OK */
  ngx_log_debug3(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
		 "SSO auth handling OUT: token.len=%d, head=%d, ret=%d",
		 ctx->token.len, ctx->head, ret);
  return (ctx->ret = ret);
}
