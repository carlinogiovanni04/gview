//
//   gview - Chrome Trace JSON loader and model for the gview viewer
//
//   Usage:
//     #define GVIEW_IMPLEMENTATION   // in exactly ONE .c file
//     #include "gview.h"
//
//   Parses the Chrome Trace subset written by microprof:
//     B/E pairs, complete X events, decimal microsecond timestamps.
//
//

#ifndef GVIEW_H
#define GVIEW_H

#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#define _CRT_SECURE_NO_WARNINGS
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define GV_NAME_NONE 0xFFFFFFFFu
#define GV_SPAN_NONE 0xFFFFFFFFu

#ifndef GVIEW_MAX_FILE_MB
#define GVIEW_MAX_FILE_MB 256u
#endif

typedef struct {
    uint32_t pid;
    uint32_t tid;
    uint64_t start_us;
    uint64_t dur_us;
    uint64_t self_us;
    uint32_t depth;
    uint32_t parent;
    uint32_t name_id;
    uint32_t flame_id;
    uint32_t child_count;
    uint8_t  complete;
} GViewSpan;

typedef struct {
    uint32_t  pid;
    uint32_t  tid;
    char      label[64];
    uint32_t *span_idx;
    uint32_t  span_count;
    uint32_t  span_cap;
    uint32_t  max_depth;
} GViewThread;

typedef struct {
    char     **names;
    uint32_t   name_count;
    uint32_t   name_cap;

    GViewSpan *spans;
    uint32_t   span_count;
    uint32_t   span_cap;

    GViewThread *threads;
    uint32_t     thread_count;
    uint32_t     thread_cap;

    uint64_t t0;
    uint64_t t1;

    uint32_t bad_events;
    uint32_t unmatched_ends;
    uint32_t open_at_end;
    uint32_t x_events;

    int loaded;
} GViewTrace;

typedef struct {
    uint32_t name_id;
    uint64_t total_us;
    uint64_t own_us;
    uint32_t parent;
    uint32_t first_child;
    uint32_t next_sibling;
    uint32_t depth;
    uint32_t span_id;
} GViewFlameNode;

typedef struct {
    GViewFlameNode *nodes;
    uint32_t        count;
    uint32_t        cap;
    uint64_t        root_total;
} GViewFlame;

void        gview_trace_init(GViewTrace *trace);
void        gview_trace_free(GViewTrace *trace);
int         gview_trace_load_file(GViewTrace *trace, const char *path, char *err,
                                  size_t err_cap);
void        gview_trace_print_summary(const GViewTrace *trace, const char *path);
const char *gview_name(const GViewTrace *trace, uint32_t name_id);

void gview_flame_init(GViewFlame *flame);
void gview_flame_free(GViewFlame *flame);
int  gview_flame_build(GViewFlame *flame, GViewTrace *trace);

#endif // GVIEW_H

#if defined(GVIEW_IMPLEMENTATION)

static int gv__reserve(void **data, uint32_t *cap, uint32_t need, size_t elem)
{
    uint32_t n;
    void    *p;

    if (need <= *cap) {
        return 1;
    }

    n = *cap ? *cap : 8u;
    while (n < need) {
        if (n > 0x3FFFFFFFu) {
            n = need;
            break;
        }
        n *= 2u;
    }

    p = realloc(*data, (size_t)n * elem);
    if (!p) {
        return 0;
    }

    *data = p;
    *cap  = n;
    return 1;
}

static char *gv__strdup(const char *s)
{
    size_t n;
    char  *p;

    if (!s) {
        return NULL;
    }

    n = strlen(s) + 1u;
    p = (char *)malloc(n);
    if (!p) {
        return NULL;
    }

    memcpy(p, s, n);
    return p;
}

#if defined(__GNUC__) || defined(__clang__)
#define GV_UNUSED __attribute__((unused))
#else
#define GV_UNUSED
#endif

static GV_UNUSED int gv__contains_ci(const char *hay, const char *needle)
{
    const char *h;
    const char *n;

    if (!needle || !needle[0]) {
        return 1;
    }
    if (!hay) {
        return 0;
    }

    for (; *hay; hay++) {
        h = hay;
        n = needle;
        while (*h && *n) {
            char a = *h;
            char b = *n;
            if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
            if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
            if (a != b) break;
            h++;
            n++;
        }
        if (*n == '\0') {
            return 1;
        }
    }

    return 0;
}

