#include "bench/bench_fixture.h"
#ifdef PSLOG_HAVE_LIBLOGGER_BENCH
#include "bench/bench_liblogger.h"
#endif
#include "pslog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct bench_sink {
  unsigned long writes;
  unsigned long bytes;
};

static int bench_sink_write(void *userdata, const char *data, size_t len,
                            size_t *written) {
  struct bench_sink *sink;

  sink = (struct bench_sink *)userdata;
  sink->writes += 1ul;
  sink->bytes += (unsigned long)len;
  if (written != NULL) {
    *written = len;
  }
  (void)data;
  return 0;
}

static double bench_now_seconds(void) {
#if defined(CLOCK_MONOTONIC)
  struct timespec ts;

  if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
    return (double)ts.tv_sec + ((double)ts.tv_nsec / 1000000000.0);
  }
#endif
  return (double)clock() / (double)CLOCKS_PER_SEC;
}

static void bench_print_result(const char *name, unsigned long iterations,
                               double elapsed, unsigned long bytes) {
  if (elapsed <= 0.0) {
    elapsed = 0.000001;
  }
  printf("%-24s iterations=%lu elapsed=%.6f sec ns/op=%.2f bytes/op=%.2f\n",
         name, iterations, elapsed,
         (elapsed * 1000000000.0) / (double)iterations,
         (double)bytes / (double)iterations);
}

static pslog_logger *bench_new_logger(struct bench_sink *sink, pslog_mode mode,
                                      pslog_color_mode color) {
  pslog_config config;
  pslog_logger *log;

  pslog_default_config(&config);
  config.mode = mode;
  config.color = color;
  config.min_level = PSLOG_LEVEL_TRACE;
  config.output.write = bench_sink_write;
  config.output.close = NULL;
  config.output.isatty = NULL;
  config.output.userdata = sink;
  log = pslog_new(&config);
  if (log == NULL) {
    fprintf(stderr, "failed to create logger\n");
    exit(1);
  }
  return log;
}

#ifdef PSLOG_HAVE_LIBLOGGER_BENCH
static pslog_bench_liblogger_level
bench_liblogger_level_from_pslog(pslog_level level) {
  switch (level) {
  case PSLOG_LEVEL_TRACE:
    return PSLOG_BENCH_LIBLOGGER_TRACE;
  case PSLOG_LEVEL_DEBUG:
    return PSLOG_BENCH_LIBLOGGER_DEBUG;
  case PSLOG_LEVEL_INFO:
    return PSLOG_BENCH_LIBLOGGER_INFO;
  case PSLOG_LEVEL_WARN:
    return PSLOG_BENCH_LIBLOGGER_WARN;
  case PSLOG_LEVEL_ERROR:
    return PSLOG_BENCH_LIBLOGGER_ERROR;
  case PSLOG_LEVEL_NOLEVEL:
    return PSLOG_BENCH_LIBLOGGER_NOLEVEL;
  default:
    return PSLOG_BENCH_LIBLOGGER_INFO;
  }
}

static pslog_bench_liblogger_field_kind
bench_liblogger_kind_from_bench(bench_field_kind kind) {
  switch (kind) {
  case BENCH_FIELD_STRING:
    return PSLOG_BENCH_LIBLOGGER_FIELD_STRING;
  case BENCH_FIELD_BOOL:
    return PSLOG_BENCH_LIBLOGGER_FIELD_BOOL;
  case BENCH_FIELD_INT64:
    return PSLOG_BENCH_LIBLOGGER_FIELD_INT64;
  case BENCH_FIELD_UINT64:
    return PSLOG_BENCH_LIBLOGGER_FIELD_UINT64;
  case BENCH_FIELD_FLOAT64:
    return PSLOG_BENCH_LIBLOGGER_FIELD_FLOAT64;
  case BENCH_FIELD_NULL:
  default:
    return PSLOG_BENCH_LIBLOGGER_FIELD_NULL;
  }
}

static void
bench_prepare_liblogger_dataset(const bench_entry_spec *entries,
                                size_t entry_count,
                                pslog_bench_liblogger_entry_spec *out_entries,
                                pslog_bench_liblogger_field_spec *out_fields) {
  size_t i;
  size_t offset;

  offset = 0u;
  for (i = 0u; i < entry_count; ++i) {
    size_t j;

    out_entries[i].level = bench_liblogger_level_from_pslog(entries[i].level);
    out_entries[i].message = entries[i].message;
    out_entries[i].field_count = entries[i].field_count;
    out_entries[i].fields = out_fields + offset;

    for (j = 0u; j < entries[i].field_count; ++j) {
      const bench_field_spec *src;
      pslog_bench_liblogger_field_spec *dst;

      src = &entries[i].fields[j];
      dst = &out_fields[offset + j];
      memset(dst, 0, sizeof(*dst));
      dst->key = src->key;
      dst->kind = bench_liblogger_kind_from_bench(src->kind);
      dst->string_value = src->string_value;
      dst->signed_value = src->signed_value;
      dst->unsigned_value = src->unsigned_value;
      dst->double_value = src->double_value;
      dst->bool_value = src->bool_value;
    }
    offset += entries[i].field_count;
  }
}

