#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lauxlib.h>
#include <lua.h>

#include <pslog.h>
#include <pslog_lua.h>

#define PSLUA_LOGGER_MT "pslog.logger"
#define PSLUA_WRAPPER_MT "pslog.wrapper"

typedef struct pslua_logger_ud {
  pslog_logger *log;
  pslog_field *scratch_fields;
  size_t scratch_capacity;
  size_t retained_refs;
  int closed;
} pslua_logger_ud;

typedef struct pslua_callback_output {
  lua_State *L;
  int fn_ref;
} pslua_callback_output;

typedef enum pslua_wrapper_kind {
  PSLUA_WRAPPER_NONE = 0,
  PSLUA_WRAPPER_BYTES,
  PSLUA_WRAPPER_TIME,
  PSLUA_WRAPPER_DURATION,
  PSLUA_WRAPPER_ERRNO,
  PSLUA_WRAPPER_TRUSTED,
  PSLUA_WRAPPER_U64
} pslua_wrapper_kind;

typedef struct pslua_wrapper_ud {
  pslua_wrapper_kind kind;
  long sec;
  long nsec;
  long offset;
  int code;
  pslog_uint64 u64_value;
  const char *text_value;
  size_t text_len;
} pslua_wrapper_ud;

static pslua_logger_ud *pslua_check_logger(lua_State *L, int index) {
  return (pslua_logger_ud *)luaL_checkudata(L, index, PSLUA_LOGGER_MT);
}

static pslua_logger_ud *pslua_test_logger(lua_State *L, int index) {
  return (pslua_logger_ud *)luaL_testudata(L, index, PSLUA_LOGGER_MT);
}

static int pslua_validate_view_out(pslog_lua_logger_view *out) {
  if (out == NULL) {
    return EINVAL;
  }
  if (out->size != sizeof(*out) ||
      out->abi_version != PSLOG_LUA_INTEROP_ABI_VERSION) {
    return EPROTO;
  }
  out->logger = NULL;
  out->flags = 0u;
  return 0;
}

static int pslua_validate_ref_out(pslog_lua_logger_ref *out) {
  if (out == NULL) {
    return EINVAL;
  }
  if (out->size != sizeof(*out) ||
      out->abi_version != PSLOG_LUA_INTEROP_ABI_VERSION) {
    return EPROTO;
  }
  out->L = NULL;
  out->registry_ref = LUA_NOREF;
  out->borrowed = NULL;
  out->derived = NULL;
  return 0;
}

int pslog_lua_is_logger(lua_State *L, int index) {
  pslua_logger_ud *ud;

  if (L == NULL) {
    return 0;
  }
  ud = pslua_test_logger(L, index);
  return ud != NULL && ud->log != NULL && !ud->closed;
}

int pslog_lua_check_logger(lua_State *L, int index,
                           pslog_lua_logger_view *out) {
  pslua_logger_ud *ud;
  int status;

  status = pslua_validate_view_out(out);
  if (status != 0) {
    return status;
  }
  if (L == NULL) {
    return EINVAL;
  }
  ud = pslua_test_logger(L, index);
  if (ud == NULL) {
    return ENOTTY;
  }
  if (ud->log == NULL || ud->closed) {
    return ENOENT;
  }
  out->logger = ud->log;
  return 0;
}

int pslog_lua_ref_logger(lua_State *L, int index, const pslog_field *fields,
                         size_t field_count, pslog_lua_logger_ref *out) {
  pslog_lua_logger_view view;
  int abs_index;
  int status;

  status = pslua_validate_ref_out(out);
  if (status != 0) {
    return status;
  }
  if (L == NULL || (field_count > 0u && fields == NULL)) {
    return EINVAL;
  }

  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  status = pslog_lua_check_logger(L, index, &view);
  if (status != 0) {
    return status;
  }

  abs_index = lua_absindex(L, index);
  lua_pushvalue(L, abs_index);
  out->registry_ref = luaL_ref(L, LUA_REGISTRYINDEX);
  if (out->registry_ref == LUA_NOREF || out->registry_ref == LUA_REFNIL) {
    out->registry_ref = LUA_NOREF;
    return ENOMEM;
  }

  out->L = L;
  out->borrowed = view.logger;
  ((pslua_logger_ud *)lua_touserdata(L, abs_index))->retained_refs += 1u;
  if (field_count > 0u) {
    out->derived = pslog_with(view.logger, fields, field_count);
    if (out->derived == NULL) {
      pslog_lua_unref_logger(out);
      return ENOMEM;
    }
  }
  return 0;
}

