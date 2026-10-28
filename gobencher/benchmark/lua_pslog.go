package benchmark

/*
#cgo CFLAGS: -O3 -DNDEBUG -I${SRCDIR}/../../build/lua-sdk/include -I${SRCDIR}/../../build/lua-host/include
#cgo LDFLAGS: ${SRCDIR}/../../build/lua-sdk/lib/libpslog.a ${SRCDIR}/../../build/lua-host/lib/liblua.a -lm -ldl -pthread

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "../../lua/src/pslog_lua.c"

typedef enum gobench_lua_field_kind {
	GOBENCH_LUA_FIELD_NULL = 0,
	GOBENCH_LUA_FIELD_STRING = 1,
	GOBENCH_LUA_FIELD_BOOL = 2,
	GOBENCH_LUA_FIELD_INT64 = 3,
	GOBENCH_LUA_FIELD_UINT64 = 4,
	GOBENCH_LUA_FIELD_FLOAT64 = 5
} gobench_lua_field_kind;

typedef struct gobench_lua_field {
	const char *key;
	gobench_lua_field_kind kind;
	const char *string_value;
	int bool_value;
	long long int64_value;
	unsigned long long uint64_value;
	double float64_value;
} gobench_lua_field;

typedef struct gobench_lua_entry {
	int level;
	const char *msg;
	const gobench_lua_field *fields;
	size_t field_count;
} gobench_lua_entry;

typedef struct gobench_lua_result {
	unsigned long long elapsed_ns;
	unsigned long long bytes;
	unsigned long long writes;
	unsigned long long ops;
} gobench_lua_result;

typedef struct gobench_lua_sink {
	unsigned long long bytes;
	unsigned long long writes;
} gobench_lua_sink;

typedef struct gobench_lua_logger {
	lua_State *L;
	int pslog_ref;
	int logger_ref;
	int trace_method_ref;
	int debug_method_ref;
	int info_method_ref;
	int warn_method_ref;
	int error_method_ref;
	int log_method_ref;
	gobench_lua_sink sink;
	char last_error[256];
} gobench_lua_logger;

typedef struct gobench_lua_prepared_entry {
	int level;
	int msg_ref;
	int *field_refs;
	size_t field_ref_count;
} gobench_lua_prepared_entry;

typedef struct gobench_lua_prepared_data {
	gobench_lua_logger *logger;
	gobench_lua_prepared_entry *entries;
	size_t entry_count;
} gobench_lua_prepared_data;

typedef struct gobench_lua_prepared_table_entry {
	int level;
	int msg_ref;
	int fields_ref;
} gobench_lua_prepared_table_entry;

typedef struct gobench_lua_prepared_table_data {
	gobench_lua_logger *logger;
	gobench_lua_prepared_table_entry *entries;
	size_t entry_count;
} gobench_lua_prepared_table_data;

static unsigned long long gobench_lua_now_ns(void) {
#if defined(CLOCK_MONOTONIC)
	struct timespec ts;
	if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
		return ((unsigned long long)ts.tv_sec * 1000000000ull) + (unsigned long long)ts.tv_nsec;
	}
#endif
	return ((unsigned long long)clock() * 1000000000ull) / (unsigned long long)CLOCKS_PER_SEC;
}

static int gobench_lua_sink_write(void *userdata, const char *data,
                                  size_t len, size_t *written) {
	gobench_lua_sink *sink = (gobench_lua_sink *)userdata;

	(void)data;
	if (sink == NULL) {
		return -1;
	}
	sink->bytes += (unsigned long long)len;
	sink->writes += 1ull;
	if (written != NULL) {
		*written = len;
	}
	return 0;
}

static void gobench_lua_set_error(gobench_lua_logger *logger, const char *message) {
	if (logger == NULL) {
		return;
	}
	if (message == NULL) {
		logger->last_error[0] = '\0';
		return;
	}
	snprintf(logger->last_error, sizeof(logger->last_error), "%s", message);
}

static int gobench_lua_bind_method(gobench_lua_logger *logger, int *ref_out,
                                   const char *name) {
	lua_rawgeti(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
	lua_getfield(logger->L, -1, name);
	lua_remove(logger->L, -2);
	if (!lua_isfunction(logger->L, -1)) {
		lua_settop(logger->L, 0);
		return -1;
	}
	*ref_out = luaL_ref(logger->L, LUA_REGISTRYINDEX);
	return 0;
}

static int gobench_lua_bind_methods(gobench_lua_logger *logger) {
	if (gobench_lua_bind_method(logger, &logger->trace_method_ref, "trace") != 0 ||
	    gobench_lua_bind_method(logger, &logger->debug_method_ref, "debug") != 0 ||
	    gobench_lua_bind_method(logger, &logger->info_method_ref, "info") != 0 ||
	    gobench_lua_bind_method(logger, &logger->warn_method_ref, "warn") != 0 ||
	    gobench_lua_bind_method(logger, &logger->error_method_ref, "error") != 0 ||
	    gobench_lua_bind_method(logger, &logger->log_method_ref, "log") != 0) {
		gobench_lua_set_error(logger, "bind logger methods");
		return -1;
	}
	return 0;
}

static int gobench_lua_method_ref_for_level(gobench_lua_logger *logger, int level) {
	switch (level) {
	case -1:
		return logger->trace_method_ref;
	case 0:
		return logger->debug_method_ref;
	case 1:
		return logger->info_method_ref;
	case 2:
		return logger->warn_method_ref;
	case 3:
		return logger->error_method_ref;
	default:
		return logger->log_method_ref;
	}
}

static int gobench_lua_set_package_paths(lua_State *L, const char *rock_tree) {
	size_t current_len;
	const char *current;
	char *new_value;
	size_t prefix_len;
	size_t current_path_len;
	int result = -1;

	lua_getglobal(L, "package");

	lua_getfield(L, -1, "path");
	current = lua_tolstring(L, -1, &current_len);
	prefix_len = (strlen(rock_tree) * 2u) + strlen("/share/lua/5.5/?.lua;/share/lua/5.5/?/init.lua;") + 1u;
	current_path_len = current != NULL ? current_len : 0u;
	new_value = (char *)malloc(prefix_len + current_path_len);
	if (new_value == NULL) {
		lua_pop(L, 2);
		return -1;
	}
	strcpy(new_value, rock_tree);
	strcat(new_value, "/share/lua/5.5/?.lua;");
	strcat(new_value, rock_tree);
	strcat(new_value, "/share/lua/5.5/?/init.lua;");
	if (current != NULL) {
		memcpy(new_value + strlen(new_value), current, current_len + 1u);
	}
	lua_pop(L, 1);
	lua_pushstring(L, new_value);
	lua_setfield(L, -2, "path");
	free(new_value);

	lua_getfield(L, -1, "cpath");
	current = lua_tolstring(L, -1, &current_len);
	prefix_len = strlen(rock_tree) + strlen("/lib/lua/5.5/?.so;") + 1u;
	current_path_len = current != NULL ? current_len : 0u;
	new_value = (char *)malloc(prefix_len + current_path_len);
	if (new_value == NULL) {
		lua_pop(L, 2);
		return -1;
	}
	strcpy(new_value, rock_tree);
	strcat(new_value, "/lib/lua/5.5/?.so;");
	if (current != NULL) {
		memcpy(new_value + strlen(new_value), current, current_len + 1u);
	}
	lua_pop(L, 1);
	lua_pushstring(L, new_value);
	lua_setfield(L, -2, "cpath");
	free(new_value);

	result = 0;
	lua_pop(L, 1);
	return result;
}

static int gobench_lua_bootstrap(lua_State *L, int timestamps) {
	const char *script =
		"return require('pslog')\n";
	int rc;

	rc = luaL_loadstring(L, script);
	if (rc != LUA_OK) {
		lua_pop(L, 1);
		return -1;
	}
	rc = lua_pcall(L, 0, 1, 0);
	if (rc != LUA_OK) {
		lua_pop(L, 1);
		return -1;
	}
	return 0;
}

static int gobench_lua_push_string(lua_State *L, const char *text) {
	lua_pushstring(L, text != NULL ? text : "");
	return 0;
}

static int gobench_lua_preload_core(lua_State *L) {
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "preload");
	lua_pushcfunction(L, luaopen_pslog_core);
	lua_setfield(L, -2, "pslog.core");
	lua_pop(L, 2);
	return 0;
}

static int gobench_lua_push_field_value(lua_State *L, int pslog_ref,
                                        const gobench_lua_field *field) {
	switch (field->kind) {
	case GOBENCH_LUA_FIELD_NULL:
		lua_pushnil(L);
		return 0;
	case GOBENCH_LUA_FIELD_STRING:
		lua_pushstring(L, field->string_value != NULL ? field->string_value : "");
		return 0;
	case GOBENCH_LUA_FIELD_BOOL:
		lua_pushboolean(L, field->bool_value);
		return 0;
	case GOBENCH_LUA_FIELD_INT64:
		lua_pushinteger(L, (lua_Integer)field->int64_value);
		return 0;
	case GOBENCH_LUA_FIELD_UINT64:
		char uint64_text[32];

		lua_rawgeti(L, LUA_REGISTRYINDEX, pslog_ref);
		lua_getfield(L, -1, "u64");
		lua_remove(L, -2);
		if (field->uint64_value <= (unsigned long long)LLONG_MAX) {
			lua_pushinteger(L, (lua_Integer)field->uint64_value);
		} else {
			snprintf(uint64_text, sizeof(uint64_text), "%llu", field->uint64_value);
			lua_pushstring(L, uint64_text);
		}
		if (lua_pcall(L, 1, 1, 0) != LUA_OK) {
			return -1;
		}
		return 0;
	case GOBENCH_LUA_FIELD_FLOAT64:
		lua_pushnumber(L, field->float64_value);
		return 0;
	default:
		lua_pushnil(L);
		return 0;
	}
}

static int gobench_lua_apply_with(gobench_lua_logger *logger,
                                  const gobench_lua_field *fields,
                                  size_t field_count) {
	size_t i;
	int nargs;

	if (logger == NULL || logger->L == NULL || field_count == 0u) {
		return 0;
	}

	lua_rawgeti(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
	lua_getfield(logger->L, -1, "with");
	lua_insert(logger->L, -2);
	nargs = 1;
	for (i = 0u; i < field_count; ++i) {
		lua_pushstring(logger->L, fields[i].key != NULL ? fields[i].key : "");
		if (gobench_lua_push_field_value(logger->L, logger->pslog_ref, &fields[i]) != 0) {
			gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
			lua_settop(logger->L, 0);
			return -1;
		}
		nargs += 2;
	}
	if (lua_pcall(logger->L, nargs, 1, 0) != LUA_OK) {
		gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
		lua_pop(logger->L, 1);
		return -1;
	}
	luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
	logger->logger_ref = luaL_ref(logger->L, LUA_REGISTRYINDEX);
	gobench_lua_set_error(logger, NULL);
	return 0;
}

static void gobench_lua_stats_reset(gobench_lua_logger *logger) {
	logger->sink.bytes = 0ull;
	logger->sink.writes = 0ull;
}

static void gobench_lua_stats_read(gobench_lua_logger *logger,
                                   unsigned long long *bytes_out,
                                   unsigned long long *writes_out) {
	if (bytes_out != NULL) {
		*bytes_out = logger->sink.bytes;
	}
	if (writes_out != NULL) {
		*writes_out = logger->sink.writes;
	}
}

static gobench_lua_logger *gobench_lua_logger_new(const char *rock_tree,
                                                  const gobench_lua_field *static_fields,
                                                  size_t static_count,
                                                  int timestamps) {
	gobench_lua_logger *logger;
	lua_State *L;
	pslog_config config;
	pslog_logger *pslog_logger;

	L = luaL_newstate();
	if (L == NULL) {
		return NULL;
	}
	luaL_openlibs(L);
	gobench_lua_preload_core(L);
	if (gobench_lua_set_package_paths(L, rock_tree) != 0) {
		lua_close(L);
		return NULL;
	}
	if (gobench_lua_bootstrap(L, timestamps) != 0) {
		lua_close(L);
		return NULL;
	}

	logger = (gobench_lua_logger *)calloc(1u, sizeof(*logger));
	if (logger == NULL) {
		lua_pop(L, 1);
		lua_close(L);
		return NULL;
	}
	logger->L = L;
	logger->pslog_ref = luaL_ref(L, LUA_REGISTRYINDEX);
	logger->trace_method_ref = LUA_NOREF;
	logger->debug_method_ref = LUA_NOREF;
	logger->info_method_ref = LUA_NOREF;
	logger->warn_method_ref = LUA_NOREF;
	logger->error_method_ref = LUA_NOREF;
	logger->log_method_ref = LUA_NOREF;
	logger->sink.bytes = 0ull;
	logger->sink.writes = 0ull;
	logger->last_error[0] = '\0';

	pslog_default_config(&config);
	config.mode = PSLOG_MODE_JSON;
	config.color = PSLOG_COLOR_NEVER;
	config.timestamps = timestamps ? 1 : 0;
	config.output.write = gobench_lua_sink_write;
	config.output.close = NULL;
	config.output.isatty = NULL;
	config.output.userdata = &logger->sink;
	config.output.owned = 0;
	pslog_logger = pslog_new(&config);
	if (pslog_logger == NULL) {
		luaL_unref(L, LUA_REGISTRYINDEX, logger->pslog_ref);
		lua_close(L);
		free(logger);
		return NULL;
	}
	lua_pushnil(L);
	pslua_push_logger(L, pslog_logger, 0, NULL, lua_gettop(L));
	lua_remove(L, -2);
	logger->logger_ref = luaL_ref(L, LUA_REGISTRYINDEX);

	if (gobench_lua_bind_methods(logger) != 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, logger->pslog_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->logger_ref);
		lua_close(L);
		free(logger);
		return NULL;
	}

	if (static_count > 0u && gobench_lua_apply_with(logger, static_fields, static_count) != 0) {
		luaL_unref(L, LUA_REGISTRYINDEX, logger->trace_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->debug_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->info_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->warn_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->error_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->log_method_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->pslog_ref);
		luaL_unref(L, LUA_REGISTRYINDEX, logger->logger_ref);
		lua_close(L);
		free(logger);
		return NULL;
	}
	return logger;
}

static gobench_lua_prepared_data *gobench_lua_prepare_data(
    gobench_lua_logger *logger, const gobench_lua_entry *entries,
    size_t entry_count) {
	gobench_lua_prepared_data *prepared;
	size_t i;
	size_t j;

	if (logger == NULL || logger->L == NULL || entries == NULL || entry_count == 0u) {
		return NULL;
	}

	prepared = (gobench_lua_prepared_data *)calloc(1u, sizeof(*prepared));
	if (prepared == NULL) {
		gobench_lua_set_error(logger, "allocate prepared lua data");
		return NULL;
	}
	prepared->entries = (gobench_lua_prepared_entry *)calloc(entry_count, sizeof(*prepared->entries));
	if (prepared->entries == NULL) {
		free(prepared);
		gobench_lua_set_error(logger, "allocate prepared lua entries");
		return NULL;
	}
	prepared->logger = logger;
	prepared->entry_count = entry_count;

	for (i = 0u; i < entry_count; ++i) {
		prepared->entries[i].level = entries[i].level;
		if (gobench_lua_push_string(logger->L, entries[i].msg) != 0) {
			gobench_lua_set_error(logger, "prepare message");
			lua_settop(logger->L, 0);
			goto fail;
		}
		prepared->entries[i].msg_ref = luaL_ref(logger->L, LUA_REGISTRYINDEX);
		prepared->entries[i].field_ref_count = entries[i].field_count * 2u;
		if (prepared->entries[i].field_ref_count == 0u) {
			continue;
		}
		prepared->entries[i].field_refs =
		    (int *)calloc(prepared->entries[i].field_ref_count, sizeof(*prepared->entries[i].field_refs));
		if (prepared->entries[i].field_refs == NULL) {
			gobench_lua_set_error(logger, "allocate prepared lua field refs");
			goto fail;
		}
		for (j = 0u; j < entries[i].field_count; ++j) {
			if (gobench_lua_push_string(logger->L, entries[i].fields[j].key) != 0) {
				gobench_lua_set_error(logger, "prepare field key");
				lua_settop(logger->L, 0);
				goto fail;
			}
			prepared->entries[i].field_refs[j * 2u] = luaL_ref(logger->L, LUA_REGISTRYINDEX);
			if (gobench_lua_push_field_value(logger->L, logger->pslog_ref, &entries[i].fields[j]) != 0) {
				gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
				lua_settop(logger->L, 0);
				goto fail;
			}
			prepared->entries[i].field_refs[(j * 2u) + 1u] = luaL_ref(logger->L, LUA_REGISTRYINDEX);
		}
	}

	return prepared;

fail:
	for (i = 0u; i < entry_count; ++i) {
		size_t ref_index;

		if (prepared->entries[i].msg_ref != LUA_NOREF && prepared->entries[i].msg_ref != 0) {
			luaL_unref(logger->L, LUA_REGISTRYINDEX, prepared->entries[i].msg_ref);
		}
		for (ref_index = 0u; ref_index < prepared->entries[i].field_ref_count; ++ref_index) {
			if (prepared->entries[i].field_refs != NULL &&
			    prepared->entries[i].field_refs[ref_index] != LUA_NOREF &&
			    prepared->entries[i].field_refs[ref_index] != 0) {
				luaL_unref(logger->L, LUA_REGISTRYINDEX, prepared->entries[i].field_refs[ref_index]);
			}
		}
		free(prepared->entries[i].field_refs);
	}
	free(prepared->entries);
	free(prepared);
	return NULL;
}

static void gobench_lua_prepared_data_destroy(gobench_lua_prepared_data *prepared) {
	size_t i;

	if (prepared == NULL) {
		return;
	}
	if (prepared->logger != NULL && prepared->logger->L != NULL && prepared->entries != NULL) {
		for (i = 0u; i < prepared->entry_count; ++i) {
			size_t ref_index;

			if (prepared->entries[i].msg_ref != LUA_NOREF && prepared->entries[i].msg_ref != 0) {
				luaL_unref(prepared->logger->L, LUA_REGISTRYINDEX, prepared->entries[i].msg_ref);
			}
			for (ref_index = 0u; ref_index < prepared->entries[i].field_ref_count; ++ref_index) {
				if (prepared->entries[i].field_refs != NULL &&
				    prepared->entries[i].field_refs[ref_index] != LUA_NOREF &&
				    prepared->entries[i].field_refs[ref_index] != 0) {
					luaL_unref(prepared->logger->L, LUA_REGISTRYINDEX, prepared->entries[i].field_refs[ref_index]);
				}
			}
			free(prepared->entries[i].field_refs);
		}
	}
	free(prepared->entries);
	free(prepared);
}

static gobench_lua_prepared_table_data *gobench_lua_prepare_table_data(
    gobench_lua_logger *logger, const gobench_lua_entry *entries,
    size_t entry_count) {
	gobench_lua_prepared_table_data *prepared;
	size_t i;
	size_t j;

	if (logger == NULL || logger->L == NULL || entries == NULL || entry_count == 0u) {
		return NULL;
	}

	prepared = (gobench_lua_prepared_table_data *)calloc(1u, sizeof(*prepared));
	if (prepared == NULL) {
		gobench_lua_set_error(logger, "allocate prepared lua table data");
		return NULL;
	}
	prepared->entries = (gobench_lua_prepared_table_entry *)calloc(entry_count, sizeof(*prepared->entries));
	if (prepared->entries == NULL) {
		free(prepared);
		gobench_lua_set_error(logger, "allocate prepared lua table entries");
		return NULL;
	}
	prepared->logger = logger;
	prepared->entry_count = entry_count;

	for (i = 0u; i < entry_count; ++i) {
		prepared->entries[i].level = entries[i].level;
		if (gobench_lua_push_string(logger->L, entries[i].msg) != 0) {
			gobench_lua_set_error(logger, "prepare table message");
			lua_settop(logger->L, 0);
			goto fail;
		}
		prepared->entries[i].msg_ref = luaL_ref(logger->L, LUA_REGISTRYINDEX);

		lua_createtable(logger->L, 0, (int)entries[i].field_count);
		for (j = 0u; j < entries[i].field_count; ++j) {
			lua_pushstring(logger->L, entries[i].fields[j].key != NULL ? entries[i].fields[j].key : "");
			if (gobench_lua_push_field_value(logger->L, logger->pslog_ref, &entries[i].fields[j]) != 0) {
				gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
				lua_settop(logger->L, 0);
				goto fail;
			}
			lua_settable(logger->L, -3);
		}
		prepared->entries[i].fields_ref = luaL_ref(logger->L, LUA_REGISTRYINDEX);
	}

	return prepared;

fail:
	for (i = 0u; i < entry_count; ++i) {
		if (prepared->entries[i].msg_ref != LUA_NOREF && prepared->entries[i].msg_ref != 0) {
			luaL_unref(logger->L, LUA_REGISTRYINDEX, prepared->entries[i].msg_ref);
		}
		if (prepared->entries[i].fields_ref != LUA_NOREF && prepared->entries[i].fields_ref != 0) {
			luaL_unref(logger->L, LUA_REGISTRYINDEX, prepared->entries[i].fields_ref);
		}
	}
	free(prepared->entries);
	free(prepared);
	return NULL;
}

static void gobench_lua_prepared_table_data_destroy(gobench_lua_prepared_table_data *prepared) {
	size_t i;

	if (prepared == NULL) {
		return;
	}
	if (prepared->logger != NULL && prepared->logger->L != NULL && prepared->entries != NULL) {
		for (i = 0u; i < prepared->entry_count; ++i) {
			if (prepared->entries[i].msg_ref != LUA_NOREF && prepared->entries[i].msg_ref != 0) {
				luaL_unref(prepared->logger->L, LUA_REGISTRYINDEX, prepared->entries[i].msg_ref);
			}
			if (prepared->entries[i].fields_ref != LUA_NOREF && prepared->entries[i].fields_ref != 0) {
				luaL_unref(prepared->logger->L, LUA_REGISTRYINDEX, prepared->entries[i].fields_ref);
			}
		}
	}
	free(prepared->entries);
	free(prepared);
}

static int gobench_lua_logger_run(gobench_lua_logger *logger,
                                  const gobench_lua_entry *entries,
                                  size_t entry_count,
                                  size_t iterations,
                                  gobench_lua_result *result) {
	unsigned long long start_ns;
	unsigned long long end_ns;
	size_t i;

	if (logger == NULL || logger->L == NULL || entries == NULL || entry_count == 0u || result == NULL) {
		return -1;
	}

	gobench_lua_stats_reset(logger);
	gobench_lua_set_error(logger, NULL);
	start_ns = gobench_lua_now_ns();
	for (i = 0u; i < iterations; ++i) {
		const gobench_lua_entry *entry = &entries[i % entry_count];
		size_t j;
		int nargs;
		int method_ref = gobench_lua_method_ref_for_level(logger, entry->level);

		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, method_ref);
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
		lua_pushstring(logger->L, entry->msg != NULL ? entry->msg : "");
		nargs = 2;
		if (method_ref == logger->log_method_ref) {
			lua_pushinteger(logger->L, (lua_Integer)entry->level);
			lua_insert(logger->L, -2);
			nargs += 1;
		}
		for (j = 0u; j < entry->field_count; ++j) {
			lua_pushstring(logger->L, entry->fields[j].key != NULL ? entry->fields[j].key : "");
			if (gobench_lua_push_field_value(logger->L, logger->pslog_ref, &entry->fields[j]) != 0) {
				gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
				lua_settop(logger->L, 0);
				return -1;
			}
			nargs += 2;
		}
		if (lua_pcall(logger->L, nargs, 0, 0) != LUA_OK) {
			gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
			lua_pop(logger->L, 1);
			return -1;
		}
	}
	end_ns = gobench_lua_now_ns();
	result->elapsed_ns = end_ns - start_ns;
	gobench_lua_stats_read(logger, &result->bytes, &result->writes);
	result->ops = iterations;
	return 0;
}

static int gobench_lua_logger_run_prepared(gobench_lua_logger *logger,
                                           const gobench_lua_prepared_data *prepared,
                                           size_t iterations,
                                           gobench_lua_result *result) {
	unsigned long long start_ns;
	unsigned long long end_ns;
	size_t i;

	if (logger == NULL || logger->L == NULL || prepared == NULL ||
	    prepared->entries == NULL || prepared->entry_count == 0u || result == NULL) {
		return -1;
	}

	gobench_lua_stats_reset(logger);
	gobench_lua_set_error(logger, NULL);
	start_ns = gobench_lua_now_ns();
	for (i = 0u; i < iterations; ++i) {
		const gobench_lua_prepared_entry *entry = &prepared->entries[i % prepared->entry_count];
		size_t ref_index;
		int nargs;
		int method_ref = gobench_lua_method_ref_for_level(logger, entry->level);

		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, method_ref);
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
		nargs = 1;
		if (method_ref == logger->log_method_ref) {
			lua_pushinteger(logger->L, (lua_Integer)entry->level);
			nargs += 1;
		}
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, entry->msg_ref);
		nargs += 1;
		for (ref_index = 0u; ref_index < entry->field_ref_count; ++ref_index) {
			lua_rawgeti(logger->L, LUA_REGISTRYINDEX, entry->field_refs[ref_index]);
			nargs += 1;
		}
		if (lua_pcall(logger->L, nargs, 0, 0) != LUA_OK) {
			gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
			lua_pop(logger->L, 1);
			return -1;
		}
	}
	end_ns = gobench_lua_now_ns();
	result->elapsed_ns = end_ns - start_ns;
	gobench_lua_stats_read(logger, &result->bytes, &result->writes);
	result->ops = iterations;
	return 0;
}

static int gobench_lua_logger_run_prepared_table(
    gobench_lua_logger *logger,
    const gobench_lua_prepared_table_data *prepared,
    size_t iterations,
    gobench_lua_result *result) {
	unsigned long long start_ns;
	unsigned long long end_ns;
	size_t i;

	if (logger == NULL || logger->L == NULL || prepared == NULL ||
	    prepared->entries == NULL || prepared->entry_count == 0u || result == NULL) {
		return -1;
	}

	gobench_lua_stats_reset(logger);
	gobench_lua_set_error(logger, NULL);
	start_ns = gobench_lua_now_ns();
	for (i = 0u; i < iterations; ++i) {
		const gobench_lua_prepared_table_entry *entry = &prepared->entries[i % prepared->entry_count];
		int nargs;
		int method_ref = gobench_lua_method_ref_for_level(logger, entry->level);

		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, method_ref);
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
		nargs = 1;
		if (method_ref == logger->log_method_ref) {
			lua_pushinteger(logger->L, (lua_Integer)entry->level);
			nargs += 1;
		}
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, entry->msg_ref);
		lua_rawgeti(logger->L, LUA_REGISTRYINDEX, entry->fields_ref);
		nargs += 2;
		if (lua_pcall(logger->L, nargs, 0, 0) != LUA_OK) {
			gobench_lua_set_error(logger, lua_tostring(logger->L, -1));
			lua_pop(logger->L, 1);
			return -1;
		}
	}
	end_ns = gobench_lua_now_ns();
	result->elapsed_ns = end_ns - start_ns;
	gobench_lua_stats_read(logger, &result->bytes, &result->writes);
	result->ops = iterations;
	return 0;
}

static const char *gobench_lua_logger_last_error(gobench_lua_logger *logger) {
	if (logger == NULL || logger->last_error[0] == '\0') {
		return NULL;
	}
	return logger->last_error;
}

static void gobench_lua_logger_destroy(gobench_lua_logger *logger) {
	if (logger == NULL) {
		return;
	}
	if (logger->L != NULL) {
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->trace_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->debug_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->info_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->warn_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->error_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->log_method_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->pslog_ref);
		luaL_unref(logger->L, LUA_REGISTRYINDEX, logger->logger_ref);
		lua_close(logger->L);
		logger->L = NULL;
	}
	free(logger);
}
*/
import "C"

