// Coupled differential-pair router (AutorouteOptions::coupledPairs). Included by Pcb.cpp inside its anonymous
// namespace, after RoutingGrid: it searches the pair's centre line on the routing grid and lays both members at the
// pair spacing from it, checking every member segment and via exactly against the copper on the grid.
//
// The search state is (layer, cell, direction, polarity side, last event was a via, straight run). The centre line
// turns by 45° at a time, never sharper, and only after a straight run long enough that the inner member's mitred
// corner cannot fold back; a layer change places two vias side by side, perpendicular to the direction, at least a
// via diameter plus clearance apart, the members jogging out to them at 45° and back after them. Costs are integers
// (hundredths of a cell), so the route does not depend on the compiler or on fused multiply-add.

/// One differential pair to route: its nets and pads (indices into the pad list; end 0 and end 1 each pair a P pad
/// with the N pad next to it), member width, edge-to-edge gap and via size.
struct PairPlan {
    int netP = -1, netN = -1;
    size_t padP[2] = {0, 0}, padN[2] = {0, 0};
    double width = 0.2, gap = 0.2;
    double minGap = 0.2;  // the members' mutual clearance (stubs and via jogs never come closer)
    double viaDiameter = 0.6, viaDrill = 0.3;
};

/// The copper of a routed pair (both members), or why there is none.
struct PairLayout {
    bool ok = false;
    std::string reason;
    std::vector<Track> tracks;
    std::vector<Via> vias;
    double coupledLength = 0, uncoupledLength = 0;
    int viaPairs = 0;
};

namespace pairs {
constexpr int kDx[8] = {1, 1, 0, -1, -1, -1, 0, 1};
constexpr int kDy[8] = {0, 1, 1, 1, 0, -1, -1, -1};
inline Vec2 unit(int dir) {
    const double s = (kDx[dir] != 0 && kDy[dir] != 0) ? 1.0 / std::sqrt(2.0) : 1.0;
    return {kDx[dir] * s, kDy[dir] * s};
}
inline Vec2 leftOf(Vec2 u) { return {-u.y, u.x}; }
/// Grid direction of a unit vector (-1 when it is not one of the eight).
inline int dirOf(Vec2 v) {
    const double len = v.length();
    if (len < 1e-9) return -1;
    for (int k = 0; k < 8; ++k)
        if (unit(k).dot(v * (1.0 / len)) > 1.0 - 1e-6) return k;
    return -1;
}
/// tan(22.5°): how far a 45° corner moves a member's vertex along its segments per unit of offset.
constexpr double kMiter = 0.41421356237309503;

/// Where the pair leaves (or arrives at) one end: the centre-line node, its direction of travel, the polarity side
/// (P member at node + side · d · left(direction)), the layers it may start on, and the stubs from the pads.
struct PairEnd {
    size_t cell = 0;
    int i = 0, j = 0;
    int dir = 0;
    int side = 1;
    std::vector<int> layers;
    std::vector<std::pair<Vec2, Vec2>> stubP, stubN;  // pad → member point, on the end's layer
    double stubLength = 0;                            // both members
};
}  // namespace pairs