void pslog_lua_unref_logger(pslog_lua_logger_ref *ref) {
  pslua_logger_ud *ud = NULL;

  if (ref == NULL) {
    return;
  }
  if (ref->derived != NULL) {
    ref->derived->destroy(ref->derived);
  }
  if (ref->L != NULL && ref->registry_ref != LUA_NOREF) {
    lua_rawgeti(ref->L, LUA_REGISTRYINDEX, ref->registry_ref);
    ud = pslua_test_logger(ref->L, -1);
    if (ud != NULL && ud->retained_refs > 0u) {
      ud->retained_refs -= 1u;
    }
    lua_pop(ref->L, 1);
    luaL_unref(ref->L, LUA_REGISTRYINDEX, ref->registry_ref);
  }
  ref->L = NULL;
  ref->registry_ref = LUA_NOREF;
  ref->borrowed = NULL;
  ref->derived = NULL;
}

static int pslua_callback_output_write(void *userdata, const char *data,
                                       size_t len, size_t *written) {
  pslua_callback_output *output = (pslua_callback_output *)userdata;

  if (output == NULL || output->L == NULL) {
    return EINVAL;
  }

  lua_rawgeti(output->L, LUA_REGISTRYINDEX, output->fn_ref);
  lua_pushlstring(output->L, data, len);
  if (lua_pcall(output->L, 1, 0, 0) != LUA_OK) {
    lua_pop(output->L, 1);
    return EIO;
  }
  if (written != NULL) {
    *written = len;
  }
  return 0;
}

static int pslua_callback_output_close(void *userdata) {
  pslua_callback_output *output = (pslua_callback_output *)userdata;

  if (output == NULL) {
    return 0;
  }
  if (output->L != NULL && output->fn_ref != LUA_NOREF) {
    luaL_unref(output->L, LUA_REGISTRYINDEX, output->fn_ref);
    output->fn_ref = LUA_NOREF;
  }
  free(output);
  return 0;
}

static pslua_logger_ud *pslua_require_logger_ud(lua_State *L, int index) {
  pslua_logger_ud *ud = pslua_check_logger(L, index);

  luaL_argcheck(L, ud->log != NULL && !ud->closed, index, "logger is closed");
  return ud;
}

static pslog_logger *pslua_require_logger(lua_State *L, int index) {
  return pslua_require_logger_ud(L, index)->log;
}

static FILE *pslua_check_file(lua_State *L, int index) {
  luaL_Stream *stream =
      (luaL_Stream *)luaL_checkudata(L, index, LUA_FILEHANDLE);

  luaL_argcheck(L, stream->closef != NULL, index, "closed file");
  return stream->f;
}

static const char *pslua_value_string(lua_State *L, int index, size_t *len) {
  int abs_index = lua_absindex(L, index);

  switch (lua_type(L, abs_index)) {
  case LUA_TSTRING:
  case LUA_TNUMBER:
    return lua_tolstring(L, abs_index, len);
  default:
    return luaL_tolstring(L, abs_index, len);
  }
}

static pslog_string_view pslua_view(const char *data, size_t len) {
  pslog_string_view view = {data, len};
  return view;
}

static int pslua_parse_level_arg(lua_State *L, int index, pslog_level *level) {
  if (lua_isinteger(L, index)) {
    *level = (pslog_level)lua_tointeger(L, index);
    return 1;
  }
  if (lua_isstring(L, index)) {
    const char *text = lua_tostring(L, index);

    if (pslog_parse_level(text, level)) {
      return 1;
    }
  }
  luaL_argerror(L, index, "expected log level name or integer");
  return 0;
}

static int pslua_parse_mode_string(const char *text, pslog_mode *mode) {
  if (text == NULL) {
    return 0;
  }
  if (strcmp(text, "console") == 0) {
    *mode = PSLOG_MODE_CONSOLE;
    return 1;
  }
  if (strcmp(text, "json") == 0 || strcmp(text, "structured") == 0) {
    *mode = PSLOG_MODE_JSON;
    return 1;
  }
  return 0;
}

