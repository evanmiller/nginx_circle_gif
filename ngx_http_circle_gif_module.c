/*
 * Copyright (C) Evan Miller
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <wand/magick-wand.h>
#define radius2index(r, cglcf) (r-(cglcf)->min_radius)/(cglcf)->step_radius

enum colors { RED, GREEN, BLUE };

static char* ngx_http_circle_gif(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_circle_gif_preconf(ngx_conf_t *cf);
static ngx_int_t ngx_http_circle_gif_postconf(ngx_conf_t *cf);

static void* ngx_http_circle_gif_template(int req_radius, size_t* image_length_ptr, 
	MagickWand *wand, PixelWand *bg_wand, PixelWand *fg_wand, DrawingWand *dwand);

static void ngx_http_circle_gif_colorize(unsigned char *image, unsigned char *bg, unsigned char *fg);

static void* ngx_http_circle_gif_create_loc_conf(ngx_conf_t *cf);

static char* ngx_http_circle_gif_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

typedef struct {
    ngx_uint_t   max_radius;
    ngx_uint_t   min_radius;
    ngx_uint_t   step_radius;
    unsigned char** circle_templates;
    size_t* circle_sizes;
    ngx_flag_t           enable;
} ngx_http_circle_gif_loc_conf_t;

static ngx_int_t ngx_http_circle_gif_init(ngx_http_circle_gif_loc_conf_t *cf);

static ngx_command_t  ngx_http_circle_gif_commands[] = {
    { ngx_string("circle_gif"),
      NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
      ngx_http_circle_gif,
      NGX_HTTP_LOC_CONF_OFFSET,
      0,
      NULL },

    { ngx_string("circle_gif_min_radius"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_circle_gif_loc_conf_t, min_radius),
      NULL },

    { ngx_string("circle_gif_max_radius"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_circle_gif_loc_conf_t, max_radius),
      NULL },

    { ngx_string("circle_gif_step_radius"),
      NGX_HTTP_MAIN_CONF|NGX_HTTP_SRV_CONF|NGX_HTTP_LOC_CONF|NGX_CONF_TAKE1,
      ngx_conf_set_num_slot,
      NGX_HTTP_LOC_CONF_OFFSET,
      offsetof(ngx_http_circle_gif_loc_conf_t, step_radius),
      NULL },

      ngx_null_command
};


static ngx_http_module_t  ngx_http_circle_gif_module_ctx = {
    ngx_http_circle_gif_preconf,   /* preconfiguration */
    ngx_http_circle_gif_postconf,  /* postconfiguration */

    NULL,                          /* create main configuration */
    NULL,                          /* init main configuration */

    NULL,                          /* create server configuration */
    NULL,                          /* merge server configuration */

    ngx_http_circle_gif_create_loc_conf,  /* create location configuration */
    ngx_http_circle_gif_merge_loc_conf /* merge location configuration */
};


ngx_module_t  ngx_http_circle_gif_module = {
    NGX_MODULE_V1,
    &ngx_http_circle_gif_module_ctx, /* module context */
    ngx_http_circle_gif_commands,   /* module directives */
    NGX_HTTP_MODULE,               /* module type */
    NULL,                          /* init master */
    NULL,                          /* init module */
    NULL,                          /* init process */
    NULL,                          /* init thread */
    NULL,                          /* exit thread */
    NULL,                          /* exit process */
    NULL,                          /* exit master */
    NGX_MODULE_V1_PADDING
};


