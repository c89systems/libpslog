#include "pslog_lua.h"

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST_ASSERT(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "assertion failed: %s (%s:%d)\n", #expr, __FILE__,       \
              __LINE__);                                                       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

extern int luaopen_pslog_core(lua_State *L);

static int preload_pslog_core(lua_State *L) {
  luaL_requiref(L, "pslog.core", luaopen_pslog_core, 1);
  lua_pop(L, 1);
  return 0;
}

static int run_chunk(lua_State *L, const char *chunk) {
  if (luaL_dostring(L, chunk) != LUA_OK) {
    fprintf(stderr, "lua error: %s\n", lua_tostring(L, -1));
    lua_pop(L, 1);
    return 1;
  }
  return 0;
}

static int test_borrowed_logger_writes(lua_State *L) {
  pslog_lua_logger_view view;

  TEST_ASSERT(run_chunk(L,
                        "sink = {}\n"
                        "log = pslog.new({ output = function(chunk) "
                        "sink[#sink + 1] = chunk end, mode = 'json', "
                        "disable_timestamp = true, no_color = true })") == 0);
  lua_getglobal(L, "log");
  TEST_ASSERT(pslog_lua_is_logger(L, -1));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
  TEST_ASSERT(view.logger != NULL);
  pslog_info(view.logger, "borrowed", NULL, 0u);
  lua_pop(L, 1);
  TEST_ASSERT(
      run_chunk(L,
                "payload = table.concat(sink)\n"
                "assert(payload:find('\"msg\":\"borrowed\"', 1, true))") == 0);
  return 0;
}

static int test_borrowed_logger_streams_callback_chunks(lua_State *L) {
  pslog_lua_logger_view view;
  char message[128];
  size_t i;

  for (i = 0u; i + 1u < sizeof(message); ++i) {
    message[i] = (char)('a' + (i % 26u));
  }
  message[sizeof(message) - 1u] = '\0';

  TEST_ASSERT(run_chunk(L, "sink = {}\n"
                           "log = pslog.new({ output = function(chunk) "
                           "sink[#sink + 1] = chunk end, mode = 'json', "
                           "disable_timestamp = true, no_color = true, "
                           "line_buffer_capacity = 16 })") == 0);
  lua_getglobal(L, "log");
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
  TEST_ASSERT(view.logger != NULL);
  pslog_info(view.logger, message, NULL, 0u);
  lua_pop(L, 1);
  TEST_ASSERT(
      run_chunk(
          L,
          "payload = table.concat(sink)\n"
          "assert(#sink > 1)\n"
          "assert(payload:find('abcdefghijklmnopqrstuvwxyz', 1, true))") == 0);
  return 0;
}

static int test_borrowed_logger_view_survives_lua_close(lua_State *L) {
  pslog_lua_logger_view view;
  pslog_logger *borrowed;

  TEST_ASSERT(run_chunk(L,
                        "sink = {}\n"
                        "log = pslog.new({ output = function(chunk) "
                        "sink[#sink + 1] = chunk end, mode = 'json', "
                        "disable_timestamp = true, no_color = true })") == 0);
  lua_getglobal(L, "log");
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
  TEST_ASSERT(view.logger != NULL);
  borrowed = view.logger;
  lua_pop(L, 1);

  TEST_ASSERT(run_chunk(L, "assert(log:close())") == 0);
  lua_getglobal(L, "log");
  TEST_ASSERT(!pslog_lua_is_logger(L, -1));
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) != 0);
  lua_pop(L, 1);

  pslog_info(borrowed, "borrowed_after_close", NULL, 0u);
  return 0;
}

static int test_ref_logger_derives_and_retains(lua_State *L) {
  pslog_lua_logger_ref ref;
  pslog_field field = pslog_str("service", "embedder");

  TEST_ASSERT(run_chunk(L,
                        "sink = {}\n"
                        "log = pslog.new({ output = function(chunk) "
                        "sink[#sink + 1] = chunk end, mode = 'json', "
                        "disable_timestamp = true, no_color = true })") == 0);
  lua_getglobal(L, "log");
  ref.size = sizeof(ref);
  ref.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_ref_logger(L, -1, &field, 1u, &ref) == 0);
  TEST_ASSERT(ref.borrowed != NULL);
  TEST_ASSERT(ref.derived != NULL);
  lua_pop(L, 1);
  TEST_ASSERT(run_chunk(L, "log = nil; collectgarbage('collect')") == 0);
  pslog_info(ref.derived, "derived", NULL, 0u);
  pslog_lua_unref_logger(&ref);
  TEST_ASSERT(ref.borrowed == NULL);
  TEST_ASSERT(ref.derived == NULL);
  TEST_ASSERT(
      run_chunk(
          L, "payload = table.concat(sink)\n"
             "assert(payload:find('\"msg\":\"derived\"', 1, true))\n"
             "assert(payload:find('\"service\":\"embedder\"', 1, true))") == 0);
  return 0;
}