import (
	"fmt"
	"os"
	"path/filepath"
	"runtime"
	"time"
	"unsafe"
)

type LuaData struct {
	entries    *C.gobench_lua_entry
	fields     *C.gobench_lua_field
	entryCount int
	fieldCount int
	strings    []*C.char
}

type LuaLogger struct {
	ptr *C.gobench_lua_logger
}

type LuaPreparedData struct {
	ptr    *C.gobench_lua_prepared_data
	logger *LuaLogger
}

type LuaPreparedTableData struct {
	ptr    *C.gobench_lua_prepared_table_data
	logger *LuaLogger
}

type LuaResult struct {
	Elapsed      time.Duration
	BytesWritten uint64
	Writes       uint64
	Ops          uint64
}

func luaRockTree() string {
	_, file, _, ok := runtime.Caller(0)
	if !ok {
		return ""
	}
	return filepath.Clean(filepath.Join(filepath.Dir(file), "..", "..", "build", "luarocks"))
}

func HasLuaPSLog() bool {
	rockTree := luaRockTree()
	if rockTree == "" {
		return false
	}
	if _, err := os.Stat(filepath.Join(rockTree, "share", "lua", "5.5", "pslog", "init.lua")); err != nil {
		return false
	}
	return true
}

func luaComparisonNames() []string {
	if !HasLuaPSLog() {
		return nil
	}
	return []string{"jsonLua"}
}

