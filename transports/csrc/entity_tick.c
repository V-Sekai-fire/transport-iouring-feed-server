#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE 1
#include "entity_tick.h"
#include "../../datasources/entity-packet/xr_grid_entity_packet.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#define PKT XR_PACKET_SIZE
#include "otel_span.h"
#if defined(__linux__)
#define ET_RECV_PATH "io_uring"
#include <linux/io_uring.h>
#include <sys/mman.h>
#include <sys/syscall.h>
/* Receive through io_uring: one multishot recvmsg into a provided-buffer ring, so packets move out of
 * the socket as they arrive and the 64 Hz tick only reaps completions. Raw syscalls, no liburing, no heap. */
#define ER_BUFS 8192
#define ER_BUFSZ 256
static struct {
  int fd, sock;
  unsigned *sq_tail, *sq_mask, *sq_array, *cq_head, *cq_tail, *cq_mask, *cq_overflow;
  struct io_uring_sqe *sqes;
  struct io_uring_cqe *cqes;
  struct io_uring_buf_ring *br;
  unsigned char *pool;
  unsigned br_tail;
  struct msghdr msg;
} er = { -1, -1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0} };
static void er_add(unsigned bid) {
  struct io_uring_buf *b = &er.br->bufs[er.br_tail & (ER_BUFS - 1)];
  b->addr = (unsigned long)(er.pool + (size_t)bid * ER_BUFSZ); b->len = ER_BUFSZ; b->bid = (unsigned short)bid;
  er.br_tail++;
  __atomic_store_n(&er.br->tail, (unsigned short)er.br_tail, __ATOMIC_RELEASE);
}
static void er_arm(void) {
  unsigned t = *er.sq_tail, idx = t & *er.sq_mask;
  struct io_uring_sqe *s = &er.sqes[idx];
  memset(s, 0, sizeof *s);
  s->opcode = IORING_OP_RECVMSG; s->fd = er.sock; s->addr = (unsigned long)&er.msg; s->len = 1;
  s->flags = IOSQE_BUFFER_SELECT; s->buf_group = 0; s->ioprio = IORING_RECV_MULTISHOT;
  er.sq_array[idx] = idx;
  __atomic_store_n(er.sq_tail, t + 1, __ATOMIC_RELEASE);
  syscall(__NR_io_uring_enter, er.fd, 1, 0, 0, 0, 0);
}
static int er_open(int sock) {
  struct io_uring_params p; memset(&p, 0, sizeof p);
  p.flags = IORING_SETUP_CQSIZE; p.cq_entries = 16384;
  er.fd = (int)syscall(__NR_io_uring_setup, 8, &p);
  if (er.fd < 0) return -errno;
  size_t sqsz = p.sq_off.array + p.sq_entries * sizeof(unsigned), cqsz = p.cq_off.cqes + p.cq_entries * sizeof(struct io_uring_cqe);
  void *sq, *cq;
  if (p.features & IORING_FEAT_SINGLE_MMAP) { if (cqsz > sqsz) sqsz = cqsz; }
  sq = mmap(0, sqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, er.fd, IORING_OFF_SQ_RING);
  if (sq == MAP_FAILED) return -errno;
  cq = (p.features & IORING_FEAT_SINGLE_MMAP) ? sq : mmap(0, cqsz, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, er.fd, IORING_OFF_CQ_RING);
  if (cq == MAP_FAILED) return -errno;
  er.sqes = mmap(0, p.sq_entries * sizeof(struct io_uring_sqe), PROT_READ | PROT_WRITE, MAP_SHARED | MAP_POPULATE, er.fd, IORING_OFF_SQES);
  if (er.sqes == MAP_FAILED) return -errno;
  er.sq_tail = (unsigned *)((char *)sq + p.sq_off.tail); er.sq_mask = (unsigned *)((char *)sq + p.sq_off.ring_mask);
  er.sq_array = (unsigned *)((char *)sq + p.sq_off.array);
  er.cq_head = (unsigned *)((char *)cq + p.cq_off.head); er.cq_tail = (unsigned *)((char *)cq + p.cq_off.tail);
  er.cq_mask = (unsigned *)((char *)cq + p.cq_off.ring_mask); er.cq_overflow = (unsigned *)((char *)cq + p.cq_off.overflow);
  er.cqes = (struct io_uring_cqe *)((char *)cq + p.cq_off.cqes);
  er.br = mmap(0, ER_BUFS * sizeof(struct io_uring_buf), PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  er.pool = mmap(0, (size_t)ER_BUFS * ER_BUFSZ, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (er.br == MAP_FAILED || er.pool == MAP_FAILED) return -ENOMEM;
  struct io_uring_buf_reg reg; memset(&reg, 0, sizeof reg);
  reg.ring_addr = (unsigned long)er.br; reg.ring_entries = ER_BUFS; reg.bgid = 0;
  if (syscall(__NR_io_uring_register, er.fd, IORING_REGISTER_PBUF_RING, &reg, 1) < 0) return -errno;
  for (unsigned i = 0; i < ER_BUFS; i++) er_add(i);
  memset(&er.msg, 0, sizeof er.msg); er.msg.msg_namelen = sizeof(struct sockaddr_in6);
  er.sock = sock;
  er_arm();
  return 0;
}
static ssize_t er_recv(void *buf, size_t cap, struct sockaddr_storage *from, socklen_t *fl) {
  for (;;) {
    unsigned head = *er.cq_head;
    if (head == __atomic_load_n(er.cq_tail, __ATOMIC_ACQUIRE)) return -1;
    struct io_uring_cqe c = er.cqes[head & *er.cq_mask];
    __atomic_store_n(er.cq_head, head + 1, __ATOMIC_RELEASE);
    if (!(c.flags & IORING_CQE_F_MORE)) er_arm();
    if (c.res < 0 || !(c.flags & IORING_CQE_F_BUFFER)) continue;
    unsigned bid = c.flags >> IORING_CQE_BUFFER_SHIFT;
    unsigned char *b = er.pool + (size_t)bid * ER_BUFSZ;
    struct io_uring_recvmsg_out o; memcpy(&o, b, sizeof o);
    memset(from, 0, sizeof *from);
    size_t nl = o.namelen < sizeof *from ? o.namelen : sizeof *from;
    memcpy(from, b + sizeof o, nl); *fl = (socklen_t)nl;
    size_t n = o.payloadlen, c2 = n < cap ? n : cap;
    memcpy(buf, b + sizeof o + er.msg.msg_namelen, c2);
    er_add(bid);
    return (ssize_t)n;
  }
}
static int64_t er_overflow(void) { return er.cq_overflow ? (int64_t)*er.cq_overflow : 0; }
/* The multishot receive holds the socket until it ends, so closing the ring is not enough: cancel it and
 * wait for its last completion, or the next bind of the same port finds the old socket still there. */
static void er_close(void) {
  if (er.fd < 0) return;
  unsigned t = *er.sq_tail, idx = t & *er.sq_mask;
  struct io_uring_sqe *s = &er.sqes[idx];
  memset(s, 0, sizeof *s);
  s->opcode = IORING_OP_ASYNC_CANCEL; s->cancel_flags = IORING_ASYNC_CANCEL_ANY | IORING_ASYNC_CANCEL_ALL; s->fd = -1;
  er.sq_array[idx] = idx;
  __atomic_store_n(er.sq_tail, t + 1, __ATOMIC_RELEASE);
  for (int i = 0, ended = 0; !ended && i < 1000; i++) {
    syscall(__NR_io_uring_enter, er.fd, i == 0 ? 1 : 0, 0, 0, 0, 0);
    unsigned head = *er.cq_head, got = 0;
    while (head != __atomic_load_n(er.cq_tail, __ATOMIC_ACQUIRE)) {
      struct io_uring_cqe c = er.cqes[head++ & *er.cq_mask]; got++;
      if ((c.res == -ECANCELED && !(c.flags & IORING_CQE_F_MORE)) || c.res == -ENOENT) ended = 1;
    }
    __atomic_store_n(er.cq_head, head, __ATOMIC_RELEASE);
    if (!got && !ended) { struct timespec w = { 0, 100000 }; nanosleep(&w, 0); }
  }
  close(er.fd); er.fd = -1;
}
#else
#define ET_RECV_PATH "recvfrom"
/* No io_uring here: the one receive path is recvfrom, fixed when this file is compiled. */
static int er_sock = -1;
static int er_open(int sock) { er_sock = sock; return 0; }
static ssize_t er_recv(void *buf, size_t cap, struct sockaddr_storage *from, socklen_t *fl) { *fl = sizeof *from; return recvfrom(er_sock, buf, cap, 0, (struct sockaddr *)from, fl); }
static void er_close(void) {}
#endif
typedef struct { struct sockaddr_storage a; socklen_t al; int used; uint32_t slot; } sess_t;
static sess_t sess[ET_MAX_USERS];
static uint8_t ents[ET_MAX_USERS][PKT];
static int64_t lat[1 << 16];
static int64_t now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec; }
static int cmp(const void *x, const void *y) { int64_t a = *(const int64_t *)x, b = *(const int64_t *)y; return (a > b) - (a < b); }
static int find(const struct sockaddr_storage *a, socklen_t al) {
  for (int i = 0; i < ET_MAX_USERS; i++)
    if (sess[i].used && sess[i].al == al && memcmp(&sess[i].a, a, al) == 0) return i;
  for (int i = 0; i < ET_MAX_USERS; i++)
    if (!sess[i].used) { sess[i].used = 1; sess[i].al = al; memcpy(&sess[i].a, a, al); memset(ents[i], 0, PKT); return i; }
  return -1;
}
static uint8_t dgs[ET_MAX_USERS][PKT * ET_VIEW];
static otel_t ot_store;
static et_out_t outs[ET_MAX_USERS];
int et_run(const char *host, uint16_t port, uint32_t ticks, uint32_t slow_ns, uint16_t *bound, et_report_t *out) {
  return et_run_with(host, port, ticks, slow_ns, bound, out, 0);
}
int et_run_with(const char *host, uint16_t port, uint32_t ticks, uint32_t slow_ns, uint16_t *bound, et_report_t *out,
                const et_io_t *io) {
  if (ticks == 0 || ticks > (1u << 16)) return -EINVAL;
  memset(sess, 0, sizeof sess); memset(out, 0, sizeof *out);
  /* An IPv6 literal gives a dual-stack socket, so IPv4 clients come as v4-mapped addresses; an
   * IPv4 literal gives an IPv4 socket as before. */
  struct sockaddr_storage a; memset(&a, 0, sizeof a);
  struct sockaddr_in6 *a6 = (struct sockaddr_in6 *)&a; struct sockaddr_in *a4 = (struct sockaddr_in *)&a;
  socklen_t al;
  int fd;
  if (inet_pton(AF_INET6, host, &a6->sin6_addr) == 1) {
    a6->sin6_family = AF_INET6; a6->sin6_port = htons(port); al = sizeof *a6;
    fd = socket(AF_INET6, SOCK_DGRAM, 0);
    if (fd >= 0) { int off = 0; setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof off); }
  } else if (inet_pton(AF_INET, host, &a4->sin_addr) == 1) {
    a4->sin_family = AF_INET; a4->sin_port = htons(port); al = sizeof *a4;
    fd = socket(AF_INET, SOCK_DGRAM, 0);
  } else return -EINVAL;
  if (fd < 0) return -errno;
  { socklen_t sl = al;
    if (bind(fd, (struct sockaddr *)&a, al) < 0 || getsockname(fd, (struct sockaddr *)&a, &sl) < 0) { int e = errno; close(fd); return -e; } }
  if (bound) *bound = ntohs(a.ss_family == AF_INET6 ? a6->sin6_port : a4->sin_port);
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  { int e = er_open(fd); if (e < 0) { close(fd); return e; } }
#ifdef __linux__
  out->ring = 1;
#endif
  if (io && io->open && io->open(fd) < 0) { close(fd); return -EIO; }
  /* Spans are recorded and flushed every run; the environment only says where they go, and /dev/null is a place. */
  otel_t *ot = &ot_store; int64_t run_t0 = now_ns();
  { const char *op = getenv("ENTITY_OTEL"); if (otel_open(ot, op && *op ? op : "/dev/null", 0) < 0) { int e = errno; er_close(); close(fd); return -e; } }
  uint64_t run_sid = otel_id(ot);
  int64_t next = now_ns() + ET_TICK_NS;
  for (uint32_t t = 0; t < ticks; t++) {
    struct timespec ts; ts.tv_sec = (time_t)(next / 1000000000LL); ts.tv_nsec = (long)(next % 1000000000LL);
#ifdef __linux__
    clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, 0);
#else
    { int64_t d = next - now_ns(); if (d > 0) { struct timespec r; r.tv_sec = (time_t)(d / 1000000000LL); r.tv_nsec = (long)(d % 1000000000LL); nanosleep(&r, 0); } }
#endif
    int64_t t0 = now_ns(), first = -1;
    uint32_t pk0 = out->packets_in, dg0 = out->datagrams_out;
    uint64_t tick_sid = otel_id(ot);
    otel_trace(ot);
    uint8_t buf[PKT + 8]; struct sockaddr_storage from; socklen_t fl;
    for (;;) {
      fl = sizeof from;
      ssize_t n = er_recv(buf, sizeof buf, &from, &fl);
      if (n < 0) break;
      if (first < 0) first = now_ns();
      if (n != PKT) continue;
      int i = find(&from, fl);
      if (i >= 0) { memcpy(ents[i], buf, PKT); out->packets_in++; }
    }
    int64_t tr = now_ns();
    if (slow_ns) { int64_t end = now_ns() + slow_ns; while (now_ns() < end) { } }
    int n = 0;
    for (int i = 0; i < ET_MAX_USERS; i++) {
      if (!sess[i].used) continue;
      int k = 0;
      for (int j = 0; j < ET_MAX_USERS && k < ET_VIEW; j++)
        if (sess[j].used) { memcpy(dgs[n] + (size_t)k * PKT, ents[j], PKT); k++; }
      outs[n].buf = dgs[n]; outs[n].len = (uint32_t)k * PKT; outs[n].to = &sess[i].a; outs[n].tolen = sess[i].al;
      n++;
    }
    int64_t tb = now_ns();
    if (io && io->send_all) out->datagrams_out += (uint32_t)io->send_all(fd, outs, n);
    else
      for (int i = 0; i < n; i++)
        if (sendto(fd, outs[i].buf, outs[i].len, 0, (const struct sockaddr *)outs[i].to, outs[i].tolen) >= 0) out->datagrams_out++;
    int64_t t1 = now_ns();
    {
      otel_rec_t *r;
      r = otel_begin(ot, otel_id(ot), "entity.recv", tick_sid, t0, tr); otel_num(r, "packets_in", (double)(out->packets_in - pk0));
      r = otel_begin(ot, otel_id(ot), "entity.build", tick_sid, tr, tb); otel_num(r, "clients", n);
      r = otel_begin(ot, otel_id(ot), "entity.send", tick_sid, tb, t1); otel_num(r, "datagrams_out", (double)(out->datagrams_out - dg0));
      r = otel_begin(ot, tick_sid, "entity.tick", 0, t0, t1); otel_kind(r, 2);
      otel_num(r, "clients", n); otel_num(r, "packets_in", (double)(out->packets_in - pk0));
      otel_num(r, "datagrams_out", (double)(out->datagrams_out - dg0)); otel_num(r, "tick_us", (double)(t1 - t0) / 1000.0);
      otel_num(r, "late_us", (double)(t0 - next) / 1000.0);
      otel_str(r, "recv", ET_RECV_PATH);
      if (t1 > next + ET_TICK_NS) otel_error(r, "TickOverrun", "the tick ran past its 15.6 ms slot");
      if (ot->n >= OTEL_FLUSH_AT && next - t1 > 3000000) otel_flush(ot);
    }
    lat[t] = t1 - (first >= 0 ? first : t0);
    if (t1 > next + ET_TICK_NS) { out->overruns++; next = t1 + ET_TICK_NS; } else next += ET_TICK_NS;
  }
  out->ticks = ticks;
  { static int64_t s[1 << 16]; memcpy(s, lat, (size_t)ticks * sizeof(int64_t));
    qsort(s, ticks, sizeof(int64_t), cmp);
    out->p50_ns = s[ticks / 2]; out->p95_ns = s[(ticks * 95u) / 100]; out->p99_ns = s[(ticks * 99u) / 100]; out->max_ns = s[ticks - 1]; }
  {
    otel_rec_t *r; otel_trace(ot);
    r = otel_begin(ot, run_sid, "entity.run", 0, run_t0, now_ns()); otel_kind(r, 2);
    otel_num(r, "ticks", ticks); otel_num(r, "packets_in", out->packets_in); otel_num(r, "datagrams_out", out->datagrams_out);
    otel_num(r, "overruns", out->overruns); otel_num(r, "p50_us", (double)out->p50_ns / 1000.0);
    otel_num(r, "p95_us", (double)out->p95_ns / 1000.0); otel_num(r, "p99_us", (double)out->p99_ns / 1000.0); otel_num(r, "max_us", (double)out->max_ns / 1000.0);
    otel_str(r, "recv", ET_RECV_PATH);
    if (out->overruns) otel_error(r, "TickOverrun", "ticks ran past their slot during the run");
  }
  out->rcvbuf_drops = -1;
#ifdef SO_MEMINFO
  { uint32_t mi[9]; socklen_t ml = sizeof mi; /* SK_MEMINFO_DROPS is index 8 */
    if (getsockopt(fd, SOL_SOCKET, SO_MEMINFO, mi, &ml) == 0 && ml >= sizeof mi) out->rcvbuf_drops = (int64_t)mi[8]; }
#endif
#ifdef __linux__
  out->rcvbuf_drops = er_overflow();
#endif
  otel_close(ot);
  er_close();
  if (io && io->close) io->close();
  close(fd);
  return 0;
}
