#include "pslog_internal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <float.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <unistd.h>
extern struct tm *gmtime_r(const time_t *timer, struct tm *result);
extern struct tm *localtime_r(const time_t *timer, struct tm *result);
#endif

#define PSLOG_DEFAULT_OUTPUT_FILE_MODE 0600

typedef struct pslog_tee_output {
  pslog_output first;
  pslog_output second;
} pslog_tee_output;

typedef struct pslog_fd_output {
  int fd;
} pslog_fd_output;

static int pslog_file_write(void *userdata, const char *data, size_t len,
                            size_t *written);
static int pslog_file_close(void *userdata);
static int pslog_file_isatty(void *userdata);
static int pslog_fd_write(void *userdata, const char *data, size_t len,
                          size_t *written);
static int pslog_fd_close(void *userdata);
static int pslog_fd_isatty(void *userdata);
static int pslog_tee_write(void *userdata, const char *data, size_t len,
                           size_t *written);
static int pslog_tee_close(void *userdata);
static int pslog_tee_isatty(void *userdata);
static int pslog_observed_write(void *userdata, const char *data, size_t len,
                                size_t *written);
static int pslog_observed_close(void *userdata);
static int pslog_observed_isatty(void *userdata);

static void pslog_logger_destroy(pslog_logger *log);
static int pslog_logger_close(pslog_logger *log);
static pslog_logger *pslog_logger_with(pslog_logger *log,
                                       const pslog_field *fields, size_t count);
static pslog_logger *pslog_logger_withf(pslog_logger *log, const char *kvfmt,
                                        ...);
static pslog_logger *pslog_logger_with_level(pslog_logger *log,
                                             pslog_level level);
static pslog_logger *pslog_logger_with_level_field(pslog_logger *log);
static void pslog_logger_log(pslog_logger *log, pslog_level level,
                             const char *msg, const pslog_field *fields,
                             size_t count);
static void pslog_logger_trace(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count);
static void pslog_logger_debug(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count);
static void pslog_logger_info(pslog_logger *log, const char *msg,
                              const pslog_field *fields, size_t count);
static void pslog_logger_warn(pslog_logger *log, const char *msg,
                              const pslog_field *fields, size_t count);
static void pslog_logger_error(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count);
static void pslog_logger_fatal(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count);
static void pslog_logger_panic(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count);
static void pslog_logger_logf(pslog_logger *log, pslog_level level,
                              const char *msg, const char *kvfmt, ...);
static void pslog_logger_tracef(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...);
static void pslog_logger_debugf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...);
static void pslog_logger_infof(pslog_logger *log, const char *msg,
                               const char *kvfmt, ...);
static void pslog_logger_warnf(pslog_logger *log, const char *msg,
                               const char *kvfmt, ...);
static void pslog_logger_errorf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...);
static void pslog_logger_fatalf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...);
static void pslog_logger_panicf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...);
static void pslog_logger_log_view(pslog_logger *log, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count);
static void pslog_logger_logf_view(pslog_logger *log, pslog_level level,
                                   pslog_string_view msg, const char *kvfmt,
                                   ...);
static void pslog_logger_trace_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count);
static void pslog_logger_tracef_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...);
static void pslog_logger_debug_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count);
static void pslog_logger_debugf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...);
static void pslog_logger_info_view(pslog_logger *log, pslog_string_view msg,
                                   const pslog_field *fields, size_t count);
static void pslog_logger_infof_view(pslog_logger *log, pslog_string_view msg,
                                    const char *kvfmt, ...);
static void pslog_logger_warn_view(pslog_logger *log, pslog_string_view msg,
                                   const pslog_field *fields, size_t count);
static void pslog_logger_warnf_view(pslog_logger *log, pslog_string_view msg,
                                    const char *kvfmt, ...);
static void pslog_logger_error_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count);
static void pslog_logger_errorf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...);
static void pslog_logger_fatal_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count);
static void pslog_logger_fatalf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...);
static void pslog_logger_panic_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count);
static void pslog_logger_panicf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...);

static pslog_logger *pslog_allocate_logger(pslog_shared_state *shared);
static int pslog_copy_fields(pslog_field **out_fields, size_t *out_count,
                             const pslog_field *base_fields, size_t base_count,
                             const pslog_field *new_fields, size_t new_count);
static void pslog_free_fields(pslog_field *fields, size_t count);
static int pslog_parse_bool_value(const char *text, int *value);
static int pslog_parse_mode_value(const char *text, pslog_mode *mode);
static int pslog_parse_file_mode(const char *text, int *mode);
static const char *pslog_env_prefix(const char *prefix);
static int pslog_env_copy(char *dst, size_t dst_size, const char *prefix,
                          const char *suffix);
static void pslog_trim_copy(char *dst, size_t dst_size, const char *text);
static int pslog_string_iequal(const char *lhs, const char *rhs);
static int pslog_string_has_prefix_ci(const char *text, const char *prefix);
static int pslog_is_utf8_continuation(unsigned char ch);
static size_t pslog_utf8_sequence_len_n(const unsigned char *cursor,
                                        size_t remaining);
static int pslog_string_is_ascii_trusted_measure(const char *text,
                                                 size_t *len_out);
static int pslog_string_is_trusted_measure(const char *text, size_t *len_out);
static unsigned long pslog_kvfmt_hash(const char *kvfmt);
static int pslog_parse_kvfmt_layout(pslog_kvfmt_cache_entry *entry,
                                    const char *kvfmt, int own_storage);
static int pslog_materialize_kvfmt_fields(const pslog_kvfmt_cache_entry *entry,
                                          pslog_field *fields,
                                          size_t *out_count, pslog_mode mode,
                                          int saved_errno, va_list ap);
static int pslog_resolve_kvfmt_entry(pslog_shared_state *shared,
                                     const char *kvfmt,
                                     pslog_kvfmt_cache_entry *local_entry,
                                     const pslog_kvfmt_cache_entry **out_entry);
static size_t
pslog_estimate_field_prefix_capacity(const pslog_field *fields, size_t count,
                                     pslog_mode mode, int color_enabled,
                                     const pslog_shared_state *shared);
static int pslog_build_field_prefix(pslog_logger_impl *impl);
static pslog_non_finite_float_policy
pslog_normalize_non_finite_float_policy(pslog_non_finite_float_policy policy);
static int pslog_size_add(size_t lhs, size_t rhs, size_t *out);
static int pslog_size_mul(size_t lhs, size_t rhs, size_t *out);
static int pslog_normalize_line_buffer_capacity(size_t capacity,
                                                size_t *normalized_out);
static int pslog_double_is_nan(double value);
static int pslog_double_is_inf(double value);
static size_t pslog_cstr_len_or_zero(const char *text);
static int pslog_string_is_console_simple_measure(const char *text,
                                                  size_t *len_out);
static void pslog_reset_kvfmt_cache_entry(pslog_kvfmt_cache_entry *entry);
static pslog_logger *pslog_vwithf_impl(pslog_logger *log, const char *kvfmt,
                                       va_list ap);
static void pslog_buffer_append_console_escaped(pslog_buffer *buffer,
                                                const char *text, size_t len,
                                                int quote);
static int pslog_output_init_file_perms(pslog_output *output, const char *path,
                                        const char *mode, int file_mode);
static int pslog_output_init_env_value(pslog_output *output,
                                       const pslog_output *base_output,
                                       const char *value, int file_mode);
static int pslog_output_init_tee(pslog_output *output, pslog_output first,
                                 pslog_output second);
static void pslog_output_reset(pslog_output *output);
static int pslog_output_equals(const pslog_output *lhs,
                               const pslog_output *rhs);
static int pslog_output_contains(const pslog_output *output,
                                 const pslog_output *candidate);
static int pslog_mutex_init(pslog_mutex *mutex);
static void pslog_mutex_destroy(pslog_mutex *mutex);
static void pslog_mutex_lock(pslog_mutex *mutex);
static void pslog_mutex_unlock(pslog_mutex *mutex);
static int pslog_output_lock(pslog_shared_state *shared);
static void pslog_output_unlock(pslog_shared_state *shared);
static int pslog_state_lock(pslog_shared_state *shared);
static void pslog_state_unlock(pslog_shared_state *shared);
static int pslog_close_shared_locked(pslog_shared_state *shared,
                                     int allow_close);
static int pslog_time_to_tm(time_t now, int utc, struct tm *tm_value);
static int pslog_local_offset_minutes(time_t now);
static void pslog_default_time_now(void *userdata, time_t *seconds_out,
                                   long *nanoseconds_out);
static time_t pslog_default_time_now_seconds(void);
static int pslog_time_format_is_rfc3339(const char *fmt);
static int pslog_time_format_is_rfc3339nano(const char *fmt);
static void pslog_errno_fallback(int err, char *buffer, size_t buffer_size);
static size_t pslog_render_timestamp_value(pslog_shared_state *shared,
                                           time_t now, long nanoseconds,
                                           char *dst, size_t dst_size);
static void pslog_build_console_level_fragment(pslog_shared_state *shared,
                                               pslog_level level, size_t slot);
static void pslog_append_json_color_string(pslog_buffer *buffer,
                                           const char *color, size_t color_len,
                                           const char *text, size_t text_len,
                                           const char *suffix,
                                           size_t suffix_len);
static void pslog_build_json_color_literals(pslog_shared_state *shared);
static void pslog_build_console_literals(pslog_shared_state *shared);
static void pslog_prepare_kvfmt_cache_entry(pslog_shared_state *shared,
                                            pslog_kvfmt_cache_entry *entry);
static size_t pslog_write_output_locked(pslog_shared_state *shared,
                                        const char *data, size_t len,
                                        size_t chunk_capacity);

#if defined(PSLOG_TEST_HOOKS)
static pslog_test_allocator_stats pslog_test_alloc_stats;
static unsigned long pslog_test_alloc_fail_after;

static int pslog_test_allocator_should_fail(void) {
  unsigned long calls;

  if (pslog_test_alloc_fail_after == 0u) {
    return 0;
  }
  calls = pslog_test_alloc_stats.malloc_calls +
          pslog_test_alloc_stats.calloc_calls +
          pslog_test_alloc_stats.realloc_calls;
  return calls >= pslog_test_alloc_fail_after;
}
#endif

void *pslog_malloc_internal(size_t size) {
#if defined(PSLOG_TEST_HOOKS)
  pslog_test_alloc_stats.malloc_calls += 1u;
  if (pslog_test_allocator_should_fail()) {
    return NULL;
  }
#endif
  return malloc(size);
}

void *pslog_calloc_internal(size_t count, size_t size) {
  size_t total_size;

#if defined(PSLOG_TEST_HOOKS)
  pslog_test_alloc_stats.calloc_calls += 1u;
  if (pslog_test_allocator_should_fail()) {
    return NULL;
  }
#endif
  if (!pslog_size_mul(count, size, &total_size)) {
    return NULL;
  }
  return calloc(count, size);
}

void *pslog_realloc_internal(void *ptr, size_t size) {
#if defined(PSLOG_TEST_HOOKS)
  pslog_test_alloc_stats.realloc_calls += 1u;
  if (pslog_test_allocator_should_fail()) {
    return NULL;
  }
#endif
  return realloc(ptr, size);
}

void pslog_free_internal(void *ptr) {
#if defined(PSLOG_TEST_HOOKS)
  if (ptr != NULL) {
    pslog_test_alloc_stats.free_calls += 1u;
  }
#endif
  free(ptr);
}

#if defined(PSLOG_TEST_HOOKS)
void pslog_test_allocator_reset(void) {
  memset(&pslog_test_alloc_stats, 0, sizeof(pslog_test_alloc_stats));
  pslog_test_alloc_fail_after = 0u;
}

void pslog_test_allocator_get(pslog_test_allocator_stats *stats) {
  if (stats != NULL) {
    *stats = pslog_test_alloc_stats;
  }
}

void pslog_test_allocator_fail_after(unsigned long calls) {
  pslog_test_alloc_fail_after = calls;
}
#endif

void pslog_default_config(pslog_config *config) {
  if (config == NULL) {
    return;
  }
  memset(config, 0, sizeof(*config));
  config->mode = PSLOG_MODE_CONSOLE;
  config->color = PSLOG_COLOR_AUTO;
  config->non_finite_float_policy = PSLOG_NON_FINITE_FLOAT_AS_STRING;
  config->line_buffer_capacity = PSLOG_DEFAULT_LINE_BUFFER_CAPACITY;
  config->min_level = PSLOG_LEVEL_DEBUG;
  config->timestamps = 1;
  config->utc = 0;
  config->with_level_field = 0;
  config->verbose_fields = 0;
  config->time_format = NULL;
  config->palette = pslog_palette_default();
  config->output = pslog_output_from_fp(stdout, 0);
}

pslog_output pslog_output_from_fp(FILE *fp, int close_on_destroy) {
  pslog_output output;

  output.write = pslog_file_write;
  output.close = close_on_destroy ? pslog_file_close : NULL;
  output.isatty = pslog_file_isatty;
  output.userdata = fp;
  output.owned = close_on_destroy ? 1 : 0;
  return output;
}

int pslog_output_init_file(pslog_output *output, const char *path,
                           const char *mode) {
  return pslog_output_init_file_perms(output, path, mode, 0666);
}

void pslog_output_destroy(pslog_output *output) {
  if (output == NULL) {
    return;
  }
  if (output->close != NULL && output->owned) {
    (void)output->close(output->userdata);
  }
  memset(output, 0, sizeof(*output));
}

void pslog_observed_output_init(pslog_observed_output *observed,
                                const pslog_output *target,
                                pslog_write_failure_fn on_failure,
                                void *failure_userdata) {
  if (observed == NULL) {
    return;
  }
  memset(observed, 0, sizeof(*observed));
  if (target != NULL) {
    observed->target = *target;
  }
  observed->on_failure = on_failure;
  observed->failure_userdata = failure_userdata;
  observed->output.write = pslog_observed_write;
  observed->output.close = pslog_observed_close;
  observed->output.isatty =
      observed->target.isatty != NULL ? pslog_observed_isatty : NULL;
  observed->output.userdata = observed;
  observed->output.owned = observed->target.owned;
}

pslog_observed_output_stats
pslog_observed_output_stats_get(const pslog_observed_output *observed) {
  pslog_observed_output_stats stats;

  stats.failures = 0u;
  stats.short_writes = 0u;
  if (observed != NULL) {
    stats.failures = observed->failures;
    stats.short_writes = observed->short_writes;
  }
  return stats;
}

int pslog_close(pslog_logger *log) {
  pslog_logger_impl *impl;

  if (log == NULL) {
    return 0;
  }
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL) {
    return 0;
  }
  return pslog_close_shared(impl->shared, impl->can_close_shared);
}

const char *pslog_level_string(pslog_level level) {
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return "trace";
  case PSLOG_LEVEL_DEBUG:
    return "debug";
  case PSLOG_LEVEL_INFO:
    return "info";
  case PSLOG_LEVEL_WARN:
    return "warn";
  case PSLOG_LEVEL_ERROR:
    return "error";
  case PSLOG_LEVEL_FATAL:
    return "fatal";
  case PSLOG_LEVEL_PANIC:
    return "panic";
  case PSLOG_LEVEL_NOLEVEL:
    return "nolevel";
  case PSLOG_LEVEL_DISABLED:
    return "disabled";
  default:
    return "info";
  }
}

int pslog_parse_level(const char *text, pslog_level *level) {
  char normalized[16];
  size_t i;

  if (text == NULL || level == NULL) {
    return 0;
  }
  pslog_trim_copy(normalized, sizeof(normalized), text);
  i = 0;
  while (normalized[i] != '\0') {
    normalized[i] = (char)tolower((unsigned char)normalized[i]);
    ++i;
  }

  if (strcmp(normalized, "trace") == 0) {
    *level = PSLOG_LEVEL_TRACE;
    return 1;
  }
  if (strcmp(normalized, "debug") == 0) {
    *level = PSLOG_LEVEL_DEBUG;
    return 1;
  }
  if (strcmp(normalized, "info") == 0) {
    *level = PSLOG_LEVEL_INFO;
    return 1;
  }
  if (strcmp(normalized, "warn") == 0 || strcmp(normalized, "warning") == 0) {
    *level = PSLOG_LEVEL_WARN;
    return 1;
  }
  if (strcmp(normalized, "error") == 0) {
    *level = PSLOG_LEVEL_ERROR;
    return 1;
  }
  if (strcmp(normalized, "fatal") == 0) {
    *level = PSLOG_LEVEL_FATAL;
    return 1;
  }
  if (strcmp(normalized, "panic") == 0) {
    *level = PSLOG_LEVEL_PANIC;
    return 1;
  }
  if (strcmp(normalized, "no") == 0 || strcmp(normalized, "none") == 0 ||
      strcmp(normalized, "nolevel") == 0) {
    *level = PSLOG_LEVEL_NOLEVEL;
    return 1;
  }
  if (strcmp(normalized, "disabled") == 0 || strcmp(normalized, "off") == 0) {
    *level = PSLOG_LEVEL_DISABLED;
    return 1;
  }
  return 0;
}

static int pslog_is_utf8_continuation(unsigned char ch) {
  return ch >= 0x80u && ch <= 0xbfu;
}

static size_t pslog_utf8_sequence_len_n(const unsigned char *cursor,
                                        size_t remaining) {
  unsigned char ch;

  if (cursor == NULL || remaining == 0u) {
    return 0u;
  }
  ch = *cursor;
  if (ch < 0x80u) {
    return 1u;
  }
  if (ch >= 0xc2u && ch <= 0xdfu) {
    if (remaining < 2u || !pslog_is_utf8_continuation(cursor[1])) {
      return 0u;
    }
    return 2u;
  }
  if (ch == 0xe0u) {
    if (remaining < 3u || cursor[1] < 0xa0u || cursor[1] > 0xbfu ||
        !pslog_is_utf8_continuation(cursor[2])) {
      return 0u;
    }
    return 3u;
  }
  if ((ch >= 0xe1u && ch <= 0xecu) || (ch >= 0xeeu && ch <= 0xefu)) {
    if (remaining < 3u || !pslog_is_utf8_continuation(cursor[1]) ||
        !pslog_is_utf8_continuation(cursor[2])) {
      return 0u;
    }
    return 3u;
  }
  if (ch == 0xedu) {
    if (remaining < 3u || cursor[1] < 0x80u || cursor[1] > 0x9fu ||
        !pslog_is_utf8_continuation(cursor[2])) {
      return 0u;
    }
    return 3u;
  }
  if (ch == 0xf0u) {
    if (remaining < 4u || cursor[1] < 0x90u || cursor[1] > 0xbfu ||
        !pslog_is_utf8_continuation(cursor[2]) ||
        !pslog_is_utf8_continuation(cursor[3])) {
      return 0u;
    }
    return 4u;
  }
  if (ch >= 0xf1u && ch <= 0xf3u) {
    if (remaining < 4u || !pslog_is_utf8_continuation(cursor[1]) ||
        !pslog_is_utf8_continuation(cursor[2]) ||
        !pslog_is_utf8_continuation(cursor[3])) {
      return 0u;
    }
    return 4u;
  }
  if (ch == 0xf4u) {
    if (remaining < 4u || cursor[1] < 0x80u || cursor[1] > 0x8fu ||
        !pslog_is_utf8_continuation(cursor[2]) ||
        !pslog_is_utf8_continuation(cursor[3])) {
      return 0u;
    }
    return 4u;
  }
  return 0u;
}

