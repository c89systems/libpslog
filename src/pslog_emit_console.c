#include "pslog_internal.h"

static int pslog_console_prefix_add(size_t *total, size_t add, size_t limit) {
  if (total == NULL || *total > limit || add > limit - *total) {
    return 0;
  }
  *total += add;
  return 1;
}

#define PSLOG_APPEND_CONSOLE_VALUE_RAW_FAST(buffer, field)                     \
  do {                                                                         \
    switch ((field)->type) {                                                   \
    case PSLOG_FIELD_NULL:                                                     \
      PSLOG_APPEND_LITERAL((buffer), "null");                                  \
      break;                                                                   \
    case PSLOG_FIELD_STRING:                                                   \
      if ((field)->console_simple_value) {                                     \
        PSLOG_BUFFER_APPEND_N_FAST((buffer), (field)->as.string_value,         \
                                   (field)->value_len);                        \
      } else {                                                                 \
        pslog_buffer_append_console_string_n(                                  \
            (buffer), (field)->as.string_value, (field)->value_len);           \
      }                                                                        \
      break;                                                                   \
    case PSLOG_FIELD_SIGNED:                                                   \
      pslog_buffer_append_signed((buffer), (field)->as.signed_value);          \
      break;                                                                   \
    case PSLOG_FIELD_UNSIGNED:                                                 \
      pslog_buffer_append_unsigned((buffer), (field)->as.unsigned_value);      \
      break;                                                                   \
    case PSLOG_FIELD_DOUBLE:                                                   \
      pslog_buffer_append_double((buffer), (field)->as.double_value);          \
      break;                                                                   \
    case PSLOG_FIELD_BOOL:                                                     \
      if ((field)->as.bool_value) {                                            \
        PSLOG_APPEND_LITERAL((buffer), "true");                                \
      } else {                                                                 \
        PSLOG_APPEND_LITERAL((buffer), "false");                               \
      }                                                                        \
      break;                                                                   \
    case PSLOG_FIELD_POINTER:                                                  \
      pslog_buffer_append_pointer((buffer), (field)->as.pointer_value);        \
      break;                                                                   \
    case PSLOG_FIELD_BYTES:                                                    \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                            \
      pslog_buffer_append_bytes_hex((buffer), (field)->as.bytes_value.data,    \
                                    (field)->as.bytes_value.len);              \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                            \
      break;                                                                   \
    case PSLOG_FIELD_TIME:                                                     \
      pslog_buffer_append_time((buffer), (field)->as.time_value, 0);           \
      break;                                                                   \
    case PSLOG_FIELD_DURATION:                                                 \
      pslog_buffer_append_duration((buffer), (field)->as.duration_value, 0);   \
      break;                                                                   \
    case PSLOG_FIELD_ERRNO: {                                                  \
      char _pslog_errno_buffer[128];                                           \
      const char *_pslog_errno_text;                                           \
      size_t _pslog_errno_len;                                                 \
      _pslog_errno_text = pslog_errno_string((int)(field)->as.signed_value,    \
                                             _pslog_errno_buffer,              \
                                             sizeof(_pslog_errno_buffer));     \
      _pslog_errno_len = strlen(_pslog_errno_text);                            \
      if (pslog_string_is_console_simple_internal(_pslog_errno_text)) {        \
        PSLOG_BUFFER_APPEND_N_FAST((buffer), _pslog_errno_text,                \
                                   _pslog_errno_len);                          \
      } else {                                                                 \
        pslog_buffer_append_console_string((buffer), _pslog_errno_text);       \
      }                                                                        \
      break;                                                                   \
    }                                                                          \
    }                                                                          \
  } while (0)

