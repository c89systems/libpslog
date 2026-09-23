#if (defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)) &&       \
    !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "bench/bench_fixture.h"
#include "pslog.h"
#if !defined(PSLOG_SINGLE_HEADER_TEST)
#include "src/pslog_internal.h"
#endif

#include <jansson.h>

#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define TEST_ASSERT(expr)                                                      \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fprintf(stderr, "assertion failed: %s (%s:%d)\n", #expr, __FILE__,       \
              __LINE__);                                                       \
      return 1;                                                                \
    }                                                                          \
  } while (0)

struct memory_sink {
  char data[131072];
  size_t len;
  int writes;
};

struct close_tracking_sink {
  int closed;
  int writes;
};

struct short_write_memory_sink {
  char data[131072];
  size_t len;
  size_t max_write;
  int writes;
};

struct write_failure_sink {
  int calls;
  pslog_write_failure last;
};

struct partial_sink {
  int closes;
  int fail_mode;
};

struct null_userdata_tracking_sink {
  int writes;
  int closes;
  int isatty_calls;
  char data[4096];
  size_t len;
};

struct isatty_tracking_sink {
  struct memory_sink output;
  void *expected_userdata;
  int calls;
  int wrong_userdata;
};

struct fake_time_source {
  time_t seconds;
  long nanoseconds;
};

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
struct threaded_log_context {
  pslog_logger *log;
  int thread_id;
  int iterations;
  int use_kvfmt;
  int use_withf;
  int use_double;
  int failed;
  const char *payload;
  const char *kvfmt;
};

struct threaded_time_context {
  pslog_logger *log;
  int thread_id;
  int iterations;
  pslog_time_value when;
};
#endif

static int memory_sink_write(void *userdata, const char *data, size_t len,
                             size_t *written) {
  struct memory_sink *sink;
  size_t copy_len;

  sink = (struct memory_sink *)userdata;
  sink->writes += 1;
  if (sink->len >= sizeof(sink->data) - 1u) {
    if (written != NULL) {
      *written = 0u;
    }
    return -1;
  }
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
  return copy_len == len ? 0 : -1;
}

static int memory_sink_isatty(void *userdata) {
  (void)userdata;
  return 0;
}

static int memory_sink_isatty_true(void *userdata) {
  (void)userdata;
  return 1;
}

static int isatty_tracking_write(void *userdata, const char *data, size_t len,
                                 size_t *written) {
  struct isatty_tracking_sink *sink;

  sink = (struct isatty_tracking_sink *)userdata;
  return memory_sink_write(&sink->output, data, len, written);
}

static int isatty_tracking_isatty(void *userdata) {
  struct isatty_tracking_sink *sink;

  sink = (struct isatty_tracking_sink *)userdata;
  if (sink == NULL) {
    return 0;
  }
  sink->calls += 1;
  if (userdata != sink->expected_userdata) {
    sink->wrong_userdata += 1;
    return 0;
  }
  return 1;
}

static int close_tracking_write(void *userdata, const char *data, size_t len,
                                size_t *written) {
  struct close_tracking_sink *sink;

  sink = (struct close_tracking_sink *)userdata;
  sink->writes += 1;
  (void)data;
  (void)len;
  if (written != NULL) {
    *written = len;
  }
  return 0;
}

static int close_tracking_close(void *userdata) {
  struct close_tracking_sink *sink;

  sink = (struct close_tracking_sink *)userdata;
  sink->closed += 1;
  return 0;
}

static int short_write_memory_sink_write(void *userdata, const char *data,
                                         size_t len, size_t *written) {
  struct short_write_memory_sink *sink;
  size_t copy_len;

  sink = (struct short_write_memory_sink *)userdata;
  sink->writes += 1;
  if (sink->len >= sizeof(sink->data) - 1u) {
    if (written != NULL) {
      *written = 0u;
    }
    return -1;
  }
  copy_len = len;
  if (sink->max_write > 0u && copy_len > sink->max_write) {
    copy_len = sink->max_write;
  }
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

static int partial_sink_write(void *userdata, const char *data, size_t len,
                              size_t *written) {
  struct partial_sink *sink;

  sink = (struct partial_sink *)userdata;
  (void)data;
  if (written != NULL) {
    *written = len > 0u ? len - 1u : 0u;
  }
  return sink->fail_mode;
}

static void fake_time_now(void *userdata, time_t *seconds_out,
                          long *nanoseconds_out) {
  struct fake_time_source *source;

  source = (struct fake_time_source *)userdata;
  if (seconds_out != NULL) {
    *seconds_out = source != NULL ? source->seconds : 0;
  }
  if (nanoseconds_out != NULL) {
    *nanoseconds_out = source != NULL ? source->nanoseconds : 0l;
  }
}

static int partial_sink_close(void *userdata) {
  struct partial_sink *sink;

  sink = (struct partial_sink *)userdata;
  sink->closes += 1;
  return sink->fail_mode;
}

static struct null_userdata_tracking_sink g_null_userdata_sink;

static int null_userdata_write(void *userdata, const char *data, size_t len,
                               size_t *written) {
  size_t copy_len;

  (void)userdata;
  g_null_userdata_sink.writes += 1;
  copy_len = len;
  if (copy_len >
      sizeof(g_null_userdata_sink.data) - g_null_userdata_sink.len - 1u) {
    copy_len =
        sizeof(g_null_userdata_sink.data) - g_null_userdata_sink.len - 1u;
  }
  if (copy_len > 0u) {
    memcpy(g_null_userdata_sink.data + g_null_userdata_sink.len, data,
           copy_len);
    g_null_userdata_sink.len += copy_len;
    g_null_userdata_sink.data[g_null_userdata_sink.len] = '\0';
  }
  if (written != NULL) {
    *written = len;
  }
  return 0;
}

static int null_userdata_close(void *userdata) {
  (void)userdata;
  g_null_userdata_sink.closes += 1;
  return 0;
}

static int null_userdata_isatty_true(void *userdata) {
  (void)userdata;
  g_null_userdata_sink.isatty_calls += 1;
  return 1;
}

static void reset_null_userdata_sink(void) {
  memset(&g_null_userdata_sink, 0, sizeof(g_null_userdata_sink));
}

static void write_failure_callback(void *userdata,
                                   const pslog_write_failure *failure) {
  struct write_failure_sink *sink;

  sink = (struct write_failure_sink *)userdata;
  sink->calls += 1;
  sink->last = *failure;
}

static void reset_sink(struct memory_sink *sink) {
  sink->len = 0u;
  sink->writes = 0;
  sink->data[0] = '\0';
}

static int contains_text(const char *haystack, const char *needle) {
  return strstr(haystack, needle) != NULL;
}

static int contains_bytes(const char *haystack, size_t haystack_len,
                          const char *needle, size_t needle_len) {
  size_t i;

  if (needle_len == 0u) {
    return 1;
  }
  if (haystack == NULL || needle == NULL || haystack_len < needle_len) {
    return 0;
  }
  for (i = 0u; i <= haystack_len - needle_len; ++i) {
    if (memcmp(haystack + i, needle, needle_len) == 0) {
      return 1;
    }
  }
  return 0;
}

static int assert_valid_json_lines(const char *text) {
  json_error_t error;
  const char *line_start;
  const char *cursor;

  if (text == NULL) {
    return 1;
  }
  line_start = text;
  cursor = text;
  while (1) {
    if (*cursor == '\n' || *cursor == '\0') {
      size_t line_len;

      line_len = (size_t)(cursor - line_start);
      if (line_len > 0u) {
        json_t *value;

        memset(&error, 0, sizeof(error));
        value = json_loadb(line_start, line_len,
                           JSON_REJECT_DUPLICATES | JSON_ALLOW_NUL, &error);
        if (value == NULL) {
          fprintf(stderr, "invalid json line: %s (line %d, column %d)\n",
                  error.text, error.line, error.column);
          return 1;
        }
        json_decref(value);
      }
      if (*cursor == '\0') {
        break;
      }
      line_start = cursor + 1;
    }
    ++cursor;
  }
  return 0;
}

static size_t count_text_occurrences(const char *haystack, const char *needle) {
  size_t count;
  size_t needle_len;
  const char *cursor;

  if (haystack == NULL || needle == NULL || *needle == '\0') {
    return 0u;
  }
  count = 0u;
  needle_len = strlen(needle);
  cursor = haystack;
  while ((cursor = strstr(cursor, needle)) != NULL) {
    ++count;
    cursor += needle_len;
  }
  return count;
}

static unsigned long
allocator_total_calls(const pslog_test_allocator_stats *stats) {
  if (stats == NULL) {
    return 0u;
  }
  return stats->malloc_calls + stats->calloc_calls + stats->realloc_calls;
}

static int assert_allocator_no_growth(const pslog_test_allocator_stats *before,
                                      const pslog_test_allocator_stats *after) {
  if (allocator_total_calls(after) != allocator_total_calls(before)) {
    fprintf(stderr,
            "allocation regression: before=%lu after=%lu (malloc=%lu "
            "calloc=%lu realloc=%lu)\n",
            allocator_total_calls(before), allocator_total_calls(after),
            after != NULL ? after->malloc_calls : 0u,
            after != NULL ? after->calloc_calls : 0u,
            after != NULL ? after->realloc_calls : 0u);
    return 1;
  }
  return 0;
}

#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
static void *threaded_log_worker(void *userdata) {
  struct threaded_log_context *ctx;
  int i;

  ctx = (struct threaded_log_context *)userdata;
  for (i = 0; i < ctx->iterations; ++i) {
    if (ctx->use_kvfmt) {
      const char *kvfmt;

      kvfmt = ctx->kvfmt;
      if (kvfmt == NULL) {
        if (ctx->payload != NULL) {
          kvfmt = "tid=%d seq=%d payload=%s";
        } else {
          kvfmt = "tid=%d seq=%d";
        }
      }
      if (ctx->use_withf) {
        pslog_logger *child;

        if (ctx->payload != NULL) {
          child =
              ctx->log->withf(ctx->log, kvfmt, ctx->thread_id, i, ctx->payload);
        } else {
          child = ctx->log->withf(ctx->log, kvfmt, ctx->thread_id, i);
        }
        if (child == NULL) {
          ctx->failed = 1;
          return NULL;
        }
        child->info(child, "thread", NULL, 0u);
        child->destroy(child);
      } else {
        if (ctx->payload != NULL) {
          ctx->log->infof(ctx->log, "thread", kvfmt, ctx->thread_id, i,
                          ctx->payload);
        } else {
          ctx->log->infof(ctx->log, "thread", kvfmt, ctx->thread_id, i);
        }
      }
    } else {
      pslog_field fields[4];
      size_t field_count;

      field_count = 0u;
      fields[field_count++] = pslog_i64("tid", (long)ctx->thread_id);
      fields[field_count++] = pslog_i64("seq", (long)i);
      if (ctx->use_double) {
        fields[field_count++] =
            pslog_f64("ratio", (double)((ctx->thread_id % 4) + 1) + 0.25);
      }
      if (ctx->payload != NULL) {
        fields[field_count++] = pslog_str("payload", ctx->payload);
      }
      ctx->log->info(ctx->log, "thread", fields, field_count);
    }
  }
  return NULL;
}

static void *threaded_time_worker(void *userdata) {
  struct threaded_time_context *ctx;
  int i;

  ctx = (struct threaded_time_context *)userdata;
  for (i = 0; i < ctx->iterations; ++i) {
    pslog_field fields[2];

    fields[0] = pslog_i64("tid", (pslog_int64)ctx->thread_id);
    fields[1] =
        pslog_time_field("when", ctx->when.epoch_seconds, ctx->when.nanoseconds,
                         ctx->when.utc_offset_minutes);
    ctx->log->info(ctx->log, "thread-time", fields, 2u);
  }
  return NULL;
}
#endif

static int run_terminating_logger_child(int panic_mode, pslog_level min_level,
                                        char *output, size_t output_size,
                                        int *exit_code, int *term_signal) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  int pipefd[2];
  pid_t pid;
  ssize_t nread;
  size_t total;
  int status;

  if (output == NULL || output_size == 0u || exit_code == NULL ||
      term_signal == NULL) {
    return -1;
  }
  output[0] = '\0';
  *exit_code = -1;
  *term_signal = -1;
  if (pipe(pipefd) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    pslog_config config;
    pslog_logger *log;
    pslog_logger *target;
    pslog_field field;

    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
      _exit(120);
    }
    close(pipefd[1]);
    pslog_default_config(&config);
    config.mode = PSLOG_MODE_JSON;
    config.color = PSLOG_COLOR_NEVER;
    config.timestamps = 0;
    config.output = pslog_output_from_fp(stdout, 0);
    log = pslog_new(&config);
    if (log == NULL) {
      _exit(121);
    }
    target = log;
    if (min_level != PSLOG_LEVEL_INFO) {
      target = log->with_level(log, min_level);
      if (target == NULL) {
        _exit(123);
      }
    }
    field = pslog_str("key", "value");
    if (panic_mode) {
      target->panic(target, "panic-child", &field, 1u);
    } else {
      target->fatal(target, "fatal-child", &field, 1u);
    }
    _exit(122);
  }

  close(pipefd[1]);
  total = 0u;
  while (total + 1u < output_size) {
    nread = read(pipefd[0], output + total, output_size - total - 1u);
    if (nread <= 0) {
      break;
    }
    total += (size_t)nread;
  }
  output[total] = '\0';
  close(pipefd[0]);
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
    *term_signal = 0;
    return 0;
  }
  if (WIFSIGNALED(status)) {
    *exit_code = -1;
    *term_signal = WTERMSIG(status);
    return 0;
  }
  return -1;
#else
  (void)panic_mode;
  (void)min_level;
  (void)output;
  (void)output_size;
  (void)exit_code;
  (void)term_signal;
  return 0;
#endif
}

#if !defined(PSLOG_COVERAGE_BUILD)
static int run_terminating_wrapper_child(int panic_mode, int kvfmt_mode,
                                         pslog_level min_level, char *output,
                                         size_t output_size, int *exit_code,
                                         int *term_signal) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  int pipefd[2];
  pid_t pid;
  ssize_t nread;
  size_t total;
  int status;

  if (output == NULL || output_size == 0u || exit_code == NULL ||
      term_signal == NULL) {
    return -1;
  }
  output[0] = '\0';
  *exit_code = -1;
  *term_signal = -1;
  if (pipe(pipefd) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    pslog_config config;
    pslog_logger *log;
    pslog_logger *target;
    pslog_field field;

    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
      _exit(120);
    }
    close(pipefd[1]);
    pslog_default_config(&config);
    config.mode = PSLOG_MODE_JSON;
    config.color = PSLOG_COLOR_NEVER;
    config.timestamps = 0;
    config.output = pslog_output_from_fp(stdout, 0);
    log = pslog_new(&config);
    if (log == NULL) {
      _exit(121);
    }
    target = log;
    if (min_level != PSLOG_LEVEL_INFO) {
      target = log->with_level(log, min_level);
      if (target == NULL) {
        _exit(123);
      }
    }
    field = pslog_str("key", "value");
    if (panic_mode) {
      if (kvfmt_mode == 4) {
        pslog_panicf_view(target, (pslog_string_view){"p\0x", 3u}, "key=%s",
                          "value");
      } else if (kvfmt_mode) {
        pslog_panicf(target, "panic-child-kvfmt",
                     kvfmt_mode == 2 ? "bad" : "key=%s", "value");
      } else {
        pslog_panic(target, "panic-child-wrapper", &field, 1u);
      }
    } else {
      if (kvfmt_mode == 3) {
        pslog_fatal_view(target, (pslog_string_view){"f\0x", 3u}, &field, 1u);
      } else if (kvfmt_mode) {
        pslog_fatalf(target, "fatal-child-kvfmt",
                     kvfmt_mode == 2 ? "bad" : "key=%s", "value");
      } else {
        pslog_fatal(target, "fatal-child-wrapper", &field, 1u);
      }
    }
    _exit(122);
  }

  close(pipefd[1]);
  total = 0u;
  while (total + 1u < output_size) {
    nread = read(pipefd[0], output + total, output_size - total - 1u);
    if (nread <= 0) {
      break;
    }
    total += (size_t)nread;
  }
  output[total] = '\0';
  close(pipefd[0]);
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
    *term_signal = 0;
    return 0;
  }
  if (WIFSIGNALED(status)) {
    *exit_code = -1;
    *term_signal = WTERMSIG(status);
    return 0;
  }
  return -1;
#else
  (void)panic_mode;
  (void)kvfmt_mode;
  (void)min_level;
  (void)output;
  (void)output_size;
  (void)exit_code;
  (void)term_signal;
  return 0;
#endif
}

#endif

static int run_kvfmt_shorter_reused_buffer_child(int use_withf, char *output,
                                                 size_t output_size,
                                                 int *exit_code,
                                                 int *term_signal) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  int pipefd[2];
  pid_t pid;
  ssize_t nread;
  size_t total;
  int status;

  if (output == NULL || output_size == 0u || exit_code == NULL ||
      term_signal == NULL) {
    return -1;
  }
  output[0] = '\0';
  *exit_code = -1;
  *term_signal = -1;
  if (pipe(pipefd) != 0) {
    return -1;
  }
  pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    return -1;
  }
  if (pid == 0) {
    long page_size;
    void *mapping;
    char *kvfmt;
    pslog_config config;
    pslog_logger *log;

    close(pipefd[0]);
    if (dup2(pipefd[1], STDOUT_FILENO) < 0) {
      _exit(120);
    }
    close(pipefd[1]);

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
      _exit(121);
    }
#if defined(MAP_ANON)
    mapping = mmap(NULL, (size_t)page_size * 2u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANON, -1, 0);
#else
    mapping = mmap(NULL, (size_t)page_size * 2u, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
#endif
    if (mapping == MAP_FAILED) {
      _exit(122);
    }

    pslog_default_config(&config);
    config.mode = PSLOG_MODE_JSON;
    config.color = PSLOG_COLOR_NEVER;
    config.timestamps = 0;
    config.output = pslog_output_from_fp(stdout, 0);
    log = pslog_new(&config);
    if (log == NULL) {
      munmap(mapping, (size_t)page_size * 2u);
      _exit(123);
    }

    kvfmt = (char *)mapping + (size_t)page_size - 5u;
    memcpy(kvfmt, "abcd=%d", 8u);
    log->infof(log, "first", kvfmt, 7);

    if (mprotect((char *)mapping + (size_t)page_size, (size_t)page_size,
                 PROT_NONE) != 0) {
      log->destroy(log);
      munmap(mapping, (size_t)page_size * 2u);
      _exit(124);
    }

    memcpy(kvfmt, "x=%d", 5u);
    if (use_withf) {
      pslog_logger *child;

      child = log->withf(log, kvfmt, 9);
      if (child == NULL) {
        log->destroy(log);
        munmap(mapping, (size_t)page_size * 2u);
        _exit(125);
      }
      child->info(child, "second", NULL, 0u);
      child->destroy(child);
    } else {
      log->infof(log, "second", kvfmt, 9);
    }

    log->destroy(log);
    munmap(mapping, (size_t)page_size * 2u);
    _exit(0);
  }

  close(pipefd[1]);
  total = 0u;
  while (total + 1u < output_size) {
    nread = read(pipefd[0], output + total, output_size - total - 1u);
    if (nread <= 0) {
      break;
    }
    total += (size_t)nread;
  }
  output[total] = '\0';
  close(pipefd[0]);
  if (waitpid(pid, &status, 0) < 0) {
    return -1;
  }
  if (WIFEXITED(status)) {
    *exit_code = WEXITSTATUS(status);
    *term_signal = 0;
    return 0;
  }
  if (WIFSIGNALED(status)) {
    *exit_code = -1;
    *term_signal = WTERMSIG(status);
    return 0;
  }
  return -1;
#else
  (void)use_withf;
  (void)output;
  (void)output_size;
  (void)exit_code;
  (void)term_signal;
  return 0;
#endif
}

static const char *find_json_string_field(const char *json, const char *key,
                                          char *value, size_t value_size) {
  char pattern[64];
  const char *start;
  const char *end;
  size_t len;

  if (json == NULL || key == NULL || value == NULL || value_size == 0u) {
    return NULL;
  }
  if (strlen(key) + 5u >= sizeof(pattern)) {
    value[0] = '\0';
    return NULL;
  }
  strcpy(pattern, "\"");
  strcat(pattern, key);
  strcat(pattern, "\":\"");
  start = strstr(json, pattern);
  if (start == NULL) {
    value[0] = '\0';
    return NULL;
  }
  start += strlen(pattern);
  end = strchr(start, '"');
  if (end == NULL) {
    value[0] = '\0';
    return NULL;
  }
  len = (size_t)(end - start);
  if (len >= value_size) {
    len = value_size - 1u;
  }
  memcpy(value, start, len);
  value[len] = '\0';
  return start;
}

static void set_env_value(const char *key, const char *value);
static void unset_env_value(const char *key);

static size_t collect_lines(char *text, char **lines, size_t max_lines) {
  char *cursor;
  size_t count;

  count = 0u;
  cursor = text;
  while (*cursor != '\0' && count < max_lines) {
    lines[count++] = cursor;
    while (*cursor != '\0' && *cursor != '\n') {
      ++cursor;
    }
    if (*cursor == '\n') {
      *cursor = '\0';
      ++cursor;
    }
  }
  return count;
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

static pslog_logger *new_logger(struct memory_sink *sink, pslog_mode mode,
                                pslog_color_mode color) {
  pslog_config config;

  pslog_default_config(&config);
  config.mode = mode;
  config.color = color;
  config.timestamps = 0;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = sink;
  return pslog_new(&config);
}

static int run_variant_sequence(struct memory_sink *sink, pslog_mode mode,
                                pslog_color_mode color) {
  pslog_config config;
  pslog_logger *root;
  pslog_logger *base;
  pslog_logger *annotated;
  pslog_logger *env_root;
  pslog_logger *env_base;
  pslog_logger *env_annotated;
  pslog_logger *error_only;
  pslog_field field;
  pslog_field large_field;
  char large_value[1025];

  memset(large_value, 'x', sizeof(large_value) - 1u);
  large_value[sizeof(large_value) - 1u] = '\0';

  pslog_default_config(&config);
  config.mode = mode;
  config.color = color;
  config.min_level = PSLOG_LEVEL_TRACE;
  config.timestamps = 0;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = sink;

  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);
  field = pslog_str("app", "demo");
  base = root->with(root, &field, 1u);
  TEST_ASSERT(base != NULL);
  annotated = base->with_level_field(base);
  TEST_ASSERT(annotated != NULL);
  field = pslog_trusted_str("value_trusted", "trusted");
  annotated->trace(annotated, "trace", &field, 1u);
  field = pslog_bytes_field("bytes", "b", 1u);
  annotated->debug(annotated, "debug", &field, 1u);

  set_env_value("LOG_LEVEL", "info");
  env_root = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(env_root != NULL);
  field = pslog_str("app", "demo");
  env_base = env_root->with(env_root, &field, 1u);
  TEST_ASSERT(env_base != NULL);
  env_annotated = env_base->with_level_field(env_base);
  TEST_ASSERT(env_annotated != NULL);
  field = pslog_bool("ok", 1);
  env_annotated->info(env_annotated, "info", &field, 1u);
  large_field = pslog_str("large", large_value);
  env_annotated->warn(env_annotated, "warn", &large_field, 1u);

  error_only = env_annotated->with_level(env_annotated, PSLOG_LEVEL_ERROR);
  TEST_ASSERT(error_only != NULL);
  error_only->info(error_only, "info_suppressed", NULL, 0u);
  field = pslog_str("err", "boom");
  error_only->error(error_only, "error", &field, 1u);

  error_only->destroy(error_only);
  env_annotated->destroy(env_annotated);
  env_base->destroy(env_base);
  env_root->destroy(env_root);
  annotated->destroy(annotated);
  base->destroy(base);
  root->destroy(root);
  unset_env_value("LOG_LEVEL");
  return 0;
}

