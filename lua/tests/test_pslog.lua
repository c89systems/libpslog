local pslog = require("pslog")

local function assert_eq(got, want, msg)
  if got ~= want then
    error(string.format("%s: expected %s, got %s", msg or "assert_eq failed", tostring(want), tostring(got)))
  end
end

local function assert_true(value, msg)
  if not value then
    error(msg or "assert_true failed")
  end
end

local function assert_contains(haystack, needle, msg)
  assert_true(haystack:find(needle, 1, true) ~= nil, msg or ("missing substring: " .. needle))
end

local function read_all(path)
  local fp = assert(io.open(path, "rb"))
  local data = assert(fp:read("*a"))
  fp:close()
  return data
end

do
  local path = "/tmp/libpslog-lua-basic.log"
  local log = pslog.new(path, {
    mode = "json",
    no_color = true,
    disable_timestamp = true,
  })

  log:info("ready", "service", "api", "attempt", 3, "ok", true, "blob", "a\0b")
  log:close()

  local payload = read_all(path)
  assert_contains(payload, '"msg":"ready"', "missing msg field")
  assert_contains(payload, '"service":"api"', "missing service field")
  assert_contains(payload, '"attempt":3', "missing integer field")
  assert_contains(payload, '"ok":true', "missing boolean field")
  assert_contains(payload, '"blob":"a\\u0000b"', "missing embedded NUL string field")
  os.remove(path)
end

do
  local chunks = {}
  local log = pslog.new_json({
    output = function(chunk) chunks[#chunks + 1] = chunk end,
    no_color = true,
    disable_timestamp = true,
  })
  local key = "k\0ey"

  log:info("m\0sg", key, "v\0al", "n\0um", 7,
    "b\0ytes", pslog.bytes("\0\255"),
    "t\0rusted", pslog.trusted("t\0xt"),
    "u\0int", pslog.u64(9),
    "ti\0me", pslog.time_unix(1711737600),
    "du\0ration", pslog.duration_s(2),
    "er\0rno", pslog.errno(2))
  log:log("warn", "g\0eneric", { ["table\0key"] = "table\0value" })
  local child = log:with("child\0key", "child\0value")
  child:info("derived\0msg")
  child:close()
  log:close()

  local payload = table.concat(chunks)
  assert_contains(payload, '"msg":"m\\u0000sg"', "level method must preserve message length")
  assert_contains(payload, '"k\\u0000ey":"v\\u0000al"', "pair string must preserve key and value lengths")
  assert_contains(payload, '"n\\u0000um":7', "pair numeric key must preserve length")
  assert_contains(payload, '"b\\u0000ytes":"00ff"', "bytes wrapper key must preserve length")
  assert_contains(payload, '"t\\u0000rusted":"t\\u0000xt"', "trusted wrapper must preserve lengths")
  assert_contains(payload, '"u\\u0000int":9', "u64 wrapper key must preserve length")
  assert_contains(payload, '"ti\\u0000me":"', "time wrapper key must preserve length")
  assert_contains(payload, '"du\\u0000ration":"', "duration wrapper key must preserve length")
  assert_contains(payload, '"er\\u0000rno":"', "errno wrapper key must preserve length")
  assert_contains(payload, '"msg":"g\\u0000eneric"', "generic method must preserve message length")
  assert_contains(payload, '"table\\u0000key":"table\\u0000value"', "table field must preserve lengths")
  assert_contains(payload, '"child\\u0000key":"child\\u0000value"', "derived field must preserve lengths")
  assert_contains(payload, '"msg":"derived\\u0000msg"', "derived message must preserve length")
  assert_true(not payload:find("\0", 1, true), "JSON output must escape NUL bytes")
end

do
  local path = "/tmp/libpslog-lua-derived.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
  }):with("service", "checkout"):with_level("warn"):with_level_field()
  local payload

  log:warn("retry", { attempt = 2, ok = false })
  log:close()
  payload = read_all(path)

  assert_contains(payload, '"service":"checkout"', "missing derived field")
  assert_contains(payload, '"loglevel":"warn"', "missing level field")
  assert_contains(payload, '"attempt":2', "missing table integer field")
  assert_contains(payload, '"ok":false', "missing table boolean field")
  os.remove(path)
end

