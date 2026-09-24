#include "pslog_internal.h"

#include <float.h>

static void pslog_append_json_value_plain(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field);
static void pslog_append_json_value_color(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field);
static void pslog_append_json_message_field_plain(pslog_shared_state *shared,
                                                  pslog_buffer *buffer,
                                                  const char *key,
                                                  pslog_string_view msg,
                                                  int first_field);
static void pslog_append_json_message_field_color(pslog_shared_state *shared,
                                                  pslog_buffer *buffer,
                                                  const char *key,
                                                  pslog_string_view msg,
                                                  int first_field);
static void pslog_append_json_field_plain(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field,
                                          int first_field);
static void pslog_append_json_field_color(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field,
                                          int first_field);
static void pslog_append_json_field_set_plain(pslog_logger_impl *impl,
                                              pslog_buffer *buffer,
                                              const pslog_field *fields,
                                              size_t count, int *first_field);
static void pslog_append_json_field_set_color(pslog_logger_impl *impl,
                                              pslog_buffer *buffer,
                                              const pslog_field *fields,
                                              size_t count, int *first_field);
static void pslog_append_json_kvfmt_fields_plain(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int *first_field, int saved_errno,
    va_list ap);
static void
pslog_append_json_kvfmt_color_prefix(pslog_shared_state *shared,
                                     pslog_buffer *buffer,
                                     const pslog_kvfmt_field_spec *spec);
static void pslog_append_json_kvfmt_fields_color(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int *first_field, int saved_errno,
    va_list ap);
static void pslog_emit_json_plain(pslog_logger_impl *impl, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count);
static void pslog_emit_json_color(pslog_logger_impl *impl, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count);
static size_t pslog_json_level_slot(pslog_level level);
static const char *pslog_json_level_field_literal(pslog_level level,
                                                  int verbose_fields,
                                                  size_t *len_out);
static const char *pslog_json_message_prefix(int verbose_fields,
                                             size_t *len_out);
static void pslog_append_json_color_literal(pslog_buffer *buffer,
                                            const char *color, size_t color_len,
                                            const char *text, size_t text_len,
                                            const char *suffix,
                                            size_t suffix_len);
static void pslog_append_json_timestamp_field_color(pslog_shared_state *shared,
                                                    pslog_buffer *buffer,
                                                    int first_field);
static void pslog_append_json_level_field_color_cached_or_dynamic(
    pslog_shared_state *shared, pslog_buffer *buffer, pslog_level level,
    int min_level_field);

static size_t pslog_json_level_slot(pslog_level level) {
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return 0u;
  case PSLOG_LEVEL_DEBUG:
    return 1u;
  case PSLOG_LEVEL_WARN:
    return 3u;
  case PSLOG_LEVEL_ERROR:
    return 4u;
  case PSLOG_LEVEL_FATAL:
    return 5u;
  case PSLOG_LEVEL_PANIC:
    return 6u;
  case PSLOG_LEVEL_NOLEVEL:
    return 7u;
  default:
    return 2u;
  }
}

