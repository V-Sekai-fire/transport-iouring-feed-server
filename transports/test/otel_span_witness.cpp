// Properties of the span emitter (csrc/otel_span.h), each beside a planted-false control. The emitter writes
// one format, so every property reads the protobuf back with a reader that shares no code with it.
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "witness/doctest.h"

#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" {
#include "../csrc/otel_span.h"
}

namespace {

struct Att {
	int kind = 2; // 1 int, 2 double, 3 string
	double d = 0;
	int64_t i = 0;
	int s = 0;
};

struct Item {
	int name = 0, kind = 1;
	bool child = false, trace = false, err = false;
	int64_t t0 = 0, t1 = 0;
	std::vector<Att> att;
};

struct Case {
	std::vector<Item> items;
};

const char *const NAMES[4] = { "entity.tick", "entity.run", "relay.pass", "a.b" };
const char *const KEYS[OTEL_ATTRS] = { "clients", "packets_in", "datagrams_out", "tick_us", "recv", "k", "n", "late_us" };
const char *const STRS[3] = { "io_uring", "recvfrom", "a string with spaces, \"quotes\" and a \\ and \xc3\xa9" };

Case gen_case(witness::RNG &rng, const witness::Level &lvl) {
	Case c;
	uint32_t n = rng.uint_range(0, static_cast<uint32_t>(lvl.fin_bound / 8));
	for (uint32_t i = 0; i < n; ++i) {
		Item it;
		it.name = rng.int_range(0, 3);
		it.kind = rng.int_range(1, 5);
		it.child = i > 0 && rng.int_range(0, 1) == 1;
		it.trace = !it.child && rng.int_range(0, 1) == 1;
		it.err = rng.int_range(0, 4) == 0;
		it.t0 = rng.int64_range(0, 4000000000000LL);
		it.t1 = it.t0 + rng.int64_range(0, 9000000);
		uint32_t na = rng.uint_range(0, OTEL_ATTRS + 2);
		for (uint32_t k = 0; k < na; ++k) {
			Att a;
			a.kind = rng.int_range(1, 3);
			a.d = rng.float_range(-1e9, 1e9);
			a.i = rng.int64_range(-1000000000000LL, 1000000000000LL);
			a.s = rng.int_range(0, 2);
			it.att.push_back(a);
		}
		c.items.push_back(it);
	}
	return c;
}

otel_t *store() {
	static otel_t *o = new otel_t;
	return o;
}

// Runs the emitter on a case and returns the bytes it wrote in one flush. traces gets the trace id each span was recorded under.
std::string emit(const Case &c, std::vector<uint64_t> *ids, std::vector<std::string> *traces, uint32_t *dropped, uint32_t *left) {
	otel_t *o = store();
	char path[] = "/tmp/otel_witness_XXXXXX";
	int fd = mkstemp(path);
	close(fd);
	otel_open(o, path, 7);
	std::vector<uint64_t> local;
	std::vector<uint64_t> &seen = ids ? *ids : local;
	for (const Item &it : c.items) {
		if (it.trace) {
			otel_trace(o);
		}
		uint64_t parent = (it.child && !seen.empty()) ? seen.back() : 0;
		uint64_t sid = otel_id(o);
		otel_rec_t *r = otel_begin(o, sid, NAMES[it.name], parent, it.t0, it.t1);
		otel_kind(r, static_cast<uint8_t>(it.kind));
		for (size_t k = 0; k < it.att.size(); ++k) {
			const Att &a = it.att[k];
			if (a.kind == 1) {
				otel_int(r, KEYS[k % OTEL_ATTRS], a.i);
			} else if (a.kind == 2) {
				otel_num(r, KEYS[k % OTEL_ATTRS], a.d);
			} else {
				otel_str(r, KEYS[k % OTEL_ATTRS], STRS[a.s]);
			}
		}
		if (it.err) {
			otel_error(r, "TickOverrun", "the tick ran past its slot");
		}
		seen.push_back(sid);
		if (traces) {
			uint8_t id[16];
			otel_pb_be(id, o->tid_hi);
			otel_pb_be(id + 8, o->tid_lo);
			traces->push_back(std::string(reinterpret_cast<char *>(id), 16));
		}
	}
	otel_flush(o);
	if (dropped) {
		*dropped = o->dropped;
	}
	if (left) {
		*left = o->n;
	}
	otel_close(o);
	std::string text;
	if (FILE *f = fopen(path, "r")) {
		char buf[65536];
		size_t n;
		while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
			text.append(buf, n);
		}
		fclose(f);
	}
	remove(path);
	return text;
}

