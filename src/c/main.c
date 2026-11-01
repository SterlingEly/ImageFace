// Image Face SPIKE — emery + gabbro only.
// Question being answered: can we stream a full-screen image from the phone into RAM,
// how big a heap does it leave us, and how long does it take?
//
// Shake the wrist to switch between 4bpp (16-colour palette) and 8bpp (full colour)
// and re-fetch. The on-screen report shows size, transfer time and free heap
// (before request -> after bitmap alloc -> after transfer), the AppMessage inbox size,
// and whether the 4-bit nibble order matched (auto-fixed if not).
#include <pebble.h>

typedef enum { ST_IDLE, ST_REQ, ST_RECV, ST_READY, ST_ERR } State;

static Window  *s_window;
static Layer   *s_canvas;
static GBitmap *s_img;
static AppTimer *s_timeout;
static State    s_state = ST_IDLE;
static int      s_bpp = 4;
static int      s_w, s_h;
static uint32_t s_total, s_tight, s_got;
static uint32_t s_t_req, s_t_done;
static int      s_free_before, s_free_alloc, s_free_done;
static uint32_t s_inbox;
static int      s_nib;          // 0 unknown, 1 high-nibble-first OK, -1 swapped (fixed)
static int      s_nib_tries;
static const char *s_err = "";
static char     s_time[8];

static uint32_t now_ms(void) {
  time_t s; uint16_t ms;
  time_ms(&s, &ms);
  return (uint32_t)(s % 100000) * 1000u + ms;
}

static void free_image(void) {
  if (s_img) { gbitmap_destroy(s_img); s_img = NULL; }
}

static void fail(const char *why) {
  s_state = ST_ERR;
  s_err = why;
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void timeout_cb(void *d) {
  s_timeout = NULL;
  if (s_state != ST_READY) fail("timeout (20s)");
}

static void request_image(void) {
  free_image();                       // free the old image BEFORE building the new one
  if (s_timeout) { app_timer_cancel(s_timeout); s_timeout = NULL; }
  s_state = ST_REQ; s_got = 0; s_nib = 0; s_nib_tries = 0;
  GRect b = layer_get_bounds(s_canvas);
  s_free_before = (int)heap_bytes_free();
  s_t_req = now_ms();

  DictionaryIterator *it;
  if (app_message_outbox_begin(&it) != APP_MSG_OK) { fail("outbox busy"); return; }
  dict_write_int32(it, MESSAGE_KEY_ReqBpp, s_bpp);
  dict_write_int32(it, MESSAGE_KEY_ReqW, b.size.w);
  dict_write_int32(it, MESSAGE_KEY_ReqH, b.size.h);
  dict_write_int32(it, MESSAGE_KEY_ReqChunk, (int32_t)s_inbox - 64);
  dict_write_end(it);
  if (app_message_outbox_send() != APP_MSG_OK) { fail("send failed"); return; }
  s_timeout = app_timer_register(20000, timeout_cb, NULL);
  layer_mark_dirty(s_canvas);
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  if (dict_find(iter, MESSAGE_KEY_JsReady)) {
    request_image();
    return;
  }

  Tuple *tw = dict_find(iter, MESSAGE_KEY_ImgW);
  if (tw) {                           // ---- header
    free_image();
    s_w     = (int)tw->value->uint32;
    s_h     = (int)dict_find(iter, MESSAGE_KEY_ImgH)->value->uint32;
    s_bpp   = (int)dict_find(iter, MESSAGE_KEY_ImgBpp)->value->uint32;
    s_total = dict_find(iter, MESSAGE_KEY_ImgTotal)->value->uint32;
    s_tight = s_total / (uint32_t)s_h;
    s_got = 0;

    if (s_bpp == 4) {
      Tuple *tp = dict_find(iter, MESSAGE_KEY_ImgPalette);
      GColor *pal = (tp && tp->length >= 16) ? (GColor *)malloc(16) : NULL;
      if (!pal) { fail("palette alloc"); return; }
      memcpy(pal, tp->value->data, 16);
      s_img = gbitmap_create_blank_with_palette(GSize(s_w, s_h), GBitmapFormat4BitPalette, pal, true);
      if (!s_img) { free(pal); }
    } else {
      s_img = gbitmap_create_blank(GSize(s_w, s_h), GBitmapFormat8Bit);
    }
    if (!s_img) { fail("bitmap alloc FAILED"); return; }
    s_free_alloc = (int)heap_bytes_free();
    s_state = ST_RECV;
    layer_mark_dirty(s_canvas);
    return;
  }

  Tuple *to = dict_find(iter, MESSAGE_KEY_ChunkOffset);   // ---- chunk
  Tuple *td = dict_find(iter, MESSAGE_KEY_ChunkData);
  if (to && td && s_img && s_state == ST_RECV) {
    uint32_t off = to->value->uint32;
    uint8_t *dst = gbitmap_get_data(s_img);
    uint32_t stride = gbitmap_get_bytes_per_row(s_img);
    const uint8_t *src = td->value->data;
    for (uint32_t i = 0; i < td->length; i++) {
      uint32_t o = off + i;
      uint32_t row = o / s_tight;
      if (row >= (uint32_t)s_h) break;
      dst[row * stride + (o - row * s_tight)] = src[i];
    }
    s_got += td->length;
    if (s_got >= s_total) {
      s_t_done = now_ms();
      s_free_done = (int)heap_bytes_free();
      s_state = ST_READY;
      if (s_timeout) { app_timer_cancel(s_timeout); s_timeout = NULL; }
    }
    layer_mark_dirty(s_canvas);
  }
}

static void inbox_dropped(AppMessageResult reason, void *ctx) {
  APP_LOG(APP_LOG_LEVEL_WARNING, "inbox dropped: %d", (int)reason);
}

static void draw_outlined(GContext *ctx, const char *text, GFont font, GRect r, int px) {
  static const int8_t dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1};
  static const int8_t dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
  graphics_context_set_text_color(ctx, GColorBlack);
  for (int i = 0; i < 8; i++) {
    graphics_draw_text(ctx, text, font, GRect(r.origin.x + dx[i] * px, r.origin.y + dy[i] * px, r.size.w, r.size.h),
                       GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
  }
  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, text, font, r, GTextOverflowModeTrailingEllipsis, GTextAlignmentCenter, NULL);
}

