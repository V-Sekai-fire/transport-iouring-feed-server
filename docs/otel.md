# Spans from the entity server

`transports/csrc/otel_span.h` is the span half of the engine's `open_telemetry` module in C99, and the
guest `interactor-otel-guest` is the same module for scripts. A hot loop records a finished span into a
fixed ring of 4,096 records, which costs one struct copy and no allocation. `otel_flush` writes one
OTLP protobuf record, and the server calls it only in the idle part of a tick, when the ring is half full and more than 3 ms remain.
A full ring drops the span and counts it.

## Where the spans go

Every tick is its own trace: an `entity.tick` root span of kind server, with children `entity.recv`, `entity.build`
and `entity.send` that split the tick into its phases. The root carries the numbers `clients`, `packets_in`,
`datagrams_out`, `tick_us` and `late_us` (how late the tick began against its slot) and the string `recv`, the
receive path (`io_uring` or `recvfrom`). A tick that ran past its slot has status error and an exception event
`TickOverrun`, which a collector lists as an error. Each run also records an `entity.run` span with the totals and
the `p50_us`, `p95_us`, `p99_us` and `max_us` of the ticks.

Attributes are typed because a collector treats the types differently: a double is something to average and graph,
a string or an integer is something to group by. So the measurements are doubles, the categories are strings, and
integers are kept for identifiers. The optional parts of the protocol are written every time, so no reader depends
on a default: the span's trace state, kind, flags, status (OK unless an error) and its three dropped counts, the
event's dropped count, and the scope's version, dropped count and schema URL. Links are the one field left out,
because an empty list has no encoding.

There is no switch: `ENTITY_OTEL=<file>` names the file and defaults to `/dev/null`, which is a place, so the work
is the same whether anyone reads it. A file that cannot be opened stops the run with an error. Spans are flushed in
the idle part of a tick, 256 records at a time and only when more than 3 ms remain.

A record is a 4-byte little-endian length and one `ScopeSpans` message. The resource, which may hold a key, is
left out on purpose, so no key is written to disk.

## What the collector wants

AppSignal's collector takes OTLP over HTTP as protobuf and reads its push key from the resource attribute
`appsignal.config.push_api_key`, not from a header. `tools/otlp_post_pb.exs` of `interactor-otel-guest` wraps
each record in a `ResourceSpans` with a resource it builds from its own environment and posts it as
`application/x-protobuf` to the collector's `/v1/traces`.

## Check

`transports/test/otel_span_witness.cpp` holds the properties, built on witness-cpp. Every property reads the
records back with an independent decoder: each field of every span matches what was recorded, including the
typed attributes, the per-span trace id, the error status and event, and every optional field; span ids are never
zero and never repeat; and a full ring drops and counts. Two planted-false properties must be caught. Three more
builds plant a defect each, `-DOTEL_PLANT_PBTIMES` (the end time written as the start), `-DOTEL_PLANT_PBPARENT`
(the parent id left out) and `-DOTEL_PLANT_PBSTATUS` (the status left out); CTest runs them with `WILL_FAIL`.

    cmake -S transports/test -B build && cmake --build build && ctest --test-dir build