static int test_ref_logger_survives_lua_close(lua_State *L) {
  pslog_lua_logger_ref ref;

  TEST_ASSERT(run_chunk(L,
                        "sink = {}\n"
                        "log = pslog.new({ output = function(chunk) "
                        "sink[#sink + 1] = chunk end, mode = 'json', "
                        "disable_timestamp = true, no_color = true })") == 0);
  lua_getglobal(L, "log");
  ref.size = sizeof(ref);
  ref.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_ref_logger(L, -1, NULL, 0u, &ref) == 0);
  TEST_ASSERT(ref.borrowed != NULL);
  TEST_ASSERT(ref.derived == NULL);
  lua_pop(L, 1);

  TEST_ASSERT(run_chunk(L, "assert(log:close())") == 0);
  lua_getglobal(L, "log");
  TEST_ASSERT(!pslog_lua_is_logger(L, -1));
  lua_pop(L, 1);

  pslog_info(ref.borrowed, "after_close", NULL, 0u);
  pslog_lua_unref_logger(&ref);
  TEST_ASSERT(ref.borrowed == NULL);
  return 0;
}

static int test_negative_paths(lua_State *L) {
  pslog_lua_logger_view view;
  pslog_lua_logger_ref ref;
  int status;

  lua_pushinteger(L, 42);
  TEST_ASSERT(!pslog_lua_is_logger(L, -1));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) != 0);
  lua_pop(L, 1);

  TEST_ASSERT(run_chunk(L, "log = pslog.new({ output = function(_) end, "
                           "mode = 'json', disable_timestamp = true })") == 0);
  lua_getglobal(L, "log");
  view.size = sizeof(view) - 1u;
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  status = pslog_lua_check_logger(L, -1, &view);
  TEST_ASSERT(status != 0);
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION + 1u;
  status = pslog_lua_check_logger(L, -1, &view);
  TEST_ASSERT(status != 0);
  ref.size = sizeof(ref);
  ref.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_ref_logger(L, -1, NULL, 1u, &ref) != 0);
  lua_pop(L, 1);
  TEST_ASSERT(run_chunk(L, "log = pslog.new({ output = function(_) end, "
                           "mode = 'json', disable_timestamp = true })\n"
                           "assert(log:close())") == 0);
  lua_getglobal(L, "log");
  TEST_ASSERT(!pslog_lua_is_logger(L, -1));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) != 0);
  lua_pop(L, 1);
  return 0;
}

static int test_multi_logger_views(lua_State *L) {
  pslog_lua_logger_view first;
  pslog_lua_logger_view second;

  TEST_ASSERT(run_chunk(L, "a = pslog.new({ output = function(_) end, "
                           "mode = 'json', disable_timestamp = true })\n"
                           "b = pslog.new({ output = function(_) end, "
                           "mode = 'json', disable_timestamp = true })") == 0);
  lua_getglobal(L, "a");
  first.size = sizeof(first);
  first.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &first) == 0);
  lua_pop(L, 1);
  lua_getglobal(L, "b");
  second.size = sizeof(second);
  second.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &second) == 0);
  lua_pop(L, 1);
  TEST_ASSERT(first.logger != NULL);
  TEST_ASSERT(second.logger != NULL);
  TEST_ASSERT(first.logger != second.logger);
  return 0;
}

static void init_view(pslog_lua_logger_view *view) {
  memset(view, 0, sizeof(*view));
  view->size = sizeof(*view);
  view->abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
}

static void init_ref(pslog_lua_logger_ref *ref) {
  memset(ref, 0, sizeof(*ref));
  ref->size = sizeof(*ref);
  ref->abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  ref->registry_ref = LUA_NOREF;
}

/* A facade rejects affinity before any backend registration or retention. */
static int retain_native(lua_State *L, int index, const pslog_field *fields,
                         size_t count, pslog_lua_logger_ref *ref) {
  pslog_lua_logger_view view;
  int status;
  init_view(&view);
  status = pslog_lua_check_logger(L, index, &view);
  if (status != 0) {
    return status;
  }
  if ((view.flags & PSLOG_LUA_LOGGER_NATIVE_SINK) == 0u) {
    return EINVAL;
  }
  return pslog_lua_ref_logger(L, index, fields, count, ref);
}

