#ifndef PSLOG_H
#define PSLOG_H

#include <stddef.h>
#include <stdio.h>

#if !defined(PSLOG_VERSION_MAJOR) || !defined(PSLOG_VERSION_MINOR) ||          \
    !defined(PSLOG_VERSION_PATCH) || !defined(PSLOG_VERSION_STRING)
#include "pslog_version.h"
#endif

/** Visibility annotation applied to every public libpslog ABI symbol. */
#if defined(__GNUC__) || defined(__clang__)
#define PSLOG_API __attribute__((visibility("default")))
#else
#define PSLOG_API
#endif

#ifdef __cplusplus
extern "C" {
#endif
/**
 * Special `pslog_config.time_format` token for RFC3339 timestamps without
 * sub-second precision.
 *
 * This layout is cacheable because the rendered value changes only when the
 * wall-clock second changes.
 */
#define PSLOG_TIME_FORMAT_RFC3339 "RFC3339"
/**
 * Special `pslog_config.time_format` token for RFC3339 timestamps with
 * nanosecond precision.
 *
 * This layout bypasses the per-second timestamp cache because the rendered
 * value can change multiple times within the same second.
 */
#define PSLOG_TIME_FORMAT_RFC3339_NANO "RFC3339Nano"
/**
 * Default line-chunk size used by emitters when
 * `pslog_config.line_buffer_capacity` is left at zero or set to this value
 * explicitly.
 *
 * This is a chunk size, not a truncation limit. Long log lines are streamed in
 * multiple writes rather than cut off.
 */
#define PSLOG_DEFAULT_LINE_BUFFER_CAPACITY 1024u

/**
 * Output encoder selected for a logger instance.
 *
 * `PSLOG_MODE_CONSOLE` emits human-oriented log lines.
 * `PSLOG_MODE_JSON` emits one structured JSON object per line.
 */
typedef enum pslog_mode {
  /** Human-oriented, line-based output. */
  PSLOG_MODE_CONSOLE = 0,
  /** One structured JSON object per line. */
  PSLOG_MODE_JSON = 1
} pslog_mode;

/**
 * Log levels understood by libpslog.
 *
 * `PSLOG_LEVEL_NOLEVEL` is a deliberate "unleveled" event and is rendered as
 * `---` in console mode and `"nolevel"` in JSON mode.
 *
 * `PSLOG_LEVEL_DISABLED` is a sentinel used for configuration and filtering. It
 * is not intended to be emitted as a normal event.
 */
typedef enum pslog_level {
  /** Most verbose diagnostic level. */
  PSLOG_LEVEL_TRACE = -1,
  /** Diagnostic level enabled by the default configuration. */
  PSLOG_LEVEL_DEBUG = 0,
  /** Normal operational event. */
  PSLOG_LEVEL_INFO = 1,
  /** Recoverable or noteworthy condition. */
  PSLOG_LEVEL_WARN = 2,
  /** Error condition. */
  PSLOG_LEVEL_ERROR = 3,
  /** Error event followed by process termination. */
  PSLOG_LEVEL_FATAL = 4,
  /** Error event followed by process abort. */
  PSLOG_LEVEL_PANIC = 5,
  /** Event deliberately emitted without a severity label. */
  PSLOG_LEVEL_NOLEVEL = 6,
  /** Filtering sentinel that suppresses every ordinary event. */
  PSLOG_LEVEL_DISABLED = 127
} pslog_level;

/**
 * Color policy for emitters that support ANSI decoration.
 *
 * `PSLOG_COLOR_AUTO` enables color only when the selected output reports that
 * it is attached to a terminal.
 */
typedef enum pslog_color_mode {
  /** Emit color only when the output reports a terminal. */
  PSLOG_COLOR_AUTO = 0,
  /** Never emit ANSI color sequences. */
  PSLOG_COLOR_NEVER = 1,
  /** Always emit ANSI color sequences. */
  PSLOG_COLOR_ALWAYS = 2
} pslog_color_mode;

/**
 * Policy used when a floating-point field is NaN or infinite.
 *
 * JSON cannot represent these values as numeric literals, so callers must
 * choose between string preservation or `null`.
 */
typedef enum pslog_non_finite_float_policy {
  /** Preserve NaN and infinity as strings. */
  PSLOG_NON_FINITE_FLOAT_AS_STRING = 0,
  /** Encode NaN and infinity as JSON null values. */
  PSLOG_NON_FINITE_FLOAT_AS_NULL = 1
} pslog_non_finite_float_policy;

/**
 * Field kinds that can be stored in `pslog_field`.
 *
 * The field type controls both the output encoding and whether the value is
 * quoted, colorized, escaped, or rendered numerically.
 */
typedef enum pslog_field_type {
  /** Null value. */
  PSLOG_FIELD_NULL = 0,
  /** String value. */
  PSLOG_FIELD_STRING = 1,
  /** Signed integer value. */
  PSLOG_FIELD_SIGNED = 2,
  /** Unsigned integer value. */
  PSLOG_FIELD_UNSIGNED = 3,
  /** Floating-point value. */
  PSLOG_FIELD_DOUBLE = 4,
  /** Boolean value. */
  PSLOG_FIELD_BOOL = 5,
  /** Pointer value. */
  PSLOG_FIELD_POINTER = 6,
  /** Borrowed bytes encoded as hexadecimal. */
  PSLOG_FIELD_BYTES = 7,
  /** Wall-clock timestamp. */
  PSLOG_FIELD_TIME = 8,
  /** Human-readable duration. */
  PSLOG_FIELD_DURATION = 9,
  /** errno-style integer rendered as error text. */
  PSLOG_FIELD_ERRNO = 10
} pslog_field_type;

/**
 * Borrowed byte slice used by `PSLOG_FIELD_BYTES`.
 *
 * The data is not copied on field construction. The pointed-to memory must stay
 * valid until the logging call that consumes the field has returned. Derived
 * loggers created with `with()` copy their attached fields into owned storage.
 */
typedef struct pslog_bytes {
  /** Start of the borrowed byte slice. */
  const unsigned char *data;
  /** Length of `data` in bytes. */
  size_t len;
} pslog_bytes;

/** Borrowed string bytes. `data` may be NULL only when `len` is zero. */
typedef struct pslog_string_view {
  const char *data;
  size_t len;
} pslog_string_view;

/**
 * Signed 64-bit integer payload type used by `pslog_i64()`.
 *
 * libpslog stores signed integer fields in this type regardless of the
 * platform `long` width so the public 64-bit helpers behave consistently on
 * both 32-bit and 64-bit targets.
 */
#if defined(_MSC_VER)
typedef __int64 pslog_int64;
#elif defined(__GNUC__) || defined(__clang__)
__extension__ typedef signed long long pslog_int64;
#else
typedef signed long long pslog_int64;
#endif

/**
 * Unsigned 64-bit integer payload type used by `pslog_u64()`.
 *
 * libpslog stores unsigned integer fields in this type regardless of the
 * platform `unsigned long` width so the public 64-bit helpers behave
 * consistently on both 32-bit and 64-bit targets.
 */
#if defined(_MSC_VER)
typedef unsigned __int64 pslog_uint64;
#elif defined(__GNUC__) || defined(__clang__)
__extension__ typedef unsigned long long pslog_uint64;
#else
typedef unsigned long long pslog_uint64;
#endif

/**
 * Absolute timestamp value used by `pslog_time_field()`.
 *
 * `epoch_seconds` and `nanoseconds` form the wall clock time. The offset is
 * expressed in minutes east of UTC and is used when formatting non-UTC times.
 */
typedef struct pslog_time_value {
  /** Whole seconds since the Unix epoch. */
  long epoch_seconds;
  /** Additional nanoseconds within the second. */
  long nanoseconds;
  /** UTC offset in minutes for local-time rendering. */
  int utc_offset_minutes;
} pslog_time_value;

/**
 * Duration value used by `pslog_duration_field()`.
 *
 * Durations are rendered in the same human-readable style as the Go version of
 * pslog, including `µs` for microseconds.
 */
typedef struct pslog_duration_value {
  /** Whole seconds component of the duration. */
  long seconds;
  /** Additional nanoseconds component of the duration. */
  long nanoseconds;
} pslog_duration_value;

/**
 * Structured field passed to logging calls.
 *
 * Applications normally create these via the `pslog_*` field constructor
 * functions rather than populating the struct manually. The cached metadata
 * fields (`key_len`, `value_len`, `trusted_key`, `trusted_value`,
 * `console_simple_value`) exist so callers can prepare fields once and reuse
 * them efficiently across many log calls. Pointer payloads are borrowed for a
 * direct logging call; `with()` copies them into the returned logger.
 */
typedef struct pslog_field {
  /** Field name. Keys are rendered exactly as provided. */
  const char *key;
  /** Cached byte length of `key`. */
  size_t key_len;
  /** Cached byte length of the string payload when the type carries one. */
  size_t value_len;
  /** Payload kind stored in `as`. */
  pslog_field_type type;
  /** Non-zero when `key` may be emitted without re-validating trust rules. */
  unsigned char trusted_key;
  /** Non-zero when the value may be emitted through the trusted-string path. */
  unsigned char trusted_value;
  /** Non-zero when console output can render the string without quoting. */
  unsigned char console_simple_value;
  /** Type-specific payload. */
  union {
    /** Payload for `PSLOG_FIELD_STRING`. */
    const char *string_value;
    /** Payload for `PSLOG_FIELD_SIGNED`. */
    pslog_int64 signed_value;
    /** Payload for `PSLOG_FIELD_UNSIGNED`. */
    pslog_uint64 unsigned_value;
    /** Payload for `PSLOG_FIELD_DOUBLE`. */
    double double_value;
    /** Payload for `PSLOG_FIELD_BOOL`. */
    int bool_value;
    /** Payload for `PSLOG_FIELD_POINTER`. */
    const void *pointer_value;
    /** Payload for `PSLOG_FIELD_BYTES`. */
    pslog_bytes bytes_value;
    /** Payload for `PSLOG_FIELD_TIME`. */
    pslog_time_value time_value;
    /** Payload for `PSLOG_FIELD_DURATION`. */
    pslog_duration_value duration_value;
  } as;
} pslog_field;

/**
 * Low-level write callback used by `pslog_output`.
 *
 * Implementations should attempt to write exactly `len` bytes from `data`.
 * `written` must be set to the number of bytes actually accepted. Returning `0`
 * indicates success. Returning non-zero reports an error; partial writes are
 * reported through `written`.
 */
typedef int (*pslog_write_fn)(void *userdata, const char *data, size_t len,
                              size_t *written);

/**
 * Low-level close callback used by `pslog_output`.
 *
 * This is called only for outputs marked as owned. Return `0` on success and a
 * non-zero status when the close operation fails.
 */
typedef int (*pslog_close_fn)(void *userdata);

/**
 * Terminal-detection callback used when `PSLOG_COLOR_AUTO` is selected.
 *
 * Return non-zero when the output should be treated as a terminal and therefore
 * receive ANSI color escapes.
 */
typedef int (*pslog_isatty_fn)(void *userdata);

/**
 * Destination used by a logger.
 *
 * The logger writes bytes through `write`, optionally closes the destination
 * through `close`, and consults `isatty` when adaptive color is enabled.
 */
typedef struct pslog_output {
  /** Called for every chunk emitted by the logger. */
  pslog_write_fn write;
  /** Optional close callback for owned outputs. */
  pslog_close_fn close;
  /** Optional terminal-detection callback for adaptive color. */
  pslog_isatty_fn isatty;
  /** Opaque pointer passed to the callbacks above. */
  void *userdata;
  /** Non-zero when the logger owns the output and may close it. */
  int owned;
} pslog_output;

/**
 * Details about a failed or partial write observed through
 * `pslog_observed_output`.
 */
typedef struct pslog_write_failure {
  /** Callback-specific or OS-specific error code, if available. */
  int error_code;
  /** Non-zero when the sink accepted only part of the requested write. */
  int short_write;
  /** Number of bytes the sink reported as written. */
  size_t written;
  /** Number of bytes libpslog attempted to write. */
  size_t attempted;
} pslog_write_failure;

/**
 * Notification callback used by `pslog_observed_output`.
 *
 * This is invoked synchronously on write failure or short write.
 */
typedef void (*pslog_write_failure_fn)(void *userdata,
                                       const pslog_write_failure *failure);

/**
 * Aggregate counters collected by `pslog_observed_output`.
 */
typedef struct pslog_observed_output_stats {
  /** Total number of failed or partial write events seen so far. */
  unsigned long failures;
  /** Number of those failures that were short writes. */
  unsigned long short_writes;
} pslog_observed_output_stats;

/**
 * Wrapper that observes write failures on another output.
 *
 * Applications typically initialize this once with
 * `pslog_observed_output_init()` and then pass `observed.output` into
 * `pslog_config.output`.
 */
typedef struct pslog_observed_output {
  /** Output handle to pass into `pslog_config.output`. */
  pslog_output output;
  /** Wrapped destination that actually receives the writes. */
  pslog_output target;
  /** Optional callback invoked on write failure. */
  pslog_write_failure_fn on_failure;
  /** Opaque pointer forwarded to `on_failure`. */
  void *failure_userdata;
  /** Total number of observed failures. */
  unsigned long failures;
  /** Total number of observed short writes. */
  unsigned long short_writes;
} pslog_observed_output;

/**
 * ANSI palette used by console and colorized JSON emitters.
 *
 * Every member is an ANSI sequence or decoration fragment. The logger treats
 * these as borrowed pointers and does not copy them.
 */
typedef struct pslog_palette {
  /** Color applied to field keys. */
  const char *key;
  /** Color applied to string values. */
  const char *string;
  /** Color applied to numeric values. */
  const char *number;
  /** Color applied to booleans. */
  const char *boolean;
  /** Color applied to null values. */
  const char *null_value;
  /** Color applied to trace level labels. */
  const char *trace;
  /** Color applied to debug level labels. */
  const char *debug;
  /** Color applied to info level labels. */
  const char *info;
  /** Color applied to warn level labels. */
  const char *warn;
  /** Color applied to error level labels. */
  const char *error;
  /** Color applied to fatal level labels. */
  const char *fatal;
  /** Color applied to panic level labels. */
  const char *panic;
  /** Color applied to timestamps. */
  const char *timestamp;
  /** Color applied to the message value. */
  const char *message;
  /** Color applied to the JSON `"msg"` key. */
  const char *message_key;
  /** Reset sequence emitted after colored fragments. */
  const char *reset;
} pslog_palette;

/**
 * Built-in ANSI palettes exposed as stable named objects.
 *
 * These symbols exist so applications can reference palettes directly in code
 * and through LSP/navigation tooling instead of relying only on string lookup.
 * They are immutable and process-lifetime stable.
 */
/** Immutable process-lifetime built-in default ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_default;
/** Immutable process-lifetime built-in Outrun Electric ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_outrun_electric;
/** Immutable process-lifetime built-in iosvkem ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_iosvkem;
/** Immutable process-lifetime built-in Gruvbox ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_gruvbox;
/** Immutable process-lifetime built-in Dracula ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_dracula;
/** Immutable process-lifetime built-in Nord ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_nord;
/** Immutable process-lifetime built-in Tokyo Night ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_tokyo_night;
/** Immutable process-lifetime built-in Solarized Nightfall ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_solarized_nightfall;
/** Immutable process-lifetime built-in Catppuccin Mocha ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_catppuccin_mocha;
/** Immutable process-lifetime built-in light Gruvbox ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_gruvbox_light;
/** Immutable process-lifetime built-in Monokai Vibrant ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_monokai_vibrant;
/** Immutable process-lifetime built-in One Dark Aurora ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_one_dark_aurora;
/** Immutable process-lifetime built-in Synthwave '84 ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_synthwave_84;
/** Immutable process-lifetime built-in Kanagawa ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_kanagawa;
/** Immutable process-lifetime built-in Rose Pine ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_rose_pine;
/** Immutable process-lifetime built-in Rose Pine Dawn ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_rose_pine_dawn;
/** Immutable process-lifetime built-in Everforest ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_everforest;
/** Immutable process-lifetime built-in light Everforest ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_everforest_light;
/** Immutable process-lifetime built-in Night Owl ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_night_owl;
/** Immutable process-lifetime built-in Ayu Mirage ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_ayu_mirage;
/** Immutable process-lifetime built-in light Ayu ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_ayu_light;
/** Immutable process-lifetime built-in One Light ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_one_light;
/** Immutable process-lifetime built-in One Dark ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_one_dark;
/** Immutable process-lifetime built-in Solarized Light ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_solarized_light;
/** Immutable process-lifetime built-in Solarized Dark ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_solarized_dark;
/** Immutable process-lifetime built-in GitHub Light ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_github_light;
/** Immutable process-lifetime built-in GitHub Dark ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_github_dark;
/** Immutable process-lifetime built-in PaperColor Light ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_papercolor_light;
/** Immutable process-lifetime built-in PaperColor Dark ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_papercolor_dark;
/** Immutable process-lifetime built-in Oceanic Next ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_oceanic_next;
/** Immutable process-lifetime built-in Horizon ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_horizon;
/** Immutable process-lifetime built-in Palenight ANSI palette. */
PSLOG_API extern const pslog_palette pslog_builtin_palette_palenight;

/**
 * Logger construction options.
 *
 * Call `pslog_default_config()` first, then override only the members relevant
 * to your use case. That preserves future defaults and keeps configuration
 * forward-compatible.
 */
typedef struct pslog_config {
  /** Console or JSON output mode. */
  pslog_mode mode;
  /** ANSI color policy. */
  pslog_color_mode color;
  /** Policy used for NaN and infinity fields. */
  pslog_non_finite_float_policy non_finite_float_policy;
  /**
   * Internal line chunk size in bytes.
   *
   * Large entries are streamed in multiple writes instead of truncated.
   * Set to zero to use `PSLOG_DEFAULT_LINE_BUFFER_CAPACITY`.
   */
  size_t line_buffer_capacity;
  /** Minimum emitted level. Lower-priority events are dropped. */
  pslog_level min_level;
  /** Non-zero to include timestamps in every emitted event. */
  int timestamps;
  /** Non-zero to render timestamps in UTC. */
  int utc;
  /**
   * Non-zero to emit the effective logger threshold as `loglevel`.
   *
   * This is equivalent to calling `with_level_field()` on the newly created
   * logger, but avoids creating an extra derived logger when the behavior
   * should apply from construction time.
   */
  int with_level_field;
  /**
   * Non-zero to use verbose built-in metadata field names.
   *
   * This expands libpslog's built-in short metadata keys from
   * `ts`/`lvl`/`msg` to `time`/`level`/`message` in both console and JSON
   * output.
   *
   * User-provided field keys are not rewritten.
   */
  int verbose_fields;
  /**
   * Optional timestamp format override.
   *
   * When NULL, libpslog uses its built-in default timestamp formats.
   * Custom values are interpreted as `strftime`-style layouts.
   * `PSLOG_TIME_FORMAT_RFC3339` and `PSLOG_TIME_FORMAT_RFC3339_NANO`
   * are also recognized as explicit built-in layouts.
   */
  const char *time_format;
  /** ANSI palette used when color is enabled; it remains caller-owned. */
  const pslog_palette *palette;
  /** Output callbacks and userdata that receive encoded bytes; they remain
   * caller-owned. */
  pslog_output output;
} pslog_config;

/**
 * Opaque logger handle whose receiver methods are declared below.
 *
 * Use `log->verb(log, ...)` for logger operations and `log->destroy(log)` to
 * release each owned handle.
 */
typedef struct pslog_logger pslog_logger;

/**
 * Logger instance used by applications.
 *
 * The function pointers on this struct are the preferred calling style for C
 * consumers. Derived loggers created through `with()`, `withf()`,
 * `with_level()`, or `with_level_field()` share the underlying sink but carry
 * their own static prefix state.
 *
 * Logging through shared logger trees is serialized internally, so concurrent
 * `log()/info()/infof()/...` calls from multiple threads are safe. Destruction
 * of the exact same `pslog_logger *` while another thread is still actively
 * using that pointer remains caller-synchronized, which is the usual lifetime
 * rule for C objects.
 */
struct pslog_logger {
  /** Opaque implementation pointer owned by libpslog. */
  void *impl;

  /**
   * Flushes and closes the owned output for this logger tree.
   *
   * Child loggers derived from another logger do not take ownership away from
   * the root. Closing a child does not close the shared root-owned sink.
   *
   * Returns `0` on success or the sink close status on failure.
   */
  int (*close)(pslog_logger *log);

  /**
   * Destroys the logger instance.
   *
   * This releases logger-owned memory. If the logger owns the configured
   * output, destruction also closes that output once.
   */
  void (*destroy)(pslog_logger *log);

  /**
   * Returns a derived logger with additional static fields.
   *
   * The returned logger inherits the receiver's output, mode, color policy,
   * minimum level, timestamp settings, and any previously attached static
   * fields, then appends `fields[0..count)` to that static field set.
   *
   * The receiver is not mutated. The returned logger can be used
   * independently from the parent and must be destroyed separately. Field data
   * is copied into logger-owned storage, so the caller only needs to keep the
   * input array alive for the duration of the `with()` call itself.
   *
   * Passing `NULL` with `count == 0` returns a structural clone of the
   * receiver without adding fields.
   *
   * Returns NULL when allocation fails.
   */
  pslog_logger *(*with)(pslog_logger *log, const pslog_field *fields,
                        size_t count);

  /**
   * Returns a derived logger with additional static fields described by
   * `kvfmt`.
   *
   * This is the derived-logger counterpart to `infof`/`logf`: `kvfmt` is
   * parsed once at derivation time, the resulting fields are stored on the
   * child logger, and subsequent log calls reuse them as ordinary static
   * fields.
   *
   * `kvfmt` uses the same `key=%verb` grammar as `logf`/`infof`/etc. For
   * example: `"service=%s attempt=%d ok=%b"`.
   *
   * Passing `NULL` for `kvfmt` clones the logger without adding fields. The
   * receiver is not mutated.
   *
   * Returns NULL when allocation fails or when `kvfmt` is invalid.
   */
  pslog_logger *(*withf)(pslog_logger *log, const char *kvfmt, ...);

  /**
   * Returns a derived logger with a different minimum level.
   *
   * This is the C equivalent of taking a configured sub-logger for a
   * subsystem.
   *
   * Returns NULL when allocation fails.
   */
  pslog_logger *(*with_level)(pslog_logger *log, pslog_level level);

  /**
   * Returns a derived logger that emits the effective level as a normal field.
   *
   * In JSON mode this adds a `loglevel` field. In console mode it appends the
   * same information as a structured key/value pair.
   *
   * Returns NULL when allocation fails.
   */
  pslog_logger *(*with_level_field)(pslog_logger *log);

  /**
   * Core structured logging entry point.
   *
   * `fields` may be NULL when `count` is zero.
   */
  void (*log)(pslog_logger *log, pslog_level level, const char *msg,
              const pslog_field *fields, size_t count);

  /** Emits a trace event through `log()`. */
  void (*trace)(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count);
  /** Emits a debug event through `log()`. */
  void (*debug)(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count);
  /** Emits an info event through `log()`. */
  void (*info)(pslog_logger *log, const char *msg, const pslog_field *fields,
               size_t count);
  /** Emits a warn event through `log()`. */
  void (*warn)(pslog_logger *log, const char *msg, const pslog_field *fields,
               size_t count);
  /** Emits an error event through `log()`. */
  void (*error)(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count);
  /**
   * Emits a fatal event and then terminates the process with exit status 1.
   *
   * Code after this call is not expected to run.
   */
  void (*fatal)(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count);
  /**
   * Emits a panic event and then aborts the process.
   *
   * Code after this call is not expected to run.
   */
  void (*panic)(pslog_logger *log, const char *msg, const pslog_field *fields,
                size_t count);

  /**
   * Structured `kvfmt` logging entry point.
   *
   * `msg` is emitted literally. `kvfmt` describes the following variadic
   * values using space-separated `key=%verb` tokens. Supported verbs are the
   * ones documented by the typed `*f` helpers below, for example `%s`, `%d`,
   * `%u`, `%ld`, `%lu`, `%f`, `%p`, `%b`, and `%m`. `%m` captures the
   * current `errno` value at log-call entry, consumes no variadic argument,
   * and renders the resolved error text using error coloring.
   *
   * Pass `NULL` for `kvfmt` to emit only the message.
   */
  void (*logf)(pslog_logger *log, pslog_level level, const char *msg,
               const char *kvfmt, ...);

  /** Emits a trace event using `kvfmt` structured arguments. */
  void (*tracef)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /** Emits a debug event using `kvfmt` structured arguments. */
  void (*debugf)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /** Emits an info event using `kvfmt` structured arguments. */
  void (*infof)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /** Emits a warn event using `kvfmt` structured arguments. */
  void (*warnf)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /** Emits an error event using `kvfmt` structured arguments. */
  void (*errorf)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /**
   * Emits a fatal event using `kvfmt` structured arguments, then exits with
   * status 1.
   */
  void (*fatalf)(pslog_logger *log, const char *msg, const char *kvfmt, ...);
  /**
   * Emits a panic event using `kvfmt` structured arguments, then aborts the
   * process.
   */
  void (*panicf)(pslog_logger *log, const char *msg, const char *kvfmt, ...);

  /** Length-aware message counterparts. Views are borrowed until return. */
  void (*log_view)(pslog_logger *log, pslog_level level, pslog_string_view msg,
                   const pslog_field *fields, size_t count);
  void (*trace_view)(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count);
  void (*debug_view)(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count);
  void (*info_view)(pslog_logger *log, pslog_string_view msg,
                    const pslog_field *fields, size_t count);
  void (*warn_view)(pslog_logger *log, pslog_string_view msg,
                    const pslog_field *fields, size_t count);
  void (*error_view)(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count);
  void (*fatal_view)(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count);
  void (*panic_view)(pslog_logger *log, pslog_string_view msg,
                     const pslog_field *fields, size_t count);
  /** `kvfmt` and its `%s` arguments remain null-terminated. */
  void (*logf_view)(pslog_logger *log, pslog_level level, pslog_string_view msg,
                    const char *kvfmt, ...);
  void (*tracef_view)(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...);
  void (*debugf_view)(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...);
  void (*infof_view)(pslog_logger *log, pslog_string_view msg,
                     const char *kvfmt, ...);
  void (*warnf_view)(pslog_logger *log, pslog_string_view msg,
                     const char *kvfmt, ...);
  void (*errorf_view)(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...);
  void (*fatalf_view)(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...);
  void (*panicf_view)(pslog_logger *log, pslog_string_view msg,
                      const char *kvfmt, ...);
};

/**
 * Fills `config` with the library defaults.
 *
 * This is the required starting point for building a `pslog_config`. Passing
 * NULL is a no-op.
 */
PSLOG_API void pslog_default_config(pslog_config *config);

/**
 * Creates a new logger from `config`.
 *
 * Passing NULL uses the defaults from `pslog_default_config()`. The config is
 * read during construction; its palette, output callbacks, and output userdata
 * remain borrowed and must outlive the logger. Returns NULL when allocation
 * fails or the configured line-buffer capacity is invalid.
 */
PSLOG_API pslog_logger *pslog_new(const pslog_config *config);

/**
 * Creates a new logger from environment overrides layered on top of `config`.
 *
 * `prefix` may be NULL to use the default `LOG_` prefix. The seed config is
 * copied first, then environment variables override selected members. This is
 * the C equivalent of "logger from env" setup in the Go implementation.
 * Passing NULL for `config` starts from library defaults. Its palette, output
 * callbacks, and output userdata follow the same borrowed-lifetime rule as
 * `pslog_new()`.
 *
 * Returns NULL for the same allocation and configuration failures as
 * `pslog_new()`.
 */
PSLOG_API pslog_logger *pslog_new_from_env(const char *prefix,
                                           const pslog_config *config);

/**
 * Builds a `pslog_output` from a `FILE *`.
 *
 * `fp` must remain usable until ownership transfers to a logger or the output
 * is destroyed. When `close_on_destroy` is non-zero, destroying the logger
 * also closes `fp`; otherwise it remains caller-owned.
 */
PSLOG_API pslog_output pslog_output_from_fp(FILE *fp, int close_on_destroy);

/**
 * Initializes `output` to append to a file path.
 *
 * `mode` uses the file-backend append/write modes; NULL selects append mode.
 * `output` and `path` must be non-NULL. On success, `output` owns the opened
 * file and can be transferred to a logger or released with
 * `pslog_output_destroy()`.
 *
 * Returns `0` on success, an errno-style code for open failures, or `-1` for
 * invalid input or allocation failure.
 */
PSLOG_API int pslog_output_init_file(pslog_output *output, const char *path,
                                     const char *mode);

/**
 * Destroys a standalone output object previously initialized by libpslog.
 *
 * This is mainly useful when an output is constructed before logger creation
 * and later discarded without being transferred into a logger. Passing NULL is
 * a no-op. The object is cleared after an owned output is closed.
 */
PSLOG_API void pslog_output_destroy(pslog_output *output);

/**
 * Wraps `target` so failures can be observed while preserving normal writes.
 *
 * The resulting wrapper is written into `observed->output`. When
 * `target->owned` is non-zero, ownership transfers to the wrapper output and
 * the original `pslog_output` must be treated as moved-from for ownership
 * purposes: do not destroy, close, or transfer it again unless you first clear
 * or override the copied ownership state yourself.
 */
PSLOG_API void pslog_observed_output_init(pslog_observed_output *observed,
                                          const pslog_output *target,
                                          pslog_write_failure_fn on_failure,
                                          void *failure_userdata);

/**
 * Returns cumulative failure statistics from an observed output wrapper.
 *
 * Passing NULL returns zero counters.
 */
PSLOG_API pslog_observed_output_stats
pslog_observed_output_stats_get(const pslog_observed_output *observed);

/**
 * Closes a logger when it owns its output.
 *
 * This is the free-function equivalent of `log->close(log)`. Passing NULL is a
 * no-op that returns `0`, which makes it suitable for straightforward cleanup
 * paths where the logger may be optional.
 */
PSLOG_API int pslog_close(pslog_logger *log);

/**
 * Returns the canonical lowercase string for `level`.
 *
 * Values outside `pslog_level` return the fallback string `"info"`.
 */
PSLOG_API const char *pslog_level_string(pslog_level level);

/**
 * Parses a level name such as `"debug"` or `"warn"`.
 *
 * Matching is case-insensitive and leading and trailing ASCII whitespace is
 * ignored. `"warning"` aliases `"warn"`; `"no"` and `"none"` alias
 * `"nolevel"`; `"off"` aliases `"disabled"`. Returns non-zero on success
 * and leaves `level` unchanged on failure. NULL input or output pointers fail.
 */
PSLOG_API int pslog_parse_level(const char *text, pslog_level *level);

/**
 * Returns non-zero when `text` satisfies libpslog's trusted-string contract.
 *
 * Trusted strings may skip repeated validation and escaping work on the hot
 * path. Use this only when you want to explicitly reason about trust decisions;
 * most callers should just use `pslog_str()` or `pslog_trusted_str()`. NULL is
 * treated as the empty trusted string.
 */
PSLOG_API int pslog_string_is_trusted(const char *text);

/**
 * Returns the built-in default ANSI palette.
 */
PSLOG_API const pslog_palette *pslog_palette_default(void);

/**
 * Looks up a built-in palette by canonical name or supported alias.
 *
 * Unknown names fall back to the built-in default palette. Passing NULL or an
 * empty string also returns the default palette.
 */
PSLOG_API const pslog_palette *pslog_palette_by_name(const char *name);

/**
 * Returns the number of built-in palettes available through the palette lookup
 * APIs.
 */
PSLOG_API size_t pslog_palette_count(void);

/**
 * Returns the canonical name of the palette at `index`.
 *
 * The returned pointer remains valid for the lifetime of the process. Returns
 * NULL when `index` is outside `pslog_palette_count()`.
 */
PSLOG_API const char *pslog_palette_name(size_t index);

/** Builds a field that renders as a null value. */
PSLOG_API pslog_field pslog_null(const char *key);

/**
 * Builds a string field.
 *
 * libpslog automatically caches key/value lengths and trust metadata so the
 * field can be reused efficiently. Both strings are borrowed until a direct
 * logging call returns; `with()` makes owned copies for a child logger.
 */
PSLOG_API pslog_field pslog_str(const char *key, const char *value);
/**
 * Creates a field that renders an errno-style integer code as error text.
 *
 * Snapshot `errno` first instead of passing it directly, for example:
 * `int err = errno; pslog_errno("error", err)`.
 *
 * The stored integer is resolved to text inside libpslog and rendered with
 * error coloring rather than normal string coloring.
 */
PSLOG_API pslog_field pslog_errno(const char *key, int err);

/**
 * Builds a string field that explicitly opts into the trusted-string fast path.
 *
 * Trust is applied only when the provided key and value satisfy the trusted
 * string rules. Unsafe inputs still fall back to normal escaping. Both strings
 * follow the borrowed-lifetime rule of `pslog_str()`.
 */
PSLOG_API pslog_field pslog_trusted_str(const char *key, const char *value);

/** Builds a signed integer field rendered numerically. */
PSLOG_API pslog_field pslog_i64(const char *key, pslog_int64 value);
/** Builds an unsigned integer field rendered numerically. */
PSLOG_API pslog_field pslog_u64(const char *key, pslog_uint64 value);
/** Builds a floating-point field using libpslog's configured float policy. */
PSLOG_API pslog_field pslog_f64(const char *key, double value);
/** Builds a boolean field. Zero is false, non-zero is true. */
PSLOG_API pslog_field pslog_bool(const char *key, int value);
/** Builds a pointer field rendered in pointer notation. */
PSLOG_API pslog_field pslog_ptr(const char *key, const void *value);

/**
 * Builds a bytes field.
 *
 * The data is borrowed, not copied. The bytes are rendered as a hex string.
 */
PSLOG_API pslog_field pslog_bytes_field(const char *key, const void *data,
                                        size_t len);

/**
 * Builds a wall-clock timestamp field.
 *
 * The offset is expressed in minutes east of UTC and is used when formatting
 * non-UTC output.
 */
PSLOG_API pslog_field pslog_time_field(const char *key, long epoch_seconds,
                                       long nanoseconds,
                                       int utc_offset_minutes);

/**
 * Builds a duration field rendered in pslog's human-readable duration style.
 */
PSLOG_API pslog_field pslog_duration_field(const char *key, long seconds,
                                           long nanoseconds);

/** Length-aware typed field constructors. Keys and string values are borrowed
 * until the log call returns; `with()` copies their exact byte lengths.
 * Embedded NUL bytes are escaped when rendered. */
PSLOG_API pslog_field pslog_null_view(pslog_string_view key);
PSLOG_API pslog_field pslog_str_view(pslog_string_view key,
                                     pslog_string_view value);
PSLOG_API pslog_field pslog_trusted_str_view(pslog_string_view key,
                                             pslog_string_view value);
PSLOG_API pslog_field pslog_errno_view(pslog_string_view key, int err);
PSLOG_API pslog_field pslog_i64_view(pslog_string_view key, pslog_int64 value);
PSLOG_API pslog_field pslog_u64_view(pslog_string_view key, pslog_uint64 value);
PSLOG_API pslog_field pslog_f64_view(pslog_string_view key, double value);
PSLOG_API pslog_field pslog_bool_view(pslog_string_view key, int value);
PSLOG_API pslog_field pslog_ptr_view(pslog_string_view key, const void *value);
PSLOG_API pslog_field pslog_bytes_field_view(pslog_string_view key,
                                             const void *data, size_t len);
PSLOG_API pslog_field pslog_time_field_view(pslog_string_view key,
                                            long epoch_seconds,
                                            long nanoseconds,
                                            int utc_offset_minutes);
PSLOG_API pslog_field pslog_duration_field_view(pslog_string_view key,
                                                long seconds, long nanoseconds);

/**
 * Emits a structured event through the generic level-aware logging entry
 * point.
 *
 * This is the free-function equivalent of `log->log(...)`. Passing NULL for
 * `log` is a no-op, which makes this convenient in call sites where the logger
 * may be optional.
 */
PSLOG_API void pslog_fields(pslog_logger *log, pslog_level level,
                            const char *msg, const pslog_field *fields,
                            size_t count);

/** Emits a trace event through the free-function API; NULL logger is a no-op.
 */
PSLOG_API void pslog_trace(pslog_logger *log, const char *msg,
                           const pslog_field *fields, size_t count);
/** Emits a debug event through the free-function API; NULL logger is a no-op.
 */
PSLOG_API void pslog_debug(pslog_logger *log, const char *msg,
                           const pslog_field *fields, size_t count);
/** Emits an info event through the free-function API; NULL logger is a no-op.
 */
PSLOG_API void pslog_info(pslog_logger *log, const char *msg,
                          const pslog_field *fields, size_t count);
/** Emits a warn event through the free-function API; NULL logger is a no-op. */
PSLOG_API void pslog_warn(pslog_logger *log, const char *msg,
                          const pslog_field *fields, size_t count);
/** Emits an error event through the free-function API; NULL logger is a no-op.
 */
PSLOG_API void pslog_error(pslog_logger *log, const char *msg,
                           const pslog_field *fields, size_t count);
/**
 * Emits a fatal event through the free-function API.
 *
 * NULL logger is a no-op. Otherwise this logs and exits with status 1.
 */
PSLOG_API void pslog_fatal(pslog_logger *log, const char *msg,
                           const pslog_field *fields, size_t count);
/**
 * Emits a panic event through the free-function API.
 *
 * NULL logger is a no-op. Otherwise this logs and aborts the process.
 */
PSLOG_API void pslog_panic(pslog_logger *log, const char *msg,
                           const pslog_field *fields, size_t count);

/**
 * Emits a structured event using the `kvfmt` variadic path.
 *
 * This is the free-function equivalent of `log->logf(...)`. See
 * `pslog_logger.logf` for the `kvfmt` contract. Passing NULL for `log` is a
 * no-op.
 */
PSLOG_API void pslog(pslog_logger *log, pslog_level level, const char *msg,
                     const char *kvfmt, ...);

/** Emits a trace event through the `kvfmt` free-function path. */
PSLOG_API void pslog_tracef(pslog_logger *log, const char *msg,
                            const char *kvfmt, ...);
/** Emits a debug event through the `kvfmt` free-function path. */
PSLOG_API void pslog_debugf(pslog_logger *log, const char *msg,
                            const char *kvfmt, ...);
/** Emits an info event through the `kvfmt` free-function path. */
PSLOG_API void pslog_infof(pslog_logger *log, const char *msg,
                           const char *kvfmt, ...);
/** Emits a warn event through the `kvfmt` free-function path. */
PSLOG_API void pslog_warnf(pslog_logger *log, const char *msg,
                           const char *kvfmt, ...);
/** Emits an error event through the `kvfmt` free-function path. */
PSLOG_API void pslog_errorf(pslog_logger *log, const char *msg,
                            const char *kvfmt, ...);
/**
 * Emits a fatal event through the `kvfmt` free-function path.
 *
 * NULL logger is a no-op. Otherwise this logs and exits with status 1.
 */
PSLOG_API void pslog_fatalf(pslog_logger *log, const char *msg,
                            const char *kvfmt, ...);
/**
 * Emits a panic event through the `kvfmt` free-function path.
 *
 * NULL logger is a no-op. Otherwise this logs and aborts the process.
 */
PSLOG_API void pslog_panicf(pslog_logger *log, const char *msg,
                            const char *kvfmt, ...);

/** Length-aware message functions. `msg.data` may be NULL only for zero length.
 * `kvfmt` and `%s` arguments in the `*f_view` functions remain C strings. */
PSLOG_API void pslog_fields_view(pslog_logger *log, pslog_level level,
                                 pslog_string_view msg,
                                 const pslog_field *fields, size_t count);
PSLOG_API void pslog_trace_view(pslog_logger *log, pslog_string_view msg,
                                const pslog_field *fields, size_t count);
PSLOG_API void pslog_debug_view(pslog_logger *log, pslog_string_view msg,
                                const pslog_field *fields, size_t count);
PSLOG_API void pslog_info_view(pslog_logger *log, pslog_string_view msg,
                               const pslog_field *fields, size_t count);
PSLOG_API void pslog_warn_view(pslog_logger *log, pslog_string_view msg,
                               const pslog_field *fields, size_t count);
PSLOG_API void pslog_error_view(pslog_logger *log, pslog_string_view msg,
                                const pslog_field *fields, size_t count);
PSLOG_API void pslog_fatal_view(pslog_logger *log, pslog_string_view msg,
                                const pslog_field *fields, size_t count);
PSLOG_API void pslog_panic_view(pslog_logger *log, pslog_string_view msg,
                                const pslog_field *fields, size_t count);
PSLOG_API void pslog_view(pslog_logger *log, pslog_level level,
                          pslog_string_view msg, const char *kvfmt, ...);
PSLOG_API void pslog_tracef_view(pslog_logger *log, pslog_string_view msg,
                                 const char *kvfmt, ...);
PSLOG_API void pslog_debugf_view(pslog_logger *log, pslog_string_view msg,
                                 const char *kvfmt, ...);
PSLOG_API void pslog_infof_view(pslog_logger *log, pslog_string_view msg,
                                const char *kvfmt, ...);
PSLOG_API void pslog_warnf_view(pslog_logger *log, pslog_string_view msg,
                                const char *kvfmt, ...);
PSLOG_API void pslog_errorf_view(pslog_logger *log, pslog_string_view msg,
                                 const char *kvfmt, ...);
PSLOG_API void pslog_fatalf_view(pslog_logger *log, pslog_string_view msg,
                                 const char *kvfmt, ...);
PSLOG_API void pslog_panicf_view(pslog_logger *log, pslog_string_view msg,
                                 const char *kvfmt, ...);

/**
 * Derives a child logger with additional static fields.
 *
 * This is the free-function equivalent of `log->with(...)`. The returned
 * logger inherits the receiver configuration and existing static fields, then
 * appends the provided `fields`.
 *
 * The receiver is not mutated. Field data is copied into logger-owned
 * storage. Passing `NULL` with `count == 0` clones the logger without adding
 * fields.
 *
 * Returns NULL when `log` is NULL or when allocation fails.
 */
PSLOG_API pslog_logger *pslog_with(pslog_logger *log, const pslog_field *fields,
                                   size_t count);

/**
 * Derives a child logger with additional static fields described by `kvfmt`.
 *
 * This is the free-function equivalent of `log->withf(...)`. The receiver is
 * not mutated.
 *
 * `kvfmt` follows the same `key=%verb` contract as
 * `tracef/debugf/infof/...`, but the parsed fields become part of the
 * returned child logger's static field set instead of being emitted on a
 * single event.
 *
 * `%m` is supported here as well and snapshots `errno` at `withf()` call
 * entry before deriving the child logger.
 *
 * Example:
 * `pslog_withf(log, "service=%s attempt=%d", "api", 3)`.
 *
 * Passing `NULL` for `kvfmt` clones the logger without adding fields. Returns
 * NULL when `log` is NULL, when `kvfmt` is invalid, or when allocation fails.
 */
PSLOG_API pslog_logger *pslog_withf(pslog_logger *log, const char *kvfmt, ...);

/**
 * Derives a child logger with a different minimum enabled level.
 *
 * This does not mutate the original logger. Returns NULL when `log` is NULL or
 * allocation fails.
 */
PSLOG_API pslog_logger *pslog_with_level(pslog_logger *log, pslog_level level);

/**
 * Derives a child logger that adds the effective level as a static field.
 *
 * This is useful when downstream consumers want the level duplicated into the
 * structured field set. Returns NULL when `log` is NULL or allocation fails.
 */
PSLOG_API pslog_logger *pslog_with_level_field(pslog_logger *log);

#ifdef __cplusplus
}
#endif

#endif