struct PbAtt {
	std::string key, s;
	int kind = 0;
	uint64_t bits = 0;
};

struct PbSpan {
	std::string trace, sid, parent, name, status_msg, ev_name, ev_type, ev_msg;
	bool has_state = false, has_status = false, has_event = false, has_dropped_attrs = false, has_dropped_events = false, has_dropped_links = false, has_flags = false;
	uint64_t kind = 0, t0 = 0, t1 = 0, status_code = 0, flags = 0, ev_time = 0, ev_dropped = 99;
	std::vector<PbAtt> attrs;
};

struct Rd {
	const uint8_t *p, *e;
	bool varint(uint64_t *v) {
		*v = 0;
		for (int sh = 0; p < e && sh < 70; sh += 7) {
			uint8_t b = *p++;
			*v |= static_cast<uint64_t>(b & 0x7F) << sh;
			if (!(b & 0x80)) {
				return true;
			}
		}
		return false;
	}
	bool bytes(std::string *s) {
		uint64_t n;
		if (!varint(&n) || n > static_cast<uint64_t>(e - p)) {
			return false;
		}
		s->assign(reinterpret_cast<const char *>(p), static_cast<size_t>(n));
		p += n;
		return true;
	}
	bool fixed(uint64_t *v, int bytes_n) {
		if (e - p < bytes_n) {
			return false;
		}
		*v = 0;
		for (int i = 0; i < bytes_n; i++) {
			*v |= static_cast<uint64_t>(p[i]) << (8 * i);
		}
		p += bytes_n;
		return true;
	}
};

Rd rd(const std::string &m) { return Rd{ reinterpret_cast<const uint8_t *>(m.data()), reinterpret_cast<const uint8_t *>(m.data()) + m.size() }; }

bool dec_attr(const std::string &m, PbAtt *out) {
	Rd r = rd(m);
	while (r.p < r.e) {
		uint64_t tag;
		std::string s;
		if (!r.varint(&tag) || (tag & 7) != 2 || !r.bytes(&s)) {
			return false;
		}
		if (tag == 0x0A) {
			out->key = s;
		} else if (tag == 0x12) {
			Rd a = rd(s);
			uint64_t t;
			if (!a.varint(&t)) {
				return false;
			}
			if (t == 0x0A) {
				out->kind = 3;
				if (!a.bytes(&out->s)) {
					return false;
				}
			} else if (t == 0x18) {
				out->kind = 1;
				if (!a.varint(&out->bits)) {
					return false;
				}
			} else if (t == 0x21) {
				out->kind = 2;
				if (!a.fixed(&out->bits, 8)) {
					return false;
				}
			} else {
				return false;
			}
			if (a.p != a.e) {
				return false;
			}
		} else {
			return false;
		}
	}
	return true;
}

bool dec_event(const std::string &m, PbSpan *o) {
	Rd r = rd(m);
	o->has_event = true;
	while (r.p < r.e) {
		uint64_t tag;
		std::string s;
		if (!r.varint(&tag)) {
			return false;
		}
		if (tag == 0x09) {
			if (!r.fixed(&o->ev_time, 8)) return false;
		} else if (tag == 0x12) {
			if (!r.bytes(&o->ev_name)) return false;
		} else if (tag == 0x1A) {
			PbAtt a;
			if (!r.bytes(&s) || !dec_attr(s, &a)) return false;
			(a.key == "exception.type" ? o->ev_type : o->ev_msg) = a.s;
		} else if (tag == 0x20) {
			if (!r.varint(&o->ev_dropped)) return false;
		} else {
			return false;
		}
	}
	return true;
}