static int test_native_classification(lua_State *L) {
  const char *outputs[] = {
      "nil",
      "'stdout'",
      "'stderr'",
      "io.stdout",
      "io.stderr",
      "'build/lua-interop-capability.log'",
      "function(_) callback_calls = callback_calls + 1 end"};
  const char *methods[] = {":with()", ":with('child', true)",
                           ":with_level('debug')", ":with_level_field()"};
  size_t i, j, fields;
  char chunk[1024];
  pslog_field field = pslog_str("service", "facade");
  for (i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
    for (j = 0; j < sizeof(methods) / sizeof(methods[0]); ++j) {
      pslog_lua_logger_view view;
      int native = i + 1 < sizeof(outputs) / sizeof(outputs[0]);
      sprintf(chunk,
              "callback_calls = 0; local parent = pslog.new_json(%s, "
              "{disable_timestamp=true}); log = parent%s; "
              "parent=nil; collectgarbage('collect')",
              outputs[i], methods[j]);
      TEST_ASSERT(run_chunk(L, chunk) == 0);
      lua_getglobal(L, "log");
      init_view(&view);
      TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
      TEST_ASSERT(view.flags == (native ? PSLOG_LUA_LOGGER_NATIVE_SINK : 0u));
      for (fields = 0; fields < 2; ++fields) {
        pslog_lua_logger_ref ref;
        int top = lua_gettop(L);
        init_ref(&ref);
        TEST_ASSERT(retain_native(L, -1, &field, fields, &ref) ==
                    (native ? 0 : EINVAL));
        TEST_ASSERT(lua_gettop(L) == top);
        if (native) {
          TEST_ASSERT(pslog_has_native_output(ref.derived ? ref.derived
                                                          : ref.borrowed));
        } else {
          TEST_ASSERT(ref.L == NULL && ref.borrowed == NULL &&
                      ref.derived == NULL);
          TEST_ASSERT(ref.registry_ref == LUA_NOREF);
          /* Lua-thread retention is still legal for function outputs. */
          TEST_ASSERT(pslog_lua_ref_logger(L, -1, &field, fields, &ref) == 0);
          TEST_ASSERT(!pslog_has_native_output(ref.derived ? ref.derived
                                                           : ref.borrowed));
        }
        pslog_lua_unref_logger(&ref);
        pslog_lua_unref_logger(&ref);
      }
      lua_pop(L, 1);
      TEST_ASSERT(run_chunk(L, "assert(callback_calls == 0); log=nil; "
                               "collectgarbage('collect')") == 0);
    }
  }
  return 0;
}

static int test_environment_classification(lua_State *L) {
  const char *outputs[] = {"default",
                           "stdout",
                           "stderr",
                           "build/lua-interop-env.log",
                           "stdout+build/lua-interop-env.log",
                           "stderr+build/lua-interop-env.log",
                           "default+build/lua-interop-env.log",
                           "build/nonexistent-interop-dir/log"};
  size_t i, callback;
  for (callback = 0; callback < 2; ++callback) {
    for (i = 0; i < sizeof(outputs) / sizeof(outputs[0]); ++i) {
      pslog_lua_logger_view view;
      int native = !callback || (i > 0 && i < 6);
      TEST_ASSERT(setenv("PSLOG_INTEROP_TEST_OUTPUT", outputs[i], 1) == 0);
      TEST_ASSERT(
          run_chunk(
              L, callback ? "log=pslog.from_env('PSLOG_INTEROP_TEST_', "
                            "{output=function(_) end, disable_timestamp=true})"
                          : "log=pslog.from_env('PSLOG_INTEROP_TEST_', "
                            "{output='stdout', disable_timestamp=true})") == 0);
      lua_getglobal(L, "log");
      init_view(&view);
      TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
      TEST_ASSERT(view.flags == (native ? PSLOG_LUA_LOGGER_NATIVE_SINK : 0u));
      lua_pop(L, 1);
      TEST_ASSERT(
          run_chunk(
              L, "assert(log:close()); log=nil; collectgarbage('collect')") ==
          0);
    }
  }
  TEST_ASSERT(unsetenv("PSLOG_INTEROP_TEST_OUTPUT") == 0);
  return 0;
}

static int unknown_write(void *userdata, const char *data, size_t len,
                         size_t *written) {
  (void)userdata;
  (void)data;
  *written = len;
  return 0;
}
static int unknown_close(void *userdata) {
  (void)userdata;
  return 0;
}
static int unknown_isatty(void *userdata) {
  (void)userdata;
  return 0;
}
static void unknown_notify(void *userdata, const pslog_write_failure *failure) {
  (void)userdata;
  (void)failure;
}

static int test_core_wrappers(void) {
  pslog_config config;
  pslog_observed_output inner, outer;
  pslog_logger *log, *child;
  FILE *fp = tmpfile();
  size_t i;
  TEST_ASSERT(fp != NULL);
  TEST_ASSERT(!pslog_has_native_output(NULL));
  for (i = 0; i < 5; ++i) {
    pslog_default_config(&config);
    config.output = pslog_output_from_fp(fp, 0);
    if (i == 1)
      config.output.write = unknown_write;
    if (i == 2)
      config.output.close = unknown_close;
    if (i == 3)
      config.output.isatty = unknown_isatty;
    pslog_observed_output_init(&inner, &config.output,
                               i == 4 ? unknown_notify : NULL, NULL);
    pslog_observed_output_init(&outer, &inner.output, NULL, NULL);
    config.output = outer.output;
    TEST_ASSERT(setenv("PSLOG_INTEROP_TEST_OUTPUT",
                       "default+build/lua-interop-observed.log", 1) == 0);
    log = pslog_new_from_env("PSLOG_INTEROP_TEST_", &config);
    TEST_ASSERT(unsetenv("PSLOG_INTEROP_TEST_OUTPUT") == 0);
    TEST_ASSERT(log != NULL);
    child = log->with_level(log, PSLOG_LEVEL_DEBUG);
    TEST_ASSERT(child != NULL);
    log->destroy(log);
    TEST_ASSERT(pslog_has_native_output(child) == (i == 0));
    TEST_ASSERT(child->close(child) == 0);
    TEST_ASSERT(pslog_has_native_output(child) == (i == 0));
    child->destroy(child);
  }
  TEST_ASSERT(fclose(fp) == 0);
  return 0;
}