static void pslog_append_json_value_plain(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field) {
  switch (field->type) {
  case PSLOG_FIELD_NULL:
    PSLOG_APPEND_LITERAL(buffer, "null");
    break;
  case PSLOG_FIELD_STRING:
    if (field->trusted_value) {
      PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(
          buffer, field->as.string_value, field->value_len);
    } else {
      pslog_buffer_append_json_string_n(buffer, field->as.string_value,
                                        field->value_len);
    }
    break;
  case PSLOG_FIELD_SIGNED:
    pslog_buffer_append_signed(buffer, field->as.signed_value);
    break;
  case PSLOG_FIELD_UNSIGNED:
    pslog_buffer_append_unsigned(buffer, field->as.unsigned_value);
    break;
  case PSLOG_FIELD_DOUBLE:
    if (field->as.double_value != field->as.double_value) {
      if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
        PSLOG_APPEND_LITERAL(buffer, "null");
      } else {
        pslog_buffer_append_json_string(buffer, "NaN");
      }
    } else if (field->as.double_value > DBL_MAX ||
               field->as.double_value < -DBL_MAX) {
      if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
        PSLOG_APPEND_LITERAL(buffer, "null");
      } else {
        pslog_buffer_append_json_string(
            buffer, field->as.double_value < 0.0 ? "-Inf" : "+Inf");
      }
    } else {
      pslog_buffer_append_double(buffer, field->as.double_value);
    }
    break;
  case PSLOG_FIELD_BOOL:
    if (field->as.bool_value) {
      PSLOG_APPEND_LITERAL(buffer, "true");
    } else {
      PSLOG_APPEND_LITERAL(buffer, "false");
    }
    break;
  case PSLOG_FIELD_POINTER:
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    pslog_buffer_append_pointer(buffer, field->as.pointer_value);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    break;
  case PSLOG_FIELD_BYTES:
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    pslog_buffer_append_bytes_hex(buffer, field->as.bytes_value.data,
                                  field->as.bytes_value.len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    break;
  case PSLOG_FIELD_TIME:
    pslog_buffer_append_time(buffer, field->as.time_value, 1);
    break;
  case PSLOG_FIELD_DURATION:
    pslog_buffer_append_duration(buffer, field->as.duration_value, 1);
    break;
  case PSLOG_FIELD_ERRNO: {
    char errno_buffer[128];

    pslog_buffer_append_json_string_maybe_trusted(
        buffer, pslog_errno_string((int)field->as.signed_value, errno_buffer,
                                   sizeof(errno_buffer)));
    break;
  }
  }
}

static void pslog_append_json_value_color(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field) {
  const pslog_palette *palette;

  palette = shared->palette;
  switch (field->type) {
  case PSLOG_FIELD_NULL:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->null_value,
                               shared->palette_null_value_len);
    PSLOG_APPEND_LITERAL(buffer, "null");
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_STRING:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                               shared->palette_string_len);
    if (field->trusted_value) {
      PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(
          buffer, field->as.string_value, field->value_len);
    } else {
      pslog_buffer_append_json_string_n(buffer, field->as.string_value,
                                        field->value_len);
    }
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_SIGNED:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                               shared->palette_number_len);
    pslog_buffer_append_signed(buffer, field->as.signed_value);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_UNSIGNED:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                               shared->palette_number_len);
    pslog_buffer_append_unsigned(buffer, field->as.unsigned_value);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_DOUBLE:
    if (field->as.double_value != field->as.double_value) {
      if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->null_value,
                                   shared->palette_null_value_len);
        PSLOG_APPEND_LITERAL(buffer, "null");
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                   shared->palette_reset_len);
      } else {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                   shared->palette_string_len);
        pslog_buffer_append_json_string_maybe_trusted(buffer, "NaN");
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                   shared->palette_reset_len);
      }
    } else if (field->as.double_value > DBL_MAX ||
               field->as.double_value < -DBL_MAX) {
      if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->null_value,
                                   shared->palette_null_value_len);
        PSLOG_APPEND_LITERAL(buffer, "null");
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                   shared->palette_reset_len);
      } else {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                   shared->palette_string_len);
        pslog_buffer_append_json_string_maybe_trusted(
            buffer, field->as.double_value < 0.0 ? "-Inf" : "+Inf");
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                   shared->palette_reset_len);
      }
    } else {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                 shared->palette_number_len);
      pslog_buffer_append_double(buffer, field->as.double_value);
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
    }
    break;
  case PSLOG_FIELD_BOOL:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->boolean,
                               shared->palette_boolean_len);
    if (field->as.bool_value) {
      PSLOG_APPEND_LITERAL(buffer, "true");
    } else {
      PSLOG_APPEND_LITERAL(buffer, "false");
    }
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_POINTER:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                               shared->palette_string_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    pslog_buffer_append_pointer(buffer, field->as.pointer_value);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_BYTES:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                               shared->palette_string_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    pslog_buffer_append_bytes_hex(buffer, field->as.bytes_value.data,
                                  field->as.bytes_value.len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_TIME:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->timestamp,
                               shared->palette_timestamp_len);
    pslog_buffer_append_time(buffer, field->as.time_value, 1);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_DURATION:
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                               shared->palette_string_len);
    pslog_buffer_append_duration(buffer, field->as.duration_value, 1);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  case PSLOG_FIELD_ERRNO: {
    char errno_buffer[128];

    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->error,
                               shared->level_color_lens[4]);
    pslog_buffer_append_json_string_maybe_trusted(
        buffer, pslog_errno_string((int)field->as.signed_value, errno_buffer,
                                   sizeof(errno_buffer)));
    PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                               shared->palette_reset_len);
    break;
  }
  }
}