static pslog_non_finite_float_policy
pslog_normalize_non_finite_float_policy(pslog_non_finite_float_policy policy) {
  switch (policy) {
  case PSLOG_NON_FINITE_FLOAT_AS_NULL:
    return policy;
  default:
    return PSLOG_NON_FINITE_FLOAT_AS_STRING;
  }
}

static int pslog_size_add(size_t lhs, size_t rhs, size_t *out) {
  if (out == NULL || lhs > (size_t)-1 - rhs) {
    return 0;
  }
  *out = lhs + rhs;
  return 1;
}

static int pslog_size_mul(size_t lhs, size_t rhs, size_t *out) {
  if (out == NULL) {
    return 0;
  }
  if (lhs != 0u && rhs > (size_t)-1 / lhs) {
    return 0;
  }
  *out = lhs * rhs;
  return 1;
}

static int pslog_normalize_line_buffer_capacity(size_t capacity,
                                                size_t *normalized_out) {
  size_t normalized;

  if (normalized_out == NULL) {
    return 0;
  }
  normalized = capacity == 0u ? PSLOG_DEFAULT_LINE_BUFFER_CAPACITY : capacity;
  if (normalized > (size_t)-1 - 1u) {
    return 0;
  }
  *normalized_out = normalized;
  return 1;
}

static int pslog_double_is_nan(double value) { return value != value; }

static int pslog_double_is_inf(double value) {
  if (pslog_double_is_nan(value)) {
    return 0;
  }
  return value > DBL_MAX || value < -DBL_MAX;
}

static size_t pslog_cstr_len_or_zero(const char *text) {
  return text != NULL ? strlen(text) : 0u;
}

static pslog_string_view pslog_cstr_view(const char *text) {
  pslog_string_view view;
  view.data = text;
  view.len = pslog_cstr_len_or_zero(text);
  return view;
}

static int pslog_string_is_console_simple_measure(const char *text,
                                                  size_t *len_out) {
  const unsigned char *cursor;
  unsigned char ch;
  size_t len;

  len = 0u;
  if (len_out != NULL) {
    *len_out = 0u;
  }
  if (text == NULL || *text == '\0') {
    return 0;
  }
  cursor = (const unsigned char *)text;
  while (*cursor != '\0') {
    ch = *cursor;
    if (((ch >= 0x09u && ch <= 0x0du) || ch == 0x20u) || ch == '=' ||
        ch == '"' || ch == '\\' || ch < 0x20u || ch == 0x7fu || ch == 0x1bu) {
      return 0;
    }
    ++cursor;
    ++len;
  }
  if (len_out != NULL) {
    *len_out = len;
  }
  return 1;
}

static int pslog_string_is_ascii_trusted_measure(const char *text,
                                                 size_t *len_out) {
  const unsigned char *cursor;
  unsigned char ch;
  size_t len;
  int trusted;

  len = 0u;
  trusted = 1;
  if (len_out != NULL) {
    *len_out = 0u;
  }
  if (text == NULL) {
    return 1;
  }

  cursor = (const unsigned char *)text;
  while (*cursor != '\0') {
    ch = *cursor;
    if (ch >= 0x80u || ch < 0x20u || ch == 0x7fu || ch == '"' || ch == '\\') {
      trusted = 0;
    }
    ++cursor;
    ++len;
  }
  if (len_out != NULL) {
    *len_out = len;
  }
  return trusted;
}

static int pslog_string_is_trusted_measure(const char *text, size_t *len_out) {
  const unsigned char *cursor;
  unsigned char ch;
  size_t len;
  int trusted;

  len = 0u;
  trusted = 1;
  if (len_out != NULL) {
    *len_out = 0u;
  }
  if (text == NULL) {
    if (len_out != NULL) {
      *len_out = 0u;
    }
    return 1;
  }
  cursor = (const unsigned char *)text;
  while (*cursor != '\0') {
    ch = *cursor;
    if (ch < 0x80u) {
      if (ch < 0x20u || ch == 0x7fu || ch == '"' || ch == '\\') {
        trusted = 0;
      }
      ++cursor;
      ++len;
      continue;
    }
    if (ch >= 0xc2u && ch <= 0xdfu) {
      if (cursor[1] == '\0' || !pslog_is_utf8_continuation(cursor[1])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 2;
      len += 2u;
      continue;
    }
    if (ch == 0xe0u) {
      if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[1] < 0xa0u ||
          cursor[1] > 0xbfu || !pslog_is_utf8_continuation(cursor[2])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 3;
      len += 3u;
      continue;
    }
    if ((ch >= 0xe1u && ch <= 0xecu) || (ch >= 0xeeu && ch <= 0xefu)) {
      if (cursor[1] == '\0' || cursor[2] == '\0' ||
          !pslog_is_utf8_continuation(cursor[1]) ||
          !pslog_is_utf8_continuation(cursor[2])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 3;
      len += 3u;
      continue;
    }
    if (ch == 0xedu) {
      if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[1] < 0x80u ||
          cursor[1] > 0x9fu || !pslog_is_utf8_continuation(cursor[2])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 3;
      len += 3u;
      continue;
    }
    if (ch == 0xf0u) {
      if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[3] == '\0' ||
          cursor[1] < 0x90u || cursor[1] > 0xbfu ||
          !pslog_is_utf8_continuation(cursor[2]) ||
          !pslog_is_utf8_continuation(cursor[3])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 4;
      len += 4u;
      continue;
    }
    if (ch >= 0xf1u && ch <= 0xf3u) {
      if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[3] == '\0' ||
          !pslog_is_utf8_continuation(cursor[1]) ||
          !pslog_is_utf8_continuation(cursor[2]) ||
          !pslog_is_utf8_continuation(cursor[3])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 4;
      len += 4u;
      continue;
    }
    if (ch == 0xf4u) {
      if (cursor[1] == '\0' || cursor[2] == '\0' || cursor[3] == '\0' ||
          cursor[1] < 0x80u || cursor[1] > 0x8fu ||
          !pslog_is_utf8_continuation(cursor[2]) ||
          !pslog_is_utf8_continuation(cursor[3])) {
        len += strlen((const char *)cursor);
        if (len_out != NULL) {
          *len_out = len;
        }
        return 0;
      }
      cursor += 4;
      len += 4u;
      continue;
    }
    {
      len += strlen((const char *)cursor);
      if (len_out != NULL) {
        *len_out = len;
      }
      return 0;
    }
  }
  if (len_out != NULL) {
    *len_out = len;
  }
  return trusted;
}

int pslog_string_is_trusted(const char *text) {
  return pslog_string_is_trusted_measure(text, (size_t *)0);
}

int pslog_string_is_console_simple_internal(const char *text) {
  return pslog_string_is_console_simple_measure(text, (size_t *)0);
}

static void pslog_errno_fallback(int err, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0u) {
    return;
  }
  snprintf(buffer, buffer_size, "Unknown error %d", err);
  buffer[buffer_size - 1u] = '\0';
}

const char *pslog_errno_string(int err, char *buffer, size_t buffer_size) {
  if (buffer == NULL || buffer_size == 0u) {
    return "";
  }
  buffer[0] = '\0';
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  {
    char *message;

    message = strerror_r(err, buffer, buffer_size);
    if (message != NULL) {
      if (message != buffer) {
        strncpy(buffer, message, buffer_size - 1u);
        buffer[buffer_size - 1u] = '\0';
      }
      return buffer;
    }
  }
#else
  if (strerror_r(err, buffer, buffer_size) == 0) {
    return buffer;
  }
#endif
  pslog_errno_fallback(err, buffer, buffer_size);
  return buffer;
}

static unsigned long pslog_kvfmt_hash(const char *kvfmt) {
  unsigned long hash;
  const unsigned char *cursor;

  hash = 1469598103ul;
  cursor = (const unsigned char *)kvfmt;
  if (cursor == NULL) {
    return hash;
  }
  while (*cursor != '\0') {
    hash ^= (unsigned long)*cursor;
    hash *= 16777619ul;
    ++cursor;
  }
  return hash;
}

static size_t pslog_kvfmt_pointer_slot(const char *kvfmt) {
  size_t value;

  value = (size_t)(const void *)kvfmt;
  value ^= value >> 4u;
  value ^= value >> 9u;
  return value % PSLOG_KVFMT_PTR_CACHE_SIZE;
}

static void pslog_reset_kvfmt_cache_entry(pslog_kvfmt_cache_entry *entry) {
  if (entry == NULL) {
    return;
  }
  pslog_free_internal(entry->kvfmt_storage);
  memset(entry, 0, sizeof(*entry));
}

static int pslog_parse_kvfmt_layout(pslog_kvfmt_cache_entry *entry,
                                    const char *kvfmt, int own_storage) {
  const char *cursor;
  const char *key_start;
  size_t key_len;
  size_t field_count;
  int long_flag;

  if (entry == NULL) {
    return -1;
  }
  pslog_reset_kvfmt_cache_entry(entry);
  if (kvfmt == NULL) {
    return 0;
  }
  entry->kvfmt_len = strlen(kvfmt);
  entry->kvfmt_hash = pslog_kvfmt_hash(kvfmt);
  if (own_storage) {
    entry->kvfmt_storage = pslog_strdup_local(kvfmt);
    if (entry->kvfmt_storage == NULL) {
      return -1;
    }
    entry->kvfmt = entry->kvfmt_storage;
  } else {
    entry->kvfmt = kvfmt;
  }

  cursor = entry->kvfmt;
  field_count = 0u;
  while (*cursor != '\0') {
    while (*cursor == ' ') {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    if (field_count >= PSLOG_KVFMT_MAX_FIELDS) {
      pslog_reset_kvfmt_cache_entry(entry);
      return -1;
    }
    key_start = cursor;
    key_len = 0u;
    while (cursor[0] != '\0' && cursor[1] != '\0') {
      if (cursor[0] == '=' && cursor[1] == '%') {
        break;
      }
      if ((unsigned char)*cursor < 0x21u || *cursor == '"' || *cursor == '\\' ||
          *cursor == 0x7f || *cursor == ' ') {
        pslog_reset_kvfmt_cache_entry(entry);
        return -1;
      }
      ++cursor;
      ++key_len;
    }
    if (key_len == 0u || cursor[0] != '=' || cursor[1] != '%') {
      pslog_reset_kvfmt_cache_entry(entry);
      return -1;
    }
    cursor += 2;
    long_flag = 0;
    if (*cursor == 'l') {
      long_flag = 1;
      ++cursor;
    }
    entry->fields[field_count].key = key_start;
    entry->fields[field_count].key_len = key_len;
    switch (*cursor) {
    case 's':
      entry->fields[field_count].arg_type = PSLOG_KVFMT_ARG_STRING;
      break;
    case 'd':
      entry->fields[field_count].arg_type =
          long_flag ? PSLOG_KVFMT_ARG_SIGNED_LONG : PSLOG_KVFMT_ARG_SIGNED_INT;
      break;
    case 'u':
      entry->fields[field_count].arg_type = long_flag
                                                ? PSLOG_KVFMT_ARG_UNSIGNED_LONG
                                                : PSLOG_KVFMT_ARG_UNSIGNED_INT;
      break;
    case 'f':
      entry->fields[field_count].arg_type = PSLOG_KVFMT_ARG_DOUBLE;
      break;
    case 'p':
      entry->fields[field_count].arg_type = PSLOG_KVFMT_ARG_POINTER;
      break;
    case 'b':
      entry->fields[field_count].arg_type = PSLOG_KVFMT_ARG_BOOL;
      break;
    case 'm':
      if (long_flag) {
        pslog_reset_kvfmt_cache_entry(entry);
        return -1;
      }
      entry->fields[field_count].arg_type = PSLOG_KVFMT_ARG_ERRNO;
      break;
    default:
      pslog_reset_kvfmt_cache_entry(entry);
      return -1;
    }
    ++cursor;
    ++field_count;
    if (*cursor != '\0' && *cursor != ' ') {
      pslog_reset_kvfmt_cache_entry(entry);
      return -1;
    }
  }
  entry->field_count = field_count;
  return 0;
}

static int
pslog_resolve_kvfmt_entry(pslog_shared_state *shared, const char *kvfmt,
                          pslog_kvfmt_cache_entry *local_entry,
                          const pslog_kvfmt_cache_entry **out_entry) {
  pslog_kvfmt_cache_entry *cache_entry;
  const pslog_kvfmt_cache_entry *ptr_cache_entry;
  size_t probe;
  size_t cache_index;
  size_t pointer_slot;
  size_t selected_index;
  unsigned long slot;
  unsigned long kvfmt_hash;
  size_t kvfmt_len;
  int have_kvfmt_len;

  if (out_entry == NULL) {
    return -1;
  }
  *out_entry = NULL;
  if (kvfmt == NULL) {
    return 0;
  }
  have_kvfmt_len = 0;
  if (shared != NULL) {
    pointer_slot = pslog_kvfmt_pointer_slot(kvfmt);
    ptr_cache_entry = shared->kvfmt_ptr_cache != NULL
                          ? shared->kvfmt_ptr_cache[pointer_slot].entry
                          : (const pslog_kvfmt_cache_entry *)0;
    if (shared->kvfmt_ptr_cache != NULL &&
        shared->kvfmt_ptr_cache[pointer_slot].input == kvfmt &&
        ptr_cache_entry != NULL && ptr_cache_entry->kvfmt != NULL) {
      kvfmt_len = strlen(kvfmt);
      have_kvfmt_len = 1;
      if (ptr_cache_entry->kvfmt_len == kvfmt_len &&
          memcmp(ptr_cache_entry->kvfmt, kvfmt, kvfmt_len + 1u) == 0) {
        *out_entry = ptr_cache_entry;
        return 0;
      }
    }
    kvfmt_hash = pslog_kvfmt_hash(kvfmt);
    if (!have_kvfmt_len) {
      kvfmt_len = strlen(kvfmt);
      have_kvfmt_len = 1;
    }
    slot = kvfmt_hash % PSLOG_KVFMT_CACHE_SIZE;
    cache_entry = NULL;
    selected_index = PSLOG_KVFMT_CACHE_SIZE;
    for (probe = 0u; probe < PSLOG_KVFMT_CACHE_PROBE; ++probe) {
      cache_index = (slot + probe) % PSLOG_KVFMT_CACHE_SIZE;
      if (shared->kvfmt_cache[cache_index].kvfmt != NULL &&
          shared->kvfmt_cache[cache_index].kvfmt_hash == kvfmt_hash &&
          shared->kvfmt_cache[cache_index].kvfmt_len == kvfmt_len &&
          memcmp(shared->kvfmt_cache[cache_index].kvfmt, kvfmt, kvfmt_len) ==
              0) {
        cache_entry = &shared->kvfmt_cache[cache_index];
        break;
      }
      if (selected_index == PSLOG_KVFMT_CACHE_SIZE &&
          shared->kvfmt_cache[cache_index].kvfmt == NULL) {
        selected_index = cache_index;
      }
    }
    if (cache_entry == NULL) {
      if (selected_index != PSLOG_KVFMT_CACHE_SIZE) {
        cache_entry = &shared->kvfmt_cache[selected_index];
        if (pslog_parse_kvfmt_layout(cache_entry, kvfmt, 1) != 0) {
          return -1;
        }
        pslog_prepare_kvfmt_cache_entry(shared, cache_entry);
      } else {
        if (local_entry == NULL) {
          return -1;
        }
        local_entry->kvfmt_storage = NULL;
        if (pslog_parse_kvfmt_layout(local_entry, kvfmt, 0) != 0) {
          return -1;
        }
        pslog_prepare_kvfmt_cache_entry(shared, local_entry);
        cache_entry = local_entry;
      }
    }
    if (cache_entry != local_entry) {
      if (shared->kvfmt_ptr_cache != NULL) {
        shared->kvfmt_ptr_cache[pointer_slot].input = kvfmt;
        shared->kvfmt_ptr_cache[pointer_slot].entry = cache_entry;
      }
    }
    *out_entry = cache_entry;
    return 0;
  }

  if (local_entry == NULL) {
    return -1;
  }
  if (pslog_parse_kvfmt_layout(local_entry, kvfmt, 0) != 0) {
    return -1;
  }
  pslog_prepare_kvfmt_cache_entry(shared, local_entry);
  *out_entry = local_entry;
  return 0;
}

static int pslog_materialize_kvfmt_fields(const pslog_kvfmt_cache_entry *entry,
                                          pslog_field *fields,
                                          size_t *out_count, pslog_mode mode,
                                          int saved_errno, va_list ap) {
  const pslog_kvfmt_field_spec *spec;
  size_t field_count;
  unsigned long slot;
  const char *string_value;

  if (out_count == NULL) {
    return -1;
  }
  *out_count = 0u;
  if (entry == NULL) {
    return -1;
  }

  field_count = entry->field_count;
  if (field_count > PSLOG_KVFMT_MAX_FIELDS) {
    return -1;
  }

  for (slot = 0u; slot < field_count; ++slot) {
    spec = &entry->fields[slot];
    fields[slot].key = spec->key;
    fields[slot].key_len = spec->key_len;
    fields[slot].trusted_key = 1u;
    fields[slot].trusted_value = 0u;
    fields[slot].console_simple_value = 0u;
    fields[slot].value_len = 0u;

    switch (spec->arg_type) {
    case PSLOG_KVFMT_ARG_STRING:
      fields[slot].type = PSLOG_FIELD_STRING;
      string_value = va_arg(ap, const char *);
      fields[slot].as.string_value = string_value != NULL ? string_value : "";
      fields[slot].trusted_value =
          (unsigned char)(pslog_string_is_ascii_trusted_measure(
                              fields[slot].as.string_value,
                              &fields[slot].value_len)
                              ? 1
                              : 0);
      if (mode == PSLOG_MODE_CONSOLE) {
        fields[slot].console_simple_value =
            (unsigned char)(pslog_string_is_console_simple_measure(
                                fields[slot].as.string_value, NULL)
                                ? 1
                                : 0);
      }
      break;
    case PSLOG_KVFMT_ARG_SIGNED_INT:
      fields[slot].type = PSLOG_FIELD_SIGNED;
      fields[slot].as.signed_value = (long)va_arg(ap, int);
      break;
    case PSLOG_KVFMT_ARG_SIGNED_LONG:
      fields[slot].type = PSLOG_FIELD_SIGNED;
      fields[slot].as.signed_value = va_arg(ap, long);
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_INT:
      fields[slot].type = PSLOG_FIELD_UNSIGNED;
      fields[slot].as.unsigned_value = (unsigned long)va_arg(ap, unsigned int);
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_LONG:
      fields[slot].type = PSLOG_FIELD_UNSIGNED;
      fields[slot].as.unsigned_value = va_arg(ap, unsigned long);
      break;
    case PSLOG_KVFMT_ARG_DOUBLE:
      fields[slot].type = PSLOG_FIELD_DOUBLE;
      fields[slot].as.double_value = va_arg(ap, double);
      break;
    case PSLOG_KVFMT_ARG_POINTER:
      fields[slot].type = PSLOG_FIELD_POINTER;
      fields[slot].as.pointer_value = va_arg(ap, void *);
      break;
    case PSLOG_KVFMT_ARG_BOOL:
      fields[slot].type = PSLOG_FIELD_BOOL;
      fields[slot].as.bool_value = va_arg(ap, int) ? 1 : 0;
      break;
    case PSLOG_KVFMT_ARG_ERRNO:
      fields[slot].type = PSLOG_FIELD_ERRNO;
      fields[slot].as.signed_value = (pslog_int64)saved_errno;
      break;
    default:
      return -1;
    }
  }

  *out_count = field_count;
  return 0;
}

pslog_field pslog_null(const char *key) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_NULL;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.pointer_value = NULL;
  return field;
}

pslog_field pslog_str(const char *key, const char *value) {
  pslog_field field;
  size_t key_len;
  size_t value_len;
  const char *string_value;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_STRING;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  string_value = value != NULL ? value : "";
  field.trusted_value = (unsigned char)(pslog_string_is_ascii_trusted_measure(
                                            string_value, &value_len)
                                            ? 1
                                            : 0);
  field.console_simple_value = 0u;
  field.value_len = value_len;
  field.as.string_value = string_value;
  return field;
}

pslog_field pslog_errno(const char *key, int err) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_ERRNO;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.signed_value = (pslog_int64)err;
  return field;
}

