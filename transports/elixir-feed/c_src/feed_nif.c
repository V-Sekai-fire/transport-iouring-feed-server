/* ============================================================================================
 *  feed_nif.c -- the SQLite and JSON side of the Elixir feed server, as one NIF.
 *
 *  SPACE SHUTTLE STYLE.
 *  PLEASE DO NOT ATTEMPT TO SIMPLIFY THIS CODE. KEEP THE SPACE SHUTTLE FLYING.
 *
 *  Every `if` has an explicit `else`, and every branch says why it is (or is not) a fault, even
 *  when the else does nothing. Spelling out each case is the point: a reader can check that no
 *  case was forgotten without reconstructing the control flow in their head. Collapsing branches
 *  "because they look the same" is how a case gets lost.
 *
 *  The file follows the JPL "Power of Ten" rules for safety-critical C (Holzmann, 2006):
 *    1. Simple control flow: no goto, no setjmp/longjmp, no recursion. Each function has one exit.
 *    2. Every loop has a fixed upper bound, stated next to it. There is no thread and no sleep: the
 *       checkpoint is two dirty NIFs driven by an Elixir state machine (lib/feed/checkpoint.ex).
 *    3. No dynamic allocation after initialisation. Response and scratch buffers are static and
 *       sized for the worst case the spec allows; an overflow is answered with a 500.
 *    4. No function is longer than 60 lines.
 *    5. Checks of the "cannot happen" kind are written as ordinary branches that recover (answer
 *       500, return an error tuple). Nothing calls abort(): aborting here would kill the BEAM.
 *    6. Data has the smallest scope: one `struct feed_state`, file-static, named g_feed.
 *    7. Every non-void return value is checked, or cast to (void) with the reason beside it.
 *    8. The preprocessor is used for includes, constants and one string-literal helper only.
 *    9. Pointers: at most one dereference level in expressions; function pointers only where the
 *       APIs demand them (deviation D1).
 *   10. Built with -Wall -Wextra -Wpedantic -Werror; zero warnings.
 *
 *  Documented deviations:
 *    D1. Function pointers: the ErlNifFunc table, and the WAL hook
 *        are required by their APIs.
 *    D2. Allocation after init: enif_make_new_binary (the VM's allocator, owned by the VM) and
 *        SQLite's own page cache and statement memory (bounded by cache_size), and one
 *        enif_alloc_env per checkpoint cycle (a few per second at most) to send `ck_due`. This file
 *        calls no malloc/realloc/free itself.
 *    D3. The SQLite amalgamation is compiled as its own translation unit with the reference's
 *        flags and -w; it is third-party code, unmodified, and is not held to these rules.
 *
 *  Roles. Elixir owns sockets, HTTP parsing, routing and the JWT signature. This file owns the
 *  database (pinned SQLite 3.54.1) and writes every database-backed response as one finished
 *  HTTP/1.1 binary, so a request costs one NIF call and one send.
 *
 *  Contract with Elixir: response NIFs always return a binary (a 500 on any fault, never a
 *  raise); open/1 returns :ok | {:error, binary}; claims/2 returns :invalid | {:ok, exp, sub, user}.
 *
 *  Durability matches datasources/reference: WAL, synchronous=NORMAL, the unix-excl VFS (WAL index
 *  in heap memory), automatic checkpoints off, a low-priority checkpoint thread on a second
 *  connection. Each write is its own autocommit transaction, so a 201 is built only after its
 *  COMMIT returned. SQLite is built SQLITE_THREADSAFE=2: the main connection is used only by the
 *  scheduler thread holding g_feed.mu, the checkpoint connection only by its own thread.
 * ============================================================================================ */
#include "sqlite3.h"
#include <erl_nif.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* ---- limits (every bound in this file is one of these) ------------------------------------ */
#define FEED_BODY_CAP      (512u * 1024u) /* response body: 20 posts x 2000 B x 6 (escaping) < 256 KB */
#define FEED_TEXT_CAP      (64u * 1024u)  /* extracted request body; spec leaves > 16 KB out of scope   */
#define FEED_PATH_CAP      4096u
#define FEED_ERR_CAP       512u
#define FEED_HDR_CAP       128u
#define FEED_FEED_ROWS     20u
#define FEED_MAX_CHARS     500u
#define FEED_CREATED_CAP   40u
#define FEED_DRAIN_MAX     4u             /* INSERT ... RETURNING yields one row; then DONE           */
#ifndef FEED_CKPT_FRAMES
#define FEED_CKPT_FRAMES   4096           /* overridable (-D) so a test build checkpoints after a few writes */
#endif
#define FEED_INT_DIGITS    24u
#define FEED_ESC_WORST     6u             /* one byte escapes to at most \u00XX                         */

#define FEED_PUTS(b, lit) feed_put((b), (lit), sizeof(lit) - 1u)

enum feed_stmt {
  ST_FEED = 0, ST_POST, ST_EXISTS, ST_LIKE, ST_INSERT, ST_BODY, ST_CLAIMS, ST_HEALTH, ST_COUNT
};

/* Post columns: id, body, created_at, username, like_count. */
#define POST_COLS "SELECT p.id, p.body, p.created_at, u.username, " \
  "(SELECT count(*) FROM likes l WHERE l.post_id = p.id) FROM posts p JOIN users u ON u.id = p.user_id "

static const char *const feed_sql[ST_COUNT] = {
  [ST_FEED] = POST_COLS "ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
  [ST_POST] = POST_COLS "WHERE p.id = ?1",
  [ST_EXISTS] = "SELECT 1 FROM posts WHERE id = ?1",
  [ST_LIKE] = "INSERT INTO likes (user_id, post_id) VALUES (?1, ?2) ON CONFLICT (user_id, post_id) DO NOTHING",
  [ST_INSERT] = "INSERT INTO posts (user_id, body) VALUES (?1, ?2) RETURNING id, created_at",
  /* CASE is lazy, so json_type/json_extract never see text that json_valid rejected. */
  [ST_BODY] = "SELECT CASE WHEN json_valid(?1) THEN json_type(?1) ELSE 'bad' END,"
    " CASE WHEN json_valid(?1) THEN (CASE WHEN json_type(?1) = 'object' AND json_type(?1, '$.body') = 'text'"
    "   THEN json_extract(?1, '$.body') END) END",
  [ST_CLAIMS] = "SELECT"
    " CASE WHEN json_valid(?1) THEN (CASE WHEN json_type(?1) = 'object' AND json_type(?1, '$.alg') = 'text'"
    "   AND json_extract(?1, '$.alg') = 'HS256' THEN 1 ELSE 0 END) ELSE 0 END,"
    " CASE WHEN json_valid(?2) THEN (CASE WHEN json_type(?2) = 'object' THEN 1 ELSE 0 END) ELSE 0 END,"
    " CASE WHEN json_valid(?2) THEN (CASE WHEN json_type(?2) = 'object' AND json_type(?2, '$.exp') IN ('integer','real')"
    "   THEN json_extract(?2, '$.exp') END) END,"
    " CASE WHEN json_valid(?2) THEN (CASE WHEN json_type(?2) = 'object' AND json_type(?2, '$.sub') = 'text'"
    "   THEN json_extract(?2, '$.sub') END) END,"
    " CASE WHEN json_valid(?2) THEN (CASE WHEN json_type(?2) = 'object' AND json_type(?2, '$.username') = 'text'"
    "   THEN json_extract(?2, '$.username') END) END",
  [ST_HEALTH] = "SELECT 1",
};