static int assert_variant_lines(const char *output, pslog_mode mode) {
  char copy[32768];
  char *lines[16];
  size_t count;
  size_t i;
  int saw_loglevel;

  strncpy(copy, output, sizeof(copy) - 1u);
  copy[sizeof(copy) - 1u] = '\0';
  count = collect_lines(copy, lines, sizeof(lines) / sizeof(lines[0]));
  TEST_ASSERT(count == 5u);
  saw_loglevel = 0;
  for (i = 0u; i < count; ++i) {
    TEST_ASSERT(contains_text(lines[i], "demo"));
    TEST_ASSERT(!contains_text(lines[i], "info_suppressed"));
    if (contains_text(lines[i], "loglevel")) {
      saw_loglevel = 1;
    }
    if (mode == PSLOG_MODE_JSON) {
      TEST_ASSERT(lines[i][0] == '{');
      TEST_ASSERT(contains_text(lines[i], "\"app\":\"demo\""));
      TEST_ASSERT(assert_valid_json_lines(lines[i]) == 0);
    } else {
      TEST_ASSERT(contains_text(lines[i], "app=demo"));
    }
  }
  TEST_ASSERT(saw_loglevel);
  return 0;
}

static int test_logger_variants_coverage(void) {
  struct memory_sink sink;
  char console_plain[32768];
  char console_color[32768];
  char json_plain[32768];
  char json_color[32768];

  reset_sink(&sink);
  TEST_ASSERT(run_variant_sequence(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER) ==
              0);
  TEST_ASSERT(assert_variant_lines(sink.data, PSLOG_MODE_JSON) == 0);
  strncpy(json_plain, sink.data, sizeof(json_plain) - 1u);
  json_plain[sizeof(json_plain) - 1u] = '\0';

  reset_sink(&sink);
  TEST_ASSERT(
      run_variant_sequence(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS) == 0);
  strip_ansi(json_color, sizeof(json_color), sink.data);
  TEST_ASSERT(assert_variant_lines(json_color, PSLOG_MODE_JSON) == 0);

  reset_sink(&sink);
  TEST_ASSERT(
      run_variant_sequence(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER) == 0);
  TEST_ASSERT(assert_variant_lines(sink.data, PSLOG_MODE_CONSOLE) == 0);
  strncpy(console_plain, sink.data, sizeof(console_plain) - 1u);
  console_plain[sizeof(console_plain) - 1u] = '\0';

  reset_sink(&sink);
  TEST_ASSERT(
      run_variant_sequence(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS) == 0);
  strip_ansi(console_color, sizeof(console_color), sink.data);
  TEST_ASSERT(assert_variant_lines(console_color, PSLOG_MODE_CONSOLE) == 0);

  TEST_ASSERT(strcmp(json_plain, json_color) == 0);
  TEST_ASSERT(strcmp(console_plain, console_color) == 0);
  return 0;
}

static void set_env_value(const char *key, const char *value) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  setenv(key, value, 1);
#else
  (void)key;
  (void)value;
#endif
}

static void unset_env_value(const char *key) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  unsetenv(key);
#else
  (void)key;
#endif
}

static void make_temp_path(char *path, size_t path_size) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  int fd;

  strncpy(path, "/tmp/libpslog-test-XXXXXX", path_size - 1u);
  path[path_size - 1u] = '\0';
  fd = mkstemp(path);
  if (fd >= 0) {
    close(fd);
    remove(path);
  }
#else
  tmpnam(path);
#endif
}

static int test_console_fields(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[2];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_i64("numkey", 2L);
  fields[1] = pslog_str("strkey", "it is a string");
  log->info(log, "hello", fields, 2u);

  TEST_ASSERT(contains_text(sink.data, "INF hello"));
  TEST_ASSERT(contains_text(sink.data, "numkey=2"));
  TEST_ASSERT(contains_text(sink.data, "strkey=\"it is a string\""));

  log->destroy(log);
  return 0;
}

static int test_console_output_matches_format(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[2];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_str("foo", "bar");
  fields[1] = pslog_str("greeting", "hello world");
  log->info(log, "ready", fields, 2u);

  TEST_ASSERT(
      strcmp(sink.data, "INF ready foo=bar greeting=\"hello world\"\n") == 0);
  log->destroy(log);
  return 0;
}

static int test_json_fields(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[2];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_bool("ok", 1);
  fields[1] = pslog_str("strkey", "value");
  log->warn(log, "oops", fields, 2u);

  TEST_ASSERT(contains_text(sink.data, "\"lvl\":\"warn\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"oops\""));
  TEST_ASSERT(contains_text(sink.data, "\"ok\":true"));
  TEST_ASSERT(contains_text(sink.data, "\"strkey\":\"value\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_finite_float_formatting(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[4];
  char expected_tenth[64];
  char expected_negzero[64];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_f64("quarter", 1.25);
  fields[1] = pslog_f64("eighth", 0.125);
  fields[2] = pslog_f64("whole", 3.0);
  fields[3] = pslog_f64("tenth", 0.1);
  log->info(log, "floats", fields, 4u);

  sprintf(expected_tenth, "\"tenth\":%.17g", 0.1);

  TEST_ASSERT(contains_text(sink.data, "\"quarter\":1.25"));
  TEST_ASSERT(contains_text(sink.data, "\"eighth\":0.125"));
  TEST_ASSERT(contains_text(sink.data, "\"whole\":3"));
  TEST_ASSERT(contains_text(sink.data, expected_tenth));

  reset_sink(&sink);
  fields[0] = pslog_f64("negzero", -0.0);
  log->info(log, "floats", fields, 1u);
  sprintf(expected_negzero, "\"negzero\":%.17g", -0.0);
  TEST_ASSERT(contains_text(sink.data, expected_negzero));

  log->destroy(log);
  return 0;
}

static int test_integer_field_extremes(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[2];
  char expected_signed[64];
  char expected_unsigned[64];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_i64("signed", LONG_MIN);
  fields[1] = pslog_u64("unsigned", ULONG_MAX);
  log->info(log, "ints", fields, 2u);

  sprintf(expected_signed, "\"signed\":%ld", LONG_MIN);
  sprintf(expected_unsigned, "\"unsigned\":%lu", ULONG_MAX);
  TEST_ASSERT(contains_text(sink.data, expected_signed));
  TEST_ASSERT(contains_text(sink.data, expected_unsigned));

  log->destroy(log);
  return 0;
}

static int test_i64_u64_preserve_64bit_payloads(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[2];
  pslog_int64 signed_value;
  pslog_uint64 unsigned_value;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  signed_value = ((pslog_int64)1099511627776ll + (pslog_int64)7);
  unsigned_value = ((pslog_uint64)1099511627776ull + (pslog_uint64)9u);
  fields[0] = pslog_i64("signed64", signed_value);
  fields[1] = pslog_u64("unsigned64", unsigned_value);
  log->info(log, "ints64", fields, 2u);

  TEST_ASSERT(contains_text(sink.data, "\"signed64\":1099511627783"));
  TEST_ASSERT(contains_text(sink.data, "\"unsigned64\":1099511627785"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_kvfmt_cache_matches_content_not_pointer(void) {
  struct memory_sink sink;
  pslog_logger *log;
  char kvfmt[32];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  strcpy(kvfmt, "a=%d");
  log->infof(log, "first", kvfmt, 7);
  TEST_ASSERT(contains_text(sink.data, "\"a\":7"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  strcpy(kvfmt, "b=%s");
  log->infof(log, "second", kvfmt, "value");
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"second\""));
  TEST_ASSERT(contains_text(sink.data, "\"b\":\"value\""));
  TEST_ASSERT(!contains_text(sink.data, "\"a\":"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_kvfmt_pointer_cache_shorter_reused_buffer_infof(void) {
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_kvfmt_shorter_reused_buffer_child(
                  0, output, sizeof(output), &exit_code, &term_signal) == 0);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(exit_code == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"first\""));
  TEST_ASSERT(contains_text(output, "\"abcd\":7"));
  TEST_ASSERT(contains_text(output, "\"msg\":\"second\""));
  TEST_ASSERT(contains_text(output, "\"x\":9"));
  TEST_ASSERT(!contains_text(output, "\"abcd\":9"));
  TEST_ASSERT(assert_valid_json_lines(output) == 0);

  return 0;
}

static int test_kvfmt_pointer_cache_shorter_reused_buffer_withf(void) {
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_kvfmt_shorter_reused_buffer_child(
                  1, output, sizeof(output), &exit_code, &term_signal) == 0);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(exit_code == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"first\""));
  TEST_ASSERT(contains_text(output, "\"abcd\":7"));
  TEST_ASSERT(contains_text(output, "\"msg\":\"second\""));
  TEST_ASSERT(contains_text(output, "\"x\":9"));
  TEST_ASSERT(!contains_text(output, "\"abcd\":9"));
  TEST_ASSERT(assert_valid_json_lines(output) == 0);

  return 0;
}

static int test_infof_kvfmt(void) {
  struct memory_sink sink;
  pslog_logger *log;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  log->infof(log, "hello", "key=%s count=%d ok=%b", "world", 123, 1);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"hello\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"world\""));
  TEST_ASSERT(contains_text(sink.data, "\"count\":123"));
  TEST_ASSERT(contains_text(sink.data, "\"ok\":true"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  log->infof(log, "hello", NULL);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"hello\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_malformed_kvfmt_rejects_suffix_after_verb(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *child;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  log->infof(log, "latency", "latency=%dms", 123);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"invalid kvfmt\""));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\":\"latency\""));
  TEST_ASSERT(!contains_text(sink.data, "\"latency\":123"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  log->infof(log, "name", "name=%s,", "api");
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"invalid kvfmt\""));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\":\"name\""));
  TEST_ASSERT(!contains_text(sink.data, "\"name\":\"api\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  child = log->withf(log, "latency=%dms", 123);
  TEST_ASSERT(child == NULL);
  child = log->withf(log, "name=%s,", "api");
  TEST_ASSERT(child == NULL);

  log->destroy(log);
  return 0;
}

#if !defined(PSLOG_SINGLE_HEADER_TEST)
static int
test_parse_kvfmt_null_shared_initializes_local_entry_parse(const char *kvfmt,
                                                           ...) {
  pslog_field fields[PSLOG_KVFMT_MAX_FIELDS];
  size_t count;
  va_list ap;
  int rc;

  va_start(ap, kvfmt);
  rc = pslog_parse_kvfmt(NULL, fields, &count, PSLOG_MODE_JSON, kvfmt, 0, ap);
  va_end(ap);
  if (rc != 0) {
    return 1;
  }
  TEST_ASSERT(count == 2u);
  TEST_ASSERT(fields[0].key_len == 3u);
  TEST_ASSERT(memcmp(fields[0].key, "key", 3u) == 0);
  TEST_ASSERT(fields[0].as.string_value != NULL);
  TEST_ASSERT(strcmp(fields[0].as.string_value, "value") == 0);
  TEST_ASSERT(fields[1].key_len == 5u);
  TEST_ASSERT(memcmp(fields[1].key, "count", 5u) == 0);
  TEST_ASSERT(fields[1].as.signed_value == 42);
  return 0;
}

static int test_parse_kvfmt_null_shared_local_entry_is_initialized(void) {
  return test_parse_kvfmt_null_shared_initializes_local_entry_parse(
      "key=%s count=%d", "value", 42);
}
#endif

static int test_kvfmt_long_key_falls_back_without_overflow(void) {
  struct memory_sink json_sink;
  struct memory_sink console_sink;
  pslog_logger *json_log;
  pslog_logger *console_log;
  pslog_logger *child;
  char key[181];
  char kvfmt[184];
  char stripped[16384];

  memset(key, 'k', sizeof(key) - 1u);
  key[sizeof(key) - 1u] = '\0';
  memcpy(kvfmt, key, sizeof(key) - 1u);
  kvfmt[sizeof(key) - 1u] = '=';
  kvfmt[sizeof(key)] = '%';
  kvfmt[sizeof(key) + 1u] = 's';
  kvfmt[sizeof(key) + 2u] = '\0';

  reset_sink(&json_sink);
  json_log = new_logger(&json_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(json_log != NULL);
  json_log->infof(json_log, "hello", kvfmt, "value");
  child = json_log->withf(json_log, kvfmt, "value");
  TEST_ASSERT(child != NULL);
  child->info(child, "child", NULL, 0u);
  child->destroy(child);
  strip_ansi(stripped, sizeof(stripped), json_sink.data);
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"hello\""));
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"child\""));
  TEST_ASSERT(contains_text(stripped, "\"value\""));
  TEST_ASSERT(contains_text(stripped, key));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  json_log->destroy(json_log);

  reset_sink(&console_sink);
  console_log =
      new_logger(&console_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(console_log != NULL);
  console_log->infof(console_log, "hello", kvfmt, "value");
  child = console_log->withf(console_log, kvfmt, "value");
  TEST_ASSERT(child != NULL);
  child->info(child, "child", NULL, 0u);
  child->destroy(child);
  strip_ansi(stripped, sizeof(stripped), console_sink.data);
  TEST_ASSERT(contains_text(stripped, "hello"));
  TEST_ASSERT(contains_text(stripped, "child"));
  TEST_ASSERT(contains_text(stripped, key));
  TEST_ASSERT(contains_text(stripped, "=value"));
  console_log->destroy(console_log);

  return 0;
}

static int test_errno_field_renders_error_text_and_color(void) {
  struct memory_sink json_sink;
  struct memory_sink console_sink;
  pslog_logger *json_log;
  pslog_logger *console_log;
  pslog_field field;
  char plain[4096];
  char expected[128];

  (void)pslog_errno_string(EACCES, expected, sizeof(expected));

  reset_sink(&json_sink);
  json_log = new_logger(&json_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(json_log != NULL);
  field = pslog_errno("error", EACCES);
  json_log->error(json_log, "open failed", &field, 1u);
  strip_ansi(plain, sizeof(plain), json_sink.data);
  TEST_ASSERT(contains_text(plain, "\"msg\":\"open failed\""));
  TEST_ASSERT(contains_text(plain, "\"error\":"));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(contains_text(json_sink.data, pslog_palette_default()->error));
  TEST_ASSERT(!contains_text(json_sink.data, pslog_palette_default()->string));
  json_log->destroy(json_log);

  reset_sink(&console_sink);
  console_log =
      new_logger(&console_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(console_log != NULL);
  field = pslog_errno("error", EACCES);
  console_log->error(console_log, "open failed", &field, 1u);
  strip_ansi(plain, sizeof(plain), console_sink.data);
  TEST_ASSERT(contains_text(plain, "open failed"));
  TEST_ASSERT(contains_text(plain, "error="));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(contains_text(console_sink.data, pslog_palette_default()->error));
  console_log->destroy(console_log);
  return 0;
}

static int test_kvfmt_errno_percent_m(void) {
  struct memory_sink json_sink;
  struct memory_sink console_sink;
  pslog_logger *json_log;
  pslog_logger *console_log;
  char plain[4096];
  char expected[128];

  (void)pslog_errno_string(ENOENT, expected, sizeof(expected));

  reset_sink(&json_sink);
  json_log = new_logger(&json_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(json_log != NULL);
  errno = ENOENT;
  json_log->errorf(json_log, "open failed", "error=%m count=%d", 7);
  strip_ansi(plain, sizeof(plain), json_sink.data);
  TEST_ASSERT(contains_text(plain, "\"error\":"));
  TEST_ASSERT(contains_text(plain, "\"count\":7"));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(contains_text(json_sink.data, pslog_palette_default()->error));
  json_log->destroy(json_log);

  reset_sink(&console_sink);
  console_log =
      new_logger(&console_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(console_log != NULL);
  errno = ENOENT;
  console_log->errorf(console_log, "open failed", "error=%m count=%d", 7);
  strip_ansi(plain, sizeof(plain), console_sink.data);
  TEST_ASSERT(contains_text(plain, "error="));
  TEST_ASSERT(contains_text(plain, "count=7"));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(count_text_occurrences(plain, expected) == 1u);
  TEST_ASSERT(contains_text(console_sink.data, pslog_palette_default()->error));
  console_log->destroy(console_log);
  return 0;
}

static int test_withf_errno_percent_m_snapshots_errno(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;
  char expected[128];

  (void)pslog_errno_string(ENOENT, expected, sizeof(expected));

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);

  errno = ENOENT;
  child = root->withf(root, "error=%m");
  TEST_ASSERT(child != NULL);
  errno = EACCES;
  child->error(child, "derived", NULL, 0u);

  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"derived\""));
  TEST_ASSERT(contains_text(sink.data, expected));
  TEST_ASSERT(!contains_text(sink.data, "Permission denied"));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_errno_static_field_prefix_renders_error_text_and_color(void) {
  struct memory_sink json_sink;
  struct memory_sink console_sink;
  pslog_logger *json_root;
  pslog_logger *json_child;
  pslog_logger *console_root;
  pslog_logger *console_child;
  pslog_field field;
  char plain[4096];
  char expected[128];

  (void)pslog_errno_string(EPIPE, expected, sizeof(expected));
  field = pslog_errno("error", EPIPE);

  reset_sink(&json_sink);
  json_root = new_logger(&json_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(json_root != NULL);
  json_child = json_root->with(json_root, &field, 1u);
  TEST_ASSERT(json_child != NULL);
  json_child->error(json_child, "write failed", NULL, 0u);
  strip_ansi(plain, sizeof(plain), json_sink.data);
  TEST_ASSERT(contains_text(plain, "\"msg\":\"write failed\""));
  TEST_ASSERT(contains_text(plain, "\"error\":"));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(contains_text(json_sink.data, pslog_palette_default()->error));
  json_child->destroy(json_child);
  json_root->destroy(json_root);

  reset_sink(&console_sink);
  console_root =
      new_logger(&console_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(console_root != NULL);
  console_child = console_root->with(console_root, &field, 1u);
  TEST_ASSERT(console_child != NULL);
  console_child->error(console_child, "write failed", NULL, 0u);
  strip_ansi(plain, sizeof(plain), console_sink.data);
  TEST_ASSERT(contains_text(plain, "write failed"));
  TEST_ASSERT(contains_text(plain, "error="));
  TEST_ASSERT(contains_text(plain, expected));
  TEST_ASSERT(contains_text(console_sink.data, pslog_palette_default()->error));
  console_child->destroy(console_child);
  console_root->destroy(console_root);
  return 0;
}

static int test_with_preserves_embedded_nul_string_lengths(void) {
  static const char value[] = {'a', '\0', 'b'};
  static const char expected[] = {'"', 'b', 'l', 'o',  'b', '"',
                                  ':', '"', 'a', '\0', 'b', '"'};
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;
  pslog_field field;

  memset(&field, 0, sizeof(field));
  field.key = "blob";
  field.key_len = 4u;
  field.value_len = sizeof(value);
  field.type = PSLOG_FIELD_STRING;
  field.trusted_key = 1u;
  field.trusted_value = 1u;
  field.console_simple_value = 0u;
  field.as.string_value = value;

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);
  child = root->with(root, &field, 1u);
  TEST_ASSERT(child != NULL);

  child->info(child, "derived", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"derived\""));
  TEST_ASSERT(contains_bytes(sink.data, sink.len, expected, sizeof(expected)));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_with_preserves_untrusted_string_lengths(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;
  pslog_field field;

  field = pslog_str("quoted", "a\"b");

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);
  child = root->with(root, &field, 1u);
  TEST_ASSERT(child != NULL);

  child->info(child, "derived", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"derived\""));
  TEST_ASSERT(contains_text(sink.data, "\"quoted\":\"a\\\"b\""));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_json_untrusted_embedded_nul_string_uses_value_len(void) {
  static const char value[] = {'a', '\0', 'b'};
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_logger *plain;
  pslog_logger *color;
  pslog_field field;
  char stripped[4096];

  memset(&field, 0, sizeof(field));
  field.key = "blob";
  field.key_len = 4u;
  field.value_len = sizeof(value);
  field.type = PSLOG_FIELD_STRING;
  field.trusted_key = 1u;
  field.trusted_value = 0u;
  field.console_simple_value = 0u;
  field.as.string_value = value;

  reset_sink(&plain_sink);
  plain = new_logger(&plain_sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(plain != NULL);
  plain->info(plain, "plain", &field, 1u);
  TEST_ASSERT(contains_text(plain_sink.data, "\"blob\":\"a\\u0000b\""));
  TEST_ASSERT(assert_valid_json_lines(plain_sink.data) == 0);
  plain->destroy(plain);

  reset_sink(&color_sink);
  color = new_logger(&color_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(color != NULL);
  color->info(color, "color", &field, 1u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(contains_text(stripped, "\"blob\":\"a\\u0000b\""));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  color->destroy(color);
  return 0;
}

static int test_verbose_fields_expand_builtin_keys_in_json(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.color = PSLOG_COLOR_NEVER;
  config.timestamps = 1;
  config.utc = 1;
  config.verbose_fields = 1;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "hello", NULL, 0u);

  TEST_ASSERT(contains_text(sink.data, "\"time\":\""));
  TEST_ASSERT(contains_text(sink.data, "\"level\":\"info\""));
  TEST_ASSERT(contains_text(sink.data, "\"message\":\"hello\""));
  TEST_ASSERT(!contains_text(sink.data, "\"ts\":"));
  TEST_ASSERT(!contains_text(sink.data, "\"lvl\":"));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\":"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_withf_child_logger(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);

  child = root->withf(root, "service=%s attempt=%d ok=%b", "worker", 3, 1);
  TEST_ASSERT(child != NULL);

  child->info(child, "derived", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"derived\""));
  TEST_ASSERT(contains_text(sink.data, "\"service\":\"worker\""));
  TEST_ASSERT(contains_text(sink.data, "\"attempt\":3"));
  TEST_ASSERT(contains_text(sink.data, "\"ok\":true"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  root->info(root, "root", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"root\""));
  TEST_ASSERT(!contains_text(sink.data, "\"service\":\"worker\""));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_nolevel_output_and_parse(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_level level;
  pslog_logger *derived;
  pslog_logger *annotated;

  TEST_ASSERT(pslog_parse_level("nolevel", &level));
  TEST_ASSERT(level == PSLOG_LEVEL_NOLEVEL);
  TEST_ASSERT(strcmp(pslog_level_string(PSLOG_LEVEL_NOLEVEL), "nolevel") == 0);

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  log->log(log, PSLOG_LEVEL_NOLEVEL, "hello", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "--- hello"));

  reset_sink(&sink);
  derived = log->with_level(log, PSLOG_LEVEL_NOLEVEL);
  TEST_ASSERT(derived != NULL);
  annotated = derived->with_level_field(derived);
  TEST_ASSERT(annotated != NULL);
  annotated->info(annotated, "derived", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "loglevel=nolevel"));
  annotated->destroy(annotated);
  derived->destroy(derived);
  log->destroy(log);

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  log->log(log, PSLOG_LEVEL_NOLEVEL, "hello", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"lvl\":\"nolevel\""));
  log->destroy(log);
  return 0;
}

static int test_with_child_logger(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;
  pslog_field base_fields[2];

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);

  base_fields[0] = pslog_i64("numkey", 2L);
  base_fields[1] = pslog_str("strkey", "base");
  child = root->with(root, base_fields, 2u);
  TEST_ASSERT(child != NULL);

  child->debug(child, "hello world", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "numkey=2"));
  TEST_ASSERT(contains_text(sink.data, "strkey=base") ||
              contains_text(sink.data, "strkey=\"base\""));

  reset_sink(&sink);
  root->debug(root, "plain", NULL, 0u);
  TEST_ASSERT(!contains_text(sink.data, "numkey=2"));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_palette_alias_and_color_json(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_config config;
  char stripped[32768];
  const pslog_palette *palette;

  palette = pslog_palette_by_name("doom-nord");
  TEST_ASSERT(palette == pslog_palette_by_name("nord"));

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.color = PSLOG_COLOR_ALWAYS;
  config.timestamps = 0;
  config.palette = pslog_palette_by_name("one-dark");
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  log->infof(log, "hello", "key=%s", "value");
  TEST_ASSERT(contains_text(sink.data, "\x1b["));
  strip_ansi(stripped, sizeof(stripped), sink.data);
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"hello\""));
  TEST_ASSERT(contains_text(stripped, "\"key\":\"value\""));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);

  log->destroy(log);
  return 0;
}

static int test_palette_exports_match_lookup(void) {
  TEST_ASSERT(pslog_palette_default() == &pslog_builtin_palette_default);
  TEST_ASSERT(pslog_palette_by_name("nord") == &pslog_builtin_palette_nord);
  TEST_ASSERT(pslog_palette_by_name("tokyo-night") ==
              &pslog_builtin_palette_tokyo_night);
  TEST_ASSERT(pslog_palette_by_name("rose-pine-dawn") ==
              &pslog_builtin_palette_rose_pine_dawn);
  TEST_ASSERT(pslog_palette_by_name("papercolor-dark") ==
              &pslog_builtin_palette_papercolor_dark);
  return 0;
}

