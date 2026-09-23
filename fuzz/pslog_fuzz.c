#include "pslog.h"

#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PSLOG_FUZZ_MAX_FIELDS 10u

struct fuzz_sink {
  char data[16384];
  size_t len;
};

static char fuzz_key_char(unsigned char ch) {
  static const char alphabet[] = "abcdefghijklmnopqrstuvwxyz0123456789_";

  return alphabet[ch % (sizeof(alphabet) - 1u)];
}

static int fuzz_sink_write(void *userdata, const char *data, size_t len,
                           size_t *written) {
  struct fuzz_sink *sink;
  size_t copy_len;

  sink = (struct fuzz_sink *)userdata;
  copy_len = len;
  if (copy_len > sizeof(sink->data) - sink->len - 1u) {
    copy_len = sizeof(sink->data) - sink->len - 1u;
  }
  memcpy(sink->data + sink->len, data, copy_len);
  sink->len += copy_len;
  sink->data[sink->len] = '\0';
  if (written != NULL) {
    *written = copy_len;
  }
  return 0;
}

static void strip_ansi(char *dst, size_t dst_size, const char *src) {
  size_t out;

  out = 0u;
  while (*src != '\0' && out + 1u < dst_size) {
    if ((unsigned char)*src == 0x1bu && src[1] == '[') {
      src += 2;
      while (*src != '\0' && *src != 'm') {
        ++src;
      }
      if (*src == 'm') {
        ++src;
      }
      continue;
    }
    dst[out] = *src;
    ++out;
    ++src;
  }
  dst[out] = '\0';
}

static unsigned char fuzz_byte(const unsigned char *data, size_t size,
                               size_t index) {
  if (size == 0u) {
    return 0u;
  }
  return data[index % size];
}

static long fuzz_long_value(const unsigned char *data, size_t size,
                            size_t offset) {
  unsigned long value;
  size_t i;

  value = 0u;
  for (i = 0u; i < sizeof(value); ++i) {
    value = (value << 8) | (unsigned long)fuzz_byte(data, size, offset + i);
  }
  return (long)value;
}

static void fuzz_make_key(char *key, size_t key_size, const unsigned char *data,
                          size_t size, size_t offset) {
  size_t key_len;
  size_t i;

  if (key_size == 0u) {
    return;
  }
  key_len = 1u + (fuzz_byte(data, size, offset) % (key_size - 1u));
  for (i = 0u; i < key_len; ++i) {
    key[i] = fuzz_key_char(fuzz_byte(data, size, offset + 1u + i));
  }
  key[key_len] = '\0';
}

static double fuzz_double_value(const unsigned char *data, size_t size,
                                size_t offset) {
  switch (fuzz_byte(data, size, offset) % 8u) {
  case 0:
    return 0.0;
  case 1:
    return -0.0;
  case 2:
    return 1.0 / 0.0;
  case 3:
    return -1.0 / 0.0;
  case 4:
    return 0.0 / 0.0;
  default:
    return (double)fuzz_long_value(data, size, offset + 1u) / 17.0;
  }
}