typedef struct worker_args {
  pslog_logger *log;
  int id;
} worker_args;
static void *native_worker(void *userdata) {
  worker_args *args = (worker_args *)userdata;
  pslog_field field = pslog_i64("worker", args->id);
  int i;
  for (i = 0; i < 200; ++i) {
    args->log->info(args->log, "native worker bounded chunk message", &field,
                    1u);
  }
  return NULL;
}

static int test_coroutine_native_workers(lua_State *L) {
  size_t fields;
  size_t owned;
  for (owned = 0; owned < 2; ++owned)
    for (fields = 0; fields < 2; ++fields) {
      pslog_lua_logger_ref ref;
      pslog_field field = pslog_str("service", "facade");
      pthread_t threads[4];
      worker_args args[4];
      lua_State *co;
      int i;
      co = lua_newthread(L);
      TEST_ASSERT(
          run_chunk(
              co, owned ? "os.remove('build/lua-interop-workers.log'); "
                          "log=pslog.new_json('build/lua-interop-workers.log', "
                          "{disable_timestamp=true, line_buffer_capacity=16}); "
                          "weak=setmetatable({log},{__mode='v'})"
                        : "file=io.tmpfile(); log=pslog.new_json(file, "
                          "{disable_timestamp=true, line_buffer_capacity=16}); "
                          "weak=setmetatable({log},{__mode='v'})") == 0);
      TEST_ASSERT(
          run_chunk(
              co, "weak_co=setmetatable({coroutine.running()},{__mode='v'})") ==
          0);
      lua_getglobal(co, "log");
      init_ref(&ref);
      TEST_ASSERT(retain_native(co, -1, &field, fields, &ref) == 0);
      TEST_ASSERT(ref.L == L);
      lua_pop(co, 1);
      lua_pop(L, 1);
      TEST_ASSERT(run_chunk(L,
                            "log=nil; collectgarbage('collect'); "
                            "assert(weak[1]); assert(weak_co[1]==nil)") == 0);
      for (i = 0; i < 4; ++i) {
        args[i].log = ref.derived ? ref.derived : ref.borrowed;
        args[i].id = i;
        TEST_ASSERT(
            pthread_create(&threads[i], NULL, native_worker, &args[i]) == 0);
      }
      /* Native writers run while the owning Lua thread forces collection. */
      TEST_ASSERT(run_chunk(L, "for i=1,20 do collectgarbage('collect') end") ==
                  0);
      for (i = 0; i < 4; ++i)
        TEST_ASSERT(pthread_join(threads[i], NULL) == 0);
      TEST_ASSERT(lua_gettop(L) == 0);
      pslog_lua_unref_logger(&ref);
      if (owned)
        TEST_ASSERT(
            run_chunk(
                L,
                "collectgarbage('collect'); "
                "file=assert(io.open('build/lua-interop-workers.log','r'))") ==
            0);
      TEST_ASSERT(
          run_chunk(
              L, "collectgarbage('collect'); assert(weak[1]==nil); "
                 "file:seek('set',0); local counts={0,0,0,0}; local total=0; "
                 "for line in file:lines() do "
                 "local id=line:match('^%{\"lvl\":\"info\",\"msg\":\"native "
                 "worker bounded chunk message\",.-\"worker\":([0-3])%}$'); "
                 "assert(id, line); "
                 "counts[tonumber(id)+1]=counts[tonumber(id)+1]+1; "
                 "total=total+1 end; "
                 "assert(total==800); for i=1,4 do assert(counts[i]==200) "
                 "end; file:close(); file=nil") == 0);
    }
  return 0;
}