static void pslog_append_json_message_field_plain(pslog_shared_state *shared,
                                                  pslog_buffer *buffer,
                                                  const char *key,
                                                  pslog_string_view msg,
                                                  int first_field) {
  size_t prefix_len;
  const char *prefix;

  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
  }
  prefix = pslog_json_message_prefix(shared->verbose_fields, &prefix_len);
  (void)key;
  PSLOG_BUFFER_APPEND_N_FAST(buffer, prefix, prefix_len);
  pslog_buffer_append_json_string_maybe_trusted_n(buffer, msg.data, msg.len);
}

static void pslog_append_json_message_field_color(pslog_shared_state *shared,
                                                  pslog_buffer *buffer,
                                                  const char *key,
                                                  pslog_string_view msg,
                                                  int first_field) {
  size_t prefix_len;
  const char *prefix;

  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
  }
  if (shared->json_color_message_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->json_color_message_prefix,
                               shared->json_color_message_prefix_len);
  } else {
    prefix = pslog_json_message_prefix(shared->verbose_fields, &prefix_len);
    (void)key;
    pslog_append_json_color_literal(buffer, shared->palette->message_key,
                                    shared->palette_message_key_len, prefix,
                                    prefix_len, NULL, 0u);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->message,
                               shared->palette_message_len);
  }
  pslog_buffer_append_json_string_maybe_trusted_n(buffer, msg.data, msg.len);
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                             shared->palette_reset_len);
}

static void pslog_append_json_field_plain(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field,
                                          int first_field) {
  if (field == NULL || field->key == NULL || field->key_len == 0u) {
    return;
  }
  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
  }
  if (field->trusted_key) {
    PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(buffer, field->key,
                                                   field->key_len);
  } else {
    pslog_buffer_append_json_string_n(buffer, field->key, field->key_len);
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ':');
  pslog_append_json_value_plain(shared, buffer, field);
}

static void pslog_append_json_field_color(pslog_shared_state *shared,
                                          pslog_buffer *buffer,
                                          const pslog_field *field,
                                          int first_field) {
  if (field == NULL || field->key == NULL || field->key_len == 0u) {
    return;
  }
  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
  }
  if (field->trusted_key) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->key,
                               shared->palette_key_len);
    PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(buffer, field->key,
                                                   field->key_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ':');
  } else {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->key,
                               shared->palette_key_len);
    pslog_buffer_append_json_string_n(buffer, field->key, field->key_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ':');
  }
  pslog_append_json_value_color(shared, buffer, field);
}

static void pslog_append_json_field_set_plain(pslog_logger_impl *impl,
                                              pslog_buffer *buffer,
                                              const pslog_field *fields,
                                              size_t count, int *first_field) {
  size_t i;

  for (i = 0u; i < count; ++i) {
    if (fields[i].key != NULL && fields[i].key_len > 0u) {
      pslog_append_json_field_plain(impl->shared, buffer, fields + i,
                                    *first_field);
      *first_field = 0;
    }
  }
}

static void pslog_append_json_field_set_color(pslog_logger_impl *impl,
                                              pslog_buffer *buffer,
                                              const pslog_field *fields,
                                              size_t count, int *first_field) {
  size_t i;

  for (i = 0u; i < count; ++i) {
    if (fields[i].key != NULL && fields[i].key_len > 0u) {
      pslog_append_json_field_color(impl->shared, buffer, fields + i,
                                    *first_field);
      *first_field = 0;
    }
  }
}