static size_t fuzz_build_fields(pslog_field *fields,
                                char keys[PSLOG_FUZZ_MAX_FIELDS][16],
                                char values[PSLOG_FUZZ_MAX_FIELDS][64],
                                const unsigned char *data, size_t size) {
  size_t field_count;
  size_t i;

  field_count = 1u + (fuzz_byte(data, size, 1u) % PSLOG_FUZZ_MAX_FIELDS);
  for (i = 0u; i < field_count; ++i) {
    size_t value_len;
    long seconds;
    long nanoseconds;

    fuzz_make_key(keys[i], sizeof(keys[i]), data, size, 2u + (i * 11u));
    value_len =
        fuzz_byte(data, size, 3u + (i * 13u)) % (sizeof(values[i]) - 1u);
    if (value_len > size) {
      value_len = size;
    }
    memcpy(values[i], data, value_len);
    values[i][value_len] = '\0';

    seconds = fuzz_long_value(data, size, 5u + (i * 17u));
    nanoseconds = fuzz_long_value(data, size, 7u + (i * 19u)) % 2000000000l;
    switch (fuzz_byte(data, size, 4u + (i * 23u)) % PSLOG_FUZZ_MAX_FIELDS) {
    case 0:
      fields[i] = pslog_null(keys[i]);
      break;
    case 1:
      fields[i] = pslog_str(keys[i], values[i]);
      break;
    case 2:
      fields[i] = pslog_trusted_str(keys[i], values[i]);
      break;
    case 3:
      fields[i] = pslog_i64(keys[i], (pslog_int64)seconds);
      break;
    case 4:
      fields[i] = pslog_u64(keys[i], (pslog_uint64)(unsigned long)seconds);
      break;
    case 5:
      fields[i] = pslog_f64(keys[i], fuzz_double_value(data, size, i));
      break;
    case 6:
      fields[i] = pslog_bool(keys[i], (int)(fuzz_byte(data, size, i) & 1u));
      break;
    case 7:
      fields[i] = pslog_ptr(keys[i], data + (size == 0u ? 0u : i % size));
      break;
    case 8:
      fields[i] = pslog_bytes_field(keys[i], data, value_len);
      break;
    default:
      if (fuzz_byte(data, size, i) & 1u) {
        fields[i] = pslog_time_field(keys[i], seconds, nanoseconds,
                                     (int)(seconds % 2880l) - 1440);
      } else {
        fields[i] = pslog_duration_field(keys[i], seconds, nanoseconds);
      }
      break;
    }
  }
  if (field_count > 0u) {
    fields[field_count - 1u] =
        pslog_errno(keys[field_count - 1u], (int)(fuzz_byte(data, size, 9u)));
  }
  return field_count;
}

static size_t fuzz_append_kvfmt_key(char *dst, size_t dst_size, size_t offset,
                                    const unsigned char *data, size_t size,
                                    size_t input_offset) {
  size_t key_len;
  size_t i;

  if (offset >= dst_size) {
    return offset;
  }
  key_len = 1u + (fuzz_byte(data, size, input_offset) % 24u);
  for (i = 0u; i < key_len && offset + 1u < dst_size; ++i) {
    dst[offset++] = fuzz_key_char(fuzz_byte(data, size, input_offset + i + 1u));
  }
  dst[offset] = '\0';
  return offset;
}

static size_t fuzz_append_kvfmt_spaces(char *dst, size_t dst_size,
                                       size_t offset, const unsigned char *data,
                                       size_t size, size_t input_offset) {
  size_t space_count;
  size_t i;

  space_count = fuzz_byte(data, size, input_offset) % 4u;
  for (i = 0u; i < space_count && offset + 1u < dst_size; ++i) {
    dst[offset++] = ' ';
  }
  dst[offset] = '\0';
  return offset;
}

static size_t fuzz_append_literal(char *dst, size_t dst_size, size_t offset,
                                  const char *literal) {
  while (*literal != '\0' && offset + 1u < dst_size) {
    dst[offset++] = *literal++;
  }
  dst[offset] = '\0';
  return offset;
}

static void fuzz_make_dynamic_kvfmt(char *dst, size_t dst_size,
                                    const unsigned char *data, size_t size,
                                    size_t offset_seed, const char *verb0,
                                    const char *verb1, const char *verb2) {
  size_t offset;

  if (dst_size == 0u) {
    return;
  }
  dst[0] = '\0';
  offset = fuzz_append_kvfmt_spaces(dst, dst_size, 0u, data, size, offset_seed);
  offset = fuzz_append_kvfmt_key(dst, dst_size, offset, data, size,
                                 offset_seed + 1u);
  offset = fuzz_append_literal(dst, dst_size, offset, "=");
  offset = fuzz_append_literal(dst, dst_size, offset, verb0);
  offset = fuzz_append_kvfmt_spaces(dst, dst_size, offset, data, size,
                                    offset_seed + 3u);
  offset = fuzz_append_kvfmt_key(dst, dst_size, offset, data, size,
                                 offset_seed + 5u);
  offset = fuzz_append_literal(dst, dst_size, offset, "=");
  offset = fuzz_append_literal(dst, dst_size, offset, verb1);
  if (verb2 != NULL) {
    offset = fuzz_append_kvfmt_spaces(dst, dst_size, offset, data, size,
                                      offset_seed + 7u);
    offset = fuzz_append_kvfmt_key(dst, dst_size, offset, data, size,
                                   offset_seed + 9u);
    offset = fuzz_append_literal(dst, dst_size, offset, "=");
    (void)fuzz_append_literal(dst, dst_size, offset, verb2);
  }
}

