# lua-pslog

`lua-pslog` is the Lua distribution of `libpslog`. It ships the same logging
engine as the C library, but exposes it through a Lua-first API that is
significantly more convenient than the raw C surface.

The rock package is named `lua-pslog`, while Lua code loads it as:

```lua
local pslog = require("pslog")
```

The goal of the binding is not to copy the C API literally. It is to preserve
the same logging product while presenting it in a form that feels natural in
Lua:

- normal Lua values are inferred automatically
- structured logging is the default style
- child loggers are cheap and explicit
- special wrappers exist only where Lua does not have a native type with the
  right semantics

In practice, the common path is simple:

```lua
local pslog = require("pslog")

local log = pslog.new_json(io.stdout, {
  no_color = true,
  disable_timestamp = true,
}):with("service", "checkout")

log:info("ready", "port", 8080, "ok", true)
```

That gives you the same core logger behavior as `libpslog`: console and JSON
output, structured fields, derived loggers, palette support, environment-driven
configuration, and the same underlying formatting and output guarantees.

## Design

The Lua binding follows a simple rule:

Ordinary Lua values should stay ordinary Lua values.

That means the usual field types do not require wrappers. If you log a string,
integer, float, boolean, or `nil`, the binding maps that value to the
appropriate typed `libpslog` field automatically. You only need an explicit
wrapper when the value represents something Lua does not model natively, such
as raw bytes, time, duration, `errno`, or an unsigned 64-bit integer.

This keeps the normal logging experience lightweight without losing the typed C
backend.

## Constructors

The module exposes three constructor entry points.

### `pslog.new(output_or_opts, opts)`

Creates a logger using console mode by default.

If the first argument is not a table, it is treated as the output destination.
If the first argument is a table and `opts` is omitted, that table is treated
as the full options object.

```lua
local log = pslog.new(io.stdout, {
  no_color = true,
  disable_timestamp = true,
})
```

### `pslog.new_json(output_or_opts, opts)`

Creates a logger in JSON mode.

```lua
local log = pslog.new_json("/tmp/app.log", {
  no_color = true,
  disable_timestamp = true,
})
```

### `pslog.new_structured(...)`

Alias for `pslog.new_json(...)`.

### `pslog.from_env(prefix_or_opts, maybe_opts)`

Creates a logger from a seed config and then applies environment overrides
through `pslog_new_from_env()`.

```lua
local log = pslog.from_env("APP_LOG_", {
  output = io.stdout,
  mode = "console",
  no_color = true,
})
```

If the first argument is a table and the second is omitted, the table is used
as the seed config and the default `LOG_` prefix is used.

This constructor is useful when you want application defaults in code while
still allowing deployment-time control through environment variables.

## Output Targets

The constructor `output` value can be one of the following:

- `nil`, to use the library default output
- `"stdout"` or `"default"`, to write to stdout
- `"stderr"`, to write to stderr
- a filesystem path string, which is opened in append-binary mode
- a Lua file handle such as `io.stdout`, `io.stderr`, or `io.open(...)`
- a Lua function, which receives rendered output chunks as strings

The function form is especially useful for tests, embedding, and benchmark
sinks:

```lua
local chunks = {}
local log = pslog.new_json({
  output = function(chunk)
    chunks[#chunks + 1] = chunk
  end,
  no_color = true,
  disable_timestamp = true,
})

log:info("ready", "service", "api")
log:close()
```

Ownership rules are straightforward:

- if you pass an existing file handle, the binding does not own that stream
- if you pass a path string or callback function, the logger owns that output
  resource

`log:close()` closes owned outputs. It does not destroy the Lua logger object
itself.

## Configuration Options

The options table maps directly onto the `pslog_config` surface exposed by the
C library. The currently supported keys are:

- `output`
- `mode`: `"console"`, `"json"`, or `"structured"`
- `color`: `"auto"`, `"never"`, or `"always"`
- `no_color`: convenience override for `"never"`
- `force_color`: convenience override for `"always"`
- `disable_timestamp`
- `utc`
- `verbose_fields`
- `time_format`
- `line_buffer_capacity`
- `min_level`: either a level name such as `"warn"` or an integer level value
- `palette`: built-in palette name
- `non_finite_float_policy`: `"string"` or `"null"`