static int test_palette_enumeration_api(void) {
  size_t count;
  size_t i;
  const char *name;

  count = pslog_palette_count();
  TEST_ASSERT(count > 0u);
  for (i = 0u; i < count; ++i) {
    name = pslog_palette_name(i);
    TEST_ASSERT(name != NULL);
    TEST_ASSERT(pslog_palette_by_name(name) != NULL);
  }
  TEST_ASSERT(pslog_palette_name(count) == NULL);
  return 0;
}

static int test_default_palette_matches_go_pslog(void) {
  const pslog_palette *palette;

  palette = pslog_palette_default();
  TEST_ASSERT(palette != NULL);
  TEST_ASSERT(strcmp(palette->key, "\x1b[36m") == 0);
  TEST_ASSERT(strcmp(palette->string, "\x1b[1;34m") == 0);
  TEST_ASSERT(strcmp(palette->number, "\x1b[35m") == 0);
  TEST_ASSERT(strcmp(palette->boolean, "\x1b[33m") == 0);
  TEST_ASSERT(strcmp(palette->null_value, "\x1b[90m") == 0);
  TEST_ASSERT(strcmp(palette->trace, "\x1b[34m") == 0);
  TEST_ASSERT(strcmp(palette->debug, "\x1b[32m") == 0);
  TEST_ASSERT(strcmp(palette->info, "\x1b[1;32m") == 0);
  TEST_ASSERT(strcmp(palette->warn, "\x1b[1;33m") == 0);
  TEST_ASSERT(strcmp(palette->error, "\x1b[1;31m") == 0);
  TEST_ASSERT(strcmp(palette->fatal, "\x1b[1;31m") == 0);
  TEST_ASSERT(strcmp(palette->panic, "\x1b[1;31m") == 0);
  TEST_ASSERT(strcmp(palette->timestamp, "\x1b[90m") == 0);
  TEST_ASSERT(strcmp(palette->message, "\x1b[1m") == 0);
  TEST_ASSERT(strcmp(palette->message_key, "\x1b[36m") == 0);
  TEST_ASSERT(strcmp(palette->reset, "\x1b[0m") == 0);
  return 0;
}

static int test_wrappers_noop_on_null(void) {
  pslog_fields(NULL, PSLOG_LEVEL_INFO, "noop", NULL, 0u);
  pslog_trace(NULL, "noop", NULL, 0u);
  pslog_debug(NULL, "noop", NULL, 0u);
  pslog_info(NULL, "noop", NULL, 0u);
  pslog_warn(NULL, "noop", NULL, 0u);
  pslog_error(NULL, "noop", NULL, 0u);
  pslog_fatal(NULL, "noop", NULL, 0u);
  pslog_panic(NULL, "noop", NULL, 0u);
  pslog_tracef(NULL, "noop", "key=%s", "value");
  pslog_debugf(NULL, "noop", "key=%s", "value");
  pslog_infof(NULL, "noop", "key=%s", "value");
  pslog_warnf(NULL, "noop", "key=%s", "value");
  pslog_errorf(NULL, "noop", "key=%s", "value");
  pslog_fatalf(NULL, "noop", "key=%s", "value");
  pslog_panicf(NULL, "noop", "key=%s", "value");
  pslog(NULL, PSLOG_LEVEL_INFO, "noop", "key=%s", "value");
  TEST_ASSERT(pslog_with(NULL, NULL, 0u) == NULL);
  TEST_ASSERT(pslog_withf(NULL, "key=%s", "value") == NULL);
  TEST_ASSERT(pslog_with_level(NULL, PSLOG_LEVEL_WARN) == NULL);
  TEST_ASSERT(pslog_with_level_field(NULL) == NULL);
  return 0;
}

static int test_length_aware_strings(void) {
  const char msg_bytes[] = {'a', '\0', 'b', 'x'};
  char key_bytes[] = {'k', '\0', 'y', 'x'};
  char value_bytes[] = {'v', '\0', 'z', 'x'};
  pslog_string_view msg = {msg_bytes, 3u};
  pslog_string_view key = {key_bytes, 3u};
  pslog_string_view value = {value_bytes, 3u};
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *child;
  pslog_field fields[2];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  fields[0] = pslog_str_view(key, value);
  fields[1] = pslog_i64_view((pslog_string_view){"countxxx", 5u}, 7);
  pslog_info_view(log, msg, fields, 2u);
  pslog_infof_view(log, msg, "plain=%s", "ok");
  log->infof_view(log, msg, "member=%d", 4);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"a\\u0000b\""));
  TEST_ASSERT(contains_text(sink.data, "\"k\\u0000y\":\"v\\u0000z\""));
  TEST_ASSERT(contains_text(sink.data, "\"count\":7"));
  TEST_ASSERT(contains_text(sink.data, "\"plain\":\"ok\""));
  TEST_ASSERT(contains_text(sink.data, "\"member\":4"));
  TEST_ASSERT(memchr(sink.data, '\0', sink.len) == NULL);
  child = pslog_with(log, fields, 2u);
  TEST_ASSERT(child != NULL);
  key_bytes[0] = 'q';
  value_bytes[0] = 'q';
  reset_sink(&sink);
  pslog_info_view(child, msg, NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"k\\u0000y\":\"v\\u0000z\""));
  child->destroy(child);
  log->destroy(log);

  reset_sink(&sink);
  key_bytes[0] = 'k';
  value_bytes[0] = 'v';
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  log->info_view(log, msg, fields, 2u);
  TEST_ASSERT(contains_text(sink.data, "a\\x00b"));
  TEST_ASSERT(contains_text(sink.data, "k\\x00y="));
  TEST_ASSERT(contains_text(sink.data, "v\\x00z"));
  TEST_ASSERT(memchr(sink.data, '\0', sink.len) == NULL);
  child = pslog_with(log, fields, 2u);
  TEST_ASSERT(child != NULL);
  reset_sink(&sink);
  pslog_info_view(child, msg, NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "k\\x00y="));
  TEST_ASSERT(contains_text(sink.data, "v\\x00z"));
  child->destroy(child);
  log->destroy(log);
  return 0;
}

static int test_length_aware_field_constructors(void) {
  const pslog_string_view key = {"keyJUNK", 3u};
  const pslog_string_view value = {"valueJUNK", 5u};
  const unsigned char bytes[] = {0u, 255u};
  pslog_field fields[12];
  struct memory_sink sink;
  pslog_logger *log;
  size_t i;

  fields[0] = pslog_null_view(key);
  fields[1] = pslog_str_view(key, value);
  fields[2] = pslog_trusted_str_view(key, value);
  fields[3] = pslog_errno_view(key, ENOENT);
  fields[4] = pslog_i64_view(key, -7);
  fields[5] = pslog_u64_view(key, 7u);
  fields[6] = pslog_f64_view(key, 1.5);
  fields[7] = pslog_bool_view(key, 1);
  fields[8] = pslog_ptr_view(key, &sink);
  fields[9] = pslog_bytes_field_view(key, bytes, sizeof(bytes));
  fields[10] = pslog_time_field_view(key, 0l, 0l, 0);
  fields[11] = pslog_duration_field_view(key, 1l, 0l);
  for (i = 0u; i < 12u; ++i) {
    TEST_ASSERT(fields[i].key_len == 3u);
    TEST_ASSERT(fields[i].trusted_key == 1u);
  }
  TEST_ASSERT(fields[1].value_len == 5u);
  TEST_ASSERT(fields[2].value_len == 5u);
  TEST_ASSERT(fields[1].trusted_value == 1u);
  TEST_ASSERT(fields[2].trusted_value == 1u);
  TEST_ASSERT(pslog_trusted_str_view(key, (pslog_string_view){"\xc3\xa9x", 2u})
                  .trusted_value == 1u);
  TEST_ASSERT(pslog_trusted_str_view(key, (pslog_string_view){"\xc3\xa9x", 1u})
                  .trusted_value == 0u);

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  pslog_fields_view(log, PSLOG_LEVEL_INFO, (pslog_string_view){"msgJUNK", 3u},
                    fields, 12u);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"msg\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":null"));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"value\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":-7"));
  TEST_ASSERT(contains_text(sink.data, "\"key\":7"));
  TEST_ASSERT(contains_text(sink.data, "\"key\":true"));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"00ff\""));
  TEST_ASSERT(!contains_text(sink.data, "JUNK"));
  log->destroy(log);
  return 0;
}

static int test_length_aware_nul_at_start_and_color(void) {
  const char key_bytes[] = {'\0', 'k'};
  const char msg_bytes[] = {'\0', 'm'};
  const char value_bytes[] = {'\0', 'v'};
  const pslog_string_view key = {key_bytes, sizeof(key_bytes)};
  const pslog_string_view msg = {msg_bytes, sizeof(msg_bytes)};
  const pslog_string_view value = {value_bytes, sizeof(value_bytes)};
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;

  field = pslog_trusted_str_view(key, value);
  TEST_ASSERT(field.trusted_key == 0u);
  TEST_ASSERT(field.trusted_value == 0u);
  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(log != NULL);
  pslog_view(log, PSLOG_LEVEL_INFO, msg, "count=%d", 2);
  log->info_view(log, msg, &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\\u0000m"));
  TEST_ASSERT(contains_text(sink.data, "\\u0000k"));
  TEST_ASSERT(contains_text(sink.data, "\\u0000v"));
  TEST_ASSERT(contains_text(sink.data, "\"count\":"));
  TEST_ASSERT(memchr(sink.data, '\0', sink.len) == NULL);
  log->destroy(log);

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(log != NULL);
  pslog_info_view(log, msg, &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\\x00m"));
  TEST_ASSERT(contains_text(sink.data, "\\x00k="));
  TEST_ASSERT(contains_text(sink.data, "\\x00v"));
  TEST_ASSERT(memchr(sink.data, '\0', sink.len) == NULL);
  log->destroy(log);
  return 0;
}

static int test_public_wrappers_and_scalar_paths_json(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *child;
  pslog_logger *child_kvfmt;
  pslog_logger *warn_only;
  pslog_logger *annotated;
  pslog_field fields[4];
  pslog_field child_fields[1];
  int anchor;
  char pointer_text[64];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  anchor = 7;
  sprintf(pointer_text, "%p", (void *)&anchor);
  fields[0] = pslog_null("none");
  fields[1] = pslog_ptr("ptr", (void *)&anchor);
  fields[2] = pslog_u64("count", 42u);
  fields[3] = pslog_f64("ratio", 1.25);
  child_fields[0] = pslog_str("scope", "alpha");

  pslog_fields(log, PSLOG_LEVEL_INFO, "fields-info", fields, 4u);
  pslog_trace(log, "trace-wrapper", NULL, 0u);
  pslog_debug(log, "debug-wrapper", NULL, 0u);
  pslog_info(log, "info-wrapper", NULL, 0u);
  pslog_warn(log, "warn-wrapper", NULL, 0u);
  pslog_error(log, "error-wrapper", NULL, 0u);
  pslog(log, PSLOG_LEVEL_INFO, "kv-all",
        "s=%s d=%d ld=%ld u=%u lu=%lu f=%f p=%p b=%b", "value", -2, -3l, 4u,
        5ul, 1.25, (void *)&anchor, 1);
  pslog_tracef(log, "tracef-wrapper", "u=%u", 9u);
  pslog_debugf(log, "debugf-wrapper", "d=%d", -9);
  pslog_infof(log, "infof-wrapper", "s=%s", "text");
  pslog_warnf(log, "warnf-wrapper", "b=%b", 1);
  errno = ENOENT;
  pslog_errorf(log, "errorf-wrapper", "error=%m");

  child = pslog_with(log, child_fields, 1u);
  TEST_ASSERT(child != NULL);
  child->info(child, "child-wrapper", NULL, 0u);

  child_kvfmt = pslog_withf(log, "req=%d", 7);
  TEST_ASSERT(child_kvfmt != NULL);
  child_kvfmt->info(child_kvfmt, "child-kvfmt-wrapper", NULL, 0u);

  warn_only = pslog_with_level(log, PSLOG_LEVEL_WARN);
  TEST_ASSERT(warn_only != NULL);
  pslog_info(warn_only, "skip-wrapper", NULL, 0u);
  pslog_warn(warn_only, "warn-only-wrapper", NULL, 0u);

  annotated = pslog_with_level_field(warn_only);
  TEST_ASSERT(annotated != NULL);
  pslog_error(annotated, "annotated-wrapper", NULL, 0u);

  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"fields-info\""));
  TEST_ASSERT(contains_text(sink.data, "\"none\":null"));
  TEST_ASSERT(contains_text(sink.data, "\"ptr\":\""));
  TEST_ASSERT(contains_text(sink.data, pointer_text));
  TEST_ASSERT(contains_text(sink.data, "\"count\":42"));
  TEST_ASSERT(contains_text(sink.data, "\"ratio\":1.25"));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"debug-wrapper\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"info-wrapper\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"warn-wrapper\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"error-wrapper\""));
  TEST_ASSERT(contains_text(sink.data, "\"s\":\"value\""));
  TEST_ASSERT(contains_text(sink.data, "\"d\":-2"));
  TEST_ASSERT(contains_text(sink.data, "\"ld\":-3"));
  TEST_ASSERT(contains_text(sink.data, "\"u\":4"));
  TEST_ASSERT(contains_text(sink.data, "\"lu\":5"));
  TEST_ASSERT(contains_text(sink.data, "\"f\":1.25"));
  TEST_ASSERT(contains_text(sink.data, "\"b\":true"));
  TEST_ASSERT(contains_text(sink.data, "\"scope\":\"alpha\""));
  TEST_ASSERT(contains_text(sink.data, "\"req\":7"));
  TEST_ASSERT(contains_text(sink.data, "\"loglevel\":\"warn\""));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\":\"skip-wrapper\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  annotated->destroy(annotated);
  warn_only->destroy(warn_only);
  child_kvfmt->destroy(child_kvfmt);
  child->destroy(child);
  log->destroy(log);
  return 0;
}

static int test_public_wrappers_and_scalar_paths_console(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field fields[5];
  int anchor;
  char pointer_text[64];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  anchor = 11;
  sprintf(pointer_text, "%p", (void *)&anchor);
  fields[0] = pslog_null("none");
  fields[1] = pslog_ptr("ptr", (void *)&anchor);
  fields[2] = pslog_u64("count", 42u);
  fields[3] = pslog_f64("ratio", 1.25);
  fields[4] = pslog_bool("ok", 1);

  pslog_fields(log, PSLOG_LEVEL_INFO, "console-fields", fields, 5u);
  pslog(log, PSLOG_LEVEL_INFO, "console-kv",
        "s=%s d=%d ld=%ld u=%u lu=%lu f=%f p=%p b=%b", "value", -2, -3l, 4u,
        5ul, 1.25, (void *)&anchor, 1);
  errno = ENOENT;
  pslog_errorf(log, "console-errno", "error=%m");

  TEST_ASSERT(contains_text(sink.data, "INF console-fields"));
  TEST_ASSERT(contains_text(sink.data, "none=null"));
  TEST_ASSERT(contains_text(sink.data, "ptr="));
  TEST_ASSERT(contains_text(sink.data, pointer_text));
  TEST_ASSERT(contains_text(sink.data, "count=42"));
  TEST_ASSERT(contains_text(sink.data, "ratio=1.25"));
  TEST_ASSERT(contains_text(sink.data, "ok=true"));
  TEST_ASSERT(contains_text(sink.data, "INF console-kv"));
  TEST_ASSERT(contains_text(sink.data, "s=value"));
  TEST_ASSERT(contains_text(sink.data, "d=-2"));
  TEST_ASSERT(contains_text(sink.data, "ld=-3"));
  TEST_ASSERT(contains_text(sink.data, "u=4"));
  TEST_ASSERT(contains_text(sink.data, "lu=5"));
  TEST_ASSERT(contains_text(sink.data, "f=1.25"));
  TEST_ASSERT(contains_text(sink.data, "b=true"));
  TEST_ASSERT(contains_text(sink.data, "ERR console-errno"));
  TEST_ASSERT(contains_text(sink.data, "error="));

  log->destroy(log);
  return 0;
}

static int test_output_init_file_api(void) {
  pslog_output output;
  char path[L_tmpnam + 16];
  FILE *fp;
  char file_data[256];
  size_t written;
  size_t nread;

  memset(&output, 0, sizeof(output));
  make_temp_path(path, sizeof(path));

  TEST_ASSERT(pslog_output_init_file(&output, path, "ab") == 0);
  TEST_ASSERT(output.write != NULL);
  TEST_ASSERT(output.close != NULL);
  TEST_ASSERT(output.isatty != NULL);
  TEST_ASSERT(output.userdata != NULL);
  TEST_ASSERT(output.owned == 1);
  TEST_ASSERT(output.isatty(output.userdata) == 0);
  TEST_ASSERT(output.write(output.userdata, "hello", 5u, &written) == 0);
  TEST_ASSERT(written == 5u);

  pslog_output_destroy(&output);
  TEST_ASSERT(output.write == NULL);
  TEST_ASSERT(output.close == NULL);
  TEST_ASSERT(output.isatty == NULL);
  TEST_ASSERT(output.userdata == NULL);
  TEST_ASSERT(output.owned == 0);

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(strcmp(file_data, "hello") == 0);
  return 0;
}

static int test_output_from_fp_owned_api(void) {
  pslog_output output;
  FILE *fp;
  FILE *verify_fp;
  char path[L_tmpnam + 16];
  char file_data[256];
  size_t written;
  size_t nread;

  make_temp_path(path, sizeof(path));
  fp = fopen(path, "wb");
  TEST_ASSERT(fp != NULL);

  output = pslog_output_from_fp(fp, 1);
  TEST_ASSERT(output.write != NULL);
  TEST_ASSERT(output.close != NULL);
  TEST_ASSERT(output.isatty != NULL);
  TEST_ASSERT(output.userdata == fp);
  TEST_ASSERT(output.owned == 1);
  TEST_ASSERT(output.isatty(output.userdata) == 0);
  TEST_ASSERT(output.write(output.userdata, "hello-fp", 8u, &written) == 0);
  TEST_ASSERT(written == 8u);

  pslog_output_destroy(&output);

  verify_fp = fopen(path, "rb");
  TEST_ASSERT(verify_fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, verify_fp);
  file_data[nread] = '\0';
  fclose(verify_fp);
  remove(path);

  TEST_ASSERT(strcmp(file_data, "hello-fp") == 0);
  return 0;
}

static int test_output_destroy_calls_close_with_null_userdata(void) {
  pslog_output output;

  reset_null_userdata_sink();
  memset(&output, 0, sizeof(output));
  output.write = null_userdata_write;
  output.close = null_userdata_close;
  output.isatty = null_userdata_isatty_true;
  output.userdata = NULL;
  output.owned = 1;

  pslog_output_destroy(&output);

  TEST_ASSERT(g_null_userdata_sink.closes == 1);
  TEST_ASSERT(output.write == NULL);
  TEST_ASSERT(output.close == NULL);
  TEST_ASSERT(output.isatty == NULL);
  TEST_ASSERT(output.userdata == NULL);
  TEST_ASSERT(output.owned == 0);
  return 0;
}

static int test_errno_string_unknown_code_falls_back(void) {
  char buffer[128];

  TEST_ASSERT(pslog_errno_string(123456789, buffer, sizeof(buffer)) == buffer);
  TEST_ASSERT(buffer[0] != '\0');
  return 0;
}

static int test_observed_output_null_target_passthrough(void) {
  pslog_observed_output observed;
  pslog_observed_output_stats stats;
  size_t written;

  memset(&observed, 0, sizeof(observed));
  pslog_observed_output_init(&observed, NULL, NULL, NULL);
  TEST_ASSERT(observed.output.write != NULL);
  TEST_ASSERT(observed.output.close != NULL);
  TEST_ASSERT(observed.output.isatty == NULL);
  TEST_ASSERT(observed.output.userdata == &observed);
  TEST_ASSERT(observed.output.write(observed.output.userdata, "ignored", 7u,
                                    &written) == 0);
  TEST_ASSERT(written == 7u);
  TEST_ASSERT(observed.output.close(observed.output.userdata) == 0);
  stats = pslog_observed_output_stats_get(&observed);
  TEST_ASSERT(stats.failures == 0u);
  TEST_ASSERT(stats.short_writes == 0u);
  return 0;
}

static int test_buffer_reserve_growth_preserves_data(void) {
  pslog_buffer buffer;
  pslog_test_allocator_stats before;
  pslog_test_allocator_stats after;

  pslog_buffer_init(&buffer, NULL, 8u);
  pslog_buffer_append_cstr(&buffer, "abc");
  TEST_ASSERT(buffer.heap_owned == 0);
  TEST_ASSERT(pslog_buffer_reserve(NULL, 16u) == -1);

  pslog_test_allocator_reset();
  pslog_test_allocator_get(&before);
  TEST_ASSERT(pslog_buffer_reserve(&buffer, 2048u) == 0);
  pslog_test_allocator_get(&after);
  TEST_ASSERT(after.malloc_calls == before.malloc_calls + 1u);
  TEST_ASSERT(after.realloc_calls == before.realloc_calls);
  TEST_ASSERT(buffer.heap_owned == 1);
  TEST_ASSERT(buffer.capacity >= 2048u);
  TEST_ASSERT(strcmp(buffer.data, "abc") == 0);

  pslog_test_allocator_reset();
  pslog_test_allocator_get(&before);
  TEST_ASSERT(pslog_buffer_reserve(&buffer, 4096u) == 0);
  pslog_test_allocator_get(&after);
  TEST_ASSERT(after.realloc_calls == before.realloc_calls + 1u);
  TEST_ASSERT(strcmp(buffer.data, "abc") == 0);

  pslog_buffer_destroy(&buffer);
  return 0;
}

static int test_buffer_rejects_capacity_overflow(void) {
  pslog_buffer buffer;

  pslog_buffer_init(&buffer, NULL, 8u);
  pslog_buffer_append_cstr(&buffer, "abc");
  TEST_ASSERT(pslog_buffer_reserve(&buffer, (size_t)-1) == -1);
  TEST_ASSERT(buffer.heap_owned == 0);
  TEST_ASSERT(buffer.len == 3u);
  TEST_ASSERT(strcmp(buffer.data, "abc") == 0);
  pslog_buffer_destroy(&buffer);
  return 0;
}

static int test_buffer_exact_inline_capacity_uses_heap_storage(void) {
  pslog_buffer buffer;
  size_t inline_payload_capacity;

  memset(&buffer, 0, sizeof(buffer));
  inline_payload_capacity = sizeof(buffer.inline_data);
  pslog_buffer_init(&buffer, NULL, inline_payload_capacity);
  TEST_ASSERT(buffer.capacity == inline_payload_capacity);
  TEST_ASSERT(buffer.heap_owned == 1);
  TEST_ASSERT(buffer.data != buffer.inline_data);
  pslog_buffer_destroy(&buffer);
  return 0;
}

static int test_buffer_heap_init_failure_keeps_inline_nul_capacity(void) {
  pslog_buffer buffer;
  char payload[PSLOG_DEFAULT_LINE_BUFFER_CAPACITY];

  memset(&buffer, 0, sizeof(buffer));
  memset(payload, 'x', sizeof(payload));
  pslog_test_allocator_reset();
  pslog_test_allocator_fail_after(1u);
  pslog_buffer_init(&buffer, NULL, sizeof(buffer.inline_data));
  pslog_test_allocator_fail_after(0u);

  TEST_ASSERT(buffer.heap_owned == 0);
  TEST_ASSERT(buffer.data == buffer.inline_data);
  TEST_ASSERT(buffer.capacity == sizeof(buffer.inline_data) - 1u);
  pslog_buffer_append_n(&buffer, payload, sizeof(payload));
  TEST_ASSERT(buffer.len == sizeof(payload));
  TEST_ASSERT(buffer.data[buffer.len] == '\0');
  pslog_buffer_destroy(&buffer);
  pslog_test_allocator_reset();
  return 0;
}

#if !defined(PSLOG_SINGLE_HEADER_TEST)
static int test_buffer_fast_char_append_drops_when_local_growth_fails(void) {
  pslog_buffer buffer;
  char payload[2048];

  memset(&buffer, 0, sizeof(buffer));
  memset(payload, 'x', sizeof(payload));
  pslog_buffer_init(&buffer, NULL, 8u);
  TEST_ASSERT(pslog_buffer_reserve(&buffer, sizeof(payload)) == 0);
  TEST_ASSERT(buffer.heap_owned == 1);
  pslog_buffer_append_n(&buffer, payload, sizeof(payload));
  TEST_ASSERT(buffer.len == buffer.capacity);
  TEST_ASSERT(buffer.data[buffer.len] == '\0');

  pslog_test_allocator_reset();
  pslog_test_allocator_fail_after(1u);
  PSLOG_BUFFER_APPEND_CHAR_FAST(&buffer, '!');
  pslog_test_allocator_fail_after(0u);

  TEST_ASSERT(buffer.len == buffer.capacity);
  TEST_ASSERT(buffer.data[buffer.len] == '\0');
  TEST_ASSERT(buffer.data[buffer.len - 1u] == 'x');

  pslog_buffer_destroy(&buffer);
  pslog_test_allocator_reset();
  return 0;
}
#endif