static void pslog_emit_json_plain(pslog_logger_impl *impl, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count) {
  pslog_buffer buffer;
  pslog_field field;
  pslog_field level_field;
  int first_field;
  const char *level_literal;
  size_t level_literal_len;

  pslog_buffer_init(&buffer, impl->shared, impl->shared->line_buffer_capacity);
  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '{');
  first_field = 1;

  if (impl->shared->timestamps) {
    field.key = impl->shared->verbose_fields ? "time" : "ts";
    field.key_len = impl->shared->verbose_fields ? 4u : 2u;
    field.type = PSLOG_FIELD_STRING;
    field.trusted_key = 1u;
    field.trusted_value = 0u;
    field.console_simple_value = 0u;
    field.value_len = 0u;
    field.as.string_value = "";
    if (!first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
    }
    PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(&buffer, field.key,
                                                   field.key_len);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ':');
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    pslog_append_timestamp(impl->shared, &buffer);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    first_field = 0;
  }

  level_literal = pslog_json_level_field_literal(
      level, impl->shared->verbose_fields, &level_literal_len);
  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
  }
  PSLOG_BUFFER_APPEND_N_FAST(&buffer, level_literal, level_literal_len);
  first_field = 0;

  if (msg.data != NULL && msg.len > 0u) {
    pslog_append_json_message_field_plain(
        impl->shared, &buffer, impl->shared->verbose_fields ? "message" : "msg",
        msg, first_field);
    first_field = 0;
  }

  if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                               impl->field_prefix_len);
    first_field = 0;
  } else {
    pslog_append_json_field_set_plain(impl, &buffer, impl->fields,
                                      impl->field_count, &first_field);
  }
  pslog_append_json_field_set_plain(impl, &buffer, fields, count, &first_field);
  if (impl->include_level_field) {
    level_field.key = "loglevel";
    level_field.key_len = 8u;
    level_field.type = PSLOG_FIELD_STRING;
    level_field.trusted_key = 1u;
    level_field.trusted_value = 1u;
    level_field.console_simple_value = 1u;
    level_field.value_len = pslog_level_string_len(impl->min_level);
    level_field.as.string_value = pslog_level_string(impl->min_level);
    pslog_append_json_field_plain(impl->shared, &buffer, &level_field,
                                  first_field);
  }

  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '}');
  pslog_buffer_finish_line(&buffer);
  pslog_write_buffer(impl->shared, &buffer);
  pslog_buffer_destroy(&buffer);
}

static const char *pslog_json_level_field_literal(pslog_level level,
                                                  int verbose_fields,
                                                  size_t *len_out) {
  static const char *short_literals[] = {
      "\"lvl\":\"trace\"", "\"lvl\":\"debug\"",  "\"lvl\":\"info\"",
      "\"lvl\":\"warn\"",  "\"lvl\":\"error\"",  "\"lvl\":\"fatal\"",
      "\"lvl\":\"panic\"", "\"lvl\":\"nolevel\""};
  static const size_t short_lengths[] = {13u, 13u, 12u, 12u,
                                         13u, 13u, 13u, 15u};
  static const char *verbose_literals[] = {
      "\"level\":\"trace\"", "\"level\":\"debug\"",  "\"level\":\"info\"",
      "\"level\":\"warn\"",  "\"level\":\"error\"",  "\"level\":\"fatal\"",
      "\"level\":\"panic\"", "\"level\":\"nolevel\""};
  static const size_t verbose_lengths[] = {15u, 15u, 14u, 14u,
                                           15u, 15u, 15u, 17u};
  size_t index;

  switch (level) {
  case PSLOG_LEVEL_TRACE:
    index = 0u;
    break;
  case PSLOG_LEVEL_DEBUG:
    index = 1u;
    break;
  case PSLOG_LEVEL_WARN:
    index = 3u;
    break;
  case PSLOG_LEVEL_ERROR:
    index = 4u;
    break;
  case PSLOG_LEVEL_FATAL:
    index = 5u;
    break;
  case PSLOG_LEVEL_PANIC:
    index = 6u;
    break;
  case PSLOG_LEVEL_NOLEVEL:
    index = 7u;
    break;
  default:
    index = 2u;
    break;
  }
  if (verbose_fields) {
    if (len_out != NULL) {
      *len_out = verbose_lengths[index];
    }
    return verbose_literals[index];
  }
  if (len_out != NULL) {
    *len_out = short_lengths[index];
  }
  return short_literals[index];
}

static const char *pslog_json_message_prefix(int verbose_fields,
                                             size_t *len_out) {
  if (verbose_fields) {
    if (len_out != NULL) {
      *len_out = 10u;
    }
    return "\"message\":";
  }
  if (len_out != NULL) {
    *len_out = 6u;
  }
  return "\"msg\":";
}