static int pslua_parse_color_string(const char *text,
                                    pslog_color_mode *color_mode) {
  if (text == NULL) {
    return 0;
  }
  if (strcmp(text, "auto") == 0) {
    *color_mode = PSLOG_COLOR_AUTO;
    return 1;
  }
  if (strcmp(text, "never") == 0 || strcmp(text, "off") == 0 ||
      strcmp(text, "false") == 0) {
    *color_mode = PSLOG_COLOR_NEVER;
    return 1;
  }
  if (strcmp(text, "always") == 0 || strcmp(text, "on") == 0 ||
      strcmp(text, "true") == 0) {
    *color_mode = PSLOG_COLOR_ALWAYS;
    return 1;
  }
  return 0;
}

static int
pslua_parse_float_policy_string(const char *text,
                                pslog_non_finite_float_policy *policy) {
  if (text == NULL) {
    return 0;
  }
  if (strcmp(text, "string") == 0) {
    *policy = PSLOG_NON_FINITE_FLOAT_AS_STRING;
    return 1;
  }
  if (strcmp(text, "null") == 0) {
    *policy = PSLOG_NON_FINITE_FLOAT_AS_NULL;
    return 1;
  }
  return 0;
}

static pslua_wrapper_ud *pslua_test_wrapper(lua_State *L, int index) {
  return (pslua_wrapper_ud *)luaL_testudata(L, index, PSLUA_WRAPPER_MT);
}

static pslua_wrapper_ud *pslua_new_wrapper(lua_State *L,
                                           pslua_wrapper_kind kind) {
  pslua_wrapper_ud *ud =
      (pslua_wrapper_ud *)lua_newuserdatauv(L, sizeof(*ud), 1);

  memset(ud, 0, sizeof(*ud));
  ud->kind = kind;
  luaL_setmetatable(L, PSLUA_WRAPPER_MT);
  return ud;
}

static pslog_uint64 pslua_parse_u64_string(lua_State *L, int arg_index,
                                           const char *text) {
  const pslog_uint64 max_value = (pslog_uint64) ~(pslog_uint64)0u;
  pslog_uint64 value = 0u;

  if (text == NULL || *text == '\0') {
    luaL_argerror(L, arg_index, "u64 wrapper requires a decimal string");
    return 0u;
  }
  while (*text != '\0') {
    if (*text < '0' || *text > '9') {
      luaL_argerror(L, arg_index, "u64 wrapper requires a decimal string");
      return 0u;
    }
    if (value > max_value / 10u ||
        (value == max_value / 10u &&
         (pslog_uint64)(*text - '0') > max_value % 10u)) {
      luaL_argerror(L, arg_index,
                    "u64 wrapper decimal string exceeds uint64 range");
      return 0u;
    }
    value = (value * 10u) + (pslog_uint64)(*text - '0');
    ++text;
  }
  return value;
}

static int pslua_wrapper_to_field(lua_State *L, pslog_field *field,
                                  const char *key, size_t key_len,
                                  int value_index) {
  int abs_index = lua_absindex(L, value_index);
  pslua_wrapper_ud *wrapper_ud;
  pslog_string_view key_view = pslua_view(key, key_len);

  wrapper_ud = pslua_test_wrapper(L, abs_index);

  if (wrapper_ud != NULL) {
    switch (wrapper_ud->kind) {
    case PSLUA_WRAPPER_BYTES:
      *field = pslog_bytes_field_view(key_view, wrapper_ud->text_value,
                                      wrapper_ud->text_len);
      return 0;
    case PSLUA_WRAPPER_TIME:
      *field = pslog_time_field_view(key_view, wrapper_ud->sec,
                                     wrapper_ud->nsec, (int)wrapper_ud->offset);
      return 0;
    case PSLUA_WRAPPER_DURATION:
      *field = pslog_duration_field_view(key_view, wrapper_ud->sec,
                                         wrapper_ud->nsec);
      return 0;
    case PSLUA_WRAPPER_ERRNO:
      *field = pslog_errno_view(key_view, wrapper_ud->code);
      return 0;
    case PSLUA_WRAPPER_TRUSTED:
      *field = pslog_trusted_str_view(
          key_view, pslua_view(wrapper_ud->text_value, wrapper_ud->text_len));
      return 0;
    case PSLUA_WRAPPER_U64:
      *field = pslog_u64_view(key_view, wrapper_ud->u64_value);
      return 0;
    case PSLUA_WRAPPER_NONE:
    default:
      return -1;
    }
  }
  return -1;
}

static int pslua_wrap_bytes(lua_State *L) {
  size_t len = 0u;
  const char *value = luaL_checklstring(L, 1, &len);
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_BYTES);

  ud->text_value = value;
  ud->text_len = len;
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  return 1;
}

