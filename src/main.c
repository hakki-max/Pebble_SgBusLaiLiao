#include <pebble.h>
#include <string.h>
#include <stdlib.h>

#define KEY_CMD           0
#define KEY_STOP_CODE     1
#define KEY_ARRIVALS_JSON 2
#define KEY_STOPS_JSON    3
#define KEY_ERROR         4

#define MAX_ARRIVALS  30
#define MAX_BOOKMARKS 16
#define MAX_NEARBY    5
#define PKEY_BM_COUNT 100
#define PKEY_BM_BASE  101

#define DIGIT_W       18
#define DIGIT_H       24
#define DIGIT_GAP     4
#define DIGIT_START_X 14
#define DIGIT_Y       52

typedef struct {
  char service[8];
  char eta[3][16];
  int  eta_count;
  char load[16];
} BusArrival;

typedef struct {
  char code[6];
  char label[24];
} Bookmark;

typedef struct {
  char code[6];
  char desc[32];
  int  dist;
} NearbyStop;

static BusArrival s_arrivals[MAX_ARRIVALS];
static int        s_arrival_count = 0;
static char       s_stop_label[32];
static char       s_current_stop_code[6];

static Bookmark s_bookmarks[MAX_BOOKMARKS];
static int      s_bookmark_count = 0;

static NearbyStop s_nearby[MAX_NEARBY];
static int        s_nearby_count = 0;

static AppTimer  *s_refresh_timer = NULL;

static Window    *s_main_window;
static Window    *s_arrivals_window;
static Window    *s_bm_window;
static Window    *s_search_window;
static Window    *s_nearby_window;
static MenuLayer *s_main_menu_layer;
static MenuLayer *s_arrivals_menu_layer;
static MenuLayer *s_bm_menu_layer;
static MenuLayer *s_nearby_menu_layer;
static Layer     *s_search_layer;

static int s_digits[5];
static int s_digit_pos = 0;

static const char *MAIN_MENU_ITEMS[] = {"Nearby Stops", "Bookmarks", "Search by Code"};
#define MAIN_MENU_COUNT 3

static void push_arrivals_window(const char *stop_code, const char *label);
static void push_nearby_window(void);
static void request_arrivals(const char *stop_code);

// ══════════════════════════════════════════════════════════
// BOOKMARKS
// ══════════════════════════════════════════════════════════
static void bookmarks_load(void) {
  s_bookmark_count = persist_exists(PKEY_BM_COUNT) ? persist_read_int(PKEY_BM_COUNT) : 0;
  for (int i = 0; i < s_bookmark_count; i++) {
    persist_read_string(PKEY_BM_BASE + i*2,     s_bookmarks[i].code,  sizeof(s_bookmarks[i].code));
    persist_read_string(PKEY_BM_BASE + i*2 + 1, s_bookmarks[i].label, sizeof(s_bookmarks[i].label));
  }
}

static void bookmark_save(const char *code, const char *label) {
  for (int i = 0; i < s_bookmark_count; i++) {
    if (strcmp(s_bookmarks[i].code, code) == 0) {
      vibes_double_pulse();
      return;
    }
  }
  if (s_bookmark_count >= MAX_BOOKMARKS) return;
  strncpy(s_bookmarks[s_bookmark_count].code,  code,  sizeof(s_bookmarks[0].code)  - 1);
  strncpy(s_bookmarks[s_bookmark_count].label, label, sizeof(s_bookmarks[0].label) - 1);
  persist_write_string(PKEY_BM_BASE + s_bookmark_count*2,     s_bookmarks[s_bookmark_count].code);
  persist_write_string(PKEY_BM_BASE + s_bookmark_count*2 + 1, s_bookmarks[s_bookmark_count].label);
  s_bookmark_count++;
  persist_write_int(PKEY_BM_COUNT, s_bookmark_count);
  vibes_short_pulse();
}

// ══════════════════════════════════════════════════════════
// ARRIVALS
// ══════════════════════════════════════════════════════════
static uint16_t arrivals_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)(s_arrival_count > 0 ? s_arrival_count : 1);
}