Invalid option values fail immediately with Lua errors. The binding does not
silently coerce unknown strings or ignore malformed configuration.

## Logging API

Logger instances expose the following methods:

- `log:with(...)`
- `log:with_level(level)`
- `log:with_level_field()`
- `log:log(level, msg, ...)`
- `log:trace(msg, ...)`
- `log:debug(msg, ...)`
- `log:info(msg, ...)`
- `log:warn(msg, ...)`
- `log:error(msg, ...)`
- `log:fatal(msg, ...)`
- `log:panic(msg, ...)`
- `log:close()`

### `log:with(...)`

Returns a child logger with additional persistent fields.

```lua
local base = pslog.new_json(io.stdout, {
  no_color = true,
  disable_timestamp = true,
})

local worker = base:with("service", "checkout", "component", "worker")
worker:info("ready")
```

`with(...)` does not mutate the receiver. This matches the mental model used by
the Go logger and keeps logger derivation explicit.

### `log:with_level(level)`

Returns a child logger with a different minimum enabled level.

### `log:with_level_field()`

Returns a child logger that emits the logger's effective threshold as a normal
structured field named `loglevel`.

That field reflects the logger configuration, not the per-event `lvl` field
that is already present in the emitted log entry.

### Level-specific methods

`trace`, `debug`, `info`, `warn`, `error`, `fatal`, and `panic` are thin
convenience methods over `log:log(level, msg, ...)`.

As in the C library:

- `fatal` logs and then terminates the process
- `panic` logs and then aborts

Lua strings carry their byte lengths. Messages, field keys, and string values
therefore preserve embedded NUL bytes, including in fields attached with
`log:with(...)`. JSON output escapes them as `\\u0000`; console output uses
`\\x00`. No string wrapper or explicit length argument is needed.

## Passing Fields

Every logging call accepts fields in one of two forms.

### Alternating key/value pairs

This is the primary API.

```lua
log:info("request handled",
  "service", "checkout",
  "attempt", 3,
  "ok", true
)
```

This form preserves order and is the best fit when you want a stable, explicit
call shape.

### A single table

```lua
log:warn("retrying", {
  attempt = 2,
  backoff_ms = 250,
  last_error = "upstream timeout",
})
```

This form is a convenience. Lua table iteration order is not guaranteed and
should be treated as unordered.

If you use the key/value form, the number of arguments after the message must
be even. Odd argument counts are rejected.

## Type Inference

Normal Lua values are converted automatically:

- `nil` becomes `pslog_null`
- `boolean` becomes `pslog_bool`
- a Lua integer becomes a signed integer field
- a Lua float becomes a `double` field
- a string becomes a normal string field
- userdata and lightuserdata become pointer fields
- all other values are converted through Lua `tostring` and logged as strings

This is the default path and should cover most use cases.

## Semantic Wrappers

Lua does not have first-class scalar types for every semantic field supported
by `libpslog`. For those cases, the binding exposes explicit wrappers:

```lua
pslog.bytes(raw)
pslog.time_unix(sec, nsec, utc_offset_minutes)
pslog.time{sec = ..., nsec = ..., offset = ...}
pslog.duration(sec, nsec)
pslog.duration_ns(ns)
pslog.duration_us(us)
pslog.duration_ms(ms)
pslog.duration_s(sec)
pslog.errno(code)
pslog.trusted(str)
pslog.u64(value)
```

Example:

```lua
log:info("typed fields",
  "payload", pslog.bytes(string.char(0, 1, 2, 255)),
  "started_at", pslog.time { sec = 1711737600, nsec = 0, offset = 0 },
  "elapsed", pslog.duration_ms(125),
  "errno", pslog.errno(2),
  "trusted_service", pslog.trusted("checkout.worker"),
  "big_counter", pslog.u64("18446744073709551615")
)
```

### `pslog.bytes(raw)`

Takes a Lua string containing arbitrary bytes, including embedded NULs.

This is still a logger field, not a binary output mode. Bytes are rendered as
hex text in both JSON and console output.

### `pslog.time_unix(sec, nsec, utc_offset_minutes)`

Builds a typed time field from Unix seconds plus optional nanoseconds and UTC
offset minutes.