static int pslua_wrap_time(lua_State *L) {
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_TIME);

  ud->sec = (long)luaL_checkinteger(L, 1);
  ud->nsec = (long)luaL_optinteger(L, 2, 0);
  ud->offset = (long)luaL_optinteger(L, 3, 0);
  return 1;
}

static int pslua_wrap_duration(lua_State *L) {
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_DURATION);

  ud->sec = (long)luaL_checkinteger(L, 1);
  ud->nsec = (long)luaL_optinteger(L, 2, 0);
  return 1;
}

static int pslua_wrap_errno(lua_State *L) {
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_ERRNO);

  ud->code = (int)luaL_checkinteger(L, 1);
  return 1;
}

static int pslua_wrap_trusted(lua_State *L) {
  size_t len = 0u;
  const char *value = luaL_checklstring(L, 1, &len);
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_TRUSTED);

  ud->text_value = value;
  ud->text_len = len;
  lua_pushvalue(L, 1);
  lua_setiuservalue(L, -2, 1);
  return 1;
}

static int pslua_wrap_u64(lua_State *L) {
  pslua_wrapper_ud *ud = pslua_new_wrapper(L, PSLUA_WRAPPER_U64);

  if (lua_isinteger(L, 1)) {
    lua_Integer value = lua_tointeger(L, 1);

    luaL_argcheck(L, value >= 0, 1, "u64 wrapper requires non-negative value");
    ud->u64_value = (pslog_uint64)value;
    return 1;
  }
  ud->u64_value = pslua_parse_u64_string(L, 1, luaL_checkstring(L, 1));
  return 1;
}

static pslog_field *pslua_reserve_fields(lua_State *L, pslua_logger_ud *ud,
                                         size_t count) {
  pslog_field *fields;

  if (count == 0u) {
    return NULL;
  }
  if (ud->scratch_capacity >= count && ud->scratch_fields != NULL) {
    return ud->scratch_fields;
  }

  fields = (pslog_field *)realloc(ud->scratch_fields, count * sizeof(*fields));
  if (fields == NULL) {
    luaL_error(L, "failed to allocate field array");
    return NULL;
  }

  ud->scratch_fields = fields;
  ud->scratch_capacity = count;
  return fields;
}

static pslog_field *pslua_ensure_fields_capacity(lua_State *L,
                                                 pslua_logger_ud *ud,
                                                 size_t *capacity_inout,
                                                 size_t needed) {
  size_t capacity = *capacity_inout;

  if (needed <= capacity && ud->scratch_fields != NULL) {
    return ud->scratch_fields;
  }
  if (capacity == 0u) {
    capacity = 8u;
  }
  while (capacity < needed) {
    size_t next_capacity = capacity * 2u;

    if (next_capacity <= capacity) {
      capacity = needed;
      break;
    }
    capacity = next_capacity;
  }
  *capacity_inout = capacity;
  return pslua_reserve_fields(L, ud, capacity);
}

static inline int pslua_set_field_from_lua(lua_State *L, pslog_field *field,
                                           const char *key, size_t key_len,
                                           int value_index) {
  int type = lua_type(L, value_index);
  size_t len;
  const char *text;
  pslog_string_view key_view = pslua_view(key, key_len);

  switch (type) {
  case LUA_TNIL:
    *field = pslog_null_view(key_view);
    return 0;
  case LUA_TBOOLEAN:
    *field = pslog_bool_view(key_view, lua_toboolean(L, value_index));
    return 0;
  case LUA_TNUMBER:
    if (lua_isinteger(L, value_index)) {
      *field =
          pslog_i64_view(key_view, (pslog_int64)lua_tointeger(L, value_index));
    } else {
      *field = pslog_f64_view(key_view, (double)lua_tonumber(L, value_index));
    }
    return 0;
  case LUA_TUSERDATA:
    if (pslua_wrapper_to_field(L, field, key, key_len, value_index) == 0) {
      return 0;
    }
    *field = pslog_ptr_view(key_view, lua_touserdata(L, value_index));
    return 0;
  case LUA_TLIGHTUSERDATA:
    *field = pslog_ptr_view(key_view, lua_touserdata(L, value_index));
    return 0;
  case LUA_TSTRING:
    text = lua_tolstring(L, value_index, &len);
    *field = pslog_str_view(key_view, pslua_view(text, len));
    return 0;
  case LUA_TTABLE:
    if (pslua_wrapper_to_field(L, field, key, key_len, value_index) == 0) {
      return 0;
    }
    text = pslua_value_string(L, value_index, &len);
    *field = pslog_str_view(key_view, pslua_view(text, len));
    return 1;
  default:
    text = pslua_value_string(L, value_index, &len);
    *field = pslog_str_view(key_view, pslua_view(text, len));
    return 1;
  }
}

