/* Spans as OTLP protobuf, written from C. A header-only port of the span half of the engine's
 * open_telemetry module (see docs/otel.md): spans are recorded after the fact into a fixed ring, so a
 * hot loop pays for a struct fill, and otel_flush appends the ring as one length-prefixed ScopeSpans
 * record. There is one format and one path; a caller that wants no output opens /dev/null. Nothing here
 * allocates or opens a socket, and only otel_flush writes.
 *
 * Attributes are typed because a collector treats the types differently: a number (otel_num, a double) is
 * something to average and graph, a string (otel_str) is a category to group by, and an integer (otel_int)
 * is read as a category too, so it is for identifiers only. A failed span says so with otel_error, which
 * sets the status and adds the exception event collectors list as an error.
 *
 * The optional parts of the protocol are written every time, so no reader depends on a default: the span's
 * trace state, kind, flags, status (OK unless an error) and its three dropped counts, the event's dropped
 * count, and the scope's version, dropped count and schema URL. Links are the one repeated field left out,
 * because an empty list has no encoding. */
#ifndef OTEL_SPAN_H
#define OTEL_SPAN_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define OTEL_CAP 4096
#define OTEL_ATTRS 8
#define OTEL_FLUSH_AT 256 /* records; the caller flushes in idle time when the ring holds this many */
#define OTEL_STR_MAX 120 /* a longer key or string is cut here */

typedef struct { const char *key, *s; double d; int64_t i; uint8_t kind; } otel_attr_t; /* kind: 1 int, 2 double, 3 string */

typedef struct {
  uint64_t t0, t1, sid, psid, tid_hi, tid_lo;
  const char *name, *exc_type, *exc_msg; /* exc_msg set: status ERROR plus one exception event at t1 */
  uint32_t nattr;
  uint8_t kind; /* 1 internal, 2 server, 3 client, 4 producer, 5 consumer */
  otel_attr_t a[OTEL_ATTRS];
} otel_rec_t;

typedef struct {
  uint64_t tid_hi, tid_lo, rng;
  int64_t wall_off; /* wall minus monotonic, in nanoseconds, fixed at open */
  uint32_t n, dropped;
  FILE *out;
  otel_rec_t scratch; /* where a span goes when the ring is full, so callers never test */
  otel_rec_t r[OTEL_CAP];
} otel_t;

static inline uint64_t otel_next(otel_t *o) { /* xorshift64, logical shifts */
  o->rng ^= o->rng << 13; o->rng ^= o->rng >> 7; o->rng ^= o->rng << 17;
  return o->rng;
}

static inline uint64_t otel_id(otel_t *o) { uint64_t v = otel_next(o); return v ? v : 1; }

/* Starts a new trace: every span recorded after this call belongs to it. */
static inline void otel_trace(otel_t *o) { o->tid_hi = otel_id(o); o->tid_lo = otel_id(o); }

/* Opens path for appending. */
static inline int otel_open(otel_t *o, const char *path, uint64_t seed) {
  struct timespec w, m;
  memset(o, 0, offsetof(otel_t, r));
  clock_gettime(CLOCK_REALTIME, &w); clock_gettime(CLOCK_MONOTONIC, &m);
  o->wall_off = ((int64_t)w.tv_sec - (int64_t)m.tv_sec) * 1000000000LL + ((int64_t)w.tv_nsec - (int64_t)m.tv_nsec);
  o->rng = seed ? seed : (uint64_t)w.tv_nsec * 2654435761u + 88172645463325252ull;
  otel_trace(o);
  o->out = fopen(path, "a");
  return o->out ? 0 : -1;
}

static inline uint64_t otel_wall(const otel_t *o, int64_t mono_ns) { return (uint64_t)(mono_ns + o->wall_off); }

/* Records a finished span, between two monotonic times, and returns it for its attributes. */
static inline otel_rec_t *otel_begin(otel_t *o, uint64_t sid, const char *name, uint64_t parent, int64_t mono0, int64_t mono1) {
  otel_rec_t *r;
  if (o->n >= OTEL_CAP) { o->dropped++; r = &o->scratch; } else r = &o->r[o->n++];
  memset(r, 0, sizeof *r);
  r->t0 = otel_wall(o, mono0); r->t1 = otel_wall(o, mono1); r->sid = sid; r->psid = parent; r->name = name;
  r->tid_hi = o->tid_hi; r->tid_lo = o->tid_lo; r->kind = 1;
  return r;
}

