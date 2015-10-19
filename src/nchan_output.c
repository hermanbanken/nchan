#include <ngx_http.h>
#include "nchan_output.h"
#include <assert.h>

//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, arg...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "OUTPUT:" fmt, ##arg)
#define ERR(fmt, arg...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "OUTPUT:" fmt, ##arg)

#define REQUEST_PCALLOC(r, what) what = ngx_pcalloc((r)->pool, sizeof(*(what)))
#define REQUEST_PALLOC(r, what) what = ngx_palloc((r)->pool, sizeof(*(what)))

//general request-output functions and the iraq and the asian countries and dated references and the, uh, such

static void ngx_http_push_flush_pending_output(ngx_http_request_t *r) {
  int                        rc;
  ngx_event_t               *wev;
  ngx_connection_t          *c;
  ngx_http_core_loc_conf_t  *clcf;
  
  c = r->connection;
  wev = c->write;
  
  //ngx_log_debug2(NGX_LOG_DEBUG_HTTP, wev->log, 0, "http writer handler: \"%V?%V\"", &r->uri, &r->args);

  clcf = ngx_http_get_module_loc_conf(r->main, ngx_http_core_module);

  if (wev->timedout) {
    if (!wev->delayed) {
      ngx_log_error(NGX_LOG_INFO, c->log, NGX_ETIMEDOUT, "request timed out");
      c->timedout = 1;
      ngx_http_finalize_request(r, NGX_HTTP_REQUEST_TIME_OUT);
      return;
    }
    wev->timedout = 0;
    wev->delayed = 0;

    if (!wev->ready) {
      ngx_add_timer(wev, clcf->send_timeout);
      if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
        ngx_http_finalize_request(r, 0);
      }
      return;
    }
  }
  
  if (wev->delayed || r->aio) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, wev->log, 0, "http writer delayed");
    if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
      ngx_http_finalize_request(r, 0);
    }
    return;
  }
  
  rc = ngx_http_push_output_filter(r, NULL);

  //ngx_log_debug3(NGX_LOG_DEBUG_HTTP, c->log, 0, "http writer output filter: %d, \"%V?%V\"", rc, &r->uri, &r->args);

  if (rc == NGX_ERROR) {
    ngx_http_finalize_request(r, rc);
    return;
  }

  if (r->buffered || r->postponed || (r == r->main && c->buffered)) {
    if (!wev->delayed) {
      ngx_add_timer(wev, clcf->send_timeout);
    }
    if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
      ngx_http_finalize_request(r, 0);
    }
    return;
  }
  //ngx_log_debug2(NGX_LOG_DEBUG_HTTP, wev->log, 0, "http writer done: \"%V?%V\"", &r->uri, &r->args);
  r->write_event_handler = ngx_http_request_empty_handler;
}

ngx_int_t ngx_http_push_output_filter(ngx_http_request_t *r, ngx_chain_t *in) {
/* from push stream module, written by
 * Wandenberg Peixoto <wandenberg@gmail.com>, Rogério Carvalho Schneider <stockrt@gmail.com>
 * thanks, guys!
*/
  ngx_http_core_loc_conf_t               *clcf;
  ngx_int_t                               rc;
  ngx_event_t                            *wev;
  ngx_connection_t                       *c;

  c = r->connection;
  wev = c->write;

  rc = ngx_http_output_filter(r, in);

  if (c->buffered & NGX_HTTP_LOWLEVEL_BUFFERED) {
    ERR("what's the deal with this NGX_HTTP_LOWLEVEL_BUFFERED thing?");
    clcf = ngx_http_get_module_loc_conf(r->main, ngx_http_core_module);
    r->write_event_handler = ngx_http_push_flush_pending_output;
    if (!wev->delayed) {
      ngx_add_timer(wev, clcf->send_timeout);
    }
    if (ngx_handle_write_event(wev, clcf->send_lowat) != NGX_OK) {
      return NGX_ERROR;
    }
    return NGX_OK;
  } 
  else {
    if (wev->timer_set) {
      ngx_del_timer(wev);
    }
  }
  return rc;
}

ngx_int_t ngx_http_push_respond_status(ngx_http_request_t *r, ngx_int_t status_code, const ngx_str_t *status_line, ngx_int_t finalize) {
  ngx_int_t    rc = NGX_OK;
  r->headers_out.status=status_code;
  if(status_line!=NULL) {
    r->headers_out.status_line.len =status_line->len;
    r->headers_out.status_line.data=status_line->data;
  }
  r->headers_out.content_length_n = 0;
  r->header_only = 1;
    
  rc= ngx_http_send_header(r);
  if(finalize) {
    ngx_http_finalize_request(r, rc);
  }
  return rc;
}

