#ifndef PSLOG_INTERNAL_H
#define PSLOG_INTERNAL_H

#include "pslog.h"

#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <pthread.h>
typedef pthread_mutex_t pslog_mutex;
#else
#error                                                                         \
    "libpslog thread-safe builds require pthread mutex support on this target"
#endif

#define PSLOG_KVFMT_MAX_FIELDS 32
#define PSLOG_KVFMT_KEY_CAPACITY 64
#define PSLOG_KVFMT_CACHE_SIZE 512
#define PSLOG_KVFMT_CACHE_PROBE 8
#define PSLOG_KVFMT_PTR_CACHE_SIZE 256
#define PSLOG_CONSOLE_PREFIX_CACHE_SIZE 256
#define PSLOG_CONSOLE_PREFIX_CACHE_PROBE 16
#define PSLOG_CONSOLE_PREFIX_INLINE_CAPACITY 128
#define PSLOG_CONSOLE_LEVEL_FRAGMENT_CAPACITY 64
#define PSLOG_JSON_COLOR_LITERAL_CAPACITY 128
#define PSLOG_JSON_COLOR_KEY_PREFIX_CAPACITY 128
#define PSLOG_CONSOLE_KVFMT_PREFIX_CAPACITY 128
#define PSLOG_DOUBLE_CACHE_SIZE 32
#define PSLOG_DOUBLE_RENDERED_CAPACITY 32
#define PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ch)                              \
  do {                                                                         \
    pslog_buffer *_pslog_buffer = (buffer);                                    \
    if (_pslog_buffer != NULL && _pslog_buffer->data != NULL &&                \
        _pslog_buffer->capacity != 0u) {                                       \
      if (_pslog_buffer->len == _pslog_buffer->capacity) {                     \
        pslog_buffer_flush(_pslog_buffer);                                     \
      }                                                                        \
      if (_pslog_buffer->data != NULL &&                                       \
          _pslog_buffer->len < _pslog_buffer->capacity) {                      \
        _pslog_buffer->data[_pslog_buffer->len] = (ch);                        \
        ++_pslog_buffer->len;                                                  \
        _pslog_buffer->data[_pslog_buffer->len] = '\0';                        \
      }                                                                        \
    }                                                                          \
  } while (0)
#define PSLOG_BUFFER_APPEND_N_FAST(buffer, text, append_len)                   \
  do {                                                                         \
    pslog_buffer *_pslog_buffer = (buffer);                                    \
    const char *_pslog_text = (text);                                          \
    size_t _pslog_append_len = (append_len);                                   \
    if (_pslog_buffer != NULL && _pslog_buffer->data != NULL &&                \
        _pslog_buffer->capacity != 0u && _pslog_text != NULL &&                \
        _pslog_append_len != 0u) {                                             \
      size_t _pslog_available = _pslog_buffer->capacity - _pslog_buffer->len;  \
      if (_pslog_available >= _pslog_append_len) {                             \
        memcpy(_pslog_buffer->data + _pslog_buffer->len, _pslog_text,          \
               _pslog_append_len);                                             \
        _pslog_buffer->len += _pslog_append_len;                               \
        _pslog_buffer->data[_pslog_buffer->len] = '\0';                        \
      } else {                                                                 \
        pslog_buffer_append_n(_pslog_buffer, _pslog_text, _pslog_append_len);  \
      }                                                                        \
    }                                                                          \
  } while (0)
#define PSLOG_APPLY_COLOR_FAST(buffer, color_code, color_len, enabled)         \
  do {                                                                         \
    if ((enabled) && (color_code) != NULL && (color_len) > 0u) {               \
      PSLOG_BUFFER_APPEND_N_FAST((buffer), (color_code), (color_len));         \
    }                                                                          \
  } while (0)
#define PSLOG_APPEND_LITERAL(buffer, literal)                                  \
  PSLOG_BUFFER_APPEND_N_FAST((buffer), (literal), sizeof(literal) - 1u)
#define PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(buffer, text, text_len) \
  do {                                                                         \
    PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                              \
    PSLOG_BUFFER_APPEND_N_FAST((buffer), (text), (text_len));                  \
    PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                              \
  } while (0)
