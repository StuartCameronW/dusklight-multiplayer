#include "world_state.hpp"

#include <algorithm>
#include <bit>
#include <limits>
#include <utility>

namespace dusk::mp::world {
namespace {

/// write_view's layout version. Bump it whenever the field order or any array size changes, so a
/// mismatched peer fails the read instead of installing shifted bytes.
constexpr std::uint8_t kViewFormatVersion = 1;
/// write_temp_mirrors' layout version (the WorldSync trailer); same rule as kViewFormatVersion.
constexpr std::uint8_t kTempFormatVersion = 1;

constexpr std::int32_t kRupeeMax = 9999;
constexpr std::int32_t kMaxLifeMax = 400;
constexpr std::int32_t kPohMax = 255;
constexpr std::int32_t kKeyNumMax = 99;

bool is_known_domain(std::uint8_t d) {
    return d <= static_cast<std::uint8_t>(Domain::Zone) ||
           (d >= static_cast<std::uint8_t>(Domain::Rupee) &&
               d <= static_cast<std::uint8_t>(Domain::Poh));
}

std::int32_t clamp_i32(std::int64_t v, std::int32_t lo, std::int32_t hi) {
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(v, lo, hi));
}

std::int16_t clamp_delta(std::int64_t d) {
    return static_cast<std::int16_t>(std::clamp<std::int64_t>(
        d, std::numeric_limits<std::int16_t>::min(), std::numeric_limits<std::int16_t>::max()));
}

void emit_bit_op(Domain d, std::uint8_t tbl, std::size_t index, std::uint8_t oldB,
    std::uint8_t newB, std::vector<WorldOp>& out) {
    if (oldB == newB) {
        return;
    }
    WorldOp op;
    op.domain = d;
    op.tbl = tbl;
    op.index = static_cast<std::uint16_t>(index);
    op.set = static_cast<std::uint8_t>(newB & ~oldB);
    op.clr = static_cast<std::uint8_t>(oldB & ~newB);
    out.push_back(op);
}

void emit_counter_op(
    Domain d, std::uint8_t tbl, std::int32_t oldV, std::int32_t newV, std::vector<WorldOp>& out) {
    if (oldV == newV) {
        return;
    }
    WorldOp op;
    op.domain = d;
    op.tbl = tbl;
    op.delta = clamp_delta(static_cast<std::int64_t>(newV) - oldV);
    op.resultAbs = newV;
    out.push_back(op);
}

/// The byte a (validated) bit op addresses in `v`.
std::uint8_t& bit_byte(WorldView& v, const WorldOp& op) {
    switch (op.domain) {
    case Domain::MemBit:
        return v.mem[op.tbl][op.index];
    case Domain::Mem2:
        return v.mem2[op.tbl][op.index];
    case Domain::FirstGet:
        return v.firstGet[op.index];
    case Domain::Items:
        return v.player[op.index];
    case Domain::Event:
    default:
        return v.event[op.index];
    }
}

void apply_bits(std::uint8_t& b, const WorldOp& op) {
    b = static_cast<std::uint8_t>((b | op.set) & ~op.clr);
}

/// Reads or writes the counter a (validated) counter op addresses, clamped to its legal range.
std::int32_t counter_value(const WorldView& v, const WorldOp& op) {
    switch (op.domain) {
    case Domain::Rupee:
        return v.rupee;
    case Domain::MaxLife:
        return v.maxLife;
    case Domain::Poh:
        return v.poh;
    case Domain::KeyNum:
    default:
        return v.key_num(op.tbl);
    }
}

void set_counter(WorldView& v, const WorldOp& op, std::int64_t value) {
    switch (op.domain) {
    case Domain::Rupee:
        v.rupee = clamp_i32(value, 0, kRupeeMax);
        break;
    case Domain::MaxLife:
        v.maxLife = clamp_i32(value, 0, kMaxLifeMax);
        break;
    case Domain::Poh:
        v.poh = clamp_i32(value, 0, kPohMax);
        break;
    case Domain::KeyNum:
    default:
        v.set_key_num(op.tbl, clamp_i32(value, 0, kKeyNumMax));
        break;
    }
}

template <std::size_t N>
void write_bytes(Writer& w, const std::array<std::uint8_t, N>& a) {
    for (const std::uint8_t b : a) {
        w.write_u8(b);
    }
}

template <std::size_t N>
bool read_bytes(Reader& r, std::array<std::uint8_t, N>& a) {
    for (std::uint8_t& b : a) {
        if (!r.read_u8(b)) {
            return false;
        }
    }
    return true;
}

struct Fnv1a {
    std::uint32_t h = 2166136261u;
    void byte(std::uint8_t b) {
        h ^= b;
        h *= 16777619u;
    }
    template <std::size_t N>
    void bytes(const std::array<std::uint8_t, N>& a) {
        for (const std::uint8_t b : a) {
            byte(b);
        }
    }
    void s32_le(std::int32_t v) {
        const auto u = static_cast<std::uint32_t>(v);
        byte(static_cast<std::uint8_t>(u));
        byte(static_cast<std::uint8_t>(u >> 8));
        byte(static_cast<std::uint8_t>(u >> 16));
        byte(static_cast<std::uint8_t>(u >> 24));
    }
};

}  // namespace

// --- ops -----------------------------------------------------------------------------------------

void write_op(Writer& w, const WorldOp& op) {
    w.write_u8(static_cast<std::uint8_t>(op.domain));
    w.write_u8(op.tbl);
    if (is_counter(op.domain)) {
        w.write_s16(op.delta);
        w.write_s32(op.resultAbs);
    } else {
        w.write_u16(op.index);
        w.write_u8(op.set);
        w.write_u8(op.clr);
    }
}