typedef struct {
    const char *p;
    const char *end;
} GVJson;

static void gv__skip_ws(GVJson *j)
{
    while (j->p < j->end) {
        char c = *j->p;
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
            j->p++;
        } else {
            break;
        }
    }
}

static int gv__take(GVJson *j, char c)
{
    gv__skip_ws(j);
    if (j->p < j->end && *j->p == c) {
        j->p++;
        return 1;
    }
    return 0;
}

static int gv__hex4(const char *p, const char *end, uint32_t *out)
{
    uint32_t v = 0;
    int      i;

    if (p + 4 > end) {
        return 0;
    }

    for (i = 0; i < 4; i++) {
        char c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            return 0;
        }
        v = (v << 4) | d;
    }

    *out = v;
    return 1;
}

static void gv__put_utf8(char *out, size_t cap, size_t *n, uint32_t cp)
{
    if (cp <= 0x7Fu) {
        if (out && *n + 1u < cap) out[*n] = (char)cp;
        *n += 1u;
    } else if (cp <= 0x7FFu) {
        if (out && *n + 2u < cap) {
            out[*n]     = (char)(0xC0u | (cp >> 6));
            out[*n + 1] = (char)(0x80u | (cp & 0x3Fu));
        }
        *n += 2u;
    } else if (cp <= 0xFFFFu) {
        if (out && *n + 3u < cap) {
            out[*n]     = (char)(0xE0u | (cp >> 12));
            out[*n + 1] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[*n + 2] = (char)(0x80u | (cp & 0x3Fu));
        }
        *n += 3u;
    } else {
        if (out && *n + 4u < cap) {
            out[*n]     = (char)(0xF0u | (cp >> 18));
            out[*n + 1] = (char)(0x80u | ((cp >> 12) & 0x3Fu));
            out[*n + 2] = (char)(0x80u | ((cp >> 6) & 0x3Fu));
            out[*n + 3] = (char)(0x80u | (cp & 0x3Fu));
        }
        *n += 4u;
    }
}

static int gv__json_string(GVJson *j, char *out, size_t cap)
{
    size_t n = 0;

    gv__skip_ws(j);
    if (j->p >= j->end || *j->p != '"') {
        return 0;
    }
    j->p++;

    while (j->p < j->end) {
        unsigned char c = (unsigned char)*j->p++;

        if (c == '"') {
            if (out && cap) out[n < cap ? n : cap - 1u] = '\0';
            return 1;
        }

        if (c == '\\') {
            char esc;
            if (j->p >= j->end) return 0;
            esc = *j->p++;

            switch (esc) {
            case '"':  c = '"';  break;
            case '\\': c = '\\'; break;
            case '/':  c = '/';  break;
            case 'b':  c = '\b'; break;
            case 'f':  c = '\f'; break;
            case 'n':  c = '\n'; break;
            case 'r':  c = '\r'; break;
            case 't':  c = '\t'; break;
            case 'u': {
                uint32_t cp = 0;
                if (!gv__hex4(j->p, j->end, &cp)) return 0;
                j->p += 4;

                if (cp >= 0xD800u && cp <= 0xDBFFu && j->p + 6 <= j->end &&
                    j->p[0] == '\\' && j->p[1] == 'u') {
                    uint32_t lo = 0;
                    if (gv__hex4(j->p + 2, j->end, &lo) && lo >= 0xDC00u && lo <= 0xDFFFu) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + (lo - 0xDC00u);
                        j->p += 6;
                    }
                }
                gv__put_utf8(out, cap, &n, cp);
                continue;
            }
            default:
                return 0;
            }
        }

        if (out && n + 1u < cap) out[n] = (char)c;
        n++;
    }

    return 0;
}

static int gv__json_number(GVJson *j, double *out)
{
    char  *endp = NULL;
    double v;

    gv__skip_ws(j);
    if (j->p >= j->end) {
        return 0;
    }

    v = strtod(j->p, &endp);
    if (endp == j->p || endp > j->end) {
        return 0;
    }

    j->p = endp;
    if (out) *out = v;
    return 1;
}