bool dec_span(const std::string &m, PbSpan *o) {
	Rd r = rd(m);
	while (r.p < r.e) {
		uint64_t tag, v;
		std::string s;
		if (!r.varint(&tag)) {
			return false;
		}
		switch (tag) {
			case 0x0A: if (!r.bytes(&o->trace)) return false; break;
			case 0x12: if (!r.bytes(&o->sid)) return false; break;
			case 0x1A: if (!r.bytes(&s) || !s.empty()) return false; o->has_state = true; break;
			case 0x22: if (!r.bytes(&o->parent)) return false; break;
			case 0x2A: if (!r.bytes(&o->name)) return false; break;
			case 0x30: if (!r.varint(&o->kind)) return false; break;
			case 0x39: if (!r.fixed(&o->t0, 8)) return false; break;
			case 0x41: if (!r.fixed(&o->t1, 8)) return false; break;
			case 0x4A: {
				PbAtt a;
				if (!r.bytes(&s) || !dec_attr(s, &a)) return false;
				o->attrs.push_back(a);
				break;
			}
			case 0x50: if (!r.varint(&v) || v != 0) return false; o->has_dropped_attrs = true; break;
			case 0x5A: if (!r.bytes(&s) || !dec_event(s, o)) return false; break;
			case 0x60: if (!r.varint(&v) || v != 0) return false; o->has_dropped_events = true; break;
			case 0x70: if (!r.varint(&v) || v != 0) return false; o->has_dropped_links = true; break;
			case 0x7A: {
				if (!r.bytes(&s)) return false;
				Rd st = rd(s);
				while (st.p < st.e) {
					uint64_t t;
					if (!st.varint(&t)) return false;
					if (t == 0x12) { if (!st.bytes(&o->status_msg)) return false; }
					else if (t == 0x18) { if (!st.varint(&o->status_code)) return false; }
					else return false;
				}
				o->has_status = true;
				break;
			}
			case 133: if (!r.fixed(&o->flags, 4)) return false; o->has_flags = true; break;
			default: return false;
		}
	}
	return true;
}

// Reads every record; false if any byte is left over or malformed, or the scope lacks its version, count or schema URL.
bool dec_all(const std::string &t, std::vector<PbSpan> *out) {
	size_t at = 0;
	while (at < t.size()) {
		if (t.size() - at < 4) {
			return false;
		}
		uint32_t len = static_cast<uint8_t>(t[at]) | static_cast<uint8_t>(t[at + 1]) << 8 | static_cast<uint8_t>(t[at + 2]) << 16 | static_cast<uint32_t>(static_cast<uint8_t>(t[at + 3])) << 24;
		at += 4;
		if (len > t.size() - at) {
			return false;
		}
		Rd r{ reinterpret_cast<const uint8_t *>(t.data()) + at, reinterpret_cast<const uint8_t *>(t.data()) + at + len };
		at += len;
		bool scoped = false, schema = false;
		while (r.p < r.e) {
			uint64_t tag;
			std::string s;
			if (!r.varint(&tag) || (tag & 7) != 2 || !r.bytes(&s)) {
				return false;
			}
			if (tag == 0x0A) {
				scoped = s == std::string("\x0A\x0D" "entity-server" "\x12\x01" "1" "\x20\x00", 20);
			} else if (tag == 0x12) {
				PbSpan sp;
				if (!dec_span(s, &sp)) {
					return false;
				}
				out->push_back(sp);
			} else if (tag == 0x1A) {
				schema = s == OTEL_SCHEMA;
			} else {
				return false;
			}
		}
		if (!scoped || !schema) {
			return false;
		}
	}
	return true;
}

} // namespace

TEST_CASE("[otel] the records decode to the spans that were recorded, every field and every optional field") {
	witness::Generator<Case> gen = &gen_case;
	std::function<bool(const Case &)> pred = [](const Case &c) {
		std::vector<uint64_t> ids;
		std::vector<std::string> traces;
		std::string t = emit(c, &ids, &traces, nullptr, nullptr);
		std::vector<PbSpan> got;
		if (!dec_all(t, &got)) {
			return false;
		}
		size_t want = c.items.size() < OTEL_CAP ? c.items.size() : OTEL_CAP;
		if (got.size() != want) {
			return false;
		}
		int64_t off = store()->wall_off;
		for (size_t i = 0; i < want; ++i) {
			const Item &it = c.items[i];
			const PbSpan &g = got[i];
			uint8_t id[8];
			otel_pb_be(id, ids[i]);
			bool root = !(it.child && i > 0);
			uint8_t pid[8] = {};
			if (!root) {
				otel_pb_be(pid, ids[i - 1]);
			}
			size_t na = it.att.size() > OTEL_ATTRS ? OTEL_ATTRS : it.att.size();
			if (g.trace != traces[i] || g.sid != std::string(reinterpret_cast<char *>(id), 8) || g.name != NAMES[it.name] || g.kind != static_cast<uint64_t>(it.kind) ||
					g.t0 != static_cast<uint64_t>(it.t0 + off) || g.t1 != static_cast<uint64_t>(it.t1 + off) || g.attrs.size() != na ||
					(root ? !g.parent.empty() : g.parent != std::string(reinterpret_cast<char *>(pid), 8))) {
				return false;
			}
			// the optional parts are always there
			if (!g.has_state || !g.has_status || !g.has_flags || g.flags != 0x101 || !g.has_dropped_attrs || !g.has_dropped_events || !g.has_dropped_links) {
				return false;
			}
			if (it.err ? (g.status_code != 2 || g.status_msg != "the tick ran past its slot" || !g.has_event || g.ev_name != "exception" || g.ev_type != "TickOverrun" ||
								 g.ev_msg != "the tick ran past its slot" || g.ev_time != static_cast<uint64_t>(it.t1 + off) || g.ev_dropped != 0)
						: (g.status_code != 1 || !g.status_msg.empty() || g.has_event)) {
				return false;
			}
			for (size_t k = 0; k < na; ++k) {
				const Att &a = it.att[k];
				const PbAtt &p = g.attrs[k];
				if (p.key != KEYS[k % OTEL_ATTRS] || p.kind != a.kind) {
					return false;
				}
				if (a.kind == 1 && p.bits != static_cast<uint64_t>(a.i)) {
					return false;
				}
				if (a.kind == 2) {
					double d;
					memcpy(&d, &p.bits, 8);
					if (d != a.d) {
						return false;
					}
				}
				if (a.kind == 3 && p.s != std::string(STRS[a.s]).substr(0, OTEL_STR_MAX)) {
					return false;
				}
			}
		}
		return true;
	};
	witness::Trial t = witness::resolve<Case>("roundtrip", gen, pred);
	INFO(t.message);
	CHECK(t.outcome != witness::Outcome::FOUND);
}