static void fuzz_make_invalid_kvfmt(char *dst, size_t dst_size,
                                    const unsigned char *data, size_t size) {
  size_t offset;
  size_t i;

  if (dst_size == 0u) {
    return;
  }
  dst[0] = '\0';
  offset = fuzz_append_kvfmt_spaces(dst, dst_size, 0u, data, size, 31u);
  offset = fuzz_append_kvfmt_key(dst, dst_size, offset, data, size, 33u);
  if (offset + 1u < dst_size) {
    static const char invalid_chars[] = "\"\\\177 \t";

    dst[offset++] = invalid_chars[fuzz_byte(data, size, 35u) %
                                  (sizeof(invalid_chars) - 1u)];
  }
  for (i = 0u; i < 16u && offset + 1u < dst_size; ++i) {
    dst[offset++] = (char)fuzz_byte(data, size, 36u + i);
  }
  dst[offset] = '\0';
}

static void fuzz_emit_kvfmt(pslog_logger *log, const char *msg,
                            const unsigned char *data, size_t size) {
  char dynamic_kvfmt[256];
  char long_key[96];
  char long_kvfmt[100];
  char many_kvfmt[256];
  unsigned int choice;
  size_t i;
  size_t offset;

  choice = (unsigned int)(fuzz_byte(data, size, 11u) % 18u);
  switch (choice) {
  case 0:
    fuzz_make_dynamic_kvfmt(dynamic_kvfmt, sizeof(dynamic_kvfmt), data, size,
                            40u, "%s", "%d", "%u");
    log->infof(log, msg, dynamic_kvfmt, "value",
               (int)fuzz_long_value(data, size, 12u),
               (unsigned int)fuzz_long_value(data, size, 13u));
    break;
  case 1:
    fuzz_make_dynamic_kvfmt(dynamic_kvfmt, sizeof(dynamic_kvfmt), data, size,
                            60u, "%ld", "%lu", "%f");
    log->infof(log, msg, dynamic_kvfmt, fuzz_long_value(data, size, 14u),
               (unsigned long)fuzz_long_value(data, size, 15u),
               fuzz_double_value(data, size, 16u));
    break;
  case 2:
    fuzz_make_dynamic_kvfmt(dynamic_kvfmt, sizeof(dynamic_kvfmt), data, size,
                            80u, "%p", "%b", NULL);
    log->infof(log, msg, dynamic_kvfmt, (const void *)data,
               (int)(fuzz_byte(data, size, 17u) & 1u));
    break;
  case 3:
    fuzz_make_dynamic_kvfmt(dynamic_kvfmt, sizeof(dynamic_kvfmt), data, size,
                            100u, "%m", "%s", NULL);
    errno = (int)(fuzz_byte(data, size, 18u) % 128u);
    log->infof(log, msg, dynamic_kvfmt, "errno-tail");
    break;
  case 4:
    fuzz_make_invalid_kvfmt(dynamic_kvfmt, sizeof(dynamic_kvfmt), data, size);
    log->infof(log, msg, dynamic_kvfmt);
    break;
  case 5:
    log->infof(log, msg, "bad=%q");
    break;
  case 6:
    log->infof(log, msg, "missing=%");
    break;
  case 7:
    log->infof(log, msg, "space =%d", 1);
    break;
  case 8:
    log->infof(log, msg, "k0=%d k1=%d k2=%d k3=%d k4=%d k5=%d k6=%d k7=%d", 0,
               1, 2, 3, 4, 5, 6, 7);
    break;
  case 9:
    offset = 0u;
    for (i = 0u; i < 33u && offset + 6u < sizeof(many_kvfmt); ++i) {
      offset +=
          (size_t)sprintf(many_kvfmt + offset, "m%lu=%%m ", (unsigned long)i);
    }
    many_kvfmt[offset == 0u ? 0u : offset - 1u] = '\0';
    errno = (int)(fuzz_byte(data, size, 22u) % 128u);
    log->infof(log, msg, many_kvfmt);
    break;
  case 10:
    memset(long_key, 'a', sizeof(long_key) - 1u);
    long_key[sizeof(long_key) - 1u] = '\0';
    strcpy(long_kvfmt, long_key);
    strcat(long_kvfmt, "=%s");
    log->infof(log, msg, long_kvfmt, "long");
    break;
  case 11:
    log->infof(log, msg, NULL);
    break;
  case 12: {
    pslog_logger *child;

    child = log->withf(log, "child=%s n=%ld", "static",
                       fuzz_long_value(data, size, 19u));
    if (child != NULL) {
      child->info(child, msg, NULL, 0u);
      child->destroy(child);
    }
  } break;
  default:
    pslog(log, PSLOG_LEVEL_INFO, msg, "free=%u bool=%b",
          (unsigned int)fuzz_long_value(data, size, 20u),
          (int)(fuzz_byte(data, size, 21u) & 1u));
    break;
  }
}