#define PSLOG_APPEND_CONSOLE_VALUE_ONLY_FAST(shared, buffer, field)            \
  do {                                                                         \
    const pslog_palette *_pslog_palette = (shared)->palette;                   \
    switch ((field)->type) {                                                   \
    case PSLOG_FIELD_NULL:                                                     \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->null_value,             \
                             (shared)->palette_null_value_len,                 \
                             (shared)->color_enabled);                         \
      PSLOG_APPEND_LITERAL((buffer), "null");                                  \
      break;                                                                   \
    case PSLOG_FIELD_STRING:                                                   \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->string,                 \
                             (shared)->palette_string_len,                     \
                             (shared)->color_enabled);                         \
      if ((field)->console_simple_value) {                                     \
        PSLOG_BUFFER_APPEND_N_FAST((buffer), (field)->as.string_value,         \
                                   (field)->value_len);                        \
      } else {                                                                 \
        pslog_buffer_append_console_string_n(                                  \
            (buffer), (field)->as.string_value, (field)->value_len);           \
      }                                                                        \
      break;                                                                   \
    case PSLOG_FIELD_SIGNED:                                                   \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->number,                 \
                             (shared)->palette_number_len,                     \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_signed((buffer), (field)->as.signed_value);          \
      break;                                                                   \
    case PSLOG_FIELD_UNSIGNED:                                                 \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->number,                 \
                             (shared)->palette_number_len,                     \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_unsigned((buffer), (field)->as.unsigned_value);      \
      break;                                                                   \
    case PSLOG_FIELD_DOUBLE:                                                   \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->number,                 \
                             (shared)->palette_number_len,                     \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_double((buffer), (field)->as.double_value);          \
      break;                                                                   \
    case PSLOG_FIELD_BOOL:                                                     \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->boolean,                \
                             (shared)->palette_boolean_len,                    \
                             (shared)->color_enabled);                         \
      if ((field)->as.bool_value) {                                            \
        PSLOG_APPEND_LITERAL((buffer), "true");                                \
      } else {                                                                 \
        PSLOG_APPEND_LITERAL((buffer), "false");                               \
      }                                                                        \
      break;                                                                   \
    case PSLOG_FIELD_POINTER:                                                  \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->number,                 \
                             (shared)->palette_number_len,                     \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_pointer((buffer), (field)->as.pointer_value);        \
      break;                                                                   \
    case PSLOG_FIELD_BYTES:                                                    \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->string,                 \
                             (shared)->palette_string_len,                     \
                             (shared)->color_enabled);                         \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                            \
      pslog_buffer_append_bytes_hex((buffer), (field)->as.bytes_value.data,    \
                                    (field)->as.bytes_value.len);              \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '"');                            \
      break;                                                                   \
    case PSLOG_FIELD_TIME:                                                     \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->timestamp,              \
                             (shared)->palette_timestamp_len,                  \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_time((buffer), (field)->as.time_value, 0);           \
      break;                                                                   \
    case PSLOG_FIELD_DURATION:                                                 \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->string,                 \
                             (shared)->palette_string_len,                     \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_duration((buffer), (field)->as.duration_value, 0);   \
      break;                                                                   \
    case PSLOG_FIELD_ERRNO: {                                                  \
      char _pslog_errno_buffer[128];                                           \
      const char *_pslog_errno_text;                                           \
      size_t _pslog_errno_len;                                                 \
      int _pslog_console_simple;                                               \
      _pslog_errno_text = pslog_errno_string((int)(field)->as.signed_value,    \
                                             _pslog_errno_buffer,              \
                                             sizeof(_pslog_errno_buffer));     \
      _pslog_errno_len = strlen(_pslog_errno_text);                            \
      _pslog_console_simple =                                                  \
          pslog_string_is_console_simple_internal(_pslog_errno_text);          \
      PSLOG_APPLY_COLOR_FAST((buffer), _pslog_palette->error,                  \
                             (shared)->level_color_lens[4],                    \
                             (shared)->color_enabled);                         \
      if (_pslog_console_simple) {                                             \
        PSLOG_BUFFER_APPEND_N_FAST((buffer), _pslog_errno_text,                \
                                   _pslog_errno_len);                          \
      } else {                                                                 \
        pslog_buffer_append_console_string((buffer), _pslog_errno_text);       \
      }                                                                        \
      break;                                                                   \
    }                                                                          \
    }                                                                          \
  } while (0)