static void arrivals_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *cb) {
  if (s_arrival_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Loading...", "Fetching arrivals", NULL);
    return;
  }
  BusArrival *a = &s_arrivals[idx->row];
  char eta_str[48] = "";
  for (int i = 0; i < a->eta_count; i++) {
    char stripped[16] = "";
    char *min_pos = strstr(a->eta[i], " min");
    if (min_pos) {
      int numlen = (int)(min_pos - a->eta[i]);
      strncpy(stripped, a->eta[i], numlen);
      stripped[numlen] = '\0';
    } else {
      strncpy(stripped, a->eta[i], sizeof(stripped) - 1);
    }
    if (i > 0) strncat(eta_str, ", ", sizeof(eta_str) - strlen(eta_str) - 1);
    strncat(eta_str, stripped, sizeof(eta_str) - strlen(eta_str) - 1);
  }
  strncat(eta_str, " min | ", sizeof(eta_str) - strlen(eta_str) - 1);
  strncat(eta_str, a->load,   sizeof(eta_str) - strlen(eta_str) - 1);
  char title[16];
  snprintf(title, sizeof(title), "Bus %s", a->service);
  menu_cell_basic_draw(ctx, cell_layer, title, eta_str, NULL);
}

static int16_t arrivals_get_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return 48;
}

static void arrivals_long_select(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  bookmark_save(s_current_stop_code, s_stop_label);
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Bookmarked stop %s (%s)", s_stop_label, s_current_stop_code);
}

static void arrivals_refresh_callback(void *ctx) {
  s_refresh_timer = NULL;
  if (!s_arrivals_window) return;
  APP_LOG(APP_LOG_LEVEL_DEBUG, "Auto-refresh arrivals for %s", s_current_stop_code);
  request_arrivals(s_current_stop_code);
  s_refresh_timer = app_timer_register(60000, arrivals_refresh_callback, NULL);
}

static void arrivals_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_arrivals_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_arrivals_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows      = arrivals_get_num_rows,
    .draw_row          = arrivals_draw_row,
    .get_cell_height   = arrivals_get_row_height,
    .select_long_click = arrivals_long_select,
  });
  menu_layer_set_click_config_onto_window(s_arrivals_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_arrivals_menu_layer));
  s_refresh_timer = app_timer_register(60000, arrivals_refresh_callback, NULL);
}

static void arrivals_window_unload(Window *window) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  menu_layer_destroy(s_arrivals_menu_layer);
  s_arrivals_menu_layer = NULL;
}

static void push_arrivals_window(const char *stop_code, const char *label) {
  strncpy(s_stop_label, label, sizeof(s_stop_label) - 1);
  strncpy(s_current_stop_code, stop_code, sizeof(s_current_stop_code) - 1);
  s_arrival_count = 0;
  if (s_arrivals_window) {
    window_destroy(s_arrivals_window);
    s_arrivals_window = NULL;
  }
  s_arrivals_window = window_create();
  window_set_window_handlers(s_arrivals_window, (WindowHandlers){
    .load   = arrivals_window_load,
    .unload = arrivals_window_unload,
  });
  window_stack_push(s_arrivals_window, true);
  request_arrivals(stop_code);
}

// ══════════════════════════════════════════════════════════
// APPMESSAGE
// ══════════════════════════════════════════════════════════
static void request_arrivals(const char *stop_code) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_cstring(out, KEY_CMD,       "arrivals");
  dict_write_cstring(out, KEY_STOP_CODE, stop_code);
  app_message_outbox_send();
}

static void request_nearby(void) {
  DictionaryIterator *out;
  if (app_message_outbox_begin(&out) != APP_MSG_OK) return;
  dict_write_cstring(out, KEY_CMD, "nearby");
  app_message_outbox_send();
}