static int gv__skip_value(GVJson *j, int depth)
{
    if (depth > 128) {
        return 0;
    }

    gv__skip_ws(j);
    if (j->p >= j->end) {
        return 0;
    }

    if (*j->p == '"') {
        return gv__json_string(j, NULL, 0);
    }

    if (*j->p == '{') {
        j->p++;
        gv__skip_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return 1;
        }
        for (;;) {
            if (!gv__json_string(j, NULL, 0)) return 0;
            if (!gv__take(j, ':')) return 0;
            if (!gv__skip_value(j, depth + 1)) return 0;
            gv__skip_ws(j);
            if (j->p < j->end && *j->p == '}') {
                j->p++;
                return 1;
            }
            if (!gv__take(j, ',')) return 0;
        }
    }

    if (*j->p == '[') {
        j->p++;
        gv__skip_ws(j);
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return 1;
        }
        for (;;) {
            if (!gv__skip_value(j, depth + 1)) return 0;
            gv__skip_ws(j);
            if (j->p < j->end && *j->p == ']') {
                j->p++;
                return 1;
            }
            if (!gv__take(j, ',')) return 0;
        }
    }

    while (j->p < j->end) {
        char c = *j->p;
        if (c == ',' || c == '}' || c == ']' || c == ' ' || c == '\t' || c == '\n' ||
            c == '\r') {
            break;
        }
        j->p++;
    }

    return 1;
}

typedef struct {
    uint32_t  pid;
    uint32_t  tid;
    char      label[64];
    uint32_t *stack;
    uint32_t  stack_cap;
    uint32_t  stack_depth;
    uint32_t *span_idx;
    uint32_t  span_count;
    uint32_t  span_cap;
    uint32_t  max_depth;
} GVParseThread;

typedef struct {
    GViewTrace    *trace;
    GVParseThread *threads;
    uint32_t       thread_count;
    uint32_t       thread_cap;

    uint32_t      *slots;
    uint32_t       slot_cap;

    uint64_t       max_ts;
} GVParse;

static GVParseThread *gv__thread(GVParse *p, uint32_t pid, uint32_t tid)
{
    uint32_t i;

    for (i = 0; i < p->thread_count; i++) {
        if (p->threads[i].pid == pid && p->threads[i].tid == tid) {
            return &p->threads[i];
        }
    }

    if (!gv__reserve((void **)&p->threads, &p->thread_cap, p->thread_count + 1u,
                     sizeof(GVParseThread))) {
        return NULL;
    }

    memset(&p->threads[p->thread_count], 0, sizeof(GVParseThread));
    p->threads[p->thread_count].pid = pid;
    p->threads[p->thread_count].tid = tid;
    snprintf(p->threads[p->thread_count].label, 64, "%u:%u", pid, tid);
    p->thread_count++;
    return &p->threads[p->thread_count - 1u];
}

static uint32_t gv__hash_name(const char *s)
{
    uint64_t h = 14695981039346656037ull;
    while (*s) {
        h ^= (unsigned char)*s++;
        h *= 1099511628211ull;
    }
    return (uint32_t)(h ^ (h >> 32));
}

static uint32_t gv__slot_lookup(const GVParse *p, const char *name)
{
    uint32_t mask = p->slot_cap - 1u;
    uint32_t i    = gv__hash_name(name) & mask;

    while (p->slots[i] != 0u) {
        uint32_t id = p->slots[i] - 1u;
        if (strcmp(p->trace->names[id], name) == 0) {
            return id;
        }
        i = (i + 1u) & mask;
    }

    return GV_NAME_NONE;
}

static int gv__slots_rehash(GVParse *p, uint32_t new_cap)
{
    uint32_t *slots = (uint32_t *)calloc(new_cap, sizeof(uint32_t));
    uint32_t  i;

    if (!slots) {
        return 0;
    }

    for (i = 0; i < p->trace->name_count; i++) {
        uint32_t mask = new_cap - 1u;
        uint32_t j    = gv__hash_name(p->trace->names[i]) & mask;
        while (slots[j] != 0u) {
            j = (j + 1u) & mask;
        }
        slots[j] = i + 1u;
    }

    free(p->slots);
    p->slots    = slots;
    p->slot_cap = new_cap;
    return 1;
}