pslog_field pslog_trusted_str(const char *key, const char *value) {
  pslog_field field;
  size_t key_len;
  size_t value_len;
  const char *string_value;

  field = pslog_str(key, value);
  key_len = 0u;
  value_len = 0u;
  string_value = value != NULL ? value : "";
  field.trusted_key =
      (unsigned char)(pslog_string_is_trusted_measure(key, &key_len) ? 1 : 0);
  field.key_len = key_len;
  field.trusted_value =
      (unsigned char)(pslog_string_is_trusted_measure(string_value, &value_len)
                          ? 1
                          : 0);
  field.value_len = value_len;
  return field;
}

pslog_field pslog_i64(const char *key, pslog_int64 value) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_SIGNED;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.signed_value = value;
  return field;
}

pslog_field pslog_u64(const char *key, pslog_uint64 value) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_UNSIGNED;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.unsigned_value = value;
  return field;
}

pslog_field pslog_f64(const char *key, double value) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_DOUBLE;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.double_value = value;
  return field;
}

pslog_field pslog_bool(const char *key, int value) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_BOOL;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.bool_value = value ? 1 : 0;
  return field;
}

pslog_field pslog_ptr(const char *key, const void *value) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_POINTER;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.pointer_value = value;
  return field;
}

pslog_field pslog_bytes_field(const char *key, const void *data, size_t len) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_BYTES;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.bytes_value.data = (const unsigned char *)data;
  field.as.bytes_value.len = len;
  return field;
}

pslog_field pslog_time_field(const char *key, long epoch_seconds,
                             long nanoseconds, int utc_offset_minutes) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_TIME;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.time_value.epoch_seconds = epoch_seconds;
  field.as.time_value.nanoseconds = nanoseconds;
  field.as.time_value.utc_offset_minutes = utc_offset_minutes;
  return field;
}

pslog_field pslog_duration_field(const char *key, long seconds,
                                 long nanoseconds) {
  pslog_field field;
  size_t key_len;

  field.key = key;
  key_len = 0u;
  field.key_len = 0u;
  field.value_len = 0u;
  field.type = PSLOG_FIELD_DURATION;
  field.trusted_key =
      (unsigned char)(pslog_string_is_ascii_trusted_measure(key, &key_len) ? 1
                                                                           : 0);
  field.key_len = key_len;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.duration_value.seconds = seconds;
  field.as.duration_value.nanoseconds = nanoseconds;
  return field;
}

static pslog_string_view pslog_normalize_view(pslog_string_view view) {
  if (view.data == NULL) {
    view.len = 0u;
  }
  return view;
}

static int pslog_view_is_trusted(pslog_string_view view, int allow_utf8) {
  size_t i;
  size_t sequence_len;
  unsigned char ch;

  view = pslog_normalize_view(view);
  for (i = 0u; i < view.len;) {
    ch = (unsigned char)view.data[i];
    if (ch < 0x20u || ch == 0x7fu || ch == '"' || ch == '\\') {
      return 0;
    }
    if (ch >= 0x80u) {
      if (!allow_utf8) {
        return 0;
      }
      sequence_len = pslog_utf8_sequence_len_n(
          (const unsigned char *)view.data + i, view.len - i);
      if (sequence_len == 0u) {
        return 0;
      }
      i += sequence_len;
    } else {
      ++i;
    }
  }
  return 1;
}

static pslog_field pslog_field_new_view(pslog_string_view key,
                                        pslog_field_type type, int allow_utf8) {
  pslog_field field;
  key = pslog_normalize_view(key);
  field.key = key.data;
  field.key_len = key.len;
  field.value_len = 0u;
  field.type = type;
  field.trusted_key = (unsigned char)pslog_view_is_trusted(key, allow_utf8);
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.pointer_value = NULL;
  return field;
}

pslog_field pslog_null_view(pslog_string_view key) {
  return pslog_field_new_view(key, PSLOG_FIELD_NULL, 0);
}

pslog_field pslog_str_view(pslog_string_view key, pslog_string_view value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_STRING, 0);
  value = pslog_normalize_view(value);
  field.as.string_value = value.data != NULL ? value.data : "";
  field.value_len = value.len;
  field.trusted_value = (unsigned char)pslog_view_is_trusted(value, 0);
  return field;
}

pslog_field pslog_trusted_str_view(pslog_string_view key,
                                   pslog_string_view value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_STRING, 1);
  value = pslog_normalize_view(value);
  field.as.string_value = value.data != NULL ? value.data : "";
  field.value_len = value.len;
  field.trusted_value = (unsigned char)pslog_view_is_trusted(value, 1);
  return field;
}

pslog_field pslog_errno_view(pslog_string_view key, int err) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_ERRNO, 0);
  field.as.signed_value = (pslog_int64)err;
  return field;
}
pslog_field pslog_i64_view(pslog_string_view key, pslog_int64 value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_SIGNED, 0);
  field.as.signed_value = value;
  return field;
}
pslog_field pslog_u64_view(pslog_string_view key, pslog_uint64 value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_UNSIGNED, 0);
  field.as.unsigned_value = value;
  return field;
}
pslog_field pslog_f64_view(pslog_string_view key, double value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_DOUBLE, 0);
  field.as.double_value = value;
  return field;
}
pslog_field pslog_bool_view(pslog_string_view key, int value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_BOOL, 0);
  field.as.bool_value = value ? 1 : 0;
  return field;
}
pslog_field pslog_ptr_view(pslog_string_view key, const void *value) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_POINTER, 0);
  field.as.pointer_value = value;
  return field;
}
pslog_field pslog_bytes_field_view(pslog_string_view key, const void *data,
                                   size_t len) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_BYTES, 0);
  field.as.bytes_value.data = (const unsigned char *)data;
  field.as.bytes_value.len = len;
  return field;
}
pslog_field pslog_time_field_view(pslog_string_view key, long epoch_seconds,
                                  long nanoseconds, int utc_offset_minutes) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_TIME, 0);
  field.as.time_value.epoch_seconds = epoch_seconds;
  field.as.time_value.nanoseconds = nanoseconds;
  field.as.time_value.utc_offset_minutes = utc_offset_minutes;
  return field;
}
pslog_field pslog_duration_field_view(pslog_string_view key, long seconds,
                                      long nanoseconds) {
  pslog_field field = pslog_field_new_view(key, PSLOG_FIELD_DURATION, 0);
  field.as.duration_value.seconds = seconds;
  field.as.duration_value.nanoseconds = nanoseconds;
  return field;
}

pslog_logger *pslog_new(const pslog_config *config) {
  pslog_config effective;
  pslog_shared_state *shared;
  pslog_logger *logger;
  pslog_logger_impl *impl;
  char *time_format_storage;
  size_t normalized_line_buffer_capacity;

  pslog_default_config(&effective);
  if (config != NULL) {
    effective = *config;
    if (effective.palette == NULL) {
      effective.palette = pslog_palette_default();
    }
    if (effective.output.write == NULL) {
      effective.output = pslog_output_from_fp(stdout, 0);
    }
  }
  if (!pslog_normalize_line_buffer_capacity(effective.line_buffer_capacity,
                                            &normalized_line_buffer_capacity)) {
    return NULL;
  }

  shared = (pslog_shared_state *)pslog_calloc_internal(1u, sizeof(*shared));
  if (shared == NULL) {
    return NULL;
  }
  if (pslog_mutex_init(&shared->state_mutex) != 0) {
    pslog_free_internal(shared);
    return NULL;
  }
  if (pslog_mutex_init(&shared->output_mutex) != 0) {
    pslog_mutex_destroy(&shared->state_mutex);
    pslog_free_internal(shared);
    return NULL;
  }
  shared->output = effective.output;
  shared->mode = effective.mode;
  shared->non_finite_float_policy = pslog_normalize_non_finite_float_policy(
      effective.non_finite_float_policy);
  shared->line_buffer_capacity = normalized_line_buffer_capacity;
  shared->timestamp_cacheable =
      !pslog_time_format_is_rfc3339nano(effective.time_format);
  shared->min_level = effective.min_level;
  shared->timestamps = effective.timestamps;
  shared->utc = effective.utc;
  shared->verbose_fields = effective.verbose_fields;
  shared->palette =
      effective.palette != NULL ? effective.palette : pslog_palette_default();
  time_format_storage = NULL;
  if (effective.time_format != NULL) {
    size_t time_format_len = strlen(effective.time_format) + 1u;

    time_format_storage =
        (char *)pslog_memdup_local(effective.time_format, time_format_len);
    if (time_format_storage == NULL) {
      pslog_release_shared(shared);
      return NULL;
    }
  }
  shared->time_format_storage = time_format_storage;
  shared->time_format =
      time_format_storage != NULL ? time_format_storage : effective.time_format;
  shared->time_now = pslog_default_time_now;
  shared->time_now_userdata = NULL;
  shared->palette_key_len = pslog_cstr_len_or_zero(shared->palette->key);
  shared->palette_string_len = pslog_cstr_len_or_zero(shared->palette->string);
  shared->palette_number_len = pslog_cstr_len_or_zero(shared->palette->number);
  shared->palette_boolean_len =
      pslog_cstr_len_or_zero(shared->palette->boolean);
  shared->palette_null_value_len =
      pslog_cstr_len_or_zero(shared->palette->null_value);
  shared->palette_timestamp_len =
      pslog_cstr_len_or_zero(shared->palette->timestamp);
  shared->palette_message_len =
      pslog_cstr_len_or_zero(shared->palette->message);
  shared->palette_message_key_len =
      pslog_cstr_len_or_zero(shared->palette->message_key);
  shared->palette_reset_len = pslog_cstr_len_or_zero(shared->palette->reset);
  shared->level_color_lens[0] = pslog_cstr_len_or_zero(shared->palette->trace);
  shared->level_color_lens[1] = pslog_cstr_len_or_zero(shared->palette->debug);
  shared->level_color_lens[2] = pslog_cstr_len_or_zero(shared->palette->info);
  shared->level_color_lens[3] = pslog_cstr_len_or_zero(shared->palette->warn);
  shared->level_color_lens[4] = pslog_cstr_len_or_zero(shared->palette->error);
  shared->level_color_lens[5] = pslog_cstr_len_or_zero(shared->palette->fatal);
  shared->level_color_lens[6] = pslog_cstr_len_or_zero(shared->palette->panic);
  shared->level_color_lens[7] =
      pslog_cstr_len_or_zero(shared->palette->null_value);
  shared->kvfmt_ptr_cache =
      (pslog_kvfmt_ptr_cache_entry *)pslog_calloc_internal(
          PSLOG_KVFMT_PTR_CACHE_SIZE, sizeof(*shared->kvfmt_ptr_cache));
  shared->refcount = 1u;
  if (effective.color == PSLOG_COLOR_ALWAYS) {
    shared->color_enabled = 1;
  } else if (effective.color == PSLOG_COLOR_NEVER) {
    shared->color_enabled = 0;
  } else if (shared->output.isatty != NULL) {
    shared->color_enabled = shared->output.isatty(shared->output.userdata);
  } else {
    shared->color_enabled = 0;
  }
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_TRACE, 0u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_DEBUG, 1u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_INFO, 2u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_WARN, 3u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_ERROR, 4u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_FATAL, 5u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_PANIC, 6u);
  pslog_build_console_level_fragment(shared, PSLOG_LEVEL_NOLEVEL, 7u);
  pslog_build_console_literals(shared);
  if (shared->color_enabled && shared->mode == PSLOG_MODE_JSON) {
    pslog_build_json_color_literals(shared);
  }

  logger = pslog_allocate_logger(shared);
  if (logger == NULL) {
    pslog_release_shared(shared);
    return NULL;
  }
  impl = (pslog_logger_impl *)logger->impl;
  impl->include_level_field = effective.with_level_field ? 1 : 0;
  return logger;
}

pslog_logger *pslog_new_from_env(const char *prefix,
                                 const pslog_config *config) {
  pslog_config effective;
  char key[128];
  char palette_value[64];
  char output_value[512];
  char output_mode_value[32];
  char error_text[256];
  const char *value;
  pslog_level level;
  int bool_value;
  int output_file_mode;
  int output_err;
  int has_output_err;
  int has_output_mode_err;
  pslog_output base_output;
  pslog_logger *logger;
  pslog_field fields[2];

  pslog_default_config(&effective);
  if (config != NULL) {
    effective = *config;
    if (effective.palette == NULL) {
      effective.palette = pslog_palette_default();
    }
    if (effective.output.write == NULL) {
      effective.output = pslog_output_from_fp(stdout, 0);
    }
  }
  output_value[0] = '\0';
  output_mode_value[0] = '\0';
  error_text[0] = '\0';
  output_file_mode = PSLOG_DEFAULT_OUTPUT_FILE_MODE;
  output_err = 0;
  has_output_err = 0;
  has_output_mode_err = 0;

  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "LEVEL")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_level(value, &level)) {
      effective.min_level = level;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "MODE")) {
    value = getenv(key);
    if (value != NULL) {
      pslog_mode mode_value;

      if (pslog_parse_mode_value(value, &mode_value)) {
        effective.mode = mode_value;
      }
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix),
                     "TIME_FORMAT")) {
    value = getenv(key);
    if (value != NULL && *value != '\0') {
      effective.time_format = value;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix),
                     "DISABLE_TIMESTAMP")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_bool_value(value, &bool_value)) {
      effective.timestamps = bool_value ? 0 : 1;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix),
                     "VERBOSE_FIELDS")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_bool_value(value, &bool_value)) {
      effective.verbose_fields = bool_value;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "UTC")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_bool_value(value, &bool_value)) {
      effective.utc = bool_value;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "NO_COLOR")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_bool_value(value, &bool_value) &&
        bool_value) {
      effective.color = PSLOG_COLOR_NEVER;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix),
                     "FORCE_COLOR")) {
    value = getenv(key);
    if (value != NULL && pslog_parse_bool_value(value, &bool_value) &&
        bool_value && effective.color != PSLOG_COLOR_NEVER) {
      effective.color = PSLOG_COLOR_ALWAYS;
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "PALETTE")) {
    value = getenv(key);
    if (value != NULL && *value != '\0') {
      pslog_trim_copy(palette_value, sizeof(palette_value), value);
      effective.palette = pslog_palette_by_name(palette_value);
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix),
                     "OUTPUT_FILE_MODE")) {
    value = getenv(key);
    if (value != NULL) {
      pslog_trim_copy(output_mode_value, sizeof(output_mode_value), value);
      if (output_mode_value[0] != '\0' &&
          !pslog_parse_file_mode(output_mode_value, &output_file_mode)) {
        has_output_mode_err = 1;
        output_file_mode = PSLOG_DEFAULT_OUTPUT_FILE_MODE;
      }
    }
  }
  if (pslog_env_copy(key, sizeof(key), pslog_env_prefix(prefix), "OUTPUT")) {
    value = getenv(key);
    if (value != NULL) {
      pslog_trim_copy(output_value, sizeof(output_value), value);
    }
    if (output_value[0] != '\0') {
      base_output = effective.output;
      pslog_output_reset(&effective.output);
      output_err = pslog_output_init_env_value(&effective.output, &base_output,
                                               output_value, output_file_mode);
      if (output_err != 0) {
        effective.output = base_output;
        has_output_err = 1;
      } else if (!pslog_output_contains(&effective.output, &base_output)) {
        pslog_output_destroy(&base_output);
      }
    }
  }

  logger = pslog_new(&effective);
  if (logger == NULL) {
    return NULL;
  }

  if (has_output_mode_err) {
    sprintf(error_text,
            "invalid OUTPUT_FILE_MODE \"%s\" (expected octal permissions in "
            "range 0000-0777)",
            output_mode_value);
    fields[0] = pslog_str("error", error_text);
    fields[1] = pslog_str("output_file_mode", output_mode_value);
    logger->error(logger, "logger.output.file_mode.invalid", fields, 2u);
  }
  if (has_output_err) {
    if (output_err > 0) {
      strcpy(error_text, strerror(output_err));
    } else {
      strcpy(error_text, "failed to initialize output");
    }
    fields[0] = pslog_str("error", error_text);
    fields[1] = pslog_str("output", output_value);
    logger->error(logger, "logger.output.open.failed", fields, 2u);
  }

  return logger;
}

void pslog_fields(pslog_logger *log, pslog_level level, const char *msg,
                  const pslog_field *fields, size_t count) {
  if (log == NULL) {
    return;
  }
  log->log(log, level, msg, fields, count);
}

void pslog_trace(pslog_logger *log, const char *msg, const pslog_field *fields,
                 size_t count) {
  if (log == NULL) {
    return;
  }
  log->trace(log, msg, fields, count);
}

void pslog_debug(pslog_logger *log, const char *msg, const pslog_field *fields,
                 size_t count) {
  if (log == NULL) {
    return;
  }
  log->debug(log, msg, fields, count);
}

void pslog_info(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count) {
  if (log == NULL) {
    return;
  }
  log->info(log, msg, fields, count);
}

void pslog_warn(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count) {
  if (log == NULL) {
    return;
  }
  log->warn(log, msg, fields, count);
}

void pslog_error(pslog_logger *log, const char *msg, const pslog_field *fields,
                 size_t count) {
  if (log == NULL) {
    return;
  }
  log->error(log, msg, fields, count);
}

void pslog_fatal(pslog_logger *log, const char *msg, const pslog_field *fields,
                 size_t count) {
  if (log == NULL) {
    return;
  }
  log->fatal(log, msg, fields, count);
}

void pslog_panic(pslog_logger *log, const char *msg, const pslog_field *fields,
                 size_t count) {
  if (log == NULL) {
    return;
  }
  log->panic(log, msg, fields, count);
}

void pslog(pslog_logger *log, pslog_level level, const char *msg,
           const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, level, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_tracef(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_TRACE, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_debugf(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_DEBUG, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_infof(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_INFO, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_warnf(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_WARN, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_errorf(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_ERROR, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_fatalf(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_FATAL, msg, kvfmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

void pslog_panicf(pslog_logger *log, const char *msg, const char *kvfmt, ...) {
  va_list ap;

  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_PANIC, msg, kvfmt, ap);
  va_end(ap);
  abort();
}

void pslog_fields_view(pslog_logger *log, pslog_level level,
                       pslog_string_view msg, const pslog_field *fields,
                       size_t count) {
  if (log != NULL) {
    log->log_view(log, level, msg, fields, count);
  }
}

void pslog_trace_view(pslog_logger *log, pslog_string_view msg,
                      const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->trace_view(log, msg, fields, count);
  }
}

void pslog_debug_view(pslog_logger *log, pslog_string_view msg,
                      const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->debug_view(log, msg, fields, count);
  }
}

void pslog_info_view(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->info_view(log, msg, fields, count);
  }
}

void pslog_warn_view(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->warn_view(log, msg, fields, count);
  }
}

void pslog_error_view(pslog_logger *log, pslog_string_view msg,
                      const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->error_view(log, msg, fields, count);
  }
}

void pslog_fatal_view(pslog_logger *log, pslog_string_view msg,
                      const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->fatal_view(log, msg, fields, count);
  }
}