static const char *const feed_pragmas[] = {
  "PRAGMA journal_mode = WAL", "PRAGMA synchronous = NORMAL", "PRAGMA temp_store = MEMORY",
  "PRAGMA cache_size = -65536", "PRAGMA mmap_size = 0", "PRAGMA wal_autocheckpoint = 0",
  "SELECT count(*) FROM sqlite_schema",
};
#define FEED_PRAGMA_COUNT (sizeof feed_pragmas / sizeof feed_pragmas[0])

struct feed_buf {
  unsigned char *p;
  size_t n;
  size_t cap;
  int overflow; /* sticky: once set, the buffer accepts nothing and the caller answers 500 */
};

struct feed_state {
  sqlite3 *db;                 /* request connection; only under mu */
  sqlite3 *ckdb;               /* checkpoint connection; only inside the two checkpoint NIFs, which never overlap */
  sqlite3_stmt *st[ST_COUNT];
  pthread_mutex_t mu;
  int wal_frames;              /* atomic: frames in the WAL after the last commit */
  int hold;                    /* atomic: 1 while the tail checkpoint holds writes; writes answer :busy */
  int armed;                   /* atomic: 1 from the `due` message until the tail checkpoint ends */
  int ck_registered;           /* under mu: ck_pid is valid */
  ErlNifPid ck_pid;            /* under mu: the checkpoint state machine */
  int opened;                  /* under mu */
  char path[FEED_PATH_CAP];    /* the path opened, so a second open/1 can be checked against it */
  unsigned long lock_faults;   /* unlock failures observed; never expected */
  struct feed_buf body;        /* response body scratch; only under mu */
  struct feed_buf text;        /* request body scratch; only under mu */
};

static unsigned char g_body_mem[FEED_BODY_CAP];
static unsigned char g_text_mem[FEED_TEXT_CAP];
static struct feed_state g_feed = {
  .mu = PTHREAD_MUTEX_INITIALIZER,
  .body = { g_body_mem, 0u, FEED_BODY_CAP, 0 },
  .text = { g_text_mem, 0u, FEED_TEXT_CAP, 0 },
};


/* The write NIFs call these after a write. feed_due_locked (under mu): 1 once the WAL has reached the
 * threshold and no cycle is running, and arms the cycle, so exactly one `due` message goes out per cycle.
 * feed_notify (after unlock): sends it. A dead state machine leaves the cycle armed until the next
 * registration, which resets it; writes are never blocked by that. */
static int feed_due_locked(void) {
  int due = 0;
  if (g_feed.ck_registered != 0 && __atomic_load_n(&g_feed.wal_frames, __ATOMIC_RELAXED) >= FEED_CKPT_FRAMES) {
    int expect = 0;
    due = __atomic_compare_exchange_n(&g_feed.armed, &expect, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) ? 1 : 0;
  } else {
    /* Below the threshold, or nobody registered. */
  }
  return due;
}

static void feed_notify(int due, const ErlNifPid *pid) {
  if (due != 0) {
    ErlNifEnv *e = enif_alloc_env();
    if (e == NULL) {
      __atomic_store_n(&g_feed.armed, 0, __ATOMIC_RELEASE); /* no memory: the next write tries again */
    } else {
      if (enif_send(NULL, pid, e, enif_make_atom(e, "ck_due")) == 0) {
        __atomic_store_n(&g_feed.armed, 0, __ATOMIC_RELEASE); /* the process is gone: re-arm for its successor */
      } else {
        /* Sent: the state machine clears `armed` when its cycle ends. */
      }
      enif_free_env(e);
    }
  } else {
    /* Nothing due. */
  }
}

/* ==== buffers ================================================================================ */

static void feed_buf_clear(struct feed_buf *b) {
  b->n = 0u;
  b->overflow = 0;
}

static void feed_put(struct feed_buf *b, const void *s, size_t k) {
  if (b->overflow != 0) {
    /* Already full: nothing more is written; the caller sees overflow and answers 500. */
  } else if (k == 0u) {
    /* Nothing to copy (and s may legitimately be NULL for an empty SQLite text). */
  } else if (s == NULL) {
    /* A non-empty copy from NULL cannot be honoured: treat as a fault. */
    b->overflow = 1;
  } else if (k > b->cap - b->n) {
    /* Would not fit: refuse rather than truncate silently. */
    b->overflow = 1;
  } else {
    (void)memcpy(b->p + b->n, s, k); /* memcpy returns its first argument; nothing to check */
    b->n += k;
  }
}

static void feed_put_int(struct feed_buf *b, int64_t v) {
  char t[FEED_INT_DIGITS];
  unsigned i = FEED_INT_DIGITS;
  int neg = (v < 0) ? 1 : 0;
  uint64_t u = (neg != 0) ? (0u - (uint64_t)v) : (uint64_t)v;
  unsigned guard = 0u;
  do { /* bound: a uint64 has at most 20 decimal digits; guard stops at 20 regardless */
    i--;
    t[i] = (char)('0' + (int)(u % 10u));
    u /= 10u;
    guard++;
  } while (u != 0u && guard < 20u);
  if (neg != 0) {
    i--;
    t[i] = '-';
  } else {
    /* Non-negative: no sign. */
  }
  feed_put(b, t + i, FEED_INT_DIGITS - i);
}

static int feed_needs_escape(unsigned char c) {
  return (c < 0x20u || c == (unsigned char)'"' || c == (unsigned char)'\\') ? 1 : 0;
}

static unsigned char feed_short_escape(unsigned char c) {
  unsigned char r;
  switch (c) {
    case '"': r = (unsigned char)'"'; break;
    case '\\': r = (unsigned char)'\\'; break;
    case '\n': r = (unsigned char)'n'; break;
    case '\r': r = (unsigned char)'r'; break;
    case '\t': r = (unsigned char)'t'; break;
    default: r = 0u; break; /* no short form: the caller writes \u00XX */
  }
  return r;
}