func cLuaKind(kind FieldKind) C.gobench_lua_field_kind {
	switch kind {
	case FieldString:
		return C.GOBENCH_LUA_FIELD_STRING
	case FieldBool:
		return C.GOBENCH_LUA_FIELD_BOOL
	case FieldInt64:
		return C.GOBENCH_LUA_FIELD_INT64
	case FieldUint64:
		return C.GOBENCH_LUA_FIELD_UINT64
	case FieldFloat64:
		return C.GOBENCH_LUA_FIELD_FLOAT64
	default:
		return C.GOBENCH_LUA_FIELD_NULL
	}
}

func NewLuaData(entries []Entry) *LuaData {
	if len(entries) == 0 {
		return &LuaData{}
	}

	totalFields := 0
	for _, entry := range entries {
		totalFields += len(entry.Fields)
	}

	data := &LuaData{
		entryCount: len(entries),
		fieldCount: totalFields,
	}
	data.entries = (*C.gobench_lua_entry)(C.calloc(C.size_t(len(entries)), C.size_t(C.sizeof_gobench_lua_entry)))
	if totalFields > 0 {
		data.fields = (*C.gobench_lua_field)(C.calloc(C.size_t(totalFields), C.size_t(C.sizeof_gobench_lua_field)))
	}
	if data.entries == nil || (totalFields > 0 && data.fields == nil) {
		data.Close()
		return nil
	}

	entrySlice := unsafe.Slice(data.entries, len(entries))
	fieldSlice := unsafe.Slice(data.fields, totalFields)
	fieldOffset := 0
	for i, entry := range entries {
		msg := C.CString(entry.Message)
		data.strings = append(data.strings, msg)
		entrySlice[i].level = C.int(entry.Level)
		entrySlice[i].msg = msg
		entrySlice[i].field_count = C.size_t(len(entry.Fields))
		if len(entry.Fields) > 0 {
			entrySlice[i].fields = &fieldSlice[fieldOffset]
		}
		for _, field := range entry.Fields {
			dst := &fieldSlice[fieldOffset]
			key := C.CString(field.Key)
			data.strings = append(data.strings, key)
			dst.key = key
			dst.kind = cLuaKind(field.Kind)
			switch field.Kind {
			case FieldString:
				value := C.CString(field.String)
				data.strings = append(data.strings, value)
				dst.string_value = value
			case FieldBool:
				if field.Bool {
					dst.bool_value = 1
				} else {
					dst.bool_value = 0
				}
			case FieldInt64:
				dst.int64_value = C.longlong(field.Int64)
			case FieldUint64:
				dst.uint64_value = C.ulonglong(field.Uint64)
			case FieldFloat64:
				dst.float64_value = C.double(field.Float64)
			}
			fieldOffset++
		}
	}

	return data
}