#define PSLOG_APPEND_CONSOLE_FIELD_FALLBACK_FAST(shared, buffer, field)        \
  do {                                                                         \
    if ((field) != NULL && (field)->key != NULL && (field)->key_len > 0u) {    \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), ' ');                            \
      PSLOG_APPLY_COLOR_FAST((buffer), (shared)->palette->reset,               \
                             (shared)->palette_reset_len,                      \
                             (shared)->color_enabled);                         \
      PSLOG_APPLY_COLOR_FAST((buffer), (shared)->palette->key,                 \
                             (shared)->palette_key_len,                        \
                             (shared)->color_enabled);                         \
      pslog_buffer_append_console_key_n((buffer), (field)->key,                \
                                        (field)->key_len);                     \
      PSLOG_BUFFER_APPEND_CHAR_FAST((buffer), '=');                            \
      PSLOG_APPLY_COLOR_FAST((buffer), (shared)->palette->reset,               \
                             (shared)->palette_reset_len,                      \
                             (shared)->color_enabled);                         \
      PSLOG_APPEND_CONSOLE_VALUE_ONLY_FAST((shared), (buffer), (field));       \
    }                                                                          \
  } while (0)

static void pslog_append_console_field_set(pslog_logger_impl *impl,
                                           pslog_buffer *buffer,
                                           const pslog_field *fields,
                                           size_t count);
static void pslog_append_console_kvfmt_fields(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int saved_errno, va_list ap);
static void
pslog_append_console_kvfmt_prefix(pslog_shared_state *shared,
                                  pslog_buffer *buffer,
                                  const pslog_kvfmt_field_spec *spec);
#if defined(PSLOG_TEST_HOOKS)
static void pslog_console_cache_lock(pslog_shared_state *shared);
static void pslog_console_cache_unlock(pslog_shared_state *shared);
#endif
static const pslog_console_prefix_cache_entry *
pslog_resolve_console_prefix(pslog_shared_state *shared,
                             const pslog_field *field);
static void pslog_append_console_field_cached(pslog_shared_state *shared,
                                              pslog_buffer *buffer,
                                              const pslog_field *field);
static const char *pslog_console_field_value_color(pslog_shared_state *shared,
                                                   const pslog_field *field,
                                                   size_t *color_len_out);
static void pslog_append_console_message_prefix(pslog_shared_state *shared,
                                                pslog_buffer *buffer);

static void pslog_append_console_message_prefix(pslog_shared_state *shared,
                                                pslog_buffer *buffer) {
  if (shared == NULL || buffer == NULL) {
    return;
  }
  if (shared->console_message_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->console_message_prefix,
                               shared->console_message_prefix_len);
    return;
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ' ');
  if (shared->color_enabled) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->message,
                               shared->palette_message_len);
  }
}

void pslog_emit_console(pslog_logger_impl *impl, pslog_level level,
                        pslog_string_view msg, const pslog_field *fields,
                        size_t count) {
  pslog_buffer buffer;
  pslog_field level_field;

  pslog_buffer_init(&buffer, impl->shared, impl->shared->line_buffer_capacity);
  pslog_buffer_lock_output(&buffer);
  if (impl->shared->timestamps) {
    PSLOG_APPLY_COLOR_FAST(&buffer, impl->shared->palette->timestamp,
                           impl->shared->palette_timestamp_len,
                           impl->shared->color_enabled);
    pslog_append_timestamp(impl->shared, &buffer);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ' ');
  }
  pslog_append_level_label(impl->shared, &buffer, level);
  if (msg.data != NULL && msg.len > 0u) {
    pslog_append_console_message_prefix(impl->shared, &buffer);
    pslog_buffer_append_console_message_n(&buffer, msg.data, msg.len);
  }
  if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                               impl->field_prefix_len);
  } else if (impl->field_count > 0u) {
    pslog_append_console_field_set(impl, &buffer, impl->fields,
                                   impl->field_count);
  }
  if (count > 0u) {
    pslog_append_console_field_set(impl, &buffer, fields, count);
  }
  if (impl->include_level_field) {
    level_field.key = "loglevel";
    level_field.key_len = 8u;
    level_field.type = PSLOG_FIELD_STRING;
    level_field.trusted_key = 1u;
    level_field.trusted_value = 1u;
    level_field.console_simple_value = 1u;
    level_field.value_len = pslog_level_string_len(impl->min_level);
    level_field.as.string_value = pslog_level_string(impl->min_level);
    pslog_append_console_field_cached(impl->shared, &buffer, &level_field);
  }
  PSLOG_APPLY_COLOR_FAST(&buffer, impl->shared->palette->reset,
                         impl->shared->palette_reset_len,
                         impl->shared->color_enabled);
  pslog_buffer_finish_line(&buffer);
  pslog_write_buffer(impl->shared, &buffer);
  pslog_buffer_unlock_output(&buffer);
  PSLOG_BUFFER_DESTROY_FAST(&buffer);
}