static void run_liblogger_fixed(const char *name, unsigned long iterations) {
  enum { bench_entry_count = 256, bench_field_count = 4 };
  pslog_bench_liblogger_entry_spec entries[bench_entry_count];
  pslog_bench_liblogger_field_spec
      fields[bench_entry_count * bench_field_count];
  pslog_bench_liblogger_result result;
  size_t i;

  memset(entries, 0, sizeof(entries));
  memset(fields, 0, sizeof(fields));
  for (i = 0u; i < bench_entry_count; ++i) {
    pslog_bench_liblogger_field_spec *entry_fields;

    entry_fields = &fields[i * bench_field_count];
    entries[i].level = PSLOG_BENCH_LIBLOGGER_INFO;
    entries[i].message = "bench";
    entries[i].fields = entry_fields;
    entries[i].field_count = bench_field_count;

    entry_fields[0].key = "request_id";
    entry_fields[0].kind = PSLOG_BENCH_LIBLOGGER_FIELD_INT64;
    entry_fields[0].signed_value = (long)i;

    entry_fields[1].key = "path";
    entry_fields[1].kind = PSLOG_BENCH_LIBLOGGER_FIELD_STRING;
    entry_fields[1].string_value = "/v1/messages";

    entry_fields[2].key = "cached";
    entry_fields[2].kind = PSLOG_BENCH_LIBLOGGER_FIELD_BOOL;
    entry_fields[2].bool_value = (i % 2u) == 0u;

    entry_fields[3].key = "ms";
    entry_fields[3].kind = PSLOG_BENCH_LIBLOGGER_FIELD_FLOAT64;
    entry_fields[3].double_value = 1.25 + (double)(i % 7u);
  }

  if (pslog_bench_liblogger_run(entries, bench_entry_count, iterations,
                                &result) != 0) {
    fprintf(stderr, "failed to run liblogger fixed benchmark\n");
    exit(1);
  }
  bench_print_result(name, iterations, (double)result.elapsed_ns / 1000000000.0,
                     (unsigned long)result.bytes);
}

static void run_liblogger_production(const char *name,
                                     unsigned long iterations) {
  pslog_bench_liblogger_entry_spec *entries;
  pslog_bench_liblogger_field_spec *fields;
  pslog_bench_liblogger_result result;
  size_t total_fields;
  size_t i;

  total_fields = 0u;
  for (i = 0u; i < pslog_bench_production_dataset.entry_count; ++i) {
    total_fields += pslog_bench_production_dataset.entries[i].field_count;
  }

  entries = (pslog_bench_liblogger_entry_spec *)calloc(
      pslog_bench_production_dataset.entry_count, sizeof(*entries));
  fields =
      (pslog_bench_liblogger_field_spec *)calloc(total_fields, sizeof(*fields));
  if (entries == NULL || fields == NULL) {
    fprintf(stderr, "failed to allocate liblogger production dataset\n");
    free(entries);
    free(fields);
    exit(1);
  }

  bench_prepare_liblogger_dataset(pslog_bench_production_dataset.entries,
                                  pslog_bench_production_dataset.entry_count,
                                  entries, fields);
  if (pslog_bench_liblogger_run(entries,
                                pslog_bench_production_dataset.entry_count,
                                iterations, &result) != 0) {
    fprintf(stderr, "failed to run liblogger production benchmark\n");
    free(entries);
    free(fields);
    exit(1);
  }

  bench_print_result(name, iterations, (double)result.elapsed_ns / 1000000000.0,
                     (unsigned long)result.bytes);
  free(entries);
  free(fields);
}
#endif

static void run_fixed_api(const char *name, pslog_mode mode,
                          pslog_color_mode color, unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field fields[4];
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    fields[0] = pslog_i64("request_id", (long)i);
    fields[1] = pslog_str("path", "/v1/messages");
    fields[2] = pslog_bool("cached", (i % 2ul) == 0ul);
    fields[3] = pslog_f64("ms", 1.25 + (double)(i % 7ul));
    log->info(log, "bench", fields, 4u);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  log->destroy(log);
}

static void run_fixed_prepared(const char *name, pslog_mode mode,
                               pslog_color_mode color,
                               unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field fields[4];
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);

  fields[0] = pslog_i64("request_id", 0l);
  fields[1] = pslog_str("path", "/v1/messages");
  fields[2] = pslog_bool("cached", 1);
  fields[3] = pslog_f64("ms", 1.25);

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    fields[0].as.signed_value = (long)i;
    fields[2].as.bool_value = (i % 2ul) == 0ul ? 1 : 0;
    fields[3].as.double_value = 1.25 + (double)(i % 7ul);
    log->info(log, "bench", fields, 4u);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  log->destroy(log);
}