static void pslog_append_json_color_literal(pslog_buffer *buffer,
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

static void pslog_append_json_timestamp_field_color(pslog_shared_state *shared,
                                                    pslog_buffer *buffer,
                                                    int first_field) {
  static const char short_time_key[] = "\"ts\":";
  static const char verbose_time_key[] = "\"time\":";
  const char *time_key;
  size_t time_key_len;

  if (shared == NULL || buffer == NULL) {
    return;
  }
  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
  }
  if (shared->json_color_timestamp_prefix_len > 0u &&
      shared->json_color_timestamp_suffix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->json_color_timestamp_prefix,
                               shared->json_color_timestamp_prefix_len);
    pslog_append_timestamp(shared, buffer);
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->json_color_timestamp_suffix,
                               shared->json_color_timestamp_suffix_len);
    return;
  }
  time_key = shared->verbose_fields ? verbose_time_key : short_time_key;
  time_key_len = shared->verbose_fields ? sizeof(verbose_time_key) - 1u
                                        : sizeof(short_time_key) - 1u;
  pslog_append_json_color_literal(buffer, shared->palette->key,
                                  shared->palette_key_len, time_key,
                                  time_key_len, NULL, 0u);
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                             shared->palette_reset_len);
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->timestamp,
                             shared->palette_timestamp_len);
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  pslog_append_timestamp(shared, buffer);
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                             shared->palette_reset_len);
}

static void pslog_append_json_level_field_color_cached_or_dynamic(
    pslog_shared_state *shared, pslog_buffer *buffer, pslog_level level,
    int min_level_field) {
  static const char short_level_key[] = "\"lvl\":";
  static const char verbose_level_key[] = "\"level\":";
  static const char loglevel_key[] = "\"loglevel\":";
  size_t slot;
  const char *key;
  size_t key_len;
  const char *level_color;
  size_t level_color_len;

  if (shared == NULL || buffer == NULL) {
    return;
  }
  slot = pslog_json_level_slot(level);
  if (min_level_field) {
    if (shared->json_color_min_level_field_lens[slot] > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer,
                                 shared->json_color_min_level_fields[slot],
                                 shared->json_color_min_level_field_lens[slot]);
      return;
    }
    key = loglevel_key;
    key_len = sizeof(loglevel_key) - 1u;
  } else {
    if (shared->json_color_level_field_lens[slot] > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->json_color_level_fields[slot],
                                 shared->json_color_level_field_lens[slot]);
      return;
    }
    key = shared->verbose_fields ? verbose_level_key : short_level_key;
    key_len = shared->verbose_fields ? sizeof(verbose_level_key) - 1u
                                     : sizeof(short_level_key) - 1u;
  }
  level_color = pslog_level_color(shared->palette, level);
  level_color_len = pslog_level_color_len(shared, level);
  pslog_append_json_color_literal(buffer, shared->palette->key,
                                  shared->palette_key_len, key, key_len, NULL,
                                  0u);
  pslog_append_json_color_literal(buffer, level_color, level_color_len, "\"",
                                  1u, NULL, 0u);
  PSLOG_BUFFER_APPEND_N_FAST(buffer, pslog_level_string(level),
                             pslog_level_string_len(level));
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                             shared->palette_reset_len);
}

static void pslog_emit_json_color(pslog_logger_impl *impl, pslog_level level,
                                  pslog_string_view msg,
                                  const pslog_field *fields, size_t count) {
  pslog_buffer buffer;
  int first_field;

  pslog_buffer_init(&buffer, impl->shared, impl->shared->line_buffer_capacity);
  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '{');
  first_field = 1;

  if (impl->shared->timestamps) {
    pslog_append_json_timestamp_field_color(impl->shared, &buffer, first_field);
    first_field = 0;
  }

  if (!first_field) {
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
  }
  pslog_append_json_level_field_color_cached_or_dynamic(impl->shared, &buffer,
                                                        level, 0);
  first_field = 0;

  if (msg.data != NULL && msg.len > 0u) {
    pslog_append_json_message_field_color(
        impl->shared, &buffer, impl->shared->verbose_fields ? "message" : "msg",
        msg, first_field);
    first_field = 0;
  }

  if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                               impl->field_prefix_len);
    first_field = 0;
  } else {
    pslog_append_json_field_set_color(impl, &buffer, impl->fields,
                                      impl->field_count, &first_field);
  }
  pslog_append_json_field_set_color(impl, &buffer, fields, count, &first_field);
  if (impl->include_level_field) {
    if (!first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
    }
    pslog_append_json_level_field_color_cached_or_dynamic(impl->shared, &buffer,
                                                          impl->min_level, 1);
  }

  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '}');
  pslog_buffer_finish_line(&buffer);
  pslog_write_buffer(impl->shared, &buffer);
  pslog_buffer_destroy(&buffer);
}