static void parse_arrivals(const char *data) {
  APP_LOG(APP_LOG_LEVEL_DEBUG, "parse_arrivals received len=%d", (int)strlen(data));
  s_arrival_count = 0;
  s_arrival_count = 0;
  static char tmp_svc[90][8];
  static char tmp_eta[90][16];
  static char tmp_load[90];
  int  tmp_count = 0;

  const char *p = data;
  while (*p && tmp_count < 90) {
    const char *sep1 = strchr(p, '|');
    if (!sep1) break;
    int svc_len = (int)(sep1 - p);
    if (svc_len <= 0 || svc_len >= 8) break;
    memset(tmp_svc[tmp_count], 0, 8);
    strncpy(tmp_svc[tmp_count], p, svc_len);
    p = sep1 + 1;

    const char *sep2 = strchr(p, '|');
    if (!sep2) break;
    int eta_len = (int)(sep2 - p);
    if (eta_len <= 0 || eta_len >= 16) break;
    memset(tmp_eta[tmp_count], 0, 16);
    strncpy(tmp_eta[tmp_count], p, eta_len);
    p = sep2 + 1;

    tmp_load[tmp_count] = *p;
    tmp_count++;
    p++;
    if (*p == ',') p++;
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Flat parsed: %d entries", tmp_count);

  memset(s_arrivals, 0, sizeof(s_arrivals));
  for (int i = 0; i < tmp_count; i++) {
    int found = -1;
    for (int j = 0; j < s_arrival_count; j++) {
      if (strcmp(s_arrivals[j].service, tmp_svc[i]) == 0) {
        found = j;
        break;
      }
    }
    if (found >= 0) {
      BusArrival *a = &s_arrivals[found];
      if (a->eta_count < 3) {
        strncpy(a->eta[a->eta_count], tmp_eta[i], sizeof(a->eta[0]) - 1);
        a->eta_count++;
      }
    } else if (s_arrival_count < MAX_ARRIVALS) {
      BusArrival *a = &s_arrivals[s_arrival_count];
      strncpy(a->service, tmp_svc[i], sizeof(a->service) - 1);
      strncpy(a->eta[0],  tmp_eta[i], sizeof(a->eta[0])  - 1);
      a->eta_count = 1;
      char lc = tmp_load[i];
      if      (lc == 'G') strncpy(a->load, "Low",  sizeof(a->load) - 1);
      else if (lc == 'Y') strncpy(a->load, "Med",  sizeof(a->load) - 1);
      else if (lc == 'R') strncpy(a->load, "Full", sizeof(a->load) - 1);
      else                strncpy(a->load, "Low",  sizeof(a->load) - 1);
      s_arrival_count++;
    }
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Grouped: %d services", s_arrival_count);

if (s_arrivals_menu_layer) {
    // Save current position before reload
    MenuIndex current = menu_layer_get_selected_index(s_arrivals_menu_layer);
    menu_layer_reload_data(s_arrivals_menu_layer);
    // Restore position — clamp to new count in case list shrank
    int restore_row = current.row;
    if (restore_row >= s_arrival_count) restore_row = s_arrival_count > 0 ? s_arrival_count - 1 : 0;
    menu_layer_set_selected_index(s_arrivals_menu_layer,
      MenuIndex(0, restore_row), MenuRowAlignCenter, false);
  }
}

static void parse_nearby(const char *data) {
  s_nearby_count = 0;
  const char *p = data;

  while (*p && s_nearby_count < MAX_NEARBY) {
    NearbyStop *n = &s_nearby[s_nearby_count];
    memset(n, 0, sizeof(NearbyStop));

    const char *sep1 = strchr(p, '|');
    if (!sep1) break;
    int len = (int)(sep1 - p);
    if (len <= 0 || len >= 6) break;
    strncpy(n->code, p, len);
    p = sep1 + 1;

    const char *sep2 = strchr(p, '|');
    if (!sep2) break;
    len = (int)(sep2 - p);
    if (len <= 0 || len >= 32) break;
    strncpy(n->desc, p, len);
    p = sep2 + 1;

    char distbuf[8] = "";
    int di = 0;
    while (*p && *p != ',' && di < 7) {
      distbuf[di++] = *p++;
    }
    distbuf[di] = '\0';
    n->dist = atoi(distbuf);

    s_nearby_count++;
    if (*p == ',') p++;
  }

  APP_LOG(APP_LOG_LEVEL_DEBUG, "Nearby stops parsed: %d", s_nearby_count);

  if (s_nearby_menu_layer) {
    menu_layer_reload_data(s_nearby_menu_layer);
  }
}

static void inbox_received(DictionaryIterator *iter, void *ctx) {
  Tuple *t_arrivals = dict_find(iter, KEY_ARRIVALS_JSON);
  Tuple *t_stop     = dict_find(iter, KEY_STOPS_JSON);
  Tuple *t_error    = dict_find(iter, KEY_ERROR);

  if (t_stop) {
    const char *val = t_stop->value->cstring;
    if (strchr(val, '|')) {
      parse_nearby(val);
    } else {
      strncpy(s_stop_label, val, sizeof(s_stop_label) - 1);
      APP_LOG(APP_LOG_LEVEL_DEBUG, "Stop label: %s", s_stop_label);
    }
  }
  if (t_arrivals) {
    parse_arrivals(t_arrivals->value->cstring);
  }
  if (t_error) {
    memset(&s_arrivals[0], 0, sizeof(BusArrival));
    strncpy(s_arrivals[0].service, "Error", sizeof(s_arrivals[0].service) - 1);
    strncpy(s_arrivals[0].eta[0],  t_error->value->cstring, sizeof(s_arrivals[0].eta[0]) - 1);
    s_arrivals[0].eta_count = 1;
    strncpy(s_arrivals[0].load, "", sizeof(s_arrivals[0].load) - 1);
    s_arrival_count = 1;
    if (s_arrivals_menu_layer) menu_layer_reload_data(s_arrivals_menu_layer);
  }
}

// ══════════════════════════════════════════════════════════
// NEARBY WINDOW
// ══════════════════════════════════════════════════════════
static uint16_t nearby_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)(s_nearby_count > 0 ? s_nearby_count : 1);
}