static uint32_t gv__intern(GVParse *p, const char *name)
{
    GViewTrace *t = p->trace;
    uint32_t    i;
    uint32_t    id;
    char       *copy;

    if (!name || !name[0]) {
        name = "<unnamed>";
    }

    if (p->slot_cap) {
        id = gv__slot_lookup(p, name);
        if (id != GV_NAME_NONE) {
            return id;
        }
    } else {
        for (i = 0; i < t->name_count; i++) {
            if (strcmp(t->names[i], name) == 0) {
                return i;
            }
        }
    }

    if (!gv__reserve((void **)&t->names, &t->name_cap, t->name_count + 1u,
                     sizeof(char *))) {
        return GV_NAME_NONE;
    }

    copy = gv__strdup(name);
    if (!copy) {
        return GV_NAME_NONE;
    }

    id = t->name_count++;
    t->names[id] = copy;

    if (p->slot_cap == 0u) {
        (void)gv__slots_rehash(p, 16u);
        return id;
    }
    if (t->name_count * 4u >= p->slot_cap * 3u) {
        (void)gv__slots_rehash(p, p->slot_cap * 2u);
        return id;
    }

    {
        uint32_t mask = p->slot_cap - 1u;
        uint32_t j    = gv__hash_name(name) & mask;
        while (p->slots[j] != 0u) {
            j = (j + 1u) & mask;
        }
        p->slots[j] = id + 1u;
    }

    return id;
}

typedef struct {
    uint32_t pid;
    uint32_t tid;
    uint64_t ts;
    uint64_t dur;
    int      has_ts;
    int      has_dur;
    char     ph;
    char     name[256];
    char     args_name[256];
} GVEvent;

static int gv__parse_args(GVJson *j, char *out, size_t cap)
{
    if (!gv__take(j, '{')) {
        return 0;
    }

    gv__skip_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return 1;
    }

    for (;;) {
        char key[64];
        if (!gv__json_string(j, key, sizeof key)) return 0;
        if (!gv__take(j, ':')) return 0;

        if (strcmp(key, "name") == 0) {
            if (!gv__json_string(j, out, cap)) return 0;
        } else {
            if (!gv__skip_value(j, 0)) return 0;
        }

        gv__skip_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return 1;
        }
        if (!gv__take(j, ',')) return 0;
    }
}

static int gv__parse_event(GVJson *j, GVEvent *e)
{
    memset(e, 0, sizeof(*e));

    if (!gv__take(j, '{')) {
        return 0;
    }

    gv__skip_ws(j);
    if (j->p < j->end && *j->p == '}') {
        j->p++;
        return 1;
    }

    for (;;) {
        char key[64];

        if (!gv__json_string(j, key, sizeof key)) return 0;
        if (!gv__take(j, ':')) return 0;

        if (strcmp(key, "ph") == 0) {
            char tmp[16];
            if (!gv__json_string(j, tmp, sizeof tmp)) return 0;
            e->ph = tmp[0];
        } else if (strcmp(key, "pid") == 0) {
            double d;
            if (!gv__json_number(j, &d)) return 0;
            e->pid = (uint32_t)d;
        } else if (strcmp(key, "tid") == 0) {
            double d;
            if (!gv__json_number(j, &d)) return 0;
            e->tid = (uint32_t)d;
        } else if (strcmp(key, "ts") == 0) {
            double d;
            if (!gv__json_number(j, &d)) return 0;
            e->ts     = d > 0.0 ? (uint64_t)(d + 0.5) : 0u;
            e->has_ts = 1;
        } else if (strcmp(key, "dur") == 0) {
            double d;
            if (!gv__json_number(j, &d)) return 0;
            e->dur     = d > 0.0 ? (uint64_t)(d + 0.5) : 0u;
            e->has_dur = 1;
        } else if (strcmp(key, "name") == 0) {
            if (!gv__json_string(j, e->name, sizeof e->name)) return 0;
        } else if (strcmp(key, "args") == 0) {
            gv__skip_ws(j);
            if (j->p < j->end && *j->p == '{') {
                if (!gv__parse_args(j, e->args_name, sizeof e->args_name)) return 0;
            } else if (!gv__skip_value(j, 0)) {
                return 0;
            }
        } else {
            if (!gv__skip_value(j, 0)) return 0;
        }

        gv__skip_ws(j);
        if (j->p < j->end && *j->p == '}') {
            j->p++;
            return 1;
        }
        if (!gv__take(j, ',')) return 0;
    }
}