void pslog_emit_json(pslog_logger_impl *impl, pslog_level level,
                     pslog_string_view msg, const pslog_field *fields,
                     size_t count) {
  if (impl == NULL || impl->shared == NULL) {
    return;
  }
  if (impl->shared->color_enabled) {
    pslog_emit_json_color(impl, level, msg, fields, count);
    return;
  }
  pslog_emit_json_plain(impl, level, msg, fields, count);
}

void pslog_emit_json_kvfmt(pslog_logger_impl *impl, pslog_level level,
                           pslog_string_view msg,
                           const pslog_kvfmt_cache_entry *entry,
                           int saved_errno, va_list ap) {
  pslog_buffer buffer;
  pslog_field field;
  pslog_field level_field;
  int first_field;

  if (impl == NULL || impl->shared == NULL) {
    return;
  }

  pslog_buffer_init(&buffer, impl->shared, impl->shared->line_buffer_capacity);
  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '{');
  first_field = 1;

  if (impl->shared->timestamps) {
    if (impl->shared->color_enabled) {
      pslog_append_json_timestamp_field_color(impl->shared, &buffer,
                                              first_field);
    } else {
      field.key = impl->shared->verbose_fields ? "time" : "ts";
      field.key_len = impl->shared->verbose_fields ? 4u : 2u;
      field.type = PSLOG_FIELD_STRING;
      field.trusted_key = 1u;
      field.trusted_value = 0u;
      field.console_simple_value = 0u;
      field.value_len = 0u;
      field.as.string_value = "";
      if (!first_field) {
        PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
      }
      PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(&buffer, field.key,
                                                     field.key_len);
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ':');
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
      pslog_append_timestamp(impl->shared, &buffer);
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '"');
    }
    first_field = 0;
  }

  if (impl->shared->color_enabled) {
    if (!first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
    }
    pslog_append_json_level_field_color_cached_or_dynamic(impl->shared, &buffer,
                                                          level, 0);
    first_field = 0;
    if (msg.data != NULL && msg.len > 0u) {
      pslog_append_json_message_field_color(
          impl->shared, &buffer,
          impl->shared->verbose_fields ? "message" : "msg", msg, first_field);
      first_field = 0;
    }
    if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                                 impl->field_prefix_len);
      first_field = 0;
    } else {
      pslog_append_json_field_set_color(impl, &buffer, impl->fields,
                                        impl->field_count, &first_field);
    }
    pslog_append_json_kvfmt_fields_color(impl->shared, &buffer, entry,
                                         &first_field, saved_errno, ap);
    if (impl->include_level_field) {
      if (!first_field) {
        PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
      }
      pslog_append_json_level_field_color_cached_or_dynamic(
          impl->shared, &buffer, impl->min_level, 1);
    }
  } else {
    size_t level_literal_len;
    const char *level_literal;

    level_literal = pslog_json_level_field_literal(
        level, impl->shared->verbose_fields, &level_literal_len);
    if (!first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ',');
    }
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, level_literal, level_literal_len);
    first_field = 0;
    if (msg.data != NULL && msg.len > 0u) {
      pslog_append_json_message_field_plain(
          impl->shared, &buffer,
          impl->shared->verbose_fields ? "message" : "msg", msg, first_field);
      first_field = 0;
    }
    if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                                 impl->field_prefix_len);
      first_field = 0;
    } else {
      pslog_append_json_field_set_plain(impl, &buffer, impl->fields,
                                        impl->field_count, &first_field);
    }
    pslog_append_json_kvfmt_fields_plain(impl->shared, &buffer, entry,
                                         &first_field, saved_errno, ap);
    if (impl->include_level_field) {
      level_field.key = "loglevel";
      level_field.key_len = 8u;
      level_field.type = PSLOG_FIELD_STRING;
      level_field.trusted_key = 1u;
      level_field.trusted_value = 1u;
      level_field.console_simple_value = 1u;
      level_field.value_len = pslog_level_string_len(impl->min_level);
      level_field.as.string_value = pslog_level_string(impl->min_level);
      pslog_append_json_field_plain(impl->shared, &buffer, &level_field,
                                    first_field);
    }
  }

  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '}');
  pslog_buffer_finish_line(&buffer);
  pslog_write_buffer(impl->shared, &buffer);
  pslog_buffer_destroy(&buffer);
}

