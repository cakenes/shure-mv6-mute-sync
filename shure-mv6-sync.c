/* SPDX-License-Identifier: GPL-2.0 */
/*
 * shure-mv6-sync: bidirectional mute sync daemon for Shure MV6
 *
 * Polls the kernel sysfs mute file for hardware state changes and syncs
 * them to the default PipeWire/PulseAudio source, and vice versa.
 *
 * Build: gcc -o shure-mv6-sync shure-mv6-sync.c $(pkg-config --cflags --libs
 * libpulse)
 */
#include <dirent.h>
#include <pulse/pulseaudio.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <unistd.h>

#define VENDOR_ID "14ed"
#define PRODUCT_ID "1026"
#define POLL_US 200000 /* sysfs poll interval: 200ms */

static char g_mute_path[512];
static pa_mainloop *g_ml;
static pa_mainloop_api *g_api;
static pa_context *g_ctx;
static pa_time_event *g_timer;

static int g_last_hw = -1; /* last known hardware mute state */
static int g_last_os = -1; /* last known OS mute state */
static int g_syncing =
    0; /* set while we are pushing a change to prevent loops */
static volatile int g_quit = 0;
static int g_exit_code = 0;

/* ---- signal handling ---- */

static void sig_handler(int s) { g_quit = 1; }

/* ---- sysfs helpers ---- */

static int find_mute_path(char *out, size_t len) {
  DIR *d = opendir("/sys/bus/hid/devices");
  if (!d)
    return -1;

  struct dirent *e;
  while ((e = readdir(d))) {
    if (e->d_name[0] == '.')
      continue;

    char path[512], buf[16];
    FILE *f;

    snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/../../idVendor",
             e->d_name);
    if (!(f = fopen(path, "r")))
      continue;
    int ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok || strncasecmp(buf, VENDOR_ID, strlen(VENDOR_ID)) != 0)
      continue;

    snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/../../idProduct",
             e->d_name);
    if (!(f = fopen(path, "r")))
      continue;
    ok = fgets(buf, sizeof(buf), f) != NULL;
    fclose(f);
    if (!ok || strncasecmp(buf, PRODUCT_ID, strlen(PRODUCT_ID)) != 0)
      continue;

    snprintf(path, sizeof(path), "/sys/bus/hid/devices/%s/mute", e->d_name);
    if (access(path, R_OK | W_OK) == 0) {
      snprintf(out, len, "%s", path);
      closedir(d);
      return 0;
    }
  }

  closedir(d);
  return -1;
}

static int read_hw(void) {
  FILE *f = fopen(g_mute_path, "r");
  if (!f)
    return -1;
  int v;
  int ok = fscanf(f, "%d", &v) == 1;
  fclose(f);
  return ok ? v : -1;
}

static void write_hw(int v) {
  FILE *f = fopen(g_mute_path, "w");
  if (!f)
    return;
  fprintf(f, "%d\n", v);
  fclose(f);
}

/* ---- PulseAudio callbacks ---- */

static void set_mute_done_cb(pa_context *c, int success, void *ud) {
  g_syncing = 0;
}

/* Called when we get source info in response to a subscribe event */
static void source_info_cb(pa_context *c, const pa_source_info *info, int eol,
                           void *ud) {
  if (eol || !info)
    return;

  int os = info->mute;
  if (os == g_last_os)
    return;
  g_last_os = os;

  /* Ignore if we triggered this change ourselves */
  if (g_syncing)
    return;

  if (os != g_last_hw) {
    g_syncing = 1;
    write_hw(os);
    g_last_hw = os;
    g_syncing = 0;
  }
}

static void subscribe_cb(pa_context *c, pa_subscription_event_type_t t,
                         uint32_t idx, void *ud) {
  unsigned fac = t & PA_SUBSCRIPTION_EVENT_FACILITY_MASK;
  unsigned typ = t & PA_SUBSCRIPTION_EVENT_TYPE_MASK;

  if (fac == PA_SUBSCRIPTION_EVENT_SOURCE &&
      typ == PA_SUBSCRIPTION_EVENT_CHANGE) {
    pa_operation *op = pa_context_get_source_info_by_name(c, "@DEFAULT_SOURCE@",
                                                          source_info_cb, NULL);
    if (op)
      pa_operation_unref(op);
  }
}

/* ---- sysfs poll timer (runs inside PA mainloop) ---- */

static void reschedule_timer(pa_time_event *te) {
  struct timeval tv;
  gettimeofday(&tv, NULL);
  tv.tv_usec += POLL_US;
  tv.tv_sec += tv.tv_usec / 1000000;
  tv.tv_usec %= 1000000;
  g_api->time_restart(te, &tv);
}

static void timer_cb(pa_mainloop_api *api, pa_time_event *te,
                     const struct timeval *tv, void *ud) {
  int hw = read_hw();
  if (hw >= 0 && hw != g_last_hw) {
    g_last_hw = hw;
    if (!g_syncing) {
      g_syncing = 1;
      pa_operation *op = pa_context_set_source_mute_by_name(
          g_ctx, "@DEFAULT_SOURCE@", hw, set_mute_done_cb, NULL);
      if (op)
        pa_operation_unref(op);
      g_last_os = hw;
    }
  }
  reschedule_timer(te);
}

/* ---- PA context state ---- */

static void context_state_cb(pa_context *c, void *ud) {
  switch (pa_context_get_state(c)) {
  case PA_CONTEXT_READY: {
    pa_context_set_subscribe_callback(c, subscribe_cb, NULL);
    pa_operation *op =
        pa_context_subscribe(c, PA_SUBSCRIPTION_MASK_SOURCE, NULL, NULL);
    if (op)
      pa_operation_unref(op);

    /* Read initial OS state */
    op = pa_context_get_source_info_by_name(c, "@DEFAULT_SOURCE@",
                                            source_info_cb, NULL);
    if (op)
      pa_operation_unref(op);

    /* Start sysfs poll timer */
    struct timeval tv = {0, POLL_US};
    g_timer = g_api->time_new(g_api, &tv, timer_cb, NULL);
    break;
  }
  case PA_CONTEXT_FAILED:
  case PA_CONTEXT_TERMINATED:
    g_exit_code = 1;
    g_quit = 1;
    break;
  default:
    break;
  }
}

/* ---- main ---- */

int main(void) {
  signal(SIGINT, sig_handler);
  signal(SIGTERM, sig_handler);

  if (find_mute_path(g_mute_path, sizeof(g_mute_path)) != 0) {
    fprintf(stderr, "shure-mv6-sync: Shure MV6 not found\n");
    return 1;
  }
  fprintf(stderr, "shure-mv6-sync: using %s\n", g_mute_path);

  g_last_hw = read_hw();

  g_ml = pa_mainloop_new();
  g_api = pa_mainloop_get_api(g_ml);
  g_ctx = pa_context_new(g_api, "shure-mv6-sync");

  pa_context_set_state_callback(g_ctx, context_state_cb, NULL);
  pa_context_connect(g_ctx, NULL, PA_CONTEXT_NOFLAGS, NULL);

  int ret = 0;
  while (!g_quit)
    pa_mainloop_iterate(g_ml, 1, &ret);

  if (g_timer)
    g_api->time_free(g_timer);
  pa_context_disconnect(g_ctx);
  pa_context_unref(g_ctx);
  pa_mainloop_free(g_ml);

  return g_exit_code;
}