void pslog_emit_console_kvfmt(pslog_logger_impl *impl, pslog_level level,
                              pslog_string_view msg,
                              const pslog_kvfmt_cache_entry *entry,
                              int saved_errno, va_list ap) {
  pslog_buffer buffer;
  pslog_field level_field;

  if (impl == NULL || impl->shared == NULL) {
    return;
  }

  pslog_buffer_init(&buffer, impl->shared, impl->shared->line_buffer_capacity);
  pslog_buffer_lock_output(&buffer);
  if (impl->shared->timestamps) {
    PSLOG_APPLY_COLOR_FAST(&buffer, impl->shared->palette->timestamp,
                           impl->shared->palette_timestamp_len,
                           impl->shared->color_enabled);
    pslog_append_timestamp(impl->shared, &buffer);
    PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, ' ');
  }
  pslog_append_level_label(impl->shared, &buffer, level);
  if (msg.data != NULL && msg.len > 0u) {
    pslog_append_console_message_prefix(impl->shared, &buffer);
    pslog_buffer_append_console_message_n(&buffer, msg.data, msg.len);
  }
  if (impl->field_prefix != NULL && impl->field_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(&buffer, impl->field_prefix,
                               impl->field_prefix_len);
  } else if (impl->field_count > 0u) {
    pslog_append_console_field_set(impl, &buffer, impl->fields,
                                   impl->field_count);
  }
  pslog_append_console_kvfmt_fields(impl->shared, &buffer, entry, saved_errno,
                                    ap);
  if (impl->include_level_field) {
    level_field.key = "loglevel";
    level_field.key_len = 8u;
    level_field.type = PSLOG_FIELD_STRING;
    level_field.trusted_key = 1u;
    level_field.trusted_value = 1u;
    level_field.console_simple_value = 1u;
    level_field.value_len = pslog_level_string_len(impl->min_level);
    level_field.as.string_value = pslog_level_string(impl->min_level);
    pslog_append_console_field_cached(impl->shared, &buffer, &level_field);
  }
  PSLOG_APPLY_COLOR_FAST(&buffer, impl->shared->palette->reset,
                         impl->shared->palette_reset_len,
                         impl->shared->color_enabled);
  pslog_buffer_finish_line(&buffer);
  pslog_write_buffer(impl->shared, &buffer);
  pslog_buffer_unlock_output(&buffer);
  PSLOG_BUFFER_DESTROY_FAST(&buffer);
}

void pslog_append_console_field_prefix(pslog_shared_state *shared,
                                       pslog_buffer *buffer,
                                       const pslog_field *fields,
                                       size_t count) {
  size_t i;

  if (shared == NULL || buffer == NULL) {
    return;
  }
  for (i = 0u; i < count; ++i) {
    pslog_append_console_field_cached(shared, buffer, fields + i);
  }
}

static void pslog_append_console_field_set(pslog_logger_impl *impl,
                                           pslog_buffer *buffer,
                                           const pslog_field *fields,
                                           size_t count) {
  pslog_append_console_field_prefix(impl->shared, buffer, fields, count);
}