static inline void otel_put(otel_rec_t *r, const char *key, uint8_t kind, double d, int64_t i, const char *s) {
  if (r->nattr >= OTEL_ATTRS) return;
  otel_attr_t *a = &r->a[r->nattr++];
  a->key = key; a->kind = kind; a->d = d; a->i = i; a->s = s;
}
static inline void otel_kind(otel_rec_t *r, uint8_t kind) { r->kind = kind; }
static inline void otel_num(otel_rec_t *r, const char *key, double v) { otel_put(r, key, 2, v, 0, 0); }
static inline void otel_int(otel_rec_t *r, const char *key, int64_t v) { otel_put(r, key, 1, 0, v, 0); }
static inline void otel_str(otel_rec_t *r, const char *key, const char *v) { otel_put(r, key, 3, 0, 0, v); }
/* type and message must outlive the flush: use literals. */
static inline void otel_error(otel_rec_t *r, const char *type, const char *msg) { r->exc_type = type; r->exc_msg = msg; }

/* Protobuf (OTLP/HTTP's own encoding). A record is a 4-byte little-endian length and one ScopeSpans
 * message. The resource, which may hold a key, is not written: the poster wraps each record in a
 * ResourceSpans with the resource it reads from its own environment. */
static inline uint32_t otel_pb_varint(uint8_t *b, uint64_t v) {
  uint32_t n = 0;
  while (v >= 0x80) { if (b) b[n] = (uint8_t)(v | 0x80); n++; v >>= 7; }
  if (b) b[n] = (uint8_t)v;
  return n + 1;
}
static inline uint32_t otel_pb_bytes(uint8_t *b, uint8_t tag, const void *p, uint32_t len) {
  uint32_t n = 1 + otel_pb_varint(b ? b + 1 : 0, len);
  if (b) { b[0] = tag; memcpy(b + n, p, len); }
  return n + len;
}
static inline uint32_t otel_pb_text(uint8_t *b, uint8_t tag, const char *s) {
  size_t l = strlen(s);
  return otel_pb_bytes(b, tag, s, (uint32_t)(l > OTEL_STR_MAX ? OTEL_STR_MAX : l));
}
static inline void otel_pb_be(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (56 - 8 * i)); }
static inline void otel_pb_le(uint8_t *b, uint64_t v) { for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i)); }

/* A KeyValue with a string, an int or a double: the AnyValue fields 1, 3 and 4. */
static inline uint32_t otel_pb_kv(uint8_t *b, uint8_t tag, const char *key, uint8_t kind, double d, int64_t i, const char *s) {
  uint8_t kv[OTEL_STR_MAX * 2 + 32], any[OTEL_STR_MAX + 16]; uint32_t m = otel_pb_text(kv, 0x0A, key), a = 0;
  if (kind == 3) a = otel_pb_text(any, 0x0A, s);
  else if (kind == 1) { any[0] = 0x18; a = 1 + otel_pb_varint(any + 1, (uint64_t)i); }
  else { uint64_t bits; memcpy(&bits, &d, 8); any[0] = 0x21; otel_pb_le(any + 1, bits); a = 9; }
  m += otel_pb_bytes(kv + m, 0x12, any, a);
  return otel_pb_bytes(b, tag, kv, m);
}

#define OTEL_FLAGS 0x101u /* W3C trace flags: sampled, and the has-is-remote bit with is-remote clear */
#define OTEL_SCHEMA "https://opentelemetry.io/schemas/1.27.0"

/* Encodes one Span into b (or only measures when b is null), fields in number order. */
static inline uint32_t otel_pb_span(const otel_rec_t *r, uint8_t *b) {
  uint8_t id[16]; uint32_t n = 0;
  otel_pb_be(id, r->tid_hi); otel_pb_be(id + 8, r->tid_lo);
  n += otel_pb_bytes(b ? b + n : 0, 0x0A, id, 16);
  otel_pb_be(id, r->sid); n += otel_pb_bytes(b ? b + n : 0, 0x12, id, 8);
  n += otel_pb_bytes(b ? b + n : 0, 0x1A, "", 0); /* trace_state, empty and written */
#ifndef OTEL_PLANT_PBPARENT
  if (r->psid) { otel_pb_be(id, r->psid); n += otel_pb_bytes(b ? b + n : 0, 0x22, id, 8); }
#endif
  n += otel_pb_text(b ? b + n : 0, 0x2A, r->name);
  if (b) b[n] = 0x30;
  n++; n += otel_pb_varint(b ? b + n : 0, r->kind);
  if (b) { b[n] = 0x39; otel_pb_le(b + n + 1, r->t0); b[n + 9] = 0x41;
#ifdef OTEL_PLANT_PBTIMES
    otel_pb_le(b + n + 10, r->t0); }
#else
    otel_pb_le(b + n + 10, r->t1); }