ngx_int_t ngx_http_push_respond_membuf(ngx_http_request_t *r, ngx_int_t status_code, const ngx_str_t *content_type, ngx_buf_t *body, ngx_int_t finalize) {
  ngx_str_t str;
  str.len = ngx_buf_size(body);
  str.data = body->start;
  return ngx_http_push_respond_string(r, status_code, content_type, &str, finalize);
}

ngx_int_t ngx_http_push_respond_string(ngx_http_request_t *r, ngx_int_t status_code, const ngx_str_t *content_type, const ngx_str_t *body, ngx_int_t finalize) {
  ngx_int_t    rc = NGX_OK;
  ngx_buf_t   *b = REQUEST_PCALLOC(r, b);
  ngx_chain_t *chain = REQUEST_PALLOC(r, chain);
  
  //assume both were alloc'd fine
  
  r->headers_out.status=status_code;
  r->headers_out.content_length_n = body->len;
  
  if(content_type) {
    r->headers_out.content_type.len = content_type->len;
    r->headers_out.content_type.data = content_type->data;
  }
  
  chain->buf=b;
  chain->next=NULL;
  
  b->last_buf = 1;
  b->last_in_chain = 1;
  //b->flush = 1;
  b->memory = 1;
  b->start = body->data;
  b->pos = body->data;
  b->end = body->data + body->len;
  b->last = b->end;
  
  ngx_http_send_header(r);
  rc= ngx_http_push_output_filter(r, chain);
  
  if(finalize) {
    ngx_http_finalize_request(r, rc);
  }
  return rc;
}

/*
#define NGX_HTTP_BUF_ALLOC_SIZE(buf)                                         \
(sizeof(*buf) +                                                              \
(((buf)->temporary || (buf)->memory) ? ngx_buf_size(buf) : 0) +              \
(((buf)->file!=NULL) ? (sizeof(*(buf)->file) + (buf)->file->name.len + 1) : 0))

//buffer is _copied_
ngx_chain_t * ngx_http_push_create_output_chain(ngx_buf_t *buf, ngx_pool_t *pool, ngx_log_t *log) {
  ngx_chain_t                    *out;
  ngx_file_t                     *file;
  ngx_pool_cleanup_t             *cln = NULL;
  ngx_pool_cleanup_file_t        *clnf = NULL;
  if((out = ngx_pcalloc(pool, sizeof(*out)))==NULL) {
    ngx_log_error(NGX_LOG_ERR, log, 0, "nchan: can't create output chain, can't allocate chain  in pool");
    return NULL;
  }
  ngx_buf_t                      *buf_copy;
  
  if((buf_copy = ngx_pcalloc(pool, NGX_HTTP_BUF_ALLOC_SIZE(buf)))==NULL) {
    //TODO: don't zero the whole thing!
    ngx_log_error(NGX_LOG_ERR, log, 0, "nchan: can't create output chain, can't allocate buffer copy in pool");
    return NULL;
  }
  ngx_http_push_copy_preallocated_buffer(buf, buf_copy);
  
  if (buf->file!=NULL) {
    if(buf->mmap) { //just the mmap, please
      buf->in_file=0;
      buf->file=NULL;
      buf->file_pos=0;
      buf->file_last=0;
    }
    else {
      file = buf_copy->file;
      file->log=log;
      if(file->fd==NGX_INVALID_FILE) {
        //ngx_log_error(NGX_LOG_ERR, log, 0, "opening invalid file at %s", file->name.data);
        file->fd=ngx_open_file(file->name.data, NGX_FILE_RDONLY, NGX_FILE_OPEN, NGX_FILE_OWNER_ACCESS);
      }
      if(file->fd==NGX_INVALID_FILE) {
        ngx_log_error(NGX_LOG_ERR, log, 0, "nchan: can't create output chain, file in buffer is invalid");
        return NULL;
      }
      else {
        //close file on cleanup
        if((cln = ngx_pool_cleanup_add(pool, sizeof(*clnf))) == NULL) {
          ngx_close_file(file->fd);
          file->fd=NGX_INVALID_FILE;
          ngx_log_error(NGX_LOG_ERR, log, 0, "nchan: can't create output chain file cleanup.");
          return NULL;
        }
        cln->handler = ngx_pool_cleanup_file;
        clnf = cln->data;
        clnf->fd = file->fd;
        clnf->name = file->name.data;
        clnf->log = pool->log;
      }
    }
  }
  
  
  
  buf_copy->last_buf = 1;
  out->buf = buf_copy;
  out->next = NULL;
  return out;
}
*/