static void pslog_append_console_kvfmt_fields(
    pslog_shared_state *shared, pslog_buffer *buffer,
    const pslog_kvfmt_cache_entry *entry, int saved_errno, va_list ap) {
  const pslog_kvfmt_field_spec *spec;
  const char *string_value;
  pslog_field errno_field;
  size_t i;

  if (shared == NULL || buffer == NULL || entry == NULL) {
    return;
  }
  for (i = 0u; i < entry->field_count; ++i) {
    spec = &entry->fields[i];
    if (spec->key == NULL || spec->key_len == 0u) {
      continue;
    }
    pslog_append_console_kvfmt_prefix(shared, buffer, spec);
    switch (spec->arg_type) {
    case PSLOG_KVFMT_ARG_STRING:
      string_value = va_arg(ap, const char *);
      pslog_buffer_append_console_string(
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
    case PSLOG_KVFMT_ARG_DOUBLE:
      pslog_buffer_append_double(buffer, va_arg(ap, double));
      break;
    case PSLOG_KVFMT_ARG_POINTER:
      pslog_buffer_append_pointer(buffer, va_arg(ap, void *));
      break;
    case PSLOG_KVFMT_ARG_BOOL:
      if (va_arg(ap, int)) {
        PSLOG_APPEND_LITERAL(buffer, "true");
      } else {
        PSLOG_APPEND_LITERAL(buffer, "false");
      }
      break;
    case PSLOG_KVFMT_ARG_ERRNO:
      errno_field.key = spec->key;
      errno_field.key_len = spec->key_len;
      errno_field.value_len = 0u;
      errno_field.type = PSLOG_FIELD_ERRNO;
      errno_field.trusted_key = 1u;
      errno_field.trusted_value = 0u;
      errno_field.console_simple_value = 0u;
      errno_field.as.signed_value = (pslog_int64)saved_errno;
      PSLOG_APPEND_CONSOLE_VALUE_ONLY_FAST(shared, buffer, &errno_field);
      break;
    default:
      break;
    }
  }
}

static void
pslog_append_console_kvfmt_prefix(pslog_shared_state *shared,
                                  pslog_buffer *buffer,
                                  const pslog_kvfmt_field_spec *spec) {
  const char *value_color;
  size_t value_color_len;

  if (shared == NULL || buffer == NULL || spec == NULL) {
    return;
  }
  if (spec->console_prefix_len > 0u) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, spec->console_prefix,
                               spec->console_prefix_len);
    return;
  }

  value_color = "";
  value_color_len = 0u;
  if (shared->color_enabled) {
    switch (spec->arg_type) {
    case PSLOG_KVFMT_ARG_STRING:
      value_color = shared->palette->string;
      value_color_len = shared->palette_string_len;
      break;
    case PSLOG_KVFMT_ARG_SIGNED_INT:
    case PSLOG_KVFMT_ARG_SIGNED_LONG:
    case PSLOG_KVFMT_ARG_UNSIGNED_INT:
    case PSLOG_KVFMT_ARG_UNSIGNED_LONG:
    case PSLOG_KVFMT_ARG_DOUBLE:
    case PSLOG_KVFMT_ARG_POINTER:
      value_color = shared->palette->number;
      value_color_len = shared->palette_number_len;
      break;
    case PSLOG_KVFMT_ARG_BOOL:
      value_color = shared->palette->boolean;
      value_color_len = shared->palette_boolean_len;
      break;
    case PSLOG_KVFMT_ARG_ERRNO:
      value_color = shared->palette->error;
      value_color_len = shared->level_color_lens[4];
      break;
    default:
      break;
    }
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                               shared->palette_reset_len);
  }
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, ' ');
  if (shared->color_enabled) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->key,
                               shared->palette_key_len);
  }
  PSLOG_BUFFER_APPEND_N_FAST(buffer, spec->key, spec->key_len);
  PSLOG_BUFFER_APPEND_CHAR_FAST(buffer, '=');
  if (shared->color_enabled) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, shared->palette->reset,
                               shared->palette_reset_len);
    if (value_color_len > 0u) {
      PSLOG_BUFFER_APPEND_N_FAST(buffer, value_color, value_color_len);
    }
  }
}