static void gv__register(GVParse *p, const GVEvent *e)
{
    GViewTrace *t = p->trace;
    GVParseThread *th;
    uint32_t name_id;
    uint32_t idx;

    if (e->ph == 'M') {
        if (strcmp(e->name, "thread_name") == 0 && e->args_name[0]) {
            th = gv__thread(p, e->pid, e->tid);
            if (th) {
                snprintf(th->label, sizeof th->label, "%s", e->args_name);
            }
        }
        return;
    }

    if (e->ph != 'B' && e->ph != 'E' && e->ph != 'X') {
        return;
    }

    if (!e->has_ts) {
        t->bad_events++;
        return;
    }

    th = gv__thread(p, e->pid, e->tid);
    if (!th) {
        t->bad_events++;
        return;
    }

    if (e->ph == 'B') {
        name_id = gv__intern(p, e->name);
        if (!gv__reserve((void **)&t->spans, &t->span_cap, t->span_count + 1u,
                         sizeof(GViewSpan))) {
            t->bad_events++;
            return;
        }

        idx = t->span_count++;
        memset(&t->spans[idx], 0, sizeof(GViewSpan));
        t->spans[idx].pid      = e->pid;
        t->spans[idx].tid      = e->tid;
        t->spans[idx].start_us = e->ts;
        t->spans[idx].depth    = th->stack_depth;
        t->spans[idx].parent   = th->stack_depth ? th->stack[th->stack_depth - 1u]
                                                 : GV_SPAN_NONE;
        t->spans[idx].name_id  = name_id;
        t->spans[idx].flame_id = GV_SPAN_NONE;

        if (th->stack_depth > th->max_depth) {
            th->max_depth = th->stack_depth;
        }

        if (!gv__reserve((void **)&th->stack, &th->stack_cap, th->stack_depth + 1u,
                         sizeof(uint32_t))) {
            t->bad_events++;
            return;
        }
        th->stack[th->stack_depth++] = idx;

        if (!gv__reserve((void **)&th->span_idx, &th->span_cap, th->span_count + 1u,
                         sizeof(uint32_t))) {
            t->bad_events++;
            return;
        }
        th->span_idx[th->span_count++] = idx;

        if (e->ts > p->max_ts) p->max_ts = e->ts;
        if (e->ts < t->t0) t->t0 = e->ts;
        return;
    }

    if (e->ph == 'E') {
        GViewSpan *s;
        uint64_t   end = e->ts;

        if (th->stack_depth == 0) {
            t->unmatched_ends++;
            return;
        }

        idx = th->stack[--th->stack_depth];
        s   = &t->spans[idx];
        if (end >= s->start_us) {
            s->dur_us = end - s->start_us;
        } else {
            s->dur_us = 0;
            t->bad_events++;
        }
        s->complete = 1;

        if (end > p->max_ts) p->max_ts = end;
        return;
    }

    // Complete event: no stack push.
    if (!e->has_dur) {
        t->bad_events++;
        return;
    }

    name_id = gv__intern(p, e->name);
    if (!gv__reserve((void **)&t->spans, &t->span_cap, t->span_count + 1u,
                     sizeof(GViewSpan))) {
        t->bad_events++;
        return;
    }

    idx = t->span_count++;
    memset(&t->spans[idx], 0, sizeof(GViewSpan));
    t->spans[idx].pid      = e->pid;
    t->spans[idx].tid      = e->tid;
    t->spans[idx].start_us = e->ts;
    t->spans[idx].dur_us   = e->dur;
    t->spans[idx].depth    = th->stack_depth;
    t->spans[idx].parent   = th->stack_depth ? th->stack[th->stack_depth - 1u]
                                             : GV_SPAN_NONE;
    t->spans[idx].name_id  = name_id;
    t->spans[idx].flame_id = GV_SPAN_NONE;
    t->spans[idx].complete = 1;

    if (th->stack_depth > th->max_depth) {
        th->max_depth = th->stack_depth;
    }

    if (!gv__reserve((void **)&th->span_idx, &th->span_cap, th->span_count + 1u,
                     sizeof(uint32_t))) {
        t->bad_events++;
        return;
    }
    th->span_idx[th->span_count++] = idx;

    if (e->ts > p->max_ts) p->max_ts = e->ts;
    if (e->ts + e->dur > p->max_ts) p->max_ts = e->ts + e->dur;
    if (e->ts < t->t0) t->t0 = e->ts;

    t->x_events++;
}