template <typename Extra>
PairLayout routeCoupledPair(const RoutingGrid& g, const std::vector<Pad>& ps, const PairPlan& plan, const Extra& extraOf,
                            size_t budget) {
    using namespace pairs;
    PairLayout out;
    const double pitch = g.pitch();
    const double w = plan.width, half = w / 2;
    const double d = (w + plan.gap) / 2;  // centre line to member centre line
    // Via pair: centres at least a via diameter plus clearance apart, holes at least the hole-to-hole spacing.
    const BoardSettings& s = g.settings();
    const double dv = std::max({d, (plan.viaDiameter + plan.minGap) / 2, (plan.viaDrill + s.minHoleToHole) / 2});
    const double jog = dv - d;  // along the direction, before and after the vias
    auto cellsFor = [&](double mm) { return std::max(1, static_cast<int>(std::ceil(mm / pitch - 1e-9))); };
    const int runTurn = cellsFor(2 * kMiter * d + 1e-6);
    const int runVia = cellsFor(jog + kMiter * d + 1e-6);
    const int runViaVia = cellsFor(2 * jog + 1e-6) + 1;
    const int cap = std::max({runTurn, runVia, runViaVia});
    const int layers = g.layers();
    const size_t cells = static_cast<size_t>(g.cols()) * static_cast<size_t>(g.rows());

    auto memberClear = [&](int l, Vec2 a, Vec2 b, int net) { return g.trackClearExact(l, a, b, half, net, extraOf); };
    // Ends: for both directions away from the pads, a few distances out, the centre-line node and the two stubs.
    auto makeEnds = [&](int end, bool arriving, std::vector<PairEnd>& ends) -> std::string {
        const Pad& pp = ps[plan.padP[end]];
        const Pad& pn = ps[plan.padN[end]];
        std::vector<int> ls;
        for (int l = 0; l < layers; ++l)
            if (pp.onLayer(l) && pn.onLayer(l) && g.layerOpen(l, plan.netP) && g.layerOpen(l, plan.netN)) ls.push_back(l);
        if (ls.empty()) return "pads on different layers";
        const Vec2 axis = pn.position - pp.position;
        const double span = axis.length();
        if (span < 1e-6 || span > 4 * (w + plan.gap) + 3.0) return "pads too far apart";
        const int axisDir = dirOf(axis);
        if (axisDir < 0) return "pads not on a 45° axis";
        const Vec2 e = unit(axisDir);
        const Vec2 mid = (pp.position + pn.position) * 0.5;
        const double padReach = std::max({pp.size.x, pp.size.y, pn.size.x, pn.size.y}) / 2;
        for (int sideSign : {1, -1}) {
            const Vec2 away = leftOf(e) * static_cast<double>(sideSign);
            const int awayDir = dirOf(away);
            const Vec2 u = arriving ? away * -1.0 : away;  // direction of travel at this end
            const int travel = arriving ? (awayDir + 4) % 8 : awayDir;
            const double kMin = std::max(std::fabs(span / 2 - d), padReach) + pitch;
            std::set<size_t> seen;
            for (int step = 0; step < 4; ++step) {
                const Vec2 want = mid + away * (kMin + step * pitch);
                const int ci = static_cast<int>(std::lround(want.x / pitch)), cj = static_cast<int>(std::lround(want.y / pitch));
                if (!g.inside(ci, cj) || !seen.insert(g.idx(ci, cj)).second) continue;
                const Vec2 node = g.pos(ci, cj);
                PairEnd pe;
                pe.cell = g.idx(ci, cj);
                pe.i = ci;
                pe.j = cj;
                pe.dir = travel;
                pe.side = leftOf(u).dot(pp.position - pn.position) > 0 ? 1 : -1;
                const Vec2 memberP = node + leftOf(u) * (pe.side * d), memberN = node - leftOf(u) * (pe.side * d);
                bool ok = true;
                auto stub = [&](Vec2 pad, Vec2 member, std::vector<std::pair<Vec2, Vec2>>& segs) {
                    const Vec2 rel = member - pad;
                    const double along = rel.dot(away);
                    const Vec2 lateral = rel - away * along;
                    const double off = lateral.length();
                    if (along < off - 1e-6) return false;
                    if (off < 1e-6 || along - off < 1e-6) {
                        segs.push_back({pad, member});
                    } else {
                        const Vec2 corner = pad + away * (along - off);
                        segs.push_back({pad, corner});
                        segs.push_back({corner, member});
                    }
                    for (const auto& sg : segs) pe.stubLength += (sg.second - sg.first).length();
                    return true;
                };
                ok = stub(pp.position, memberP, pe.stubP) && stub(pn.position, memberN, pe.stubN);
                if (!ok) continue;
                for (int l : ls) {
                    bool layerOk = true;
                    for (const auto& sg : pe.stubP) layerOk = layerOk && memberClear(l, sg.first, sg.second, plan.netP);
                    for (const auto& sg : pe.stubN) layerOk = layerOk && memberClear(l, sg.first, sg.second, plan.netN);
                    for (const auto& a : pe.stubP)
                        for (const auto& b : pe.stubN)
                            if (segmentSegmentDistance(a.first, a.second, b.first, b.second) < w + plan.minGap - 1e-9) layerOk = false;
                    if (layerOk) pe.layers.push_back(l);
                }
                if (!pe.layers.empty()) ends.push_back(std::move(pe));
            }
        }
        return ends.empty() ? "no room to fan out from the pads" : "";
    };
    std::vector<PairEnd> starts, goals;
    if (std::string why = makeEnds(0, false, starts); !why.empty()) {
        out.reason = why;
        return out;
    }
    if (std::string why = makeEnds(1, true, goals); !why.empty()) {
        out.reason = why;
        return out;
    }

    // ---- search ----
    struct Node {
        uint64_t key;
        int64_t cost;
        int parent;
    };
    auto encode = [&](int l, size_t c, int dir, int side, int lastVia, int run) {
        uint64_t k = static_cast<uint64_t>(l) * cells + c;
        k = k * 8 + static_cast<uint64_t>(dir);
        k = k * 2 + (side > 0 ? 1u : 0u);
        k = k * 2 + static_cast<uint64_t>(lastVia);
        return k * static_cast<uint64_t>(cap + 1) + static_cast<uint64_t>(run);
    };
    struct State {
        int l;
        size_t c;
        int dir, side, lastVia, run;
    };
    auto decode = [&](uint64_t k) {
        State st;
        st.run = static_cast<int>(k % static_cast<uint64_t>(cap + 1));
        k /= static_cast<uint64_t>(cap + 1);
        st.lastVia = static_cast<int>(k % 2);
        k /= 2;
        st.side = k % 2 ? 1 : -1;
        k /= 2;
        st.dir = static_cast<int>(k % 8);
        k /= 8;
        st.c = static_cast<size_t>(k % cells);
        st.l = static_cast<int>(k / cells);
        return st;
    };
    std::vector<Node> nodes;
    std::unordered_map<uint64_t, int> index;
    // Open list ordered by f, then by the remaining estimate (deeper nodes first among equals), then by creation.
    using QE = std::tuple<int64_t, int64_t, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    // A layer no end can be reached on costs at least one via pair more (admissible).
    std::vector<char> goalLayer(static_cast<size_t>(layers), 0);
    for (const PairEnd& ge : goals)
        for (int l : ge.layers) goalLayer[static_cast<size_t>(l)] = 1;
    auto heuristic = [&](size_t c, int l) {
        const int i = static_cast<int>(c % static_cast<size_t>(g.cols())), j = static_cast<int>(c / static_cast<size_t>(g.cols()));
        int64_t best = std::numeric_limits<int64_t>::max();
        for (const PairEnd& ge : goals) {
            const int dx = std::abs(i - ge.i), dy = std::abs(j - ge.j);
            best = std::min<int64_t>(best, 100 * std::max(dx, dy) + 41 * std::min(dx, dy));
        }
        return goalLayer[static_cast<size_t>(l)] ? best : best + 1800;
    };
    auto costOfMm = [&](double mm) { return static_cast<int64_t>(std::llround(100.0 * mm / pitch)); };
    auto push = [&](uint64_t key, int64_t cost, int parent, size_t c, int l) {
        auto it = index.find(key);
        if (it != index.end() && nodes[static_cast<size_t>(it->second)].cost <= cost) return;
        int id;
        if (it == index.end()) {
            id = static_cast<int>(nodes.size());
            nodes.push_back({key, cost, parent});
            index.emplace(key, id);
        } else {
            id = it->second;
            nodes[static_cast<size_t>(id)].cost = cost;
            nodes[static_cast<size_t>(id)].parent = parent;
        }
        const int64_t h = heuristic(c, l);
        open.push({cost + h, h, id});
    };
    for (size_t k = 0; k < starts.size(); ++k)
        for (int l : starts[k].layers)
            push(encode(l, starts[k].cell, starts[k].dir, starts[k].side, 0, 0), costOfMm(starts[k].stubLength), -1 - static_cast<int>(k), starts[k].cell, l);
    // Via-pair validity per (cell, direction, side): both vias exactly clear (the jogs depend on the layers).
    std::unordered_map<uint64_t, char> viaOk;
    int found = -1, foundGoal = -1;
    size_t expanded = 0;
    while (!open.empty()) {
        const auto [f, hq, id] = open.top();
        open.pop();
        const Node node = nodes[static_cast<size_t>(id)];
        const State st = decode(node.key);
        if (f - hq > node.cost) continue;  // stale
        const Vec2 at = g.cellPos(st.c);
        // Goal: a node of the far end, arriving in its direction on the right side after a straight run.
        if (st.run >= (st.lastVia ? runVia : runTurn))
            for (size_t k = 0; k < goals.size() && found < 0; ++k)
                if (goals[k].cell == st.c && goals[k].dir == st.dir && goals[k].side == st.side &&
                    std::find(goals[k].layers.begin(), goals[k].layers.end(), st.l) != goals[k].layers.end()) {
                    found = id;
                    foundGoal = static_cast<int>(k);
                }
        if (found >= 0) break;
        if (++expanded > budget) break;
        const int i = static_cast<int>(st.c % static_cast<size_t>(g.cols())), j = static_cast<int>(st.c / static_cast<size_t>(g.cols()));
        const int netAt[2] = {plan.netP, plan.netN};
        // Lateral steps: straight on, or a 45° turn after a long enough straight run.
        for (int turn : {0, 1, -1}) {
            const int nd = (st.dir + turn + 8) % 8;
            if (turn != 0 && st.run < (st.lastVia ? runVia : runTurn)) continue;
            const int ni = i + kDx[nd], nj = j + kDy[nd];
            if (!g.inside(ni, nj)) continue;
            const Vec2 next = g.pos(ni, nj);
            const Vec2 u = unit(nd);
            const double ext = kMiter * d + 1e-6;  // an outer member's mitred corner reaches this far past the node
            bool clear = true;
            for (int m = 0; m < 2 && clear; ++m) {
                const Vec2 off = leftOf(u) * (m == 0 ? st.side * d : -st.side * d);
                clear = memberClear(st.l, at + off - u * ext, next + off + u * ext, netAt[m]);
            }
            if (!clear) continue;
            int64_t step = (kDx[nd] != 0 && kDy[nd] != 0) ? 141 : 100;
            const bool horizontal = kDy[nd] == 0, vertical = kDx[nd] == 0;
            if (layers > 1 && ((st.l % 2 == 0 && vertical) || (st.l % 2 == 1 && horizontal))) step = step * 5 / 4;
            if (g.layerCost(st.l, plan.netP) > 1.0f) step = step * 8 / 5;
            if (turn != 0) step += 60;
            const size_t nc = g.idx(ni, nj);
            const int run = turn == 0 ? std::min(cap, st.run + 1) : 1;
            push(encode(st.l, nc, nd, st.side, turn == 0 ? st.lastVia : 0, run), node.cost + step, id, nc, st.l);
        }
        // Coupled via pair: both members change layer here.
        if (layers > 1 && st.run >= (st.lastVia ? runViaVia : runVia)) {
            const Vec2 u = unit(st.dir), left = leftOf(u);
            const Vec2 viaP = at + left * (st.side * dv), viaN = at - left * (st.side * dv);
            const uint64_t vk = (static_cast<uint64_t>(st.c) * 8 + static_cast<uint64_t>(st.dir)) * 2 + (st.side > 0 ? 1u : 0u);
            auto cached = viaOk.find(vk);
            if (cached == viaOk.end()) {
                const bool ok = g.viaClearExact(viaP, plan.netP, plan.viaDiameter, plan.viaDrill, extraOf) &&
                                g.viaClearExact(viaN, plan.netN, plan.viaDiameter, plan.viaDrill, extraOf);
                cached = viaOk.emplace(vk, ok ? 1 : 0).first;
            }
            if (cached->second) {
                auto jogsClear = [&](int l, double sign) {  // sign -1: the jog before the vias, +1: after
                    if (jog <= 1e-9) return true;
                    for (int m = 0; m < 2; ++m) {
                        const double sd = m == 0 ? st.side : -st.side;
                        const Vec2 from = at + u * (sign * jog) + left * (sd * d);
                        if (!memberClear(l, from, at + left * (sd * dv), netAt[m])) return false;
                    }
                    return true;
                };
                if (jogsClear(st.l, -1.0))
                    for (int ol = 0; ol < layers; ++ol) {
                        if (ol == st.l || !g.layerOpen(ol, plan.netP) || !g.layerOpen(ol, plan.netN)) continue;
                        if (!jogsClear(ol, 1.0)) continue;
                        push(encode(ol, st.c, st.dir, st.side, 1, 0), node.cost + 1800 + 50 * (std::abs(ol - st.l) - 1), id, st.c, ol);
                    }
            }
        }
    }
    if (found < 0) {
        out.reason = expanded > budget ? "search budget exhausted" : "no coupled path";
        return out;
    }

    // ---- members from the centre line ----
    std::vector<State> path;
    int startEnd = -1;
    for (int id = found;; ) {
        path.push_back(decode(nodes[static_cast<size_t>(id)].key));
        const int p = nodes[static_cast<size_t>(id)].parent;
        if (p < 0) {
            startEnd = -1 - p;
            break;
        }
        id = p;
    }
    std::reverse(path.begin(), path.end());
    const PairEnd& se = starts[static_cast<size_t>(startEnd)];
    const PairEnd& ge = goals[static_cast<size_t>(foundGoal)];
    const int side = path.front().side;
    struct Member {
        int net;
        double sign;  // offset side relative to left(direction)
        std::vector<Vec2> pts;
        std::vector<Vec2> dirs;  // expected direction of each segment (pts[k] → pts[k+1])
        int layer;
    };
    Member members[2] = {{plan.netP, static_cast<double>(side), {}, {}, path.front().l},
                         {plan.netN, static_cast<double>(-side), {}, {}, path.front().l}};
    auto emit = [&](Member& m) {
        for (size_t k = 0; k + 1 < m.pts.size(); ++k) {
            Track t;
            t.net = m.net;
            t.layer = m.layer;
            t.width = w;
            t.a = m.pts[k];
            t.b = m.pts[k + 1];
            if ((t.b - t.a).length() > 1e-9) out.tracks.push_back(t);
        }
        m.pts.clear();
        m.dirs.clear();
    };
    auto addStubs = [&](int layer, int net, const std::vector<std::pair<Vec2, Vec2>>& stub) {
        for (const auto& sg : stub) {
            Track t;
            t.net = net;
            t.layer = layer;
            t.width = w;
            t.a = sg.first;
            t.b = sg.second;
            out.tracks.push_back(t);
        }
    };
    addStubs(path.front().l, plan.netP, se.stubP);
    addStubs(path.front().l, plan.netN, se.stubN);
    out.uncoupledLength += se.stubLength + ge.stubLength;
    bool folded = false;
    for (Member& m : members) {
        const Vec2 u0 = unit(path.front().dir);
        m.pts.push_back(g.cellPos(path.front().c) + leftOf(u0) * (m.sign * d));
        m.dirs.push_back(u0);
    }
    for (size_t k = 1; k < path.size(); ++k) {
        const State& a = path[k - 1];
        const State& b = path[k];
        const Vec2 at = g.cellPos(a.c);
        if (a.l != b.l) {  // via pair at a.c
            const Vec2 u = unit(a.dir), left = leftOf(u);
            for (Member& m : members) {
                const Vec2 via = at + left * (m.sign * dv);
                if (jog > 1e-9) {
                    m.pts.push_back(at - u * jog + left * (m.sign * d));
                    m.dirs.push_back(u);
                }
                m.pts.push_back(via);
                emit(m);
                Via v;
                v.net = m.net;
                v.position = via;
                v.drill = plan.viaDrill;
                v.diameter = plan.viaDiameter;
                out.vias.push_back(v);
                m.layer = b.l;
                m.pts.push_back(via);
                m.dirs.push_back(u);
                if (jog > 1e-9) {
                    m.pts.push_back(at + u * jog + left * (m.sign * d));
                    m.dirs.push_back(u);
                }
                out.uncoupledLength += jog > 1e-9 ? 2 * (via - (at - u * jog + left * (m.sign * d))).length() : 0.0;
            }
            out.coupledLength -= 2 * jog;
            ++out.viaPairs;
            continue;
        }
        out.coupledLength += (g.cellPos(b.c) - at).length();
        if (a.dir != b.dir) {  // a 45° corner at a.c: each member's mitred vertex
            const Vec2 u1 = unit(a.dir), u2 = unit(b.dir);
            const Vec2 n = (leftOf(u1) + leftOf(u2)) * (1.0 / (1.0 + u1.dot(u2)));
            for (Member& m : members) {
                m.pts.push_back(at + n * (m.sign * d));
                m.dirs.push_back(u2);
            }
        }
    }
    for (Member& m : members) {
        const Vec2 uEnd = unit(path.back().dir);
        m.pts.push_back(g.cellPos(path.back().c) + leftOf(uEnd) * (m.sign * d));
        // A member segment must run the way its centre line does (an inner corner must not fold back).
        for (size_t k = 0; k + 1 < m.pts.size(); ++k)
            if ((m.pts[k + 1] - m.pts[k]).dot(m.dirs[k]) < -1e-9) folded = true;
        emit(m);
    }
    addStubs(path.back().l, plan.netP, ge.stubP);
    addStubs(path.back().l, plan.netN, ge.stubN);
    if (folded) {
        out.reason = "member folds at a corner";
        out.tracks.clear();
        out.vias.clear();
        return out;
    }
    // Final exact check of the laid-out copper: every member track against the board, and the members against each
    // other at their gap.
    for (const Track& t : out.tracks)
        if (!memberClear(t.layer, t.a, t.b, t.net)) {
            out.reason = "member clearance";
            out.tracks.clear();
            out.vias.clear();
            return out;
        }
    for (const Track& a : out.tracks)
        for (const Track& b : out.tracks)
            if (a.net == plan.netP && b.net == plan.netN && a.layer == b.layer &&
                segmentSegmentDistance(a.a, a.b, b.a, b.b) < w + plan.minGap - 1e-6) {
                out.reason = "members too close";
                out.tracks.clear();
                out.vias.clear();
                return out;
            }
    for (const Via& v : out.vias)
        for (const Track& t : out.tracks)
            if (t.net != v.net && pointSegmentDistance(v.position, t.a, t.b) < v.diameter / 2 + half + plan.minGap - 1e-6) {
                out.reason = "via too close to the other member";
                out.tracks.clear();
                out.vias.clear();
                return out;
            }
    out.ok = true;
    return out;
}