/* JSON string escape. Room for the worst case is checked once up front, so the loop writes
 * without per-byte checks. Bound: n iterations, n <= the buffer's capacity. */
static void feed_put_esc(struct feed_buf *b, const unsigned char *s, size_t n) {
  static const char hx[] = "0123456789abcdef";
  if (b->overflow != 0) {
    /* Already failed: write nothing. */
  } else if (n == 0u) {
    /* Empty string: nothing to write. */
  } else if (s == NULL || n > (b->cap - b->n) / FEED_ESC_WORST) {
    b->overflow = 1; /* no room for the worst case: refuse */
  } else {
    unsigned char *o = b->p + b->n;
    for (size_t i = 0u; i < n; i++) {
      unsigned char c = s[i];
      if (feed_needs_escape(c) == 0) {
        *o++ = c; /* the common case: copied as-is */
      } else if (feed_short_escape(c) != 0u) {
        *o++ = (unsigned char)'\\';
        *o++ = feed_short_escape(c);
      } else {
        *o++ = (unsigned char)'\\'; *o++ = (unsigned char)'u'; *o++ = (unsigned char)'0';
        *o++ = (unsigned char)'0'; *o++ = (unsigned char)hx[c >> 4]; *o++ = (unsigned char)hx[c & 15u];
      }
    }
    b->n = (size_t)(o - b->p);
  }
}

/* ==== responses ============================================================================== */

static const char *feed_status_line(int s) {
  const char *r;
  switch (s) {
    case 200: r = "HTTP/1.1 200 OK\r\n"; break;
    case 201: r = "HTTP/1.1 201 Created\r\n"; break;
    case 400: r = "HTTP/1.1 400 Bad Request\r\n"; break;
    case 401: r = "HTTP/1.1 401 Unauthorized\r\n"; break;
    case 404: r = "HTTP/1.1 404 Not Found\r\n"; break;
    case 503: r = "HTTP/1.1 503 Service Unavailable\r\n"; break;
    default: r = "HTTP/1.1 500 Internal Server Error\r\n"; break; /* any other code is a 500 */
  }
  return r;
}

/* One finished response. If the header cannot be formatted or the VM gives no binary, the
 * answer is the atom `error`, which Elixir maps to a canned 500 (it never raises). */
static ERL_NIF_TERM feed_respond(ErlNifEnv *env, int status, const unsigned char *body, size_t n) {
  char h[FEED_HDR_CAP];
  ERL_NIF_TERM t;
  int hn = snprintf(h, sizeof h, "%sContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n",
                    feed_status_line(status), n);
  if (hn <= 0 || (size_t)hn >= sizeof h) {
    t = enif_make_atom(env, "error"); /* header did not fit: cannot happen with these constants */
  } else if (n > 0u && body == NULL) {
    t = enif_make_atom(env, "error"); /* a body length without a body is a caller fault */
  } else {
    unsigned char *o = enif_make_new_binary(env, (size_t)hn + n, &t);
    if (o == NULL) {
      t = enif_make_atom(env, "error"); /* VM refused the allocation */
    } else {
      (void)memcpy(o, h, (size_t)hn);
      if (n > 0u) {
        (void)memcpy(o + hn, body, n);
      } else {
        /* Empty body: header only. */
      }
    }
  }
  return t;
}

static ERL_NIF_TERM feed_error(ErlNifEnv *env, int status, const char *msg) {
  char b[96];
  ERL_NIF_TERM t;
  int n = snprintf(b, sizeof b, "{\"error\":\"%s\"}", msg);
  if (n <= 0 || (size_t)n >= sizeof b) {
    t = enif_make_atom(env, "error"); /* message too long: all callers pass short literals */
  } else {
    t = feed_respond(env, status, (const unsigned char *)b, (size_t)n);
  }
  return t;
}

static ERL_NIF_TERM feed_ise(ErlNifEnv *env) { return feed_error(env, 500, "internal server error"); }

static ERL_NIF_TERM feed_respond_buf(ErlNifEnv *env, int status, const struct feed_buf *b) {
  ERL_NIF_TERM t;
  if (b->overflow != 0) {
    t = feed_ise(env); /* the body did not fit (or a column was NULL): never send a cut body */
  } else {
    t = feed_respond(env, status, b->p, b->n);
  }
  return t;
}

/* ==== locking and statements ================================================================= */

static int feed_lock(void) {
  int ok;
  if (pthread_mutex_lock(&g_feed.mu) != 0) {
    ok = 0; /* only on a corrupt mutex; the caller answers 500 without touching the database */
  } else if (g_feed.opened == 0) {
    ok = 0; /* not open yet (open/1 failed or was never called): answer 500 */
    if (pthread_mutex_unlock(&g_feed.mu) != 0) {
      g_feed.lock_faults++;
    } else {
      /* Released. */
    }
  } else {
    ok = 1;
  }
  return ok;
}

static void feed_unlock(void) {
  if (pthread_mutex_unlock(&g_feed.mu) != 0) {
    g_feed.lock_faults++; /* we hold it, so this means corruption; nothing safer to do than count */
  } else {
    /* Released. */
  }
}

/* Reset and unbind, so no SQLITE_STATIC binding outlives the Erlang binary it points into.
 * Returns the reset code (which repeats the last step's error, if any). */
static int feed_finish(sqlite3_stmt *s) {
  int rc = sqlite3_reset(s);
  if (sqlite3_clear_bindings(s) != SQLITE_OK) {
    rc = SQLITE_ERROR; /* clear_bindings only fails on a misuse; report it as a fault */
  } else {
    /* Bindings cleared. */
  }
  return rc;
}

/* ==== rows =================================================================================== */

static void feed_put_text_col(struct feed_buf *b, sqlite3_stmt *s, int i) {
  const unsigned char *t = sqlite3_column_text(s, i); /* text first, then bytes (SQLite's rule) */
  int n = sqlite3_column_bytes(s, i);
  if (t == NULL || n < 0) {
    b->overflow = 1; /* NOT NULL columns came back NULL (or OOM): answer 500 */
  } else {
    feed_put_esc(b, t, (size_t)n);
  }
}

static void feed_put_post(struct feed_buf *b, sqlite3_stmt *s) {
  FEED_PUTS(b, "{\"id\":");
  feed_put_int(b, (int64_t)sqlite3_column_int64(s, 0));
  FEED_PUTS(b, ",\"body\":\"");
  feed_put_text_col(b, s, 1);
  FEED_PUTS(b, "\",\"created_at\":\"");
  feed_put_text_col(b, s, 2);
  FEED_PUTS(b, "\",\"author\":\"");
  feed_put_text_col(b, s, 3);
  FEED_PUTS(b, "\",\"like_count\":");
  feed_put_int(b, (int64_t)sqlite3_column_int64(s, 4));
  FEED_PUTS(b, "}");
}