static int gv__parse_events(GVParse *p, GVJson *j)
{
    if (!gv__take(j, '[')) {
        return 0;
    }

    gv__skip_ws(j);
    if (j->p < j->end && *j->p == ']') {
        j->p++;
        return 1;
    }

    for (;;) {
        gv__skip_ws(j);
        if (j->p >= j->end) {
            return 0;
        }

        if (*j->p == '{') {
            GVEvent e;
            if (!gv__parse_event(j, &e)) return 0;
            gv__register(p, &e);
        } else {
            if (!gv__skip_value(j, 0)) return 0;
        }

        gv__skip_ws(j);
        if (j->p < j->end && *j->p == ']') {
            j->p++;
            return 1;
        }
        if (!gv__take(j, ',')) return 0;
    }
}

static int gv__parse_document(GVParse *p, const char *data, size_t size)
{
    GVJson j;
    int    found = 0;

    j.p   = data;
    j.end = data + size;

    gv__skip_ws(&j);
    if (j.p >= j.end) {
        return 0;
    }

    if (*j.p == '[') {
        return gv__parse_events(p, &j);
    }

    if (!gv__take(&j, '{')) {
        return 0;
    }

    for (;;) {
        char key[64];

        if (!gv__json_string(&j, key, sizeof key)) return 0;
        if (!gv__take(&j, ':')) return 0;

        if (strcmp(key, "traceEvents") == 0) {
            if (!gv__parse_events(p, &j)) return 0;
            found = 1;
        } else {
            if (!gv__skip_value(&j, 0)) return 0;
        }

        gv__skip_ws(&j);
        if (j.p < j.end && *j.p == '}') {
            j.p++;
            break;
        }
        if (!gv__take(&j, ',')) return 0;
    }

    return found;
}

void gview_trace_init(GViewTrace *trace)
{
    if (trace) {
        memset(trace, 0, sizeof(*trace));
    }
}

void gview_trace_free(GViewTrace *trace)
{
    uint32_t i;

    if (!trace) {
        return;
    }

    for (i = 0; i < trace->name_count; i++) {
        free(trace->names[i]);
    }
    free(trace->names);

    for (i = 0; i < trace->thread_count; i++) {
        free(trace->threads[i].span_idx);
    }
    free(trace->threads);
    free(trace->spans);

    memset(trace, 0, sizeof(*trace));
}

const char *gview_name(const GViewTrace *trace, uint32_t name_id)
{
    if (!trace || name_id >= trace->name_count || !trace->names[name_id]) {
        return "?";
    }
    return trace->names[name_id];
}