static void run_fixed_api_view(const char *name, pslog_mode mode,
                               pslog_color_mode color,
                               unsigned long iterations) {
  const pslog_string_view message = {"bench", 5u};
  const pslog_string_view request_id = {"request_id", 10u};
  const pslog_string_view path = {"path", 4u};
  const pslog_string_view path_value = {"/v1/messages", 12u};
  const pslog_string_view cached = {"cached", 6u};
  const pslog_string_view ms = {"ms", 2u};
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field fields[4];
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    fields[0] = pslog_i64_view(request_id, (long)i);
    fields[1] = pslog_str_view(path, path_value);
    fields[2] = pslog_bool_view(cached, (i % 2ul) == 0ul);
    fields[3] = pslog_f64_view(ms, 1.25 + (double)(i % 7ul));
    log->info_view(log, message, fields, 4u);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  log->destroy(log);
}

static void run_fixed_prepared_view(const char *name, pslog_mode mode,
                                    pslog_color_mode color,
                                    unsigned long iterations) {
  const pslog_string_view message = {"bench", 5u};
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field fields[4];
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);

  fields[0] = pslog_i64_view((pslog_string_view){"request_id", 10u}, 0l);
  fields[1] = pslog_str_view((pslog_string_view){"path", 4u},
                             (pslog_string_view){"/v1/messages", 12u});
  fields[2] = pslog_bool_view((pslog_string_view){"cached", 6u}, 1);
  fields[3] = pslog_f64_view((pslog_string_view){"ms", 2u}, 1.25);

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    fields[0].as.signed_value = (long)i;
    fields[2].as.bool_value = (i % 2ul) == 0ul ? 1 : 0;
    fields[3].as.double_value = 1.25 + (double)(i % 7ul);
    log->info_view(log, message, fields, 4u);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  log->destroy(log);
}

static void run_fixed_pair(const char *name, pslog_mode mode,
                           pslog_color_mode color, unsigned long iterations) {
  char api_name[64];
  char prepared_name[64];

  sprintf(api_name, "%s_api", name);
  sprintf(prepared_name, "%s_prepared", name);
  run_fixed_api(api_name, mode, color, iterations);
  run_fixed_prepared(prepared_name, mode, color, iterations);
}

static void run_fixed_view_pair(const char *name, pslog_mode mode,
                                pslog_color_mode color,
                                unsigned long iterations) {
  char api_name[64];
  char prepared_name[64];

  sprintf(api_name, "%s_api_view", name);
  sprintf(prepared_name, "%s_prepared_view", name);
  run_fixed_api_view(api_name, mode, color, iterations);
  run_fixed_prepared_view(prepared_name, mode, color, iterations);
}