static void nearby_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *cb) {
  if (s_nearby_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "Searching...", "Getting GPS fix", NULL);
    return;
  }
  NearbyStop *n = &s_nearby[idx->row];
  char subtitle[24];
  snprintf(subtitle, sizeof(subtitle), "%s  |  %dm", n->code, n->dist);
  menu_cell_basic_draw(ctx, cell_layer, n->desc, subtitle, NULL);
}

static int16_t nearby_get_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return 44;
}

static void nearby_select_click(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_nearby_count == 0) return;
  NearbyStop *n = &s_nearby[idx->row];
  push_arrivals_window(n->code, n->desc);
}

static void nearby_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_nearby_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_nearby_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows    = nearby_get_num_rows,
    .draw_row        = nearby_draw_row,
    .get_cell_height = nearby_get_row_height,
    .select_click    = nearby_select_click,
  });
  menu_layer_set_click_config_onto_window(s_nearby_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_nearby_menu_layer));
}

static void nearby_window_unload(Window *window) {
  menu_layer_destroy(s_nearby_menu_layer);
  s_nearby_menu_layer = NULL;
  s_nearby_window = NULL;
}

static void push_nearby_window(void) {
  if (s_nearby_window) {
    window_destroy(s_nearby_window);
    s_nearby_window = NULL;
  }
  s_nearby_window = window_create();
  window_set_window_handlers(s_nearby_window, (WindowHandlers){
    .load   = nearby_window_load,
    .unload = nearby_window_unload,
  });
  window_stack_push(s_nearby_window, true);
}