int gview_trace_load_file(GViewTrace *trace, const char *path, char *err, size_t err_cap)
{
    FILE    *f;
    long     file_size;
    char    *data;
    size_t   read_size;
    GVParse  p;
    GViewTrace tmp;
    int      ok;
    uint32_t i;

    if (err && err_cap) err[0] = '\0';

    if (!trace || !path) {
        if (err && err_cap) snprintf(err, err_cap, "invalid arguments");
        return 0;
    }

    gview_trace_free(trace);

    f = fopen(path, "rb");
    if (!f) {
        if (err && err_cap) snprintf(err, err_cap, "could not open %s", path);
        return 0;
    }

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        if (err && err_cap) snprintf(err, err_cap, "could not seek %s", path);
        return 0;
    }
    file_size = ftell(f);
    if (file_size < 0) {
        fclose(f);
        if (err && err_cap) snprintf(err, err_cap, "could not size %s", path);
        return 0;
    }
    if ((uint64_t)file_size > (uint64_t)GVIEW_MAX_FILE_MB * 1024u * 1024u) {
        fclose(f);
        if (err && err_cap) snprintf(err, err_cap, "trace is larger than %u MiB",
                                     GVIEW_MAX_FILE_MB);
        return 0;
    }
    if (fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        if (err && err_cap) snprintf(err, err_cap, "could not rewind %s", path);
        return 0;
    }

    data = (char *)malloc((size_t)file_size + 1u);
    if (!data) {
        fclose(f);
        if (err && err_cap) snprintf(err, err_cap, "out of memory");
        return 0;
    }

    read_size = fread(data, 1u, (size_t)file_size, f);
    fclose(f);

    if (read_size != (size_t)file_size) {
        free(data);
        if (err && err_cap) snprintf(err, err_cap, "could not read %s", path);
        return 0;
    }
    data[file_size] = '\0';

    gview_trace_init(&tmp);
    tmp.t0 = UINT64_MAX;
    memset(&p, 0, sizeof(p));
    p.trace = &tmp;

    ok = gv__parse_document(&p, data, (size_t)file_size);
    free(data);

    for (i = 0; i < p.thread_count; i++) {
        while (p.threads[i].stack_depth > 0) {
            uint32_t  idx = p.threads[i].stack[--p.threads[i].stack_depth];
            GViewSpan *s  = &tmp.spans[idx];
            s->dur_us     = p.max_ts >= s->start_us ? p.max_ts - s->start_us : 0;
            s->complete   = 0;
            tmp.open_at_end++;
        }
    }

    if (ok) {
        tmp.t1 = p.max_ts;
        if (tmp.t0 == UINT64_MAX || tmp.span_count == 0) tmp.t0 = 0;
        if (tmp.t1 < tmp.t0) tmp.t1 = tmp.t0;

        if (!gv__reserve((void **)&tmp.threads, &tmp.thread_cap, p.thread_count,
                         sizeof(GViewThread))) {
            ok = 0;
        }
    }

    if (ok) {
        for (i = 0; i < p.thread_count; i++) {
            GViewThread *th = &tmp.threads[tmp.thread_count++];
            memset(th, 0, sizeof(*th));
            th->pid        = p.threads[i].pid;
            th->tid        = p.threads[i].tid;
            th->span_idx   = p.threads[i].span_idx;
            th->span_count = p.threads[i].span_count;
            th->span_cap   = p.threads[i].span_cap;
            th->max_depth  = p.threads[i].max_depth;
            snprintf(th->label, sizeof th->label, "%s", p.threads[i].label);
            p.threads[i].span_idx = NULL;
        }

        for (i = 0; i < tmp.span_count; i++) {
            GViewSpan *s = &tmp.spans[i];
            s->self_us   = s->dur_us;
            if (s->parent != GV_SPAN_NONE && s->parent < tmp.span_count) {
                tmp.spans[s->parent].child_count++;
            }
        }
        for (i = 0; i < tmp.span_count; i++) {
            GViewSpan *s = &tmp.spans[i];
            if (s->parent != GV_SPAN_NONE && s->parent < tmp.span_count) {
                GViewSpan *parent = &tmp.spans[s->parent];
                if (s->dur_us >= parent->self_us) {
                    parent->self_us = 0;
                } else {
                    parent->self_us -= s->dur_us;
                }
            }
        }

        tmp.loaded = 1;
        *trace     = tmp;
    } else {
        gview_trace_free(&tmp);
    }

    for (i = 0; i < p.thread_count; i++) {
        free(p.threads[i].stack);
        free(p.threads[i].span_idx);
    }
    free(p.threads);
    free(p.slots);

    if (!ok) {
        if (err && err_cap) snprintf(err, err_cap, "could not parse %s", path);
        return 0;
    }

    return 1;
}

void gview_flame_init(GViewFlame *flame)
{
    if (flame) {
        memset(flame, 0, sizeof(*flame));
    }
}

void gview_flame_free(GViewFlame *flame)
{
    if (!flame) {
        return;
    }
    free(flame->nodes);
    memset(flame, 0, sizeof(*flame));
}

static uint32_t gv__flame_add(GViewFlame *flame, uint32_t name_id, uint32_t parent,
                              uint32_t depth)
{
    GViewFlameNode *n;

    if (!gv__reserve((void **)&flame->nodes, &flame->cap, flame->count + 1u,
                     sizeof(GViewFlameNode))) {
        return GV_SPAN_NONE;
    }

    n = &flame->nodes[flame->count];
    memset(n, 0, sizeof(*n));
    n->name_id      = name_id;
    n->parent       = parent;
    n->first_child  = GV_SPAN_NONE;
    n->next_sibling = GV_SPAN_NONE;
    n->depth        = depth;
    n->span_id      = GV_SPAN_NONE;

    flame->count++;
    return flame->count - 1u;
}