TEST_CASE("[otel] span ids are never zero and never repeat, and every trace id is non-zero") {
	witness::Generator<Case> gen = &gen_case;
	std::function<bool(const Case &)> pred = [](const Case &c) {
		std::string t = emit(c, nullptr, nullptr, nullptr, nullptr);
		std::vector<PbSpan> got;
		if (!dec_all(t, &got)) {
			return false;
		}
		std::vector<std::string> seen;
		for (const PbSpan &g : got) {
			if (g.sid == std::string(8, '\0') || g.trace == std::string(16, '\0')) {
				return false;
			}
			for (const std::string &x : seen) {
				if (x == g.sid) {
					return false;
				}
			}
			seen.push_back(g.sid);
		}
		return true;
	};
	witness::Trial t = witness::resolve<Case>("ids distinct", gen, pred);
	INFO(t.message);
	CHECK(t.outcome != witness::Outcome::FOUND);
}

TEST_CASE("[otel] a full ring drops and counts, and a flush empties it") {
	witness::Generator<Case> gen = [](witness::RNG &rng, const witness::Level &) {
		Case c;
		c.items.resize(static_cast<uint32_t>(OTEL_CAP) - 8 + rng.uint_range(0, 16));
		return c;
	};
	std::function<bool(const Case &)> pred = [](const Case &c) {
		uint32_t dropped = 99, left = 99;
		std::string t = emit(c, nullptr, nullptr, &dropped, &left);
		std::vector<PbSpan> got;
		size_t n = c.items.size();
		return dec_all(t, &got) && dropped == (n > OTEL_CAP ? n - OTEL_CAP : 0) && left == 0 && got.size() == (n < OTEL_CAP ? n : OTEL_CAP);
	};
	witness::Trial t = witness::resolve<Case>("ring bounds", gen, pred);
	INFO(t.message);
	CHECK(t.outcome != witness::Outcome::FOUND);
}

TEST_CASE("[otel] falsifiability: a flush that leaves spans queued is planted false and is caught") {
	witness::Generator<Case> gen = &gen_case;
	std::function<bool(const Case &)> pred = [](const Case &c) {
		uint32_t left = 0;
		emit(c, nullptr, nullptr, nullptr, &left);
		return left != 0 || c.items.empty();
	};
	witness::Trial t = witness::resolve<Case>("flush leaves spans", gen, pred);
	INFO(t.message);
	CHECK(t.outcome == witness::Outcome::FOUND);
}

TEST_CASE("[otel] falsifiability: every measurement being an integer is planted false and is caught") {
	witness::Generator<Case> gen = &gen_case;
	std::function<bool(const Case &)> pred = [](const Case &c) {
		std::vector<PbSpan> got;
		if (!dec_all(emit(c, nullptr, nullptr, nullptr, nullptr), &got)) {
			return false;
		}
		for (const PbSpan &g : got) {
			for (const PbAtt &a : g.attrs) {
				if (a.kind != 1) {
					return false;
				}
			}
		}
		return true;
	};
	witness::Trial t = witness::resolve<Case>("all attributes are integers", gen, pred);
	INFO(t.message);
	CHECK(t.outcome == witness::Outcome::FOUND);
}