#endif
  n += 18;
  for (uint32_t k = 0; k < r->nattr; k++) { const otel_attr_t *a = &r->a[k]; n += otel_pb_kv(b ? b + n : 0, 0x4A, a->key, a->kind, a->d, a->i, a->s); }
  if (b) { b[n] = 0x50; b[n + 1] = 0; } n += 2; /* dropped_attributes_count */
  if (r->exc_msg) {
    uint8_t ev[2 * OTEL_STR_MAX * 2 + 96]; uint32_t e;
    ev[0] = 0x09; otel_pb_le(ev + 1, r->t1); e = 9; /* Event: time 1, name 2, attributes 3, dropped_attributes_count 4 */
    e += otel_pb_text(ev + e, 0x12, "exception");
    e += otel_pb_kv(ev + e, 0x1A, "exception.type", 3, 0, 0, r->exc_type);
    e += otel_pb_kv(ev + e, 0x1A, "exception.message", 3, 0, 0, r->exc_msg);
    ev[e++] = 0x20; ev[e++] = 0;
    n += otel_pb_bytes(b ? b + n : 0, 0x5A, ev, e);
  }
  if (b) { b[n] = 0x60; b[n + 1] = 0; b[n + 2] = 0x70; b[n + 3] = 0; } n += 4; /* dropped_events_count, dropped_links_count */
#ifndef OTEL_PLANT_PBSTATUS
  { uint8_t st[OTEL_STR_MAX + 16]; uint32_t s = 0; /* Status: message 2, code 3 */
    if (r->exc_msg) s += otel_pb_text(st, 0x12, r->exc_msg);
    st[s++] = 0x18; st[s++] = r->exc_msg ? 2 : 1;
    n += otel_pb_bytes(b ? b + n : 0, 0x7A, st, s); }
#endif
  if (b) { b[n] = 0x85; b[n + 1] = 0x01; b[n + 2] = (uint8_t)OTEL_FLAGS; b[n + 3] = (uint8_t)(OTEL_FLAGS >> 8); b[n + 4] = 0; b[n + 5] = 0; } n += 6; /* flags, fixed32 */
  return n;
}

static inline void otel_flush(otel_t *o) {
  static const char scope[] = "entity-server";
  uint8_t head[32], tmp[4096], sc[64], tail[64];
  if (!o->out || o->n == 0) return;
  uint32_t sclen = otel_pb_bytes(sc, 0x0A, scope, (uint32_t)strlen(scope)); /* InstrumentationScope: name 1, version 2, dropped_attributes_count 4 */
  sclen += otel_pb_bytes(sc + sclen, 0x12, "1", 1);
  sc[sclen++] = 0x20; sc[sclen++] = 0;
  uint32_t tl = otel_pb_bytes(tail, 0x1A, OTEL_SCHEMA, (uint32_t)strlen(OTEL_SCHEMA)); /* ScopeSpans.schema_url */
  uint32_t body = 1 + otel_pb_varint(0, sclen) + sclen + tl;
  for (uint32_t i = 0; i < o->n; i++) { uint32_t m = otel_pb_span(&o->r[i], 0); body += 1 + otel_pb_varint(0, m) + m; }
  head[0] = (uint8_t)body; head[1] = (uint8_t)(body >> 8); head[2] = (uint8_t)(body >> 16); head[3] = (uint8_t)(body >> 24);
  fwrite(head, 1, 4, o->out);
  { uint32_t h = 0; head[h++] = 0x0A; h += otel_pb_varint(head + h, sclen); fwrite(head, 1, h, o->out); fwrite(sc, 1, sclen, o->out); }
  for (uint32_t i = 0; i < o->n; i++) {
    uint32_t m = otel_pb_span(&o->r[i], tmp), h = 0;
    head[h++] = 0x12; h += otel_pb_varint(head + h, m);
    fwrite(head, 1, h, o->out); fwrite(tmp, 1, m, o->out);
  }
  fwrite(tail, 1, tl, o->out);
  fflush(o->out);
  o->n = 0;
}

static inline void otel_close(otel_t *o) { otel_flush(o); if (o->out) fclose(o->out); o->out = 0; }
#endif