static void build_report(char *o, size_t n) {
  switch (s_state) {
    case ST_IDLE:
      snprintf(o, n, "waiting for phone...");
      break;
    case ST_REQ:
      snprintf(o, n, "requesting %dbpp\ninbox %lu B", s_bpp, (unsigned long)s_inbox);
      break;
    case ST_RECV:
      snprintf(o, n, "receiving %lu%%\n%lu / %lu B", (unsigned long)(s_got * 100 / (s_total ? s_total : 1)),
               (unsigned long)s_got, (unsigned long)s_total);
      break;
    case ST_READY: {
      uint32_t dt = s_t_done - s_t_req;
      const char *nib = (s_bpp != 4) ? "n/a" : (s_nib == 1 ? "hi ok" : (s_nib == -1 ? "lo (fixed)" : "?"));
      snprintf(o, n, "%dbpp %luKB %lu.%02lus\nfree %dK>%dK>%dK\ninbox %lu nib %s",
               s_bpp, (unsigned long)(s_total / 1024), (unsigned long)(dt / 1000), (unsigned long)((dt % 1000) / 10),
               s_free_before / 1024, s_free_alloc / 1024, s_free_done / 1024,
               (unsigned long)s_inbox, nib);
      break;
    }
    default:
      snprintf(o, n, "ERR: %s\nfree %dK", s_err, (int)heap_bytes_free() / 1024);
  }
}

static void canvas_update(Layer *layer, GContext *ctx) {
  GRect b = layer_get_bounds(layer);
  graphics_context_set_fill_color(ctx, GColorBlack);
  graphics_fill_rect(ctx, b, 0, GCornerNone);

  if (s_img && s_state == ST_READY) {
    graphics_context_set_compositing_mode(ctx, GCompOpAssign);
    graphics_draw_bitmap_in_rect(ctx, s_img, b);

    // Nibble-order probe: the phone put RED at the centre pixel and BLUE right of it.
    // Read the frame buffer back; if they are swapped, swap every nibble and redraw.
    if (s_bpp == 4 && s_nib == 0 && s_nib_tries < 3) {
      s_nib_tries++;
      GBitmap *fb = graphics_capture_frame_buffer(ctx);
      if (fb) {
        uint8_t *d = gbitmap_get_data(fb);
        int rb = gbitmap_get_bytes_per_row(fb);
        int cx = (b.size.w >> 1) & ~1, cy = b.size.h >> 1;
        uint8_t px = d[cy * rb + cx];
        graphics_release_frame_buffer(ctx, fb);
        if (px == 0xF0) {
          s_nib = 1;
        } else if (px == 0xC3) {
          uint8_t *data = gbitmap_get_data(s_img);
          size_t len = (size_t)gbitmap_get_bytes_per_row(s_img) * (size_t)s_h;
          for (size_t i = 0; i < len; i++) data[i] = (uint8_t)((data[i] << 4) | (data[i] >> 4));
          s_nib = -1;
          layer_mark_dirty(layer);
        }
      }
    }
  }

  char rep[96];
  build_report(rep, sizeof(rep));
  draw_outlined(ctx, rep, fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD), GRect(0, 36, b.size.w, 56), 1);
  draw_outlined(ctx, s_time, fonts_get_system_font(FONT_KEY_BITHAM_42_BOLD), GRect(0, 138, b.size.w, 50), 2);
}

static void update_time(void) {
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(s_time, sizeof(s_time), clock_is_24h_style() ? "%H:%M" : "%I:%M", t);
  if (!clock_is_24h_style() && s_time[0] == '0') memmove(s_time, s_time + 1, strlen(s_time));
  if (s_canvas) layer_mark_dirty(s_canvas);
}

static void tick_handler(struct tm *t, TimeUnits u) { update_time(); }

static void tap_handler(AccelAxisType axis, int32_t direction) {
  s_bpp = (s_bpp == 4) ? 8 : 4;       // shake: toggle 4bpp <-> 8bpp and re-fetch
  request_image();
}

static void window_load(Window *w) {
  Layer *root = window_get_root_layer(w);
  s_canvas = layer_create(layer_get_bounds(root));
  layer_set_update_proc(s_canvas, canvas_update);
  layer_add_child(root, s_canvas);
  update_time();
}

static void window_unload(Window *w) {
  layer_destroy(s_canvas);
  s_canvas = NULL;
}

int main(void) {
  s_inbox = app_message_inbox_size_maximum();
  app_message_register_inbox_received(inbox_received);
  app_message_register_inbox_dropped(inbox_dropped);
  if (app_message_open(s_inbox, 256) != APP_MSG_OK) {
    s_inbox = 4096;
    app_message_open(s_inbox, 256);
  }
  s_window = window_create();
  window_set_background_color(s_window, GColorBlack);
  window_set_window_handlers(s_window, (WindowHandlers){ .load = window_load, .unload = window_unload });
  window_stack_push(s_window, true);
  tick_timer_service_subscribe(MINUTE_UNIT, tick_handler);
  accel_tap_service_subscribe(tap_handler);
  app_event_loop();
  accel_tap_service_unsubscribe();
  tick_timer_service_unsubscribe();
  free_image();
  window_destroy(s_window);
  return 0;
}