void pslog_panic_view(pslog_logger *log, pslog_string_view msg,
                      const pslog_field *fields, size_t count) {
  if (log != NULL) {
    log->panic_view(log, msg, fields, count);
  }
}

void pslog_view(pslog_logger *log, pslog_level level, pslog_string_view msg,
                const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, level, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_tracef_view(pslog_logger *log, pslog_string_view msg,
                       const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_TRACE, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_debugf_view(pslog_logger *log, pslog_string_view msg,
                       const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_DEBUG, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_infof_view(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_INFO, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_warnf_view(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_WARN, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_errorf_view(pslog_logger *log, pslog_string_view msg,
                       const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_ERROR, msg, kvfmt, ap);
  va_end(ap);
}

void pslog_fatalf_view(pslog_logger *log, pslog_string_view msg,
                       const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_FATAL, msg, kvfmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

void pslog_panicf_view(pslog_logger *log, pslog_string_view msg,
                       const char *kvfmt, ...) {
  va_list ap;
  if (log == NULL) {
    return;
  }
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_PANIC, msg, kvfmt, ap);
  va_end(ap);
  abort();
}

pslog_logger *pslog_with(pslog_logger *log, const pslog_field *fields,
                         size_t count) {
  if (log == NULL) {
    return NULL;
  }
  return log->with(log, fields, count);
}

pslog_logger *pslog_withf(pslog_logger *log, const char *kvfmt, ...) {
  pslog_logger *child;
  va_list ap;

  if (log == NULL) {
    return NULL;
  }
  va_start(ap, kvfmt);
  child = pslog_vwithf_impl(log, kvfmt, ap);
  va_end(ap);
  return child;
}

pslog_logger *pslog_with_level(pslog_logger *log, pslog_level level) {
  if (log == NULL) {
    return NULL;
  }
  return log->with_level(log, level);
}

pslog_logger *pslog_with_level_field(pslog_logger *log) {
  if (log == NULL) {
    return NULL;
  }
  return log->with_level_field(log);
}

void pslog_buffer_init(pslog_buffer *buffer, pslog_shared_state *shared,
                       size_t capacity) {
  size_t alloc_size;
  size_t normalized_capacity;

  if (buffer == NULL) {
    return;
  }
  buffer->shared = shared;
  if (!pslog_normalize_line_buffer_capacity(capacity, &normalized_capacity)) {
    normalized_capacity = PSLOG_DEFAULT_LINE_BUFFER_CAPACITY;
  }
  buffer->capacity = normalized_capacity;
  buffer->chunk_capacity = normalized_capacity;
  buffer->output_locked = 0;
  buffer->heap_owned = 0;
  buffer->data = buffer->inline_data;
  if (buffer->capacity >= sizeof(buffer->inline_data)) {
    if (pslog_size_add(buffer->capacity, 1u, &alloc_size)) {
      buffer->data = (char *)pslog_malloc_internal(alloc_size);
    }
    if (buffer->data != NULL && buffer->data != buffer->inline_data) {
      buffer->heap_owned = 1;
    } else {
      buffer->data = buffer->inline_data;
      buffer->capacity = sizeof(buffer->inline_data) - 1u;
      buffer->chunk_capacity = PSLOG_DEFAULT_LINE_BUFFER_CAPACITY;
    }
  }
  buffer->len = 0u;
  buffer->data[0] = '\0';
}

int pslog_buffer_reserve(pslog_buffer *buffer, size_t min_capacity) {
  size_t alloc_size;
  char *new_data;
  size_t new_capacity;

  if (buffer == NULL) {
    return -1;
  }
  if (min_capacity > (size_t)-1 - 1u || buffer->capacity > (size_t)-1 - 1u) {
    return -1;
  }
  if (min_capacity <= buffer->capacity) {
    return 0;
  }
  new_capacity = buffer->capacity != 0u ? buffer->capacity : 1u;
  while (new_capacity < min_capacity) {
    if (new_capacity > ((size_t)-1) / 2u) {
      new_capacity = min_capacity;
      break;
    }
    new_capacity *= 2u;
  }
  if (buffer->heap_owned) {
    if (!pslog_size_add(new_capacity, 1u, &alloc_size)) {
      return -1;
    }
    new_data = (char *)pslog_realloc_internal(buffer->data, alloc_size);
    if (new_data == NULL) {
      return -1;
    }
    buffer->data = new_data;
  } else {
    if (!pslog_size_add(new_capacity, 1u, &alloc_size)) {
      return -1;
    }
    new_data = (char *)pslog_malloc_internal(alloc_size);
    if (new_data == NULL) {
      return -1;
    }
    if (buffer->len > 0u) {
      memcpy(new_data, buffer->data, buffer->len);
    }
    new_data[buffer->len] = '\0';
    buffer->data = new_data;
    buffer->heap_owned = 1;
  }
  buffer->capacity = new_capacity;
  return 0;
}

void pslog_buffer_destroy(pslog_buffer *buffer) {
  if (buffer == NULL) {
    return;
  }
  if (buffer->heap_owned && buffer->data != NULL) {
    pslog_free_internal(buffer->data);
  }
  buffer->shared = NULL;
  buffer->data = buffer->inline_data;
  buffer->capacity = sizeof(buffer->inline_data);
  buffer->chunk_capacity = PSLOG_DEFAULT_LINE_BUFFER_CAPACITY;
  buffer->output_locked = 0;
  buffer->heap_owned = 0;
  buffer->len = 0u;
  buffer->data[0] = '\0';
}

void pslog_buffer_append_char(pslog_buffer *buffer, char ch) {
  if (buffer == NULL || buffer->data == NULL || buffer->capacity == 0u) {
    return;
  }
  if (buffer->len == buffer->capacity) {
    pslog_buffer_flush(buffer);
    if (buffer->data == NULL || buffer->capacity == 0u ||
        buffer->len == buffer->capacity) {
      return;
    }
  }
  buffer->data[buffer->len] = ch;
  ++buffer->len;
  buffer->data[buffer->len] = '\0';
}

void pslog_buffer_append_n(pslog_buffer *buffer, const char *text, size_t len) {
  size_t available;
  size_t chunk;

  if (buffer == NULL || buffer->data == NULL || buffer->capacity == 0u ||
      text == NULL || len == 0u) {
    return;
  }
  while (len > 0u) {
    if (buffer->len == buffer->capacity) {
      pslog_buffer_flush(buffer);
      if (buffer->data == NULL || buffer->capacity == 0u ||
          buffer->len == buffer->capacity) {
        return;
      }
    }
    available = buffer->capacity - buffer->len;
    if (available == 0u) {
      continue;
    }
    chunk = len;
    if (chunk > available) {
      chunk = available;
    }
    memcpy(buffer->data + buffer->len, text, chunk);
    buffer->len += chunk;
    buffer->data[buffer->len] = '\0';
    text += chunk;
    len -= chunk;
  }
}

void pslog_buffer_append_cstr(pslog_buffer *buffer, const char *text) {
  if (text == NULL) {
    return;
  }
  pslog_buffer_append_n(buffer, text, strlen(text));
}

void pslog_buffer_append_json_string(pslog_buffer *buffer, const char *text) {
  pslog_buffer_append_json_string_n(buffer, text,
                                    text != NULL ? strlen(text) : 0u);
}

void pslog_buffer_append_json_string_n(pslog_buffer *buffer, const char *text,
                                       size_t len) {
  static const char hex[] = "0123456789abcdef";
  static const unsigned char escape_kind[256] = {
      6, 6, 6, 6, 6, 6, 6, 6, 6, 5, 4, 6, 6, 3, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6,
      6, 6, 6, 6, 6, 6, 6, 6, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 6, 2, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
      0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
  unsigned char ch;
  unsigned char kind;
  const char *cursor;
  const char *chunk_start;
  const char *end;
  size_t utf8_len;
  size_t remaining;

  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  if (text != NULL && len > 0u) {
    cursor = text;
    chunk_start = text;
    end = text + len;
    while (cursor < end) {
      ch = (unsigned char)*cursor;
      kind = 0u;
      if (ch >= 0x80u) {
        remaining = (size_t)(end - cursor);
        utf8_len =
            pslog_utf8_sequence_len_n((const unsigned char *)cursor, remaining);
        if (utf8_len == 0u) {
          if (cursor > chunk_start) {
            PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                       (size_t)(cursor - chunk_start));
          }
          PSLOG_APPEND_LITERAL(buffer, "\\u00");
          PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[(ch >> 4) & 0x0fu]);
          PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[ch & 0x0fu]);
          chunk_start = cursor + 1;
        } else {
          cursor += utf8_len;
          continue;
        }
      } else {
        kind = escape_kind[ch];
        if (kind != 0u) {
          if (cursor > chunk_start) {
            PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                       (size_t)(cursor - chunk_start));
          }
          if (kind == 1u || kind == 2u) {
            PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '\\');
            PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, (char)ch);
          } else if (kind == 3u) {
            PSLOG_APPEND_LITERAL(buffer, "\\r");
          } else if (kind == 4u) {
            PSLOG_APPEND_LITERAL(buffer, "\\n");
          } else if (kind == 5u) {
            PSLOG_APPEND_LITERAL(buffer, "\\t");
          } else {
            PSLOG_APPEND_LITERAL(buffer, "\\u00");
            PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[(ch >> 4) & 0x0fu]);
            PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[ch & 0x0fu]);
          }
          chunk_start = cursor + 1;
        }
      }
      if (ch >= 0x80u || kind != 0u) {
        ++cursor;
        continue;
      }
      ++cursor;
    }
    if (cursor > chunk_start) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                 (size_t)(cursor - chunk_start));
    }
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
}

void pslog_buffer_append_json_string_maybe_trusted(pslog_buffer *buffer,
                                                   const char *text) {
  pslog_buffer_append_json_string_maybe_trusted_n(buffer, text,
                                                  pslog_cstr_len_or_zero(text));
}

void pslog_buffer_append_json_string_maybe_trusted_n(pslog_buffer *buffer,
                                                     const char *text,
                                                     size_t len) {
  pslog_string_view view;
  view.data = text;
  view.len = len;
  if (pslog_view_is_trusted(view, 1)) {
    PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(
        buffer, text != NULL ? text : "", len);
    return;
  }
  pslog_buffer_append_json_string_n(buffer, text, len);
}

void pslog_buffer_append_json_trusted_string(pslog_buffer *buffer,
                                             const char *text) {
  pslog_buffer_append_json_trusted_string_n(buffer, text,
                                            text != NULL ? strlen(text) : 0u);
}

void pslog_buffer_append_json_trusted_string_n(pslog_buffer *buffer,
                                               const char *text, size_t len) {
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  if (text != NULL && len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, text, len);
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
}

void pslog_buffer_append_console_string(pslog_buffer *buffer,
                                        const char *text) {
  pslog_buffer_append_console_string_n(buffer, text,
                                       pslog_cstr_len_or_zero(text));
}

void pslog_buffer_append_console_string_n(pslog_buffer *buffer,
                                          const char *text, size_t len) {
  static const char hex[] = "0123456789abcdef";
  unsigned char ch;
  const char *cursor;
  const char *chunk_start;
  const char *end;

  if (text == NULL || len == 0u) {
    return;
  }

  cursor = text;
  end = text + len;
  while (cursor < end) {
    ch = (unsigned char)*cursor;
    if (((ch >= 0x09u && ch <= 0x0du) || ch == 0x20u) || ch == '=' ||
        ch == '"' || ch == '\\' || ch < 0x20u || ch == 0x7fu || ch == 0x1bu) {
      break;
    }
    ++cursor;
  }
  if (cursor == end) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, text, (size_t)(cursor - text));
    return;
  }

  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  if (cursor > text) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, text, (size_t)(cursor - text));
  }
  chunk_start = cursor;
  while (cursor < end) {
    ch = (unsigned char)*cursor;
    if (ch == '"' || ch == '\\' || ch == '\n' || ch == '\r' || ch == '\t' ||
        ch < 0x20u || ch == 0x7fu || ch == 0x1bu) {
      if (cursor > chunk_start) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                   (size_t)(cursor - chunk_start));
      }
      if (ch == '"') {
        PSLOG_APPEND_LITERAL(buffer, "\\\"");
      } else if (ch == '\\') {
        PSLOG_APPEND_LITERAL(buffer, "\\\\");
      } else if (ch == '\n') {
        PSLOG_APPEND_LITERAL(buffer, "\\n");
      } else if (ch == '\r') {
        PSLOG_APPEND_LITERAL(buffer, "\\r");
      } else if (ch == '\t') {
        PSLOG_APPEND_LITERAL(buffer, "\\t");
      } else {
        PSLOG_APPEND_LITERAL(buffer, "\\x");
        PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[(ch >> 4) & 0x0fu]);
        PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[ch & 0x0fu]);
      }
      chunk_start = cursor + 1;
    }
    ++cursor;
  }
  if (cursor > chunk_start) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                               (size_t)(cursor - chunk_start));
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
}

void pslog_buffer_append_console_message(pslog_buffer *buffer,
                                         const char *text) {
  pslog_buffer_append_console_message_n(buffer, text,
                                        pslog_cstr_len_or_zero(text));
}

void pslog_buffer_append_console_message_n(pslog_buffer *buffer,
                                           const char *text, size_t len) {
  pslog_buffer_append_console_escaped(buffer, text, len, 0);
}

void pslog_buffer_append_console_key_n(pslog_buffer *buffer, const char *text,
                                       size_t len) {
  const char *cursor = text;
  const char *segment = text;
  const char *end = text + len;
  while (cursor < end) {
    if (*cursor == '\0') {
      if (cursor > segment) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, segment, (size_t)(cursor - segment));
      }
      PSLOG_APPEND_LITERAL(buffer, "\\x00");
      segment = cursor + 1;
    }
    ++cursor;
  }
  if (end > segment) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, segment, (size_t)(end - segment));
  }
}

void pslog_buffer_append_bytes_hex(pslog_buffer *buffer,
                                   const unsigned char *data, size_t len) {
  static const char hex[] = "0123456789abcdef";
  size_t i;

  for (i = 0u; i < len; ++i) {
    pslog_buffer_append_char(buffer, hex[(data[i] >> 4) & 0x0fu]);
    pslog_buffer_append_char(buffer, hex[data[i] & 0x0fu]);
  }
}

void pslog_buffer_append_signed(pslog_buffer *buffer, pslog_int64 value) {
  static const char digit_pairs[] = "00010203040506070809"
                                    "10111213141516171819"
                                    "20212223242526272829"
                                    "30313233343536373839"
                                    "40414243444546474849"
                                    "50515253545556575859"
                                    "60616263646566676869"
                                    "70717273747576777879"
                                    "80818283848586878889"
                                    "90919293949596979899";
  char tmp[32];
  char *cursor;
  pslog_uint64 magnitude;
  int negative;

  cursor = tmp + sizeof(tmp);
  negative = value < (pslog_int64)0 ? 1 : 0;
  if (negative) {
    magnitude = (pslog_uint64)(-(value + (pslog_int64)1)) + (pslog_uint64)1;
  } else {
    magnitude = (pslog_uint64)value;
  }
  while (magnitude >= (pslog_uint64)100) {
    size_t pair_index;

    pair_index = (size_t)((magnitude % (pslog_uint64)100) * (pslog_uint64)2);
    magnitude /= (pslog_uint64)100;
    cursor -= 2;
    cursor[0] = digit_pairs[pair_index];
    cursor[1] = digit_pairs[pair_index + 1u];
  }
  if (magnitude < (pslog_uint64)10) {
    --cursor;
    *cursor = (char)('0' + (int)magnitude);
  } else {
    size_t pair_index;

    pair_index = (size_t)(magnitude * (pslog_uint64)2);
    cursor -= 2;
    cursor[0] = digit_pairs[pair_index];
    cursor[1] = digit_pairs[pair_index + 1u];
  }
  if (negative) {
    --cursor;
    *cursor = '-';
  }
  PSLOG_BUFFER_APPEND_N_FAST(buffer, cursor,
                             (size_t)((tmp + sizeof(tmp)) - cursor));
}

void pslog_buffer_append_unsigned(pslog_buffer *buffer, pslog_uint64 value) {
  static const char digit_pairs[] = "00010203040506070809"
                                    "10111213141516171819"
                                    "20212223242526272829"
                                    "30313233343536373839"
                                    "40414243444546474849"
                                    "50515253545556575859"
                                    "60616263646566676869"
                                    "70717273747576777879"
                                    "80818283848586878889"
                                    "90919293949596979899";
  char tmp[32];
  char *cursor;

  cursor = tmp + sizeof(tmp);
  while (value >= (pslog_uint64)100) {
    size_t pair_index;

    pair_index = (size_t)((value % (pslog_uint64)100) * (pslog_uint64)2);
    value /= (pslog_uint64)100;
    cursor -= 2;
    cursor[0] = digit_pairs[pair_index];
    cursor[1] = digit_pairs[pair_index + 1u];
  }
  if (value < (pslog_uint64)10) {
    --cursor;
    *cursor = (char)('0' + (int)value);
  } else {
    size_t pair_index;

    pair_index = (size_t)(value * (pslog_uint64)2);
    cursor -= 2;
    cursor[0] = digit_pairs[pair_index];
    cursor[1] = digit_pairs[pair_index + 1u];
  }
  PSLOG_BUFFER_APPEND_N_FAST(buffer, cursor,
                             (size_t)((tmp + sizeof(tmp)) - cursor));
}

void pslog_normalize_decimal_separator(char *text, size_t len) {
  size_t i;

  if (text == NULL || len == 0u) {
    return;
  }
  for (i = 0u; i < len; ++i) {
    if (text[i] == ',') {
      text[i] = '.';
    }
  }
}

int pslog_format_double_ascii(char *dst, size_t dst_size, double value) {
  char *separator;
  int len;

  if (dst == NULL || dst_size == 0u) {
    return -1;
  }
  len = sprintf(dst, "%.17g", value);
  if (len <= 0) {
    dst[0] = '\0';
    return len;
  }
  if ((size_t)len >= dst_size) {
    dst[dst_size - 1u] = '\0';
    len = (int)(dst_size - 1u);
  }
  separator = (char *)memchr(dst, ',', (size_t)len);
  if (separator != NULL) {
    *separator = '.';
  }
  return len;
}