static int pslua_collect_fields_from_pairs(lua_State *L, pslua_logger_ud *ud,
                                           int start_index,
                                           pslog_field **out_fields,
                                           size_t *out_count) {
  int top = lua_gettop(L);
  int arg_count = top - start_index + 1;
  int i;
  pslog_field *fields;

  if (arg_count <= 0) {
    *out_fields = NULL;
    *out_count = 0u;
    return top;
  }
  if ((arg_count % 2) != 0) {
    luaL_error(L, "expected key/value pairs");
  }

  fields = pslua_reserve_fields(L, ud, (size_t)(arg_count / 2));

  for (i = 0; i < arg_count; i += 2) {
    size_t key_len = 0u;
    const char *key = lua_tolstring(L, start_index + i, &key_len);

    luaL_argcheck(L, key != NULL, start_index + i,
                  "field key must be string-like");
    pslua_set_field_from_lua(L, &fields[i / 2], key, key_len,
                             start_index + i + 1);
  }

  *out_fields = fields;
  *out_count = (size_t)(arg_count / 2);
  return lua_gettop(L);
}

static int pslua_collect_fields_from_table(lua_State *L, pslua_logger_ud *ud,
                                           int index, pslog_field **out_fields,
                                           size_t *out_count) {
  int abs_index = lua_absindex(L, index);
  int field_index = 0;
  size_t capacity = 0u;
  pslog_field *fields = NULL;

  lua_pushnil(L);
  while (lua_next(L, abs_index) != 0) {
    if (lua_type(L, -2) == LUA_TSTRING) {
      size_t key_len = 0u;
      const char *key = lua_tolstring(L, -2, &key_len);
      int extra_stack;

      luaL_argcheck(L, key != NULL, index, "field key must be string-like");
      fields = pslua_ensure_fields_capacity(L, ud, &capacity,
                                            (size_t)field_index + 1u);
      extra_stack =
          pslua_set_field_from_lua(L, &fields[field_index], key, key_len, -1);
      field_index++;
      if (extra_stack != 0) {
        lua_insert(L, -3);
      }
      lua_pop(L, 1);
    } else {
      size_t key_len = 0u;
      const char *key;
      int extra_stack;

      lua_pushvalue(L, -2);
      key = lua_tolstring(L, -1, &key_len);
      luaL_argcheck(L, key != NULL, index, "field key must be string-like");
      fields = pslua_ensure_fields_capacity(L, ud, &capacity,
                                            (size_t)field_index + 1u);
      extra_stack =
          pslua_set_field_from_lua(L, &fields[field_index], key, key_len, -2);
      field_index++;
      if (extra_stack != 0) {
        lua_insert(L, -4);
      }
      lua_insert(L, -3);
      lua_pop(L, 1);
    }
  }

  if (field_index == 0) {
    *out_fields = NULL;
    *out_count = 0u;
    return lua_gettop(L);
  }
  *out_fields = fields;
  *out_count = (size_t)field_index;
  return lua_gettop(L);
}

static int pslua_collect_fields(lua_State *L, pslua_logger_ud *ud,
                                int start_index, pslog_field **out_fields,
                                size_t *out_count) {
  int top = lua_gettop(L);

  if (start_index > top) {
    *out_fields = NULL;
    *out_count = 0u;
    return top;
  }

  if ((top - start_index + 1) == 1 && lua_istable(L, start_index)) {
    return pslua_collect_fields_from_table(L, ud, start_index, out_fields,
                                           out_count);
  }
  return pslua_collect_fields_from_pairs(L, ud, start_index, out_fields,
                                         out_count);
}

static void pslua_free_fields(pslog_field *fields) { (void)fields; }

static int pslua_push_logger(lua_State *L, pslog_logger *log) {
  pslua_logger_ud *ud;

  if (log == NULL) {
    return luaL_error(L, "failed to create logger");
  }

  ud = (pslua_logger_ud *)lua_newuserdatauv(L, sizeof(*ud), 0);
  memset(ud, 0, sizeof(*ud));
  ud->log = log;
  luaL_setmetatable(L, PSLUA_LOGGER_MT);
  return 1;
}