int gview_flame_build(GViewFlame *flame, GViewTrace *trace)
{
    uint32_t i;

    gview_flame_free(flame);
    gview_flame_init(flame);

    if (!trace || !trace->loaded) {
        return 0;
    }

    if (gv__flame_add(flame, GV_NAME_NONE, GV_SPAN_NONE, 0) == GV_SPAN_NONE) {
        gview_flame_free(flame);
        return 0;
    }

    for (i = 0; i < trace->span_count; i++) {
        trace->spans[i].flame_id = GV_SPAN_NONE;
    }

    for (i = 0; i < trace->span_count; i++) {
        GViewSpan *s          = &trace->spans[i];
        uint32_t   parent     = 0;
        uint32_t   child;
        uint32_t   found      = GV_SPAN_NONE;

        if (s->parent != GV_SPAN_NONE && s->parent < trace->span_count) {
            uint32_t pf = trace->spans[s->parent].flame_id;
            if (pf != GV_SPAN_NONE) parent = pf;
        }

        child = flame->nodes[parent].first_child;
        while (child != GV_SPAN_NONE) {
            if (flame->nodes[child].name_id == s->name_id) {
                found = child;
                break;
            }
            child = flame->nodes[child].next_sibling;
        }

        if (found == GV_SPAN_NONE) {
            found = gv__flame_add(flame, s->name_id, parent,
                                  flame->nodes[parent].depth + 1u);
            if (found == GV_SPAN_NONE) {
                gview_flame_free(flame);
                return 0;
            }
            flame->nodes[found].next_sibling = flame->nodes[parent].first_child;
            flame->nodes[parent].first_child = found;
        }

        s->flame_id = found;
        flame->nodes[found].own_us += s->self_us;
        if (flame->nodes[found].span_id == GV_SPAN_NONE) {
            flame->nodes[found].span_id = i;
        }
    }

    for (i = flame->count; i > 1u; i--) {
        GViewFlameNode *n = &flame->nodes[i - 1u];
        n->total_us += n->own_us;
        if (n->parent < flame->count) {
            flame->nodes[n->parent].total_us += n->total_us;
        }
    }

    flame->root_total = flame->count ? flame->nodes[0].total_us : 0;
    return 1;
}

typedef struct {
    uint32_t id;
    uint64_t total;
    uint64_t self;
    uint32_t hits;
} GVNameStat;

static int gv__stat_cmp(const void *a, const void *b)
{
    const GVNameStat *x = (const GVNameStat *)a;
    const GVNameStat *y = (const GVNameStat *)b;
    if (x->total < y->total) return 1;
    if (x->total > y->total) return -1;
    return 0;
}

void gview_trace_print_summary(const GViewTrace *trace, const char *path)
{
    GVNameStat *stats;
    uint32_t    i;
    uint32_t    shown;

    if (!trace || !trace->loaded) {
        printf("gview: no trace loaded\n");
        return;
    }

    printf("trace:   %s\n", path ? path : "-");
    printf("spans:   %u\n", trace->span_count);
    printf("threads: %u\n", trace->thread_count);
    printf("names:   %u\n", trace->name_count);
    printf("time:    %.3f ms\n", (double)(trace->t1 - trace->t0) / 1000.0);
    printf("warnings: bad=%u unmatched_end=%u open_at_end=%u\n", trace->bad_events,
           trace->unmatched_ends, trace->open_at_end);
    printf("complete events: %u\n", trace->x_events);

    stats = (GVNameStat *)calloc(trace->name_count ? trace->name_count : 1u,
                                 sizeof(GVNameStat));
    if (!stats) {
        return;
    }

    for (i = 0; i < trace->span_count; i++) {
        const GViewSpan *s = &trace->spans[i];
        if (s->name_id < trace->name_count) {
            stats[s->name_id].id = s->name_id;
            stats[s->name_id].total += s->dur_us;
            stats[s->name_id].self += s->self_us;
            stats[s->name_id].hits++;
        }
    }

    qsort(stats, trace->name_count, sizeof(GVNameStat), gv__stat_cmp);

    printf("\ntop zones by inclusive time:\n");
    shown = trace->name_count < 10u ? trace->name_count : 10u;
    for (i = 0; i < shown; i++) {
        if (stats[i].hits == 0) continue;
        printf("  %-32s %10.3f ms  self %10.3f ms  hits %u\n",
               gview_name(trace, stats[i].id), (double)stats[i].total / 1000.0,
               (double)stats[i].self / 1000.0, stats[i].hits);
    }

    free(stats);
}

#endif // GVIEW_IMPLEMENTATION