static ngx_int_t
ngx_http_circle_gif_handler(ngx_http_request_t *r)
{
    ngx_int_t     rc;
    ngx_buf_t    *b;
    ngx_chain_t   out;
    unsigned char *image;
    // all right, let's figure out what they're asking for
    u_int bg_color, fg_color, req_radius = 0;
    size_t i;
    ngx_int_t power;
    char *digit;
    unsigned char bg[3]; 
    unsigned char fg[3];

    ngx_http_circle_gif_loc_conf_t  *cglcf;
    cglcf = ngx_http_get_module_loc_conf(r, ngx_http_circle_gif_module);

    if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK && rc != NGX_AGAIN) {
        return rc;
    }

    if (r->headers_in.if_modified_since) {
        return NGX_HTTP_NOT_MODIFIED;
    }

    digit = (char *)r->uri.data + r->uri.len - 1;

    if (!(*digit-- == 'f' && *digit-- == 'i' && *digit-- == 'g' && *digit-- == '.')) {
	ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Invalid extension with %s", digit);
        return NGX_HTTP_NOT_FOUND;
    }
    for(i=0, power=1; *digit >= '0' && *digit <= '9' && i < (r->uri.len - 7 - 7 - 4); i++, power *= 10) {
        req_radius += (*digit-- - '0')*power;
    }
    fg_color   = strtol(digit -= 6, NULL, 16); // "XXXXXX"
    bg_color   = strtol(digit -= 7, NULL, 16); // "XXXXXX/"

    bg[RED] = bg_color >> 16;
    bg[GREEN] = bg_color >> 8;
    bg[BLUE] = bg_color;

    fg[RED] = fg_color >> 16;
    fg[GREEN] = fg_color >> 8;
    fg[BLUE] = fg_color;

    r->headers_out.content_type.len = sizeof("image/gif") - 1;
    r->headers_out.content_type.data = (u_char *) "image/gif";

    if (req_radius < cglcf->min_radius || req_radius > cglcf->max_radius) {
	ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Invalid radius %ui", req_radius);
        return NGX_HTTP_NOT_FOUND;
    }

    int radius_index = radius2index(req_radius, cglcf);

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = cglcf->circle_sizes[radius_index];

    if (r->method == NGX_HTTP_HEAD) {
        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
    if (b == NULL) {
	ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate response buffer.");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf = b;
    out.next = NULL;

    image = ngx_palloc(r->pool, cglcf->circle_sizes[radius_index]);
    if (image == NULL) {
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "Failed to allocate memory for circle image.");
      return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    ngx_memcpy(image, cglcf->circle_templates[radius_index], cglcf->circle_sizes[radius_index]);
    ngx_http_circle_gif_colorize(image, bg, fg);

    b->pos = image;
    b->last = image + cglcf->circle_sizes[radius_index];

    b->memory = 1;
    b->last_buf = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static char *
ngx_http_circle_gif(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t  *clcf;
    ngx_http_circle_gif_loc_conf_t *cglcf = conf;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_circle_gif_handler;

    cglcf->enable = 1;

    return NGX_CONF_OK;
}

static ngx_int_t 
ngx_http_circle_gif_preconf(ngx_conf_t *cf)
{
    MagickWandGenesis();
    return 0;
}

static ngx_int_t 
ngx_http_circle_gif_postconf(ngx_conf_t *cf)
{
    MagickWandTerminus();
    return 0;
}

static ngx_int_t
ngx_http_circle_gif_init(ngx_http_circle_gif_loc_conf_t *cglcf)
{
  u_int i;
  MagickWand *wand;
  PixelWand *bg_wand, *fg_wand;
  DrawingWand *dwand;

  wand = NewMagickWand();
  bg_wand = NewPixelWand();
  fg_wand = NewPixelWand();
  dwand = NewDrawingWand();
  if ((cglcf->circle_templates = malloc((1+radius2index(cglcf->max_radius, cglcf))*sizeof(unsigned char*))) == NULL ||
	  (cglcf->circle_sizes = malloc((1+radius2index(cglcf->max_radius, cglcf))*sizeof(size_t))) == NULL) {
    perror("malloc()");
    return NGX_ERROR;
  }
  for (i=0;i<=radius2index(cglcf->max_radius, cglcf);i++) {
    cglcf->circle_templates[i] = ngx_http_circle_gif_template(cglcf->min_radius+i*cglcf->step_radius, &cglcf->circle_sizes[i], 
            wand, bg_wand, fg_wand, dwand);
  }
  DestroyMagickWand( wand );
  DestroyPixelWand( fg_wand );
  DestroyPixelWand( bg_wand );
  DestroyDrawingWand( dwand );
  return i;
}

// build a B&W circle GIF in memory and return a pointer to it.
static void* ngx_http_circle_gif_template(int req_radius, size_t* image_length_ptr, 
	MagickWand *wand, PixelWand *bg_wand, PixelWand *fg_wand, DrawingWand *dwand)
{
  float radius = req_radius - 0.5;

  ClearDrawingWand(dwand);
  PixelSetColor(bg_wand, "#000000");
  PixelSetColor(fg_wand, "#ffffff");
  MagickNewImage(wand, req_radius*2, req_radius*2, bg_wand);
  DrawSetFillColor(dwand, fg_wand);
  DrawCircle(dwand, radius, radius, 0, radius);
  MagickDrawImage(wand, dwand);
  MagickSetImageFormat(wand, "gif");

  unsigned char *image = MagickGetImageBlob(wand, image_length_ptr);

  MagickRemoveImage(wand);
  return image;
}

static void ngx_http_circle_gif_colorize(unsigned char *image, unsigned char *bg, unsigned char *fg)
{
  unsigned char whiteness, blackness;
  // A note about GIFs: the size of the color table is stored in
  // the least significant 3 bits of the 11th byte. The color table itself
  // begins on the 14th byte; it has, successively, a red byte,
  // blue byte, and green byte, and repeats. This is the color palette.
  // We replace the white-ness of the standard image with the foreground
  // color, and the blackness with the background color.
  // See also: http://www.martinreddy.net/gfx/2d/GIF89a.txt
  unsigned char *color_table_ptr = &image[13];
  unsigned char *end_color_table_ptr = color_table_ptr+3*(1 << ((image[10] & 0x7) + 1));
  // yikes, pointer arithmetic! Not really necessary, but kinda fun
  while(color_table_ptr < end_color_table_ptr) {
    whiteness = *color_table_ptr; // actually the red byte; our B&W template has equal parts r, g, and b
    blackness = ~whiteness;
    // now assign new values to the red, green, and blue bytes
    *color_table_ptr++ = (whiteness*fg[RED]  +blackness*bg[RED])  /0xff;
    *color_table_ptr++ = (whiteness*fg[GREEN]+blackness*bg[GREEN])/0xff;
    *color_table_ptr++ = (whiteness*fg[BLUE] +blackness*bg[BLUE]) /0xff;
  }
}

static void *
ngx_http_circle_gif_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_circle_gif_loc_conf_t  *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(ngx_http_circle_gif_loc_conf_t));
    if (conf == NULL) {
        return NGX_CONF_ERROR;
    }
    conf->min_radius = NGX_CONF_UNSET_UINT;
    conf->max_radius = NGX_CONF_UNSET_UINT;
    conf->step_radius = NGX_CONF_UNSET_UINT;
    conf->enable = NGX_CONF_UNSET;
    return conf;
}

static char *
ngx_http_circle_gif_merge_loc_conf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_circle_gif_loc_conf_t *prev = parent;
    ngx_http_circle_gif_loc_conf_t *conf = child;

    ngx_conf_merge_uint_value(conf->min_radius, prev->min_radius, 10);
    ngx_conf_merge_uint_value(conf->max_radius, prev->max_radius, 20);
    ngx_conf_merge_uint_value(conf->step_radius, prev->step_radius, 2);
    ngx_conf_merge_value(conf->enable, prev->enable, 0);

    if (conf->min_radius < 1) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "min_radius must be equal or more than 1"); 
        return NGX_CONF_ERROR;
    }
    if (conf->max_radius < conf->min_radius) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0, "max_radius must be equal or more than min_radius"); 
        return NGX_CONF_ERROR;
    }

    if(conf->enable)
        ngx_http_circle_gif_init(conf);

    return NGX_CONF_OK;
}