void pslog_buffer_append_double(pslog_buffer *buffer, double value) {
  union {
    double value;
    unsigned char bytes[sizeof(double)];
  } bits;
  pslog_double_cache_entry *entry;
  unsigned long hash;
  size_t i;
  char tmp[64];
  int len;

  if (pslog_double_is_nan(value)) {
    pslog_buffer_append_cstr(buffer, "nan");
    return;
  }
  if (pslog_double_is_inf(value)) {
    pslog_buffer_append_cstr(buffer, value < 0.0 ? "-inf" : "inf");
    return;
  }
  if (buffer != NULL && buffer->shared != NULL) {
    bits.value = value;
    hash = 1469598103ul;
    for (i = 0u; i < sizeof(bits.bytes); ++i) {
      hash ^= (unsigned long)bits.bytes[i];
      hash *= 16777619ul;
    }
    pslog_state_lock(buffer->shared);
    entry = &buffer->shared->double_cache[hash % PSLOG_DOUBLE_CACHE_SIZE];
    if (entry->valid &&
        memcmp(entry->bits, bits.bytes, sizeof(bits.bytes)) == 0) {
      len = (int)entry->rendered_len;
      if (len > 0 && (size_t)len < sizeof(tmp)) {
        memcpy(tmp, entry->rendered, (size_t)len);
        tmp[(size_t)len] = '\0';
      } else {
        len = 0;
      }
      pslog_state_unlock(buffer->shared);
      if (len > 0) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
      }
      return;
    }
    pslog_state_unlock(buffer->shared);
    len = pslog_format_double_ascii(tmp, sizeof(tmp), value);
    if (len > 0) {
      entry = &buffer->shared->double_cache[hash % PSLOG_DOUBLE_CACHE_SIZE];
      if ((size_t)len < sizeof(entry->rendered)) {
        pslog_state_lock(buffer->shared);
        entry->valid = 0;
        memcpy(entry->bits, bits.bytes, sizeof(bits.bytes));
        memcpy(entry->rendered, tmp, (size_t)len);
        entry->rendered[(size_t)len] = '\0';
        entry->rendered_len = (size_t)len;
        entry->valid = 1;
        pslog_state_unlock(buffer->shared);
      }
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
    }
    return;
  }
  len = pslog_format_double_ascii(tmp, sizeof(tmp), value);
  if (len > 0) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
  }
}

void pslog_buffer_append_pointer(pslog_buffer *buffer, const void *value) {
  char tmp[64];
  int len;

  len = sprintf(tmp, "%p", value);
  if (len > 0) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
  }
}

void pslog_buffer_append_time(pslog_buffer *buffer, pslog_time_value value,
                              int quote) {
  time_t base;
  struct tm tm_value;
  char tmp[64];
  long offset_seconds;
  long hours;
  long minutes;
  char sign;

  offset_seconds = (long)value.utc_offset_minutes * 60l;
  base = (time_t)(value.epoch_seconds + offset_seconds);
  if (!pslog_time_to_tm(base, 1, &tm_value)) {
    if (quote) {
      pslog_buffer_append_json_string(buffer, "");
    } else {
      pslog_buffer_append_console_string(buffer, "");
    }
    return;
  }
  if (quote) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  }
  {
    size_t len;

    len = strftime(tmp, sizeof(tmp), "%Y-%m-%dT%H:%M:%S", &tm_value);
    if (len > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, len);
    }
  }
  if (value.nanoseconds != 0l) {
    char frac[32];
    char *end;
    size_t frac_len;

    sprintf(frac, ".%09ld", labs(value.nanoseconds));
    end = frac + strlen(frac) - 1;
    while (end > frac + 1 && *end == '0') {
      *end = '\0';
      --end;
    }
    frac_len = (size_t)(end - frac + 1);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, frac, frac_len);
  }
  if (value.utc_offset_minutes == 0) {
    pslog_buffer_append_char(buffer, 'Z');
  } else {
    sign = value.utc_offset_minutes < 0 ? '-' : '+';
    hours = labs((long)value.utc_offset_minutes) / 60l;
    minutes = labs((long)value.utc_offset_minutes) % 60l;
    {
      int len;

      len = sprintf(tmp, "%c%02ld:%02ld", sign, hours, minutes);
      if (len > 0) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
      }
    }
  }
  if (quote) {
    pslog_buffer_append_char(buffer, '"');
  }
}

void pslog_buffer_append_duration(pslog_buffer *buffer,
                                  pslog_duration_value value, int quote) {
  char tmp[64];
  static const char micro_suffix[] = "\xC2\xB5s";
  long seconds;
  long nanoseconds;
  int negative;

  seconds = value.seconds;
  nanoseconds = value.nanoseconds;
  negative = 0;
  if (seconds < 0l && nanoseconds > 0l) {
    negative = 1;
    seconds += 1l;
    nanoseconds = 1000000000l - nanoseconds;
    seconds = labs(seconds);
  } else if (seconds > 0l && nanoseconds < 0l) {
    seconds -= 1l;
    nanoseconds = 1000000000l + nanoseconds;
  } else if (seconds < 0l || nanoseconds < 0l) {
    negative = 1;
    seconds = labs(seconds);
    nanoseconds = labs(nanoseconds);
  }

  if (quote) {
    pslog_buffer_append_char(buffer, '"');
  }
  if (negative) {
    pslog_buffer_append_char(buffer, '-');
  }
  if (seconds == 0l && nanoseconds < 1000l) {
    {
      int len;

      len = sprintf(tmp, "%ldns", nanoseconds);
      if (len > 0) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
      }
    }
  } else if (seconds == 0l && nanoseconds < 1000000l) {
    int len;

    len = sprintf(tmp, "%.3f", (double)nanoseconds / 1000.0);
    if (len > 0) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
    }
    pslog_buffer_append_n(buffer, micro_suffix, 3u);
    if (quote) {
      pslog_buffer_append_char(buffer, '"');
    }
    return;
  } else if (seconds == 0l && nanoseconds < 1000000000l) {
    int len;

    len = sprintf(tmp, "%.3fms", (double)nanoseconds / 1000000.0);
    if (len > 0) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
    }
  } else if (seconds < 60l) {
    int len;

    len = sprintf(tmp, "%.3fs",
                  (double)seconds + ((double)nanoseconds / 1000000000.0));
    if (len > 0) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
    }
  } else {
    int len;

    len = sprintf(tmp, "%ldm%ld.%03lds", seconds / 60l, seconds % 60l,
                  nanoseconds / 1000000l);
    if (len > 0) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, tmp, (size_t)len);
    }
  }
  if (quote) {
    pslog_buffer_append_char(buffer, '"');
  }
}

void pslog_buffer_finish_line(pslog_buffer *buffer) {
  pslog_buffer_append_char(buffer, '\n');
}

char *pslog_strdup_local(const char *text) {
  size_t len;
  char *copy;

  if (text == NULL) {
    text = "";
  }
  len = strlen(text);
  copy = (char *)pslog_malloc_internal(len + 1u);
  if (copy == NULL) {
    return NULL;
  }
  memcpy(copy, text, len + 1u);
  return copy;
}

void *pslog_memdup_local(const void *src, size_t len) {
  void *dst;

  if (src == NULL || len == 0u) {
    return NULL;
  }
  dst = pslog_malloc_internal(len);
  if (dst == NULL) {
    return NULL;
  }
  memcpy(dst, src, len);
  return dst;
}

void pslog_retain_shared(pslog_shared_state *shared) {
  if (shared != NULL) {
    pslog_state_lock(shared);
    ++shared->refcount;
    pslog_state_unlock(shared);
  }
}

void pslog_release_shared(pslog_shared_state *shared) {
  int should_free;
  size_t i;

  if (shared == NULL) {
    return;
  }
  should_free = 0;
  pslog_state_lock(shared);
  if (shared->refcount > 1u) {
    --shared->refcount;
  } else {
    should_free = 1;
  }
  pslog_state_unlock(shared);
  if (!should_free) {
    return;
  }
  (void)pslog_close_shared(shared, 1);
  for (i = 0u; i < PSLOG_KVFMT_CACHE_SIZE; ++i) {
    pslog_reset_kvfmt_cache_entry(&shared->kvfmt_cache[i]);
  }
  pslog_free_internal(shared->time_format_storage);
  shared->time_format_storage = NULL;
  pslog_free_internal(shared->kvfmt_ptr_cache);
  shared->kvfmt_ptr_cache = NULL;
  pslog_mutex_destroy(&shared->output_mutex);
  pslog_mutex_destroy(&shared->state_mutex);
  pslog_free_internal(shared);
}

int pslog_close_shared(pslog_shared_state *shared, int allow_close) {
  if (shared == NULL) {
    return 0;
  }
  pslog_output_lock(shared);
  allow_close = pslog_close_shared_locked(shared, allow_close);
  pslog_output_unlock(shared);
  return allow_close;
}

int pslog_should_log(pslog_logger_impl *impl, pslog_level level) {
  pslog_level threshold;

  if (impl == NULL || impl->shared == NULL) {
    return 0;
  }
  if (impl->shared->closed) {
    return 0;
  }
  if (level == PSLOG_LEVEL_DISABLED) {
    return 0;
  }
  if (level == PSLOG_LEVEL_FATAL || level == PSLOG_LEVEL_PANIC) {
    return 1;
  }
  if (impl->min_level == PSLOG_LEVEL_DISABLED) {
    return 0;
  }
  threshold = impl->min_level;
  if (threshold == PSLOG_LEVEL_NOLEVEL) {
    threshold = PSLOG_LEVEL_INFO;
  }
  return level >= threshold;
}

static size_t pslog_write_output_locked(pslog_shared_state *shared,
                                        const char *data, size_t len,
                                        size_t chunk_capacity) {
  size_t offset;
  size_t chunk_len;
  size_t written;
  int err;

  if (shared == NULL || data == NULL || len == 0u || shared->closed ||
      shared->output.write == NULL) {
    return 0u;
  }

  offset = 0u;
  while (offset < len) {
    chunk_len = len - offset;
    if (chunk_capacity > 0u && chunk_len > chunk_capacity) {
      chunk_len = chunk_capacity;
    }
    written = 0u;
    err = shared->output.write(shared->output.userdata, data + offset,
                               chunk_len, &written);
    if (written > chunk_len) {
      written = chunk_len;
    }
    offset += written;
    if (err != 0 || written == 0u) {
      break;
    }
  }
  return offset;
}

void pslog_write_buffer(pslog_shared_state *shared, pslog_buffer *buffer) {
  if (buffer == NULL || buffer->len == 0u) {
    return;
  }
  if (shared == NULL) {
    buffer->len = 0u;
    if (buffer->data != NULL) {
      buffer->data[0] = '\0';
    }
    return;
  }
  if (!buffer->output_locked) {
    pslog_output_lock(shared);
  }
  (void)pslog_write_output_locked(shared, buffer->data, buffer->len,
                                  buffer->chunk_capacity);
  pslog_output_unlock(shared);
  buffer->output_locked = 0;
  buffer->len = 0u;
  buffer->data[0] = '\0';
}

void pslog_buffer_flush(pslog_buffer *buffer) {
  pslog_shared_state *shared;

  if (buffer == NULL || buffer->len == 0u) {
    return;
  }
  shared = buffer->shared;
  if (shared == NULL) {
    if (pslog_buffer_reserve(
            buffer, buffer->capacity != 0u ? buffer->capacity * 2u : 1u) != 0) {
      return;
    }
    return;
  }
  if (shared->output.write == NULL) {
    buffer->len = 0u;
    if (buffer->data != NULL) {
      buffer->data[0] = '\0';
    }
    return;
  }
  if (!buffer->output_locked) {
    pslog_output_lock(shared);
    buffer->output_locked = 1;
  }
  if (shared->closed || shared->output.write == NULL) {
    pslog_output_unlock(shared);
    buffer->output_locked = 0;
    buffer->len = 0u;
    buffer->data[0] = '\0';
    return;
  }
  (void)pslog_write_output_locked(shared, buffer->data, buffer->len,
                                  buffer->chunk_capacity);
  buffer->len = 0u;
  buffer->data[0] = '\0';
}

void pslog_buffer_lock_output(pslog_buffer *buffer) {
  if (buffer == NULL || buffer->shared == NULL || buffer->output_locked) {
    return;
  }
  pslog_output_lock(buffer->shared);
  buffer->output_locked = 1;
}

void pslog_buffer_unlock_output(pslog_buffer *buffer) {
  if (buffer == NULL || buffer->shared == NULL || !buffer->output_locked) {
    return;
  }
  pslog_output_unlock(buffer->shared);
  buffer->output_locked = 0;
}

void pslog_append_timestamp(pslog_shared_state *shared, pslog_buffer *buffer) {
  time_t now;
  long nanoseconds;
  char stamp[64];
  size_t len;

  if (shared == NULL || buffer == NULL || !shared->timestamps) {
    return;
  }
  now = 0;
  nanoseconds = 0l;
  if (shared->timestamp_cacheable &&
      shared->time_now == pslog_default_time_now) {
    now = pslog_default_time_now_seconds();
  } else if (shared->time_now != NULL) {
    shared->time_now(shared->time_now_userdata, &now, &nanoseconds);
  } else {
    pslog_default_time_now(NULL, &now, &nanoseconds);
  }
  if (shared->timestamp_cacheable) {
    pslog_state_lock(shared);
    if (shared->cached_timestamp_len > 0u &&
        shared->cached_timestamp_second == now) {
      len = shared->cached_timestamp_len;
      if (len >= sizeof(stamp)) {
        len = sizeof(stamp) - 1u;
      }
      ++shared->timestamp_cache_hits;
      if (buffer->data != NULL && len <= buffer->capacity - buffer->len) {
        memcpy(buffer->data + buffer->len, shared->cached_timestamp, len);
        buffer->len += len;
        buffer->data[buffer->len] = '\0';
        pslog_state_unlock(shared);
        return;
      }
      memcpy(stamp, shared->cached_timestamp, len);
      stamp[len] = '\0';
      pslog_state_unlock(shared);
      PSLOG_BUFFER_APPEND_N_FAST(buffer, stamp, len);
      return;
    }
    pslog_state_unlock(shared);
    len = pslog_render_timestamp_value(shared, now, 0l, stamp, sizeof(stamp));
    if (len == 0u) {
      return;
    }
    pslog_state_lock(shared);
    if (shared->cached_timestamp_second != now ||
        shared->cached_timestamp_len == 0u) {
      if (len >= sizeof(shared->cached_timestamp)) {
        len = sizeof(shared->cached_timestamp) - 1u;
      }
      memcpy(shared->cached_timestamp, stamp, len);
      shared->cached_timestamp[len] = '\0';
      shared->cached_timestamp_len = len;
      shared->cached_timestamp_second = now;
      ++shared->timestamp_cache_misses;
    } else {
      len = shared->cached_timestamp_len;
      ++shared->timestamp_cache_hits;
      if (buffer->data != NULL && len <= buffer->capacity - buffer->len) {
        memcpy(buffer->data + buffer->len, shared->cached_timestamp, len);
        buffer->len += len;
        buffer->data[buffer->len] = '\0';
        pslog_state_unlock(shared);
        return;
      }
      memcpy(stamp, shared->cached_timestamp, len);
      stamp[len] = '\0';
    }
    pslog_state_unlock(shared);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, stamp, len);
    return;
  }
  len = pslog_render_timestamp_value(shared, now, nanoseconds, stamp,
                                     sizeof(stamp));
  if (len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, stamp, len);
  }
}

void pslog_apply_color(pslog_buffer *buffer, const char *color_code,
                       int enabled) {
  if (enabled && color_code != NULL && *color_code != '\0') {
    pslog_buffer_append_cstr(buffer, color_code);
  }
}

void pslog_apply_color_len(pslog_buffer *buffer, const char *color_code,
                           size_t color_len, int enabled) {
  PSLOG_APPLY_COLOR_FAST(buffer, color_code, color_len, enabled);
}

const char *pslog_level_color(const pslog_palette *palette, pslog_level level) {
  if (palette == NULL) {
    return "";
  }
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return palette->trace;
  case PSLOG_LEVEL_DEBUG:
    return palette->debug;
  case PSLOG_LEVEL_INFO:
    return palette->info;
  case PSLOG_LEVEL_WARN:
    return palette->warn;
  case PSLOG_LEVEL_ERROR:
    return palette->error;
  case PSLOG_LEVEL_FATAL:
    return palette->fatal;
  case PSLOG_LEVEL_PANIC:
    return palette->panic;
  case PSLOG_LEVEL_NOLEVEL:
    return palette->null_value;
  default:
    return palette->info;
  }
}

size_t pslog_level_color_len(const pslog_shared_state *shared,
                             pslog_level level) {
  if (shared == NULL) {
    return 0u;
  }
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return shared->level_color_lens[0];
  case PSLOG_LEVEL_DEBUG:
    return shared->level_color_lens[1];
  case PSLOG_LEVEL_INFO:
    return shared->level_color_lens[2];
  case PSLOG_LEVEL_WARN:
    return shared->level_color_lens[3];
  case PSLOG_LEVEL_ERROR:
    return shared->level_color_lens[4];
  case PSLOG_LEVEL_FATAL:
    return shared->level_color_lens[5];
  case PSLOG_LEVEL_PANIC:
    return shared->level_color_lens[6];
  case PSLOG_LEVEL_NOLEVEL:
    return shared->level_color_lens[7];
  default:
    return shared->level_color_lens[2];
  }
}

size_t pslog_level_string_len(pslog_level level) {
  switch (level) {
  case PSLOG_LEVEL_TRACE:
  case PSLOG_LEVEL_DEBUG:
  case PSLOG_LEVEL_ERROR:
  case PSLOG_LEVEL_FATAL:
  case PSLOG_LEVEL_PANIC:
    return 5u;
  case PSLOG_LEVEL_INFO:
  case PSLOG_LEVEL_WARN:
    return 4u;
  case PSLOG_LEVEL_NOLEVEL:
    return 7u;
  case PSLOG_LEVEL_DISABLED:
    return 8u;
  default:
    return 4u;
  }
}

const char *pslog_level_console_label(pslog_level level) {
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return "TRC";
  case PSLOG_LEVEL_DEBUG:
    return "DBG";
  case PSLOG_LEVEL_INFO:
    return "INF";
  case PSLOG_LEVEL_WARN:
    return "WRN";
  case PSLOG_LEVEL_ERROR:
    return "ERR";
  case PSLOG_LEVEL_FATAL:
    return "FTL";
  case PSLOG_LEVEL_PANIC:
    return "PNC";
  case PSLOG_LEVEL_NOLEVEL:
    return "---";
  default:
    return "INF";
  }
}

static size_t pslog_level_console_label_len(pslog_level level) {
  (void)level;
  return 3u;
}

void pslog_append_level_label(pslog_shared_state *shared, pslog_buffer *buffer,
                              pslog_level level) {
  size_t slot;

  if (shared == NULL || buffer == NULL) {
    return;
  }
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    slot = 0u;
    break;
  case PSLOG_LEVEL_DEBUG:
    slot = 1u;
    break;
  case PSLOG_LEVEL_INFO:
    slot = 2u;
    break;
  case PSLOG_LEVEL_WARN:
    slot = 3u;
    break;
  case PSLOG_LEVEL_ERROR:
    slot = 4u;
    break;
  case PSLOG_LEVEL_FATAL:
    slot = 5u;
    break;
  case PSLOG_LEVEL_PANIC:
    slot = 6u;
    break;
  case PSLOG_LEVEL_NOLEVEL:
    slot = 7u;
    break;
  default:
    slot = 2u;
    break;
  }
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->console_level_fragments[slot],
                             shared->console_level_fragment_lens[slot]);
}

static void pslog_append_owned_field_copy(pslog_field *dst,
                                          const pslog_field *src) {
  memset(dst, 0, sizeof(*dst));
  dst->key = src->key;
  dst->key_len = src->key_len;
  dst->value_len = src->value_len;
  dst->type = src->type;
  dst->trusted_key = src->trusted_key;
  dst->trusted_value = src->trusted_value;
  dst->console_simple_value = src->console_simple_value;
  dst->as = src->as;
}