static int test_native_explicit_close(lua_State *L) {
  size_t owned, fields;
  for (owned = 0; owned < 2; ++owned)
    for (fields = 0; fields < 2; ++fields) {
      pslog_lua_logger_ref ref;
      pslog_lua_logger_view view;
      pslog_field field = pslog_str("service", "facade");
      TEST_ASSERT(
          run_chunk(L, owned
                           ? "log=pslog.new_json('build/"
                             "lua-interop-close.log', {disable_timestamp=true})"
                           : "file=io.tmpfile(); log=pslog.new_json(file, "
                             "{disable_timestamp=true})") == 0);
      lua_getglobal(L, "log");
      init_ref(&ref);
      TEST_ASSERT(retain_native(L, -1, &field, fields, &ref) == 0);
      lua_pop(L, 1);
      TEST_ASSERT(run_chunk(L, "assert(log:close()); assert(log:close())") ==
                  0);
      lua_getglobal(L, "log");
      init_view(&view);
      TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == ENOENT);
      TEST_ASSERT(view.logger == NULL && view.flags == 0u);
      lua_pop(L, 1);
      (ref.derived ? ref.derived : ref.borrowed)
          ->info(ref.derived ? ref.derived : ref.borrowed, "after_close", NULL,
                 0u);
      TEST_ASSERT(pslog_has_native_output(ref.borrowed));
      TEST_ASSERT(run_chunk(L, "log=nil; collectgarbage('collect')") == 0);
      pslog_lua_unref_logger(&ref);
      TEST_ASSERT(run_chunk(L, "collectgarbage('collect')") == 0);
      if (!owned)
        TEST_ASSERT(run_chunk(L, "file:seek('set',0); "
                                 "assert(file:read('a'):find('after_close',1,"
                                 "true)); file:close(); file=nil") == 0);
    }
  return 0;
}

static int test_validation_contract(lua_State *L) {
  pslog_lua_logger_view view;
  pslog_lua_logger_ref ref;
  size_t kind;
  int top;
  TEST_ASSERT(run_chunk(L, "log=pslog.new_json('stdout')") == 0);
  for (kind = 0; kind < 7; ++kind) {
    lua_getglobal(L, "log");
    if (kind == 0) {
      lua_pop(L, 1);
      lua_pushinteger(L, 42);
    }
    if (kind == 1) {
      lua_pop(L, 1);
      lua_newuserdatauv(L, 8, 0);
    }
    init_view(&view);
    init_ref(&ref);
    if (kind == 2) {
      view.size--;
      ref.size--;
    }
    if (kind == 3) {
      view.abi_version++;
      ref.abi_version++;
    }
    top = lua_gettop(L);
    TEST_ASSERT(pslog_lua_check_logger(kind == 4 ? NULL : L, -1,
                                       kind == 5 ? NULL : &view) ==
                (kind < 2   ? ENOTTY
                 : kind < 4 ? EPROTO
                 : kind < 6 ? EINVAL
                            : 0));
    TEST_ASSERT(pslog_lua_ref_logger(
                    kind == 4 ? NULL : L, -1, NULL, kind == 6 ? 1u : 0u,
                    kind == 5 ? NULL : &ref) == (kind < 2   ? ENOTTY
                                                 : kind < 4 ? EPROTO
                                                            : EINVAL));
    TEST_ASSERT(lua_gettop(L) == top);
    TEST_ASSERT(ref.borrowed == NULL && ref.derived == NULL && ref.L == NULL);
    TEST_ASSERT(ref.registry_ref == LUA_NOREF);
    if (kind != 6)
      TEST_ASSERT(view.logger == NULL && view.flags == 0u);
    lua_pop(L, 1);
  }
  TEST_ASSERT(run_chunk(L, "assert(log:close())") == 0);
  lua_getglobal(L, "log");
  init_view(&view);
  init_ref(&ref);
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == ENOENT);
  TEST_ASSERT(pslog_lua_ref_logger(L, -1, NULL, 0u, &ref) == ENOENT);
  TEST_ASSERT(view.logger == NULL && view.flags == 0u && ref.L == NULL &&
              ref.borrowed == NULL);
  lua_pop(L, 1);
  return 0;
}

#ifdef PSLOG_TEST_HOOKS
/* Core test-only allocation injection never enters the public interop header.
 */
extern void pslog_test_allocator_fail_after(unsigned long calls);
extern void pslog_test_allocator_reset(void);
static int test_ref_derivation_failure(lua_State *L) {
  pslog_lua_logger_ref ref;
  pslog_field field = pslog_str("service", "facade");
  pslog_lua_logger_view view;
  int top, i;
  TEST_ASSERT(run_chunk(L, "log=pslog.new_json('stdout'); "
                           "weak=setmetatable({log},{__mode='v'})") == 0);
  lua_getglobal(L, "log");
  top = lua_gettop(L);
  init_ref(&ref);
  pslog_test_allocator_reset();
  pslog_test_allocator_fail_after(1u);
  for (i = 0; i < 1000; ++i) {
    init_view(&view);
    TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
    TEST_ASSERT(view.flags == PSLOG_LUA_LOGGER_NATIVE_SINK);
    TEST_ASSERT(pslog_has_native_output(view.logger));
  }
  TEST_ASSERT(pslog_lua_ref_logger(L, -1, &field, 1u, &ref) == ENOMEM);
  pslog_test_allocator_fail_after(0u);
  TEST_ASSERT(ref.L == NULL && ref.borrowed == NULL && ref.derived == NULL &&
              ref.registry_ref == LUA_NOREF);
  TEST_ASSERT(lua_gettop(L) == top);
  lua_pop(L, 1);
  TEST_ASSERT(
      run_chunk(
          L, "log=nil; collectgarbage('collect'); assert(weak[1]==nil)") == 0);
  return 0;
}
#endif