static int test_level_parse_and_string_api(void) {
  pslog_level level;

  TEST_ASSERT(strcmp(pslog_level_string(PSLOG_LEVEL_DISABLED), "disabled") ==
              0);
  TEST_ASSERT(strcmp(pslog_level_string((pslog_level)999), "info") == 0);

  TEST_ASSERT(pslog_parse_level("trace", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_TRACE);
  TEST_ASSERT(pslog_parse_level("debug", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_DEBUG);
  TEST_ASSERT(pslog_parse_level("error", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_ERROR);
  TEST_ASSERT(pslog_parse_level("fatal", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_FATAL);
  TEST_ASSERT(pslog_parse_level("panic", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_PANIC);
  TEST_ASSERT(pslog_parse_level("disabled", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_DISABLED);
  TEST_ASSERT(pslog_parse_level("off", &level) == 1);
  TEST_ASSERT(level == PSLOG_LEVEL_DISABLED);
  TEST_ASSERT(pslog_parse_level("unknown", &level) == 0);
  TEST_ASSERT(pslog_parse_level(NULL, &level) == 0);
  TEST_ASSERT(pslog_parse_level("info", NULL) == 0);
  return 0;
}

static int test_json_color_additional_paths(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *warn_only;
  pslog_logger *annotated;
  pslog_field fields[4];
  int anchor;
  char huge_key[256];
  char stripped[32768];

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(log != NULL);

  anchor = 13;
  memset(huge_key, 'k', sizeof(huge_key) - 1u);
  huge_key[sizeof(huge_key) - 1u] = '\0';

  memset(fields, 0, sizeof(fields));
  fields[0] = pslog_null("none");
  fields[1] = pslog_u64("count", 42u);
  fields[2] = pslog_ptr("ptr", (void *)&anchor);
  fields[3].key = "bad\"key";
  fields[3].key_len = strlen(fields[3].key);
  fields[3].value_len = 0u;
  fields[3].type = PSLOG_FIELD_NULL;
  fields[3].trusted_key = 0u;
  fields[3].trusted_value = 0u;
  fields[3].console_simple_value = 0u;
  fields[3].as.pointer_value = NULL;

  pslog_fields(log, PSLOG_LEVEL_PANIC, "panic-generic", fields, 4u);
  pslog(log, PSLOG_LEVEL_FATAL, "fatal-generic", "u=%u lu=%lu f=%f p=%p", 7u,
        9ul, 1.5, (void *)&anchor);
  pslog_fields(log, PSLOG_LEVEL_NOLEVEL, "nolevel-generic", NULL, 0u);
  pslog_fields(log, PSLOG_LEVEL_DISABLED, "disabled-fields", NULL, 0u);
  pslog(log, PSLOG_LEVEL_DISABLED, "disabled-kvfmt", "u=%u", 99u);

  warn_only = pslog_with_level(log, PSLOG_LEVEL_WARN);
  TEST_ASSERT(warn_only != NULL);
  annotated = pslog_with_level_field(warn_only);
  TEST_ASSERT(annotated != NULL);
  pslog(annotated, PSLOG_LEVEL_ERROR, "annotated-kv", "u=%u lu=%lu f=%f p=%p",
        11u, 12ul, 2.5, (void *)&anchor);

  strip_ansi(stripped, sizeof(stripped), sink.data);
  TEST_ASSERT(contains_text(stripped, "\"lvl\":\"panic\""));
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"panic-generic\""));
  TEST_ASSERT(contains_text(stripped, "\"none\":null"));
  TEST_ASSERT(contains_text(stripped, "\"count\":42"));
  TEST_ASSERT(contains_text(stripped, "\"ptr\":\""));
  TEST_ASSERT(contains_text(stripped, "\"bad\\\"key\":null"));
  TEST_ASSERT(contains_text(stripped, "\"lvl\":\"fatal\""));
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"fatal-generic\""));
  TEST_ASSERT(contains_text(stripped, "\"u\":7"));
  TEST_ASSERT(contains_text(stripped, "\"lu\":9"));
  TEST_ASSERT(contains_text(stripped, "\"f\":1.5"));
  TEST_ASSERT(contains_text(stripped, "\"lvl\":\"nolevel\""));
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"nolevel-generic\""));
  TEST_ASSERT(contains_text(stripped, "\"msg\":\"annotated-kv\""));
  TEST_ASSERT(contains_text(stripped, "\"loglevel\":\"warn\""));
  TEST_ASSERT(!contains_text(stripped, "\"msg\":\"disabled-fields\""));
  TEST_ASSERT(!contains_text(stripped, "\"msg\":\"disabled-kvfmt\""));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);

  annotated->destroy(annotated);
  warn_only->destroy(warn_only);
  log->destroy(log);
  return 0;
}

static int test_json_plain_kvfmt_include_level_field(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *warn_only;
  pslog_logger *annotated;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  warn_only = pslog_with_level(log, PSLOG_LEVEL_WARN);
  TEST_ASSERT(warn_only != NULL);
  annotated = pslog_with_level_field(warn_only);
  TEST_ASSERT(annotated != NULL);

  pslog(annotated, PSLOG_LEVEL_ERROR, "annotated-plain", "u=%u", 5u);

  TEST_ASSERT(contains_text(sink.data, "\"lvl\":\"error\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"annotated-plain\""));
  TEST_ASSERT(contains_text(sink.data, "\"u\":5"));
  TEST_ASSERT(contains_text(sink.data, "\"loglevel\":\"warn\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  annotated->destroy(annotated);
  warn_only->destroy(warn_only);
  log->destroy(log);
  return 0;
}

static int test_disabled_level_is_not_emitted_by_generic_dispatch(void) {
  struct memory_sink sink;
  pslog_logger *log;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  pslog_fields(log, PSLOG_LEVEL_DISABLED, "disabled-fields", NULL, 0u);
  pslog(log, PSLOG_LEVEL_DISABLED, "disabled-kvfmt", "u=%u", 9u);

  TEST_ASSERT(sink.len == 0u);

  log->destroy(log);
  return 0;
}

static int test_console_color_additional_paths(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_logger *warn_only;
  pslog_logger *annotated;
  pslog_field fields[3];
  char stripped[32768];
  char huge_key[256];
  const char *fallback_value;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(log != NULL);

  memset(huge_key, 'x', sizeof(huge_key) - 1u);
  huge_key[sizeof(huge_key) - 1u] = '\0';

  memset(fields, 0, sizeof(fields));
  fields[0].key = huge_key;
  fields[0].key_len = strlen(huge_key);
  fields[0].value_len = strlen("fallback-once");
  fields[0].type = PSLOG_FIELD_STRING;
  fields[0].trusted_key = 1u;
  fields[0].trusted_value = 1u;
  fields[0].console_simple_value = 1u;
  fields[0].as.string_value = "fallback-once";
  fields[1].key = "";
  fields[1].key_len = 0u;
  fields[1].type = PSLOG_FIELD_STRING;
  fields[1].trusted_key = 1u;
  fields[1].trusted_value = 1u;
  fields[1].console_simple_value = 1u;
  fields[1].value_len = 1u;
  fields[1].as.string_value = "x";
  fields[2].key = NULL;
  fields[2].key_len = 0u;
  fields[2].type = PSLOG_FIELD_STRING;
  fields[2].trusted_key = 0u;
  fields[2].trusted_value = 1u;
  fields[2].console_simple_value = 1u;
  fields[2].value_len = 1u;
  fields[2].as.string_value = "y";

  pslog_fields(log, PSLOG_LEVEL_FATAL, "fatal-console-generic", fields, 3u);
  pslog(log, PSLOG_LEVEL_PANIC, "panic-console-generic",
        "u=%u lu=%lu f=%f p=%p", 1u, 2ul, 3.5, (void *)huge_key);
  pslog_fields(log, PSLOG_LEVEL_NOLEVEL, "nolevel-console", NULL, 0u);

  warn_only = pslog_with_level(log, PSLOG_LEVEL_WARN);
  TEST_ASSERT(warn_only != NULL);
  annotated = pslog_with_level_field(warn_only);
  TEST_ASSERT(annotated != NULL);
  pslog(annotated, PSLOG_LEVEL_ERROR, "annotated-console", "u=%u", 4u);

  strip_ansi(stripped, sizeof(stripped), sink.data);
  fallback_value = "fallback-once";
  TEST_ASSERT(contains_text(stripped, "fatal-console-generic"));
  TEST_ASSERT(contains_text(stripped, "panic-console-generic"));
  TEST_ASSERT(contains_text(stripped, "nolevel-console"));
  TEST_ASSERT(contains_text(stripped, "annotated-console"));
  TEST_ASSERT(contains_text(stripped, " loglevel=warn"));
  TEST_ASSERT(contains_text(stripped, " u=1"));
  TEST_ASSERT(contains_text(stripped, " lu=2"));
  TEST_ASSERT(contains_text(stripped, " f=3.5"));
  TEST_ASSERT(count_text_occurrences(stripped, fallback_value) == 1u);
  TEST_ASSERT(!contains_text(stripped, " =x"));

  annotated->destroy(annotated);
  warn_only->destroy(warn_only);
  log->destroy(log);
  return 0;
}

static int test_internal_buffer_helper_paths(void) {
  pslog_buffer buffer;
  pslog_duration_value duration;

  pslog_buffer_init(&buffer, NULL, 0u);

  pslog_buffer_append_json_trusted_string(&buffer, "alpha");
  TEST_ASSERT(strcmp(buffer.data, "\"alpha\"") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_json_trusted_string_n(&buffer, "beta", 2u);
  TEST_ASSERT(strcmp(buffer.data, "\"be\"") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_double(&buffer, 1.25);
  TEST_ASSERT(contains_text(buffer.data, "1.25"));
  TEST_ASSERT(pslog_format_double_ascii(buffer.data, sizeof(buffer.inline_data),
                                        3.5) > 0);
  TEST_ASSERT(strcmp(buffer.data, "3.5") == 0);
  strcpy(buffer.data, "3,14159e+00");
  pslog_normalize_decimal_separator(buffer.data, strlen(buffer.data));
  TEST_ASSERT(strcmp(buffer.data, "3.14159e+00") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_double(&buffer, 0.0 / 0.0);
  TEST_ASSERT(strcmp(buffer.data, "nan") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_double(&buffer, 1.0 / 0.0);
  TEST_ASSERT(strcmp(buffer.data, "inf") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_double(&buffer, -1.0 / 0.0);
  TEST_ASSERT(strcmp(buffer.data, "-inf") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = 0l;
  duration.nanoseconds = 321l;
  pslog_buffer_append_duration(&buffer, duration, 0);
  TEST_ASSERT(strcmp(buffer.data, "321ns") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = 0l;
  duration.nanoseconds = 321678l;
  pslog_buffer_append_duration(&buffer, duration, 1);
  TEST_ASSERT(contains_text(buffer.data, "321.678"));
  TEST_ASSERT(contains_text(buffer.data, "\xC2\xB5s"));

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = 0l;
  duration.nanoseconds = 321678000l;
  pslog_buffer_append_duration(&buffer, duration, 0);
  TEST_ASSERT(strcmp(buffer.data, "321.678ms") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = -1l;
  duration.nanoseconds = -500000000l;
  pslog_buffer_append_duration(&buffer, duration, 1);
  TEST_ASSERT(strcmp(buffer.data, "\"-1.500s\"") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = -1l;
  duration.nanoseconds = 500000000l;
  pslog_buffer_append_duration(&buffer, duration, 1);
  TEST_ASSERT(strcmp(buffer.data, "\"-500.000ms\"") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  duration.seconds = 61l;
  duration.nanoseconds = 123000000l;
  pslog_buffer_append_duration(&buffer, duration, 0);
  TEST_ASSERT(strcmp(buffer.data, "1m1.123s") == 0);

  buffer.len = 0u;
  buffer.data[0] = '\0';
  pslog_buffer_append_console_string(&buffer, "\"\\\r\t");
  TEST_ASSERT(strcmp(buffer.data, "\"\\\"\\\\\\r\\t\"") == 0);

  pslog_buffer_destroy(&buffer);
  return 0;
}

static int test_with_level_controls(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *warn_only;
  pslog_logger *with_level_field;

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);

  warn_only = root->with_level(root, PSLOG_LEVEL_WARN);
  TEST_ASSERT(warn_only != NULL);
  warn_only->info(warn_only, "skip", NULL, 0u);
  TEST_ASSERT(sink.len == 0u);
  warn_only->warn(warn_only, "emit", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"lvl\":\"warn\""));

  reset_sink(&sink);
  with_level_field = warn_only->with_level_field(warn_only);
  TEST_ASSERT(with_level_field != NULL);
  with_level_field->error(with_level_field, "boom", NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "\"loglevel\":\"warn\""));

  with_level_field->destroy(with_level_field);
  warn_only->destroy(warn_only);
  root->destroy(root);
  return 0;
}

static int test_constructor_with_level_field_matches_derived_logger(void) {
  struct memory_sink ctor_sink;
  struct memory_sink derived_sink;
  pslog_config config;
  pslog_logger *ctor_log;
  pslog_logger *root;
  pslog_logger *derived;
  pslog_field field;

  reset_sink(&ctor_sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.color = PSLOG_COLOR_NEVER;
  config.timestamps = 0;
  config.min_level = PSLOG_LEVEL_WARN;
  config.with_level_field = 1;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &ctor_sink;
  ctor_log = pslog_new(&config);
  TEST_ASSERT(ctor_log != NULL);

  field = pslog_str("service", "api");
  ctor_log->error(ctor_log, "ctor", &field, 1u);

  TEST_ASSERT(contains_text(ctor_sink.data, "\"msg\":\"ctor\""));
  TEST_ASSERT(contains_text(ctor_sink.data, "\"service\":\"api\""));
  TEST_ASSERT(contains_text(ctor_sink.data, "\"lvl\":\"error\""));
  TEST_ASSERT(contains_text(ctor_sink.data, "\"loglevel\":\"warn\""));
  TEST_ASSERT(assert_valid_json_lines(ctor_sink.data) == 0);

  reset_sink(&derived_sink);
  config.with_level_field = 0;
  config.output.userdata = &derived_sink;
  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);
  derived = root->with_level_field(root);
  TEST_ASSERT(derived != NULL);

  field = pslog_str("service", "api");
  derived->error(derived, "ctor", &field, 1u);

  TEST_ASSERT(strcmp(ctor_sink.data, derived_sink.data) == 0);

  derived->destroy(derived);
  root->destroy(root);
  ctor_log->destroy(ctor_log);
  return 0;
}

static int test_new_from_env(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  char stripped[32768];

  reset_sink(&sink);
  pslog_default_config(&config);
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_MODE", "json");
  set_env_value("LOG_LEVEL", "warn");
  set_env_value("LOG_VERBOSE_FIELDS", "true");
  set_env_value("LOG_DISABLE_TIMESTAMP", "true");
  set_env_value("LOG_FORCE_COLOR", "true");
  set_env_value("LOG_PALETTE", "one-dark");

  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "skip", NULL, 0u);
  TEST_ASSERT(sink.len == 0u);

  log->warnf(log, "hello", "key=%s", "value");
  TEST_ASSERT(contains_text(sink.data, "\x1b["));
  strip_ansi(stripped, sizeof(stripped), sink.data);
  TEST_ASSERT(contains_text(stripped, "\"level\":\"warn\""));
  TEST_ASSERT(contains_text(stripped, "\"message\":\"hello\""));
  TEST_ASSERT(contains_text(stripped, "\"key\":\"value\""));
  TEST_ASSERT(!contains_text(stripped, "\"ts\""));

  log->destroy(log);

  unset_env_value("LOG_MODE");
  unset_env_value("LOG_LEVEL");
  unset_env_value("LOG_VERBOSE_FIELDS");
  unset_env_value("LOG_DISABLE_TIMESTAMP");
  unset_env_value("LOG_FORCE_COLOR");
  unset_env_value("LOG_PALETTE");
  return 0;
}

static int test_new_from_env_trimmed_overrides(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.color = PSLOG_COLOR_ALWAYS;
  config.timestamps = 1;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_MODE", " json ");
  set_env_value("LOG_LEVEL", " warn ");
  set_env_value("LOG_VERBOSE_FIELDS", " true ");
  set_env_value("LOG_DISABLE_TIMESTAMP", " true ");
  set_env_value("LOG_NO_COLOR", " true ");
  set_env_value("LOG_FORCE_COLOR", " true ");

  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "skip", NULL, 0u);
  TEST_ASSERT(sink.len == 0u);
  log->warnf(log, "hello", "key=%s", "value");
  TEST_ASSERT(!contains_text(sink.data, "\x1b["));
  TEST_ASSERT(contains_text(sink.data, "\"level\":\"warn\""));
  TEST_ASSERT(contains_text(sink.data, "\"message\":\"hello\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"value\""));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\""));
  TEST_ASSERT(!contains_text(sink.data, "\"ts\""));
  log->destroy(log);

  unset_env_value("LOG_MODE");
  unset_env_value("LOG_LEVEL");
  unset_env_value("LOG_VERBOSE_FIELDS");
  unset_env_value("LOG_DISABLE_TIMESTAMP");
  unset_env_value("LOG_NO_COLOR");
  unset_env_value("LOG_FORCE_COLOR");
  return 0;
}

static int test_new_from_env_invalid_mode_keeps_seed(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_MODE", " nope ");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "seeded", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_MODE");

  TEST_ASSERT(contains_text(sink.data, "INF seeded"));
  TEST_ASSERT(!contains_text(sink.data, "\"msg\""));
  return 0;
}

static int test_new_from_env_invalid_palette_falls_back_default(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_palette custom;

  reset_sink(&sink);
  pslog_default_config(&config);
  custom = *pslog_palette_default();
  custom.message = "[MSG]";
  custom.message_key = "[MSGKEY]";
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_ALWAYS;
  config.palette = &custom;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_PALETTE", " not-a-palette ");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "seed", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_PALETTE");

  TEST_ASSERT(!contains_text(sink.data, "[MSG]"));
  TEST_ASSERT(contains_text(sink.data, pslog_palette_default()->message));
  return 0;
}

static int test_new_from_env_time_format(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  char ts[64];

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_TIME_FORMAT", "%Y-%m-%d");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "timefmt", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_TIME_FORMAT");

  TEST_ASSERT(find_json_string_field(sink.data, "ts", ts, sizeof(ts)) != NULL);
  TEST_ASSERT(strlen(ts) == 10u);
  TEST_ASSERT(ts[4] == '-');
  TEST_ASSERT(ts[7] == '-');
  TEST_ASSERT(strchr(ts, 'T') == NULL);
  return 0;
}

static int test_new_from_env_utc(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  char ts[64];
  size_t len;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_UTC", " true ");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "utc", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_UTC");

  TEST_ASSERT(find_json_string_field(sink.data, "ts", ts, sizeof(ts)) != NULL);
  len = strlen(ts);
  TEST_ASSERT(len > 0u);
  TEST_ASSERT(ts[len - 1u] == 'Z');
  return 0;
}

static int test_json_default_timestamp_has_zone_suffix(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  char ts[64];
  size_t len;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "local", NULL, 0u);
  log->destroy(log);

  TEST_ASSERT(find_json_string_field(sink.data, "ts", ts, sizeof(ts)) != NULL);
  len = strlen(ts);
  TEST_ASSERT(len > 0u);
  if (ts[len - 1u] == 'Z') {
    return 0;
  }
  TEST_ASSERT(len >= 6u);
  TEST_ASSERT((ts[len - 6u] == '+' || ts[len - 6u] == '-'));
  TEST_ASSERT(ts[len - 3u] == ':');
  TEST_ASSERT(ts[len - 5u] >= '0' && ts[len - 5u] <= '9');
  TEST_ASSERT(ts[len - 4u] >= '0' && ts[len - 4u] <= '9');
  TEST_ASSERT(ts[len - 2u] >= '0' && ts[len - 2u] <= '9');
  TEST_ASSERT(ts[len - 1u] >= '0' && ts[len - 1u] <= '9');
  return 0;
}

static int test_timestamp_cache_per_second(void) {
  struct memory_sink sink;
  struct fake_time_source source;
  pslog_config config;
  pslog_logger *log;
  pslog_logger_impl *impl;
  char first[64];
  char second[64];
  char third[64];

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  source.seconds = 1710000000;
  source.nanoseconds = 123456789l;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  impl = (pslog_logger_impl *)log->impl;
  TEST_ASSERT(impl != NULL && impl->shared != NULL);
  impl->shared->time_now = fake_time_now;
  impl->shared->time_now_userdata = &source;

  log->info(log, "one", NULL, 0u);
  source.nanoseconds = 987654321l;
  log->info(log, "two", NULL, 0u);
  source.seconds += 1;
  source.nanoseconds = 111111111l;
  log->info(log, "three", NULL, 0u);

  TEST_ASSERT(find_json_string_field(sink.data, "ts", first, sizeof(first)) !=
              NULL);
  {
    const char *line2;
    const char *line3;

    line2 = strchr(sink.data, '\n');
    TEST_ASSERT(line2 != NULL);
    ++line2;
    TEST_ASSERT(find_json_string_field(line2, "ts", second, sizeof(second)) !=
                NULL);
    line3 = strchr(line2, '\n');
    TEST_ASSERT(line3 != NULL);
    ++line3;
    TEST_ASSERT(find_json_string_field(line3, "ts", third, sizeof(third)) !=
                NULL);
  }
  TEST_ASSERT(strcmp(first, second) == 0);
  TEST_ASSERT(strcmp(first, third) != 0);
  TEST_ASSERT(impl->shared->timestamp_cache_misses == 2u);
  TEST_ASSERT(impl->shared->timestamp_cache_hits >= 1u);

  log->destroy(log);
  return 0;
}

static int test_timestamp_cache_disabled_for_rfc3339nano(void) {
  struct memory_sink sink;
  struct fake_time_source source;
  pslog_config config;
  pslog_logger *log;
  pslog_logger_impl *impl;
  char first[64];
  char second[64];

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_NEVER;
  config.time_format = PSLOG_TIME_FORMAT_RFC3339_NANO;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  source.seconds = 1710000000;
  source.nanoseconds = 123456789l;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  impl = (pslog_logger_impl *)log->impl;
  TEST_ASSERT(impl != NULL && impl->shared != NULL);
  impl->shared->time_now = fake_time_now;
  impl->shared->time_now_userdata = &source;

  log->info(log, "one", NULL, 0u);
  source.nanoseconds = 987654321l;
  log->info(log, "two", NULL, 0u);

  TEST_ASSERT(find_json_string_field(sink.data, "ts", first, sizeof(first)) !=
              NULL);
  {
    const char *line2;

    line2 = strchr(sink.data, '\n');
    TEST_ASSERT(line2 != NULL);
    ++line2;
    TEST_ASSERT(find_json_string_field(line2, "ts", second, sizeof(second)) !=
                NULL);
  }
  TEST_ASSERT(strchr(first, '.') != NULL);
  TEST_ASSERT(strchr(second, '.') != NULL);
  TEST_ASSERT(strcmp(first, second) != 0);
  TEST_ASSERT(impl->shared->timestamp_cache_misses == 0u);
  TEST_ASSERT(impl->shared->timestamp_cache_hits == 0u);

  log->destroy(log);
  return 0;
}

static int test_new_from_env_output_open_failure(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  char path[L_tmpnam + 16];

  reset_sink(&sink);
  make_temp_path(path, sizeof(path));
  TEST_ASSERT(mkdir(path, 0700) == 0);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_OUTPUT", path);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "after", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");
  rmdir(path);

  TEST_ASSERT(
      contains_text(sink.data, "\"msg\":\"logger.output.open.failed\""));
  TEST_ASSERT(contains_text(sink.data, "\"output\":\""));
  TEST_ASSERT(contains_text(sink.data, "\"error\":\""));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"after\""));
  return 0;
}