/* ==== GET /feed ============================================================================== */

static ERL_NIF_TERM feed_feed_locked(ErlNifEnv *env) {
  sqlite3_stmt *s = g_feed.st[ST_FEED];
  struct feed_buf *b = &g_feed.body;
  int rc = SQLITE_ROW;
  unsigned rows = 0u;
  ERL_NIF_TERM r;
  feed_buf_clear(b);
  FEED_PUTS(b, "{\"posts\":[");
  /* Bound: LIMIT 20 gives at most 20 rows then DONE, so at most 21 steps; a 21st row is a fault. */
  while (rc == SQLITE_ROW && rows <= FEED_FEED_ROWS) {
    rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      if (rows > 0u) {
        FEED_PUTS(b, ",");
      } else {
        /* First post: no separator. */
      }
      feed_put_post(b, s);
      rows++;
    } else if (rc == SQLITE_DONE) {
      /* End of the page: the loop ends on its own. */
    } else {
      /* SQLite error: the loop ends and the request is answered 500 below. */
    }
  }
  FEED_PUTS(b, "]}");
  if (feed_finish(s) != SQLITE_OK || rc != SQLITE_DONE || rows > FEED_FEED_ROWS) {
    r = feed_ise(env); /* a step error, or more rows than LIMIT allows */
  } else {
    r = feed_respond_buf(env, 200, b);
  }
  return r;
}

static ERL_NIF_TERM nif_feed(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ERL_NIF_TERM r;
  (void)argv; /* arity 0 */
  if (argc != 0) {
    r = feed_ise(env); /* the VM enforces arity; this cannot happen */
  } else if (feed_lock() == 0) {
    r = feed_ise(env); /* not open, or the lock failed */
  } else {
    r = feed_feed_locked(env);
    feed_unlock();
  }
  return r;
}

/* ==== GET /posts/:id ========================================================================= */

static ERL_NIF_TERM feed_post_locked(ErlNifEnv *env, int64_t id) {
  sqlite3_stmt *s = g_feed.st[ST_POST];
  struct feed_buf *b = &g_feed.body;
  ERL_NIF_TERM r;
  if (sqlite3_bind_int64(s, 1, (sqlite3_int64)id) != SQLITE_OK) {
    (void)feed_finish(s); /* the bind already failed; the reset code adds nothing */
    r = feed_ise(env);
  } else {
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      feed_buf_clear(b);
      FEED_PUTS(b, "{\"post\":");
      feed_put_post(b, s);
      FEED_PUTS(b, "}");
      r = feed_respond_buf(env, 200, b);
    } else if (rc == SQLITE_DONE) {
      r = feed_error(env, 404, "post not found"); /* no such id: an answer, not a fault */
    } else {
      r = feed_ise(env); /* SQLite error */
    }
    if (feed_finish(s) != SQLITE_OK && rc == SQLITE_ROW) {
      r = feed_ise(env); /* the row was read but the statement ended badly: do not trust it */
    } else {
      /* Clean end, or the step error was already answered. */
    }
  }
  return r;
}

static ERL_NIF_TERM nif_post(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifSInt64 id = 0;
  ERL_NIF_TERM r;
  if (argc != 1) {
    r = feed_ise(env); /* the VM enforces arity; this cannot happen */
  } else if (enif_get_int64(env, argv[0], &id) == 0 || id <= 0) {
    r = feed_ise(env); /* Elixir validated the id; anything else is a caller fault, not a raise */
  } else if (feed_lock() == 0) {
    r = feed_ise(env);
  } else {
    r = feed_post_locked(env, (int64_t)id);
    feed_unlock();
  }
  return r;
}

/* ==== POST /posts/:id/like =================================================================== */

/* 1 = the post exists, 0 = it does not, -1 = SQLite fault. */
static int feed_post_exists(int64_t id) {
  sqlite3_stmt *s = g_feed.st[ST_EXISTS];
  int out;
  if (sqlite3_bind_int64(s, 1, (sqlite3_int64)id) != SQLITE_OK) {
    out = -1;
  } else {
    int rc = sqlite3_step(s);
    if (rc == SQLITE_ROW) {
      out = 1;
    } else if (rc == SQLITE_DONE) {
      out = 0;
    } else {
      out = -1;
    }
  }
  if (feed_finish(s) != SQLITE_OK && out == 1) {
    out = -1; /* the row was seen but the statement ended badly */
  } else {
    /* Clean end, or the fault is already recorded. */
  }
  return out;
}

/* 1 = a new like, 0 = a repeat, -1 = SQLite fault. */
static int feed_insert_like(int64_t uid, int64_t id) {
  sqlite3_stmt *s = g_feed.st[ST_LIKE];
  int out;
  if (sqlite3_bind_int64(s, 1, (sqlite3_int64)uid) != SQLITE_OK ||
      sqlite3_bind_int64(s, 2, (sqlite3_int64)id) != SQLITE_OK) {
    out = -1;
  } else if (sqlite3_step(s) != SQLITE_DONE) {
    out = -1; /* the write did not commit: never claim it did */
  } else if (sqlite3_changes(g_feed.db) > 0) {
    out = 1;
  } else {
    out = 0; /* ON CONFLICT DO NOTHING: the like was already there */
  }
  if (feed_finish(s) != SQLITE_OK) {
    out = -1;
  } else {
    /* Clean end. */
  }
  return out;
}

static ERL_NIF_TERM feed_like_locked(ErlNifEnv *env, int64_t uid, int64_t id) {
  struct feed_buf *b = &g_feed.body;
  ERL_NIF_TERM r;
  int ex = feed_post_exists(id);
  if (ex == 0) {
    r = feed_error(env, 404, "post not found");
  } else if (ex < 0) {
    r = feed_ise(env);
  } else {
    int nw = feed_insert_like(uid, id);
    if (nw < 0) {
      r = feed_ise(env);
    } else {
      feed_buf_clear(b);
      if (nw == 1) {
        FEED_PUTS(b, "{\"liked\":true,\"already_liked\":false,\"post_id\":");
      } else {
        FEED_PUTS(b, "{\"liked\":true,\"already_liked\":true,\"post_id\":");
      }
      feed_put_int(b, id);
      FEED_PUTS(b, "}");
      r = feed_respond_buf(env, (nw == 1) ? 201 : 200, b);
    }
  }
  return r;
}