static int test_coroutine_function_sink(lua_State *L) {
  pslog_lua_logger_ref ref;
  pslog_lua_logger_view view;
  lua_State *co = lua_newthread(L);
  TEST_ASSERT(
      run_chunk(co,
                "sink={}; local owner=coroutine.running(); "
                "log=pslog.new_json(function(chunk) "
                "assert(coroutine.running()==owner); "
                "sink[#sink+1]=chunk end, {disable_timestamp=true}); "
                "log=log:with('child',true); collectgarbage('collect')") == 0);
  lua_getglobal(co, "log");
  init_view(&view);
  init_ref(&ref);
  TEST_ASSERT(pslog_lua_check_logger(co, -1, &view) == 0 && view.flags == 0u);
  TEST_ASSERT(pslog_lua_ref_logger(co, -1, NULL, 0u, &ref) == 0);
  TEST_ASSERT(ref.L == L);
  lua_pop(co, 1);
  lua_pop(L, 1);
  TEST_ASSERT(run_chunk(L, "log=nil; collectgarbage('collect')") == 0);
  pslog_info(ref.borrowed, "coroutine_callback", NULL, 0u);
  pslog_lua_unref_logger(&ref);
  TEST_ASSERT(
      run_chunk(
          L,
          "collectgarbage('collect'); "
          "assert(table.concat(sink):find('coroutine_callback',1,true))") == 0);
  return 0;
}

static int test_suspended_coroutine_collection(lua_State *L) {
  TEST_ASSERT(
      run_chunk(L,
                "local co=coroutine.create(function() "
                "local log=pslog.new_json(function(_) end); coroutine.yield(); "
                "log:info('unused') end); assert(coroutine.resume(co)); "
                "weak_co=setmetatable({co},{__mode='v'}); co=nil; "
                "collectgarbage('collect'); collectgarbage('collect'); "
                "assert(weak_co[1]==nil)") == 0);
  return 0;
}

typedef struct lua_allocator_guard {
  pthread_t owner;
  pthread_mutex_t mutex;
  unsigned int foreign_calls;
} lua_allocator_guard;

static void *guarded_lua_alloc(void *userdata, void *ptr, size_t old_size,
                               size_t new_size) {
  lua_allocator_guard *guard = (lua_allocator_guard *)userdata;
  (void)old_size;
  if (!pthread_equal(pthread_self(), guard->owner)) {
    pthread_mutex_lock(&guard->mutex);
    guard->foreign_calls++;
    pthread_mutex_unlock(&guard->mutex);
  }
  if (new_size == 0) {
    free(ptr);
    return NULL;
  }
  return realloc(ptr, new_size);
}

typedef struct lua_allocation_fault {
  int armed;
  size_t allowance;
  size_t live;
} lua_allocation_fault;

static void *fault_lua_alloc(void *userdata, void *ptr, size_t old_size,
                             size_t new_size) {
  lua_allocation_fault *fault = (lua_allocation_fault *)userdata;
  void *next;

  if (new_size == 0u) {
    if (ptr != NULL) {
      --fault->live;
    }
    free(ptr);
    return NULL;
  }
  if (fault->armed && (ptr == NULL || new_size > old_size)) {
    if (fault->allowance == 0u) {
      return NULL;
    }
    --fault->allowance;
  }
  next = realloc(ptr, new_size);
  if (next != NULL && ptr == NULL) {
    ++fault->live;
  }
  return next;
}

static int test_lua_adoption_failures(void) {
  static const char *const operations[] = {
      "value=p.new_logger(config)",
      "value=p.new_logger_from_env('PSLOG_OOM_TEST_',config)",
      "value=parent:with()",
      "value=parent:with('key','value')",
      "value=parent:with({key='value'})",
      "value=parent:with_level('warn')",
      "value=parent:with_level_field()"};
  static const char *const outputs[] = {"'/dev/null'", "function()end",
                                        "assert(io.tmpfile())"};
  size_t operation, output, budget;

  for (output = 0u; output < sizeof(outputs) / sizeof(outputs[0]); ++output) {
    for (operation = 0u; operation < sizeof(operations) / sizeof(operations[0]);
         ++operation) {
      int failures = 0, successes = 0;
      for (budget = 0u; budget < 24u; ++budget) {
        lua_allocation_fault fault = {0, budget, 0u};
        lua_State *L = lua_newstate(fault_lua_alloc, &fault, 0u);
        char setup[512];
        int status;

        TEST_ASSERT(L != NULL);
        luaL_openlibs(L);
        luaL_requiref(L, "pslog.core", luaopen_pslog_core, 0);
        lua_setglobal(L, "p");
        snprintf(setup, sizeof(setup),
                 "config={output=%s,no_color=true,disable_timestamp=true}; "
                 "parent=p.new_logger(config)",
                 outputs[output]);
        TEST_ASSERT(run_chunk(L, setup) == 0);
        TEST_ASSERT(luaL_loadstring(L, operations[operation]) == LUA_OK);
        fault.armed = 1;
        status = lua_pcall(L, 0, 0, 0);
        fault.armed = 0;
        TEST_ASSERT(status == LUA_OK || status == LUA_ERRMEM);
        failures += status == LUA_ERRMEM;
        successes += status == LUA_OK;
        lua_settop(L, 0);
        TEST_ASSERT(run_chunk(L, "if value then assert(value:close()) end; "
                                 "parent:info('healthy after fault'); "
                                 "assert(parent:close())") == 0);
        lua_close(L);
        TEST_ASSERT(fault.live == 0u);
      }
      TEST_ASSERT(failures > 0 && successes > 0);
    }
  }
  return 0;
}