do
  local ctor_path = "/tmp/libpslog-lua-level-field-ctor.log"
  local derived_path = "/tmp/libpslog-lua-level-field-derived.log"
  local ctor = pslog.new_json(ctor_path, {
    no_color = true,
    disable_timestamp = true,
    min_level = "warn",
    with_level_field = true,
  })
  local derived = pslog.new_json(derived_path, {
    no_color = true,
    disable_timestamp = true,
    min_level = "warn",
  }):with_level_field()
  local ctor_payload
  local derived_payload

  ctor:error("ctor", "service", "api")
  derived:error("ctor", "service", "api")
  ctor:close()
  derived:close()

  ctor_payload = read_all(ctor_path)
  derived_payload = read_all(derived_path)

  assert_contains(ctor_payload, '"loglevel":"warn"', "missing constructor level field")
  assert_eq(ctor_payload, derived_payload, "constructor level field should match derived logger output")
  os.remove(ctor_path)
  os.remove(derived_path)
end

do
  local path = "/tmp/libpslog-lua-console.log"
  local log = pslog.new(path, {
    mode = "console",
    no_color = true,
    disable_timestamp = true,
  })
  local payload

  log:info("ready", { service = "api", port = 8080 })
  log:close()
  payload = read_all(path)

  assert_contains(payload, "INF", "missing console level")
  assert_contains(payload, "ready", "missing console message")
  assert_contains(payload, "service=api", "missing console table field")
  assert_contains(payload, "port=8080", "missing console numeric field")
  os.remove(path)
end

do
  local chunks = {}
  local log = pslog.new({
    output = function(chunk) chunks[#chunks + 1] = chunk end,
    no_color = true,
    disable_timestamp = true,
  })
  log:info("m\0sg", "k\0ey", "v\0al")
  log:close()
  local payload = table.concat(chunks)
  assert_contains(payload, "m\\x00sg", "console message must preserve length")
  assert_contains(payload, "k\\x00ey=", "console key must preserve length")
  assert_contains(payload, "v\\x00al", "console value must preserve length")
  assert_true(not payload:find("\0", 1, true), "console output must escape NUL bytes")
end

do
  local path = "/tmp/libpslog-lua-numeric-keys.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
  })
  local payload

  log:info("numeric keys", {
    [1] = "one",
    [2] = "two",
    service = "api",
  })
  log:close()
  payload = read_all(path)

  assert_contains(payload, '"1":"one"', "missing first numeric table key")
  assert_contains(payload, '"2":"two"', "missing second numeric table key")
  assert_contains(payload, '"service":"api"', "missing string table key")
  os.remove(path)
end

do
  local path = "/tmp/libpslog-lua-table-temporaries.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
  }):with({
    [1] = { nested = true },
    [2] = "two",
    nested = { value = "stringified" },
  })
  local payload

  log:info("temporaries")
  log:close()
  payload = read_all(path)

  assert_contains(payload, '"1":"table:', "missing converted numeric key with stringified table value")
  assert_contains(payload, '"2":"two"', "missing converted numeric key")
  assert_contains(payload, '"nested":"table:', "missing stringified table value")
  os.remove(path)
end