static ERL_NIF_TERM nif_like(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifSInt64 uid = 0;
  ErlNifSInt64 id = 0;
  ERL_NIF_TERM r;
  int ck_pid_ok = 0;
  ErlNifPid ck_pid;
  if (argc != 2) {
    r = feed_ise(env);
  } else if (enif_get_int64(env, argv[0], &uid) == 0 || uid <= 0) {
    r = feed_ise(env); /* uid comes from a verified token; anything else is a caller fault */
  } else if (enif_get_int64(env, argv[1], &id) == 0 || id <= 0) {
    r = feed_ise(env);
  } else if (feed_lock() == 0) {
    r = feed_ise(env);
  } else {
    int held = __atomic_load_n(&g_feed.hold, __ATOMIC_ACQUIRE); /* inside the lock: see feed_tail */
    r = (held != 0) ? enif_make_atom(env, "busy") : feed_like_locked(env, (int64_t)uid, (int64_t)id);
    ck_pid_ok = feed_due_locked();
    ck_pid = g_feed.ck_pid;
    feed_unlock();
    feed_notify(ck_pid_ok, &ck_pid);
  }
  return r;
}

/* ==== POST /posts: body extraction, trim, insert ============================================ */

enum feed_body_rc { BODY_OK = 0, BODY_MALFORMED, BODY_MISSING, BODY_FAULT };

/* Validates the request JSON and copies its "body" string into g_feed.text. */
static enum feed_body_rc feed_extract_body(const ErlNifBinary *raw) {
  sqlite3_stmt *s = g_feed.st[ST_BODY];
  enum feed_body_rc out;
  feed_buf_clear(&g_feed.text);
  if (raw->size > (size_t)FEED_TEXT_CAP) {
    out = BODY_FAULT; /* > 64 KB: out of the spec's scope; answered 500 rather than parsed */
  } else if (sqlite3_bind_text(s, 1, (const char *)raw->data, (int)raw->size, SQLITE_STATIC) != SQLITE_OK) {
    out = BODY_FAULT;
  } else if (sqlite3_step(s) != SQLITE_ROW) {
    out = BODY_FAULT;
  } else {
    const unsigned char *top = sqlite3_column_text(s, 0);
    if (top == NULL || strcmp((const char *)top, "bad") == 0) {
      out = BODY_MALFORMED; /* not JSON at all */
    } else if (sqlite3_column_type(s, 1) != SQLITE_TEXT) {
      out = BODY_MISSING;   /* JSON, but no string "body" (or not an object) */
    } else {
      const unsigned char *t = sqlite3_column_text(s, 1);
      int n = sqlite3_column_bytes(s, 1);
      if (n < 0) {
        out = BODY_FAULT;
      } else {
        feed_put(&g_feed.text, t, (size_t)n);
        out = (g_feed.text.overflow != 0) ? BODY_FAULT : BODY_OK;
      }
    }
  }
  if (feed_finish(s) != SQLITE_OK && out == BODY_OK) {
    out = BODY_FAULT;
  } else {
    /* Clean end, or the outcome is already decided. */
  }
  return out;
}

/* JavaScript's String.prototype.trim set: Unicode White_Space without NEL (U+0085), plus BOM. */
static int feed_js_space(uint32_t cp) {
  return ((cp >= 9u && cp <= 13u) || cp == 0x20u || cp == 0xA0u || cp == 0x1680u ||
          (cp >= 0x2000u && cp <= 0x200Au) || cp == 0x2028u || cp == 0x2029u || cp == 0x202Fu ||
          cp == 0x205Fu || cp == 0x3000u || cp == 0xFEFFu) ? 1 : 0;
}

/* The code point at s (n >= 1 bytes left); *len gets its byte length (1..4, never > n).
 * A malformed or truncated sequence is one byte long and is not a space. */
static uint32_t feed_utf8_at(const unsigned char *s, size_t n, size_t *len) {
  uint32_t c = s[0];
  uint32_t cp;
  if (c < 0x80u) {
    *len = 1u; cp = c;
  } else if ((c & 0xE0u) == 0xC0u && n >= 2u) {
    *len = 2u; cp = ((c & 0x1Fu) << 6) | (s[1] & 0x3Fu);
  } else if ((c & 0xF0u) == 0xE0u && n >= 3u) {
    *len = 3u; cp = ((c & 0x0Fu) << 12) | ((uint32_t)(s[1] & 0x3Fu) << 6) | (s[2] & 0x3Fu);
  } else if ((c & 0xF8u) == 0xF0u && n >= 4u) {
    *len = 4u;
    cp = ((c & 0x07u) << 18) | ((uint32_t)(s[1] & 0x3Fu) << 12) | ((uint32_t)(s[2] & 0x3Fu) << 6) | (s[3] & 0x3Fu);
  } else {
    *len = 1u; cp = 0xFFFDu; /* stray continuation byte or cut sequence: one byte, not a space */
  }
  return cp;
}

/* Trims [*a, *z) of g_feed.text. Bound: each step consumes >= 1 byte of at most text.n. */
static void feed_trim(size_t *a, size_t *z) {
  const unsigned char *p = g_feed.text.p;
  size_t lo = 0u;
  size_t hi = g_feed.text.n;
  size_t len = 1u;
  int more = 1;
  while (lo < hi && more != 0) {
    if (feed_js_space(feed_utf8_at(p + lo, hi - lo, &len)) != 0) {
      lo += len;
    } else {
      more = 0; /* first non-space from the front */
    }
  }
  more = 1;
  while (hi > lo && more != 0) {
    size_t s = hi - 1u;
    unsigned back = 0u;
    while (s > lo && (p[s] & 0xC0u) == 0x80u && back < 3u) { /* bound: a lead byte is <= 3 back */
      s--;
      back++;
    }
    if (feed_js_space(feed_utf8_at(p + s, hi - s, &len)) != 0 && s + len == hi) {
      hi = s;
    } else {
      more = 0; /* last non-space from the back (or a malformed tail, which is kept) */
    }
  }
  *a = lo;
  *z = hi;
}

/* Code points in [a, z): every byte that is not a continuation byte starts one. Bound: z - a. */
static size_t feed_count_chars(size_t a, size_t z) {
  size_t chars = 0u;
  for (size_t i = a; i < z; i++) {
    chars += ((g_feed.text.p[i] & 0xC0u) != 0x80u) ? 1u : 0u;
  }
  return chars;
}