func (d *LuaData) Close() {
	if d == nil {
		return
	}
	for _, text := range d.strings {
		C.free(unsafe.Pointer(text))
	}
	d.strings = nil
	if d.fields != nil {
		C.free(unsafe.Pointer(d.fields))
		d.fields = nil
	}
	if d.entries != nil {
		C.free(unsafe.Pointer(d.entries))
		d.entries = nil
	}
	d.entryCount = 0
	d.fieldCount = 0
}

func NewLuaLogger(staticFields []Field, timestamps bool) (*LuaLogger, error) {
	var staticData *LuaData
	var staticFieldsPtr *C.gobench_lua_field
	var staticCount C.size_t

	if !HasLuaPSLog() {
		return nil, fmt.Errorf("lua pslog benchmark support is not available")
	}
	if len(staticFields) > 0 {
		staticEntries := []Entry{{Fields: staticFields}}
		staticData = NewLuaData(staticEntries)
		if staticData == nil || staticData.fields == nil {
			if staticData != nil {
				staticData.Close()
			}
			return nil, fmt.Errorf("allocate lua static fields")
		}
		defer staticData.Close()
		staticFieldsPtr = staticData.fields
		staticCount = C.size_t(len(staticFields))
	}

	rockTreeC := C.CString(luaRockTree())
	defer C.free(unsafe.Pointer(rockTreeC))

	ptr := C.gobench_lua_logger_new(rockTreeC, staticFieldsPtr, staticCount, boolToInt(timestamps))
	if ptr == nil {
		return nil, fmt.Errorf("create Lua benchmark logger")
	}
	return &LuaLogger{ptr: ptr}, nil
}