bool read_op(Reader& r, WorldOp& out) {
    std::uint8_t domain = 0;
    if (!r.read_u8(domain) || !is_known_domain(domain)) {
        return false;
    }
    WorldOp op;
    op.domain = static_cast<Domain>(domain);
    if (!r.read_u8(op.tbl)) {
        return false;
    }
    if (is_counter(op.domain)) {
        if (!r.read_s16(op.delta) || !r.read_s32(op.resultAbs)) {
            return false;
        }
    } else {
        if (!r.read_u16(op.index) || !r.read_u8(op.set) || !r.read_u8(op.clr)) {
            return false;
        }
    }
    out = op;
    return true;
}

void write_ops(Writer& w, const std::vector<WorldOp>& ops) {
    // The reader refuses more than kMaxOpsPerMessage; chunking a big diff is the caller's job. The
    // u16 clamp only guards the count field itself and can never be reached by a real view diff.
    const std::size_t n = std::min<std::size_t>(ops.size(), 0xFFFF);
    w.write_u16(static_cast<std::uint16_t>(n));
    for (std::size_t i = 0; i < n; ++i) {
        write_op(w, ops[i]);
    }
}

bool read_ops(Reader& r, std::vector<WorldOp>& out) {
    out.clear();
    std::uint16_t n = 0;
    if (!r.read_u16(n) || n > kMaxOpsPerMessage) {
        return false;
    }
    out.reserve(n);
    for (std::uint16_t i = 0; i < n; ++i) {
        WorldOp op;
        if (!read_op(r, op)) {
            out.clear();
            return false;
        }
        out.push_back(op);
    }
    return true;
}

// --- diff / apply --------------------------------------------------------------------------------

void diff(const WorldView& shadow, const WorldView& cur, std::vector<WorldOp>& out) {
    for (std::size_t i = 0; i < kEventBytes; ++i) {
        emit_bit_op(Domain::Event, kNoTbl, i, shadow.event[i], cur.event[i], out);
    }
    for (std::size_t t = 0; t < kMemTbls; ++t) {
        for (std::size_t i = 0; i < kMemBitBytes; ++i) {
            // mKeyNum is a counter; it travels as a KeyNum op below, never as bits.
            if (i == kMemKeyNumOffset) {
                continue;
            }
            emit_bit_op(Domain::MemBit, static_cast<std::uint8_t>(t), i, shadow.mem[t][i],
                cur.mem[t][i], out);
        }
    }
    for (std::size_t t = 0; t < kMem2Tbls; ++t) {
        for (std::size_t i = 0; i < kMem2Bytes; ++i) {
            emit_bit_op(Domain::Mem2, static_cast<std::uint8_t>(t), i, shadow.mem2[t][i],
                cur.mem2[t][i], out);
        }
    }
    for (std::size_t i = 0; i < kFirstGetBytes; ++i) {
        emit_bit_op(Domain::FirstGet, kNoTbl, i, shadow.firstGet[i], cur.firstGet[i], out);
    }
    for (std::size_t i = 0; i < kPlayerBytes; ++i) {
        emit_bit_op(Domain::Items, kNoTbl, i, shadow.player[i], cur.player[i], out);
    }

    emit_counter_op(Domain::Rupee, kNoTbl, shadow.rupee, cur.rupee, out);
    for (std::size_t t = 0; t < kMemTbls; ++t) {
        // Table 6's key count is rewritten by every warp on both machines (see kLocalOnlyKeyTbl).
        if (t == kLocalOnlyKeyTbl) {
            continue;
        }
        const auto tbl = static_cast<std::uint8_t>(t);
        emit_counter_op(Domain::KeyNum, tbl, shadow.key_num(tbl), cur.key_num(tbl), out);
    }
    emit_counter_op(Domain::MaxLife, kNoTbl, shadow.maxLife, cur.maxLife, out);
    emit_counter_op(Domain::Poh, kNoTbl, shadow.poh, cur.poh, out);
}

bool op_in_range(const WorldOp& op) {
    switch (op.domain) {
    case Domain::Event:
        return op.tbl == kNoTbl && op.index < kEventBytes;
    case Domain::MemBit:
        return op.tbl < kMemTbls && op.index < kMemBitBytes && op.index != kMemKeyNumOffset;
    case Domain::Mem2:
        return op.tbl < kMem2Tbls && op.index < kMem2Bytes;
    case Domain::FirstGet:
        return op.tbl == kNoTbl && op.index < kFirstGetBytes;
    case Domain::Items:
        return op.tbl == kNoTbl && op.index < kPlayerBytes;
    case Domain::Dan:
        // Not saved state; the stage table is the dan stage number (a save table, 0..31).
        return op.tbl < kMemTbls && (op.index >> 8) == 0 && temp_byte(op.index) < kDanBytes;
    case Domain::Zone:
        // index = room << 8 | byte, plus kTempMirrorOnly.
        return op.tbl < kMemTbls && zone_room(op.index) < kZoneRooms &&
               temp_byte(op.index) < kZoneBytes;
    case Domain::KeyNum:
        return op.tbl < kMemTbls && op.tbl != kLocalOnlyKeyTbl;
    case Domain::Rupee:
    case Domain::MaxLife:
    case Domain::Poh:
        return op.tbl == kNoTbl;
    }
    return false;
}