do
  local parsed = pslog.parse_level("warn")
  local palettes = pslog.available_palettes()

  assert_eq(parsed, "warn", "parse_level")
  assert_true(type(pslog.version()) == "string" and #pslog.version() > 0, "version should be non-empty")
  assert_true(type(palettes) == "table" and #palettes > 0, "expected built-in palettes")
  assert_eq(pslog._wrap_bytes, nil, "private wrapper helper leaked through facade")
  assert_eq(pslog._wrap_trusted, nil, "private trusted wrapper helper leaked through facade")
  assert_eq(pslog.new_logger, nil, "private constructor helper leaked through facade")
  assert_eq(pslog.new_logger_from_env, nil, "private env constructor helper leaked through facade")
end

do
  local chunks = {}
  local total = 0
  local log = pslog.new_json({
    output = function(chunk)
      chunks[#chunks + 1] = chunk
      total = total + #chunk
    end,
    no_color = true,
    disable_timestamp = true,
  })

  log:info("callback", "service", "api", "ok", true)
  log:close()
  assert_true(#chunks > 0, "expected callback output chunks")
  assert_true(total > 0, "expected callback output bytes")
  assert_contains(table.concat(chunks), '"msg":"callback"', "missing callback output payload")
end

do
  local path = "/tmp/libpslog-lua-wrappers.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
    utc = true,
    time_format = "RFC3339",
  })
  local payload

  log:info("typed",
    "bytes", pslog.bytes(string.char(0, 1, 2, 255)),
    "time_a", pslog.time_unix(1711737600, 0, 0),
    "time_b", pslog.time { sec = 1711737600, nsec = 0, offset = 0 },
    "duration_a", pslog.duration(0, 125000000),
    "duration_b", pslog.duration_ms(250),
    "duration_c", pslog.duration_us(1250),
    "duration_d", pslog.duration_ns(42),
    "duration_e", pslog.duration_s(2.5),
    "errno", pslog.errno(2),
    "trusted", pslog.trusted("svc.checkout"),
    "trusted_nul", pslog.trusted("a\0b"),
    "big", pslog.u64(42),
    "huge", pslog.u64("18446744073709551615")
  )
  log:close()
  payload = read_all(path)

  assert_contains(payload, '"bytes":"000102ff"', "missing bytes wrapper")
  assert_contains(payload, '"time_a":"2024-03-29T18:40:00Z"', "missing time_unix wrapper")
  assert_contains(payload, '"time_b":"2024-03-29T18:40:00Z"', "missing time table wrapper")
  assert_contains(payload, '"duration_a":"125.000ms"', "missing duration wrapper")
  assert_contains(payload, '"duration_b":"250.000ms"', "missing duration_ms wrapper")
  assert_contains(payload, '"duration_c":"1.250ms"', "missing duration_us wrapper")
  assert_contains(payload, '"duration_d":"42ns"', "missing duration_ns wrapper")
  assert_contains(payload, '"duration_e":"2.500s"', "missing duration_s wrapper")
  assert_contains(payload, '"trusted":"svc.checkout"', "missing trusted wrapper")
  assert_contains(payload, '"trusted_nul":"a\\u0000b"', "missing trusted embedded NUL wrapper")
  assert_contains(payload, '"big":42', "missing u64 wrapper")
  assert_contains(payload, '"huge":18446744073709551615', "missing u64 decimal-string wrapper")
  assert_contains(payload, '"errno":"', "missing errno wrapper")
  os.remove(path)
end

do
  local path = "/tmp/libpslog-lua-time-defaults.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
    utc = true,
    time_format = "RFC3339",
  })

  log:info("defaults",
    "time_unix", pslog.time_unix(1711737600),
    "time_table", pslog.time { epoch_seconds = 1711737600, nanoseconds = 0, utc_offset_minutes = 0 },
    "neg_duration", pslog.duration_ms(-1500)
  )
  log:close()

  local payload = read_all(path)
  assert_contains(payload, '"time_unix":"2024-03-29T18:40:00Z"', "missing defaulted time_unix wrapper")
  assert_contains(payload, '"time_table":"2024-03-29T18:40:00Z"', "missing aliased time table wrapper")
  assert_contains(payload, '"neg_duration":"-1.500s"', "missing negative duration wrapper")
  os.remove(path)
end

do
  local ok, err

  ok, err = pcall(function() pslog.bytes(123) end)
  assert_true(not ok and err:find("expects a Lua string", 1, true) ~= nil, "expected bytes type failure")

  ok, err = pcall(function() pslog.time("bad") end)
  assert_true(not ok and err:find("expects a table", 1, true) ~= nil, "expected time type failure")

  ok, err = pcall(function() pslog.errno("x") end)
  assert_true(not ok and err:find("expects an integer code", 1, true) ~= nil, "expected errno type failure")

  ok, err = pcall(function() pslog.trusted(false) end)
  assert_true(not ok and err:find("expects a string", 1, true) ~= nil, "expected trusted type failure")

  ok, err = pcall(function() pslog.u64(-1) end)
  assert_true(not ok and err:find("expects a non%-negative integer") ~= nil, "expected u64 range failure")

  ok, err = pcall(function() pslog.u64("18446744073709551616") end)
  assert_true(not ok and err:find("exceeds uint64 range", 1, true) ~= nil, "expected u64 decimal overflow failure")

  ok, err = pcall(function() pslog.new({ mode = "bogus" }) end)
  assert_true(not ok and err:find("invalid mode", 1, true) ~= nil, "expected invalid mode failure")

  ok, err = pcall(function() pslog.new({ color = "bogus" }) end)
  assert_true(not ok and err:find("invalid color mode", 1, true) ~= nil, "expected invalid color failure")

  ok, err = pcall(function() pslog.new({ non_finite_float_policy = "bogus" }) end)
  assert_true(not ok and err:find("invalid non_finite_float_policy", 1, true) ~= nil, "expected invalid float policy failure")

  ok, err = pcall(function() pslog.new({ output = {} }) end)
  assert_true(not ok and err:find("output must be", 1, true) ~= nil, "expected invalid output failure")

  ok, err = pcall(function()
    local log = pslog.new_json({
      output = function() end,
      no_color = true,
      disable_timestamp = true,
    })
    log:info("odd", "k1", "v1", "orphan")
  end)
  assert_true(not ok and err:find("expected key/value pairs", 1, true) ~= nil, "expected key/value arity failure")