static void run_production_log_fields_ctor(const char *name, pslog_mode mode,
                                           pslog_color_mode color,
                                           unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field *fields;
  size_t max_fields;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);
  max_fields =
      bench_dataset_max_fields(pslog_bench_production_dataset.entries,
                               pslog_bench_production_dataset.entry_count);
  fields = NULL;
  if (max_fields > 0u) {
    fields = (pslog_field *)calloc(max_fields, sizeof(pslog_field));
    if (fields == NULL) {
      fprintf(stderr, "failed to allocate production api fields\n");
      log->destroy(log);
      exit(1);
    }
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_entry_spec *entry;

    entry = &pslog_bench_production_dataset
                 .entries[i % pslog_bench_production_dataset.entry_count];
    bench_build_fields(entry->fields, entry->field_count, fields);
    log->log(log, entry->level, entry->message, fields, entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  free(fields);
  log->destroy(log);
}

static void run_production_with_log_fields_ctor(const char *name,
                                                pslog_mode mode,
                                                pslog_color_mode color,
                                                unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *root;
  pslog_logger *active;
  bench_prepared_fields prepared_static;
  const bench_entry_spec *entries;
  size_t entry_count;
  size_t max_fields;
  pslog_field *fields;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  memset(&prepared_static, 0, sizeof(prepared_static));
  root = bench_new_logger(&sink, mode, color);
  active = root;

  entries = pslog_bench_production_dataset.entries;
  entry_count = pslog_bench_production_dataset.entry_count;
  if (pslog_bench_production_dataset.static_field_count > 0u) {
    if (bench_prepare_fields(pslog_bench_production_dataset.static_fields,
                             pslog_bench_production_dataset.static_field_count,
                             &prepared_static) != 0) {
      fprintf(stderr, "failed to prepare production static fields\n");
      root->destroy(root);
      exit(1);
    }
    active =
        root->with(root, prepared_static.fields, prepared_static.field_count);
    if (active == NULL) {
      fprintf(stderr, "failed to derive production with logger\n");
      bench_destroy_prepared_fields(&prepared_static);
      root->destroy(root);
      exit(1);
    }
    entries = pslog_bench_production_dataset.dynamic_entries;
    entry_count = pslog_bench_production_dataset.dynamic_entry_count;
  }

  max_fields = bench_dataset_max_fields(entries, entry_count);
  fields = NULL;
  if (max_fields > 0u) {
    fields = (pslog_field *)calloc(max_fields, sizeof(pslog_field));
    if (fields == NULL) {
      fprintf(stderr, "failed to allocate production with fields\n");
      if (active != root) {
        active->destroy(active);
      }
      bench_destroy_prepared_fields(&prepared_static);
      root->destroy(root);
      exit(1);
    }
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_entry_spec *entry;

    entry = &entries[i % entry_count];
    bench_build_fields(entry->fields, entry->field_count, fields);
    active->log(active, entry->level, entry->message, fields,
                entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  free(fields);
  if (active != root) {
    active->destroy(active);
  }
  bench_destroy_prepared_fields(&prepared_static);
  root->destroy(root);
}

static void run_production_log_fields(const char *name, pslog_mode mode,
                                      pslog_color_mode color,
                                      unsigned long iterations) {
  struct bench_sink sink;
  bench_prepared_dataset prepared_all;
  pslog_logger *log;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  memset(&prepared_all, 0, sizeof(prepared_all));
  log = bench_new_logger(&sink, mode, color);

  if (bench_prepare_dataset(pslog_bench_production_dataset.entries,
                            pslog_bench_production_dataset.entry_count,
                            &prepared_all) != 0) {
    fprintf(stderr, "failed to prepare full production dataset\n");
    log->destroy(log);
    exit(1);
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_prepared_entry *entry;

    entry = &prepared_all.entries[i % prepared_all.entry_count];
    log->log(log, entry->level, entry->message, entry->fields,
             entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  bench_destroy_prepared_dataset(&prepared_all);
  log->destroy(log);
}

static void run_production_with_log_fields(const char *name, pslog_mode mode,
                                           pslog_color_mode color,
                                           unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *root;
  pslog_logger *active;
  bench_prepared_dataset prepared_dynamic;
  bench_prepared_fields prepared_static;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  memset(&prepared_dynamic, 0, sizeof(prepared_dynamic));
  memset(&prepared_static, 0, sizeof(prepared_static));
  root = bench_new_logger(&sink, mode, color);
  active = root;

  if (pslog_bench_production_dataset.static_field_count > 0u) {
    if (bench_prepare_fields(pslog_bench_production_dataset.static_fields,
                             pslog_bench_production_dataset.static_field_count,
                             &prepared_static) != 0 ||
        bench_prepare_dataset(
            pslog_bench_production_dataset.dynamic_entries,
            pslog_bench_production_dataset.dynamic_entry_count,
            &prepared_dynamic) != 0) {
      fprintf(stderr, "failed to prepare production with dataset\n");
      bench_destroy_prepared_fields(&prepared_static);
      bench_destroy_prepared_dataset(&prepared_dynamic);
      root->destroy(root);
      exit(1);
    }
    active =
        root->with(root, prepared_static.fields, prepared_static.field_count);
    if (active == NULL) {
      fprintf(stderr, "failed to derive production with logger\n");
      bench_destroy_prepared_fields(&prepared_static);
      bench_destroy_prepared_dataset(&prepared_dynamic);
      root->destroy(root);
      exit(1);
    }
  } else if (bench_prepare_dataset(pslog_bench_production_dataset.entries,
                                   pslog_bench_production_dataset.entry_count,
                                   &prepared_dynamic) != 0) {
    fprintf(stderr, "failed to prepare production dataset\n");
    root->destroy(root);
    exit(1);
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_prepared_entry *entry;

    entry = &prepared_dynamic.entries[i % prepared_dynamic.entry_count];
    active->log(active, entry->level, entry->message, entry->fields,
                entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  if (active != root) {
    active->destroy(active);
  }
  bench_destroy_prepared_fields(&prepared_static);
  bench_destroy_prepared_dataset(&prepared_dynamic);
  root->destroy(root);
}

static void run_production_level_fields_ctor(const char *name, pslog_mode mode,
                                             pslog_color_mode color,
                                             unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *log;
  pslog_field *fields;
  size_t max_fields;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);
  max_fields =
      bench_dataset_max_fields(pslog_bench_production_dataset.entries,
                               pslog_bench_production_dataset.entry_count);
  fields = NULL;
  if (max_fields > 0u) {
    fields = (pslog_field *)calloc(max_fields, sizeof(pslog_field));
    if (fields == NULL) {
      fprintf(stderr, "failed to allocate production level fields\n");
      log->destroy(log);
      exit(1);
    }
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_entry_spec *entry;

    entry = &pslog_bench_production_dataset
                 .entries[i % pslog_bench_production_dataset.entry_count];
    bench_build_fields(entry->fields, entry->field_count, fields);
    bench_log_level_fields(log, entry->level, entry->message, fields,
                           entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  free(fields);
  log->destroy(log);
}

static void run_production_with_level_fields_ctor(const char *name,
                                                  pslog_mode mode,
                                                  pslog_color_mode color,
                                                  unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *root;
  pslog_logger *active;
  bench_prepared_fields prepared_static;
  const bench_entry_spec *entries;
  size_t entry_count;
  size_t max_fields;
  pslog_field *fields;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  memset(&prepared_static, 0, sizeof(prepared_static));
  root = bench_new_logger(&sink, mode, color);
  active = root;

  entries = pslog_bench_production_dataset.entries;
  entry_count = pslog_bench_production_dataset.entry_count;
  if (pslog_bench_production_dataset.static_field_count > 0u) {
    if (bench_prepare_fields(pslog_bench_production_dataset.static_fields,
                             pslog_bench_production_dataset.static_field_count,
                             &prepared_static) != 0) {
      fprintf(stderr, "failed to prepare production static fields\n");
      root->destroy(root);
      exit(1);
    }
    active =
        root->with(root, prepared_static.fields, prepared_static.field_count);
    if (active == NULL) {
      fprintf(stderr, "failed to derive production with logger\n");
      bench_destroy_prepared_fields(&prepared_static);
      root->destroy(root);
      exit(1);
    }
    entries = pslog_bench_production_dataset.dynamic_entries;
    entry_count = pslog_bench_production_dataset.dynamic_entry_count;
  }

  max_fields = bench_dataset_max_fields(entries, entry_count);
  fields = NULL;
  if (max_fields > 0u) {
    fields = (pslog_field *)calloc(max_fields, sizeof(pslog_field));
    if (fields == NULL) {
      fprintf(stderr, "failed to allocate production with level fields\n");
      if (active != root) {
        active->destroy(active);
      }
      bench_destroy_prepared_fields(&prepared_static);
      root->destroy(root);
      exit(1);
    }
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    const bench_entry_spec *entry;

    entry = &entries[i % entry_count];
    bench_build_fields(entry->fields, entry->field_count, fields);
    bench_log_level_fields(active, entry->level, entry->message, fields,
                           entry->field_count);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  free(fields);
  if (active != root) {
    active->destroy(active);
  }
  bench_destroy_prepared_fields(&prepared_static);
  root->destroy(root);
}

static void run_production_levelf_kvfmt(const char *name, pslog_mode mode,
                                        pslog_color_mode color,
                                        unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *log;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  log = bench_new_logger(&sink, mode, color);

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    pslog_bench_production_levelf_dataset
        .calls[i % pslog_bench_production_levelf_dataset.entry_count](log);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  log->destroy(log);
}

static void run_production_with_levelf_kvfmt(const char *name, pslog_mode mode,
                                             pslog_color_mode color,
                                             unsigned long iterations) {
  struct bench_sink sink;
  pslog_logger *root;
  pslog_logger *active;
  bench_prepared_fields prepared_static;
  double start;
  double finish;
  unsigned long i;

  memset(&sink, 0, sizeof(sink));
  memset(&prepared_static, 0, sizeof(prepared_static));
  root = bench_new_logger(&sink, mode, color);
  active = root;

  if (pslog_bench_production_dataset.static_field_count > 0u) {
    if (bench_prepare_fields(pslog_bench_production_dataset.static_fields,
                             pslog_bench_production_dataset.static_field_count,
                             &prepared_static) != 0) {
      fprintf(stderr, "failed to prepare production static fields\n");
      root->destroy(root);
      exit(1);
    }
    active =
        root->with(root, prepared_static.fields, prepared_static.field_count);
    if (active == NULL) {
      fprintf(stderr, "failed to derive production with logger\n");
      bench_destroy_prepared_fields(&prepared_static);
      root->destroy(root);
      exit(1);
    }
  }

  start = bench_now_seconds();
  for (i = 0ul; i < iterations; ++i) {
    pslog_bench_production_dynamic_levelf_dataset
        .calls[i % pslog_bench_production_dynamic_levelf_dataset.entry_count](
            active);
  }
  finish = bench_now_seconds();

  bench_print_result(name, iterations, finish - start, sink.bytes);
  if (active != root) {
    active->destroy(active);
  }
  bench_destroy_prepared_fields(&prepared_static);
  root->destroy(root);
}

static void run_production_suite(const char *name, pslog_mode mode,
                                 pslog_color_mode color,
                                 unsigned long iterations) {
  char log_fields_name[96];
  char with_log_fields_name[96];
  char log_fields_build_name[96];
  char with_log_fields_build_name[96];
  char level_fields_build_name[96];
  char with_level_fields_build_name[96];
  char levelf_kvfmt_name[96];
  char with_levelf_kvfmt_name[96];

  sprintf(log_fields_name, "%s_prod_log_fields", name);
  sprintf(with_log_fields_name, "%s_prod_with_log_fields", name);
  sprintf(log_fields_build_name, "%s_prod_log_fields_build", name);
  sprintf(with_log_fields_build_name, "%s_prod_with_log_fields_build", name);
  sprintf(level_fields_build_name, "%s_prod_level_fields_build", name);
  sprintf(with_level_fields_build_name, "%s_prod_with_level_fields_build",
          name);
  sprintf(levelf_kvfmt_name, "%s_prod_levelf_kvfmt", name);
  sprintf(with_levelf_kvfmt_name, "%s_prod_with_levelf_kvfmt", name);
  run_production_log_fields(log_fields_name, mode, color, iterations);
  run_production_with_log_fields(with_log_fields_name, mode, color, iterations);
  run_production_log_fields_ctor(log_fields_build_name, mode, color,
                                 iterations);
  run_production_with_log_fields_ctor(with_log_fields_build_name, mode, color,
                                      iterations);
  run_production_level_fields_ctor(level_fields_build_name, mode, color,
                                   iterations);
  run_production_with_level_fields_ctor(with_level_fields_build_name, mode,
                                        color, iterations);
  run_production_levelf_kvfmt(levelf_kvfmt_name, mode, color, iterations);
  run_production_with_levelf_kvfmt(with_levelf_kvfmt_name, mode, color,
                                   iterations);
}

static void run_all(unsigned long iterations) {
  run_fixed_pair("console", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER, iterations);
  run_fixed_view_pair("console", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                      iterations);
  run_fixed_pair("consolecolor", PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                 iterations);
  run_fixed_view_pair("consolecolor", PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                      iterations);
  run_fixed_pair("json", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER, iterations);
  run_fixed_view_pair("json", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER, iterations);
  run_fixed_pair("jsoncolor", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS, iterations);
  run_fixed_view_pair("jsoncolor", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                      iterations);

  run_production_suite("console", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                       iterations);
  run_production_suite("consolecolor", PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                       iterations);
  run_production_suite("json", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER, iterations);
  run_production_suite("jsoncolor", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                       iterations);
#ifdef PSLOG_HAVE_LIBLOGGER_BENCH
  run_liblogger_fixed("liblogger_json", iterations);
  run_liblogger_production("liblogger_json_prod", iterations);
#endif
}

int main(int argc, char **argv) {
  unsigned long iterations;
  int ran;

  iterations = 200000ul;
  if (argc > 1) {
    iterations = (unsigned long)strtoul(argv[1], (char **)0, 10);
    if (iterations == 0ul) {
      iterations = 200000ul;
    }
  }

  ran = 0;
  if (argc <= 2) {
    run_all(iterations);
    return 0;
  }

  {
    int i;

    for (i = 2; i < argc; ++i) {
      if (strcmp(argv[i], "console") == 0) {
        run_fixed_pair("console", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_api") == 0) {
        run_fixed_api("console_api", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                      iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prepared") == 0) {
        run_fixed_prepared("console_prepared", PSLOG_MODE_CONSOLE,
                           PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_api_view") == 0) {
        run_fixed_api_view("console_api_view", PSLOG_MODE_CONSOLE,
                           PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prepared_view") == 0) {
        run_fixed_prepared_view("console_prepared_view", PSLOG_MODE_CONSOLE,
                                PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod") == 0 ||
                 strcmp(argv[i], "console_production") == 0) {
        run_production_suite("console", PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                             iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_log_fields") == 0 ||
                 strcmp(argv[i], "console_prod_api") == 0) {
        run_production_log_fields("console_prod_log_fields", PSLOG_MODE_CONSOLE,
                                  PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "console_prod_with") == 0 ||
                 strcmp(argv[i], "console_prod_prepared") == 0) {
        run_production_with_log_fields("console_prod_with_log_fields",
                                       PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_log_fields_build") == 0 ||
                 strcmp(argv[i], "console_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "console_prod_public_api") == 0) {
        run_production_log_fields_ctor("console_prod_log_fields_build",
                                       PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_with_log_fields_build") == 0 ||
                 strcmp(argv[i], "console_prod_with_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "console_prod_public_with") == 0) {
        run_production_with_log_fields_ctor(
            "console_prod_with_log_fields_build", PSLOG_MODE_CONSOLE,
            PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_level_fields_build") == 0 ||
                 strcmp(argv[i], "console_prod_level_fields_ctor") == 0) {
        run_production_level_fields_ctor("console_prod_level_fields_build",
                                         PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_with_level_fields_build") == 0 ||
                 strcmp(argv[i], "console_prod_with_level_fields_ctor") == 0) {
        run_production_with_level_fields_ctor(
            "console_prod_with_level_fields_build", PSLOG_MODE_CONSOLE,
            PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_levelf_kvfmt") == 0) {
        run_production_levelf_kvfmt("console_prod_levelf_kvfmt",
                                    PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                                    iterations);
        ran = 1;
      } else if (strcmp(argv[i], "console_prod_with_levelf_kvfmt") == 0) {
        run_production_with_levelf_kvfmt("console_prod_with_levelf_kvfmt",
                                         PSLOG_MODE_CONSOLE, PSLOG_COLOR_NEVER,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor") == 0 ||
                 strcmp(argv[i], "colcon") == 0) {
        run_fixed_pair("consolecolor", PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_api") == 0 ||
                 strcmp(argv[i], "colcon_api") == 0) {
        run_fixed_api("consolecolor_api", PSLOG_MODE_CONSOLE,
                      PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prepared") == 0 ||
                 strcmp(argv[i], "colcon_prepared") == 0) {
        run_fixed_prepared("consolecolor_prepared", PSLOG_MODE_CONSOLE,
                           PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_api_view") == 0) {
        run_fixed_api_view("consolecolor_api_view", PSLOG_MODE_CONSOLE,
                           PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prepared_view") == 0) {
        run_fixed_prepared_view("consolecolor_prepared_view",
                                PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod") == 0 ||
                 strcmp(argv[i], "colcon_prod") == 0) {
        run_production_suite("consolecolor", PSLOG_MODE_CONSOLE,
                             PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_log_fields") == 0 ||
                 strcmp(argv[i], "colcon_prod_log_fields") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_api") == 0 ||
                 strcmp(argv[i], "colcon_prod_api") == 0) {
        run_production_log_fields("consolecolor_prod_log_fields",
                                  PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                  iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "colcon_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_with") == 0 ||
                 strcmp(argv[i], "colcon_prod_with") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_prepared") == 0 ||
                 strcmp(argv[i], "colcon_prod_prepared") == 0) {
        run_production_with_log_fields("consolecolor_prod_with_log_fields",
                                       PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_log_fields_build") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "colcon_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_public_api") == 0 ||
                 strcmp(argv[i], "colcon_prod_public_api") == 0) {
        run_production_log_fields_ctor("consolecolor_prod_log_fields_build",
                                       PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_with_log_fields_build") ==
                     0 ||
                 strcmp(argv[i], "consolecolor_prod_with_log_fields_ctor") ==
                     0 ||
                 strcmp(argv[i], "colcon_prod_with_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_public_with") == 0 ||
                 strcmp(argv[i], "colcon_prod_public_with") == 0) {
        run_production_with_log_fields_ctor(
            "consolecolor_prod_with_log_fields_build", PSLOG_MODE_CONSOLE,
            PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_level_fields_build") == 0 ||
                 strcmp(argv[i], "consolecolor_prod_level_fields_ctor") == 0 ||
                 strcmp(argv[i], "colcon_prod_level_fields_ctor") == 0) {
        run_production_level_fields_ctor("consolecolor_prod_level_fields_build",
                                         PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_with_level_fields_build") ==
                     0 ||
                 strcmp(argv[i], "consolecolor_prod_with_level_fields_ctor") ==
                     0 ||
                 strcmp(argv[i], "colcon_prod_with_level_fields_ctor") == 0) {
        run_production_with_level_fields_ctor(
            "consolecolor_prod_with_level_fields_build", PSLOG_MODE_CONSOLE,
            PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_levelf_kvfmt") == 0 ||
                 strcmp(argv[i], "colcon_prod_levelf_kvfmt") == 0) {
        run_production_levelf_kvfmt("consolecolor_prod_levelf_kvfmt",
                                    PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                    iterations);
        ran = 1;
      } else if (strcmp(argv[i], "consolecolor_prod_with_levelf_kvfmt") == 0 ||
                 strcmp(argv[i], "colcon_prod_with_levelf_kvfmt") == 0) {
        run_production_with_levelf_kvfmt("consolecolor_prod_with_levelf_kvfmt",
                                         PSLOG_MODE_CONSOLE, PSLOG_COLOR_ALWAYS,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json") == 0) {
        run_fixed_pair("json", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_api") == 0) {
        run_fixed_api("json_api", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                      iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prepared") == 0) {
        run_fixed_prepared("json_prepared", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                           iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_api_view") == 0) {
        run_fixed_api_view("json_api_view", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                           iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prepared_view") == 0) {
        run_fixed_prepared_view("json_prepared_view", PSLOG_MODE_JSON,
                                PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod") == 0 ||
                 strcmp(argv[i], "json_production") == 0) {
        run_production_suite("json", PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                             iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_log_fields") == 0 ||
                 strcmp(argv[i], "json_prod_api") == 0) {
        run_production_log_fields("json_prod_log_fields", PSLOG_MODE_JSON,
                                  PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "json_prod_with") == 0 ||
                 strcmp(argv[i], "json_prod_prepared") == 0) {
        run_production_with_log_fields("json_prod_with_log_fields",
                                       PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_log_fields_build") == 0 ||
                 strcmp(argv[i], "json_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "json_prod_public_api") == 0) {
        run_production_log_fields_ctor("json_prod_log_fields_build",
                                       PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_with_log_fields_build") == 0 ||
                 strcmp(argv[i], "json_prod_with_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "json_prod_public_with") == 0) {
        run_production_with_log_fields_ctor("json_prod_with_log_fields_build",
                                            PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                                            iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_level_fields_build") == 0 ||
                 strcmp(argv[i], "json_prod_level_fields_ctor") == 0) {
        run_production_level_fields_ctor("json_prod_level_fields_build",
                                         PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_with_level_fields_build") == 0 ||
                 strcmp(argv[i], "json_prod_with_level_fields_ctor") == 0) {
        run_production_with_level_fields_ctor(
            "json_prod_with_level_fields_build", PSLOG_MODE_JSON,
            PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_levelf_kvfmt") == 0) {
        run_production_levelf_kvfmt("json_prod_levelf_kvfmt", PSLOG_MODE_JSON,
                                    PSLOG_COLOR_NEVER, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "json_prod_with_levelf_kvfmt") == 0) {
        run_production_with_levelf_kvfmt("json_prod_with_levelf_kvfmt",
                                         PSLOG_MODE_JSON, PSLOG_COLOR_NEVER,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor") == 0 ||
                 strcmp(argv[i], "coljson") == 0) {
        run_fixed_pair("jsoncolor", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_api") == 0 ||
                 strcmp(argv[i], "coljson_api") == 0) {
        run_fixed_api("jsoncolor_api", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                      iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prepared") == 0 ||
                 strcmp(argv[i], "coljson_prepared") == 0) {
        run_fixed_prepared("jsoncolor_prepared", PSLOG_MODE_JSON,
                           PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_api_view") == 0) {
        run_fixed_api_view("jsoncolor_api_view", PSLOG_MODE_JSON,
                           PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prepared_view") == 0) {
        run_fixed_prepared_view("jsoncolor_prepared_view", PSLOG_MODE_JSON,
                                PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod") == 0 ||
                 strcmp(argv[i], "coljson_prod") == 0) {
        run_production_suite("jsoncolor", PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                             iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_log_fields") == 0 ||
                 strcmp(argv[i], "coljson_prod_log_fields") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_api") == 0 ||
                 strcmp(argv[i], "coljson_prod_api") == 0) {
        run_production_log_fields("jsoncolor_prod_log_fields", PSLOG_MODE_JSON,
                                  PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "coljson_prod_with_log_fields") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_with") == 0 ||
                 strcmp(argv[i], "coljson_prod_with") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_prepared") == 0 ||
                 strcmp(argv[i], "coljson_prod_prepared") == 0) {
        run_production_with_log_fields("jsoncolor_prod_with_log_fields",
                                       PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_log_fields_build") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "coljson_prod_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_public_api") == 0 ||
                 strcmp(argv[i], "coljson_prod_public_api") == 0) {
        run_production_log_fields_ctor("jsoncolor_prod_log_fields_build",
                                       PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                                       iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_with_log_fields_build") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_with_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "coljson_prod_with_log_fields_ctor") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_public_with") == 0 ||
                 strcmp(argv[i], "coljson_prod_public_with") == 0) {
        run_production_with_log_fields_ctor(
            "jsoncolor_prod_with_log_fields_build", PSLOG_MODE_JSON,
            PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_level_fields_build") == 0 ||
                 strcmp(argv[i], "jsoncolor_prod_level_fields_ctor") == 0 ||
                 strcmp(argv[i], "coljson_prod_level_fields_ctor") == 0) {
        run_production_level_fields_ctor("jsoncolor_prod_level_fields_build",
                                         PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                                         iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_with_level_fields_build") ==
                     0 ||
                 strcmp(argv[i], "jsoncolor_prod_with_level_fields_ctor") ==
                     0 ||
                 strcmp(argv[i], "coljson_prod_with_level_fields_ctor") == 0) {
        run_production_with_level_fields_ctor(
            "jsoncolor_prod_with_level_fields_build", PSLOG_MODE_JSON,
            PSLOG_COLOR_ALWAYS, iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_levelf_kvfmt") == 0 ||
                 strcmp(argv[i], "coljson_prod_levelf_kvfmt") == 0) {
        run_production_levelf_kvfmt("jsoncolor_prod_levelf_kvfmt",
                                    PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                                    iterations);
        ran = 1;
      } else if (strcmp(argv[i], "jsoncolor_prod_with_levelf_kvfmt") == 0 ||
                 strcmp(argv[i], "coljson_prod_with_levelf_kvfmt") == 0) {
        run_production_with_levelf_kvfmt("jsoncolor_prod_with_levelf_kvfmt",
                                         PSLOG_MODE_JSON, PSLOG_COLOR_ALWAYS,
                                         iterations);
        ran = 1;
#ifdef PSLOG_HAVE_LIBLOGGER_BENCH
      } else if (strcmp(argv[i], "liblogger") == 0 ||
                 strcmp(argv[i], "liblogger_json") == 0) {
        run_liblogger_fixed("liblogger_json", iterations);
        ran = 1;
      } else if (strcmp(argv[i], "liblogger_json_prod") == 0 ||
                 strcmp(argv[i], "liblogger_prod") == 0) {
        run_liblogger_production("liblogger_json_prod", iterations);
        ran = 1;
#endif
      } else if (strcmp(argv[i], "all") == 0) {
        run_all(iterations);
        ran = 1;
      } else {
        fprintf(stderr, "unknown variant: %s\n", argv[i]);
        return 1;
      }
    }
  }

  if (!ran) {
    fprintf(stderr, "no variants selected\n");
    return 1;
  }

  return 0;
}