void pslog_append_json_field_prefix(pslog_shared_state *shared,
                                    pslog_buffer *buffer,
                                    const pslog_field *fields, size_t count) {
  size_t i;
  int first_field;

  if (shared == NULL || buffer == NULL) {
    return;
  }
  first_field = 0;
  for (i = 0u; i < count; ++i) {
    if (shared->color_enabled) {
      pslog_append_json_field_color(shared, buffer, fields + i, first_field);
    } else {
      pslog_append_json_field_plain(shared, buffer, fields + i, first_field);
    }
    first_field = 0;
  }
}

static void pslog_append_json_kvfmt_fields_plain(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int *first_field, int saved_errno,
    va_list ap) {
  const pslog_kvfmt_field_spec *spec;
  const char *string_value;
  size_t i;

  if (shared == NULL || buffer == NULL || entry == NULL ||
      first_field == NULL) {
    return;
  }
  for (i = 0u; i < entry->field_count; ++i) {
    spec = &entry->fields[i];
    if (!*first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
    }
    if (spec->json_color_prefix_len > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, spec->json_color_prefix,
                                 spec->json_color_prefix_len);
    } else {
      PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(buffer, spec->key,
                                                     spec->key_len);
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ':');
    }
    switch (spec->arg_type) {
    case PSLOG_KVFMT_ARG_STRING:
      string_value = va_arg(ap, const char *);
      pslog_buffer_append_json_string_maybe_trusted(
          buffer, string_value != NULL ? string_value : "");
      break;
    case PSLOG_KVFMT_ARG_SIGNED_INT:
      pslog_buffer_append_signed(buffer, (long)va_arg(ap, int));
      break;
    case PSLOG_KVFMT_ARG_SIGNED_LONG:
      pslog_buffer_append_signed(buffer, va_arg(ap, long));
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_INT:
      pslog_buffer_append_unsigned(buffer,
                                   (unsigned long)va_arg(ap, unsigned int));
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_LONG:
      pslog_buffer_append_unsigned(buffer, va_arg(ap, unsigned long));
      break;
    case PSLOG_KVFMT_ARG_DOUBLE: {
      double value;

      value = va_arg(ap, double);
      if (value != value) {
        if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
          PSLOG_APPEND_LITERAL(buffer, "null");
        } else {
          pslog_buffer_append_json_string_maybe_trusted(buffer, "NaN");
        }
      } else if (value > DBL_MAX || value < -DBL_MAX) {
        if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
          PSLOG_APPEND_LITERAL(buffer, "null");
        } else {
          pslog_buffer_append_json_string_maybe_trusted(
              buffer, value < 0.0 ? "-Inf" : "+Inf");
        }
      } else {
        pslog_buffer_append_double(buffer, value);
      }
      break;
    }
    case PSLOG_KVFMT_ARG_POINTER:
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
      pslog_buffer_append_pointer(buffer, va_arg(ap, void *));
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
      break;
    case PSLOG_KVFMT_ARG_BOOL:
      if (va_arg(ap, int)) {
        PSLOG_APPEND_LITERAL(buffer, "true");
      } else {
        PSLOG_APPEND_LITERAL(buffer, "false");
      }
      break;
    case PSLOG_KVFMT_ARG_ERRNO: {
      char errno_buffer[128];

      pslog_buffer_append_json_string_maybe_trusted(
          buffer,
          pslog_errno_string(saved_errno, errno_buffer, sizeof(errno_buffer)));
      break;
    }
    default:
      break;
    }
    *first_field = 0;
  }
}