static int test_new_from_env_output_file_mode_invalid(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_OUTPUT_FILE_MODE", "0x1ff");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "after", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT_FILE_MODE");

  TEST_ASSERT(
      contains_text(sink.data, "\"msg\":\"logger.output.file_mode.invalid\""));
  TEST_ASSERT(contains_text(sink.data, "\"output_file_mode\":\"0x1ff\""));
  TEST_ASSERT(contains_text(sink.data, "\"error\":\"invalid OUTPUT_FILE_MODE"));
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"after\""));
  return 0;
}

static int test_console_message_escape(void) {
  struct memory_sink sink;
  pslog_logger *plain;
  pslog_logger *color;
  char stripped[32768];
  const char *msg;

  msg = "line\nwith\tesc\x1b[31mred";

  reset_sink(&sink);
  plain = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(plain != NULL);
  plain->info(plain, msg, NULL, 0u);
  TEST_ASSERT(contains_text(sink.data, "line\\nwith\\tesc\\x1b[31mred"));
  TEST_ASSERT(strchr(sink.data, '\t') == NULL);
  plain->destroy(plain);

  reset_sink(&sink);
  color = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(color != NULL);
  color->info(color, msg, NULL, 0u);
  strip_ansi(stripped, sizeof(stripped), sink.data);
  TEST_ASSERT(contains_text(stripped, "line\\nwith\\tesc\\x1b[31mred"));
  color->destroy(color);
  return 0;
}

static int test_unicode_printable_all_modes(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;
  char stripped[32768];
  const char *samples[4];
  pslog_mode modes[4];
  pslog_color_mode colors[4];
  size_t i;
  size_t j;

  samples[0] = "µ";
  samples[1] = "→";
  samples[2] = "✅";
  samples[3] = "🚀";
  modes[0] = PSLOG_MODE_CONSOLE;
  modes[1] = PSLOG_MODE_CONSOLE;
  modes[2] = PSLOG_MODE_JSON;
  modes[3] = PSLOG_MODE_JSON;
  colors[0] = PSLOG_COLOR_NEVER;
  colors[1] = PSLOG_COLOR_ALWAYS;
  colors[2] = PSLOG_COLOR_NEVER;
  colors[3] = PSLOG_COLOR_ALWAYS;

  for (i = 0u; i < 4u; ++i) {
    for (j = 0u; j < 4u; ++j) {
      reset_sink(&sink);
      log = new_logger(&sink, modes[i], colors[i]);
      TEST_ASSERT(log != NULL);
      field = pslog_str("value", samples[j]);
      log->info(log, "event", &field, 1u);

      if (colors[i] == PSLOG_COLOR_ALWAYS) {
        strip_ansi(stripped, sizeof(stripped), sink.data);
      } else {
        strncpy(stripped, sink.data, sizeof(stripped) - 1u);
        stripped[sizeof(stripped) - 1u] = '\0';
      }
      TEST_ASSERT(contains_text(stripped, samples[j]));
      TEST_ASSERT(strstr(stripped, "\\x") == NULL);
      TEST_ASSERT(strstr(stripped, "\\u00") == NULL);
      log->destroy(log);
    }
  }
  return 0;
}

static int test_time_and_duration_fields(void) {
  struct memory_sink sink;
  pslog_logger *json;
  pslog_logger *console;
  pslog_field fields[2];

  reset_sink(&sink);
  json = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(json != NULL);
  fields[0] = pslog_time_field("time", 1761993296l, 789123000l, 120);
  fields[1] = pslog_duration_field("duration", 1l, 500000000l);
  json->info(json, "event", fields, 2u);
  TEST_ASSERT(contains_text(sink.data,
                            "\"time\":\"2025-11-01T12:34:56.789123+02:00\""));
  TEST_ASSERT(contains_text(sink.data, "\"duration\":\"1.500s\""));
  json->destroy(json);

  reset_sink(&sink);
  console = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(console != NULL);
  console->info(console, "event", fields, 2u);
  TEST_ASSERT(
      contains_text(sink.data, "time=2025-11-01T12:34:56.789123+02:00"));
  TEST_ASSERT(contains_text(sink.data, "duration=1.500s"));
  console->destroy(console);

  reset_sink(&sink);
  json = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(json != NULL);
  fields[1] = pslog_duration_field("duration", -1l, 500000000l);
  json->info(json, "event", fields, 2u);
  TEST_ASSERT(contains_text(sink.data, "\"duration\":\"-500.000ms\""));
  json->destroy(json);

  reset_sink(&sink);
  console = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(console != NULL);
  console->info(console, "event", fields, 2u);
  TEST_ASSERT(contains_text(sink.data, "duration=-500.000ms"));
  console->destroy(console);
  return 0;
}

static int test_console_quoting_del_and_highbit(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_logger *plain;
  pslog_logger *color;
  pslog_field field;
  char stripped[32768];
  char del_value[8];
  char highbit_value[6];

  del_value[0] = 'h';
  del_value[1] = 'a';
  del_value[2] = 's';
  del_value[3] = (char)0x7f;
  del_value[4] = 'd';
  del_value[5] = 'e';
  del_value[6] = 'l';
  del_value[7] = '\0';
  highbit_value[0] = 'h';
  highbit_value[1] = 'i';
  highbit_value[2] = (char)0x80;
  highbit_value[3] = 'g';
  highbit_value[4] = 'h';
  highbit_value[5] = '\0';

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  plain = new_logger(&plain_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  color = new_logger(&color_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(plain != NULL);
  TEST_ASSERT(color != NULL);

  field = pslog_str("value", del_value);
  plain->info(plain, "event", &field, 1u);
  color->info(color, "event", &field, 1u);
  TEST_ASSERT(contains_text(plain_sink.data, "value=\"has\\x7fdel\""));
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(contains_text(stripped, "value=\"has\\x7fdel\""));

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  field = pslog_str("value", highbit_value);
  plain->info(plain, "event", &field, 1u);
  color->info(color, "event", &field, 1u);
  TEST_ASSERT(contains_text(plain_sink.data, "value=hi"));
  TEST_ASSERT(strstr(plain_sink.data, "\\x80") == NULL);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(contains_text(stripped, "value=hi"));
  TEST_ASSERT(strstr(stripped, "\\x80") == NULL);

  plain->destroy(plain);
  color->destroy(color);
  return 0;
}

static int test_console_escape_preserves_indent_spaces(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_logger *plain;
  pslog_logger *color;
  pslog_field field;
  char stripped[32768];
  const char *src;

  src = "Schema:\n  {\n   \"items\": {\n    \"$ref\": "
        "\"#/components/schemas/VoucherLine\"\n   },\n    \"minItems\": 2,\n   "
        " \"type\": \"array\"\n  }";

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  plain = new_logger(&plain_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  color = new_logger(&color_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(plain != NULL);
  TEST_ASSERT(color != NULL);

  field = pslog_str("value", src);
  plain->info(plain, "event", &field, 1u);
  color->info(color, "event", &field, 1u);
  TEST_ASSERT(strstr(plain_sink.data, "\\x20") == NULL);
  TEST_ASSERT(contains_text(plain_sink.data,
                            "value=\"Schema:\\n  {\\n   \\\"items\\\": {"));
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(strstr(stripped, "\\x20") == NULL);
  TEST_ASSERT(
      contains_text(stripped, "value=\"Schema:\\n  {\\n   \\\"items\\\": {"));

  plain->destroy(plain);
  color->destroy(color);
  return 0;
}

static int test_keyed_fields_preserved_across_lines(void) {
  struct memory_sink sink;
  pslog_logger *root;
  pslog_logger *child;
  pslog_field base_field;
  pslog_field fields[2];
  char copy[32768];
  char *lines[8];
  size_t count;

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);
  base_field = pslog_str("app", "demo");
  child = root->with(root, &base_field, 1u);
  TEST_ASSERT(child != NULL);
  fields[0] = pslog_str("user", "alice");
  fields[1] = pslog_str("note", "multi\nline");
  child->info(child, "hello", fields, 2u);
  child->warn(child, "bye", fields, 2u);

  strncpy(copy, sink.data, sizeof(copy) - 1u);
  copy[sizeof(copy) - 1u] = '\0';
  count = collect_lines(copy, lines, sizeof(lines) / sizeof(lines[0]));
  TEST_ASSERT(count == 2u);
  TEST_ASSERT(contains_text(lines[0], "\"app\":\"demo\""));
  TEST_ASSERT(contains_text(lines[0], "\"user\":\"alice\""));
  TEST_ASSERT(contains_text(lines[0], "\"note\":\"multi\\nline\""));
  TEST_ASSERT(contains_text(lines[1], "\"app\":\"demo\""));
  TEST_ASSERT(contains_text(lines[1], "\"user\":\"alice\""));
  TEST_ASSERT(contains_text(lines[1], "\"note\":\"multi\\nline\""));

  child->destroy(child);
  root->destroy(root);

  reset_sink(&sink);
  root = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(root != NULL);
  child = root->with(root, &base_field, 1u);
  TEST_ASSERT(child != NULL);
  child->info(child, "hello", fields, 2u);
  TEST_ASSERT(contains_text(sink.data, "app=demo"));
  TEST_ASSERT(contains_text(sink.data, "user=alice"));
  TEST_ASSERT(contains_text(sink.data, "note=\"multi\\nline\""));

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_duration_micro_sign_all_modes(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;
  char stripped[32768];
  pslog_mode modes[4];
  pslog_color_mode colors[4];
  size_t i;

  modes[0] = PSLOG_MODE_CONSOLE;
  modes[1] = PSLOG_MODE_CONSOLE;
  modes[2] = PSLOG_MODE_JSON;
  modes[3] = PSLOG_MODE_JSON;
  colors[0] = PSLOG_COLOR_NEVER;
  colors[1] = PSLOG_COLOR_ALWAYS;
  colors[2] = PSLOG_COLOR_NEVER;
  colors[3] = PSLOG_COLOR_ALWAYS;

  for (i = 0u; i < 4u; ++i) {
    reset_sink(&sink);
    log = new_logger(&sink, modes[i], colors[i]);
    TEST_ASSERT(log != NULL);
    field = pslog_duration_field("duration", 0l, 321678l);
    log->info(log, "event", &field, 1u);
    if (colors[i] == PSLOG_COLOR_ALWAYS) {
      strip_ansi(stripped, sizeof(stripped), sink.data);
    } else {
      strncpy(stripped, sink.data, sizeof(stripped) - 1u);
      stripped[sizeof(stripped) - 1u] = '\0';
    }
    if (modes[i] == PSLOG_MODE_JSON) {
      TEST_ASSERT(contains_text(stripped, "\"duration\":\"321.678µs\""));
    } else {
      TEST_ASSERT(contains_text(stripped, "duration=321.678µs"));
    }
    TEST_ASSERT(strstr(stripped, "\\x") == NULL);
    TEST_ASSERT(strstr(stripped, "\\u00b5") == NULL);
    log->destroy(log);
  }
  return 0;
}

static int test_trusted_string_helpers(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;
  char invalid_utf8[3];

  TEST_ASSERT(pslog_string_is_trusted("plain_ascii"));
  TEST_ASSERT(pslog_string_is_trusted("status ✅"));
  TEST_ASSERT(!pslog_string_is_trusted("needs\\escape"));
  TEST_ASSERT(!pslog_string_is_trusted("quote\""));
  TEST_ASSERT(!pslog_string_is_trusted("line\nbreak"));
  invalid_utf8[0] = (char)0xff;
  invalid_utf8[1] = (char)0xfe;
  invalid_utf8[2] = '\0';
  TEST_ASSERT(!pslog_string_is_trusted(invalid_utf8));

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);
  field = pslog_str("auto", "trusted_value");
  TEST_ASSERT(field.trusted_value == 1u);
  TEST_ASSERT(field.console_simple_value == 0u);
  TEST_ASSERT(field.value_len == 13u);
  log->info(log, "hello", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"auto\":\"trusted_value\""));
  field = pslog_trusted_str("trusted_key", "trusted_value");
  log->info(log, "hello", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"trusted_key\":\"trusted_value\""));
  TEST_ASSERT(!contains_text(sink.data, "\\u"));
  reset_sink(&sink);
  field = pslog_trusted_str("emoji", "status ✅");
  TEST_ASSERT(field.trusted_key == 1u);
  TEST_ASSERT(field.trusted_value == 1u);
  TEST_ASSERT(field.console_simple_value == 0u);
  log->info(log, "hello", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"emoji\":\"status ✅\""));
  field = pslog_trusted_str(invalid_utf8, invalid_utf8);
  TEST_ASSERT(field.trusted_key == 0u);
  TEST_ASSERT(field.trusted_value == 0u);
  TEST_ASSERT(field.console_simple_value == 0u);
  log->destroy(log);
  return 0;
}

static int test_json_string_escape_contract(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  field = pslog_str("value", "line\nbreak");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"value\":\"line\\nbreak\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  field = pslog_str("value", "quote\"needed");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"value\":\"quote\\\"needed\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  field = pslog_str("value", "\x1b");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"value\":\"\\u001b\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  field = pslog_str("value", "can't <tag>");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"value\":\"can't <tag>\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  reset_sink(&sink);
  field = pslog_str("needs\n", "value");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "\"needs\\n\":\"value\""));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
}