static int test_callback_allocation_failure(void) {
  lua_allocation_fault fault = {0, 0u, 0u};
  lua_State *L = lua_newstate(fault_lua_alloc, &fault, 0u);
  pslog_lua_logger_view view;
  int top;

  TEST_ASSERT(L != NULL);
  luaL_openlibs(L);
  luaL_requiref(L, "pslog.core", luaopen_pslog_core, 0);
  lua_setglobal(L, "p");
  TEST_ASSERT(run_chunk(L, "calls=0; log=p.new_logger({no_color=true, "
                           "disable_timestamp=true, output=function() "
                           "calls=calls+1 end})") == 0);
  lua_getglobal(L, "log");
  init_view(&view);
  TEST_ASSERT(pslog_lua_check_logger(L, -1, &view) == 0);
  top = lua_gettop(L);
  /* No outer Lua protection exists here: callback setup itself must catch
   * allocation failure before it can unwind across the native output lock. */
  fault.armed = 1;
  pslog_info(view.logger, "callback chunk allocation failure", NULL, 0u);
  fault.armed = 0;
  TEST_ASSERT(lua_gettop(L) == top);
  TEST_ASSERT(run_chunk(L, "assert(calls==0); log:info('recovered'); "
                           "assert(calls>0); assert(log:close())") == 0);
  lua_close(L);
  TEST_ASSERT(fault.live == 0u);
  return 0;
}

static int test_callback_graph_and_reentry(lua_State *L) {
  TEST_ASSERT(
      run_chunk(L,
                "local weak=setmetatable({},{__mode='v'}); local calls=0; "
                "local root=pslog.new{disable_timestamp=true,output=function() "
                "calls=calls+1; collectgarbage('collect') end}; weak[1]=root; "
                "local child=root:with_level('info'); root=nil; "
                "collectgarbage('collect'); assert(weak[1]==nil); "
                "child:info('root collected'); assert(calls>0); "
                "assert(child:close())") == 0);
  TEST_ASSERT(
      run_chunk(
          L, "local weak=setmetatable({},{__mode='v'}); "
             "do local log; log=pslog.new{output=function() return log end}; "
             "weak[1]=log end; collectgarbage('collect'); "
             "collectgarbage('collect'); assert(weak[1]==nil); "
             "local parent,child; local calls,checks=0,0; "
             "parent=pslog.new{disable_timestamp=true,output=function() "
             "calls=calls+1; "
             "for _,log in ipairs({parent,child}) do "
             "for _,op in ipairs({function()log:info('nested')end, "
             "function()log:close()end,function()log:with()end, "
             "function()log:with_level('warn')end, "
             "function()log:with_level_field()end}) do "
             "local ok,err=pcall(op); assert(not ok and "
             "err:find('callback is active',1,true)); checks=checks+1 end end; "
             "collectgarbage('collect') end}; "
             "child=parent:with_level('info'); parent:info('outer'); "
             "child:info('healthy'); assert(calls>=2 and checks==calls*10); "
             "assert(parent:close()); "
             "assert(child:close())") == 0);
  TEST_ASSERT(
      run_chunk(
          L, "local weak=setmetatable({},{__mode='v'}); local child; "
             "do local file=assert(io.tmpfile()); weak[1]=file; "
             "child=pslog.new(file):with_level('info') end; "
             "collectgarbage('collect'); assert(weak[1]~=nil); "
             "child:info('file still open'); assert(child:close()); "
             "child=nil; collectgarbage('collect'); collectgarbage('collect'); "
             "assert(weak[1]==nil)") == 0);
  return 0;
}