#if defined(PSLOG_TEST_HOOKS)
static void pslog_console_cache_lock(pslog_shared_state *shared) {
  if (shared != NULL) {
    (void)pthread_mutex_lock(&shared->state_mutex);
  }
}

static void pslog_console_cache_unlock(pslog_shared_state *shared) {
  if (shared != NULL) {
    (void)pthread_mutex_unlock(&shared->state_mutex);
  }
}
#endif

static const pslog_console_prefix_cache_entry *
pslog_resolve_console_prefix(pslog_shared_state *shared,
                             const pslog_field *field) {
  pslog_console_prefix_cache_entry *entry;
  const char *value_color;
  size_t slot;
  size_t probe;
  size_t index;
  size_t selected_index;
  size_t value_color_len;
  size_t prefix_len;
  size_t prefix_limit;

  if (shared == NULL || field == NULL || field->key == NULL ||
      field->key_len == 0u ||
      (!field->trusted_key &&
       memchr(field->key, '\0', field->key_len) != NULL)) {
    return NULL;
  }

  slot = ((((size_t)(const void *)field->key) >> 4u) ^
          ((size_t)field->type << 3u) ^ field->key_len) %
         PSLOG_CONSOLE_PREFIX_CACHE_SIZE;
  selected_index = slot;
  entry = NULL;
  for (probe = 0u; probe < PSLOG_CONSOLE_PREFIX_CACHE_PROBE; ++probe) {
    index = (slot + probe) % PSLOG_CONSOLE_PREFIX_CACHE_SIZE;
    if (shared->console_prefix_cache[index].key == field->key &&
        shared->console_prefix_cache[index].type == field->type &&
        shared->console_prefix_cache[index].key_len == field->key_len &&
        memcmp(shared->console_prefix_cache[index].key_copy, field->key,
               field->key_len) == 0) {
      return &shared->console_prefix_cache[index];
    }
    if (entry == NULL && shared->console_prefix_cache[index].key == NULL) {
      entry = &shared->console_prefix_cache[index];
      selected_index = index;
      break;
    }
  }
  if (entry == NULL) {
    entry = &shared->console_prefix_cache[selected_index];
  }

  prefix_limit = sizeof(entry->prefix) - 1u;
  prefix_len = 0u;
  if (!pslog_console_prefix_add(&prefix_len, 1u, prefix_limit) ||
      !pslog_console_prefix_add(&prefix_len, field->key_len, prefix_limit) ||
      !pslog_console_prefix_add(&prefix_len, 1u, prefix_limit)) {
    return NULL;
  }
  if (shared->color_enabled) {
    value_color =
        pslog_console_field_value_color(shared, field, &value_color_len);
    if (!pslog_console_prefix_add(&prefix_len, shared->palette_reset_len,
                                  prefix_limit) ||
        !pslog_console_prefix_add(&prefix_len, shared->palette_key_len,
                                  prefix_limit) ||
        !pslog_console_prefix_add(&prefix_len, shared->palette_reset_len,
                                  prefix_limit) ||
        !pslog_console_prefix_add(&prefix_len, value_color_len, prefix_limit)) {
      return NULL;
    }
    entry->prefix_len = 0u;
    if (shared->palette_reset_len > 0u) {
      memcpy(entry->prefix + entry->prefix_len, shared->palette->reset,
             shared->palette_reset_len);
      entry->prefix_len += shared->palette_reset_len;
    }
    entry->prefix[entry->prefix_len++] = ' ';
    if (shared->palette_key_len > 0u) {
      memcpy(entry->prefix + entry->prefix_len, shared->palette->key,
             shared->palette_key_len);
      entry->prefix_len += shared->palette_key_len;
    }
    memcpy(entry->prefix + entry->prefix_len, field->key, field->key_len);
    entry->prefix_len += field->key_len;
    entry->prefix[entry->prefix_len++] = '=';
    if (shared->palette_reset_len > 0u) {
      memcpy(entry->prefix + entry->prefix_len, shared->palette->reset,
             shared->palette_reset_len);
      entry->prefix_len += shared->palette_reset_len;
    }
    if (value_color != NULL && value_color_len > 0u) {
      memcpy(entry->prefix + entry->prefix_len, value_color, value_color_len);
      entry->prefix_len += value_color_len;
    }
  } else {
    entry->prefix_len = 0u;
    entry->prefix[entry->prefix_len++] = ' ';
    memcpy(entry->prefix + entry->prefix_len, field->key, field->key_len);
    entry->prefix_len += field->key_len;
    entry->prefix[entry->prefix_len++] = '=';
  }
  entry->prefix[entry->prefix_len] = '\0';
  entry->key = field->key;
  entry->key_len = field->key_len;
  entry->type = field->type;
  memcpy(entry->key_copy, field->key, field->key_len);
  entry->key_copy[field->key_len] = '\0';
  return entry;
}

