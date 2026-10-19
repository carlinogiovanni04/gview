#define GVIEW_IMPLEMENTATION
#include "gview.h"

#include <stdio.h>
#include <string.h>

static int g_failures = 0;

#define CHECK(cond)                                                            \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);             \
            g_failures++;                                                      \
        }                                                                      \
    } while (0)

#define CHECK_STR(a, b) CHECK(strcmp((a), (b)) == 0)

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t n;
    if (!f) return 0;
    n = fwrite(text, 1, strlen(text), f);
    fclose(f);
    return n == strlen(text);
}

static void test_nested(void)
{
    const char *path = "gview_test_nested.json";
    const char *json =
        "{\"traceEvents\":["
        "{\"ph\":\"M\",\"pid\":1,\"tid\":7,\"name\":\"thread_name\",\"args\":{\"name\":\"worker\"}},"
        "{\"ph\":\"B\",\"pid\":1,\"tid\":7,\"ts\":100.0,\"name\":\"frame\"},"
        "{\"ph\":\"B\",\"pid\":1,\"tid\":7,\"ts\":110.5,\"name\":\"update\"},"
        "{\"ph\":\"E\",\"pid\":1,\"tid\":7,\"ts\":200.0},"
        "{\"ph\":\"X\",\"pid\":1,\"tid\":7,\"ts\":210,\"dur\":30.5,\"name\":\"render\"},"
        "{\"ph\":\"E\",\"pid\":1,\"tid\":7,\"ts\":300},"
        "{\"ph\":\"B\",\"pid\":1,\"tid\":8,\"ts\":50,\"name\":\"other\"},"
        "{\"ph\":\"E\",\"pid\":1,\"tid\":8,\"ts\":80}"
        "]}";
    GViewTrace trace;
    GViewFlame flame;
    char       err[256];
    uint32_t   frame_node;

    CHECK(write_text(path, json));

    gview_trace_init(&trace);
    CHECK(gview_trace_load_file(&trace, path, err, sizeof err));
    CHECK(trace.loaded);
    CHECK(trace.span_count == 4);
    CHECK(trace.thread_count == 2);
    CHECK(trace.name_count == 4);
    CHECK(trace.t0 == 50);
    CHECK(trace.t1 == 300);
    CHECK(trace.bad_events == 0);
    CHECK(trace.unmatched_ends == 0);
    CHECK(trace.open_at_end == 0);
    CHECK(trace.x_events == 1);

    CHECK_STR(trace.threads[0].label, "worker");
    CHECK_STR(trace.threads[1].label, "1:8");
    CHECK(trace.threads[0].span_count == 3);
    CHECK(trace.threads[1].span_count == 1);

    CHECK_STR(gview_name(&trace, trace.spans[0].name_id), "frame");
    CHECK_STR(gview_name(&trace, trace.spans[1].name_id), "update");
    CHECK_STR(gview_name(&trace, trace.spans[2].name_id), "render");
    CHECK_STR(gview_name(&trace, trace.spans[3].name_id), "other");

    CHECK(trace.spans[0].start_us == 100);
    CHECK(trace.spans[0].dur_us == 200);
    CHECK(trace.spans[0].self_us == 80);
    CHECK(trace.spans[0].parent == GV_SPAN_NONE);
    CHECK(trace.spans[0].depth == 0);
    CHECK(trace.spans[0].child_count == 2);
    CHECK(trace.spans[0].complete == 1);

    CHECK(trace.spans[1].start_us == 111);
    CHECK(trace.spans[1].dur_us == 89);
    CHECK(trace.spans[1].self_us == 89);
    CHECK(trace.spans[1].parent == 0);
    CHECK(trace.spans[1].depth == 1);

    CHECK(trace.spans[2].start_us == 210);
    CHECK(trace.spans[2].dur_us == 31);
    CHECK(trace.spans[2].self_us == 31);
    CHECK(trace.spans[2].parent == 0);
    CHECK(trace.spans[2].depth == 1);

    CHECK(trace.spans[3].dur_us == 30);
    CHECK(trace.spans[3].parent == GV_SPAN_NONE);

    gview_flame_init(&flame);
    CHECK(gview_flame_build(&flame, &trace));
    CHECK(flame.count == 5);
    CHECK(flame.root_total == 230);

    frame_node = trace.spans[0].flame_id;
    CHECK(frame_node != GV_SPAN_NONE);
    CHECK_STR(gview_name(&trace, flame.nodes[frame_node].name_id), "frame");
    CHECK(flame.nodes[frame_node].total_us == 200);
    CHECK(flame.nodes[frame_node].own_us == 80);

    CHECK(trace.spans[1].flame_id != frame_node);
    CHECK(trace.spans[2].flame_id != frame_node);

    gview_flame_free(&flame);
    gview_trace_free(&trace);
    remove(path);
}