// ══════════════════════════════════════════════════════════
// SEARCH / DIGIT PICKER
// ══════════════════════════════════════════════════════════
static void search_layer_draw(Layer *layer, GContext *ctx) {
  GRect bounds = layer_get_bounds(layer);

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx,
    "Stop Code",
    fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
    GRect(0, 4, bounds.size.w, 20),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentCenter,
    NULL);

  for (int i = 0; i < 5; i++) {
    int x = DIGIT_START_X + i * (DIGIT_W + DIGIT_GAP);
    bool active = (i == s_digit_pos);
    graphics_context_set_fill_color(ctx, active ? GColorWhite : GColorDarkGray);
    graphics_fill_rect(ctx, GRect(x, DIGIT_Y, DIGIT_W, DIGIT_H), 4, GCornersAll);
    char d[2] = {(char)('0' + s_digits[i]), '\0'};
    graphics_context_set_text_color(ctx, active ? GColorBlack : GColorWhite);
    graphics_draw_text(ctx, d,
      fonts_get_system_font(FONT_KEY_GOTHIC_24_BOLD),
      GRect(x, DIGIT_Y + 1, DIGIT_W, DIGIT_H),
      GTextOverflowModeTrailingEllipsis,
      GTextAlignmentCenter,
      NULL);
    if (active) {
      graphics_context_set_fill_color(ctx, GColorBlack);
      graphics_fill_rect(ctx, GRect(x + 2, DIGIT_Y + DIGIT_H - 3, DIGIT_W - 4, 2), 0, GCornersAll);
      graphics_context_set_text_color(ctx, GColorLightGray);
      graphics_draw_text(ctx, "^",
        fonts_get_system_font(FONT_KEY_GOTHIC_14),
        GRect(x, DIGIT_Y - 14, DIGIT_W, 14),
        GTextOverflowModeTrailingEllipsis,
        GTextAlignmentCenter, NULL);
    }
  }

  graphics_context_set_text_color(ctx, GColorLightGray);
  graphics_draw_text(ctx,
    "UP/DN: change  SEL: next/search",
    fonts_get_system_font(FONT_KEY_GOTHIC_14),
    GRect(4, DIGIT_Y + DIGIT_H + 8, bounds.size.w - 8, 30),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentCenter,
    NULL);

  char code_preview[6] = "";
  int preview_len = s_digit_pos < 4 ? s_digit_pos + 1 : 5;
  for (int i = 0; i < preview_len; i++) {
    code_preview[i] = (char)('0' + s_digits[i]);
  }
  code_preview[preview_len] = '\0';

  graphics_context_set_text_color(ctx, GColorWhite);
  graphics_draw_text(ctx, code_preview,
    fonts_get_system_font(FONT_KEY_GOTHIC_18_BOLD),
    GRect(0, DIGIT_Y + DIGIT_H + 40, bounds.size.w, 24),
    GTextOverflowModeTrailingEllipsis,
    GTextAlignmentCenter, NULL);

  if (s_digit_pos == 4) {
    graphics_context_set_text_color(ctx, GColorGreen);
    graphics_draw_text(ctx,
      "SELECT to search",
      fonts_get_system_font(FONT_KEY_GOTHIC_14_BOLD),
      GRect(4, DIGIT_Y + DIGIT_H + 68, bounds.size.w - 8, 20),
      GTextOverflowModeTrailingEllipsis,
      GTextAlignmentCenter, NULL);
  }
}

static void search_up_click(ClickRecognizerRef r, void *ctx) {
  s_digits[s_digit_pos] = (s_digits[s_digit_pos] + 1) % 10;
  layer_mark_dirty(s_search_layer);
}

static void search_down_click(ClickRecognizerRef r, void *ctx) {
  s_digits[s_digit_pos] = (s_digits[s_digit_pos] + 9) % 10;
  layer_mark_dirty(s_search_layer);
}

static void search_select_click(ClickRecognizerRef r, void *ctx) {
  if (s_digit_pos < 4) {
    s_digit_pos++;
    layer_mark_dirty(s_search_layer);
  } else {
    char code[6];
    snprintf(code, sizeof(code), "%d%d%d%d%d",
      s_digits[0], s_digits[1], s_digits[2], s_digits[3], s_digits[4]);
    window_stack_pop(true);
    push_arrivals_window(code, code);
  }
}

static void search_back_click(ClickRecognizerRef r, void *ctx) {
  if (s_digit_pos > 0) {
    s_digits[s_digit_pos] = 0;
    s_digit_pos--;
    layer_mark_dirty(s_search_layer);
  } else {
    window_stack_pop(true);
  }
}

static void search_click_config(void *ctx) {
  window_single_click_subscribe(BUTTON_ID_UP,     search_up_click);
  window_single_click_subscribe(BUTTON_ID_DOWN,   search_down_click);
  window_single_click_subscribe(BUTTON_ID_SELECT, search_select_click);
  window_single_click_subscribe(BUTTON_ID_BACK,   search_back_click);
}

static void search_window_load(Window *window) {
  window_set_background_color(window, GColorBlack);
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_search_layer = layer_create(bounds);
  layer_set_update_proc(s_search_layer, search_layer_draw);
  layer_add_child(root, s_search_layer);
  window_set_click_config_provider(window, search_click_config);
}

static void search_window_unload(Window *window) {
  layer_destroy(s_search_layer);
  s_search_layer = NULL;
  s_search_window = NULL;
}