static void pslua_parse_output(lua_State *L, int index, pslog_config *config,
                               int *output_owned) {
  int abs_index = lua_absindex(L, index);

  lua_getfield(L, abs_index, "output");
  if (lua_isnil(L, -1)) {
    lua_pop(L, 1);
    return;
  }

  if (lua_type(L, -1) == LUA_TSTRING) {
    const char *value = lua_tostring(L, -1);

    if (strcmp(value, "stdout") == 0 || strcmp(value, "default") == 0) {
      config->output = pslog_output_from_fp(stdout, 0);
      lua_pop(L, 1);
      return;
    }
    if (strcmp(value, "stderr") == 0) {
      config->output = pslog_output_from_fp(stderr, 0);
      lua_pop(L, 1);
      return;
    }
    if (pslog_output_init_file(&config->output, value, "ab") != 0) {
      int saved_errno = errno;

      lua_pop(L, 1);
      luaL_error(L, "failed to open output path '%s': %s", value,
                 strerror(saved_errno));
    }
    *output_owned = 1;
    lua_pop(L, 1);
    return;
  }

  if (lua_isfunction(L, -1)) {
    pslua_callback_output *output =
        (pslua_callback_output *)calloc(1u, sizeof(*output));

    if (output == NULL) {
      lua_pop(L, 1);
      luaL_error(L, "failed to allocate callback output");
    }
    output->L = L;
    lua_pushvalue(L, -1);
    output->fn_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    config->output.write = pslua_callback_output_write;
    config->output.close = pslua_callback_output_close;
    config->output.isatty = NULL;
    config->output.userdata = output;
    config->output.owned = 1;
    *output_owned = 1;
    lua_pop(L, 1);
    return;
  }

  if (luaL_testudata(L, -1, LUA_FILEHANDLE) != NULL) {
    config->output = pslog_output_from_fp(pslua_check_file(L, -1), 0);
    lua_pop(L, 1);
    return;
  }

  luaL_error(L, "output must be nil, a path string, stdout/stderr, a file, or "
                "a function");
}