static int test_json_invalid_utf8_bytes_are_escaped(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_logger *plain_log;
  pslog_logger *color_log;
  pslog_field field;
  char message[6];
  char value[6];
  char stripped[4096];

  message[0] = 'm';
  message[1] = (char)0xff;
  message[2] = 's';
  message[3] = 'g';
  message[4] = '!';
  message[5] = '\0';
  value[0] = 'v';
  value[1] = (char)0x80;
  value[2] = 'l';
  value[3] = (char)0xfe;
  value[4] = '!';
  value[5] = '\0';

  reset_sink(&plain_sink);
  plain_log = new_logger(&plain_sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  TEST_ASSERT(plain_log != NULL);
  field = pslog_str("value", value);
  plain_log->info(plain_log, message, &field, 1u);
  TEST_ASSERT(contains_text(plain_sink.data, "\"msg\":\"m\\u00ffsg!\""));
  TEST_ASSERT(
      contains_text(plain_sink.data, "\"value\":\"v\\u0080l\\u00fe!\""));
  TEST_ASSERT(assert_valid_json_lines(plain_sink.data) == 0);
  plain_log->destroy(plain_log);

  reset_sink(&color_sink);
  color_log = new_logger(&color_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(color_log != NULL);
  field = pslog_str("value", value);
  color_log->info(color_log, message, &field, 1u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(contains_text(stripped, "\"value\":\"v\\u0080l\\u00fe!\""));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  color_log->destroy(color_log);
  return 0;
}

static int test_console_empty_string_matches_go_pslog(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_field field;

  reset_sink(&sink);
  log = new_logger(&sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(log != NULL);

  field = pslog_str("empty", "");
  log->info(log, "event", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, " empty="));
  TEST_ASSERT(!contains_text(sink.data, " empty=\"\""));

  log->destroy(log);
  return 0;
}

static int test_json_non_finite_float_policy(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_config config;
  pslog_logger *plain;
  pslog_logger *color;
  pslog_field fields[3];
  char stripped[32768];

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &plain_sink;
  plain = pslog_new(&config);
  TEST_ASSERT(plain != NULL);

  config.color = PSLOG_COLOR_ALWAYS;
  config.output.userdata = &color_sink;
  color = pslog_new(&config);
  TEST_ASSERT(color != NULL);

  fields[0] = pslog_f64("nan", strtod("nan", (char **)0));
  fields[1] = pslog_f64("pos_inf", strtod("inf", (char **)0));
  fields[2] = pslog_f64("neg_inf", strtod("-inf", (char **)0));
  plain->info(plain, "nf", fields, 3u);
  color->info(color, "nf", fields, 3u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(strcmp(plain_sink.data, stripped) == 0);
  TEST_ASSERT(contains_text(plain_sink.data, "\"nan\":\"NaN\""));
  TEST_ASSERT(contains_text(plain_sink.data, "\"pos_inf\":\"+Inf\""));
  TEST_ASSERT(contains_text(plain_sink.data, "\"neg_inf\":\"-Inf\""));
  TEST_ASSERT(assert_valid_json_lines(plain_sink.data) == 0);
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  plain->destroy(plain);
  color->destroy(color);

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.non_finite_float_policy = PSLOG_NON_FINITE_FLOAT_AS_NULL;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &plain_sink;
  plain = pslog_new(&config);
  TEST_ASSERT(plain != NULL);

  config.color = PSLOG_COLOR_ALWAYS;
  config.output.userdata = &color_sink;
  color = pslog_new(&config);
  TEST_ASSERT(color != NULL);

  plain->info(plain, "nf", fields, 3u);
  color->info(color, "nf", fields, 3u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(strcmp(plain_sink.data, stripped) == 0);
  TEST_ASSERT(contains_text(plain_sink.data, "\"nan\":null"));
  TEST_ASSERT(contains_text(plain_sink.data, "\"pos_inf\":null"));
  TEST_ASSERT(contains_text(plain_sink.data, "\"neg_inf\":null"));
  TEST_ASSERT(assert_valid_json_lines(plain_sink.data) == 0);
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  plain->destroy(plain);
  color->destroy(color);

  reset_sink(&plain_sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.non_finite_float_policy = (pslog_non_finite_float_policy)255;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &plain_sink;
  plain = pslog_new(&config);
  TEST_ASSERT(plain != NULL);
  plain->info(plain, "nf", fields, 3u);
  TEST_ASSERT(contains_text(plain_sink.data, "\"nan\":\"NaN\""));
  plain->destroy(plain);
  return 0;
}

static int test_line_buffer_capacity_override(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_field field;
  char value[10001];

  memset(value, 'x', sizeof(value) - 1u);
  value[sizeof(value) - 4u] = 'E';
  value[sizeof(value) - 3u] = 'N';
  value[sizeof(value) - 2u] = 'D';
  value[sizeof(value) - 1u] = '\0';

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  field = pslog_str("payload", value);
  log->info(log, "big", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "END"));
  TEST_ASSERT(!contains_text(sink.data, "..."));
  TEST_ASSERT(sink.writes > 1);
  log->destroy(log);

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.line_buffer_capacity = 12288u;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "big", &field, 1u);
  TEST_ASSERT(contains_text(sink.data, "END"));
  TEST_ASSERT(!contains_text(sink.data, "..."));
  TEST_ASSERT(sink.writes == 1);
  log->destroy(log);
  return 0;
}

static int test_line_buffer_capacity_overflow_is_rejected(void) {
  struct memory_sink sink;
  pslog_config config;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.line_buffer_capacity = (size_t)-1;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  TEST_ASSERT(pslog_new(&config) == NULL);
  return 0;
}

static int test_with_rejects_field_count_overflow(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *root;
  pslog_logger *base;
  pslog_field field;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);

  field = pslog_str("key", "value");
  base = root->with(root, &field, 1u);
  TEST_ASSERT(base != NULL);
  TEST_ASSERT(base->with(base, &field, (size_t)-1) == NULL);

  base->destroy(base);
  root->destroy(root);
  return 0;
}

static int test_console_prefix_cache_rejects_key_len_overflow(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_logger_impl *impl;
  pslog_field field;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_ALWAYS;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  memset(&field, 0, sizeof(field));
  field.key = "k";
  field.key_len = (size_t)-1;
  field.type = PSLOG_FIELD_STRING;
  field.as.string_value = "value";
  field.value_len = 5u;

  impl = (pslog_logger_impl *)log->impl;
  TEST_ASSERT(impl != NULL);
  TEST_ASSERT(impl->shared != NULL);
  TEST_ASSERT(pslog_test_console_prefix_cacheable(impl->shared, &field) == 0);

  log->destroy(log);
  return 0;
}

static int test_console_prefix_cache_checks_mutated_key_contents(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_field field;
  char key[4];

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  memcpy(key, "foo", sizeof(key));
  field = pslog_str(key, "one");
  log->info(log, "first", &field, 1u);

  memcpy(key, "bar", sizeof(key));
  field = pslog_str(key, "two");
  log->info(log, "second", &field, 1u);

  TEST_ASSERT(contains_text(sink.data, "foo=one"));
  TEST_ASSERT(contains_text(sink.data, "bar=two"));
  TEST_ASSERT(!contains_text(sink.data, "foo=two"));

  log->destroy(log);
  return 0;
}

static int test_zero_alloc_hot_path_emission(void) {
  static const pslog_mode modes[] = {PSLOG_MODE_CONSOLE, PSLOG_MODE_JSON};
  static const pslog_color_mode colors[] = {PSLOG_COLOR_NEVER,
                                            PSLOG_COLOR_ALWAYS};
  struct memory_sink sink;
  struct fake_time_source time_source;
  char big_payload[2048];
  size_t mode_index;
  size_t color_index;
  size_t i;

  for (i = 0u; i < sizeof(big_payload) - 1u; ++i) {
    big_payload[i] = (char)('a' + (i % 26u));
  }
  big_payload[sizeof(big_payload) - 1u] = '\0';

  for (mode_index = 0u; mode_index < sizeof(modes) / sizeof(modes[0]);
       ++mode_index) {
    for (color_index = 0u; color_index < sizeof(colors) / sizeof(colors[0]);
         ++color_index) {
      pslog_config config;
      pslog_logger *root;
      pslog_logger *child;
      pslog_logger_impl *impl;
      pslog_field fields[2];
      pslog_field big_field;
      pslog_test_allocator_stats before;
      pslog_test_allocator_stats after;

      reset_sink(&sink);
      memset(&time_source, 0, sizeof(time_source));
      time_source.seconds = 1700000000;
      time_source.nanoseconds = 123000000l;

      pslog_default_config(&config);
      config.mode = modes[mode_index];
      config.color = colors[color_index];
      config.timestamps = 1;
      config.utc = 1;
      config.output.write = memory_sink_write;
      config.output.close = NULL;
      config.output.isatty = colors[color_index] == PSLOG_COLOR_ALWAYS
                                 ? memory_sink_isatty_true
                                 : memory_sink_isatty;
      config.output.userdata = &sink;

      root = pslog_new(&config);
      TEST_ASSERT(root != NULL);
      impl = (pslog_logger_impl *)root->impl;
      TEST_ASSERT(impl != NULL);
      TEST_ASSERT(impl->shared != NULL);
      impl->shared->time_now = fake_time_now;
      impl->shared->time_now_userdata = &time_source;
      child = root->withf(root, "service=%s static=%b", "api", 1);
      TEST_ASSERT(child != NULL);

      fields[0] = pslog_i64("count", (pslog_int64)42);
      fields[1] = pslog_str("payload", "value");
      big_field = pslog_str("big", big_payload);

      root->info(root, "warm", fields, 2u);
      child->infof(child, "warmf", "count=%d payload=%s", 42, "value");
      root->info(root, "big", &big_field, 1u);

      pslog_test_allocator_reset();
      pslog_test_allocator_get(&before);
      for (i = 0u; i < 8u; ++i) {
        root->info(root, "emit", fields, 2u);
        child->infof(child, "emitf", "count=%d payload=%s", 42, "value");
        root->info(root, "big", &big_field, 1u);
      }
      pslog_test_allocator_get(&after);
      TEST_ASSERT(assert_allocator_no_growth(&before, &after) == 0);

      child->destroy(child);
      root->destroy(root);
    }
  }
  return 0;
}

static int run_bench_fixture(struct memory_sink *sink, pslog_mode mode,
                             pslog_color_mode color, int prepared,
                             unsigned long iterations) {
  pslog_logger *log;
  pslog_field fields[4];
  unsigned long i;

  reset_sink(sink);
  log = new_logger(sink, mode, color);
  TEST_ASSERT(log != NULL);

  fields[0] = pslog_i64("request_id", 0l);
  fields[1] = pslog_str("path", "/v1/messages");
  fields[2] = pslog_bool("cached", 1);
  fields[3] = pslog_f64("ms", 1.25);

  for (i = 0ul; i < iterations; ++i) {
    if (!prepared) {
      fields[0] = pslog_i64("request_id", (long)i);
      fields[1] = pslog_str("path", "/v1/messages");
      fields[2] = pslog_bool("cached", (i % 2ul) == 0ul);
      fields[3] = pslog_f64("ms", 1.25 + (double)(i % 7ul));
    } else {
      fields[0].as.signed_value = (long)i;
      fields[2].as.bool_value = (i % 2ul) == 0ul ? 1 : 0;
      fields[3].as.double_value = 1.25 + (double)(i % 7ul);
    }
    log->info(log, "bench", fields, 4u);
  }

  log->destroy(log);
  return 0;
}

static int assert_bench_line(const char *line, pslog_mode mode,
                             unsigned long index) {
  char expected[256];
  char ms_buf[64];

  sprintf(ms_buf, "%.17g", 1.25 + (double)(index % 7ul));
  if (mode == PSLOG_MODE_JSON) {
    sprintf(expected,
            "{\"lvl\":\"info\",\"msg\":\"bench\",\"request_id\":%lu,"
            "\"path\":\"/v1/messages\",\"cached\":%s,\"ms\":%s}",
            index, (index % 2ul) == 0ul ? "true" : "false", ms_buf);
  } else {
    sprintf(expected,
            "INF bench request_id=%lu path=/v1/messages cached=%s ms=%s", index,
            (index % 2ul) == 0ul ? "true" : "false", ms_buf);
  }
  TEST_ASSERT(strcmp(line, expected) == 0);
  return 0;
}

static int assert_bench_fixture_output(const char *output, pslog_mode mode,
                                       unsigned long iterations) {
  char copy[32768];
  char *lines[32];
  size_t count;
  size_t i;

  TEST_ASSERT(strlen(output) < sizeof(copy));
  strncpy(copy, output, sizeof(copy) - 1u);
  copy[sizeof(copy) - 1u] = '\0';
  count = collect_lines(copy, lines, sizeof(lines) / sizeof(lines[0]));
  TEST_ASSERT(count == (size_t)iterations);
  for (i = 0u; i < count; ++i) {
    TEST_ASSERT(assert_bench_line(lines[i], mode, (unsigned long)i) == 0);
  }
  return 0;
}

static int test_benchmark_fixture_equivalence(void) {
  struct memory_sink api_sink;
  struct memory_sink prepared_sink;
  char api_plain[32768];
  char prepared_plain[32768];
  const unsigned long iterations = 20ul;
  const pslog_mode modes[4] = {PSLOG_MODE_CONSOLE, PSLOG_MODE_CONSOLE,
                               PSLOG_MODE_JSON, PSLOG_MODE_JSON};
  const pslog_color_mode colors[4] = {PSLOG_COLOR_NEVER, PSLOG_COLOR_ALWAYS,
                                      PSLOG_COLOR_NEVER, PSLOG_COLOR_ALWAYS};
  size_t i;

  for (i = 0u; i < 4u; ++i) {
    TEST_ASSERT(
        run_bench_fixture(&api_sink, modes[i], colors[i], 0, iterations) == 0);
    TEST_ASSERT(run_bench_fixture(&prepared_sink, modes[i], colors[i], 1,
                                  iterations) == 0);
    TEST_ASSERT(api_sink.len == strlen(api_sink.data));
    TEST_ASSERT(prepared_sink.len == strlen(prepared_sink.data));
    TEST_ASSERT(api_sink.len == prepared_sink.len);
    TEST_ASSERT(api_sink.writes == prepared_sink.writes);
    TEST_ASSERT(strcmp(api_sink.data, prepared_sink.data) == 0);
    TEST_ASSERT(api_sink.writes > 1);

    if (colors[i] == PSLOG_COLOR_ALWAYS) {
      strip_ansi(api_plain, sizeof(api_plain), api_sink.data);
      strip_ansi(prepared_plain, sizeof(prepared_plain), prepared_sink.data);
    } else {
      strncpy(api_plain, api_sink.data, sizeof(api_plain) - 1u);
      api_plain[sizeof(api_plain) - 1u] = '\0';
      strncpy(prepared_plain, prepared_sink.data, sizeof(prepared_plain) - 1u);
      prepared_plain[sizeof(prepared_plain) - 1u] = '\0';
    }

    TEST_ASSERT(strcmp(api_plain, prepared_plain) == 0);
    TEST_ASSERT(assert_bench_fixture_output(api_plain, modes[i], iterations) ==
                0);
  }
  return 0;
}

static int test_repeated_double_rendering_regression(void) {
  struct memory_sink json_sink;
  struct memory_sink console_sink;
  pslog_logger *json_log;
  pslog_logger *console_log;
  pslog_field json_fields[3];
  pslog_field console_fields[3];
  char expected_json[1024];
  char expected_console[1024];
  char repeat_buf[64];
  char negative_zero_buf[64];
  char decimal_buf[64];

  reset_sink(&json_sink);
  reset_sink(&console_sink);
  json_log = new_logger(&json_sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  console_log =
      new_logger(&console_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  TEST_ASSERT(json_log != NULL);
  TEST_ASSERT(console_log != NULL);

  sprintf(repeat_buf, "%.17g", 42.125);
  sprintf(negative_zero_buf, "%.17g", -0.0);
  sprintf(decimal_buf, "%.17g", 0.1);

  json_fields[0] = pslog_f64("repeat_a", 42.125);
  json_fields[1] = pslog_f64("repeat_b", 42.125);
  json_fields[2] = pslog_f64("negative_zero", -0.0);
  json_log->info(json_log, "cache", json_fields, 3u);

  json_fields[0] = pslog_f64("repeat_a", 42.125);
  json_fields[1] = pslog_f64("repeat_b", 0.1);
  json_fields[2] = pslog_f64("negative_zero", -0.0);
  json_log->info(json_log, "cache", json_fields, 3u);

  snprintf(expected_json, sizeof(expected_json),
           "{\"lvl\":\"info\",\"msg\":\"cache\",\"repeat_a\":%s,"
           "\"repeat_b\":%s,\"negative_zero\":%s}\n"
           "{\"lvl\":\"info\",\"msg\":\"cache\",\"repeat_a\":%s,"
           "\"repeat_b\":%s,\"negative_zero\":%s}\n",
           repeat_buf, repeat_buf, negative_zero_buf, repeat_buf, decimal_buf,
           negative_zero_buf);
  TEST_ASSERT(strcmp(json_sink.data, expected_json) == 0);

  console_fields[0] = pslog_f64("repeat_a", 42.125);
  console_fields[1] = pslog_f64("repeat_b", 42.125);
  console_fields[2] = pslog_f64("negative_zero", -0.0);
  console_log->info(console_log, "cache", console_fields, 3u);

  console_fields[0] = pslog_f64("repeat_a", 42.125);
  console_fields[1] = pslog_f64("repeat_b", 0.1);
  console_fields[2] = pslog_f64("negative_zero", -0.0);
  console_log->info(console_log, "cache", console_fields, 3u);

  snprintf(expected_console, sizeof(expected_console),
           "INF cache repeat_a=%s repeat_b=%s negative_zero=%s\n"
           "INF cache repeat_a=%s repeat_b=%s negative_zero=%s\n",
           repeat_buf, repeat_buf, negative_zero_buf, repeat_buf, decimal_buf,
           negative_zero_buf);
  TEST_ASSERT(strcmp(console_sink.data, expected_console) == 0);

  json_log->destroy(json_log);
  console_log->destroy(console_log);
  return 0;
}

enum production_fixture_variant {
  PRODUCTION_FIXTURE_LOG_FIELDS_CTOR = 0,
  PRODUCTION_FIXTURE_WITH_LOG_FIELDS_CTOR = 1,
  PRODUCTION_FIXTURE_LOG_FIELDS = 2,
  PRODUCTION_FIXTURE_WITH_LOG_FIELDS = 3,
  PRODUCTION_FIXTURE_LEVEL_FIELDS_CTOR = 4,
  PRODUCTION_FIXTURE_WITH_LEVEL_FIELDS_CTOR = 5,
  PRODUCTION_FIXTURE_LEVELF_KVFMT = 6,
  PRODUCTION_FIXTURE_WITH_LEVELF_KVFMT = 7
};

static int run_production_fixture(struct memory_sink *sink, pslog_mode mode,
                                  pslog_color_mode color, int variant_kind,
                                  unsigned long iterations) {
  pslog_config config;
  pslog_logger *root;
  pslog_logger *active;
  bench_prepared_fields prepared_static;
  bench_prepared_dataset prepared_all;
  bench_prepared_dataset prepared_dynamic;
  const bench_entry_spec *entries;
  size_t entry_count;
  size_t max_fields;
  pslog_field *fields;
  unsigned long i;

  reset_sink(sink);
  memset(&prepared_static, 0, sizeof(prepared_static));
  memset(&prepared_all, 0, sizeof(prepared_all));
  memset(&prepared_dynamic, 0, sizeof(prepared_dynamic));
  pslog_default_config(&config);
  config.mode = mode;
  config.color = color;
  config.min_level = PSLOG_LEVEL_TRACE;
  config.timestamps = 0;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = sink;
  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);
  active = root;
  entries = pslog_bench_production_dataset.entries;
  entry_count = pslog_bench_production_dataset.entry_count;
  fields = NULL;

  if (variant_kind == PRODUCTION_FIXTURE_WITH_LOG_FIELDS_CTOR ||
      variant_kind == PRODUCTION_FIXTURE_WITH_LOG_FIELDS ||
      variant_kind == PRODUCTION_FIXTURE_WITH_LEVEL_FIELDS_CTOR ||
      variant_kind == PRODUCTION_FIXTURE_WITH_LEVELF_KVFMT) {
    if (pslog_bench_production_dataset.static_field_count > 0u) {
      TEST_ASSERT(bench_prepare_fields(
                      pslog_bench_production_dataset.static_fields,
                      pslog_bench_production_dataset.static_field_count,
                      &prepared_static) == 0);
      active =
          root->with(root, prepared_static.fields, prepared_static.field_count);
      TEST_ASSERT(active != NULL);
      entries = pslog_bench_production_dataset.dynamic_entries;
      entry_count = pslog_bench_production_dataset.dynamic_entry_count;
    }
  }

  if (variant_kind == PRODUCTION_FIXTURE_LOG_FIELDS) {
    TEST_ASSERT(
        bench_prepare_dataset(pslog_bench_production_dataset.entries,
                              pslog_bench_production_dataset.entry_count,
                              &prepared_all) == 0);
  } else if (variant_kind == PRODUCTION_FIXTURE_WITH_LOG_FIELDS) {
    if (pslog_bench_production_dataset.static_field_count > 0u) {
      TEST_ASSERT(bench_prepare_dataset(
                      pslog_bench_production_dataset.dynamic_entries,
                      pslog_bench_production_dataset.dynamic_entry_count,
                      &prepared_dynamic) == 0);
    } else {
      TEST_ASSERT(
          bench_prepare_dataset(pslog_bench_production_dataset.entries,
                                pslog_bench_production_dataset.entry_count,
                                &prepared_dynamic) == 0);
    }
  } else {
    max_fields = bench_dataset_max_fields(entries, entry_count);
    if (max_fields > 0u) {
      fields = (pslog_field *)calloc(max_fields, sizeof(pslog_field));
      TEST_ASSERT(fields != NULL);
    }
  }

  for (i = 0ul; i < iterations; ++i) {
    if (variant_kind == PRODUCTION_FIXTURE_LOG_FIELDS) {
      const bench_prepared_entry *entry;

      entry = &prepared_all.entries[i % prepared_all.entry_count];
      active->log(active, entry->level, entry->message, entry->fields,
                  entry->field_count);
    } else if (variant_kind == PRODUCTION_FIXTURE_WITH_LOG_FIELDS) {
      const bench_prepared_entry *entry;

      entry = &prepared_dynamic.entries[i % prepared_dynamic.entry_count];
      active->log(active, entry->level, entry->message, entry->fields,
                  entry->field_count);
    } else if (variant_kind == PRODUCTION_FIXTURE_LEVEL_FIELDS_CTOR ||
               variant_kind == PRODUCTION_FIXTURE_WITH_LEVEL_FIELDS_CTOR) {
      const bench_entry_spec *entry;

      entry = &entries[i % entry_count];
      bench_build_fields(entry->fields, entry->field_count, fields);
      bench_log_level_fields(active, entry->level, entry->message, fields,
                             entry->field_count);
    } else if (variant_kind == PRODUCTION_FIXTURE_LEVELF_KVFMT) {
      pslog_bench_production_levelf_dataset
          .calls[i % pslog_bench_production_levelf_dataset.entry_count](active);
    } else if (variant_kind == PRODUCTION_FIXTURE_WITH_LEVELF_KVFMT) {
      pslog_bench_production_dynamic_levelf_dataset
          .calls[i % pslog_bench_production_dynamic_levelf_dataset.entry_count](
              active);
    } else {
      const bench_entry_spec *entry;

      entry = &entries[i % entry_count];
      bench_build_fields(entry->fields, entry->field_count, fields);
      active->log(active, entry->level, entry->message, fields,
                  entry->field_count);
    }
  }

  free(fields);
  bench_destroy_prepared_fields(&prepared_static);
  bench_destroy_prepared_dataset(&prepared_all);
  bench_destroy_prepared_dataset(&prepared_dynamic);
  if (active != root) {
    active->destroy(active);
  }
  root->destroy(root);
  return 0;
}

static int test_production_benchmark_fixture_equivalence(void) {
  struct memory_sink log_fields_ctor_sink;
  struct memory_sink with_log_fields_ctor_sink;
  struct memory_sink log_fields_sink;
  struct memory_sink with_log_fields_sink;
  struct memory_sink level_fields_ctor_sink;
  struct memory_sink with_level_fields_ctor_sink;
  struct memory_sink levelf_kvfmt_sink;
  struct memory_sink with_levelf_kvfmt_sink;
  char log_fields_ctor_plain[131072];
  char with_log_fields_ctor_plain[131072];
  char log_fields_plain[131072];
  char with_log_fields_plain[131072];
  char level_fields_ctor_plain[131072];
  char with_level_fields_ctor_plain[131072];
  char levelf_kvfmt_plain[131072];
  char with_levelf_kvfmt_plain[131072];
  char log_fields_ctor_copy[131072];
  char with_log_fields_ctor_copy[131072];
  char log_fields_copy[131072];
  char with_log_fields_copy[131072];
  char level_fields_ctor_copy[131072];
  char with_level_fields_ctor_copy[131072];
  char levelf_kvfmt_copy[131072];
  char with_levelf_kvfmt_copy[131072];
  char *log_fields_ctor_lines[64];
  char *with_log_fields_ctor_lines[64];
  char *log_fields_lines[64];
  char *with_log_fields_lines[64];
  char *level_fields_ctor_lines[64];
  char *with_level_fields_ctor_lines[64];
  char *levelf_kvfmt_lines[64];
  char *with_levelf_kvfmt_lines[64];
  size_t log_fields_ctor_count;
  size_t with_log_fields_ctor_count;
  size_t log_fields_count;
  size_t with_log_fields_count;
  size_t level_fields_ctor_count;
  size_t with_level_fields_ctor_count;
  size_t levelf_kvfmt_count;
  size_t with_levelf_kvfmt_count;
  const unsigned long iterations = 16ul;
  const pslog_mode modes[4] = {PSLOG_MODE_CONSOLE, PSLOG_MODE_CONSOLE,
                               PSLOG_MODE_JSON, PSLOG_MODE_JSON};
  const pslog_color_mode colors[4] = {PSLOG_COLOR_NEVER, PSLOG_COLOR_ALWAYS,
                                      PSLOG_COLOR_NEVER, PSLOG_COLOR_ALWAYS};
  size_t i;

  for (i = 0u; i < 4u; ++i) {
    TEST_ASSERT(run_production_fixture(
                    &log_fields_ctor_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_LOG_FIELDS_CTOR, iterations) == 0);
    TEST_ASSERT(run_production_fixture(
                    &with_log_fields_ctor_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_WITH_LOG_FIELDS_CTOR, iterations) == 0);
    TEST_ASSERT(run_production_fixture(&log_fields_sink, modes[i], colors[i],
                                       PRODUCTION_FIXTURE_LOG_FIELDS,
                                       iterations) == 0);
    TEST_ASSERT(run_production_fixture(
                    &with_log_fields_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_WITH_LOG_FIELDS, iterations) == 0);
    TEST_ASSERT(run_production_fixture(
                    &level_fields_ctor_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_LEVEL_FIELDS_CTOR, iterations) == 0);
    TEST_ASSERT(run_production_fixture(
                    &with_level_fields_ctor_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_WITH_LEVEL_FIELDS_CTOR,
                    iterations) == 0);
    TEST_ASSERT(run_production_fixture(&levelf_kvfmt_sink, modes[i], colors[i],
                                       PRODUCTION_FIXTURE_LEVELF_KVFMT,
                                       iterations) == 0);
    TEST_ASSERT(run_production_fixture(
                    &with_levelf_kvfmt_sink, modes[i], colors[i],
                    PRODUCTION_FIXTURE_WITH_LEVELF_KVFMT, iterations) == 0);

    if (colors[i] == PSLOG_COLOR_ALWAYS) {
      strip_ansi(log_fields_ctor_plain, sizeof(log_fields_ctor_plain),
                 log_fields_ctor_sink.data);
      strip_ansi(with_log_fields_ctor_plain, sizeof(with_log_fields_ctor_plain),
                 with_log_fields_ctor_sink.data);
      strip_ansi(log_fields_plain, sizeof(log_fields_plain),
                 log_fields_sink.data);
      strip_ansi(with_log_fields_plain, sizeof(with_log_fields_plain),
                 with_log_fields_sink.data);
      strip_ansi(level_fields_ctor_plain, sizeof(level_fields_ctor_plain),
                 level_fields_ctor_sink.data);
      strip_ansi(with_level_fields_ctor_plain,
                 sizeof(with_level_fields_ctor_plain),
                 with_level_fields_ctor_sink.data);
      strip_ansi(levelf_kvfmt_plain, sizeof(levelf_kvfmt_plain),
                 levelf_kvfmt_sink.data);
      strip_ansi(with_levelf_kvfmt_plain, sizeof(with_levelf_kvfmt_plain),
                 with_levelf_kvfmt_sink.data);
    } else {
      strncpy(log_fields_ctor_plain, log_fields_ctor_sink.data,
              sizeof(log_fields_ctor_plain) - 1u);
      log_fields_ctor_plain[sizeof(log_fields_ctor_plain) - 1u] = '\0';
      strncpy(with_log_fields_ctor_plain, with_log_fields_ctor_sink.data,
              sizeof(with_log_fields_ctor_plain) - 1u);
      with_log_fields_ctor_plain[sizeof(with_log_fields_ctor_plain) - 1u] =
          '\0';
      strncpy(log_fields_plain, log_fields_sink.data,
              sizeof(log_fields_plain) - 1u);
      log_fields_plain[sizeof(log_fields_plain) - 1u] = '\0';
      strncpy(with_log_fields_plain, with_log_fields_sink.data,
              sizeof(with_log_fields_plain) - 1u);
      with_log_fields_plain[sizeof(with_log_fields_plain) - 1u] = '\0';
      strncpy(level_fields_ctor_plain, level_fields_ctor_sink.data,
              sizeof(level_fields_ctor_plain) - 1u);
      level_fields_ctor_plain[sizeof(level_fields_ctor_plain) - 1u] = '\0';
      strncpy(with_level_fields_ctor_plain, with_level_fields_ctor_sink.data,
              sizeof(with_level_fields_ctor_plain) - 1u);
      with_level_fields_ctor_plain[sizeof(with_level_fields_ctor_plain) - 1u] =
          '\0';
      strncpy(levelf_kvfmt_plain, levelf_kvfmt_sink.data,
              sizeof(levelf_kvfmt_plain) - 1u);
      levelf_kvfmt_plain[sizeof(levelf_kvfmt_plain) - 1u] = '\0';
      strncpy(with_levelf_kvfmt_plain, with_levelf_kvfmt_sink.data,
              sizeof(with_levelf_kvfmt_plain) - 1u);
      with_levelf_kvfmt_plain[sizeof(with_levelf_kvfmt_plain) - 1u] = '\0';
    }

    TEST_ASSERT(strcmp(log_fields_ctor_plain, log_fields_plain) == 0);
    TEST_ASSERT(strcmp(log_fields_ctor_plain, level_fields_ctor_plain) == 0);
    TEST_ASSERT(strcmp(log_fields_ctor_plain, levelf_kvfmt_plain) == 0);
    TEST_ASSERT(strcmp(with_log_fields_ctor_plain, with_log_fields_plain) == 0);
    TEST_ASSERT(
        strcmp(with_log_fields_ctor_plain, with_level_fields_ctor_plain) == 0);
    TEST_ASSERT(strcmp(with_log_fields_ctor_plain, with_levelf_kvfmt_plain) ==
                0);

    strncpy(log_fields_ctor_copy, log_fields_ctor_plain,
            sizeof(log_fields_ctor_copy) - 1u);
    log_fields_ctor_copy[sizeof(log_fields_ctor_copy) - 1u] = '\0';
    strncpy(with_log_fields_ctor_copy, with_log_fields_ctor_plain,
            sizeof(with_log_fields_ctor_copy) - 1u);
    with_log_fields_ctor_copy[sizeof(with_log_fields_ctor_copy) - 1u] = '\0';
    strncpy(log_fields_copy, log_fields_plain, sizeof(log_fields_copy) - 1u);
    log_fields_copy[sizeof(log_fields_copy) - 1u] = '\0';
    strncpy(with_log_fields_copy, with_log_fields_plain,
            sizeof(with_log_fields_copy) - 1u);
    with_log_fields_copy[sizeof(with_log_fields_copy) - 1u] = '\0';
    strncpy(level_fields_ctor_copy, level_fields_ctor_plain,
            sizeof(level_fields_ctor_copy) - 1u);
    level_fields_ctor_copy[sizeof(level_fields_ctor_copy) - 1u] = '\0';
    strncpy(with_level_fields_ctor_copy, with_level_fields_ctor_plain,
            sizeof(with_level_fields_ctor_copy) - 1u);
    with_level_fields_ctor_copy[sizeof(with_level_fields_ctor_copy) - 1u] =
        '\0';
    strncpy(levelf_kvfmt_copy, levelf_kvfmt_plain,
            sizeof(levelf_kvfmt_copy) - 1u);
    levelf_kvfmt_copy[sizeof(levelf_kvfmt_copy) - 1u] = '\0';
    strncpy(with_levelf_kvfmt_copy, with_levelf_kvfmt_plain,
            sizeof(with_levelf_kvfmt_copy) - 1u);
    with_levelf_kvfmt_copy[sizeof(with_levelf_kvfmt_copy) - 1u] = '\0';

    log_fields_ctor_count = collect_lines(
        log_fields_ctor_copy, log_fields_ctor_lines,
        sizeof(log_fields_ctor_lines) / sizeof(log_fields_ctor_lines[0]));
    with_log_fields_ctor_count =
        collect_lines(with_log_fields_ctor_copy, with_log_fields_ctor_lines,
                      sizeof(with_log_fields_ctor_lines) /
                          sizeof(with_log_fields_ctor_lines[0]));
    log_fields_count =
        collect_lines(log_fields_copy, log_fields_lines,
                      sizeof(log_fields_lines) / sizeof(log_fields_lines[0]));
    with_log_fields_count = collect_lines(
        with_log_fields_copy, with_log_fields_lines,
        sizeof(with_log_fields_lines) / sizeof(with_log_fields_lines[0]));
    level_fields_ctor_count = collect_lines(
        level_fields_ctor_copy, level_fields_ctor_lines,
        sizeof(level_fields_ctor_lines) / sizeof(level_fields_ctor_lines[0]));
    with_level_fields_ctor_count =
        collect_lines(with_level_fields_ctor_copy, with_level_fields_ctor_lines,
                      sizeof(with_level_fields_ctor_lines) /
                          sizeof(with_level_fields_ctor_lines[0]));
    levelf_kvfmt_count = collect_lines(levelf_kvfmt_copy, levelf_kvfmt_lines,
                                       sizeof(levelf_kvfmt_lines) /
                                           sizeof(levelf_kvfmt_lines[0]));
    with_levelf_kvfmt_count = collect_lines(
        with_levelf_kvfmt_copy, with_levelf_kvfmt_lines,
        sizeof(with_levelf_kvfmt_lines) / sizeof(with_levelf_kvfmt_lines[0]));
    TEST_ASSERT(log_fields_ctor_count == (size_t)iterations);
    TEST_ASSERT(with_log_fields_ctor_count == (size_t)iterations);
    TEST_ASSERT(log_fields_count == (size_t)iterations);
    TEST_ASSERT(with_log_fields_count == (size_t)iterations);
    TEST_ASSERT(level_fields_ctor_count == (size_t)iterations);
    TEST_ASSERT(with_level_fields_ctor_count == (size_t)iterations);
    TEST_ASSERT(levelf_kvfmt_count == (size_t)iterations);
    TEST_ASSERT(with_levelf_kvfmt_count == (size_t)iterations);

    {
      size_t line_index;

      for (line_index = 0u; line_index < with_log_fields_count; ++line_index) {
        TEST_ASSERT(strcmp(log_fields_ctor_lines[line_index],
                           log_fields_lines[line_index]) == 0);
        TEST_ASSERT(strcmp(log_fields_ctor_lines[line_index],
                           level_fields_ctor_lines[line_index]) == 0);
        TEST_ASSERT(strcmp(log_fields_ctor_lines[line_index],
                           levelf_kvfmt_lines[line_index]) == 0);
        TEST_ASSERT(strcmp(with_log_fields_ctor_lines[line_index],
                           with_log_fields_lines[line_index]) == 0);
        TEST_ASSERT(strcmp(with_log_fields_ctor_lines[line_index],
                           with_level_fields_ctor_lines[line_index]) == 0);
        TEST_ASSERT(strcmp(with_log_fields_ctor_lines[line_index],
                           with_levelf_kvfmt_lines[line_index]) == 0);
        if (modes[i] == PSLOG_MODE_JSON) {
          TEST_ASSERT(strstr(log_fields_ctor_lines[line_index],
                             "\"app\":\"lockd\"") != NULL);
          TEST_ASSERT(strstr(with_log_fields_ctor_lines[line_index],
                             "\"app\":\"lockd\"") != NULL);
        } else {
          TEST_ASSERT(strstr(log_fields_ctor_lines[line_index], "app=lockd") !=
                      NULL);
          TEST_ASSERT(strstr(with_log_fields_ctor_lines[line_index],
                             "app=lockd") != NULL);
        }
      }
    }

    TEST_ASSERT(log_fields_ctor_sink.writes > 0);
    TEST_ASSERT(with_log_fields_ctor_sink.writes > 0);
    TEST_ASSERT(log_fields_sink.writes > 0);
    TEST_ASSERT(with_log_fields_sink.writes > 0);
    TEST_ASSERT(level_fields_ctor_sink.writes > 0);
    TEST_ASSERT(with_level_fields_ctor_sink.writes > 0);
    TEST_ASSERT(levelf_kvfmt_sink.writes > 0);
    TEST_ASSERT(with_levelf_kvfmt_sink.writes > 0);
  }
  return 0;
}

static int test_fatal_and_panic_termination(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_terminating_logger_child(0, PSLOG_LEVEL_INFO, output,
                                           sizeof(output), &exit_code,
                                           &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"fatal-child\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_logger_child(1, PSLOG_LEVEL_INFO, output,
                                           sizeof(output), &exit_code,
                                           &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"panic-child\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));
#endif
  return 0;
}

static int test_fatal_and_panic_free_wrappers_termination(void) {
#if defined(PSLOG_COVERAGE_BUILD)
  return 0;
#elif defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_terminating_wrapper_child(0, 0, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"fatal-child-wrapper\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_wrapper_child(0, 1, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"fatal-child-kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_wrapper_child(1, 0, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"panic-child-wrapper\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_wrapper_child(1, 1, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"panic-child-kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_wrapper_child(0, 3, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"f\\u0000x\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));

  TEST_ASSERT(run_terminating_wrapper_child(1, 4, PSLOG_LEVEL_INFO, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"p\\u0000x\""));
  TEST_ASSERT(contains_text(output, "\"key\":\"value\""));
#endif
  return 0;
}

static int test_fatal_and_panic_bypass_disabled_threshold(void) {
#if defined(PSLOG_COVERAGE_BUILD)
  return 0;
#elif defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_terminating_logger_child(0, PSLOG_LEVEL_DISABLED, output,
                                           sizeof(output), &exit_code,
                                           &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"fatal-child\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"fatal\""));

  TEST_ASSERT(run_terminating_logger_child(1, PSLOG_LEVEL_DISABLED, output,
                                           sizeof(output), &exit_code,
                                           &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"panic-child\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"panic\""));

  TEST_ASSERT(run_terminating_wrapper_child(0, 1, PSLOG_LEVEL_DISABLED, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"fatal-child-kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"fatal\""));

  TEST_ASSERT(run_terminating_wrapper_child(1, 1, PSLOG_LEVEL_DISABLED, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"panic-child-kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"panic\""));
#endif
  return 0;
}

static int
test_malformed_kvfmt_fatal_and_panic_still_diagnose_when_disabled(void) {
#if defined(PSLOG_COVERAGE_BUILD)
  return 0;
#elif defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  char output[4096];
  int exit_code;
  int term_signal;

  TEST_ASSERT(run_terminating_wrapper_child(0, 2, PSLOG_LEVEL_DISABLED, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == 1);
  TEST_ASSERT(term_signal == 0);
  TEST_ASSERT(contains_text(output, "\"msg\":\"invalid kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"fatal\""));
  TEST_ASSERT(!contains_text(output, "\"msg\":\"fatal-child-kvfmt\""));

  TEST_ASSERT(run_terminating_wrapper_child(1, 2, PSLOG_LEVEL_DISABLED, output,
                                            sizeof(output), &exit_code,
                                            &term_signal) == 0);
  TEST_ASSERT(exit_code == -1);
  TEST_ASSERT(term_signal == SIGABRT);
  TEST_ASSERT(contains_text(output, "\"msg\":\"invalid kvfmt\""));
  TEST_ASSERT(contains_text(output, "\"lvl\":\"panic\""));
  TEST_ASSERT(!contains_text(output, "\"msg\":\"panic-child-kvfmt\""));
#endif
  return 0;
}

static int test_plain_color_parity(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_logger *plain;
  pslog_logger *color;
  pslog_field fields[4];
  char stripped[32768];

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  fields[0] = pslog_str("value", "hello world");
  fields[1] = pslog_i64("count", 3l);
  fields[2] = pslog_bool("ok", 1);
  fields[3] = pslog_time_field("time", 1761993296l, 0l, 0);

  plain = new_logger(&plain_sink, PSLOG_MODE_JSON, PSLOG_COLOR_NEVER);
  color = new_logger(&color_sink, PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(plain != NULL);
  TEST_ASSERT(color != NULL);
  plain->info(plain, "msg", fields, 4u);
  color->info(color, "msg", fields, 4u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(strcmp(plain_sink.data, stripped) == 0);
  plain->destroy(plain);
  color->destroy(color);

  reset_sink(&plain_sink);
  reset_sink(&color_sink);
  plain = new_logger(&plain_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER);
  color = new_logger(&color_sink, PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS);
  TEST_ASSERT(plain != NULL);
  TEST_ASSERT(color != NULL);
  plain->info(plain, "msg", fields, 4u);
  color->info(color, "msg", fields, 4u);
  strip_ansi(stripped, sizeof(stripped), color_sink.data);
  TEST_ASSERT(strcmp(plain_sink.data, stripped) == 0);
  plain->destroy(plain);
  color->destroy(color);
  return 0;
}

static int test_structured_message_palette(void) {
  struct memory_sink sink;
  pslog_logger *log;
  pslog_config config;
  pslog_palette palette;

  reset_sink(&sink);
  pslog_default_config(&config);
  palette = *pslog_palette_default();
  palette.message = "[MSG]";
  palette.message_key = "[MSGKEY]";
  palette.key = "[KEY]";
  palette.number = "[NUM]";
  config.mode = PSLOG_MODE_JSON;
  config.color = PSLOG_COLOR_ALWAYS;
  config.timestamps = 0;
  config.palette = &palette;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->infof(log, "msg", "foo=%d", 1);
  TEST_ASSERT(contains_text(sink.data, "[MSGKEY]\"msg\""));
  TEST_ASSERT(contains_text(sink.data, "[MSG]\"msg\""));
  TEST_ASSERT(contains_text(sink.data, "[KEY]\"foo\""));
  TEST_ASSERT(contains_text(sink.data, "[NUM]1"));
  log->destroy(log);
  return 0;
}

static int test_long_json_palette_literals_fit_cache_or_fallback(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_logger_impl *impl;
  pslog_shared_state *shared;
  pslog_palette palette;
  char key_color[160];
  char message_color[176];
  char level_color[184];
  char reset_color[168];
  char stripped[sizeof(sink.data)];
  size_t offset;

  offset = 0u;
  while (offset + 5u < sizeof(key_color)) {
    memcpy(key_color + offset, "\x1b[36m", 5u);
    offset += 5u;
  }
  key_color[offset] = '\0';
  offset = 0u;
  while (offset + 7u < sizeof(message_color)) {
    memcpy(message_color + offset, "\x1b[1;35m", 7u);
    offset += 7u;
  }
  message_color[offset] = '\0';
  offset = 0u;
  while (offset + 7u < sizeof(level_color)) {
    memcpy(level_color + offset, "\x1b[1;32m", 7u);
    offset += 7u;
  }
  level_color[offset] = '\0';
  offset = 0u;
  while (offset + 4u < sizeof(reset_color)) {
    memcpy(reset_color + offset, "\x1b[0m", 4u);
    offset += 4u;
  }
  reset_color[offset] = '\0';

  reset_sink(&sink);
  pslog_default_config(&config);
  palette = *pslog_palette_default();
  palette.key = key_color;
  palette.message_key = key_color;
  palette.message = message_color;
  palette.timestamp = key_color;
  palette.info = level_color;
  palette.reset = reset_color;
  config.mode = PSLOG_MODE_JSON;
  config.color = PSLOG_COLOR_ALWAYS;
  config.timestamps = 1;
  config.palette = &palette;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty_true;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  impl = (pslog_logger_impl *)log->impl;
  TEST_ASSERT(impl != NULL);
  shared = impl->shared;
  TEST_ASSERT(shared != NULL);
  TEST_ASSERT(shared->json_color_message_prefix_len <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  TEST_ASSERT(shared->json_color_timestamp_prefix_len <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  TEST_ASSERT(shared->json_color_timestamp_suffix_len <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  TEST_ASSERT(shared->json_color_level_field_lens[2] <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  TEST_ASSERT(shared->json_color_min_level_field_lens[2] <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  TEST_ASSERT(shared->console_message_prefix_len <
              PSLOG_JSON_COLOR_LITERAL_CAPACITY);
  log->infof(log, "msg", "value=%d", 7);
  strip_ansi(stripped, sizeof(stripped), sink.data);
  TEST_ASSERT(contains_text(stripped, "\"msg\""));
  TEST_ASSERT(assert_valid_json_lines(stripped) == 0);
  log->destroy(log);
  return 0;
}

static int test_color_auto_respects_isatty(void) {
  struct memory_sink plain_sink;
  struct memory_sink color_sink;
  pslog_config config;
  pslog_logger *plain;
  pslog_logger *color;

  reset_sink(&plain_sink);
  reset_sink(&color_sink);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_AUTO;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.userdata = &plain_sink;
  config.output.isatty = memory_sink_isatty;
  plain = pslog_new(&config);
  TEST_ASSERT(plain != NULL);

  config.output.userdata = &color_sink;
  config.output.isatty = memory_sink_isatty_true;
  color = pslog_new(&config);
  TEST_ASSERT(color != NULL);

  plain->info(plain, "plain", NULL, 0u);
  color->info(color, "color", NULL, 0u);

  TEST_ASSERT(!contains_text(plain_sink.data, "\x1b["));
  TEST_ASSERT(contains_text(color_sink.data, "\x1b["));

  plain->destroy(plain);
  color->destroy(color);
  return 0;
}

static int test_color_auto_calls_isatty_with_null_userdata(void) {
  pslog_config config;
  pslog_logger *log;

  reset_null_userdata_sink();
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_AUTO;
  config.output.write = null_userdata_write;
  config.output.close = null_userdata_close;
  config.output.isatty = null_userdata_isatty_true;
  config.output.userdata = NULL;
  config.output.owned = 0;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "color", NULL, 0u);
  TEST_ASSERT(g_null_userdata_sink.isatty_calls == 1);
  TEST_ASSERT(contains_text(g_null_userdata_sink.data, "\x1b["));
  log->destroy(log);
  return 0;
}

static int test_thread_safe_shared_logger(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *root;
  pslog_logger *shared;
  pslog_field field;
  pthread_t threads[6];
  struct threaded_log_context ctx[6];
  size_t i;
  size_t expected;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);
  field = pslog_str("static", "yes");
  shared = root->with(root, &field, 1u);
  TEST_ASSERT(shared != NULL);

  expected = 0u;
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    ctx[i].log = shared;
    ctx[i].thread_id = (int)i;
    ctx[i].iterations = 120;
    ctx[i].use_kvfmt = (i % 2u) ? 1 : 0;
    ctx[i].use_withf = 0;
    ctx[i].use_double = 0;
    ctx[i].failed = 0;
    ctx[i].payload = NULL;
    ctx[i].kvfmt = NULL;
    expected += (size_t)ctx[i].iterations;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_log_worker, &ctx[i]) == 0);
  }
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
    TEST_ASSERT(ctx[i].failed == 0);
  }

  TEST_ASSERT(count_text_occurrences(sink.data, "\"msg\":\"thread\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\"static\":\"yes\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\n") == expected);

  shared->destroy(shared);
  root->destroy(root);
  return 0;
#else
  return 0;
#endif
}

static int test_thread_safe_chunked_shared_logger(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *root;
  pslog_logger *shared;
  pslog_field field;
  pthread_t threads[4];
  struct threaded_log_context ctx[4];
  char payload[192];
  size_t i;
  size_t expected;

  memset(payload, 'x', sizeof(payload) - 1u);
  payload[sizeof(payload) - 4u] = 'E';
  payload[sizeof(payload) - 3u] = 'N';
  payload[sizeof(payload) - 2u] = 'D';
  payload[sizeof(payload) - 1u] = '\0';

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.line_buffer_capacity = 48u;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);
  field = pslog_str("static", "yes");
  shared = root->with(root, &field, 1u);
  TEST_ASSERT(shared != NULL);

  expected = 0u;
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    ctx[i].log = shared;
    ctx[i].thread_id = (int)i;
    ctx[i].iterations = 40;
    ctx[i].use_kvfmt = (i % 2u) ? 1 : 0;
    ctx[i].use_withf = 0;
    ctx[i].use_double = 0;
    ctx[i].failed = 0;
    ctx[i].payload = payload;
    ctx[i].kvfmt = NULL;
    expected += (size_t)ctx[i].iterations;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_log_worker, &ctx[i]) == 0);
  }
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
    TEST_ASSERT(ctx[i].failed == 0);
  }

  TEST_ASSERT(count_text_occurrences(sink.data, "\"msg\":\"thread\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\"payload\":\"") == expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "END\"") == expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\"static\":\"yes\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\n") == expected);
  TEST_ASSERT(sink.writes > (int)expected);

  shared->destroy(shared);
  root->destroy(root);
  return 0;