bool apply_op(WorldView& view, WorldView& shadow, const WorldOp& op, bool useAbsolute) {
    // Dan/Zone are not in the WorldView: they go to a TempMirror / TempView (apply_temp_op).
    if (is_temp(op.domain) || !op_in_range(op)) {
        return false;
    }
    if (is_counter(op.domain)) {
        const std::int64_t next = useAbsolute ?
                                      static_cast<std::int64_t>(op.resultAbs) :
                                      static_cast<std::int64_t>(counter_value(view, op)) + op.delta;
        // The same resulting value goes into both, so the next diff sees no change here.
        set_counter(view, op, next);
        set_counter(shadow, op, next);
    } else {
        apply_bits(bit_byte(view, op), op);
        apply_bits(bit_byte(shadow, op), op);
    }
    return true;
}

// --- host-side proposal limits -------------------------------------------------------------------

ProposalVerdict judge_proposal(const std::vector<WorldOp>& ops) {
    ProposalVerdict v;
    for (const WorldOp& op : ops) {
        if (!op_in_range(op)) {
            ++v.badOps;
            continue;
        }
        if (is_counter(op.domain)) {
            continue;
        }
        const auto touched =
            static_cast<std::uint32_t>(std::popcount(static_cast<std::uint8_t>(op.set | op.clr)));
        if (is_temp(op.domain)) {
            // Never saved (see world_state.hpp), so not counted against the save's limit.
            v.tempBitsTouched += touched;
            continue;
        }
        v.bitsTouched += touched;
        if (op.domain == Domain::Event) {
            v.eventClears += static_cast<std::uint32_t>(std::popcount(op.clr));
        }
    }
    v.accept = v.badOps == 0 && v.bitsTouched <= kMaxProposalBits &&
               v.eventClears <= kMaxProposalEventClears &&
               v.tempBitsTouched <= kMaxProposalTempBits;
    return v;
}

// --- temporary domains (Dan, Zone) ---------------------------------------------------------------

void diff_temp(const TempView& shadow, const TempView& cur, std::vector<WorldOp>& out) {
    if (cur.tbl < 0 || shadow.tbl != cur.tbl) {
        return;
    }
    const auto tbl = static_cast<std::uint8_t>(cur.tbl);
    for (std::size_t i = 0; i < kDanBytes; ++i) {
        emit_bit_op(Domain::Dan, tbl, i, shadow.dan[i], cur.dan[i], out);
    }
    const std::uint64_t both = shadow.rooms & cur.rooms;
    for (std::size_t room = 0; room < kZoneRooms; ++room) {
        if ((both >> room & 1) == 0) {
            continue;
        }
        for (std::size_t i = 0; i < kZoneBytes; ++i) {
            if (shadow.zone[room][i] != cur.zone[room][i]) {
                emit_bit_op(Domain::Zone, tbl, zone_index(room, i), shadow.zone[room][i],
                    cur.zone[room][i], out);
            }
        }
    }
}

bool apply_temp_op(TempMirror& m, const WorldOp& op) {
    if (!is_temp(op.domain) || !op_in_range(op)) {
        return false;
    }
    if (op.domain == Domain::Dan) {
        apply_bits(m.dan[temp_byte(op.index)], op);
    } else {
        const std::uint8_t room = zone_room(op.index);
        apply_bits(m.zone[room][temp_byte(op.index)], op);
        m.rooms |= std::uint64_t{1} << room;
    }
    return true;
}

bool apply_temp_op(TempView& v, const WorldOp& op) {
    if (!is_temp(op.domain) || !op_in_range(op) || v.tbl < 0 ||
        static_cast<std::uint8_t>(v.tbl) != op.tbl)
    {
        return false;
    }
    if (op.domain == Domain::Dan) {
        apply_bits(v.dan[temp_byte(op.index)], op);
        return true;
    }
    const std::uint8_t room = zone_room(op.index);
    if ((v.rooms >> room & 1) == 0) {
        return false;
    }
    apply_bits(v.zone[room][temp_byte(op.index)], op);
    return true;
}

void announce_dan(std::uint8_t tbl, const std::array<std::uint8_t, kDanBytes>& dan,
    std::vector<WorldOp>& out) {
    for (std::size_t i = 0; i < kDanBytes; ++i) {
        WorldOp op;
        op.domain = Domain::Dan;
        op.tbl = tbl;
        op.index = static_cast<std::uint16_t>(i | kTempMirrorOnly);
        op.set = dan[i];
        op.clr = static_cast<std::uint8_t>(~dan[i]);
        out.push_back(op);
    }
}

void announce_zone(std::uint8_t tbl, std::uint8_t room,
    const std::array<std::uint8_t, kZoneBytes>& zone, std::vector<WorldOp>& out) {
    for (std::size_t i = 0; i < kZoneBytes; ++i) {
        WorldOp op;
        op.domain = Domain::Zone;
        op.tbl = tbl;
        op.index = zone_index(room, i, true);
        op.set = zone[i];
        op.clr = static_cast<std::uint8_t>(~zone[i]);
        out.push_back(op);
    }
}

void write_temp_mirrors(Writer& w, const TempMirrors& m) {
    w.write_u8(kTempFormatVersion);
    // There are only 32 tables; the clamp just keeps the count byte honest.
    const std::size_t n = std::min<std::size_t>(m.size(), kMemTbls);
    w.write_u8(static_cast<std::uint8_t>(n));
    std::size_t written = 0;
    for (const auto& [tbl, mirror] : m) {
        if (written++ == n) {
            break;
        }
        w.write_u8(tbl);
        write_bytes(w, mirror.dan);
        w.write_u64(mirror.rooms);
        for (std::size_t room = 0; room < kZoneRooms; ++room) {
            if ((mirror.rooms >> room & 1) != 0) {
                write_bytes(w, mirror.zone[room]);
            }
        }
    }
}