static int pslog_copy_field(pslog_field *dst, const pslog_field *src) {
  char *key_copy;
  char *string_copy;
  void *bytes_copy;

  pslog_append_owned_field_copy(dst, src);
  if (src->key_len == (size_t)-1 ||
      (src->type == PSLOG_FIELD_STRING && src->value_len == (size_t)-1)) {
    return -1;
  }
  key_copy = (char *)pslog_malloc_internal(src->key_len + 1u);
  if (key_copy == NULL) {
    return -1;
  }
  if (src->key != NULL && src->key_len > 0u) {
    memcpy(key_copy, src->key, src->key_len);
  }
  key_copy[src->key_len] = '\0';
  dst->key = key_copy;

  if (src->type == PSLOG_FIELD_STRING) {
    string_copy = (char *)pslog_malloc_internal(src->value_len + 1u);
    if (string_copy == NULL) {
      pslog_free_internal((void *)dst->key);
      dst->key = NULL;
      return -1;
    }
    if (src->as.string_value != NULL && src->value_len > 0u) {
      memcpy(string_copy, src->as.string_value, src->value_len);
    }
    string_copy[src->value_len] = '\0';
    dst->as.string_value = string_copy;
  } else if (src->type == PSLOG_FIELD_BYTES) {
    if (src->as.bytes_value.len > 0u) {
      bytes_copy =
          pslog_memdup_local(src->as.bytes_value.data, src->as.bytes_value.len);
      if (bytes_copy == NULL) {
        pslog_free_internal((void *)dst->key);
        dst->key = NULL;
        return -1;
      }
      dst->as.bytes_value.data = (const unsigned char *)bytes_copy;
    } else {
      dst->as.bytes_value.data = NULL;
    }
  }
  return 0;
}

static void pslog_free_fields(pslog_field *fields, size_t count) {
  size_t i;

  if (fields == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    pslog_free_internal((void *)fields[i].key);
    if (fields[i].type == PSLOG_FIELD_STRING) {
      pslog_free_internal((void *)fields[i].as.string_value);
    } else if (fields[i].type == PSLOG_FIELD_BYTES) {
      pslog_free_internal((void *)fields[i].as.bytes_value.data);
    }
  }
  pslog_free_internal(fields);
}

static int pslog_copy_fields(pslog_field **out_fields, size_t *out_count,
                             const pslog_field *base_fields, size_t base_count,
                             const pslog_field *new_fields, size_t new_count) {
  size_t total;
  size_t i;
  pslog_field *copy;

  *out_fields = NULL;
  *out_count = 0u;
  if (!pslog_size_add(base_count, new_count, &total)) {
    return -1;
  }
  if (total == 0u) {
    return 0;
  }
  copy = (pslog_field *)pslog_calloc_internal(total, sizeof(*copy));
  if (copy == NULL) {
    return -1;
  }
  for (i = 0u; i < base_count; ++i) {
    if (pslog_copy_field(copy + i, base_fields + i) != 0) {
      pslog_free_fields(copy, i);
      return -1;
    }
  }
  for (i = 0u; i < new_count; ++i) {
    if (pslog_copy_field(copy + base_count + i, new_fields + i) != 0) {
      pslog_free_fields(copy, base_count + i);
      return -1;
    }
  }
  *out_fields = copy;
  *out_count = total;
  return 0;
}

static size_t
pslog_estimate_field_prefix_capacity(const pslog_field *fields, size_t count,
                                     pslog_mode mode, int color_enabled,
                                     const pslog_shared_state *shared) {
  size_t total;
  size_t i;
  size_t color_cost;
  const pslog_field *field;

  total = 0u;
  color_cost = 0u;
  if (color_enabled && shared != NULL && shared->palette != NULL) {
    color_cost = shared->palette_key_len + shared->palette_reset_len +
                 shared->palette_string_len + shared->palette_number_len +
                 shared->palette_boolean_len + shared->palette_null_value_len +
                 shared->palette_timestamp_len + shared->palette_message_len;
  }

  for (i = 0u; i < count; ++i) {
    field = fields + i;
    if (field->key == NULL || field->key_len == 0u) {
      continue;
    }
    if (mode == PSLOG_MODE_JSON) {
      total += 4u + (field->key_len * 6u) + color_cost;
    } else {
      total += 2u + (field->key_len * 4u) + color_cost;
    }
    switch (field->type) {
    case PSLOG_FIELD_NULL:
      total += 8u;
      break;
    case PSLOG_FIELD_STRING:
      total += 4u + (field->value_len * (mode == PSLOG_MODE_JSON ? 6u : 4u));
      break;
    case PSLOG_FIELD_SIGNED:
    case PSLOG_FIELD_UNSIGNED:
    case PSLOG_FIELD_DOUBLE:
    case PSLOG_FIELD_POINTER:
    case PSLOG_FIELD_DURATION:
      total += 64u;
      break;
    case PSLOG_FIELD_BOOL:
      total += 8u;
      break;
    case PSLOG_FIELD_BYTES:
      total += 4u + (field->as.bytes_value.len * 2u);
      break;
    case PSLOG_FIELD_TIME:
      total += 80u;
      break;
    default:
      total += 32u;
      break;
    }
  }
  return total + 32u;
}

static int pslog_build_field_prefix(pslog_logger_impl *impl) {
  pslog_buffer buffer;
  size_t capacity;

  if (impl == NULL || impl->shared == NULL) {
    return -1;
  }
  pslog_free_internal(impl->field_prefix);
  impl->field_prefix = NULL;
  impl->field_prefix_len = 0u;
  if (impl->fields == NULL || impl->field_count == 0u) {
    return 0;
  }

  capacity = pslog_estimate_field_prefix_capacity(
      impl->fields, impl->field_count, impl->shared->mode,
      impl->shared->color_enabled, impl->shared);
  pslog_buffer_init(&buffer, NULL, capacity);
  if (impl->shared->mode == PSLOG_MODE_JSON) {
    pslog_append_json_field_prefix(impl->shared, &buffer, impl->fields,
                                   impl->field_count);
  } else {
    pslog_append_console_field_prefix(impl->shared, &buffer, impl->fields,
                                      impl->field_count);
  }
  if (buffer.len > 0u) {
    impl->field_prefix =
        (char *)pslog_memdup_local(buffer.data, buffer.len + 1u);
    if (impl->field_prefix == NULL) {
      pslog_buffer_destroy(&buffer);
      return -1;
    }
    impl->field_prefix_len = buffer.len;
  }
  pslog_buffer_destroy(&buffer);
  return 0;
}

static pslog_logger *pslog_allocate_logger(pslog_shared_state *shared) {
  pslog_logger *logger;
  pslog_logger_impl *impl;

  logger = (pslog_logger *)pslog_calloc_internal(1u, sizeof(*logger));
  impl = (pslog_logger_impl *)pslog_calloc_internal(1u, sizeof(*impl));
  if (logger == NULL || impl == NULL) {
    pslog_free_internal(logger);
    pslog_free_internal(impl);
    return NULL;
  }
  impl->shared = shared;
  logger->impl = impl;
  logger->close = pslog_logger_close;
  logger->destroy = pslog_logger_destroy;
  logger->with = pslog_logger_with;
  logger->withf = pslog_logger_withf;
  logger->with_level = pslog_logger_with_level;
  logger->with_level_field = pslog_logger_with_level_field;
  logger->log = pslog_logger_log;
  logger->trace = pslog_logger_trace;
  logger->debug = pslog_logger_debug;
  logger->info = pslog_logger_info;
  logger->warn = pslog_logger_warn;
  logger->error = pslog_logger_error;
  logger->fatal = pslog_logger_fatal;
  logger->panic = pslog_logger_panic;
  logger->logf = pslog_logger_logf;
  logger->tracef = pslog_logger_tracef;
  logger->debugf = pslog_logger_debugf;
  logger->infof = pslog_logger_infof;
  logger->warnf = pslog_logger_warnf;
  logger->errorf = pslog_logger_errorf;
  logger->fatalf = pslog_logger_fatalf;
  logger->panicf = pslog_logger_panicf;
  logger->log_view = pslog_logger_log_view;
  logger->logf_view = pslog_logger_logf_view;
  logger->trace_view = pslog_logger_trace_view;
  logger->tracef_view = pslog_logger_tracef_view;
  logger->debug_view = pslog_logger_debug_view;
  logger->debugf_view = pslog_logger_debugf_view;
  logger->info_view = pslog_logger_info_view;
  logger->infof_view = pslog_logger_infof_view;
  logger->warn_view = pslog_logger_warn_view;
  logger->warnf_view = pslog_logger_warnf_view;
  logger->error_view = pslog_logger_error_view;
  logger->errorf_view = pslog_logger_errorf_view;
  logger->fatal_view = pslog_logger_fatal_view;
  logger->fatalf_view = pslog_logger_fatalf_view;
  logger->panic_view = pslog_logger_panic_view;
  logger->panicf_view = pslog_logger_panicf_view;
  impl->min_level = shared->min_level;
  impl->include_level_field = 0;
  impl->can_close_shared = shared->output.owned;
  return logger;
}

pslog_logger *pslog_clone_with_fields(pslog_logger *log,
                                      const pslog_field *fields, size_t count) {
  pslog_logger *clone;
  pslog_logger_impl *src_impl;
  pslog_logger_impl *dst_impl;

  if (log == NULL) {
    return NULL;
  }
  src_impl = (pslog_logger_impl *)log->impl;
  pslog_retain_shared(src_impl->shared);
  clone = pslog_allocate_logger(src_impl->shared);
  if (clone == NULL) {
    pslog_release_shared(src_impl->shared);
    return NULL;
  }
  dst_impl = (pslog_logger_impl *)clone->impl;
  if (pslog_copy_fields(&dst_impl->fields, &dst_impl->field_count,
                        src_impl->fields, src_impl->field_count, fields,
                        count) != 0) {
    clone->destroy(clone);
    return NULL;
  }
  if (pslog_build_field_prefix(dst_impl) != 0) {
    clone->destroy(clone);
    return NULL;
  }
  dst_impl->min_level = src_impl->min_level;
  dst_impl->include_level_field = src_impl->include_level_field;
  dst_impl->can_close_shared = 0;
  return clone;
}

void pslog_log_impl(pslog_logger *log, pslog_level level, const char *msg,
                    const pslog_field *fields, size_t count) {
  pslog_logger_impl *impl;
  pslog_string_view message;
  if (log == NULL) {
    return;
  }
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL || impl->shared == NULL || !pslog_should_log(impl, level)) {
    return;
  }
  message = pslog_cstr_view(msg);
  if (impl->shared->mode == PSLOG_MODE_JSON) {
    pslog_emit_json(impl, level, message, fields, count);
  } else {
    pslog_emit_console(impl, level, message, fields, count);
  }
}

void pslog_log_view_impl(pslog_logger *log, pslog_level level,
                         pslog_string_view msg, const pslog_field *fields,
                         size_t count) {
  pslog_logger_impl *impl;
  pslog_shared_state *shared;

  if (log == NULL) {
    return;
  }
  if (msg.data == NULL && msg.len != 0u) {
    return;
  }
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL || impl->shared == NULL) {
    return;
  }
  shared = impl->shared;
  if (!pslog_should_log(impl, level)) {
    return;
  }
  if (shared->mode == PSLOG_MODE_JSON) {
    pslog_emit_json(impl, level, msg, fields, count);
  } else {
    pslog_emit_console(impl, level, msg, fields, count);
  }
}

int pslog_parse_kvfmt(pslog_shared_state *shared, pslog_field *fields,
                      size_t *out_count, pslog_mode mode, const char *kvfmt,
                      int saved_errno, va_list ap) {
  pslog_kvfmt_cache_entry local_entry;
  const pslog_kvfmt_cache_entry *cache_entry;
  int used_local_entry;

  *out_count = 0u;
  if (kvfmt == NULL) {
    return 0;
  }
  local_entry.kvfmt_storage = NULL;
  used_local_entry = 0;
  if (shared != NULL) {
    pslog_state_lock(shared);
  }
  if (pslog_resolve_kvfmt_entry(shared, kvfmt, &local_entry, &cache_entry) !=
      0) {
    if (shared != NULL) {
      pslog_state_unlock(shared);
    }
    return -1;
  }
  used_local_entry = cache_entry == &local_entry;
  if (shared != NULL) {
    pslog_state_unlock(shared);
  }

  if (pslog_materialize_kvfmt_fields(cache_entry, fields, out_count, mode,
                                     saved_errno, ap) != 0) {
    if (used_local_entry) {
      pslog_reset_kvfmt_cache_entry(&local_entry);
    }
    return -1;
  }
  if (used_local_entry) {
    pslog_reset_kvfmt_cache_entry(&local_entry);
  }
  return 0;
}

void pslog_vlogf_impl(pslog_logger *log, pslog_level level, const char *msg,
                      const char *kvfmt, va_list ap) {
  pslog_logger_impl *impl;
  if (log == NULL) {
    return;
  }
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL || impl->shared == NULL || !pslog_should_log(impl, level)) {
    return;
  }
  pslog_vlogf_view_impl(log, level, pslog_cstr_view(msg), kvfmt, ap);
}

void pslog_vlogf_view_impl(pslog_logger *log, pslog_level level,
                           pslog_string_view msg, const char *kvfmt,
                           va_list ap) {
  const pslog_kvfmt_cache_entry *cache_entry;
  pslog_kvfmt_cache_entry local_entry;
  pslog_logger_impl *impl;
  pslog_shared_state *shared;
  pslog_level diagnostic_level;
  int saved_errno;
  int used_local_entry;

  saved_errno = errno;

  if (log == NULL) {
    return;
  }
  if (msg.data == NULL && msg.len != 0u) {
    return;
  }
  if (kvfmt == NULL) {
    pslog_log_view_impl(log, level, msg, NULL, 0u);
    return;
  }
  local_entry.kvfmt_storage = NULL;
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL || impl->shared == NULL) {
    return;
  }
  shared = impl->shared;
  if (!pslog_should_log(impl, level)) {
    return;
  }
  used_local_entry = 0;
  pslog_state_lock(shared);
  if (pslog_resolve_kvfmt_entry(shared, kvfmt, &local_entry, &cache_entry) !=
      0) {
    pslog_state_unlock(shared);
    diagnostic_level = PSLOG_LEVEL_ERROR;
    if (level == PSLOG_LEVEL_FATAL || level == PSLOG_LEVEL_PANIC) {
      diagnostic_level = level;
    }
    pslog_log_impl(log, diagnostic_level, "invalid kvfmt", NULL, 0u);
    return;
  }
  used_local_entry = cache_entry == &local_entry;
  pslog_state_unlock(shared);
  if (shared->mode == PSLOG_MODE_JSON) {
    pslog_emit_json_kvfmt(impl, level, msg, cache_entry, saved_errno, ap);
  } else {
    pslog_emit_console_kvfmt(impl, level, msg, cache_entry, saved_errno, ap);
  }
  if (used_local_entry) {
    pslog_reset_kvfmt_cache_entry(&local_entry);
  }
}

static int pslog_mutex_init(pslog_mutex *mutex) {
  pthread_mutexattr_t attr;
  int init_err;

  init_err = pthread_mutexattr_init(&attr);
  if (init_err != 0) {
    return pthread_mutex_init(mutex, (pthread_mutexattr_t *)0);
  }
#if defined(PTHREAD_MUTEX_ADAPTIVE_NP)
  (void)pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ADAPTIVE_NP);
#endif
  init_err = pthread_mutex_init(mutex, &attr);
  (void)pthread_mutexattr_destroy(&attr);
  return init_err;
}

static void pslog_mutex_destroy(pslog_mutex *mutex) {
  (void)pthread_mutex_destroy(mutex);
}

static void pslog_mutex_lock(pslog_mutex *mutex) {
  (void)pthread_mutex_lock(mutex);
}

static void pslog_mutex_unlock(pslog_mutex *mutex) {
  (void)pthread_mutex_unlock(mutex);
}

static int pslog_output_lock(pslog_shared_state *shared) {
  if (shared == NULL) {
    return -1;
  }
  pslog_mutex_lock(&shared->output_mutex);
  return 0;
}

static void pslog_output_unlock(pslog_shared_state *shared) {
  if (shared == NULL) {
    return;
  }
  pslog_mutex_unlock(&shared->output_mutex);
}

static int pslog_state_lock(pslog_shared_state *shared) {
  if (shared == NULL) {
    return -1;
  }
  pslog_mutex_lock(&shared->state_mutex);
  return 0;
}

static void pslog_state_unlock(pslog_shared_state *shared) {
  if (shared == NULL) {
    return;
  }
  pslog_mutex_unlock(&shared->state_mutex);
}

static int pslog_close_shared_locked(pslog_shared_state *shared,
                                     int allow_close) {
  int close_err;

  if (shared == NULL) {
    return 0;
  }
  if (shared->closed) {
    return shared->close_err;
  }
  if (!allow_close || !shared->output.owned) {
    return 0;
  }
  shared->closed = 1;
  close_err = 0;
  if (shared->output.close != NULL) {
    close_err = shared->output.close(shared->output.userdata);
  }
  shared->close_err = close_err;
  shared->output.close = NULL;
  return shared->close_err;
}

static int pslog_time_to_tm(time_t now, int utc, struct tm *tm_value) {
  if (tm_value == NULL) {
    return 0;
  }
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  if (utc) {
    return gmtime_r(&now, tm_value) != NULL;
  }
  return localtime_r(&now, tm_value) != NULL;
#else
  {
    struct tm *tm_ptr;

    if (utc) {
      tm_ptr = gmtime(&now);
    } else {
      tm_ptr = localtime(&now);
    }
    if (tm_ptr == NULL) {
      return 0;
    }
    *tm_value = *tm_ptr;
    return 1;
  }
#endif
}

static int pslog_local_offset_minutes(time_t now) {
  struct tm local_tm;

  if (!pslog_time_to_tm(now, 0, &local_tm)) {
    return 0;
  }

  {
    struct tm gm_tm;
    time_t local_epoch;
    time_t gm_epoch;

    if (!pslog_time_to_tm(now, 1, &gm_tm)) {
      return 0;
    }
    local_epoch = mktime(&local_tm);
    gm_epoch = mktime(&gm_tm);
    if (local_epoch == (time_t)-1 || gm_epoch == (time_t)-1) {
      return 0;
    }
    return (int)((local_epoch - gm_epoch) / 60);
  }
}

static void pslog_default_time_now(void *userdata, time_t *seconds_out,
                                   long *nanoseconds_out) {
  (void)userdata;
#if defined(CLOCK_REALTIME)
  {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
      if (seconds_out != NULL) {
        *seconds_out = ts.tv_sec;
      }
      if (nanoseconds_out != NULL) {
        *nanoseconds_out = (long)ts.tv_nsec;
      }
      return;
    }
  }
#endif
  if (seconds_out != NULL) {
    *seconds_out = time((time_t *)0);
  }
  if (nanoseconds_out != NULL) {
    *nanoseconds_out = 0l;
  }
}

static time_t pslog_default_time_now_seconds(void) { return time((time_t *)0); }

static int pslog_time_format_is_rfc3339(const char *fmt) {
  return fmt != NULL && strcmp(fmt, PSLOG_TIME_FORMAT_RFC3339) == 0;
}

static int pslog_time_format_is_rfc3339nano(const char *fmt) {
  return fmt != NULL && strcmp(fmt, PSLOG_TIME_FORMAT_RFC3339_NANO) == 0;
}

