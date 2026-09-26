#include <errno.h>
#include <pslog_lua.h>
#include <string.h>

/* The staged Lua runtime uses 64-bit integers, including C89 consumers. */
typedef char lua_integer_matches_runtime[(sizeof(lua_Integer) == 8u) ? 1 : -1];

#ifdef __cplusplus
extern "C" {
#endif
#include <lauxlib.h>
#include <lualib.h>
extern int luaopen_pslog_core(lua_State *L);
#ifdef __cplusplus
}
#endif

int main(void) {
  lua_State *L = luaL_newstate();
  pslog_lua_logger_view view;
  pslog_lua_logger_ref ref;
  pslog_field field = pslog_str("consumer", "installed");
  int result = 1;
  if (L == NULL)
    return 1;
  luaL_openlibs(L);
  luaL_requiref(L, "pslog.core", luaopen_pslog_core, 1);
  lua_pop(L, 1);
  if (luaL_dostring(
          L, "native=require('pslog.core').new_logger({output='stdout'}); "
             "bound=require('pslog.core').new_logger({output=function(_) "
             "error('unexpected callback') end})") != LUA_OK)
    goto done;
  memset(&view, 0, sizeof(view));
  view.size = sizeof(view);
  view.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  memset(&ref, 0, sizeof(ref));
  ref.size = sizeof(ref);
  ref.abi_version = PSLOG_LUA_INTEROP_ABI_VERSION;
  lua_getglobal(L, "native");
  if (pslog_lua_check_logger(L, -1, &view) != 0 ||
      view.flags != PSLOG_LUA_LOGGER_NATIVE_SINK)
    goto done;
  if (pslog_lua_ref_logger(L, -1, &field, 1u, &ref) != 0)
    goto done;
  if (!pslog_has_native_output(ref.derived)) {
    pslog_lua_unref_logger(&ref);
    goto done;
  }
  pslog_lua_unref_logger(&ref);
  lua_pop(L, 1);
  lua_getglobal(L, "bound");
  if (pslog_lua_check_logger(L, -1, &view) != 0 || view.flags != 0u)
    goto done;
  lua_pop(L, 1);
  if (lua_gettop(L) != 0 || pslog_lua_check_logger(NULL, 0, &view) != EINVAL ||
      view.logger != NULL || view.flags != 0u)
    goto done;
  result = 0;
done:
  lua_close(L);
  return result;
}