bool read_temp_mirrors(Reader& r, TempMirrors& out) {
    std::uint8_t version = 0;
    std::uint8_t n = 0;
    if (!r.read_u8(version) || version != kTempFormatVersion || !r.read_u8(n) || n > kMemTbls) {
        return false;
    }
    TempMirrors parsed;
    for (std::uint8_t i = 0; i < n; ++i) {
        std::uint8_t tbl = 0;
        TempMirror mirror;
        if (!r.read_u8(tbl) || tbl >= kMemTbls || !read_bytes(r, mirror.dan) ||
            !r.read_u64(mirror.rooms))
        {
            return false;
        }
        for (std::size_t room = 0; room < kZoneRooms; ++room) {
            if ((mirror.rooms >> room & 1) != 0 && !read_bytes(r, mirror.zone[room])) {
                return false;
            }
        }
        parsed[tbl] = mirror;
    }
    out = std::move(parsed);
    return true;
}

// --- whole-view transfer + digest ----------------------------------------------------------------

void write_view(Writer& w, const WorldView& v) {
    w.write_u8(kViewFormatVersion);
    write_bytes(w, v.event);
    for (const auto& t : v.mem) {
        write_bytes(w, t);
    }
    for (const auto& t : v.mem2) {
        write_bytes(w, t);
    }
    write_bytes(w, v.firstGet);
    write_bytes(w, v.player);
    w.write_s32(v.rupee);
    w.write_s32(v.maxLife);
    w.write_s32(v.poh);
}

bool read_view(Reader& r, WorldView& out) {
    std::uint8_t version = 0;
    if (!r.read_u8(version) || version != kViewFormatVersion) {
        return false;
    }
    // Parse into a scratch view so a short packet never leaves `out` half-written.
    WorldView v;
    if (!read_bytes(r, v.event)) {
        return false;
    }
    for (auto& t : v.mem) {
        if (!read_bytes(r, t)) {
            return false;
        }
    }
    for (auto& t : v.mem2) {
        if (!read_bytes(r, t)) {
            return false;
        }
    }
    if (!read_bytes(r, v.firstGet) || !read_bytes(r, v.player) || !r.read_s32(v.rupee) ||
        !r.read_s32(v.maxLife) || !r.read_s32(v.poh))
    {
        return false;
    }
    out = v;
    return true;
}

std::uint32_t digest(const WorldView& v) {
    Fnv1a f;
    f.bytes(v.event);
    for (std::size_t tbl = 0; tbl < kMemTbls; ++tbl) {
        if (tbl == kLocalOnlyKeyTbl) {
            // Table 6's key count is local (see kLocalOnlyKeyTbl): diff() never sends it, so the
            // two ends may legitimately differ there. Hash it as 0 or every digest would mismatch.
            auto t = v.mem[tbl];
            t[kMemKeyNumOffset] = 0;
            f.bytes(t);
        } else {
            f.bytes(v.mem[tbl]);
        }
    }
    for (const auto& t : v.mem2) {
        f.bytes(t);
    }
    f.bytes(v.firstGet);
    f.bytes(v.player);
    f.s32_le(v.rupee);
    f.s32_le(v.maxLife);
    f.s32_le(v.poh);
    return f.h;
}

// --- self-test -----------------------------------------------------------------------------------

namespace {

bool views_equal(const WorldView& a, const WorldView& b) {
    return a.event == b.event && a.mem == b.mem && a.mem2 == b.mem2 && a.firstGet == b.firstGet &&
           a.player == b.player && a.rupee == b.rupee && a.maxLife == b.maxLife && a.poh == b.poh;
}

WorldOp bit_op(
    Domain d, std::uint8_t tbl, std::uint16_t index, std::uint8_t set, std::uint8_t clr) {
    WorldOp op;
    op.domain = d;
    op.tbl = tbl;
    op.index = index;
    op.set = set;
    op.clr = clr;
    return op;
}

WorldOp counter_op(Domain d, std::uint8_t tbl, std::int16_t delta, std::int32_t resultAbs) {
    WorldOp op;
    op.domain = d;
    op.tbl = tbl;
    op.delta = delta;
    op.resultAbs = resultAbs;
    return op;
}

}  // namespace