#define PSLOG_BUFFER_DESTROY_FAST(buffer)                                      \
  do {                                                                         \
    pslog_buffer *_pslog_buffer = (buffer);                                    \
    if (_pslog_buffer != NULL) {                                               \
      if (_pslog_buffer->heap_owned && _pslog_buffer->data != NULL) {          \
        pslog_free_internal(_pslog_buffer->data);                              \
      }                                                                        \
      _pslog_buffer->data = NULL;                                              \
      _pslog_buffer->len = 0u;                                                 \
      _pslog_buffer->capacity = 0u;                                            \
      _pslog_buffer->heap_owned = 0;                                           \
      _pslog_buffer->shared = NULL;                                            \
      _pslog_buffer->inline_data[0] = '\0';                                    \
    }                                                                          \
  } while (0)

typedef struct pslog_shared_state pslog_shared_state;

typedef struct pslog_test_allocator_stats {
  unsigned long malloc_calls;
  unsigned long calloc_calls;
  unsigned long realloc_calls;
  unsigned long free_calls;
} pslog_test_allocator_stats;

typedef struct pslog_buffer {
  pslog_shared_state *shared;
  char *data;
  size_t len;
  size_t capacity;
  size_t chunk_capacity;
  int output_locked;
  int heap_owned;
  char inline_data[PSLOG_DEFAULT_LINE_BUFFER_CAPACITY + 1u];
} pslog_buffer;

typedef enum pslog_kvfmt_arg_type {
  PSLOG_KVFMT_ARG_STRING = 0,
  PSLOG_KVFMT_ARG_SIGNED_INT = 1,
  PSLOG_KVFMT_ARG_SIGNED_LONG = 2,
  PSLOG_KVFMT_ARG_UNSIGNED_INT = 3,
  PSLOG_KVFMT_ARG_UNSIGNED_LONG = 4,
  PSLOG_KVFMT_ARG_DOUBLE = 5,
  PSLOG_KVFMT_ARG_POINTER = 6,
  PSLOG_KVFMT_ARG_BOOL = 7,
  PSLOG_KVFMT_ARG_ERRNO = 8
} pslog_kvfmt_arg_type;

typedef struct pslog_kvfmt_field_spec {
  const char *key;
  size_t key_len;
  pslog_kvfmt_arg_type arg_type;
  size_t console_prefix_len;
  char console_prefix[PSLOG_CONSOLE_KVFMT_PREFIX_CAPACITY];
  size_t json_color_prefix_len;
  char json_color_prefix[PSLOG_JSON_COLOR_KEY_PREFIX_CAPACITY];
} pslog_kvfmt_field_spec;

typedef struct pslog_kvfmt_cache_entry {
  const char *kvfmt;
  char *kvfmt_storage;
  size_t kvfmt_len;
  unsigned long kvfmt_hash;
  size_t field_count;
  pslog_kvfmt_field_spec fields[PSLOG_KVFMT_MAX_FIELDS];
} pslog_kvfmt_cache_entry;

typedef struct pslog_kvfmt_ptr_cache_entry {
  const char *input;
  const pslog_kvfmt_cache_entry *entry;
} pslog_kvfmt_ptr_cache_entry;

typedef struct pslog_console_prefix_cache_entry {
  const char *key;
  size_t key_len;
  pslog_field_type type;
  size_t prefix_len;
  char prefix[PSLOG_CONSOLE_PREFIX_INLINE_CAPACITY];
  char key_copy[PSLOG_CONSOLE_PREFIX_INLINE_CAPACITY];
} pslog_console_prefix_cache_entry;

typedef struct pslog_double_cache_entry {
  int valid;
  unsigned char bits[sizeof(double)];
  size_t rendered_len;
  char rendered[PSLOG_DOUBLE_RENDERED_CAPACITY];
} pslog_double_cache_entry;