static void pslua_parse_config(lua_State *L, int index, pslog_config *config,
                               int *output_owned) {
  int abs_index = lua_absindex(L, index);

  pslog_default_config(config);
  *output_owned = 0;
  if (index == 0 || lua_isnoneornil(L, index)) {
    return;
  }

  luaL_checktype(L, abs_index, LUA_TTABLE);

  lua_getfield(L, abs_index, "mode");
  if (!lua_isnil(L, -1)) {
    const char *text = luaL_checkstring(L, -1);

    if (!pslua_parse_mode_string(text, &config->mode)) {
      luaL_error(L, "invalid mode '%s'", text);
    }
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "color");
  if (!lua_isnil(L, -1)) {
    const char *text = luaL_checkstring(L, -1);

    if (!pslua_parse_color_string(text, &config->color)) {
      luaL_error(L, "invalid color mode '%s'", text);
    }
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "no_color");
  if (!lua_isnil(L, -1) && lua_toboolean(L, -1)) {
    config->color = PSLOG_COLOR_NEVER;
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "force_color");
  if (!lua_isnil(L, -1) && lua_toboolean(L, -1)) {
    config->color = PSLOG_COLOR_ALWAYS;
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "disable_timestamp");
  if (!lua_isnil(L, -1)) {
    config->timestamps = !lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "utc");
  if (!lua_isnil(L, -1)) {
    config->utc = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "with_level_field");
  if (!lua_isnil(L, -1)) {
    config->with_level_field = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "verbose_fields");
  if (!lua_isnil(L, -1)) {
    config->verbose_fields = lua_toboolean(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "time_format");
  if (!lua_isnil(L, -1)) {
    config->time_format = luaL_checkstring(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "line_buffer_capacity");
  if (!lua_isnil(L, -1)) {
    config->line_buffer_capacity = (size_t)luaL_checkinteger(L, -1);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "min_level");
  if (!lua_isnil(L, -1)) {
    pslua_parse_level_arg(L, -1, &config->min_level);
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "palette");
  if (!lua_isnil(L, -1)) {
    config->palette = pslog_palette_by_name(luaL_checkstring(L, -1));
  }
  lua_pop(L, 1);

  lua_getfield(L, abs_index, "non_finite_float_policy");
  if (!lua_isnil(L, -1)) {
    const char *text = luaL_checkstring(L, -1);

    if (!pslua_parse_float_policy_string(text,
                                         &config->non_finite_float_policy)) {
      luaL_error(L, "invalid non_finite_float_policy '%s'", text);
    }
  }
  lua_pop(L, 1);

  pslua_parse_output(L, abs_index, config, output_owned);
}

static int pslua_logger_gc(lua_State *L) {
  pslua_logger_ud *ud = pslua_check_logger(L, 1);

  if (ud->log != NULL) {
    ud->log->destroy(ud->log);
    ud->log = NULL;
  }
  ud->closed = 1;
  ud->retained_refs = 0u;
  free(ud->scratch_fields);
  ud->scratch_fields = NULL;
  ud->scratch_capacity = 0u;
  return 0;
}

static int pslua_logger_close(lua_State *L) {
  pslua_logger_ud *ud = pslua_check_logger(L, 1);
  int status = 0;

  if (ud->log != NULL && !ud->closed) {
    status = pslog_close(ud->log);
    ud->closed = 1;
  }
  lua_pushboolean(L, status == 0);
  if (status != 0) {
    lua_pushinteger(L, status);
    return 2;
  }
  return 1;
}

static int pslua_logger_with(lua_State *L) {
  pslua_logger_ud *ud = pslua_check_logger(L, 1);
  pslog_logger *log = pslua_require_logger(L, 1);
  pslog_field *fields = NULL;
  size_t count = 0u;
  int base_top = lua_gettop(L);
  int cleanup_top = pslua_collect_fields(L, ud, 2, &fields, &count);
  pslog_logger *child = pslog_with(log, fields, count);

  pslua_free_fields(fields);
  (void)cleanup_top;
  lua_settop(L, base_top);
  return pslua_push_logger(L, child);
}

static int pslua_logger_with_level(lua_State *L) {
  pslog_logger *log = pslua_require_logger(L, 1);
  pslog_level level;

  pslua_parse_level_arg(L, 2, &level);
  return pslua_push_logger(L, pslog_with_level(log, level));
}

static int pslua_logger_with_level_field(lua_State *L) {
  return pslua_push_logger(L,
                           pslog_with_level_field(pslua_require_logger(L, 1)));
}

static int pslua_logger_log(lua_State *L) {
  pslua_logger_ud *ud = pslua_require_logger_ud(L, 1);
  pslog_logger *log = ud->log;
  pslog_level level;
  size_t msg_len = 0u;
  const char *msg = luaL_checklstring(L, 3, &msg_len);
  pslog_string_view msg_view = pslua_view(msg, msg_len);
  pslog_field *fields = NULL;
  size_t count = 0u;
  int base_top = lua_gettop(L);
  int cleanup_top;

  pslua_parse_level_arg(L, 2, &level);
  cleanup_top = pslua_collect_fields(L, ud, 4, &fields, &count);
  switch (level) {
  case PSLOG_LEVEL_FATAL:
    pslog_fatal_view(log, msg_view, fields, count);
    break;
  case PSLOG_LEVEL_PANIC:
    pslog_panic_view(log, msg_view, fields, count);
    break;
  default:
    pslog_fields_view(log, level, msg_view, fields, count);
    break;
  }
  pslua_free_fields(fields);
  (void)cleanup_top;
  lua_settop(L, base_top);
  return 0;
}

static int pslua_logger_log_at(lua_State *L, pslog_level level) {
  pslua_logger_ud *ud = pslua_require_logger_ud(L, 1);
  pslog_logger *log = ud->log;
  size_t msg_len = 0u;
  const char *msg = luaL_checklstring(L, 2, &msg_len);
  pslog_string_view msg_view = pslua_view(msg, msg_len);
  pslog_field *fields = NULL;
  size_t count = 0u;
  int base_top = lua_gettop(L);
  int cleanup_top = pslua_collect_fields(L, ud, 3, &fields, &count);

  switch (level) {
  case PSLOG_LEVEL_FATAL:
    pslog_fatal_view(log, msg_view, fields, count);
    break;
  case PSLOG_LEVEL_PANIC:
    pslog_panic_view(log, msg_view, fields, count);
    break;
  default:
    pslog_fields_view(log, level, msg_view, fields, count);
    break;
  }
  pslua_free_fields(fields);
  (void)cleanup_top;
  lua_settop(L, base_top);
  return 0;
}

static int pslua_logger_trace(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_TRACE);
}

static int pslua_logger_debug(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_DEBUG);
}

static int pslua_logger_info(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_INFO);
}