bool selftest(std::string& failure) {
#define WS_CHECK(cond, msg)                                                                        \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            failure = msg;                                                                         \
            return false;                                                                          \
        }                                                                                          \
    } while (0)

    // 1. diff -> wire -> apply round-trip on a fresh view, and the shadow update (ping-pong bug).
    {
        WorldView cur;
        cur.event[3] = 0x81;
        cur.event[255] = 0x40;
        cur.mem[2][5] = 0x10;
        cur.mem[31][0x1F] = 0x02;
        cur.mem[2][kMemKeyNumOffset] = 3;
        cur.mem[kLocalOnlyKeyTbl][kMemKeyNumOffset] = 2;
        // Table 6's bits still replicate; only its keyNum is local.
        cur.mem[kLocalOnlyKeyTbl][0] = 0x08;
        cur.mem2[10][7] = 0xFF;
        cur.mem2[63][0] = 0x01;
        cur.firstGet[31] = 0x01;
        cur.player[0] = 0x2A;
        cur.player[kPlayerBytes - 1] = 2;
        cur.rupee = 150;
        cur.maxLife = 20;
        cur.poh = 5;

        const WorldView zero;
        std::vector<WorldOp> ops;
        diff(zero, cur, ops);
        WS_CHECK(!ops.empty(), "diff of a changed view produced no ops");
        for (const WorldOp& op : ops) {
            WS_CHECK(!(op.domain == Domain::MemBit && op.index == kMemKeyNumOffset),
                "diff emitted the keyNum byte as a MemBit op");
            WS_CHECK(!(op.domain == Domain::KeyNum && op.tbl == kLocalOnlyKeyTbl),
                "diff emitted a KeyNum op for the local-only table 6");
            WS_CHECK(op_in_range(op), "diff emitted an op that op_in_range rejects");
        }

        Writer w;
        write_ops(w, ops);
        Reader r(w.data().data(), w.size());
        std::vector<WorldOp> wireOps;
        WS_CHECK(read_ops(r, wireOps), "read_ops failed on write_ops output");
        WS_CHECK(r.remaining() == 0, "read_ops left bytes unread");
        WS_CHECK(wireOps.size() == ops.size(), "read_ops returned a different op count");

        WorldView view;
        WorldView shadow;
        for (const WorldOp& op : wireOps) {
            WS_CHECK(apply_op(view, shadow, op, false), "apply_op rejected an op from diff");
        }
        WorldView expected = cur;
        expected.mem[kLocalOnlyKeyTbl][kMemKeyNumOffset] = 0;
        WS_CHECK(views_equal(view, expected),
            "round-trip did not reproduce cur (except table 6 keyNum)");
        WS_CHECK(views_equal(shadow, view), "apply_op did not update the shadow to match the view");

        std::vector<WorldOp> again;
        diff(shadow, view, again);
        WS_CHECK(again.empty(), "second diff after apply is not empty (ping-pong)");
    }

    // 2. Two players flipping different bits of one byte merge, in either order.
    {
        WorldView a;
        WorldView sa;
        const WorldOp p1 = bit_op(Domain::Event, kNoTbl, 0, 0x01, 0);
        const WorldOp p2 = bit_op(Domain::Event, kNoTbl, 0, 0x02, 0);
        WS_CHECK(apply_op(a, sa, p1, false) && apply_op(a, sa, p2, false), "bit apply failed");
        WS_CHECK(
            a.event[0] == 0x03 && sa.event[0] == 0x03, "disjoint sets in one byte did not merge");

        WorldView b;
        WorldView sb;
        WS_CHECK(apply_op(b, sb, p2, false) && apply_op(b, sb, p1, false), "bit apply failed");
        WS_CHECK(b.event[0] == a.event[0], "bit merge depends on order");

        // A clear and a set of different bits merge too.
        const WorldOp c1 = bit_op(Domain::MemBit, 4, 0x10, 0, 0x01);
        const WorldOp c2 = bit_op(Domain::MemBit, 4, 0x10, 0x04, 0);
        a.mem[4][0x10] = 0x03;
        sa.mem[4][0x10] = 0x03;
        WS_CHECK(apply_op(a, sa, c1, false) && apply_op(a, sa, c2, false), "bit apply failed");
        WS_CHECK(a.mem[4][0x10] == 0x06 && sa.mem[4][0x10] == 0x06, "clear + set did not merge");
    }

    // 3. Counters: delta vs absolute, clamps, and diff's s16 delta clamp.
    {
        WorldView v;
        WorldView s;
        v.rupee = 100;
        s.rupee = 100;
        const WorldOp add = counter_op(Domain::Rupee, kNoTbl, 50, 999);
        WS_CHECK(
            apply_op(v, s, add, false) && v.rupee == 150 && s.rupee == 150, "rupee delta path");
        WS_CHECK(
            apply_op(v, s, add, true) && v.rupee == 999 && s.rupee == 999, "rupee absolute path");

        v.rupee = 9990;
        WS_CHECK(
            apply_op(v, s, add, false) && v.rupee == 9999 && s.rupee == 9999, "rupee upper clamp");
        v.rupee = 100;
        WS_CHECK(apply_op(v, s, counter_op(Domain::Rupee, kNoTbl, -200, 0), false) && v.rupee == 0,
            "rupee lower clamp");
        WS_CHECK(
            apply_op(v, s, counter_op(Domain::MaxLife, kNoTbl, 0, 500), true) && v.maxLife == 400,
            "maxLife clamp");
        WS_CHECK(apply_op(v, s, counter_op(Domain::Poh, kNoTbl, 0, 300), true) && v.poh == 255,
            "poh clamp");
        WS_CHECK(apply_op(v, s, counter_op(Domain::KeyNum, 3, 200, 0), false) &&
                     v.key_num(3) == 99 && s.key_num(3) == 99,
            "keyNum clamp");
        WS_CHECK(apply_op(v, s, counter_op(Domain::KeyNum, 3, -1, 0), false) && v.key_num(3) == 98,
            "keyNum delta path");

        WorldView before = v;
        WorldView beforeS = s;
        WS_CHECK(!apply_op(v, s, counter_op(Domain::KeyNum, kLocalOnlyKeyTbl, 1, 1), false),
            "apply_op accepted a KeyNum op for table 6");
        WS_CHECK(!apply_op(v, s, counter_op(Domain::Rupee, 0, 1, 1), false),
            "apply_op accepted rupee tbl 0");
        WS_CHECK(!apply_op(v, s, bit_op(Domain::MemBit, 1, kMemKeyNumOffset, 1, 0), false),
            "apply_op accepted a MemBit op on the keyNum byte");
        WS_CHECK(!apply_op(v, s, bit_op(Domain::Mem2, 64, 0, 1, 0), false),
            "apply_op accepted mem2 tbl 64");
        WS_CHECK(!apply_op(v, s, bit_op(Domain::Items, kNoTbl, kPlayerBytes, 1, 0), false),
            "apply_op accepted an Items index past kPlayerBytes");
        WS_CHECK(
            !apply_op(v, s, bit_op(Domain::Dan, kNoTbl, 0, 1, 0), false), "apply_op accepted Dan");
        WS_CHECK(
            views_equal(v, before) && views_equal(s, beforeS), "a rejected op wrote something");

        WorldView sh;
        WorldView cu;
        cu.rupee = 40000;
        std::vector<WorldOp> ops;
        diff(sh, cu, ops);
        WS_CHECK(ops.size() == 1 && ops[0].domain == Domain::Rupee && ops[0].delta == 32767 &&
                     ops[0].resultAbs == 40000,
            "diff did not clamp a large positive delta to s16");
        ops.clear();
        sh.rupee = 40000;
        cu.rupee = 0;
        diff(sh, cu, ops);
        WS_CHECK(ops.size() == 1 && ops[0].delta == -32768 && ops[0].resultAbs == 0,
            "diff did not clamp a large negative delta to s16");
    }

    // 4. Whole-view transfer and digest.
    {
        WorldView v;
        v.event[7] = 0x5A;
        v.mem[9][3] = 0xC3;
        v.mem[9][kMemKeyNumOffset] = 4;
        v.mem2[50][2] = 0x11;
        v.firstGet[0] = 0x80;
        v.player[40] = 0x07;
        v.rupee = 1234;
        v.maxLife = 60;
        v.poh = 42;

        Writer w;
        write_view(w, v);
        WorldView back;
        Reader r(w.data().data(), w.size());
        WS_CHECK(read_view(r, back) && r.remaining() == 0, "read_view failed on write_view output");
        WS_CHECK(views_equal(back, v), "read_view did not reproduce the view");
        WS_CHECK(digest(back) == digest(v), "digest differs for equal views");

        WorldView changed = v;
        changed.poh = 43;
        WS_CHECK(digest(changed) != digest(v), "digest did not change with a counter");
        changed = v;
        changed.mem2[63][7] ^= 0x01;
        WS_CHECK(digest(changed) != digest(v), "digest did not change with a mem2 bit");

        std::vector<std::uint8_t> bad = w.data();
        bad[0] = static_cast<std::uint8_t>(kViewFormatVersion + 1);
        Reader rv(bad.data(), bad.size());
        WorldView sink;
        WS_CHECK(!read_view(rv, sink), "read_view accepted a wrong format version");
        Reader rs(w.data().data(), w.size() - 1);
        WS_CHECK(!read_view(rs, sink), "read_view accepted truncated data");
    }

    // 5. Host-side proposal limits.
    {
        std::vector<WorldOp> ops;
        for (std::uint16_t i = 0; i < 8; ++i) {
            ops.push_back(bit_op(Domain::MemBit, 1, i, 0xFF, 0));
        }
        WS_CHECK(judge_proposal(ops).accept, "judge_proposal refused exactly 64 bits");
        ops.push_back(bit_op(Domain::MemBit, 1, 8, 0x01, 0));
        const ProposalVerdict tooMany = judge_proposal(ops);
        WS_CHECK(!tooMany.accept && tooMany.bitsTouched == 65, "judge_proposal accepted 65 bits");

        std::vector<WorldOp> clears;
        for (std::uint16_t i = 0; i < 8; ++i) {
            clears.push_back(bit_op(Domain::Event, kNoTbl, i, 0, 0x01));
        }
        WS_CHECK(judge_proposal(clears).accept, "judge_proposal refused exactly 8 event clears");
        clears.push_back(bit_op(Domain::Event, kNoTbl, 8, 0, 0x01));
        const ProposalVerdict tooManyClears = judge_proposal(clears);
        WS_CHECK(!tooManyClears.accept && tooManyClears.eventClears == 9,
            "judge_proposal accepted 9 event clears");

        std::vector<WorldOp> malformed{bit_op(Domain::MemBit, 40, 0, 1, 0)};
        const ProposalVerdict bad = judge_proposal(malformed);
        WS_CHECK(!bad.accept && bad.badOps == 1, "judge_proposal accepted a malformed op");

        std::vector<WorldOp> counters{counter_op(Domain::Rupee, kNoTbl, 500, 600)};
        WS_CHECK(judge_proposal(counters).accept && judge_proposal(counters).bitsTouched == 0,
            "judge_proposal counted a counter op as bits");
    }

    // 6. Malformed wire input.
    {
        std::vector<WorldOp> ops{bit_op(Domain::Event, kNoTbl, 1, 0x01, 0),
            counter_op(Domain::KeyNum, 2, 1, 1), bit_op(Domain::Mem2, 5, 3, 0, 0x02)};
        Writer w;
        write_ops(w, ops);
        Reader truncated(w.data().data(), w.size() - 1);
        std::vector<WorldOp> out;
        WS_CHECK(!read_ops(truncated, out), "read_ops accepted a truncated buffer");

        Writer big;
        big.write_u16(static_cast<std::uint16_t>(kMaxOpsPerMessage + 1));
        Reader rb(big.data().data(), big.size());
        WS_CHECK(!read_ops(rb, out), "read_ops accepted n > kMaxOpsPerMessage");

        Writer unknown;
        unknown.write_u8(7);  // between Zone (6) and Rupee (16): not a domain
        unknown.write_u8(kNoTbl);
        unknown.write_u16(0);
        unknown.write_u8(0);
        unknown.write_u8(0);
        Reader ru(unknown.data().data(), unknown.size());
        WorldOp op;
        WS_CHECK(!read_op(ru, op), "read_op accepted an unknown domain");
    }

    // 7. Temporary domains (Dan, Zone): not in the view, co-location diff, mirrors, announcements.
    {
        // Ranges and the wire.
        WS_CHECK(op_in_range(bit_op(Domain::Dan, 0, 0, 1, 0)), "op_in_range refused Dan byte 0");
        WS_CHECK(op_in_range(bit_op(Domain::Dan, 31, kDanBytes - 1, 1, 0)),
            "op_in_range refused the last Dan byte");
        WS_CHECK(op_in_range(bit_op(Domain::Dan, 3, 5 | kTempMirrorOnly, 1, 0)),
            "op_in_range refused a mirror-only Dan op");
        WS_CHECK(!op_in_range(bit_op(Domain::Dan, 32, 0, 1, 0)), "op_in_range accepted Dan tbl 32");
        WS_CHECK(!op_in_range(bit_op(Domain::Dan, kNoTbl, 0, 1, 0)),
            "op_in_range accepted Dan with no table");
        WS_CHECK(!op_in_range(bit_op(Domain::Dan, 0, kDanBytes, 1, 0)),
            "op_in_range accepted Dan byte 24");
        WS_CHECK(!op_in_range(bit_op(Domain::Dan, 0, 0x100, 1, 0)),
            "op_in_range accepted a Dan index with a room part");
        WS_CHECK(op_in_range(bit_op(Domain::Zone, 0, zone_index(63, kZoneBytes - 1), 1, 0)),
            "op_in_range refused zone room 63 byte 11");
        WS_CHECK(!op_in_range(bit_op(Domain::Zone, 0, zone_index(64, 0), 1, 0)),
            "op_in_range accepted zone room 64");
        WS_CHECK(!op_in_range(bit_op(Domain::Zone, 0, zone_index(1, kZoneBytes), 1, 0)),
            "op_in_range accepted zone byte 12 (unk12 / dSv_zoneActor_c are not replicated)");
        WS_CHECK(zone_room(zone_index(37, 4, true)) == 37 &&
                     temp_byte(zone_index(37, 4, true)) == 4 &&
                     temp_mirror_only(zone_index(37, 4, true)) &&
                     !temp_mirror_only(zone_index(37, 4)),
            "zone index encoding does not round-trip");

        WorldView view;
        WorldView shadow;
        WS_CHECK(!apply_op(view, shadow, bit_op(Domain::Dan, 0, 0, 1, 0), false),
            "apply_op wrote a Dan op into the saved view");
        WS_CHECK(!apply_op(view, shadow, bit_op(Domain::Zone, 0, zone_index(1, 0), 1, 0), false),
            "apply_op wrote a Zone op into the saved view");

        // diff_temp: gameplay only, co-located only.
        TempView a;
        a.tbl = 16;
        a.rooms = (std::uint64_t{1} << 3) | (std::uint64_t{1} << 5);
        TempView b = a;
        b.dan[0] = 0x21;
        b.dan[kDanBytes - 1] = 0x80;
        b.zone[3][4] = 0x01;  // a one-zone switch in room 3
        b.zone[5][0] = 0x40;  // a zone switch in room 5
        b.rooms |= std::uint64_t{1} << 9;  // room 9 appeared: lifecycle, no ops
        b.zone[9][0] = 0xFF;
        std::vector<WorldOp> ops;
        diff_temp(a, b, ops);
        WS_CHECK(ops.size() == 4, "diff_temp did not emit exactly the 4 gameplay changes");
        for (const WorldOp& op : ops) {
            WS_CHECK(op.tbl == 16 && op_in_range(op) && !temp_mirror_only(op.index),
                "diff_temp emitted a bad or mirror-only op");
            WS_CHECK(!(op.domain == Domain::Zone && zone_room(op.index) == 9),
                "diff_temp reported a newly created zone");
        }
        std::vector<WorldOp> none;
        TempView elsewhere = b;
        elsewhere.tbl = 17;
        diff_temp(a, elsewhere, none);
        WS_CHECK(none.empty(), "diff_temp reported a stage-table change as ops");
        TempView noTbl = b;
        noTbl.tbl = -1;
        diff_temp(noTbl, noTbl, none);
        diff_temp(a, noTbl, none);
        WS_CHECK(none.empty(), "diff_temp emitted ops with no table");
        TempView gone = a;
        gone.rooms = std::uint64_t{1} << 5;
        gone.zone[3][4] = 0xFF;
        diff_temp(a, gone, none);
        WS_CHECK(none.empty(), "diff_temp reported a removed zone");

        // Through the wire, then into a co-located view and a mirror.
        Writer w;
        write_ops(w, ops);
        Reader r(w.data().data(), w.size());
        std::vector<WorldOp> wireOps;
        WS_CHECK(read_ops(r, wireOps) && wireOps.size() == ops.size(),
            "temp ops did not survive the wire");
        TempView there = a;
        TempMirror mirror;
        for (const WorldOp& op : wireOps) {
            WS_CHECK(apply_temp_op(there, op), "apply_temp_op(view) refused a co-located op");
            WS_CHECK(apply_temp_op(mirror, op), "apply_temp_op(mirror) refused an op");
        }
        WS_CHECK(there.dan == b.dan && there.zone[3] == b.zone[3] && there.zone[5] == b.zone[5],
            "the co-located view did not end up equal to the sender");
        WS_CHECK(mirror.dan == b.dan && mirror.zone[5] == b.zone[5] &&
                     mirror.rooms == ((std::uint64_t{1} << 3) | (std::uint64_t{1} << 5)),
            "the mirror did not record the ops");
        std::vector<WorldOp> echo;
        TempView thereShadow = there;
        diff_temp(thereShadow, there, echo);
        WS_CHECK(echo.empty(), "applying temp ops to view + shadow still diffs (ping-pong)");

        // A view for another table, or without that room, is not written.
        TempView other;
        other.tbl = 2;
        WS_CHECK(!apply_temp_op(other, wireOps[0]), "apply_temp_op wrote another table's dan");
        TempView noRoom = a;
        noRoom.rooms = 0;
        const WorldOp zoneOp = bit_op(Domain::Zone, 16, zone_index(5, 0), 0x40, 0);
        WS_CHECK(!apply_temp_op(noRoom, zoneOp), "apply_temp_op wrote a zone the view lacks");

        // Mask merge: two players, two bits of one dan byte, either order.
        TempMirror m1;
        TempMirror m2;
        const WorldOp p1 = bit_op(Domain::Dan, 16, 2, 0x01, 0);
        const WorldOp p2 = bit_op(Domain::Dan, 16, 2, 0x10, 0);
        apply_temp_op(m1, p1);
        apply_temp_op(m1, p2);
        apply_temp_op(m2, p2);
        apply_temp_op(m2, p1);
        WS_CHECK(m1.dan[2] == 0x11 && m2.dan[2] == 0x11, "dan bits did not merge");

        // Announcements overwrite a stale mirror exactly.
        TempMirror stale;
        stale.dan.fill(0xA5);
        stale.zone[7].fill(0x5A);
        std::array<std::uint8_t, kDanBytes> fresh{};
        fresh[1] = 0x02;
        std::array<std::uint8_t, kZoneBytes> freshZone{};
        freshZone[0] = 0x80;
        std::vector<WorldOp> ann;
        announce_dan(16, fresh, ann);
        announce_zone(16, 7, freshZone, ann);
        WS_CHECK(ann.size() == kDanBytes + kZoneBytes, "announcement op count");
        for (const WorldOp& op : ann) {
            WS_CHECK(temp_mirror_only(op.index) && op_in_range(op),
                "announcement op not mirror-only / out of range");
            apply_temp_op(stale, op);
        }
        WS_CHECK(stale.dan == fresh && stale.zone[7] == freshZone && (stale.rooms >> 7 & 1) != 0,
            "announcement did not overwrite the stale mirror");

        // Proposal limits: temp bits are not counted against the save limit, but are bounded.
        std::vector<WorldOp> mixed;
        for (std::uint16_t i = 0; i < 8; ++i) {
            mixed.push_back(bit_op(Domain::MemBit, 1, i, 0xFF, 0));
        }
        announce_dan(16, fresh, mixed);
        const ProposalVerdict mv = judge_proposal(mixed);
        WS_CHECK(mv.accept && mv.bitsTouched == 64 && mv.tempBitsTouched == kDanBytes * 8,
            "judge_proposal counted temp bits against the saved-bit limit");
        std::vector<WorldOp> fullTable;
        announce_dan(16, fresh, fullTable);
        for (std::size_t room = 0; room < kZoneRooms; ++room) {
            announce_zone(16, static_cast<std::uint8_t>(room), freshZone, fullTable);
        }
        WS_CHECK(judge_proposal(fullTable).accept, "judge_proposal refused a full-table announce");
        const std::vector<WorldOp> once = fullTable;
        fullTable.insert(fullTable.end(), once.begin(), once.end());
        WS_CHECK(!judge_proposal(fullTable).accept,
            "judge_proposal accepted two full-table overwrites");

        // The WorldSync trailer.
        TempMirrors mirrors;
        mirrors[16] = mirror;
        mirrors[0].dan[0] = 0x01;
        mirrors[31].rooms = std::uint64_t{1} << 63;
        mirrors[31].zone[63][11] = 0x7F;
        Writer tw;
        write_temp_mirrors(tw, mirrors);
        Reader tr(tw.data().data(), tw.size());
        TempMirrors back;
        WS_CHECK(read_temp_mirrors(tr, back) && tr.remaining() == 0,
            "read_temp_mirrors failed on write_temp_mirrors output");
        WS_CHECK(back.size() == 3 && back[16].dan == mirror.dan &&
                     back[16].rooms == mirror.rooms && back[16].zone[5] == mirror.zone[5] &&
                     back[0].dan[0] == 0x01 && back[31].zone[63][11] == 0x7F,
            "read_temp_mirrors did not reproduce the mirrors");
        Reader trs(tw.data().data(), tw.size() - 1);
        TempMirrors sink;
        WS_CHECK(!read_temp_mirrors(trs, sink), "read_temp_mirrors accepted truncated data");
        Writer empty;
        write_temp_mirrors(empty, TempMirrors{});
        Reader er(empty.data().data(), empty.size());
        WS_CHECK(read_temp_mirrors(er, sink) && sink.empty() && er.remaining() == 0,
            "an empty mirror set did not round-trip");
    }

#undef WS_CHECK
    failure.clear();
    return true;
}

}  // namespace dusk::mp::world