struct pslog_shared_state {
  pslog_mutex state_mutex;
  pslog_mutex output_mutex;
  pslog_output output;
  pslog_mode mode;
  pslog_non_finite_float_policy non_finite_float_policy;
  size_t line_buffer_capacity;
  pslog_level min_level;
  int timestamps;
  int timestamp_cacheable;
  int utc;
  int verbose_fields;
  int color_enabled;
  const char *time_format;
  char *time_format_storage;
  const pslog_palette *palette;
  size_t palette_key_len;
  size_t palette_string_len;
  size_t palette_number_len;
  size_t palette_boolean_len;
  size_t palette_null_value_len;
  size_t palette_timestamp_len;
  size_t palette_message_len;
  size_t palette_message_key_len;
  size_t palette_reset_len;
  size_t level_color_lens[8];
  size_t console_level_fragment_lens[8];
  char console_level_fragments[8][PSLOG_CONSOLE_LEVEL_FRAGMENT_CAPACITY];
  size_t json_color_level_field_lens[8];
  char json_color_level_fields[8][PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  size_t json_color_min_level_field_lens[8];
  char json_color_min_level_fields[8][PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  size_t json_color_message_prefix_len;
  char json_color_message_prefix[PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  size_t json_color_timestamp_prefix_len;
  char json_color_timestamp_prefix[PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  size_t json_color_timestamp_suffix_len;
  char json_color_timestamp_suffix[PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  size_t console_message_prefix_len;
  char console_message_prefix[PSLOG_JSON_COLOR_LITERAL_CAPACITY];
  int closed;
  int close_err;
  unsigned long refcount;
  time_t cached_timestamp_second;
  size_t cached_timestamp_len;
  unsigned long timestamp_cache_hits;
  unsigned long timestamp_cache_misses;
  char cached_timestamp[64];
  void (*time_now)(void *userdata, time_t *seconds_out, long *nanoseconds_out);
  void *time_now_userdata;
  pslog_kvfmt_cache_entry kvfmt_cache[PSLOG_KVFMT_CACHE_SIZE];
  pslog_kvfmt_ptr_cache_entry *kvfmt_ptr_cache;
  pslog_console_prefix_cache_entry
      console_prefix_cache[PSLOG_CONSOLE_PREFIX_CACHE_SIZE];
  pslog_double_cache_entry double_cache[PSLOG_DOUBLE_CACHE_SIZE];
};

typedef struct pslog_logger_impl {
  pslog_shared_state *shared;
  pslog_field *fields;
  size_t field_count;
  char *field_prefix;
  size_t field_prefix_len;
  pslog_level min_level;
  int include_level_field;
  int can_close_shared;
} pslog_logger_impl;

void pslog_buffer_init(pslog_buffer *buffer, pslog_shared_state *shared,
                       size_t capacity);
void pslog_buffer_destroy(pslog_buffer *buffer);
void pslog_buffer_append_char(pslog_buffer *buffer, char ch);
void pslog_buffer_append_cstr(pslog_buffer *buffer, const char *text);
void pslog_buffer_append_n(pslog_buffer *buffer, const char *text, size_t len);
void pslog_buffer_append_json_string(pslog_buffer *buffer, const char *text);
void pslog_buffer_append_json_string_n(pslog_buffer *buffer, const char *text,
                                       size_t len);
void pslog_buffer_append_json_string_maybe_trusted(pslog_buffer *buffer,
                                                   const char *text);
void pslog_buffer_append_json_string_maybe_trusted_n(pslog_buffer *buffer,
                                                     const char *text,
                                                     size_t len);
void pslog_buffer_append_json_trusted_string(pslog_buffer *buffer,
                                             const char *text);
void pslog_buffer_append_json_trusted_string_n(pslog_buffer *buffer,
                                               const char *text, size_t len);
void pslog_buffer_append_console_string(pslog_buffer *buffer, const char *text);
void pslog_buffer_append_console_message(pslog_buffer *buffer,
                                         const char *text);
void pslog_buffer_append_console_string_n(pslog_buffer *buffer,
                                          const char *text, size_t len);
void pslog_buffer_append_console_message_n(pslog_buffer *buffer,
                                           const char *text, size_t len);
void pslog_buffer_append_console_key_n(pslog_buffer *buffer, const char *text,
                                       size_t len);
void pslog_buffer_append_bytes_hex(pslog_buffer *buffer,
                                   const unsigned char *data, size_t len);
void pslog_buffer_append_signed(pslog_buffer *buffer, pslog_int64 value);
void pslog_buffer_append_unsigned(pslog_buffer *buffer, pslog_uint64 value);
void pslog_normalize_decimal_separator(char *text, size_t len);
int pslog_format_double_ascii(char *dst, size_t dst_size, double value);
void pslog_buffer_append_double(pslog_buffer *buffer, double value);
void pslog_buffer_append_pointer(pslog_buffer *buffer, const void *value);
void pslog_buffer_append_time(pslog_buffer *buffer, pslog_time_value value,
                              int quote);
void pslog_buffer_append_duration(pslog_buffer *buffer,
                                  pslog_duration_value value, int quote);
void pslog_buffer_finish_line(pslog_buffer *buffer);
void pslog_buffer_flush(pslog_buffer *buffer);
int pslog_buffer_reserve(pslog_buffer *buffer, size_t min_capacity);
void pslog_buffer_lock_output(pslog_buffer *buffer);
void pslog_buffer_unlock_output(pslog_buffer *buffer);

void *pslog_malloc_internal(size_t size);
void *pslog_calloc_internal(size_t count, size_t size);
void *pslog_realloc_internal(void *ptr, size_t size);
void pslog_free_internal(void *ptr);

char *pslog_strdup_local(const char *text);
void *pslog_memdup_local(const void *src, size_t len);
void pslog_retain_shared(pslog_shared_state *shared);
void pslog_release_shared(pslog_shared_state *shared);
int pslog_close_shared(pslog_shared_state *shared, int allow_close);
int pslog_should_log(pslog_logger_impl *impl, pslog_level level);
void pslog_emit_console(pslog_logger_impl *impl, pslog_level level,
                        pslog_string_view msg, const pslog_field *fields,
                        size_t count);
void pslog_emit_console_kvfmt(pslog_logger_impl *impl, pslog_level level,
                              pslog_string_view msg,
                              const pslog_kvfmt_cache_entry *entry,
                              int saved_errno, va_list ap);
void pslog_append_console_field_prefix(pslog_shared_state *shared,
                                       pslog_buffer *buffer,
                                       const pslog_field *fields, size_t count);
void pslog_emit_json(pslog_logger_impl *impl, pslog_level level,
                     pslog_string_view msg, const pslog_field *fields,
                     size_t count);
void pslog_emit_json_kvfmt(pslog_logger_impl *impl, pslog_level level,
                           pslog_string_view msg,
                           const pslog_kvfmt_cache_entry *entry,
                           int saved_errno, va_list ap);
void pslog_append_json_field_prefix(pslog_shared_state *shared,
                                    pslog_buffer *buffer,
                                    const pslog_field *fields, size_t count);
void pslog_write_buffer(pslog_shared_state *shared, pslog_buffer *buffer);
void pslog_append_timestamp(pslog_shared_state *shared, pslog_buffer *buffer);
void pslog_append_level_label(pslog_shared_state *shared, pslog_buffer *buffer,
                              pslog_level level);
void pslog_apply_color(pslog_buffer *buffer, const char *color_code,
                       int enabled);
void pslog_apply_color_len(pslog_buffer *buffer, const char *color_code,
                           size_t color_len, int enabled);
const char *pslog_level_color(const pslog_palette *palette, pslog_level level);
size_t pslog_level_color_len(const pslog_shared_state *shared,
                             pslog_level level);
const char *pslog_level_console_label(pslog_level level);
size_t pslog_level_string_len(pslog_level level);
int pslog_parse_kvfmt(pslog_shared_state *shared, pslog_field *fields,
                      size_t *out_count, pslog_mode mode, const char *kvfmt,
                      int saved_errno, va_list ap);
const char *pslog_errno_string(int err, char *buffer, size_t buffer_size);
int pslog_string_is_console_simple_internal(const char *text);
pslog_logger *pslog_clone_with_fields(pslog_logger *log,
                                      const pslog_field *fields, size_t count);
void pslog_log_impl(pslog_logger *log, pslog_level level, const char *msg,
                    const pslog_field *fields, size_t count);
void pslog_log_view_impl(pslog_logger *log, pslog_level level,
                         pslog_string_view msg, const pslog_field *fields,
                         size_t count);
void pslog_vlogf_impl(pslog_logger *log, pslog_level level, const char *msg,
                      const char *kvfmt, va_list ap);
void pslog_vlogf_view_impl(pslog_logger *log, pslog_level level,
                           pslog_string_view msg, const char *kvfmt,
                           va_list ap);

#if defined(PSLOG_TEST_HOOKS)
void pslog_test_allocator_reset(void);
void pslog_test_allocator_get(pslog_test_allocator_stats *stats);
void pslog_test_allocator_fail_after(unsigned long calls);
int pslog_test_console_prefix_cacheable(pslog_shared_state *shared,
                                        const pslog_field *field);
#endif

#endif