static int pslua_logger_warn(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_WARN);
}

static int pslua_logger_error(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_ERROR);
}

static int pslua_logger_fatal(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_FATAL);
}

static int pslua_logger_panic(lua_State *L) {
  return pslua_logger_log_at(L, PSLOG_LEVEL_PANIC);
}

static int pslua_new_logger(lua_State *L) {
  pslog_config config;
  int output_owned = 0;
  pslog_logger *log;

  pslua_parse_config(L, 1, &config, &output_owned);
  log = pslog_new(&config);
  if (log == NULL && output_owned) {
    pslog_output_destroy(&config.output);
  }
  return pslua_push_logger(L, log);
}

static int pslua_new_logger_from_env(lua_State *L) {
  const char *prefix = NULL;
  pslog_config config;
  int output_owned = 0;
  pslog_logger *log;

  if (!lua_isnoneornil(L, 1)) {
    prefix = luaL_checkstring(L, 1);
  }
  pslua_parse_config(L, 2, &config, &output_owned);
  log = pslog_new_from_env(prefix, &config);
  if (log == NULL && output_owned) {
    pslog_output_destroy(&config.output);
  }
  return pslua_push_logger(L, log);
}

static int pslua_parse_level(lua_State *L) {
  pslog_level level;

  if (!pslog_parse_level(luaL_checkstring(L, 1), &level)) {
    lua_pushnil(L);
    return 1;
  }
  lua_pushstring(L, pslog_level_string(level));
  return 1;
}

static int pslua_level_string(lua_State *L) {
  pslog_level level;

  pslua_parse_level_arg(L, 1, &level);
  lua_pushstring(L, pslog_level_string(level));
  return 1;
}

static int pslua_version(lua_State *L) {
  lua_pushstring(L, PSLOG_VERSION_STRING);
  return 1;
}

static int pslua_available_palettes(lua_State *L) {
  size_t count = pslog_palette_count();
  size_t i;

  lua_createtable(L, (int)count, 0);
  for (i = 0; i < count; i++) {
    lua_pushstring(L, pslog_palette_name(i));
    lua_rawseti(L, -2, (lua_Integer)(i + 1u));
  }
  return 1;
}

static const luaL_Reg pslua_logger_methods[] = {
    {"close", pslua_logger_close},
    {"with", pslua_logger_with},
    {"with_level", pslua_logger_with_level},
    {"with_level_field", pslua_logger_with_level_field},
    {"log", pslua_logger_log},
    {"trace", pslua_logger_trace},
    {"debug", pslua_logger_debug},
    {"info", pslua_logger_info},
    {"warn", pslua_logger_warn},
    {"error", pslua_logger_error},
    {"fatal", pslua_logger_fatal},
    {"panic", pslua_logger_panic},
    {NULL, NULL}};

static const luaL_Reg pslua_module_funcs[] = {
    {"new_logger", pslua_new_logger},
    {"new_logger_from_env", pslua_new_logger_from_env},
    {"_wrap_bytes", pslua_wrap_bytes},
    {"_wrap_time", pslua_wrap_time},
    {"_wrap_duration", pslua_wrap_duration},
    {"_wrap_errno", pslua_wrap_errno},
    {"_wrap_trusted", pslua_wrap_trusted},
    {"_wrap_u64", pslua_wrap_u64},
    {"parse_level", pslua_parse_level},
    {"level_string", pslua_level_string},
    {"available_palettes", pslua_available_palettes},
    {"version", pslua_version},
    {NULL, NULL}};

static void pslua_register_logger(lua_State *L) {
  luaL_newmetatable(L, PSLUA_LOGGER_MT);
  lua_pushcfunction(L, pslua_logger_gc);
  lua_setfield(L, -2, "__gc");
  lua_newtable(L);
  luaL_setfuncs(L, pslua_logger_methods, 0);
  lua_setfield(L, -2, "__index");
  lua_pop(L, 1);
}

static void pslua_register_wrapper(lua_State *L) {
  luaL_newmetatable(L, PSLUA_WRAPPER_MT);
  lua_pop(L, 1);
}

int luaopen_pslog_core(lua_State *L) {
  pslua_register_logger(L);
  pslua_register_wrapper(L);
  luaL_newlib(L, pslua_module_funcs);
  lua_pushstring(L, "console");
  lua_setfield(L, -2, "MODE_CONSOLE");
  lua_pushstring(L, "json");
  lua_setfield(L, -2, "MODE_JSON");
  return 1;
}