end

do
  local chunks = {}
  local seen = {}
  local log = pslog.new_json({
    output = function(chunk)
      chunks[#chunks + 1] = chunk
    end,
    no_color = true,
    disable_timestamp = true,
  })
  local i

  for i = 1, 512 do
    local key = string.format("dynamic_%03d_\\_\"", i)
    local value = string.format("value-%03d-with-escape-\\-and-quote-\"-suffix-%d", i, i * 97)
    local msg = string.format("metadata-%03d", i)

    seen[msg] = { key = key, value = value }
    log:info(msg, key, value)
    key = nil
    value = nil
    collectgarbage("collect")
  end
  log:close()

  local payload = table.concat(chunks)
  for msg, field in pairs(seen) do
    local escaped_key = field.key:gsub("\\", "\\\\"):gsub('"', '\\"')
    local escaped_value = field.value:gsub("\\", "\\\\"):gsub('"', '\\"')

    assert_contains(payload, string.format('"msg":"%s"', msg), "missing metadata message")
    assert_contains(payload, string.format('"%s":"%s"', escaped_key, escaped_value),
      "missing dynamic key/value field")
  end
end

do
  local chunks = {}
  local seen = {}
  local log = pslog.new_json({
    output = function(chunk)
      chunks[#chunks + 1] = chunk
    end,
    no_color = true,
    disable_timestamp = true,
  })
  local mt = {
    __tostring = function(self)
      return self.value
    end,
  }
  local i

  for i = 1, 512 do
    local value = string.format("value-%03d-with-escape-\\-and-quote-\"-suffix-%d", i, i * 97)
    local wrapped = setmetatable({ value = value }, mt)
    local msg = string.format("fallback-%03d", i)

    seen[msg] = value
    log:info(msg, "stringified", wrapped)
    collectgarbage("collect")
  end
  log:close()

  local payload = table.concat(chunks)
  for msg, value in pairs(seen) do
    assert_contains(payload, string.format('"msg":"%s"', msg), "missing fallback message")
    assert_contains(payload, string.format('"stringified":"%s"', value:gsub("\\", "\\\\"):gsub('"', '\\"')),
      "missing fallback stringified value")
  end
end

do
  local path = "/tmp/libpslog-lua-close.log"
  local log = pslog.new_json(path, {
    no_color = true,
    disable_timestamp = true,
  })

  assert_true(log:close(), "close should succeed")
  assert_true(log:close(), "second close should be harmless")
  os.remove(path)
end

do
  local package_path = package.path
  local package_cpath = package.cpath
  local script = string.format(
    "package.path=%q; package.cpath=%q; local pslog=require(\"pslog\"); local chunks={}; local log=pslog.from_env(\"APP_LOG_\",{output=function(chunk) chunks[#chunks+1]=chunk end}); log:debug(\"env works\",\"service\",\"api\"); log:close(); io.write(table.concat(chunks))",
    package_path,
    package_cpath
  )
  local cmd = table.concat({
    "APP_LOG_MODE=json",
    "APP_LOG_LEVEL=debug",
    "APP_LOG_NO_COLOR=1",
    "APP_LOG_DISABLE_TIMESTAMP=1",
    "lua -e",
    string.format("%q", script),
  }, " ")
  local pipe = assert(io.popen(cmd, "r"))
  local payload = assert(pipe:read("*a"))
  local ok, _, status = pipe:close()

  assert_true(ok and status == 0, "expected env subprocess success")
  assert_contains(payload, '"msg":"env works"', "missing env message")
  assert_contains(payload, '"service":"api"', "missing env field")
  assert_contains(payload, '"lvl":"debug"', "missing env level override")
end

print("lua pslog tests passed")