#else
  return 0;
#endif
}

static int test_thread_safe_console_and_double_caches(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pthread_t threads[6];
  struct threaded_log_context ctx[6];
  size_t i;
  size_t expected;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  expected = 0u;
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    ctx[i].log = log;
    ctx[i].thread_id = (int)i;
    ctx[i].iterations = 120;
    ctx[i].use_kvfmt = 0;
    ctx[i].use_withf = 0;
    ctx[i].use_double = 1;
    ctx[i].failed = 0;
    ctx[i].payload = NULL;
    ctx[i].kvfmt = NULL;
    expected += (size_t)ctx[i].iterations;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_log_worker, &ctx[i]) == 0);
  }
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
    TEST_ASSERT(ctx[i].failed == 0);
  }

  TEST_ASSERT(count_text_occurrences(sink.data, "thread") == expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "ratio=") == expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\n") == expected);

  log->destroy(log);
  return 0;
#else
  return 0;
#endif
}

static int test_thread_safe_kvfmt_cache_entry_lifetime(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  static const char *const kvfmts[] = {
      "tid=%d seq=%d alpha=%s", "tid=%d seq=%d beta=%s",
      "tid=%d seq=%d gamma=%s", "tid=%d seq=%d delta=%s"};
  static const char *const keys[] = {
      "\"alpha\":\"payload\"", "\"beta\":\"payload\"", "\"gamma\":\"payload\"",
      "\"delta\":\"payload\""};
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pthread_t threads[4];
  struct threaded_log_context ctx[4];
  size_t i;
  size_t expected;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  expected = 0u;
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    ctx[i].log = log;
    ctx[i].thread_id = (int)i;
    ctx[i].iterations = 300;
    ctx[i].use_kvfmt = 1;
    ctx[i].use_withf = 0;
    ctx[i].use_double = 0;
    ctx[i].failed = 0;
    ctx[i].payload = "payload";
    ctx[i].kvfmt = kvfmts[i];
    expected += (size_t)ctx[i].iterations;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_log_worker, &ctx[i]) == 0);
  }
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
    TEST_ASSERT(ctx[i].failed == 0);
  }
  /* The sink remains mutable until every writer has joined. */
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(count_text_occurrences(sink.data, keys[i]) ==
                (size_t)ctx[i].iterations);
  }

  TEST_ASSERT(count_text_occurrences(sink.data, "\"msg\":\"thread\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\n") == expected);
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
#else
  return 0;
#endif
}

static int test_thread_safe_withf_kvfmt_cache_entry_lifetime(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  static const char *const kvfmts[] = {
      "tid=%d seq=%d alpha=%s", "tid=%d seq=%d beta=%s",
      "tid=%d seq=%d gamma=%s", "tid=%d seq=%d delta=%s"};
  static const char *const keys[] = {
      "\"alpha\":\"payload\"", "\"beta\":\"payload\"", "\"gamma\":\"payload\"",
      "\"delta\":\"payload\""};
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pthread_t threads[4];
  struct threaded_log_context ctx[4];
  size_t i;
  size_t expected;

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);

  expected = 0u;
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    ctx[i].log = log;
    ctx[i].thread_id = (int)i;
    ctx[i].iterations = 200;
    ctx[i].use_kvfmt = 1;
    ctx[i].use_withf = 1;
    ctx[i].use_double = 0;
    ctx[i].failed = 0;
    ctx[i].payload = "payload";
    ctx[i].kvfmt = kvfmts[i];
    expected += (size_t)ctx[i].iterations;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_log_worker, &ctx[i]) == 0);
  }
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
    TEST_ASSERT(ctx[i].failed == 0);
  }
  /* The sink remains mutable until every writer has joined. */
  for (i = 0u; i < sizeof(threads) / sizeof(threads[0]); ++i) {
    TEST_ASSERT(count_text_occurrences(sink.data, keys[i]) ==
                (size_t)ctx[i].iterations);
  }

  TEST_ASSERT(count_text_occurrences(sink.data, "\"msg\":\"thread\"") ==
              expected);
  TEST_ASSERT(count_text_occurrences(sink.data, "\n") == expected);
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);

  log->destroy(log);
  return 0;
#else
  return 0;
#endif
}

static int test_thread_safe_time_field_rendering(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *root;
  pthread_t threads[4];
  struct threaded_time_context ctx[4];
  int i;
  const char *expected[4];
  const long epoch_seconds[4] = {946684800l, 978307200l, 1009843200l,
                                 1041379200l};

  expected[0] = "\"tid\":0,\"when\":\"2000-01-01T00:00:00Z\"";
  expected[1] = "\"tid\":1,\"when\":\"2001-01-01T00:00:00Z\"";
  expected[2] = "\"tid\":2,\"when\":\"2002-01-01T00:00:00Z\"";
  expected[3] = "\"tid\":3,\"when\":\"2003-01-01T00:00:00Z\"";

  reset_sink(&sink);
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);

  for (i = 0; i < 4; ++i) {
    memset(&ctx[i], 0, sizeof(ctx[i]));
    ctx[i].log = root;
    ctx[i].thread_id = i;
    ctx[i].iterations = 200;
    ctx[i].when.epoch_seconds = epoch_seconds[i];
    ctx[i].when.nanoseconds = 0l;
    ctx[i].when.utc_offset_minutes = 0;
    TEST_ASSERT(pthread_create(&threads[i], (pthread_attr_t *)0,
                               threaded_time_worker, &ctx[i]) == 0);
  }

  for (i = 0; i < 4; ++i) {
    TEST_ASSERT(pthread_join(threads[i], (void **)0) == 0);
  }

  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);
  for (i = 0; i < 4; ++i) {
    TEST_ASSERT(count_text_occurrences(sink.data, expected[i]) == 200u);
  }

  root->destroy(root);
  return 0;
#else
  return 0;
#endif
}

static int test_observed_output_success_passthrough(void) {
  struct memory_sink sink;
  struct write_failure_sink failures;
  pslog_config config;
  pslog_logger *log;
  pslog_observed_output observed;
  pslog_observed_output_stats stats;

  reset_sink(&sink);
  memset(&failures, 0, sizeof(failures));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;
  pslog_observed_output_init(&observed, &config.output, write_failure_callback,
                             &failures);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "hello", NULL, 0u);
  log->destroy(log);

  stats = pslog_observed_output_stats_get(&observed);
  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"hello\""));
  TEST_ASSERT(failures.calls == 0);
  TEST_ASSERT(stats.failures == 0u);
  TEST_ASSERT(stats.short_writes == 0u);
  return 0;
}

static int test_observed_output_reports_short_write(void) {
  struct write_failure_sink failures;
  struct partial_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_observed_output observed;
  pslog_observed_output_stats stats;

  memset(&failures, 0, sizeof(failures));
  memset(&sink, 0, sizeof(sink));
  sink.fail_mode = 0;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = partial_sink_write;
  config.output.close = partial_sink_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 0;
  pslog_observed_output_init(&observed, &config.output, write_failure_callback,
                             &failures);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "hello", NULL, 0u);
  log->destroy(log);

  stats = pslog_observed_output_stats_get(&observed);
  TEST_ASSERT(failures.calls >= 1);
  TEST_ASSERT(failures.last.error_code == 0);
  TEST_ASSERT(failures.last.short_write == 1);
  TEST_ASSERT(failures.last.written < failures.last.attempted);
  TEST_ASSERT(stats.failures >= 1u);
  TEST_ASSERT(stats.short_writes >= 1u);
  return 0;
}

static int test_observed_output_reports_error(void) {
  struct write_failure_sink failures;
  struct partial_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_observed_output observed;
  pslog_observed_output_stats stats;

  memset(&failures, 0, sizeof(failures));
  memset(&sink, 0, sizeof(sink));
  sink.fail_mode = 23;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = partial_sink_write;
  config.output.close = partial_sink_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 0;
  pslog_observed_output_init(&observed, &config.output, write_failure_callback,
                             &failures);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "hello", NULL, 0u);
  log->destroy(log);

  stats = pslog_observed_output_stats_get(&observed);
  TEST_ASSERT(failures.calls == 1);
  TEST_ASSERT(failures.last.error_code == 23);
  TEST_ASSERT(failures.last.short_write == 1);
  TEST_ASSERT(stats.failures == 1u);
  TEST_ASSERT(stats.short_writes == 1u);
  return 0;
}

static int test_observed_output_color_auto_forwards_isatty_userdata(void) {
  struct isatty_tracking_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_observed_output observed;

  memset(&sink, 0, sizeof(sink));
  reset_sink(&sink.output);
  sink.expected_userdata = &sink;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_AUTO;
  config.output.write = isatty_tracking_write;
  config.output.close = NULL;
  config.output.isatty = isatty_tracking_isatty;
  config.output.userdata = &sink;
  pslog_observed_output_init(&observed, &config.output, NULL, NULL);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(sink.calls == 1);
  TEST_ASSERT(sink.wrong_userdata == 0);
  log->info(log, "observed-color", NULL, 0u);
  TEST_ASSERT(contains_text(sink.output.data, "\x1b["));
  log->destroy(log);
  return 0;
}

static int test_short_write_retries_small_line(void) {
  struct short_write_memory_sink sink;
  pslog_config config;
  pslog_logger *log;

  memset(&sink, 0, sizeof(sink));
  sink.max_write = 5u;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = short_write_memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->infof(log, "hello", "key=%s count=%d", "value", 9);
  log->destroy(log);

  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"hello\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"value\""));
  TEST_ASSERT(contains_text(sink.data, "\"count\":9"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);
  TEST_ASSERT(sink.writes > 1);
  TEST_ASSERT(sink.len > 0u);
  TEST_ASSERT(sink.data[sink.len - 1u] == '\n');
  return 0;
}