static size_t pslog_render_timestamp_value(pslog_shared_state *shared,
                                           time_t now, long nanoseconds,
                                           char *dst, size_t dst_size) {
  pslog_buffer buffer;
  struct tm tm_value;
  const char *fmt;
  pslog_time_value time_value;

  if (shared == NULL || dst == NULL || dst_size == 0u) {
    return 0u;
  }
  fmt = shared->time_format;
  if (fmt == NULL) {
    if (shared->mode == PSLOG_MODE_JSON) {
      size_t rendered_len;

      time_value.epoch_seconds = (long)now;
      time_value.nanoseconds = 0l;
      time_value.utc_offset_minutes =
          shared->utc ? 0 : pslog_local_offset_minutes(now);
      pslog_buffer_init(&buffer, NULL, dst_size - 1u);
      pslog_buffer_append_time(&buffer, time_value, 0);
      rendered_len = buffer.len;
      if (buffer.len >= dst_size) {
        buffer.len = dst_size - 1u;
        buffer.data[buffer.len] = '\0';
      }
      memcpy(dst, buffer.data, buffer.len + 1u);
      pslog_buffer_destroy(&buffer);
      return rendered_len < dst_size ? rendered_len : dst_size - 1u;
    }
    fmt = "%d%H%M";
  } else if (pslog_time_format_is_rfc3339(fmt) ||
             pslog_time_format_is_rfc3339nano(fmt)) {
    size_t rendered_len;

    time_value.epoch_seconds = (long)now;
    time_value.nanoseconds =
        pslog_time_format_is_rfc3339nano(fmt) ? nanoseconds : 0l;
    time_value.utc_offset_minutes =
        shared->utc ? 0 : pslog_local_offset_minutes(now);
    pslog_buffer_init(&buffer, NULL, dst_size - 1u);
    pslog_buffer_append_time(&buffer, time_value, 0);
    rendered_len = buffer.len;
    if (buffer.len >= dst_size) {
      buffer.len = dst_size - 1u;
      buffer.data[buffer.len] = '\0';
    }
    memcpy(dst, buffer.data, buffer.len + 1u);
    pslog_buffer_destroy(&buffer);
    return rendered_len < dst_size ? rendered_len : dst_size - 1u;
  }

  if (!pslog_time_to_tm(now, shared->utc, &tm_value)) {
    return 0u;
  }
  if (strftime(dst, dst_size, fmt, &tm_value) == 0u) {
    return 0u;
  }
  return strlen(dst);
}

static void pslog_build_console_level_fragment(pslog_shared_state *shared,
                                               pslog_level level, size_t slot) {
  const char *color_code;
  const char *label;
  size_t color_len;
  size_t label_len;
  size_t total;

  if (shared == NULL || slot >= 8u) {
    return;
  }
  label = pslog_level_console_label(level);
  label_len = pslog_level_console_label_len(level);
  color_code = pslog_level_color(shared->palette, level);
  color_len = pslog_level_color_len(shared, level);
  total = label_len;
  if (shared->color_enabled && color_code != NULL && color_len > 0u) {
    total += color_len + shared->palette_reset_len;
  }
  if (total >= PSLOG_CONSOLE_LEVEL_FRAGMENT_CAPACITY) {
    total = label_len;
    color_len = 0u;
    color_code = "";
  }
  shared->console_level_fragment_lens[slot] = 0u;
  if (shared->color_enabled && color_len > 0u) {
    memcpy(shared->console_level_fragments[slot], color_code, color_len);
    shared->console_level_fragment_lens[slot] += color_len;
  }
  memcpy(shared->console_level_fragments[slot] +
             shared->console_level_fragment_lens[slot],
         label, label_len);
  shared->console_level_fragment_lens[slot] += label_len;
  if (shared->color_enabled && shared->palette_reset_len > 0u &&
      color_len > 0u) {
    memcpy(shared->console_level_fragments[slot] +
               shared->console_level_fragment_lens[slot],
           shared->palette->reset, shared->palette_reset_len);
    shared->console_level_fragment_lens[slot] += shared->palette_reset_len;
  }
  shared->console_level_fragments[slot]
                                 [shared->console_level_fragment_lens[slot]] =
      '\0';
}

static void pslog_append_json_color_string(pslog_buffer *buffer,
                                           const char *color, size_t color_len,
                                           const char *text, size_t text_len,
                                           const char *suffix,
                                           size_t suffix_len) {
  if (color != NULL && color_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, color, color_len);
  }
  if (text != NULL && text_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, text, text_len);
  }
  if (suffix != NULL && suffix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, suffix, suffix_len);
  }
}

static void pslog_store_json_color_literal(size_t *len_out, char *dst,
                                           size_t capacity,
                                           const pslog_buffer *buffer) {
  if (len_out == NULL || dst == NULL || capacity == 0u || buffer == NULL) {
    return;
  }
  if (buffer->len + 1u > capacity) {
    *len_out = 0u;
    dst[0] = '\0';
    return;
  }
  *len_out = buffer->len;
  memcpy(dst, buffer->data, buffer->len + 1u);
}

static void pslog_build_json_color_literals(pslog_shared_state *shared) {
  static const pslog_level levels[] = {PSLOG_LEVEL_TRACE, PSLOG_LEVEL_DEBUG,
                                       PSLOG_LEVEL_INFO,  PSLOG_LEVEL_WARN,
                                       PSLOG_LEVEL_ERROR, PSLOG_LEVEL_FATAL,
                                       PSLOG_LEVEL_PANIC, PSLOG_LEVEL_NOLEVEL};
  static const char short_level_key[] = "\"lvl\":";
  static const char verbose_level_key[] = "\"level\":";
  static const char loglevel_key[] = "\"loglevel\":";
  static const char short_msg_key[] = "\"msg\":";
  static const char verbose_msg_key[] = "\"message\":";
  static const char short_time_key[] = "\"ts\":";
  static const char verbose_time_key[] = "\"time\":";
  const char *level_key;
  const char *msg_key;
  const char *time_key;
  size_t level_key_len;
  size_t msg_key_len;
  size_t time_key_len;
  size_t i;

  if (shared == NULL) {
    return;
  }
  level_key = shared->verbose_fields ? verbose_level_key : short_level_key;
  level_key_len = shared->verbose_fields ? sizeof(verbose_level_key) - 1u
                                         : sizeof(short_level_key) - 1u;
  msg_key = shared->verbose_fields ? verbose_msg_key : short_msg_key;
  msg_key_len = shared->verbose_fields ? sizeof(verbose_msg_key) - 1u
                                       : sizeof(short_msg_key) - 1u;
  time_key = shared->verbose_fields ? verbose_time_key : short_time_key;
  time_key_len = shared->verbose_fields ? sizeof(verbose_time_key) - 1u
                                        : sizeof(short_time_key) - 1u;

  for (i = 0u; i < 8u; ++i) {
    pslog_buffer buffer;
    pslog_level level;

    level = levels[i];

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
    pslog_append_json_color_string(&buffer, shared->palette->key,
                                   shared->palette_key_len, level_key,
                                   level_key_len, NULL, 0u);
    pslog_append_json_color_string(
        &buffer, pslog_level_color(shared->palette, level),
        pslog_level_color_len(shared, level), "\"", 1u, NULL, 0u);
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, pslog_level_string(level),
                               pslog_level_string_len(level));
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->reset,
                               shared->palette_reset_len);
    pslog_store_json_color_literal(&shared->json_color_level_field_lens[i],
                                   shared->json_color_level_fields[i],
                                   sizeof(shared->json_color_level_fields[i]),
                                   &buffer);
    pslog_buffer_destroy(&buffer);

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
    pslog_append_json_color_string(&buffer, shared->palette->key,
                                   shared->palette_key_len, loglevel_key,
                                   sizeof(loglevel_key) - 1u, NULL, 0u);
    pslog_append_json_color_string(
        &buffer, pslog_level_color(shared->palette, level),
        pslog_level_color_len(shared, level), "\"", 1u, NULL, 0u);
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, pslog_level_string(level),
                               pslog_level_string_len(level));
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->reset,
                               shared->palette_reset_len);
    pslog_store_json_color_literal(
        &shared->json_color_min_level_field_lens[i],
        shared->json_color_min_level_fields[i],
        sizeof(shared->json_color_min_level_fields[i]), &buffer);
    pslog_buffer_destroy(&buffer);
  }

  {
    pslog_buffer buffer;

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
    pslog_append_json_color_string(&buffer, shared->palette->message_key,
                                   shared->palette_message_key_len, msg_key,
                                   msg_key_len, NULL, 0u);
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->message,
                               shared->palette_message_len);
    pslog_store_json_color_literal(&shared->json_color_message_prefix_len,
                                   shared->json_color_message_prefix,
                                   sizeof(shared->json_color_message_prefix),
                                   &buffer);
    pslog_buffer_destroy(&buffer);

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
    pslog_append_json_color_string(&buffer, shared->palette->key,
                                   shared->palette_key_len, time_key,
                                   time_key_len, NULL, 0u);
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->reset,
                               shared->palette_reset_len);
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->timestamp,
                               shared->palette_timestamp_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    pslog_store_json_color_literal(&shared->json_color_timestamp_prefix_len,
                                   shared->json_color_timestamp_prefix,
                                   sizeof(shared->json_color_timestamp_prefix),
                                   &buffer);
    pslog_buffer_destroy(&buffer);

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->reset,
                               shared->palette_reset_len);
    pslog_store_json_color_literal(&shared->json_color_timestamp_suffix_len,
                                   shared->json_color_timestamp_suffix,
                                   sizeof(shared->json_color_timestamp_suffix),
                                   &buffer);
    pslog_buffer_destroy(&buffer);
  }
}

static void pslog_build_console_literals(pslog_shared_state *shared) {
  pslog_buffer buffer;

  if (shared == NULL) {
    return;
  }

  pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_LITERAL_CAPACITY - 1u);
  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ' ');
  if (shared->color_enabled) {
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->message,
                               shared->palette_message_len);
  }
  pslog_store_json_color_literal(
      &shared->console_message_prefix_len, shared->console_message_prefix,
      sizeof(shared->console_message_prefix), &buffer);
  pslog_buffer_destroy(&buffer);
}

static void pslog_prepare_kvfmt_cache_entry(pslog_shared_state *shared,
                                            pslog_kvfmt_cache_entry *entry) {
  size_t i;
  const pslog_palette *palette;
  const char *value_color;
  size_t value_color_len;

  if (shared == NULL || entry == NULL) {
    return;
  }
  palette = shared->palette;
  for (i = 0u; i < entry->field_count; ++i) {
    pslog_buffer buffer;

    pslog_buffer_init(&buffer, NULL, PSLOG_CONSOLE_KVFMT_PREFIX_CAPACITY - 1u);
    if (shared->color_enabled) {
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, palette->reset,
                                 shared->palette_reset_len);
    }
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ' ');
    if (shared->color_enabled) {
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, palette->key,
                                 shared->palette_key_len);
    }
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, entry->fields[i].key,
                               entry->fields[i].key_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '=');
    value_color = "";
    value_color_len = 0u;
    if (shared->color_enabled) {
      switch (entry->fields[i].arg_type) {
      case PSLOG_KVFMT_ARG_STRING:
        value_color = palette->string;
        value_color_len = shared->palette_string_len;
        break;
      case PSLOG_KVFMT_ARG_SIGNED_INT:
      case PSLOG_KVFMT_ARG_SIGNED_LONG:
      case PSLOG_KVFMT_ARG_UNSIGNED_INT:
      case PSLOG_KVFMT_ARG_UNSIGNED_LONG:
      case PSLOG_KVFMT_ARG_DOUBLE:
      case PSLOG_KVFMT_ARG_POINTER:
        value_color = palette->number;
        value_color_len = shared->palette_number_len;
        break;
      case PSLOG_KVFMT_ARG_BOOL:
        value_color = palette->boolean;
        value_color_len = shared->palette_boolean_len;
        break;
      default:
        break;
      }
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, palette->reset,
                                 shared->palette_reset_len);
      if (value_color != NULL && value_color_len > 0u) {
        PSLOG_BUFFER_APPEND_N_FAST(&buffer, value_color, value_color_len);
      }
    }
    if (buffer.len < sizeof(entry->fields[i].console_prefix)) {
      entry->fields[i].console_prefix_len = buffer.len;
      memcpy(entry->fields[i].console_prefix, buffer.data, buffer.len + 1u);
    } else {
      entry->fields[i].console_prefix_len = 0u;
      entry->fields[i].console_prefix[0] = '\0';
    }
    pslog_buffer_destroy(&buffer);

    pslog_buffer_init(&buffer, NULL, PSLOG_JSON_COLOR_KEY_PREFIX_CAPACITY - 1u);
    if (shared->color_enabled) {
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, shared->palette->key,
                                 shared->palette_key_len);
    }
    PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(
        &buffer, entry->fields[i].key, entry->fields[i].key_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ':');
    if (buffer.len < sizeof(entry->fields[i].json_color_prefix)) {
      entry->fields[i].json_color_prefix_len = buffer.len;
      memcpy(entry->fields[i].json_color_prefix, buffer.data, buffer.len + 1u);
    } else {
      entry->fields[i].json_color_prefix_len = 0u;
      entry->fields[i].json_color_prefix[0] = '\0';
    }
    pslog_buffer_destroy(&buffer);
  }
}

static void pslog_logger_destroy(pslog_logger *log) {
  pslog_logger_impl *impl;

  if (log == NULL) {
    return;
  }
  impl = (pslog_logger_impl *)log->impl;
  if (impl != NULL) {
    pslog_free_fields(impl->fields, impl->field_count);
    pslog_free_internal(impl->field_prefix);
    pslog_release_shared(impl->shared);
    pslog_free_internal(impl);
  }
  pslog_free_internal(log);
}

static int pslog_logger_close(pslog_logger *log) { return pslog_close(log); }

static pslog_logger *
pslog_logger_with(pslog_logger *log, const pslog_field *fields, size_t count) {
  return pslog_clone_with_fields(log, fields, count);
}

static pslog_logger *pslog_vwithf_impl(pslog_logger *log, const char *kvfmt,
                                       va_list ap) {
  pslog_logger_impl *impl;
  const pslog_kvfmt_cache_entry *cache_entry;
  pslog_kvfmt_cache_entry local_entry;
  pslog_field fields[PSLOG_KVFMT_MAX_FIELDS];
  size_t count;
  int saved_errno;
  pslog_logger *child;
  int used_local_entry;

  if (log == NULL) {
    return NULL;
  }
  if (kvfmt == NULL) {
    return pslog_clone_with_fields(log, NULL, 0u);
  }
  local_entry.kvfmt_storage = NULL;
  impl = (pslog_logger_impl *)log->impl;
  if (impl == NULL || impl->shared == NULL) {
    return NULL;
  }
  saved_errno = errno;
  used_local_entry = 0;
  pslog_state_lock(impl->shared);
  if (pslog_resolve_kvfmt_entry(impl->shared, kvfmt, &local_entry,
                                &cache_entry) != 0) {
    pslog_state_unlock(impl->shared);
    return NULL;
  }
  used_local_entry = cache_entry == &local_entry;
  pslog_state_unlock(impl->shared);
  if (pslog_materialize_kvfmt_fields(cache_entry, fields, &count,
                                     impl->shared->mode, saved_errno,
                                     ap) != 0) {
    if (used_local_entry) {
      pslog_reset_kvfmt_cache_entry(&local_entry);
    }
    return NULL;
  }
  child = pslog_clone_with_fields(log, fields, count);
  if (used_local_entry) {
    pslog_reset_kvfmt_cache_entry(&local_entry);
  }
  return child;
}

static pslog_logger *pslog_logger_withf(pslog_logger *log, const char *kvfmt,
                                        ...) {
  pslog_logger *child;
  va_list ap;

  va_start(ap, kvfmt);
  child = pslog_vwithf_impl(log, kvfmt, ap);
  va_end(ap);
  return child;
}

static pslog_logger *pslog_logger_with_level(pslog_logger *log,
                                             pslog_level level) {
  pslog_logger *clone;
  pslog_logger_impl *clone_impl;

  clone = pslog_clone_with_fields(log, NULL, 0u);
  if (clone == NULL) {
    return NULL;
  }
  clone_impl = (pslog_logger_impl *)clone->impl;
  clone_impl->min_level = level;
  return clone;
}

static pslog_logger *pslog_logger_with_level_field(pslog_logger *log) {
  pslog_logger *clone;
  pslog_logger_impl *clone_impl;

  clone = pslog_clone_with_fields(log, NULL, 0u);
  if (clone == NULL) {
    return NULL;
  }
  clone_impl = (pslog_logger_impl *)clone->impl;
  clone_impl->include_level_field = 1;
  return clone;
}

static void pslog_logger_log(pslog_logger *log, pslog_level level,
                             const char *msg, const pslog_field *fields,
                             size_t count) {
  pslog_log_impl(log, level, msg, fields, count);
}

static void pslog_logger_trace(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_TRACE, msg, fields, count);
}

static void pslog_logger_debug(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_DEBUG, msg, fields, count);
}

static void pslog_logger_info(pslog_logger *log, const char *msg,
                              const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_INFO, msg, fields, count);
}

static void pslog_logger_warn(pslog_logger *log, const char *msg,
                              const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_WARN, msg, fields, count);
}

static void pslog_logger_error(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_ERROR, msg, fields, count);
}

static void pslog_logger_fatal(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_FATAL, msg, fields, count);
  exit(EXIT_FAILURE);
}

static void pslog_logger_panic(pslog_logger *log, const char *msg,
                               const pslog_field *fields, size_t count) {
  pslog_log_impl(log, PSLOG_LEVEL_PANIC, msg, fields, count);
  abort();
}

static void pslog_logger_logf(pslog_logger *log, pslog_level level,
                              const char *msg, const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, level, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_tracef(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_TRACE, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_debugf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_DEBUG, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_infof(pslog_logger *log, const char *msg,
                               const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_INFO, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_warnf(pslog_logger *log, const char *msg,
                               const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_WARN, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_errorf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_ERROR, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_fatalf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_FATAL, msg, kvfmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

static void pslog_logger_panicf(pslog_logger *log, const char *msg,
                                const char *kvfmt, ...) {
  va_list ap;

  va_start(ap, kvfmt);
  pslog_vlogf_impl(log, PSLOG_LEVEL_PANIC, msg, kvfmt, ap);
  va_end(ap);
  abort();
}

static void pslog_logger_log_view(pslog_logger *log, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, level, msg, fields, count);
}

static void pslog_logger_trace_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_TRACE, msg, fields, count);
}

static void pslog_logger_debug_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_DEBUG, msg, fields, count);
}

static void pslog_logger_info_view(pslog_logger *log, pslog_string_view msg,
                                   const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_INFO, msg, fields, count);
}

static void pslog_logger_warn_view(pslog_logger *log, pslog_string_view msg,
                                   const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_WARN, msg, fields, count);
}

static void pslog_logger_error_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_ERROR, msg, fields, count);
}

static void pslog_logger_fatal_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_FATAL, msg, fields, count);
  exit(EXIT_FAILURE);
}

static void pslog_logger_panic_view(pslog_logger *log, pslog_string_view msg,
                                    const pslog_field *fields, size_t count) {
  pslog_log_view_impl(log, PSLOG_LEVEL_PANIC, msg, fields, count);
  abort();
}

static void pslog_logger_logf_view(pslog_logger *log, pslog_level level,
                                   pslog_string_view msg, const char *kvfmt,
                                   ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, level, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_tracef_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_TRACE, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_debugf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_DEBUG, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_infof_view(pslog_logger *log, pslog_string_view msg,
                                    const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_INFO, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_warnf_view(pslog_logger *log, pslog_string_view msg,
                                    const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_WARN, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_errorf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_ERROR, msg, kvfmt, ap);
  va_end(ap);
}

static void pslog_logger_fatalf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_FATAL, msg, kvfmt, ap);
  va_end(ap);
  exit(EXIT_FAILURE);
}