int pslog_fuzz_one_input(const unsigned char *data, size_t size) {
  struct fuzz_sink plain_sink;
  struct fuzz_sink color_sink;
  pslog_config config;
  pslog_logger *plain_log;
  pslog_logger *color_log;
  pslog_field fields[PSLOG_FUZZ_MAX_FIELDS];
  pslog_field view_field;
  pslog_string_view view_key;
  pslog_string_view view_value;
  pslog_string_view view_message;
  char msg[64];
  char keys[PSLOG_FUZZ_MAX_FIELDS][16];
  char values[PSLOG_FUZZ_MAX_FIELDS][64];
  char stripped[16384];
  size_t msg_len;
  size_t field_count;

  if (size == 0u) {
    return 0;
  }

  memset(&plain_sink, 0, sizeof(plain_sink));
  memset(&color_sink, 0, sizeof(color_sink));
  pslog_default_config(&config);
  config.mode = (data[0] & 1u) ? PSLOG_MODE_JSON : PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.non_finite_float_policy = (data[0] & 2u)
                                       ? PSLOG_NON_FINITE_FLOAT_AS_NULL
                                       : PSLOG_NON_FINITE_FLOAT_AS_STRING;
  config.output.write = fuzz_sink_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = &plain_sink;
  config.color = PSLOG_COLOR_NEVER;

  plain_log = pslog_new(&config);
  config.output.userdata = &color_sink;
  config.color = PSLOG_COLOR_ALWAYS;
  color_log = pslog_new(&config);
  if (plain_log == NULL || color_log == NULL) {
    if (plain_log != NULL) {
      plain_log->destroy(plain_log);
    }
    if (color_log != NULL) {
      color_log->destroy(color_log);
    }
    return 0;
  }

  msg_len = size > sizeof(msg) - 1u ? sizeof(msg) - 1u : size;
  memcpy(msg, data, msg_len);
  msg[msg_len] = '\0';

  field_count = fuzz_build_fields(fields, keys, values, data, size);
  view_key.data = keys[0];
  view_key.len = strlen(keys[0]) + 1u;
  view_value.data = (const char *)data;
  view_value.len = size < 64u ? size : 64u;
  view_message = view_value;
  view_field = pslog_str_view(view_key, view_value);

  plain_log->info(plain_log, msg, fields, field_count);
  plain_log->info_view(plain_log, view_message, &view_field, 1u);
  fuzz_emit_kvfmt(plain_log, msg, data, size);
  color_log->info(color_log, msg, fields, field_count);
  color_log->info_view(color_log, view_message, &view_field, 1u);
  fuzz_emit_kvfmt(color_log, msg, data, size);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  if (strcmp(plain_sink.data, stripped) != 0) {
    abort();
  }
  plain_log->destroy(plain_log);
  color_log->destroy(color_log);
  return 0;
}

int main(void) {
  unsigned char data[16384];
  size_t size;

  size = fread(data, 1u, sizeof(data), stdin);
  return pslog_fuzz_one_input(data, size);
}