static void test_open_and_complete(void)
{
    const char *path = "gview_test_open.json";
    const char *json =
        "{\"traceEvents\":["
        "{\"ph\":\"B\",\"pid\":1,\"tid\":1,\"ts\":10,\"name\":\"open\"},"
        "{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"ts\":40,\"dur\":5,\"name\":\"late\"}"
        "]}";
    GViewTrace trace;
    char       err[256];

    CHECK(write_text(path, json));

    gview_trace_init(&trace);
    CHECK(gview_trace_load_file(&trace, path, err, sizeof err));
    CHECK(trace.span_count == 2);
    CHECK(trace.open_at_end == 1);
    CHECK(trace.bad_events == 0);
    CHECK(trace.unmatched_ends == 0);
    CHECK(trace.t0 == 10);
    CHECK(trace.t1 == 45);

    CHECK(trace.spans[0].complete == 0);
    CHECK(trace.spans[0].dur_us == 35);
    CHECK(trace.spans[0].self_us == 30);
    CHECK(trace.spans[1].parent == 0);
    CHECK(trace.spans[1].depth == 1);
    CHECK(trace.spans[1].dur_us == 5);

    gview_trace_free(&trace);
    remove(path);
}

static void test_warnings(void)
{
    const char *path = "gview_test_warn.json";
    const char *json =
        "{\"traceEvents\":["
        "{\"ph\":\"E\",\"pid\":1,\"tid\":1,\"ts\":5},"
        "{\"ph\":\"X\",\"pid\":1,\"tid\":1,\"ts\":10,\"name\":\"no_dur\"},"
        "{\"ph\":\"B\",\"pid\":1,\"tid\":1,\"name\":\"no_ts\"}"
        "]}";
    GViewTrace trace;
    char       err[256];

    CHECK(write_text(path, json));

    gview_trace_init(&trace);
    CHECK(gview_trace_load_file(&trace, path, err, sizeof err));
    CHECK(trace.span_count == 0);
    CHECK(trace.bad_events == 2);
    CHECK(trace.unmatched_ends == 1);
    CHECK(trace.t0 == 0);
    CHECK(trace.t1 == 0);

    gview_trace_free(&trace);
    remove(path);
}

static void test_escapes_and_array(void)
{
    const char *path = "gview_test_escape.json";
    const char *json =
        "["
        "{\"ph\":\"B\",\"pid\":2,\"tid\":3,\"ts\":0.25,\"name\":\"a\\\"b\\\\c\"},"
        "{\"ph\":\"E\",\"pid\":2,\"tid\":3,\"ts\":1.75},"
        "{\"ph\":\"X\",\"pid\":2,\"tid\":3,\"ts\":3,\"dur\":1,\"name\":\"caf\\u00e9\"}"
        "]";
    GViewTrace trace;
    char       err[256];

    CHECK(write_text(path, json));

    gview_trace_init(&trace);
    CHECK(gview_trace_load_file(&trace, path, err, sizeof err));
    CHECK(trace.span_count == 2);
    CHECK(trace.thread_count == 1);
    CHECK(trace.t0 == 0);
    CHECK(trace.t1 == 4);
    CHECK(trace.x_events == 1);

    CHECK_STR(gview_name(&trace, trace.spans[0].name_id), "a\"b\\c");
    CHECK(trace.spans[0].dur_us == 2);
    CHECK_STR(gview_name(&trace, trace.spans[1].name_id), "caf\xc3\xa9");
    CHECK(trace.spans[1].start_us == 3);
    CHECK(trace.spans[1].dur_us == 1);

    gview_trace_free(&trace);
    remove(path);
}

int main(void)
{
    test_nested();
    test_open_and_complete();
    test_warnings();
    test_escapes_and_array();

    if (g_failures == 0) {
        printf("all tests passed\n");
        return 0;
    }

    printf("%d check(s) failed\n", g_failures);
    return 1;
}