static void push_search_window(void) {
  for (int i = 0; i < 5; i++) s_digits[i] = 0;
  s_digit_pos = 0;
  s_search_window = window_create();
  window_set_window_handlers(s_search_window, (WindowHandlers){
    .load   = search_window_load,
    .unload = search_window_unload,
  });
  window_stack_push(s_search_window, true);
}

// ══════════════════════════════════════════════════════════
// BOOKMARKS WINDOW
// ══════════════════════════════════════════════════════════
static uint16_t bm_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return (uint16_t)(s_bookmark_count > 0 ? s_bookmark_count : 1);
}

static void bm_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *cb) {
  if (s_bookmark_count == 0) {
    menu_cell_basic_draw(ctx, cell_layer, "No bookmarks", "Long press stop to save", NULL);
    return;
  }
  Bookmark *b = &s_bookmarks[idx->row];
  menu_cell_basic_draw(ctx, cell_layer, b->code, b->label, NULL);
}

static int16_t bm_get_row_height(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  return 44;
}

static void bm_select_click(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  if (s_bookmark_count == 0) return;
  Bookmark *b = &s_bookmarks[idx->row];
  push_arrivals_window(b->code, b->label);
}

static void bm_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_bm_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_bm_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows    = bm_get_num_rows,
    .draw_row        = bm_draw_row,
    .get_cell_height = bm_get_row_height,
    .select_click    = bm_select_click,
  });
  menu_layer_set_click_config_onto_window(s_bm_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_bm_menu_layer));
}

static void bm_window_unload(Window *window) {
  menu_layer_destroy(s_bm_menu_layer);
  s_bm_menu_layer = NULL;
  s_bm_window = NULL;
}

static void push_bookmarks_window(void) {
  s_bm_window = window_create();
  window_set_window_handlers(s_bm_window, (WindowHandlers){
    .load   = bm_window_load,
    .unload = bm_window_unload,
  });
  window_stack_push(s_bm_window, true);
}

// ══════════════════════════════════════════════════════════
// MAIN MENU
// ══════════════════════════════════════════════════════════
static uint16_t main_get_num_rows(MenuLayer *ml, uint16_t section, void *ctx) {
  return MAIN_MENU_COUNT;
}

static void main_draw_row(GContext *ctx, const Layer *cell_layer, MenuIndex *idx, void *cb) {
  menu_cell_basic_draw(ctx, cell_layer, MAIN_MENU_ITEMS[idx->row], NULL, NULL);
}

static void main_select_click(MenuLayer *ml, MenuIndex *idx, void *ctx) {
  switch (idx->row) {
    case 0:
      s_nearby_count = 0;
      push_nearby_window();
      request_nearby();
      break;
    case 1:
      push_bookmarks_window();
      break;
    case 2:
      push_search_window();
      break;
  }
}

static void main_window_load(Window *window) {
  Layer *root = window_get_root_layer(window);
  GRect bounds = layer_get_bounds(root);
  s_main_menu_layer = menu_layer_create(bounds);
  menu_layer_set_callbacks(s_main_menu_layer, NULL, (MenuLayerCallbacks){
    .get_num_rows = main_get_num_rows,
    .draw_row     = main_draw_row,
    .select_click = main_select_click,
  });
  menu_layer_set_click_config_onto_window(s_main_menu_layer, window);
  layer_add_child(root, menu_layer_get_layer(s_main_menu_layer));
}

static void main_window_unload(Window *window) {
  menu_layer_destroy(s_main_menu_layer);
}

// ══════════════════════════════════════════════════════════
// INIT / DEINIT
// ══════════════════════════════════════════════════════════
static void init(void) {
  bookmarks_load();
  app_message_open(4096, 512);
  app_message_register_inbox_received(inbox_received);
  s_main_window = window_create();
  window_set_window_handlers(s_main_window, (WindowHandlers){
    .load   = main_window_load,
    .unload = main_window_unload,
  });
  window_stack_push(s_main_window, true);
}

static void deinit(void) {
  if (s_refresh_timer) {
    app_timer_cancel(s_refresh_timer);
    s_refresh_timer = NULL;
  }
  window_destroy(s_main_window);
  if (s_arrivals_window) window_destroy(s_arrivals_window);
  if (s_nearby_window)   window_destroy(s_nearby_window);
}

int main(void) {
  init();
  app_event_loop();
  deinit();
}