static void pslog_append_json_kvfmt_fields_color(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int *first_field, int saved_errno,
    va_list ap) {
  const pslog_kvfmt_field_spec *spec;
  const pslog_palette *palette;
  const char *string_value;
  size_t i;

  if (shared == NULL || buffer == NULL || entry == NULL ||
      first_field == NULL) {
    return;
  }
  palette = shared->palette;
  for (i = 0u; i < entry->field_count; ++i) {
    spec = &entry->fields[i];
    if (!*first_field) {
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ',');
    }
    pslog_append_json_kvfmt_color_prefix(shared, buffer, spec);
    switch (spec->arg_type) {
    case PSLOG_KVFMT_ARG_STRING:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                 shared->palette_string_len);
      string_value = va_arg(ap, const char *);
      pslog_buffer_append_json_string_maybe_trusted(
          buffer, string_value != NULL ? string_value : "");
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_SIGNED_INT:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                 shared->palette_number_len);
      pslog_buffer_append_signed(buffer, (long)va_arg(ap, int));
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_SIGNED_LONG:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                 shared->palette_number_len);
      pslog_buffer_append_signed(buffer, va_arg(ap, long));
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_INT:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                 shared->palette_number_len);
      pslog_buffer_append_unsigned(buffer,
                                   (unsigned long)va_arg(ap, unsigned int));
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_UNSIGNED_LONG:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                 shared->palette_number_len);
      pslog_buffer_append_unsigned(buffer, va_arg(ap, unsigned long));
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_DOUBLE: {
      double value;

      value = va_arg(ap, double);
      if (value != value) {
        if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->null_value,
                                     shared->palette_null_value_len);
          PSLOG_APPEND_LITERAL(buffer, "null");
        } else {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                     shared->palette_string_len);
          pslog_buffer_append_json_string_maybe_trusted(buffer, "NaN");
        }
      } else if (value > DBL_MAX || value < -DBL_MAX) {
        if (shared->non_finite_float_policy == PSLOG_NON_FINITE_FLOAT_AS_NULL) {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->null_value,
                                     shared->palette_null_value_len);
          PSLOG_APPEND_LITERAL(buffer, "null");
        } else {
          PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                     shared->palette_string_len);
          pslog_buffer_append_json_string_maybe_trusted(
              buffer, value < 0.0 ? "-Inf" : "+Inf");
        }
      } else {
        PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->number,
                                   shared->palette_number_len);
        pslog_buffer_append_double(buffer, value);
      }
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    }
    case PSLOG_KVFMT_ARG_POINTER:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->string,
                                 shared->palette_string_len);
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
      pslog_buffer_append_pointer(buffer, va_arg(ap, void *));
      PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '"');
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_BOOL:
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->boolean,
                                 shared->palette_boolean_len);
      if (va_arg(ap, int)) {
        PSLOG_APPEND_LITERAL(buffer, "true");
      } else {
        PSLOG_APPEND_LITERAL(buffer, "false");
      }
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    case PSLOG_KVFMT_ARG_ERRNO: {
      char errno_buffer[128];

      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->error,
                                 shared->level_color_lens[4]);
      pslog_buffer_append_json_string_maybe_trusted(
          buffer,
          pslog_errno_string(saved_errno, errno_buffer, sizeof(errno_buffer)));
      PSLOG_BUFFER_APPEND_N_FAST(buffer, palette->reset,
                                 shared->palette_reset_len);
      break;
    }
    default:
      break;
    }
    *first_field = 0;
  }
}

static void
pslog_append_json_kvfmt_color_prefix(pslog_shared_state *shared,
                                     pslog_buffer *buffer,
                                     const pslog_kvfmt_field_spec *spec) {
  if (shared == NULL || buffer == NULL || spec == NULL) {
    return;
  }
  if (spec->json_color_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, spec->json_color_prefix,
                               spec->json_color_prefix_len);
    return;
  }
  PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->key,
                             shared->palette_key_len);
  PSLOG_BUFFER_APPEND_JSON_TRUSTED_STRING_N_FAST(buffer, spec->key,
                                                 spec->key_len);
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ':');
}
