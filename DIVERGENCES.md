# Divergences from the Rust sample (rust-iouring-kensac)

Rule: translate first, modify later. Each line is something that is not yet a translation.

Behaviour: deploy/scripts/diff.sh sends 101 raw requests to the Rust binary and to this port on copies of the same seed. 101 of 101 replies are byte-identical (only timestamps and uptime masked).

Structure, not yet translated from src/main.rs:
- one ring; the sample's multiple rings with spill (MAX_RINGS) are missing
- DONE: multishot recv with one provided-buffer ring (BUF_COUNT 1024 x BUF_SIZE 2048), buffers returned after the bytes are copied out
- still different: ENOBUFS re-arms the receive at once; the sample queues a single-shot OP_READ (read queue, READS_PER_PASS) and re-arms after the socket drains
- one listener; the sample uses 8 SO_REUSEPORT listeners and ACCEPT_DEPTH accepts
- accepted sockets are plain fds; the sample uses direct descriptors (register_files_sparse)
- wait is a fixed 1 ms (a quarter of a 4 ms tick); the sample waits for WAIT_MIN completions or WAIT_NS
- no READS_PER_PASS / ANSWERS_PER_PASS budgets, ready list or read queue
- no output buffer pool (POOL_MAX, OUT_CAP); responses are built by ByteArray append
- Content-Length is written once from the body size; the sample reserves a 4-digit slot and fixes it up
- user_data keeps a 24-bit generation; the sample's is 16 bits

Libraries replaced: sha2 crate -> src Sha256.lean (RFC vectors pass); memchr -> findFrom.

Edits to copied files: datasources/csrc/shim.c has one #ifdef __linux__ guard around setpriority(SYS_gettid); datasources/csrc/lean_glue.c carries an fcntl64 stub for Lean's older glibc.

Per-request cost (profiles/README.md): allocation (lean_alloc_small_object_core, mi_free), List-based escaping (escByte, List.foldl) and ByteArray copies dominate the Lean side.

Entity server: the io_uring and SQPOLL send paths were removed. CI showed no difference from plain syscalls at 64 and 256 clients (docs/tick-budget.md). The feed server's io_uring loop is still here.
Feed server: the io_uring event loop (ring_glue.c) was replaced by a poll(2) loop (poll_glue.c) with the same Lean-facing contract. RFD66606-2360 ships one win64 build, where io_uring cannot run, and the entity-server sweep found no difference between plain syscalls and io_uring at 64 and 256 clients (docs/tick-budget.md). Multishot receive and provided buffers are gone with it. RFD66606-2360 now allows an io_uring path beside the portable one (operator, 2026-11-12); the win64 build keeps poll. The removed code is in git history before commit 7a350f6.