static int test_field_conversion_reentry(lua_State *L) {
  TEST_ASSERT(
      run_chunk(
          L,
          "local chunks={}; local log=pslog.new_json(function(s) "
          "chunks[#chunks+1]=s end,{disable_timestamp=true}); "
          "local fields={}; for i=1,100 do fields['k'..i]=i end; "
          "local value=setmetatable({},{__tostring=function() "
          "log:info('nested conversion',fields); return 'unsafe' end}); "
          "for _,op in ipairs({function() log:info('outer','k',value) end, "
          "function() log:with('k',value) end}) do "
          "local ok,err=pcall(op); assert(not ok and "
          "err:find('field collection is active',1,true)) end; "
          "assert(#chunks==0); log:info('recovered','ok',true); "
          "local text=setmetatable({},{__tostring=function() "
          "return 'converted' end}); local many={}; "
          "for i=1,100 do many[i]=text end; log:info('temporaries',many); "
          "assert(table.concat(chunks):find('converted',1,true)); "
          "local marker={}; local bad=setmetatable({},{__tostring=function() "
          "error(marker) end}); local "
          "ok,err=pcall(log.info,log,'bad','k',bad); "
          "assert(not ok and err==marker); log:info('healthy'); "
          "assert(log:close())") == 0);
  return 0;
}

#ifdef PSLOG_TEST_HOOKS
static int count_close(void *userdata) {
  ++*(int *)userdata;
  return 0;
}

static int discard_write(void *userdata, const char *data, size_t len,
                         size_t *written) {
  (void)userdata;
  (void)data;
  *written = len;
  return 0;
}

static int test_native_constructor_rollback(void) {
  static const char *const outputs[] = {NULL, "default", "stderr",
                                        "build/pslog-oom-output.log",
                                        "default+build/pslog-oom-output.log"};
  size_t output;
  unsigned long failure;

  for (output = 0u; output < sizeof(outputs) / sizeof(outputs[0]); ++output) {
    int failures = 0, successes = 0;
    for (failure = 1u; failure < 16u; ++failure) {
      int closes = 0;
      pslog_config config;
      pslog_logger *log;

      pslog_default_config(&config);
      config.time_format = "%Y";
      config.output.write = discard_write;
      config.output.close = count_close;
      config.output.isatty = NULL;
      config.output.owned = 1;
      config.output.userdata = &closes;
      if (outputs[output] != NULL) {
        TEST_ASSERT(setenv("PSLOG_ROLLBACK_OUTPUT", outputs[output], 1) == 0);
      }
      pslog_test_allocator_reset();
      pslog_test_allocator_fail_after(failure);
      log = outputs[output] == NULL
                ? pslog_new(&config)
                : pslog_new_from_env("PSLOG_ROLLBACK_", &config);
      pslog_test_allocator_fail_after(0u);
      if (log == NULL) {
        ++failures;
        TEST_ASSERT(closes == 0);
        pslog_output_destroy(&config.output);
      } else {
        ++successes;
        TEST_ASSERT(pslog_close(log) == 0);
        TEST_ASSERT(closes == 1);
        log->destroy(log);
      }
      TEST_ASSERT(closes == 1);
      pslog_test_allocator_reset();
      TEST_ASSERT(unsetenv("PSLOG_ROLLBACK_OUTPUT") == 0);
    }
    TEST_ASSERT(failures > 0 && successes > 0);
  }
  TEST_ASSERT(remove("build/pslog-oom-output.log") == 0);
  return 0;
}
#endif

int main(void) {
  lua_allocator_guard guard;
  lua_State *L;
  int failed = 0;

  guard.owner = pthread_self();
  guard.foreign_calls = 0;
  TEST_ASSERT(pthread_mutex_init(&guard.mutex, NULL) == 0);
  L = lua_newstate(guarded_lua_alloc, &guard, 0);

  TEST_ASSERT(L != NULL);
  luaL_openlibs(L);
  preload_pslog_core(L);
  if (run_chunk(
          L, "package.path = './lua/?.lua;./lua/?/init.lua;' .. package.path\n"
             "pslog = require('pslog')") != 0) {
    lua_close(L);
    return 1;
  }

  failed |= test_borrowed_logger_writes(L);
  failed |= test_borrowed_logger_streams_callback_chunks(L);
  failed |= test_borrowed_logger_view_survives_lua_close(L);
  failed |= test_ref_logger_derives_and_retains(L);
  failed |= test_ref_logger_survives_lua_close(L);
  failed |= test_negative_paths(L);
  failed |= test_multi_logger_views(L);
  failed |= test_native_classification(L);
  failed |= test_environment_classification(L);
  failed |= test_core_wrappers();
  failed |= test_coroutine_native_workers(L);
  failed |= test_native_explicit_close(L);
  failed |= test_validation_contract(L);
  failed |= test_coroutine_function_sink(L);
  failed |= test_suspended_coroutine_collection(L);
  failed |= test_callback_graph_and_reentry(L);
  failed |= test_field_conversion_reentry(L);
  failed |= test_lua_adoption_failures();
  failed |= test_callback_allocation_failure();
#ifdef PSLOG_TEST_HOOKS
  failed |= test_ref_derivation_failure(L);
  failed |= test_native_constructor_rollback();
#endif
  lua_close(L);
  TEST_ASSERT(guard.foreign_calls == 0);
  TEST_ASSERT(pthread_mutex_destroy(&guard.mutex) == 0);
  return failed;
}