func RunLua(logger *LuaLogger, data *LuaData, iterations int) (LuaResult, error) {
	var result C.gobench_lua_result

	if logger == nil || logger.ptr == nil {
		return LuaResult{}, fmt.Errorf("lua benchmark logger not initialized")
	}
	if data == nil || data.entries == nil || data.entryCount == 0 {
		return LuaResult{}, fmt.Errorf("lua benchmark data not initialized")
	}
	if iterations < 0 {
		iterations = 0
	}
	if rc := C.gobench_lua_logger_run(logger.ptr, data.entries, C.size_t(data.entryCount), C.size_t(iterations), &result); rc != 0 {
		if detail := C.gobench_lua_logger_last_error(logger.ptr); detail != nil {
			return LuaResult{}, fmt.Errorf("lua benchmark run failed: %s", C.GoString(detail))
		}
		return LuaResult{}, fmt.Errorf("lua benchmark run failed")
	}
	return LuaResult{
		Elapsed:      time.Duration(result.elapsed_ns),
		BytesWritten: uint64(result.bytes),
		Writes:       uint64(result.writes),
		Ops:          uint64(result.ops),
	}, nil
}

func PrepareLuaData(logger *LuaLogger, data *LuaData) (*LuaPreparedData, error) {
	if logger == nil || logger.ptr == nil {
		return nil, fmt.Errorf("lua benchmark logger not initialized")
	}
	if data == nil || data.entries == nil || data.entryCount == 0 {
		return nil, fmt.Errorf("lua benchmark data not initialized")
	}

	ptr := C.gobench_lua_prepare_data(logger.ptr, data.entries, C.size_t(data.entryCount))
	if ptr == nil {
		if detail := C.gobench_lua_logger_last_error(logger.ptr); detail != nil {
			return nil, fmt.Errorf("prepare lua benchmark data failed: %s", C.GoString(detail))
		}
		return nil, fmt.Errorf("prepare lua benchmark data failed")
	}

	return &LuaPreparedData{ptr: ptr, logger: logger}, nil
}