/* INSERT ... RETURNING. Returns 0 and fills id/created on success, -1 on any fault. */
static int feed_insert_post(int64_t uid, size_t a, size_t z, int64_t *id, char *created, size_t *cn) {
  sqlite3_stmt *s = g_feed.st[ST_INSERT];
  int out = -1;
  if (sqlite3_bind_int64(s, 1, (sqlite3_int64)uid) != SQLITE_OK ||
      sqlite3_bind_text(s, 2, (const char *)g_feed.text.p + a, (int)(z - a), SQLITE_STATIC) != SQLITE_OK) {
    out = -1;
  } else if (sqlite3_step(s) != SQLITE_ROW) {
    out = -1; /* the insert did not happen (CHECK, I/O): never claim it did */
  } else {
    const unsigned char *ct = sqlite3_column_text(s, 1);
    int n = sqlite3_column_bytes(s, 1);
    if (ct == NULL || n <= 0 || (size_t)n >= FEED_CREATED_CAP) {
      out = -1; /* created_at must be the 24-byte wire timestamp */
    } else {
      *id = (int64_t)sqlite3_column_int64(s, 0);
      (void)memcpy(created, ct, (size_t)n);
      *cn = (size_t)n;
      int rc = SQLITE_ROW;
      unsigned k = 0u;
      while (rc == SQLITE_ROW && k < FEED_DRAIN_MAX) { /* bound: RETURNING yields one row */
        rc = sqlite3_step(s);
        k++;
      }
      out = (rc == SQLITE_DONE) ? 0 : -1; /* the commit happens on DONE; only then is it a 201 */
    }
  }
  if (feed_finish(s) != SQLITE_OK) {
    out = -1;
  } else {
    /* Clean end. */
  }
  return out;
}

static ERL_NIF_TERM feed_render_created(ErlNifEnv *env, int64_t id, size_t a, size_t z,
                                        const char *created, size_t cn, const ErlNifBinary *user) {
  struct feed_buf *b = &g_feed.body;
  feed_buf_clear(b);
  FEED_PUTS(b, "{\"post\":{\"id\":");
  feed_put_int(b, id);
  FEED_PUTS(b, ",\"body\":\"");
  feed_put_esc(b, g_feed.text.p + a, z - a);
  FEED_PUTS(b, "\",\"created_at\":\"");
  feed_put_esc(b, (const unsigned char *)created, cn);
  FEED_PUTS(b, "\",\"author\":\"");
  feed_put_esc(b, user->data, user->size);
  FEED_PUTS(b, "\",\"like_count\":0}}");
  return feed_respond_buf(env, 201, b);
}

static ERL_NIF_TERM feed_create_locked(ErlNifEnv *env, int64_t uid, const ErlNifBinary *user,
                                       const ErlNifBinary *raw) {
  ERL_NIF_TERM r;
  enum feed_body_rc br = feed_extract_body(raw);
  if (br == BODY_MALFORMED) {
    r = feed_error(env, 400, "malformed JSON body");
  } else if (br == BODY_MISSING) {
    r = feed_error(env, 400, "body is required");
  } else if (br != BODY_OK) {
    r = feed_ise(env);
  } else {
    size_t a = 0u;
    size_t z = 0u;
    feed_trim(&a, &z);
    if (z == a) {
      r = feed_error(env, 400, "body is required"); /* empty after trimming */
    } else if (feed_count_chars(a, z) > FEED_MAX_CHARS) {
      r = feed_error(env, 400, "body must be at most 500 characters");
    } else {
      int64_t id = 0;
      char created[FEED_CREATED_CAP];
      size_t cn = 0u;
      if (feed_insert_post(uid, a, z, &id, created, &cn) != 0) {
        r = feed_ise(env);
      } else {
        r = feed_render_created(env, id, a, z, created, cn, user);
      }
    }
  }
  return r;
}

static ERL_NIF_TERM nif_create(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifSInt64 uid = 0;
  ErlNifBinary user;
  ErlNifBinary raw;
  ERL_NIF_TERM r;
  int ck_pid_ok = 0;
  ErlNifPid ck_pid;
  if (argc != 3) {
    r = feed_ise(env);
  } else if (enif_get_int64(env, argv[0], &uid) == 0 || uid <= 0) {
    r = feed_ise(env);
  } else if (enif_inspect_binary(env, argv[1], &user) == 0 || enif_inspect_binary(env, argv[2], &raw) == 0) {
    r = feed_ise(env); /* Elixir passes binaries; anything else is a caller fault, not a raise */
  } else if (feed_lock() == 0) {
    r = feed_ise(env);
  } else {
    int held = __atomic_load_n(&g_feed.hold, __ATOMIC_ACQUIRE);
    r = (held != 0) ? enif_make_atom(env, "busy") : feed_create_locked(env, (int64_t)uid, &user, &raw);
    ck_pid_ok = feed_due_locked();
    ck_pid = g_feed.ck_pid;
    feed_unlock();
    feed_notify(ck_pid_ok, &ck_pid);
  }
  return r;
}

/* ==== JWT claims (signature is checked in Elixir first) ====================================== */

static ERL_NIF_TERM feed_text_or_nil(ErlNifEnv *env, sqlite3_stmt *s, int i) {
  ERL_NIF_TERM r;
  if (sqlite3_column_type(s, i) != SQLITE_TEXT) {
    r = enif_make_atom(env, "nil"); /* absent or not a string: Elixir answers "invalid token payload" */
  } else {
    const unsigned char *t = sqlite3_column_text(s, i);
    int n = sqlite3_column_bytes(s, i);
    unsigned char *o = (n >= 0 && t != NULL) ? enif_make_new_binary(env, (size_t)n, &r) : NULL;
    if (o == NULL) {
      r = enif_make_atom(env, "nil");
    } else if (n > 0) {
      (void)memcpy(o, t, (size_t)n);
    } else {
      /* Empty string: the binary is already empty. */
    }
  }
  return r;
}

static ERL_NIF_TERM feed_claims_locked(ErlNifEnv *env, const ErlNifBinary *h, const ErlNifBinary *p) {
  sqlite3_stmt *s = g_feed.st[ST_CLAIMS];
  ERL_NIF_TERM r = enif_make_atom(env, "invalid");
  if (sqlite3_bind_text(s, 1, (const char *)h->data, (int)h->size, SQLITE_STATIC) != SQLITE_OK ||
      sqlite3_bind_text(s, 2, (const char *)p->data, (int)p->size, SQLITE_STATIC) != SQLITE_OK) {
    /* Bind failed: stays :invalid. */
  } else if (sqlite3_step(s) != SQLITE_ROW) {
    /* SQLite error: stays :invalid (a 401, never a 201). */
  } else if (sqlite3_column_int(s, 0) == 0 || sqlite3_column_int(s, 1) == 0) {
    /* Header is not {"alg":"HS256",...} or payload is not an object: :invalid. */
  } else {
    ERL_NIF_TERM exp = (sqlite3_column_type(s, 2) == SQLITE_NULL)
      ? enif_make_atom(env, "nil") : enif_make_double(env, sqlite3_column_double(s, 2));
    r = enif_make_tuple4(env, enif_make_atom(env, "ok"), exp, feed_text_or_nil(env, s, 3),
                         feed_text_or_nil(env, s, 4));
  }
  (void)feed_finish(s); /* a read-only statement: a reset error cannot change the decision above */
  return r;
}