static void pslog_logger_panicf_view(pslog_logger *log, pslog_string_view msg,
                                     const char *kvfmt, ...) {
  va_list ap;
  va_start(ap, kvfmt);
  pslog_vlogf_view_impl(log, PSLOG_LEVEL_PANIC, msg, kvfmt, ap);
  va_end(ap);
  abort();
}

static int pslog_file_write(void *userdata, const char *data, size_t len,
                            size_t *written) {
  FILE *fp;
  size_t result;
  int err;
  int flush_err;

  fp = (FILE *)userdata;
  if (fp == NULL) {
    if (written != NULL) {
      *written = 0u;
    }
    return -1;
  }
  errno = 0;
  result = fwrite(data, 1u, len, fp);
  if (written != NULL) {
    *written = result;
  }
  flush_err = fflush(fp);
  if (result == len && flush_err == 0) {
    return 0;
  }
  err = errno;
  if (result == len && flush_err != 0 && err == 0) {
    err = EIO;
  }
  if (err == 0) {
    err = EIO;
  }
  return err;
}

static int pslog_file_close(void *userdata) {
  FILE *fp;

  fp = (FILE *)userdata;
  if (fp != NULL) {
    errno = 0;
    if (fclose(fp) == 0) {
      return 0;
    }
    if (errno != 0) {
      return errno;
    }
    return EIO;
  }
  return 0;
}

static int pslog_file_isatty(void *userdata) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  FILE *fp;
  int fd;

  fp = (FILE *)userdata;
  if (fp == NULL) {
    return 0;
  }
  fd = fileno(fp);
  if (fd < 0) {
    return 0;
  }
  return isatty(fd) ? 1 : 0;
#else
  (void)userdata;
  return 0;
#endif
}

static int pslog_fd_write(void *userdata, const char *data, size_t len,
                          size_t *written) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  pslog_fd_output *output;
  ssize_t result;

  output = (pslog_fd_output *)userdata;
  if (output == NULL || output->fd < 0) {
    if (written != NULL) {
      *written = 0u;
    }
    return -1;
  }
  errno = 0;
  result = write(output->fd, data, len);
  if (result < 0) {
    if (written != NULL) {
      *written = 0u;
    }
    return errno != 0 ? errno : -1;
  }
  if (written != NULL) {
    *written = (size_t)result;
  }
  return 0;
#else
  (void)userdata;
  (void)data;
  if (written != NULL) {
    *written = 0u;
  }
  return -1;
#endif
}

static int pslog_fd_close(void *userdata) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  pslog_fd_output *output;
  int err;

  output = (pslog_fd_output *)userdata;
  if (output == NULL) {
    return 0;
  }
  err = 0;
  if (output->fd >= 0) {
    errno = 0;
    if (close(output->fd) != 0) {
      err = errno != 0 ? errno : -1;
    }
  }
  pslog_free_internal(output);
  return err;
#else
  pslog_free_internal(userdata);
  return -1;
#endif
}

static int pslog_fd_isatty(void *userdata) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  pslog_fd_output *output;

  output = (pslog_fd_output *)userdata;
  if (output == NULL || output->fd < 0) {
    return 0;
  }
  return isatty(output->fd) ? 1 : 0;
#else
  (void)userdata;
  return 0;
#endif
}

static int pslog_tee_write(void *userdata, const char *data, size_t len,
                           size_t *written) {
  pslog_tee_output *tee;
  size_t first_written;
  size_t second_written;
  size_t current_written;
  size_t common_written;
  int first_err;
  int second_err;
  int progressed;

  tee = (pslog_tee_output *)userdata;
  if (tee == NULL) {
    if (written != NULL) {
      *written = 0u;
    }
    return -1;
  }
  first_written = tee->first.write != NULL ? 0u : len;
  second_written = tee->second.write != NULL ? 0u : len;
  first_err = 0;
  second_err = 0;

  while (first_written < len || second_written < len) {
    progressed = 0;
    if (first_written < len) {
      current_written = 0u;
      first_err = tee->first.write(tee->first.userdata, data + first_written,
                                   len - first_written, &current_written);
      if (current_written > len - first_written) {
        current_written = len - first_written;
      }
      first_written += current_written;
      if (current_written > 0u) {
        progressed = 1;
      }
      if (first_err != 0) {
        break;
      }
      if (current_written == 0u) {
        first_err = EIO;
        break;
      }
    }
    if (second_written < len) {
      current_written = 0u;
      second_err =
          tee->second.write(tee->second.userdata, data + second_written,
                            len - second_written, &current_written);
      if (current_written > len - second_written) {
        current_written = len - second_written;
      }
      second_written += current_written;
      if (current_written > 0u) {
        progressed = 1;
      }
      if (second_err != 0) {
        break;
      }
      if (current_written == 0u) {
        second_err = EIO;
        break;
      }
    }
    if (!progressed) {
      break;
    }
  }
  common_written =
      first_written < second_written ? first_written : second_written;
  if (written != NULL) {
    *written = common_written;
  }
  if (first_written == len && second_written == len) {
    return 0;
  }
  if (first_err != 0) {
    return first_err;
  }
  if (second_err != 0) {
    return second_err;
  }
  return EIO;
}

static int pslog_tee_close(void *userdata) {
  pslog_tee_output *tee;
  int err;
  int close_err;

  tee = (pslog_tee_output *)userdata;
  if (tee == NULL) {
    return 0;
  }
  err = 0;
  if (tee->first.close != NULL && tee->first.owned) {
    close_err = tee->first.close(tee->first.userdata);
    if (err == 0) {
      err = close_err;
    }
  }
  if (tee->second.close != NULL && tee->second.owned) {
    close_err = tee->second.close(tee->second.userdata);
    if (err == 0) {
      err = close_err;
    }
  }
  pslog_free_internal(tee);
  return err;
}

static int pslog_tee_isatty(void *userdata) {
  pslog_tee_output *tee;

  tee = (pslog_tee_output *)userdata;
  if (tee == NULL) {
    return 0;
  }
  if (tee->first.isatty != NULL && tee->first.isatty(tee->first.userdata)) {
    return 1;
  }
  if (tee->second.isatty != NULL && tee->second.isatty(tee->second.userdata)) {
    return 1;
  }
  return 0;
}

static int pslog_observed_write(void *userdata, const char *data, size_t len,
                                size_t *written) {
  pslog_observed_output *observed;
  pslog_write_failure failure;
  size_t current_written;
  int err;

  observed = (pslog_observed_output *)userdata;
  if (observed == NULL || observed->target.write == NULL) {
    if (written != NULL) {
      *written = len;
    }
    return 0;
  }

  current_written = 0u;
  err = observed->target.write(observed->target.userdata, data, len,
                               &current_written);
  if (written != NULL) {
    *written = current_written;
  }
  if (err == 0 && current_written == len) {
    return 0;
  }

  observed->failures += 1u;
  failure.error_code = err;
  failure.short_write = current_written != len;
  failure.written = current_written;
  failure.attempted = len;
  if (failure.short_write) {
    observed->short_writes += 1u;
  }
  if (observed->on_failure != NULL) {
    observed->on_failure(observed->failure_userdata, &failure);
  }
  return err;
}

static int pslog_observed_close(void *userdata) {
  pslog_observed_output *observed;

  observed = (pslog_observed_output *)userdata;
  if (observed == NULL || observed->target.close == NULL ||
      !observed->target.owned) {
    return 0;
  }
  return observed->target.close(observed->target.userdata);
}

static int pslog_observed_isatty(void *userdata) {
  pslog_observed_output *observed;

  observed = (pslog_observed_output *)userdata;
  if (observed == NULL || observed->target.isatty == NULL) {
    return 0;
  }
  return observed->target.isatty(observed->target.userdata);
}

static int pslog_parse_bool_value(const char *text, int *value) {
  char normalized[16];
  size_t i;

  if (text == NULL || value == NULL) {
    return 0;
  }
  pslog_trim_copy(normalized, sizeof(normalized), text);
  i = 0u;
  while (normalized[i] != '\0') {
    normalized[i] = (char)tolower((unsigned char)normalized[i]);
    ++i;
  }

  if (strcmp(normalized, "1") == 0 || strcmp(normalized, "true") == 0 ||
      strcmp(normalized, "yes") == 0 || strcmp(normalized, "on") == 0) {
    *value = 1;
    return 1;
  }
  if (strcmp(normalized, "0") == 0 || strcmp(normalized, "false") == 0 ||
      strcmp(normalized, "no") == 0 || strcmp(normalized, "off") == 0) {
    *value = 0;
    return 1;
  }
  return 0;
}

static int pslog_parse_mode_value(const char *text, pslog_mode *mode) {
  char normalized[16];
  size_t i;

  if (text == NULL || mode == NULL) {
    return 0;
  }
  pslog_trim_copy(normalized, sizeof(normalized), text);
  i = 0u;
  while (normalized[i] != '\0') {
    normalized[i] = (char)tolower((unsigned char)normalized[i]);
    ++i;
  }
  if (strcmp(normalized, "json") == 0 ||
      strcmp(normalized, "structured") == 0) {
    *mode = PSLOG_MODE_JSON;
    return 1;
  }
  if (strcmp(normalized, "console") == 0) {
    *mode = PSLOG_MODE_CONSOLE;
    return 1;
  }
  return 0;
}

static int pslog_parse_file_mode(const char *text, int *mode) {
  char trimmed[32];
  unsigned long parsed;
  const char *cursor;
  size_t i;

  if (text == NULL || mode == NULL) {
    return 0;
  }
  pslog_trim_copy(trimmed, sizeof(trimmed), text);
  if (trimmed[0] == '\0') {
    return 0;
  }
  cursor = trimmed;
  if (cursor[0] == '0' && (cursor[1] == 'o' || cursor[1] == 'O')) {
    cursor += 2;
  }
  if (*cursor == '\0') {
    return 0;
  }
  i = 0u;
  while (cursor[i] != '\0') {
    if (cursor[i] < '0' || cursor[i] > '7') {
      return 0;
    }
    ++i;
  }
  parsed = strtoul(cursor, (char **)0, 8);
  if (parsed > 0777ul) {
    return 0;
  }
  *mode = (int)parsed;
  return 1;
}

static const char *pslog_env_prefix(const char *prefix) {
  return (prefix != NULL && *prefix != '\0') ? prefix : "LOG_";
}

static int pslog_env_copy(char *dst, size_t dst_size, const char *prefix,
                          const char *suffix) {
  size_t prefix_len;
  size_t suffix_len;

  if (dst == NULL || prefix == NULL || suffix == NULL) {
    return 0;
  }
  prefix_len = strlen(prefix);
  suffix_len = strlen(suffix);
  if (prefix_len + suffix_len + 1u > dst_size) {
    return 0;
  }
  memcpy(dst, prefix, prefix_len);
  memcpy(dst + prefix_len, suffix, suffix_len + 1u);
  return 1;
}

static void pslog_trim_copy(char *dst, size_t dst_size, const char *text) {
  const char *start;
  const char *end;
  size_t len;

  if (dst == NULL || dst_size == 0u) {
    return;
  }
  if (text == NULL) {
    dst[0] = '\0';
    return;
  }
  start = text;
  while (*start != '\0' && isspace((unsigned char)*start)) {
    ++start;
  }
  end = start + strlen(start);
  while (end > start && isspace((unsigned char)end[-1])) {
    --end;
  }
  len = (size_t)(end - start);
  if (len >= dst_size) {
    len = dst_size - 1u;
  }
  memcpy(dst, start, len);
  dst[len] = '\0';
}

static int pslog_string_iequal(const char *lhs, const char *rhs) {
  unsigned char left;
  unsigned char right;

  if (lhs == NULL || rhs == NULL) {
    return 0;
  }
  while (*lhs != '\0' && *rhs != '\0') {
    left = (unsigned char)*lhs;
    right = (unsigned char)*rhs;
    if (tolower(left) != tolower(right)) {
      return 0;
    }
    ++lhs;
    ++rhs;
  }
  return *lhs == '\0' && *rhs == '\0';
}

static int pslog_string_has_prefix_ci(const char *text, const char *prefix) {
  unsigned char left;
  unsigned char right;

  if (text == NULL || prefix == NULL) {
    return 0;
  }
  while (*prefix != '\0') {
    if (*text == '\0') {
      return 0;
    }
    left = (unsigned char)*text;
    right = (unsigned char)*prefix;
    if (tolower(left) != tolower(right)) {
      return 0;
    }
    ++text;
    ++prefix;
  }
  return 1;
}

static void pslog_output_reset(pslog_output *output) {
  if (output != NULL) {
    memset(output, 0, sizeof(*output));
  }
}

static int pslog_output_equals(const pslog_output *lhs,
                               const pslog_output *rhs) {
  if (lhs == rhs) {
    return 1;
  }
  if (lhs == NULL || rhs == NULL) {
    return 0;
  }
  return lhs->write == rhs->write && lhs->close == rhs->close &&
         lhs->isatty == rhs->isatty && lhs->userdata == rhs->userdata &&
         lhs->owned == rhs->owned;
}

static int pslog_output_contains(const pslog_output *output,
                                 const pslog_output *candidate) {
  const pslog_tee_output *tee;

  if (output == NULL || candidate == NULL) {
    return 0;
  }
  if (pslog_output_equals(output, candidate)) {
    return 1;
  }
  if (output->write != pslog_tee_write || output->userdata == NULL) {
    return 0;
  }
  tee = (const pslog_tee_output *)output->userdata;
  return pslog_output_contains(&tee->first, candidate) ||
         pslog_output_contains(&tee->second, candidate);
}

static int pslog_output_init_file_perms(pslog_output *output, const char *path,
                                        const char *mode, int file_mode) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  int fd;
  int flags;
  pslog_fd_output *fd_output;
#else
  int err;
  FILE *fp;
#endif

  if (output == NULL || path == NULL) {
    return -1;
  }
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  flags = O_WRONLY | O_CREAT;
  if (mode != NULL && strchr(mode, 'w') != NULL) {
    flags |= O_TRUNC;
  } else {
    flags |= O_APPEND;
  }
  errno = 0;
  fd = open(path, flags, (mode_t)file_mode);
  if (fd < 0) {
    return errno != 0 ? errno : -1;
  }
  fd_output = (pslog_fd_output *)pslog_calloc_internal(1u, sizeof(*fd_output));
  if (fd_output == NULL) {
    close(fd);
    return -1;
  }
  fd_output->fd = fd;
  output->write = pslog_fd_write;
  output->close = pslog_fd_close;
  output->isatty = pslog_fd_isatty;
  output->userdata = fd_output;
  output->owned = 1;
  return 0;
#else
  fp = fopen(path, mode != NULL ? mode : "ab");
  if (fp == NULL) {
    err = errno != 0 ? errno : -1;
    return err;
  }
  *output = pslog_output_from_fp(fp, 1);
  return 0;
#endif
}

static int pslog_output_init_tee(pslog_output *output, pslog_output first,
                                 pslog_output second) {
  pslog_tee_output *tee;

  if (output == NULL) {
    return -1;
  }
  tee = (pslog_tee_output *)pslog_calloc_internal(1u, sizeof(*tee));
  if (tee == NULL) {
    return -1;
  }
  tee->first = first;
  tee->second = second;
  output->write = pslog_tee_write;
  output->close = pslog_tee_close;
  output->isatty = pslog_tee_isatty;
  output->userdata = tee;
  output->owned = first.owned || second.owned;
  return 0;
}

static int pslog_output_init_env_value(pslog_output *output,
                                       const pslog_output *base_output,
                                       const char *value, int file_mode) {
  char trimmed[512];
  char path_value[512];
  const char *path;
  pslog_output first;
  pslog_output second;
  int err;

  if (output == NULL || base_output == NULL || value == NULL) {
    return -1;
  }
  pslog_trim_copy(trimmed, sizeof(trimmed), value);
  if (trimmed[0] == '\0') {
    *output = *base_output;
    return 0;
  }

  if (pslog_string_iequal(trimmed, "default")) {
    *output = *base_output;
    return 0;
  }
  if (pslog_string_iequal(trimmed, "stdout")) {
    *output = pslog_output_from_fp(stdout, 0);
    return 0;
  }
  if (pslog_string_iequal(trimmed, "stderr")) {
    *output = pslog_output_from_fp(stderr, 0);
    return 0;
  }
  if (pslog_string_has_prefix_ci(trimmed, "stdout+")) {
    path = trimmed + strlen("stdout+");
    pslog_trim_copy(path_value, sizeof(path_value), path);
    if (path_value[0] == '\0') {
      *output = pslog_output_from_fp(stdout, 0);
      return 0;
    }
    first = pslog_output_from_fp(stdout, 0);
    err = pslog_output_init_file_perms(&second, path_value, "ab", file_mode);
    if (err != 0) {
      return err;
    }
    err = pslog_output_init_tee(output, first, second);
    if (err != 0) {
      pslog_output_destroy(&second);
    }
    return err;
  }
  if (pslog_string_has_prefix_ci(trimmed, "stderr+")) {
    path = trimmed + strlen("stderr+");
    pslog_trim_copy(path_value, sizeof(path_value), path);
    if (path_value[0] == '\0') {
      *output = pslog_output_from_fp(stderr, 0);
      return 0;
    }
    first = pslog_output_from_fp(stderr, 0);
    err = pslog_output_init_file_perms(&second, path_value, "ab", file_mode);
    if (err != 0) {
      return err;
    }
    err = pslog_output_init_tee(output, first, second);
    if (err != 0) {
      pslog_output_destroy(&second);
    }
    return err;
  }
  if (pslog_string_has_prefix_ci(trimmed, "default+")) {
    path = trimmed + strlen("default+");
    pslog_trim_copy(path_value, sizeof(path_value), path);
    if (path_value[0] == '\0') {
      *output = *base_output;
      return 0;
    }
    first = *base_output;
    err = pslog_output_init_file_perms(&second, path_value, "ab", file_mode);
    if (err != 0) {
      return err;
    }
    err = pslog_output_init_tee(output, first, second);
    if (err != 0) {
      pslog_output_destroy(&second);
    }
    return err;
  }

  return pslog_output_init_file_perms(output, trimmed, "ab", file_mode);
}

static void pslog_buffer_append_console_escaped(pslog_buffer *buffer,
                                                const char *text, size_t len,
                                                int quote) {
  static const char hex[] = "0123456789abcdef";
  unsigned char ch;
  const char *cursor;
  const char *chunk_start;
  const char *end;

  if (quote) {
    pslog_buffer_append_char(buffer, '"');
  }
  if (text != NULL) {
    cursor = text;
    chunk_start = text;
    end = text + len;
    while (cursor < end) {
      ch = (unsigned char)*cursor;
      if (ch == '"' && quote) {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\\"");
        chunk_start = cursor + 1;
      } else if (ch == '\\' && quote) {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\\\");
        chunk_start = cursor + 1;
      } else if (ch == '\n') {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\n");
        chunk_start = cursor + 1;
      } else if (ch == '\r') {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\r");
        chunk_start = cursor + 1;
      } else if (ch == '\t') {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\t");
        chunk_start = cursor + 1;
      } else if (ch < 0x20u || ch == 0x7fu || ch == 0x1bu) {
        if (cursor > chunk_start) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                     (size_t)(cursor - chunk_start));
        }
        PSLOG_APPEND_LITERAL(buffer, "\\x");
        PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[(ch >> 4) & 0x0fu]);
        PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, hex[ch & 0x0fu]);
        chunk_start = cursor + 1;
      }
      ++cursor;
    }
    if (cursor > chunk_start) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, chunk_start,
                                 (size_t)(cursor - chunk_start));
    }
  }
  if (quote) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  }
}