Missing trailing arguments default to zero.

### `pslog.time{...}`

Lua-friendly table form for the same time field. The table accepts:

- `sec`
- `nsec`
- `offset`

It also accepts these aliases:

- `epoch_seconds`
- `nanoseconds`
- `utc_offset_minutes`

### Duration helpers

The canonical constructor is:

```lua
pslog.duration(sec, nsec)
```

The convenience forms are:

- `pslog.duration_ns(ns)`
- `pslog.duration_us(us)`
- `pslog.duration_ms(ms)`
- `pslog.duration_s(sec)`

All of them normalize into the native C duration field representation before
emission.

### `pslog.errno(code)`

Creates an `errno` field using an integer error code.

### `pslog.trusted(str)`

Marks a string as trusted for the underlying C emission path.

### `pslog.u64(value)`

Creates an explicit unsigned 64-bit field. Unsignedness is not inferred from
normal Lua integers.

Accepted values are:

- a non-negative Lua integer
- a decimal string

The string form is important when you need values outside the signed integer
range that Lua would otherwise represent ambiguously.

## Helper Functions

The module also exposes a small set of helpers:

- `pslog.parse_level(text)` returns the canonical level string or `nil`
- `pslog.level_string(level)` returns the canonical string for a level
- `pslog.available_palettes()` returns the built-in palette names
- `pslog.version()` returns the bound `libpslog` version string

## C Embedder Interop

The rock also installs `pslog_lua.h` for C embedders that already host Lua and
need to share `pslog.logger` userdata with other native modules loaded in the
same process.

The interop helpers are exported by `pslog.core`, not by the standalone
`libpslog` shared library. Native modules that use this surface should include
`pslog_lua.h`, load or link against the Lua binding module that provides the
`pslog_lua_*` symbols, and link against `libpslog` for the core logger API.

Callers initialize `size` and `abi_version` before passing
`pslog_lua_logger_view` or `pslog_lua_logger_ref` to the helper functions.
`pslog_lua_check_logger()` returns a borrowed logger view for a live Lua logger.
`pslog_lua_ref_logger()` keeps the Lua userdata alive through the Lua registry
and can optionally create a C-owned derived logger with additional fields.
Release retained references with `pslog_lua_unref_logger()`.

Borrowed views remain owned by Lua and are valid only while the backing userdata
is live on the owning `lua_State`. After `log:close()`, new interop checks reject
the closed userdata, while already obtained borrowed or retained logger pointers
remain allocated until the userdata lifetime or retained reference ends.

## Examples

Reference examples live under [`examples/`](./examples/):

- [`examples/example.lua`](./examples/example.lua)
- [`examples/basic.lua`](./examples/basic.lua)
- [`examples/callback.lua`](./examples/callback.lua)
- [`examples/from_env.lua`](./examples/from_env.lua)

Run them from the repository root after building the local rock tree:

```sh
make lua-rock
eval "$(make lua-env)"
build/lua-runtime/pslog_lua lua/examples/example.lua
build/lua-runtime/pslog_lua lua/examples/basic.lua
build/lua-runtime/pslog_lua lua/examples/from_env.lua
build/lua-runtime/pslog_lua lua/examples/callback.lua
```

## Local Development

Build the local rock tree:

```sh
make lua-rock
```

This installs the selected-compiler public C SDK under `build/lua-sdk/` first.
The rock, Lua/C interop checks, and embedded Lua benchmarks compile and link
only against that installed local SDK, while Lua 5.5 headers and its static
runtime are built from checksum-pinned Lua 5.5.1 sources with the selected
compiler and staged under `build/lua-host/`. The local interpreter is
`build/lua-runtime/pslog_lua`; use it for local module tests and examples.

Lua 5.5.1 is the supported Lua runtime. `make lua-env` configures Lua module
search paths for the repo-local rock tree. Native library paths are private
to the local interpreter, so host tools keep their own runtime.

Run the Lua tests:

```sh
make lua-test
```

The Lua test suite covers:

- constructor behavior and option parsing
- console and JSON output
- derived logger behavior
- callback sinks
- environment-driven construction
- semantic wrappers
- output ownership and `close()` behavior
- validation errors and failure cases