func RunLuaPrepared(logger *LuaLogger, prepared *LuaPreparedData, iterations int) (LuaResult, error) {
	var result C.gobench_lua_result

	if logger == nil || logger.ptr == nil {
		return LuaResult{}, fmt.Errorf("lua benchmark logger not initialized")
	}
	if prepared == nil || prepared.ptr == nil {
		return LuaResult{}, fmt.Errorf("lua prepared benchmark data not initialized")
	}
	if prepared.logger != logger {
		return LuaResult{}, fmt.Errorf("lua prepared benchmark data belongs to a different logger")
	}
	if iterations < 0 {
		iterations = 0
	}

	if rc := C.gobench_lua_logger_run_prepared(logger.ptr, prepared.ptr, C.size_t(iterations), &result); rc != 0 {
		if detail := C.gobench_lua_logger_last_error(logger.ptr); detail != nil {
			return LuaResult{}, fmt.Errorf("lua prepared benchmark run failed: %s", C.GoString(detail))
		}
		return LuaResult{}, fmt.Errorf("lua prepared benchmark run failed")
	}

	return LuaResult{
		Elapsed:      time.Duration(result.elapsed_ns),
		BytesWritten: uint64(result.bytes),
		Writes:       uint64(result.writes),
		Ops:          uint64(result.ops),
	}, nil
}