static ERL_NIF_TERM nif_claims(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifBinary h;
  ErlNifBinary p;
  ERL_NIF_TERM r;
  if (argc != 2) {
    r = enif_make_atom(env, "invalid");
  } else if (enif_inspect_binary(env, argv[0], &h) == 0 || enif_inspect_binary(env, argv[1], &p) == 0) {
    r = enif_make_atom(env, "invalid");
  } else if (feed_lock() == 0) {
    r = enif_make_atom(env, "invalid"); /* not open: no token can be checked, so none is accepted */
  } else {
    r = feed_claims_locked(env, &h, &p);
    feed_unlock();
  }
  return r;
}

/* ==== GET /health ============================================================================ */

static ERL_NIF_TERM feed_health_locked(ErlNifEnv *env, int64_t up) {
  sqlite3_stmt *s = g_feed.st[ST_HEALTH];
  struct feed_buf *b = &g_feed.body;
  ERL_NIF_TERM r;
  int rc = sqlite3_step(s);
  int fr = feed_finish(s);
  feed_buf_clear(b);
  if (rc == SQLITE_ROW && fr == SQLITE_OK) {
    FEED_PUTS(b, "{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    feed_put_int(b, up);
    FEED_PUTS(b, "}");
    r = feed_respond_buf(env, 200, b);
  } else {
    const char *m = sqlite3_errmsg(g_feed.db);
    FEED_PUTS(b, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
    feed_put_esc(b, (const unsigned char *)m, (m != NULL) ? strlen(m) : 0u);
    FEED_PUTS(b, "\"}");
    r = feed_respond_buf(env, 503, b);
  }
  return r;
}

static ERL_NIF_TERM nif_health(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifSInt64 up = 0;
  ERL_NIF_TERM r;
  if (argc != 1) {
    r = feed_ise(env);
  } else if (enif_get_int64(env, argv[0], &up) == 0 || up < 0) {
    r = feed_ise(env);
  } else if (pthread_mutex_lock(&g_feed.mu) != 0) {
    r = feed_ise(env);
  } else if (g_feed.opened == 0) {
    feed_unlock();
    r = feed_error(env, 503, "database not open"); /* health reports the state instead of a 500 */
  } else {
    r = feed_health_locked(env, (int64_t)up);
    feed_unlock();
  }
  return r;
}

/* ==== checkpoints ============================================================================ */

static int feed_wal_hook(void *arg, sqlite3 *d, const char *name, int frames) {
  (void)arg; (void)d; (void)name; /* one database, one hook: nothing to tell apart */
  __atomic_store_n(&g_feed.wal_frames, frames, __ATOMIC_RELAXED);
  return SQLITE_OK;
}

/* ---- the checkpoint, driven by the Elixir state machine (lib/feed/checkpoint.ex) ------------------
 * No thread and no sleep. A write NIF sends `ck_due` once the WAL passes the threshold; the state
 * machine then calls feed_bulk (writes go on), then feed_tail (writes held), then goes idle. */

/* Copies what the readers and writers leave alone. Never takes mu. 1 on success, 0 on an I/O error. */
static ERL_NIF_TERM nif_ck_bulk(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  int log = 0;
  int done = 0;
  int rc = SQLITE_MISUSE;
  (void)argv;
  if (argc == 0 && g_feed.ckdb != NULL) {
    rc = sqlite3_wal_checkpoint_v2(g_feed.ckdb, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
  } else {
    /* Not open, or a wrong call: report an error below. */
  }
  return enif_make_atom(env, (rc == SQLITE_OK || rc == SQLITE_BUSY) ? "ok" : "error");
}

/* Holds writes, waits for the write that may be in flight by taking and dropping mu (a write that gets
 * mu afterwards sees `hold` inside the lock and answers :busy), then copies the rest. When everything is
 * copied the next write restarts the log. Always clears `hold` and `armed`. Returns :done if the log was
 * fully copied back, :partial if readers still pinned frames. */
static ERL_NIF_TERM nif_ck_tail(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  int log = -1;
  int done = -2;
  int rc = SQLITE_MISUSE;
  const char *answer = "error";
  (void)argv;
  if (argc != 0 || g_feed.ckdb == NULL) {
    /* Wrong call or not open. */
  } else {
    __atomic_store_n(&g_feed.hold, 1, __ATOMIC_RELEASE);
    if (feed_lock() == 0) {
      /* Cannot take the barrier: skip the tail, the bulk pass already ran. */
    } else {
      feed_unlock(); /* barrier: every write now either finished or will see `hold` and back off */
      rc = sqlite3_wal_checkpoint_v2(g_feed.ckdb, NULL, SQLITE_CHECKPOINT_PASSIVE, &log, &done);
      if (rc == SQLITE_OK && log >= 0 && log == done) {
        __atomic_store_n(&g_feed.wal_frames, 0, __ATOMIC_RELAXED); /* caught up: the log restarts */
        answer = "done";
      } else {
        answer = "partial"; /* retried at the next threshold crossing */
      }
#ifdef FEED_TEST_TRACE
      (void)fprintf(stderr, "ckpt tail rc=%d log=%d done=%d\n", rc, log, done);
#endif
    }
    __atomic_store_n(&g_feed.hold, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_feed.armed, 0, __ATOMIC_RELEASE);
  }
  return enif_make_atom(env, answer);
}

/* The state machine registers itself; a restart registers again and re-arms. */
static ERL_NIF_TERM nif_ck_register(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifPid pid;
  const char *answer = "error";
  (void)argv;
  if (argc != 0 || enif_self(env, &pid) == NULL) {
    /* Wrong call. */
  } else if (feed_lock() == 0) {
    /* Cannot lock. */
  } else {
    g_feed.ck_pid = pid;
    g_feed.ck_registered = 1;
    __atomic_store_n(&g_feed.armed, 0, __ATOMIC_RELEASE);
    __atomic_store_n(&g_feed.hold, 0, __ATOMIC_RELEASE);
    feed_unlock();
    answer = "ok";
  }
  return enif_make_atom(env, answer);
}

/* ==== open / close =========================================================================== */

static void feed_close_all(void) {
  for (unsigned i = 0u; i < (unsigned)ST_COUNT; i++) { /* bound: ST_COUNT */
    (void)sqlite3_finalize(g_feed.st[i]); /* finalize repeats an old step error; nothing to act on */
    g_feed.st[i] = NULL;
  }
  if (g_feed.ckdb != NULL) {
    (void)sqlite3_close_v2(g_feed.ckdb); /* close_v2 defers if busy; the process is ending anyway */
    g_feed.ckdb = NULL;
  } else {
    /* Never opened. */
  }
  if (g_feed.db != NULL) {
    (void)sqlite3_close_v2(g_feed.db);
    g_feed.db = NULL;
  } else {
    /* Never opened. */
  }
}

/* Returns NULL on success, or the name of the step that failed (the caller adds errmsg). */
static const char *feed_open_db(const char *path) {
  const char *fail = NULL;
  int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX;
  if (sqlite3_initialize() != SQLITE_OK) {
    fail = "init";
  } else if (sqlite3_open_v2(path, &g_feed.db, flags, "unix-excl") != SQLITE_OK) {
    fail = "open"; /* unix-excl: this process holds the file; the WAL index lives in heap memory */
  } else {
    for (size_t i = 0u; i < FEED_PRAGMA_COUNT && fail == NULL; i++) { /* bound: pragma count */
      if (sqlite3_exec(g_feed.db, feed_pragmas[i], NULL, NULL, NULL) != SQLITE_OK) {
        fail = feed_pragmas[i];
      } else {
        /* Applied. */
      }
    }
  }
  if (fail == NULL) {
    (void)sqlite3_wal_hook(g_feed.db, feed_wal_hook, NULL); /* returns the previous hook (none) */
  } else {
    /* Failed: no hook to install. */
  }
  return fail;
}

static const char *feed_prepare_all(void) {
  const char *fail = NULL;
  for (unsigned i = 0u; i < (unsigned)ST_COUNT && fail == NULL; i++) { /* bound: ST_COUNT */
    if (sqlite3_prepare_v3(g_feed.db, feed_sql[i], -1, SQLITE_PREPARE_PERSISTENT, &g_feed.st[i], NULL) != SQLITE_OK) {
      fail = feed_sql[i];
    } else {
      /* Prepared. */
    }
  }
  return fail;
}

static const char *feed_open_checkpoint_db(const char *path) {
  const char *fail = NULL;
  if (sqlite3_open_v2(path, &g_feed.ckdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_NOMUTEX, "unix-excl") != SQLITE_OK) {
    fail = "checkpoint open";
  } else if (sqlite3_exec(g_feed.ckdb, "PRAGMA wal_autocheckpoint = 0", NULL, NULL, NULL) != SQLITE_OK) {
    fail = "checkpoint pragma";
  } else if (sqlite3_exec(g_feed.ckdb, "SELECT count(*) FROM sqlite_schema", NULL, NULL, NULL) != SQLITE_OK) {
    fail = "checkpoint read"; /* a connection that has never read has no WAL open: checkpoints answer -1/-1 */
  } else {
    /* Open, with its WAL attached. */
  }
  return fail;
}

static ERL_NIF_TERM feed_err_tuple(ErlNifEnv *env, const char *what) {
  char m[FEED_ERR_CAP];
  ERL_NIF_TERM t;
  const char *detail = (g_feed.db != NULL) ? sqlite3_errmsg(g_feed.db) : "no database";
  int n = snprintf(m, sizeof m, "%s: %s", what, (detail != NULL) ? detail : "unknown");
  size_t k = (n <= 0) ? 0u : (((size_t)n >= sizeof m) ? sizeof m - 1u : (size_t)n); /* truncation is fine for a message */
  unsigned char *o = enif_make_new_binary(env, k, &t);
  if (o == NULL) {
    t = enif_make_atom(env, "open_failed");
  } else if (k > 0u) {
    (void)memcpy(o, m, k);
  } else {
    /* Empty message. */
  }
  return enif_make_tuple2(env, enif_make_atom(env, "error"), t);
}

static ERL_NIF_TERM feed_open_locked(ErlNifEnv *env, const char *path) {
  ERL_NIF_TERM r;
  const char *fail = NULL;
  if (g_feed.opened != 0) {
    r = (strcmp(g_feed.path, path) == 0) ? enif_make_atom(env, "ok") /* idempotent on the same path */
      : feed_err_tuple(env, "already open on another path");
  } else {
    fail = feed_open_db(path);
    fail = (fail != NULL) ? fail : feed_prepare_all();
    fail = (fail != NULL) ? fail : feed_open_checkpoint_db(path);
    if (fail != NULL) {
      r = feed_err_tuple(env, fail);
      feed_close_all(); /* leave nothing half-open, so a retry starts clean */
    } else {
      (void)memcpy(g_feed.path, path, strlen(path) + 1u); /* fits: the caller checked the length */
      g_feed.opened = 1;
      r = enif_make_atom(env, "ok");
    }
  }
  return r;
}

static ERL_NIF_TERM nif_open(ErlNifEnv *env, int argc, const ERL_NIF_TERM argv[]) {
  ErlNifBinary pb;
  char path[FEED_PATH_CAP];
  ERL_NIF_TERM r;
  if (argc != 1) {
    r = feed_err_tuple(env, "arity");
  } else if (enif_inspect_iolist_as_binary(env, argv[0], &pb) == 0) {
    r = feed_err_tuple(env, "path is not a binary");
  } else if (pb.size == 0u || pb.size >= sizeof path || memchr(pb.data, 0, pb.size) != NULL) {
    r = feed_err_tuple(env, "path is empty, too long or holds a NUL");
  } else if (pthread_mutex_lock(&g_feed.mu) != 0) {
    r = feed_err_tuple(env, "lock");
  } else {
    (void)memcpy(path, pb.data, pb.size);
    path[pb.size] = '\0';
    r = feed_open_locked(env, path);
    feed_unlock();
  }
  return r;
}

/* ==== NIF lifecycle ========================================================================== */

static int feed_load(ErlNifEnv *env, void **priv, ERL_NIF_TERM info) {
  (void)env; (void)priv; (void)info; /* all state is file-static and statically initialised */
  return 0;
}

static void feed_unload(ErlNifEnv *env, void *priv) {
  (void)env; (void)priv;
  g_feed.ck_registered = 0;
  feed_close_all();
  g_feed.opened = 0;
}

static ErlNifFunc feed_funcs[] = { /* D1: the VM requires this function-pointer table */
  {"open", 1, nif_open, ERL_NIF_DIRTY_JOB_IO_BOUND},
  {"feed", 0, nif_feed, 0},
  {"post", 1, nif_post, 0},
  {"like", 2, nif_like, 0},
  {"create", 3, nif_create, 0},
  {"claims", 2, nif_claims, 0},
  {"health", 1, nif_health, 0},
  {"ck_register", 0, nif_ck_register, 0},
  {"ck_bulk", 0, nif_ck_bulk, ERL_NIF_DIRTY_JOB_IO_BOUND},
  {"ck_tail", 0, nif_ck_tail, ERL_NIF_DIRTY_JOB_IO_BOUND},
};

ERL_NIF_INIT(Elixir.Feed.Nif, feed_funcs, feed_load, NULL, NULL, feed_unload)