static int test_short_write_retries_chunked_line(void) {
  struct short_write_memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  pslog_field field;
  char payload[256];
  size_t i;

  memset(&sink, 0, sizeof(sink));
  sink.max_write = 7u;
  for (i = 0u; i < sizeof(payload) - 1u; ++i) {
    payload[i] = (char)('a' + (i % 26u));
  }
  payload[sizeof(payload) - 1u] = '\0';

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.line_buffer_capacity = 32u;
  config.output.write = short_write_memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = &sink;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  field = pslog_str("payload", payload);
  log->info(log, "chunked", &field, 1u);
  log->destroy(log);

  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"chunked\""));
  TEST_ASSERT(contains_text(sink.data, "\"payload\":\""));
  TEST_ASSERT(contains_text(sink.data, payload));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);
  TEST_ASSERT(sink.writes > 1);
  TEST_ASSERT(sink.len > 0u);
  TEST_ASSERT(sink.data[sink.len - 1u] == '\n');
  return 0;
}

static int test_close_does_not_close_user_output(void) {
  struct close_tracking_sink sink;
  pslog_config config;
  pslog_logger *log;

  memset(&sink, 0, sizeof(sink));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = close_tracking_write;
  config.output.close = close_tracking_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 0;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "before", NULL, 0u);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(sink.closed == 0);
  log->info(log, "after", NULL, 0u);
  TEST_ASSERT(sink.writes >= 2);
  log->destroy(log);
  return 0;
}

static int test_close_returns_owned_output_error(void) {
  struct partial_sink sink;
  pslog_config config;
  pslog_logger *log;

  memset(&sink, 0, sizeof(sink));
  sink.fail_mode = 19;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = partial_sink_write;
  config.output.close = partial_sink_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 1;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(log->close(log) == 19);
  TEST_ASSERT(log->close(log) == 19);
  TEST_ASSERT(sink.closes == 1);
  log->destroy(log);
  return 0;
}

static int test_close_calls_owned_output_with_null_userdata(void) {
  pslog_config config;
  pslog_logger *log;

  reset_null_userdata_sink();
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = null_userdata_write;
  config.output.close = null_userdata_close;
  config.output.isatty = NULL;
  config.output.userdata = NULL;
  config.output.owned = 1;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  log->info(log, "before", NULL, 0u);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(g_null_userdata_sink.closes == 1);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(g_null_userdata_sink.closes == 1);
  log->destroy(log);
  return 0;
}

static int
test_with_zero_length_bytes_field_does_not_free_borrowed_pointer(void) {
  static const unsigned char sentinel[] = {'x'};
  pslog_config config;
  pslog_logger *root;
  pslog_logger *child;
  pslog_field field;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  root = pslog_new(&config);
  TEST_ASSERT(root != NULL);

  field = pslog_bytes_field("blob", sentinel, 0u);
  child = root->with(root, &field, 1u);
  TEST_ASSERT(child != NULL);

  child->destroy(child);
  root->destroy(root);
  return 0;
}

static int test_observed_output_close_ownership(void) {
  struct close_tracking_sink user_sink;
  struct partial_sink owned_sink;
  pslog_config config;
  pslog_logger *log;
  pslog_observed_output observed;

  memset(&user_sink, 0, sizeof(user_sink));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = close_tracking_write;
  config.output.close = close_tracking_close;
  config.output.isatty = NULL;
  config.output.userdata = &user_sink;
  config.output.owned = 0;
  pslog_observed_output_init(&observed, &config.output, NULL, NULL);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(user_sink.closed == 0);
  log->destroy(log);

  memset(&owned_sink, 0, sizeof(owned_sink));
  owned_sink.fail_mode = 0;
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = partial_sink_write;
  config.output.close = partial_sink_close;
  config.output.isatty = NULL;
  config.output.userdata = &owned_sink;
  config.output.owned = 1;
  pslog_observed_output_init(&observed, &config.output, NULL, NULL);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(owned_sink.closes == 1);
  log->destroy(log);

  reset_null_userdata_sink();
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = null_userdata_write;
  config.output.close = null_userdata_close;
  config.output.isatty = NULL;
  config.output.userdata = NULL;
  config.output.owned = 1;
  pslog_observed_output_init(&observed, &config.output, NULL, NULL);
  config.output = observed.output;

  log = pslog_new(&config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(log->close(log) == 0);
  TEST_ASSERT(g_null_userdata_sink.closes == 1);
  log->destroy(log);
  return 0;
}

static int test_close_closes_owned_env_output(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char file_data[4096];
  size_t nread;

  reset_sink(&sink);
  make_temp_path(path, sizeof(path));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_OUTPUT", path);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "before_close", NULL, 0u);
  TEST_ASSERT(log->close(log) == 0);
  log->info(log, "after_close", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(file_data, "before_close"));
  TEST_ASSERT(!contains_text(file_data, "after_close"));
  TEST_ASSERT(sink.len == 0u);
  return 0;
}

static int test_env_output_override_closes_replaced_owned_sink(void) {
  struct close_tracking_sink sink;
  pslog_config config;
  pslog_logger *log;

  memset(&sink, 0, sizeof(sink));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = close_tracking_write;
  config.output.close = close_tracking_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 1;

  set_env_value("LOG_OUTPUT", "stdout");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(sink.closed == 1);
  log->info(log, "stdout_sink", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  TEST_ASSERT(sink.closed == 1);
  TEST_ASSERT(sink.writes == 0);
  return 0;
}

static int test_env_output_default_tee_keeps_reused_owned_sink_open(void) {
  struct close_tracking_sink sink;
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char env_value[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  memset(&sink, 0, sizeof(sink));
  make_temp_path(path, sizeof(path));
  strcpy(env_value, "default+");
  strcat(env_value, path);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = close_tracking_write;
  config.output.close = close_tracking_close;
  config.output.isatty = NULL;
  config.output.userdata = &sink;
  config.output.owned = 1;

  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(sink.closed == 0);
  log->info(log, "tee_default", NULL, 0u);
  TEST_ASSERT(sink.writes == 1);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  TEST_ASSERT(sink.closed == 1);

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(file_data, "\"msg\":\"tee_default\""));
  return 0;
}

static int test_env_output_default_tee_closes_owned_null_userdata_sink(void) {
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char env_value[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  reset_null_userdata_sink();
  make_temp_path(path, sizeof(path));
  strcpy(env_value, "default+");
  strcat(env_value, path);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = null_userdata_write;
  config.output.close = null_userdata_close;
  config.output.isatty = NULL;
  config.output.userdata = NULL;
  config.output.owned = 1;

  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(g_null_userdata_sink.closes == 0);
  log->info(log, "tee_default_null_userdata", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  TEST_ASSERT(g_null_userdata_sink.closes == 1);

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(
      contains_text(file_data, "\"msg\":\"tee_default_null_userdata\""));
  return 0;
}

static int test_close_on_child_does_not_close_root_owned_output(void) {
  pslog_config config;
  pslog_logger *root;
  pslog_logger *child;
  FILE *fp;
  char path[L_tmpnam + 16];
  char file_data[4096];
  size_t nread;

  make_temp_path(path, sizeof(path));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  set_env_value("LOG_OUTPUT", path);
  root = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(root != NULL);
  child = root->with(root, NULL, 0u);
  TEST_ASSERT(child != NULL);
  TEST_ASSERT(child->close(child) == 0);
  root->info(root, "still_open", NULL, 0u);
  root->close(root);
  root->info(root, "after_root_close", NULL, 0u);
  child->destroy(child);
  root->destroy(root);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(file_data, "still_open"));
  TEST_ASSERT(!contains_text(file_data, "after_root_close"));
  return 0;
}

static int test_env_output_tee_default_and_file(void) {
  struct memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char env_value[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  reset_sink(&sink);
  make_temp_path(path, sizeof(path));
  strcpy(env_value, "default+");
  strcat(env_value, path);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "tee", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(sink.data, "INF tee"));
  TEST_ASSERT(contains_text(file_data, "INF tee"));
  return 0;
}

static int test_env_output_tee_default_and_file_retries_short_write(void) {
  struct short_write_memory_sink sink;
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char env_value[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  memset(&sink, 0, sizeof(sink));
  sink.max_write = 7u;
  make_temp_path(path, sizeof(path));
  strcpy(env_value, "default+");
  strcat(env_value, path);

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = short_write_memory_sink_write;
  config.output.close = NULL;
  config.output.isatty = memory_sink_isatty;
  config.output.userdata = &sink;

  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->infof(log, "tee", "key=%s count=%d", "value", 7);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(sink.data, "\"msg\":\"tee\""));
  TEST_ASSERT(contains_text(sink.data, "\"key\":\"value\""));
  TEST_ASSERT(contains_text(sink.data, "\"count\":7"));
  TEST_ASSERT(assert_valid_json_lines(sink.data) == 0);
  TEST_ASSERT(strcmp(sink.data, file_data) == 0);
  TEST_ASSERT(sink.writes > 1);
  return 0;
}

static int test_env_output_path_with_plus(void) {
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  make_temp_path(path, sizeof(path));
  strcat(path, "+suffix.log");

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  set_env_value("LOG_OUTPUT", path);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "plus_path", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(file_data, "plus_path"));
  TEST_ASSERT(!contains_text(file_data, "logger.output.open.failed"));
  return 0;
}

static int test_env_output_file_mode_default(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  pslog_config config;
  pslog_logger *log;
  char path[L_tmpnam + 16];
  struct stat st;

  make_temp_path(path, sizeof(path));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  set_env_value("LOG_OUTPUT", path);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "mode_default", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  TEST_ASSERT(stat(path, &st) == 0);
  remove(path);
  TEST_ASSERT(((unsigned int)st.st_mode & 0777u & ~0600u) == 0u);
#else
  return 0;
#endif
  return 0;
}

static int test_env_output_file_mode_override(void) {
#if defined(__unix__) || defined(__APPLE__) || defined(__FreeBSD__)
  pslog_config config;
  pslog_logger *log;
  char path[L_tmpnam + 16];
  struct stat st;

  make_temp_path(path, sizeof(path));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  set_env_value("LOG_OUTPUT", path);
  set_env_value("LOG_OUTPUT_FILE_MODE", "0644");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "mode_override", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");
  unset_env_value("LOG_OUTPUT_FILE_MODE");

  TEST_ASSERT(stat(path, &st) == 0);
  remove(path);
  TEST_ASSERT(((unsigned int)st.st_mode & 0777u & ~0644u) == 0u);
#else
  return 0;
#endif
  return 0;
}

static int test_new_from_env_prefixed_aliases_and_modes(void) {
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char path[L_tmpnam + 16];
  char file_data[4096];
  size_t nread;

  make_temp_path(path, sizeof(path));
  pslog_default_config(&config);
  config.mode = PSLOG_MODE_CONSOLE;
  config.timestamps = 1;
  config.color = PSLOG_COLOR_ALWAYS;

  set_env_value("APP_MODE", "structured");
  set_env_value("APP_DISABLE_TIMESTAMP", "no");
  set_env_value("APP_NO_COLOR", "yes");
  set_env_value("APP_OUTPUT", path);
  set_env_value("APP_OUTPUT_FILE_MODE", "0o640");

  log = pslog_new_from_env("APP_", &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "prefixed-env", NULL, 0u);
  log->destroy(log);

  unset_env_value("APP_MODE");
  unset_env_value("APP_DISABLE_TIMESTAMP");
  unset_env_value("APP_NO_COLOR");
  unset_env_value("APP_OUTPUT");
  unset_env_value("APP_OUTPUT_FILE_MODE");

  fp = fopen(path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(path);

  TEST_ASSERT(contains_text(file_data, "\"msg\":\"prefixed-env\""));
  TEST_ASSERT(contains_text(file_data, "\"ts\":\""));
  TEST_ASSERT(!contains_text(file_data, "\x1b["));
  return 0;
}

static int test_env_output_stdout_and_stderr_plus_file(void) {
  pslog_config config;
  pslog_logger *log;
  FILE *fp;
  char stdout_path[L_tmpnam + 16];
  char stderr_path[L_tmpnam + 16];
  char env_value[L_tmpnam + 32];
  char file_data[4096];
  size_t nread;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;

  make_temp_path(stdout_path, sizeof(stdout_path));
  strcpy(env_value, "stdout+");
  strcat(env_value, stdout_path);
  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "stdout_plus_file", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(stdout_path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(stdout_path);
  TEST_ASSERT(contains_text(file_data, "stdout_plus_file"));

  make_temp_path(stderr_path, sizeof(stderr_path));
  strcpy(env_value, "stderr+");
  strcat(env_value, stderr_path);
  set_env_value("LOG_OUTPUT", env_value);
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  log->info(log, "stderr_plus_file", NULL, 0u);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");

  fp = fopen(stderr_path, "rb");
  TEST_ASSERT(fp != NULL);
  nread = fread(file_data, 1u, sizeof(file_data) - 1u, fp);
  file_data[nread] = '\0';
  fclose(fp);
  remove(stderr_path);
  TEST_ASSERT(contains_text(file_data, "stderr_plus_file"));
  return 0;
}

static int test_env_override_closes_owned_seed_output_with_null_userdata(void) {
  pslog_config config;
  pslog_logger *log;

  pslog_default_config(&config);
  config.mode = PSLOG_MODE_JSON;
  config.timestamps = 0;
  config.color = PSLOG_COLOR_NEVER;
  config.output.write = null_userdata_write;
  config.output.close = null_userdata_close;
  config.output.isatty = null_userdata_isatty_true;
  config.output.userdata = NULL;
  config.output.owned = 1;

  reset_null_userdata_sink();
  set_env_value("LOG_OUTPUT", "stdout");
  log = pslog_new_from_env(NULL, &config);
  TEST_ASSERT(log != NULL);
  TEST_ASSERT(g_null_userdata_sink.closes == 1);
  log->destroy(log);
  unset_env_value("LOG_OUTPUT");
  return 0;
}

int main(void) {
  if (test_length_aware_strings() != 0) {
    return 1;
  }
  if (test_length_aware_field_constructors() != 0) {
    return 1;
  }
  if (test_length_aware_nul_at_start_and_color() != 0) {
    return 1;
  }
  if (test_console_fields() != 0) {
    return 1;
  }
  if (test_console_output_matches_format() != 0) {
    return 1;
  }
  if (test_json_fields() != 0) {
    return 1;
  }
  if (test_finite_float_formatting() != 0) {
    return 1;
  }
  if (test_integer_field_extremes() != 0) {
    return 1;
  }
  if (test_i64_u64_preserve_64bit_payloads() != 0) {
    return 1;
  }
  if (test_kvfmt_cache_matches_content_not_pointer() != 0) {
    return 1;
  }
  if (test_kvfmt_pointer_cache_shorter_reused_buffer_infof() != 0) {
    return 1;
  }
  if (test_kvfmt_pointer_cache_shorter_reused_buffer_withf() != 0) {
    return 1;
  }
  if (test_infof_kvfmt() != 0) {
    return 1;
  }
  if (test_malformed_kvfmt_rejects_suffix_after_verb() != 0) {
    return 1;
  }
#if !defined(PSLOG_SINGLE_HEADER_TEST)
  if (test_parse_kvfmt_null_shared_local_entry_is_initialized() != 0) {
    return 1;
  }
#endif
  if (test_kvfmt_long_key_falls_back_without_overflow() != 0) {
    return 1;
  }
  if (test_errno_field_renders_error_text_and_color() != 0) {
    return 1;
  }
  if (test_kvfmt_errno_percent_m() != 0) {
    return 1;
  }
  if (test_withf_errno_percent_m_snapshots_errno() != 0) {
    return 1;
  }
  if (test_errno_static_field_prefix_renders_error_text_and_color() != 0) {
    return 1;
  }
  if (test_verbose_fields_expand_builtin_keys_in_json() != 0) {
    return 1;
  }
  if (test_withf_child_logger() != 0) {
    return 1;
  }
  if (test_nolevel_output_and_parse() != 0) {
    return 1;
  }
  if (test_with_child_logger() != 0) {
    return 1;
  }
  if (test_palette_alias_and_color_json() != 0) {
    return 1;
  }
  if (test_palette_exports_match_lookup() != 0) {
    return 1;
  }
  if (test_palette_enumeration_api() != 0) {
    return 1;
  }
  if (test_default_palette_matches_go_pslog() != 0) {
    return 1;
  }
  if (test_wrappers_noop_on_null() != 0) {
    return 1;
  }
  if (test_public_wrappers_and_scalar_paths_json() != 0) {
    return 1;
  }
  if (test_public_wrappers_and_scalar_paths_console() != 0) {
    return 1;
  }
  if (test_output_init_file_api() != 0) {
    return 1;
  }
  if (test_output_from_fp_owned_api() != 0) {
    return 1;
  }
  if (test_output_destroy_calls_close_with_null_userdata() != 0) {
    return 1;
  }
  if (test_errno_string_unknown_code_falls_back() != 0) {
    return 1;
  }
  if (test_observed_output_null_target_passthrough() != 0) {
    return 1;
  }
  if (test_buffer_reserve_growth_preserves_data() != 0) {
    return 1;
  }
  if (test_buffer_rejects_capacity_overflow() != 0) {
    return 1;
  }
  if (test_buffer_exact_inline_capacity_uses_heap_storage() != 0) {
    return 1;
  }
  if (test_buffer_heap_init_failure_keeps_inline_nul_capacity() != 0) {
    return 1;
  }
#if !defined(PSLOG_SINGLE_HEADER_TEST)
  if (test_buffer_fast_char_append_drops_when_local_growth_fails() != 0) {
    return 1;
  }
#endif
  if (test_level_parse_and_string_api() != 0) {
    return 1;
  }
  if (test_json_color_additional_paths() != 0) {
    return 1;
  }
  if (test_json_plain_kvfmt_include_level_field() != 0) {
    return 1;
  }
  if (test_console_color_additional_paths() != 0) {
    return 1;
  }
  if (test_internal_buffer_helper_paths() != 0) {
    return 1;
  }
  if (test_with_level_controls() != 0) {
    return 1;
  }
  if (test_disabled_level_is_not_emitted_by_generic_dispatch() != 0) {
    return 1;
  }
  if (test_constructor_with_level_field_matches_derived_logger() != 0) {
    return 1;
  }
  if (test_new_from_env() != 0) {
    return 1;
  }
  if (test_new_from_env_trimmed_overrides() != 0) {
    return 1;
  }
  if (test_new_from_env_invalid_mode_keeps_seed() != 0) {
    return 1;
  }
  if (test_new_from_env_invalid_palette_falls_back_default() != 0) {
    return 1;
  }
  if (test_new_from_env_time_format() != 0) {
    return 1;
  }
  if (test_new_from_env_utc() != 0) {
    return 1;
  }
  if (test_json_default_timestamp_has_zone_suffix() != 0) {
    return 1;
  }
  if (test_timestamp_cache_per_second() != 0) {
    return 1;
  }
  if (test_timestamp_cache_disabled_for_rfc3339nano() != 0) {
    return 1;
  }
  if (test_new_from_env_output_open_failure() != 0) {
    return 1;
  }
  if (test_new_from_env_output_file_mode_invalid() != 0) {
    return 1;
  }
  if (test_console_message_escape() != 0) {
    return 1;
  }
  if (test_unicode_printable_all_modes() != 0) {
    return 1;
  }
  if (test_time_and_duration_fields() != 0) {
    return 1;
  }
  if (test_console_quoting_del_and_highbit() != 0) {
    return 1;
  }
  if (test_console_escape_preserves_indent_spaces() != 0) {
    return 1;
  }
  if (test_keyed_fields_preserved_across_lines() != 0) {
    return 1;
  }
  if (test_duration_micro_sign_all_modes() != 0) {
    return 1;
  }
  if (test_trusted_string_helpers() != 0) {
    return 1;
  }
  if (test_json_string_escape_contract() != 0) {
    return 1;
  }
  if (test_json_invalid_utf8_bytes_are_escaped() != 0) {
    return 1;
  }
  if (test_console_empty_string_matches_go_pslog() != 0) {
    return 1;
  }
  if (test_json_non_finite_float_policy() != 0) {
    return 1;
  }
  if (test_line_buffer_capacity_override() != 0) {
    return 1;
  }
  if (test_line_buffer_capacity_overflow_is_rejected() != 0) {
    return 1;
  }
  if (test_with_rejects_field_count_overflow() != 0) {
    return 1;
  }
  if (test_console_prefix_cache_rejects_key_len_overflow() != 0) {
    return 1;
  }
  if (test_console_prefix_cache_checks_mutated_key_contents() != 0) {
    return 1;
  }
  if (test_with_preserves_embedded_nul_string_lengths() != 0) {
    return 1;
  }
  if (test_with_preserves_untrusted_string_lengths() != 0) {
    return 1;
  }
  if (test_json_untrusted_embedded_nul_string_uses_value_len() != 0) {
    return 1;
  }
  if (test_zero_alloc_hot_path_emission() != 0) {
    return 1;
  }
  if (test_benchmark_fixture_equivalence() != 0) {
    return 1;
  }
  if (test_repeated_double_rendering_regression() != 0) {
    return 1;
  }
  if (test_production_benchmark_fixture_equivalence() != 0) {
    return 1;
  }
  if (test_fatal_and_panic_termination() != 0) {
    return 1;
  }
  if (test_fatal_and_panic_free_wrappers_termination() != 0) {
    return 1;
  }
  if (test_fatal_and_panic_bypass_disabled_threshold() != 0) {
    return 1;
  }
  if (test_malformed_kvfmt_fatal_and_panic_still_diagnose_when_disabled() !=
      0) {
    return 1;
  }
  if (test_logger_variants_coverage() != 0) {
    return 1;
  }
  if (test_plain_color_parity() != 0) {
    return 1;
  }
  if (test_structured_message_palette() != 0) {
    return 1;
  }
  if (test_long_json_palette_literals_fit_cache_or_fallback() != 0) {
    return 1;
  }
  if (test_color_auto_respects_isatty() != 0) {
    return 1;
  }
  if (test_color_auto_calls_isatty_with_null_userdata() != 0) {
    return 1;
  }
  if (test_thread_safe_shared_logger() != 0) {
    return 1;
  }
  if (test_thread_safe_chunked_shared_logger() != 0) {
    return 1;
  }
  if (test_thread_safe_console_and_double_caches() != 0) {
    return 1;
  }
  if (test_thread_safe_kvfmt_cache_entry_lifetime() != 0) {
    return 1;
  }
  if (test_thread_safe_withf_kvfmt_cache_entry_lifetime() != 0) {
    return 1;
  }
  if (test_thread_safe_time_field_rendering() != 0) {
    return 1;
  }
  if (test_observed_output_success_passthrough() != 0) {
    return 1;
  }
  if (test_observed_output_reports_short_write() != 0) {
    return 1;
  }
  if (test_observed_output_reports_error() != 0) {
    return 1;
  }
  if (test_observed_output_color_auto_forwards_isatty_userdata() != 0) {
    return 1;
  }
  if (test_short_write_retries_small_line() != 0) {
    return 1;
  }
  if (test_short_write_retries_chunked_line() != 0) {
    return 1;
  }
  if (test_close_does_not_close_user_output() != 0) {
    return 1;
  }
  if (test_close_returns_owned_output_error() != 0) {
    return 1;
  }
  if (test_close_calls_owned_output_with_null_userdata() != 0) {
    return 1;
  }
  if (test_with_zero_length_bytes_field_does_not_free_borrowed_pointer() != 0) {
    return 1;
  }
  if (test_observed_output_close_ownership() != 0) {
    return 1;
  }
  if (test_close_closes_owned_env_output() != 0) {
    return 1;
  }
  if (test_env_output_override_closes_replaced_owned_sink() != 0) {
    return 1;
  }
  if (test_env_output_default_tee_keeps_reused_owned_sink_open() != 0) {
    return 1;
  }
  if (test_env_output_default_tee_closes_owned_null_userdata_sink() != 0) {
    return 1;
  }
  if (test_close_on_child_does_not_close_root_owned_output() != 0) {
    return 1;
  }
  if (test_env_output_tee_default_and_file() != 0) {
    return 1;
  }
  if (test_env_output_tee_default_and_file_retries_short_write() != 0) {
    return 1;
  }
  if (test_env_output_path_with_plus() != 0) {
    return 1;
  }
  if (test_env_output_file_mode_default() != 0) {
    return 1;
  }
  if (test_env_output_file_mode_override() != 0) {
    return 1;
  }
  if (test_new_from_env_prefixed_aliases_and_modes() != 0) {
    return 1;
  }
  if (test_env_output_stdout_and_stderr_plus_file() != 0) {
    return 1;
  }
  if (test_env_override_closes_owned_seed_output_with_null_userdata() != 0) {
    return 1;
  }
  return 0;
}