func PrepareLuaTableData(logger *LuaLogger, data *LuaData) (*LuaPreparedTableData, error) {
	if logger == nil || logger.ptr == nil {
		return nil, fmt.Errorf("lua benchmark logger not initialized")
	}
	if data == nil || data.entries == nil || data.entryCount == 0 {
		return nil, fmt.Errorf("lua benchmark data not initialized")
	}

	ptr := C.gobench_lua_prepare_table_data(logger.ptr, data.entries, C.size_t(data.entryCount))
	if ptr == nil {
		if detail := C.gobench_lua_logger_last_error(logger.ptr); detail != nil {
			return nil, fmt.Errorf("prepare lua table benchmark data failed: %s", C.GoString(detail))
		}
		return nil, fmt.Errorf("prepare lua table benchmark data failed")
	}

	return &LuaPreparedTableData{ptr: ptr, logger: logger}, nil
}

func RunLuaPreparedTable(logger *LuaLogger, prepared *LuaPreparedTableData, iterations int) (LuaResult, error) {
	var result C.gobench_lua_result

	if logger == nil || logger.ptr == nil {
		return LuaResult{}, fmt.Errorf("lua benchmark logger not initialized")
	}
	if prepared == nil || prepared.ptr == nil {
		return LuaResult{}, fmt.Errorf("lua prepared table benchmark data not initialized")
	}
	if prepared.logger != logger {
		return LuaResult{}, fmt.Errorf("lua prepared table benchmark data belongs to a different logger")
	}
	if iterations < 0 {
		iterations = 0
	}

	if rc := C.gobench_lua_logger_run_prepared_table(logger.ptr, prepared.ptr, C.size_t(iterations), &result); rc != 0 {
		if detail := C.gobench_lua_logger_last_error(logger.ptr); detail != nil {
			return LuaResult{}, fmt.Errorf("lua prepared table benchmark run failed: %s", C.GoString(detail))
		}
		return LuaResult{}, fmt.Errorf("lua prepared table benchmark run failed")
	}

	return LuaResult{
		Elapsed:      time.Duration(result.elapsed_ns),
		BytesWritten: uint64(result.bytes),
		Writes:       uint64(result.writes),
		Ops:          uint64(result.ops),
	}, nil
}

func (p *LuaPreparedData) Close() {
	if p != nil && p.ptr != nil {
		C.gobench_lua_prepared_data_destroy(p.ptr)
		p.ptr = nil
		p.logger = nil
	}
}

func (p *LuaPreparedTableData) Close() {
	if p != nil && p.ptr != nil {
		C.gobench_lua_prepared_table_data_destroy(p.ptr)
		p.ptr = nil
		p.logger = nil
	}
}

func (l *LuaLogger) Close() {
	if l != nil && l.ptr != nil {
		C.gobench_lua_logger_destroy(l.ptr)
		l.ptr = nil
	}
}

func boolToInt(v bool) C.int {
	if v {
		return 1
	}
	return 0
}