#if defined(PSLOG_TEST_HOOKS)
int pslog_test_console_prefix_cacheable(pslog_shared_state *shared,
                                        const pslog_field *field) {
  const pslog_console_prefix_cache_entry *prefix;

  pslog_console_cache_lock(shared);
  prefix = pslog_resolve_console_prefix(shared, field);
  pslog_console_cache_unlock(shared);
  return prefix != NULL;
}
#endif

static void pslog_append_console_field_cached(pslog_shared_state *shared,
                                              pslog_buffer *buffer,
                                              const pslog_field *field) {
  const pslog_console_prefix_cache_entry *prefix;

  if (shared == NULL || buffer == NULL || field == NULL || field->key == NULL ||
      field->key_len == 0u) {
    return;
  }
  prefix = pslog_resolve_console_prefix(shared, field);
  if (prefix != NULL) {
    PSLOG_BUFFER_APPEND_N_FAST(buffer, prefix->prefix, prefix->prefix_len);
    PSLOG_APPEND_CONSOLE_VALUE_RAW_FAST(buffer, field);
    return;
  }
  PSLOG_APPEND_CONSOLE_FIELD_FALLBACK_FAST(shared, buffer, field);
}

static const char *pslog_console_field_value_color(pslog_shared_state *shared,
                                                   const pslog_field *field,
                                                   size_t *color_len_out) {
  if (color_len_out != NULL) {
    *color_len_out = 0u;
  }
  if (shared == NULL || field == NULL || shared->palette == NULL) {
    return "";
  }

  switch (field->type) {
  case PSLOG_FIELD_NULL:
    if (color_len_out != NULL) {
      *color_len_out = shared->palette_null_value_len;
    }
    return shared->palette->null_value;
  case PSLOG_FIELD_STRING:
  case PSLOG_FIELD_BYTES:
  case PSLOG_FIELD_DURATION:
    if (color_len_out != NULL) {
      *color_len_out = shared->palette_string_len;
    }
    return shared->palette->string;
  case PSLOG_FIELD_SIGNED:
  case PSLOG_FIELD_UNSIGNED:
  case PSLOG_FIELD_DOUBLE:
  case PSLOG_FIELD_POINTER:
    if (color_len_out != NULL) {
      *color_len_out = shared->palette_number_len;
    }
    return shared->palette->number;
  case PSLOG_FIELD_BOOL:
    if (color_len_out != NULL) {
      *color_len_out = shared->palette_boolean_len;
    }
    return shared->palette->boolean;
  case PSLOG_FIELD_TIME:
    if (color_len_out != NULL) {
      *color_len_out = shared->palette_timestamp_len;
    }
    return shared->palette->timestamp;
  case PSLOG_FIELD_ERRNO:
    if (color_len_out != NULL) {
      *color_len_out = shared->level_color_lens[4];
    }
    return shared->palette->error;
  default:
    return "";
  }
}
