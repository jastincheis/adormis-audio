#include "BassGenerator.h"

namespace wf::bass
{
namespace
{
// --- Random numbers (mulberry32, as on the page) ------------------------------

class Random
{
public:
    explicit Random (juce::uint32 seed) : state (seed) {}

    double operator()()
    {
        state += 0x6D2B79F5u;
        juce::uint32 t = state;
        t = (t ^ (t >> 15)) * (t | 1u);
        t ^= t + (t ^ (t >> 7)) * (t | 61u);
        return (double) (t ^ (t >> 14)) / 4294967296.0;
    }

private:
    juce::uint32 state;
};

// JavaScript's ">>> 0" of an integer-valued double.
juce::uint32 toUint32 (double v)
{
    const auto m = std::fmod (std::floor (v), 4294967296.0);
    return (juce::uint32) (m < 0.0 ? m + 4294967296.0 : m);
}

template <typename T>
T pickWeighted (Random& r, const std::vector<std::pair<T, double>>& items)
{
    double sum = 0.0;

    for (auto& [value, w] : items)
        sum += std::max (0.0, w);

    auto x = r() * sum;

    for (auto& [value, w] : items)
    {
        x -= std::max (0.0, w);

        if (x <= 0.0)
            return value;
    }

    return items.front().first;
}

int jsRound (double v)  { return (int) std::floor (v + 0.5); }

// --- Styles -------------------------------------------------------------------

enum Fill { roll, oct, push, approach, run, rest };

struct Style
{
    StyleInfo info;
    bool sub, glide;
    double glideP;
    bool legato;
    int lo, hi;
    double span;
    std::array<double, 7> degree;
    double stepW, leap, rep, appr, chrom, ghost;
    std::vector<Fill> fills;
    std::vector<const char*> templates;   // 'x' onset, '-' sustain, '.' rest, 'g' ghost
};

const std::vector<Style>& styleTable()
{
    static const std::vector<Style> table {
        { { "trap", "Trap 808", "glide, octave" }, true, true, 0.45, true, -5, 12, 7,
          { 7, .6, 2.2, .8, 3.6, 1.4, 2.4 }, .8, 2.2, 1.3, .35, .3, 0,
          { roll, oct, push, approach },
          { "x------x--x-----", "x-----x-----x-x-", "x--x------x--x--", "x-------x-x--x--", "x--------x--x---" } },
        { { "drill", "Drill", "frequent slides" }, true, true, 0.75, true, -5, 12, 8,
          { 6, 1.6, 2.4, 1, 3, 2.2, 1.6 }, 1.3, 1.6, .7, .5, .5, 0,
          { push, approach, run, roll },
          { "x-----x--x---x--", "x--x--x---x--x--", "x---x--x--x---x-", "x------x-x--x---" } },
        { { "house", "House", "off-beat" }, false, false, 0, false, -5, 12, 9,
          { 6, 1, 1.6, 1.2, 3.4, 1.4, 2 }, 1, 2.4, 1.6, .45, .35, .25,
          { oct, approach, run },
          { "..x-..x-..x-..x-", "..x-..x.x.x-..x.", "..x-..xx..x-..xx", ".xx-..x-.xx-..x-", "..x-.xx-..x-.x.x" } },
        { { "techno", "Techno", "rolling 16ths" }, false, false, 0, false, -5, 12, 5,
          { 9, 1.4, 1.2, 1, 2.4, 1.2, 1.4 }, .7, 1.8, 2.6, .25, .5, 0,
          { oct, approach, run },
          { ".xxx.xxx.xxx.xxx", "..x...x...x...x.", ".x.x.xx..x.x.xxx", ".xx..xx..xx..x.x" } },
        { { "boombap", "Hip-hop / Boom bap", "syncopated" }, false, false, 0, false, -5, 12, 9,
          { 6, 1.2, 2, 1.6, 3.2, 1.4, 2.6 }, 1.2, 1.6, .9, .55, .45, .3,
          { run, approach, rest },
          { "x--...x---x--...", "x-.x...x--x-..x.", "x---.x.---x-.x--", "x-....x-.x--..x-" } },
        { { "funk", "Funk", "ghost notes, octave" }, false, false, 0, false, -5, 12, 10,
          { 6, 1.4, 2.2, 1.4, 3, 1.6, 2.8 }, 1.3, 2.6, .8, .65, .65, .6,
          { oct, run, approach },
          { "x-.gx.x.g.x-.gx.", "x.gx..x.x-.gx.x.", "x-..gxx.x.g.xg.x", "x--gx-.xg.x-.x.g" } },
        { { "rnb", "R&B / Neo-soul", "long notes, walks" }, false, false, 0, true, -5, 12, 10,
          { 5, 1.6, 3, 1.4, 3, 1.8, 2.6 }, 1.5, 1.2, .6, .7, .55, .15,
          { run, approach, rest },
          { "x-------x-------", "x-----x-----x---", "x-------.-x---x-", "x---------x--x--" } },
        { { "reggaeton", "Reggaeton", "dembow 3-3-2" }, true, false, 0, false, -5, 12, 7,
          { 8, 1, 1.8, 1.2, 3.4, 1.4, 1.8 }, .9, 2, 1.4, .4, .3, 0,
          { approach, oct, run },
          { "x--x--x-x--x--x-", "x--x--x-x-----x-", "x-----x-x--x--x-", "x--x--x-x--x-x-x" } },
        { { "dnb", "Drum & Bass / Reese", "long, legato" }, true, true, 0.6, true, -5, 12, 8,
          { 6, 1.8, 2.4, 1.2, 2.6, 2, 1.8 }, 1.4, 1.2, .6, .5, .45, 0,
          { push, run, approach },
          { "x---------x-----", "x-----x---x-----", "x--------x---x--", "x-------x-----x-" } },
        { { "pop", "Pop / Rock", "eighths on the root" }, false, false, 0, false, -5, 12, 9,
          { 8, 1.2, 2, 1.2, 3.4, 1.4, 1.2 }, 1, 1.8, 2, .6, .25, 0,
          { run, approach, oct },
          { "x-x-x-x-x-x-x-x-", "x---x-x-x---x-x-", "x-.xx-x-x-.xx-x-", "x-x-x-.xx-x-x-x-" } },
    };

    return table;
}

enum Contour { flat, arch, asc, desc, wave, zigzag };

struct Character
{
    const char* name;
    int phrase;
    double step, leap, rep, temp;
    int add;
    double sync, rest;
    std::vector<Contour> contours;
    std::vector<Fill> fills;
};

const std::array<Character, 3>& characters()
{
    static const std::array<Character, 3> table { {
        { "Groove", 4, .45, 1, 1.25, .55, 0, .12, .08, { flat, arch }, { oct, approach, roll, run } },
        { "Melodic", 8, 1, .8, .45, .75, 1, .2, .04, { asc, wave, arch }, { run, approach, run } },
        { "Syncopated", 2, .4, 1.8, .8, .9, 0, .55, .3, { desc, zigzag }, { push, rest, oct, roll } },
    } };

    return table;
}

using Form = std::vector<std::string>;

const std::vector<Form>& formsFor (int phrase)
{
    static const std::vector<Form> two { { "A", "C" }, { "B", "D" }, { "A", "A1" }, { "B", "C" } };
    static const std::vector<Form> four { { "A", "A1", "A", "C" }, { "A", "B", "A", "C" }, { "A", "A1", "B", "C" }, { "A", "A", "A2", "C" } };
    static const std::vector<Form> eight { { "A", "A1", "A", "B", "A", "A2", "B1", "C" },
                                           { "A", "A1", "B", "B1", "A", "A2", "A", "C" },
                                           { "A", "B", "A", "B1", "A", "A1", "A2", "C" } };
    return phrase == 2 ? two : phrase == 8 ? eight : four;
}

double contourAt (Contour kind, double u)
{
    switch (kind)
    {
        case arch:   return std::sin (juce::MathConstants<double>::pi * u);
        case asc:    return u;
        case desc:   return 1.0 - u;
        case wave:   return 0.5 + 0.5 * std::sin (juce::MathConstants<double>::twoPi * u * 2.0 - juce::MathConstants<double>::halfPi);
        case zigzag: return ((int) std::floor (u * 8.0) % 2) != 0 ? 0.85 : 0.15;
        case flat:   break;
    }

    return 0.25;
}

// Scale degree -> MIDI pitch, counted from the chord root `rootMidi`; with a
// `third` (3 or 4) the chord's third is that one, whatever the scale says.
std::function<int (int)> diatonicInScale (int keyTonic, bool minor, int rootMidi);

std::function<int (int)> diatonic (int keyTonic, bool minor, int rootMidi, int third = 0)
{
    auto base = diatonicInScale (keyTonic, minor, rootMidi);

    if (third != 3 && third != 4)
        return base;

    return [base, rootMidi, third] (int n)
    {
        const auto p = base (n);
        const auto interval = ((p - rootMidi) % 12 + 12) % 12;
        return interval == 3 || interval == 4 ? p + third - interval : p;
    };
}

std::function<int (int)> diatonicInScale (int keyTonic, bool minor, int rootMidi)
{
    const auto& sc = scaleSteps (minor);
    const auto pc = ((rootMidi % 12) + 12) % 12;
    int index = -1;

    for (int i = 0; i < 7; ++i)
        if ((keyTonic + sc[(size_t) i]) % 12 == pc)
            index = i;

    auto floorDiv = [] (int a, int b) { return (int) std::floor ((double) a / b); };

    if (index < 0)
    {
        static const int major[] { 0, 2, 4, 5, 7, 9, 11 };
        return [rootMidi, floorDiv] (int n) { return rootMidi + major[((n % 7) + 7) % 7] + 12 * floorDiv (n, 7); };
    }

    const auto tonic = rootMidi - sc[(size_t) index];
    return [tonic, index, &sc, floorDiv] (int n)
    {
        const auto j = index + n;
        return tonic + sc[(size_t) (((j % 7) + 7) % 7)] + 12 * floorDiv (j, 7);
    };
}

struct CellNote
{
    int s, l;
    bool ghost = false, accent = false;
};

using Cell = std::vector<CellNote>;

Cell parseTemplate (const char* t)
{
    Cell cell;

    for (int s = 0; s < 16; ++s)
    {
        if (t[s] == 'x' || t[s] == 'g')
        {
            int l = 1;

            while (s + l < 16 && t[s + l] == '-')
                ++l;

            cell.push_back ({ s, l, t[s] == 'g', false });
        }
    }

    return cell;
}

int strength (int s)  { return s == 0 ? 3 : s == 8 ? 2 : s % 4 == 0 ? 1 : 0; }

bool hasStart (const Cell& c, int s)
{
    return std::any_of (c.begin(), c.end(), [s] (auto& n) { return n.s == s; });
}

void sortCell (Cell& c)
{
    std::stable_sort (c.begin(), c.end(), [] (auto& a, auto& b) { return a.s < b.s; });
}

// Rhythm variations: fewer or more hits, anticipations, rests, ghosts.
Cell mutateCell (const Cell& cell, Random& r, const Style& st, const Character& ch, int cx, double amount)
{
    Cell c = cell;
    std::array<double, 16> grid {};
    const int base[] { 0, 1, 2, 1, 3, 1, 2, 1, 2, 1, 2, 1, 3, 1, 2, 1 };

    for (int s = 0; s < 16; ++s)
        grid[(size_t) s] = base[s] * (st.legato ? 0.7 : 1.0);

    if (cx == 1 && c.size() > 3)
    {
        Cell kept;

        for (auto& n : c)
            if (n.s == 0 || strength (n.s) >= 1 || r() < 0.4)
                kept.push_back (n);

        c = kept;

        if (c.size() > 4)
            c.resize (4);
    }

    const auto adds = std::max (0, (cx == 3 ? 2 : cx == 2 ? 1 : 0) + ch.add + (amount > 1.0 ? 1 : 0) - (st.legato ? 1 : 0));

    for (int k = 0; k < adds; ++k)
    {
        std::vector<std::pair<int, double>> free;

        for (int s = 1; s < 16; ++s)
            if (! hasStart (c, s))
                free.push_back ({ s, grid[(size_t) s] + (hasStart (c, s - 1) || hasStart (c, s + 1) ? 0.4 : 0.0) });

        if (free.empty())
            break;

        const auto s = pickWeighted (r, free);
        c.push_back ({ s, 1, st.ghost > 0.0 && r() < st.ghost * 0.6, false });
    }

    sortCell (c);

    // Syncopation: pull a beat onset one 16th early (anticipation).
    for (auto& n : c)
    {
        if (n.s > 0 && n.s % 4 == 0 && r() < ch.sync * amount && ! hasStart (c, n.s - 1))
        {
            n.s -= 1;
            n.l += 1;
            n.accent = true;
        }
    }

    // Rests: drop a weak onset.
    if (c.size() > 2)
    {
        Cell kept;

        for (auto& n : c)
            if (n.s == 0 || strength (n.s) >= 2 || r() > ch.rest * amount)
                kept.push_back (n);

        c = kept;
    }

    if (st.ghost > 0.0 && cx >= 2)
        for (auto& n : c)
            if (! n.ghost && strength (n.s) == 0 && r() < st.ghost * 0.25)
                n.ghost = true;

    sortCell (c);

    for (size_t i = 0; i < c.size(); ++i)
    {
        const auto next = i + 1 < c.size() ? c[i + 1].s : 16;
        c[i].l = std::max (1, std::min (c[i].l, next - c[i].s));
    }

    return c;
}

bool sameRhythm (const Cell& a, const Cell& b)
{
    if (a.size() != b.size())
        return false;

    for (size_t i = 0; i < a.size(); ++i)
        if (a[i].s != b[i].s)
            return false;

    return true;
}

struct OutNote
{
    int s, l, p, v;
    bool ghost = false, accent = false, glide = false;
};

std::vector<Note> buildVariant (const Settings& g, const std::vector<int>& rootPcs, const std::vector<int>& thirds, int vi,
                                const std::vector<int>& perm)
{
    const auto& st = styleTable()[(size_t) g.style];
    const auto& ch = characters()[(size_t) vi];
    const auto cx = g.complexity, bars = g.bars;
    Random r (toUint32 ((double) g.seeds[(size_t) vi] * 7919.0 + vi * 104729.0 + cx * 131.0 + g.style * 17.0 + g.octave * 7.0));

    int phrase = ch.phrase;

    while (phrase > bars)
        phrase /= 2;

    const auto tone = resolvedTone (g);
    const auto base = (tone == Tone::tr808 || tone == Tone::sub ? 24 : 36) + 12 * g.octave;
    auto T = base + g.keyTonic;

    if (g.keyTonic > 7)
        T -= 12;

    auto rootMidi = [T] (int pc) { return T - 5 + ((((pc - (T - 5)) % 12) + 12) % 12); };
    std::vector<int> roots;

    for (auto pc : rootPcs)
        roots.push_back (rootMidi (pc));

    const auto shape = ch.contours[(size_t) std::floor (r() * (double) ch.contours.size())];
    const auto span = st.span * (vi == 1 ? 1.25 : 1.0);

    // Cells: A = the variant's own template, B = a contrasting one.
    const auto tA = parseTemplate (st.templates[(size_t) perm[(size_t) vi % perm.size()]]);
    const auto tB = parseTemplate (st.templates[(size_t) perm[(size_t) (vi + 1) % perm.size()]]);
    const auto cellA = mutateCell (tA, r, st, ch, cx, 1.0);
    auto cellB = mutateCell (tB, r, st, ch, cx, 1.5);

    if (sameRhythm (cellA, cellB))
        cellB = mutateCell (cellB, r, st, ch, std::min (3, cx + 1), 2.0);

    using Degrees = std::vector<std::optional<int>>;
    using ScaleFn = std::function<int (int)>;

    // Pitch choice in scale degrees relative to the bar's chord root.
    auto pickDegree = [&] (const ScaleFn& sd, int R, std::optional<int> prevP, std::optional<int> prev2, int s,
                           double target, bool first, std::optional<int> avoid)
    {
        const auto str = strength (s);
        std::vector<std::pair<int, double>> candidates;

        for (int d = -4; d <= 11; ++d)
        {
            const auto p = sd (d);

            if (p < R + st.lo || p > R + st.hi)
                continue;

            const auto dm = ((d % 7) + 7) % 7;
            const auto chordTone = dm == 0 || dm == 2 || dm == 4;
            auto w = st.degree[(size_t) dm];

            if (first)          w *= dm == 0 ? (d == 0 ? 14.0 : 3.0) : 0.04;
            else if (str >= 2)  w *= chordTone ? 3.0 : 0.15;
            else if (str == 1)  w *= chordTone ? 2.0 : 0.5;
            else                w *= chordTone ? 1.0 : 1.25;

            if (prevP.has_value())
            {
                const auto move = std::abs (p - *prevP);

                if (move == 0)                    w *= st.rep * ch.rep;
                else if (move <= 2)               w *= 1.0 + 2.5 * ch.step * st.stepW;
                else if (move <= 5)               w *= 1.1;
                else if (move == 7 || move == 12) w *= 0.55 * st.leap * ch.leap;
                else                              w *= 0.12;
            }

            if (prev2.has_value() && p == *prev2 && (! prevP.has_value() || p != *prevP) && ! first)
                w *= vi == 1 ? 0.22 : 0.55;

            const auto dz = (p - target) / 5.0;
            w *= std::exp (-dz * dz / 2.0) + 0.06;

            if (avoid.has_value() && d == *avoid)
                w *= 0.25;

            candidates.push_back ({ d, std::pow (w, 1.0 / ch.temp) });
        }

        return candidates.empty() ? 0 : pickWeighted (r, candidates);
    };

    const auto inBar = std::array<double, 3> { 2, 5, 3 }[(size_t) vi];

    auto targetAt = [&] (int b, int s)
    {
        const auto u = (double) ((b % phrase) * 16 + s) / (phrase * 16.0);
        const auto k = b / phrase;
        return T + contourAt (shape, u) * span + (k % 2 != 0 ? 2.0 : 0.0)
                 + inBar * std::sin (juce::MathConstants<double>::pi * s / 16.0);
    };

    auto composeMotif = [&] (const Cell& cell, int b, const ScaleFn& sd, int R, std::optional<int> prevP, bool startOnRoot,
                             std::optional<int> keep, const Degrees* orig)
    {
        Degrees ds;
        auto pp = prevP;
        std::optional<int> p2;

        for (size_t i = 0; i < cell.size(); ++i)
        {
            const auto& n = cell[i];
            int d;

            if (keep.has_value() && (int) i < *keep && orig != nullptr && i < orig->size() && (*orig)[i].has_value())
                d = *(*orig)[i];
            else if (n.ghost && i > 0)
                d = ds[i - 1].value_or (0);
            else
                d = pickDegree (sd, R, pp, p2, n.s, targetAt (b, n.s), startOnRoot && i == 0 && n.s == 0,
                                orig != nullptr && i < orig->size() ? (*orig)[i] : std::nullopt);

            ds.push_back (d);
            p2 = pp;
            pp = sd (d);
        }

        return ds;
    };

    auto fit = [&] (const ScaleFn& sd, int R, int d)
    {
        auto p = sd (d);
        int k = 0;

        while (p > R + st.hi && k++ < 3)  { d -= 7; p = sd (d); }
        while (p < R + st.lo && k++ < 6)  { d += 7; p = sd (d); }

        return p;
    };

    std::vector<OutNote> out;
    std::optional<int> prevP;
    std::optional<Degrees> motA, motB;
    bool pushed = false;

    std::vector<Fill> fillSeq;

    for (auto f : ch.fills)
        if (std::find (st.fills.begin(), st.fills.end(), f) != st.fills.end())
            fillSeq.push_back (f);

    fillSeq.insert (fillSeq.end(), st.fills.begin(), st.fills.end());
    const auto fillIndex = (int) std::floor (r() * 2.0);

    for (int b = 0; b < bars; ++b)
    {
        const auto k = b / phrase, pos = b % phrase;
        const auto& forms = formsFor (phrase);
        auto form = forms[(size_t) ((vi + k * (phrase == 2 ? 1 : 2) + perm[0]) % (int) forms.size())];

        if (k > 0 && form.back() == "C")
            form.back() = "D";

        if (k % 2 == 1 && phrase >= 4 && form[2] == "A")
            form[2] = "A2";

        const auto label = form[(size_t) pos];
        const auto R = roots[(size_t) b], nextR = roots[(size_t) ((b + 1) % bars)];
        const auto thirdOf = [&] (int bar) { return thirds.empty() ? 0 : thirds[(size_t) (bar % (int) thirds.size())]; };
        const auto sd = diatonic (g.keyTonic, g.minor, R, thirdOf (b)), sdN = diatonic (g.keyTonic, g.minor, nextR, thirdOf (b + 1));
        Cell cell;
        Degrees ds;

        if (label[0] == 'B')
        {
            cell = label == "B1" ? mutateCell (cellB, r, st, ch, cx, 0.8) : cellB;

            if (! motB.has_value())
                motB = composeMotif (cellB, b, sd, R, prevP, true, std::nullopt, nullptr);

            if (label == "B1")
                ds = composeMotif (cell, b, sd, R, prevP, true, std::max (1, (int) cell.size() - 2), &*motB);
            else
                ds = Degrees (motB->begin(), motB->begin() + (long) std::min (motB->size(), cell.size()));

            while (ds.size() < cell.size())
                ds.push_back (ds.empty() ? 0 : ds.back().value_or (0));
        }
        else
        {
            const auto previous = pos > 0 ? form[(size_t) pos - 1] : std::string();
            const auto useA = ! (label == "C" || label == "D") || (previous != "B" && previous != "B1");
            const auto& src = useA ? cellA : cellB;
            cell = label == "A2" ? mutateCell (src, r, st, ch, cx, 1.4) : src;

            if (! motA.has_value() && useA)
                motA = composeMotif (cellA, b, sd, R, prevP, true, std::nullopt, nullptr);

            if (! useA && ! motB.has_value())
                motB = composeMotif (cellB, b, sd, R, prevP, true, std::nullopt, nullptr);

            const auto& motif = useA ? *motA : *motB;

            if (label == "A1")
                ds = composeMotif (cell, b, sd, R, prevP, true, std::max (1, (int) cell.size() - (cx >= 2 ? 2 : 1)), &motif);
            else if (label == "A2")
                ds = composeMotif (cell, b, sd, R, prevP, true, std::max (1, (int) std::ceil (cell.size() / 2.0)), &motif);
            else
                ds = motif;
        }

        std::vector<OutNote> notes;

        for (size_t i = 0; i < cell.size(); ++i)
        {
            const auto d = i < ds.size() && ds[i].has_value() ? *ds[i] : 0;
            notes.push_back ({ cell[i].s, cell[i].l, fit (sd, R, d), 0, cell[i].ghost, cell[i].accent });
        }

        // A pushed (anticipated) downbeat from the previous bar replaces this bar's first hit.
        if (pushed)
        {
            if (! notes.empty() && notes.front().s == 0)
            {
                const auto first = notes.front();
                notes.erase (notes.begin());

                if (! out.empty() && st.legato)
                    out.back().l += first.l;
            }

            pushed = false;
        }

        // Phrase ending: a fill leading into the next phrase.
        const auto isEnd = label == "C" || label == "D";

        if (isEnd)
        {
            const auto type = fillSeq[(size_t) ((fillIndex + (label == "D" ? 1 : 0) + k) % (int) fillSeq.size())];
            const auto from = type == run && cx == 3 ? 10 : 12;
            std::vector<OutNote> kept;

            for (auto& n : notes)
                if (n.s < from)
                    kept.push_back (n);

            notes = kept;

            if (! notes.empty())
                notes.back().l = std::min (notes.back().l, from - notes.back().s);

            const auto up = nextR - 4 < R + st.lo || r() < 0.5;

            if (type == run)
            {
                const std::vector<int> steps = from == 10 ? std::vector<int> { 10, 12, 13, 14, 15 }
                                             : cx == 1   ? std::vector<int> { 12, 14 }
                                                         : std::vector<int> { 12, 13, 14, 15 };
                const auto count = (int) steps.size();

                for (int i = 0; i < count; ++i)
                {
                    const auto dd = up ? -(count - i) : (count - i);
                    notes.push_back ({ steps[(size_t) i], 1, sdN (dd), 0, false, i == count - 1 });
                }
            }
            else if (type == oct)
            {
                const std::vector<int> steps = st.sub || cx == 1 ? std::vector<int> { 12, 14 } : std::vector<int> { 12, 13, 14, 15 };

                for (size_t i = 0; i < steps.size(); ++i)
                    notes.push_back ({ steps[i], st.sub ? 2 : 1, i % 2 != 0 ? R + 12 : R, 0, false, i % 2 != 0 });
            }
            else if (type == roll)
            {
                for (int s : { 12, 13, 14 })
                    notes.push_back ({ s, 1, R, 0 });

                notes.push_back ({ 15, 1, fit (sd, R, 4), 0, false, true });
            }
            else if (type == approach)
            {
                notes.push_back ({ 12, 2, fit (sd, R, 4), 0 });
                const auto chromatic = r() < st.chrom;
                notes.push_back ({ 14, 2, chromatic ? nextR + (prevP.has_value() && *prevP > nextR ? 1 : -1) : sdN (up ? -1 : 1),
                                   0, false, true });
            }
            else if (type == push)
            {
                notes.push_back ({ 14, 2, nextR, 0, false, true });
                pushed = b < bars - 1;
            }
            else if (type == rest)
            {
                notes.push_back ({ 15, 1, sdN (-1), 0, false, true });
            }
        }
        else if (nextR != R && r() < st.appr * (ch.step + 0.4))
        {
            // Approach note into the next chord on the last weak 16th / 8th.
            if (! notes.empty() && notes.back().s >= 13 && strength (notes.back().s) == 0 && ! notes.back().ghost)
            {
                auto& last = notes.back();
                last.p = r() < st.chrom ? nextR + (last.p > nextR ? 1 : -1) : sdN (last.p > nextR ? 1 : -1);
                last.accent = true;
            }
            else if (cx >= 2 && ! std::any_of (notes.begin(), notes.end(), [] (auto& n) { return n.s >= 14; }))
            {
                if (! notes.empty())
                    notes.back().l = std::min (notes.back().l, 15 - notes.back().s);

                const auto chromatic = r() < st.chrom;
                notes.push_back ({ 15, 1, chromatic ? nextR + (R > nextR ? 1 : -1) : sdN (R > nextR ? 1 : -1), 0, false, true });
            }
        }

        std::stable_sort (notes.begin(), notes.end(), [] (auto& x, auto& y) { return x.s < y.s; });
        const auto phraseStart = pos == 0;

        for (auto& n : notes)
        {
            const auto str = strength (n.s);
            auto v = std::array<int, 4> { 84, 96, 108, 118 }[(size_t) str] + (n.s % 2 == 0 && str == 0 ? 6 : 0);

            if (n.accent)                  v += 12;
            if (phraseStart && n.s == 0)   v += 6;
            if (n.ghost)                   v = 46 + (int) std::floor (r() * 10.0);

            v += jsRound ((r() - 0.5) * 8.0);
            out.push_back ({ b * 16 + n.s, n.l, n.p, std::clamp (v, 1, 127) });
            prevP = n.p;
        }
    }

    // Durations: legato styles sustain into the next hit, the Syncopated character leaves air.
    std::stable_sort (out.begin(), out.end(), [] (auto& a, auto& b) { return a.s != b.s ? a.s < b.s : a.p < b.p; });
    const auto total = bars * 16;

    for (size_t i = 0; i < out.size(); ++i)
    {
        auto& n = out[i];
        const auto gap = (i + 1 < out.size() ? out[i + 1].s : total) - n.s;

        if (gap <= 0)
        {
            out.erase (out.begin() + (long) i);
            --i;
            continue;
        }

        if (st.legato && n.v > 60)
        {
            n.l = gap;

            if (vi == 2 && gap > 3 && r() < ch.rest * 1.5)
                n.l = std::max (2, (int) std::floor (gap * 0.6));
        }

        n.l = std::max (1, std::min (n.l, gap));
    }

    // Keep every note in an audible bass register: nothing under ~31 Hz (B0) for the 808 / sub.
    const auto minPitch = (tone == Tone::tr808 || tone == Tone::sub ? 23 : 33) + 12 * std::max (0, g.octave);

    for (auto& n : out)
        while (n.p < minPitch)
            n.p += 12;

    if (st.glide)
    {
        const auto factor = vi == 2 ? 1.25 : vi == 1 ? 1.1 : 0.85;

        for (size_t i = 1; i < out.size(); ++i)
        {
            const auto& a = out[i - 1];
            auto& b = out[i];
            b.glide = a.s + a.l >= b.s && a.p != b.p && r() < st.glideP * factor;
        }
    }

    std::vector<Note> result;

    for (auto& n : out)
        result.push_back ({ n.s, n.l, n.p, n.v, n.glide });

    return result;
}

// --- Synthesis ----------------------------------------------------------------

// A Web Audio style automated parameter (set / linear / exponential / target).
class Param
{
public:
    explicit Param (double initial) : initialValue (initial) {}

    void set (double v, double t)               { add ({ Kind::set, v, t, 0 }); }
    void linearTo (double v, double t)          { add ({ Kind::linear, v, t, 0 }); }
    void exponentialTo (double v, double t)     { add ({ Kind::exponential, v, t, 0 }); }
    void target (double v, double t, double tau){ add ({ Kind::target, v, t, tau }); }

    double at (double t) const
    {
        auto value = initialValue, since = 0.0, goal = 0.0, tau = 1.0;
        auto approaching = false;

        auto current = [&] (double time)
        {
            return approaching ? goal + (value - goal) * std::exp (-(time - since) / tau) : value;
        };

        for (const auto& e : events)
        {
            if (e.kind == Kind::linear || e.kind == Kind::exponential)
            {
                if (t < e.time)
                {
                    if (t < since)
                        return current (t);

                    const auto start = current (since);
                    const auto u = (t - since) / std::max (1.0e-9, e.time - since);

                    if (e.kind == Kind::linear || start * e.value <= 0.0)
                        return start + (e.value - start) * u;

                    return start * std::pow (e.value / start, u);
                }

                value = e.value;
                since = e.time;
                approaching = false;
                continue;
            }

            if (t < e.time)
                return current (t);

            const auto valueThen = current (e.time);
            since = e.time;

            if (e.kind == Kind::set)
            {
                value = e.value;
                approaching = false;
            }
            else
            {
                value = valueThen;
                goal = e.value;
                tau = std::max (1.0e-6, e.tau);
                approaching = true;
            }
        }

        return current (t);
    }

private:
    enum class Kind { set, linear, exponential, target };
    struct Event { Kind kind; double value, time, tau; };

    void add (Event e)
    {
        const auto it = std::upper_bound (events.begin(), events.end(), e.time, [] (double t, const Event& x) { return t < x.time; });
        events.insert (it, e);
    }

    double initialValue;
    std::vector<Event> events;
};

enum class Wave { sine, saw, square };

double polyBlep (double t, double dt)
{
    if (t < dt)        { t /= dt; return t + t - t * t - 1.0; }
    if (t > 1.0 - dt)  { t = (t - 1.0) / dt; return t * t + t + t + 1.0; }
    return 0.0;
}

// Renders `seconds` of an oscillator whose frequency follows `freq` (times
// `multiplier`, detuned by `cents`), from t0, into `out`.
void oscillator (std::vector<float>& out, double rate, double t0, Wave wave, const Param& freq, double multiplier,
                 double cents, float gain)
{
    const auto detune = multiplier * std::pow (2.0, cents / 1200.0);
    double phase = 0.0;

    for (size_t i = 0; i < out.size(); ++i)
    {
        const auto f = freq.at (t0 + (double) i / rate) * detune;
        const auto dt = std::min (0.5, f / rate);
        double v;

        switch (wave)
        {
            case Wave::sine:   v = std::sin (juce::MathConstants<double>::twoPi * phase); break;
            case Wave::saw:    v = 2.0 * phase - 1.0 - polyBlep (phase, dt); break;
            case Wave::square:
            default:
            {
                v = phase < 0.5 ? 1.0 : -1.0;
                v += polyBlep (phase, dt);
                v -= polyBlep (std::fmod (phase + 0.5, 1.0), dt);
                break;
            }
        }

        out[i] += (float) v * gain;
        phase += dt;

        if (phase >= 1.0)
            phase -= 1.0;
    }
}

// Low-pass with an automated cutoff; Q in dB like Web Audio.
void lowPass (std::vector<float>& x, double rate, double t0, const Param& cutoff, double qDb)
{
    const auto q = std::pow (10.0, qDb / 20.0);
    double x1 = 0, x2 = 0, y1 = 0, y2 = 0;
    double b0 = 0, b1 = 0, b2 = 0, a1 = 0, a2 = 0;

    for (size_t i = 0; i < x.size(); ++i)
    {
        if (i % 16 == 0)
        {
            const auto f = std::clamp (cutoff.at (t0 + (double) i / rate), 10.0, rate * 0.49);
            const auto w = juce::MathConstants<double>::twoPi * f / rate;
            const auto alpha = std::sin (w) / (2.0 * q), cw = std::cos (w), a0 = 1.0 + alpha;
            b0 = (1.0 - cw) / 2.0 / a0;
            b1 = (1.0 - cw) / a0;
            b2 = b0;
            a1 = -2.0 * cw / a0;
            a2 = (1.0 - alpha) / a0;
        }

        const auto v = (double) x[i];
        const auto y = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2;
        x2 = x1; x1 = v; y2 = y1; y1 = y;
        x[i] = (float) y;
    }
}

double midiToHz (int m)  { return 440.0 * std::pow (2.0, (m - 69) / 12.0); }

// One note of the chosen bass sound, added into `dest` (both channels).
void bassVoice (juce::AudioBuffer<float>& dest, double rate, const Note& n, double t, double duration,
                std::optional<int> prevPitch, Tone tone, double tune)
{
    const auto f = midiToHz (n.pitch) * tune;
    const auto end = t + std::max (0.04, duration);
    const auto glide = n.glide && prevPitch.has_value();
    const auto velocity = n.velocity / 127.0;

    auto pitchParam = [&] (double multiplier)
    {
        Param p (f * multiplier);

        if (glide)
        {
            p.set (midiToHz (*prevPitch) * tune * multiplier, t);
            p.exponentialTo (f * multiplier, t + 0.08);
        }
        else
        {
            p.set (f * multiplier, t);
        }

        return p;
    };

    Param gain (1.0);
    double tail = 0.1;
    std::vector<float> filtered, direct;

    auto length = [&] (double extra) { return (size_t) std::ceil ((end + extra - t) * rate); };

    switch (tone)
    {
        case Tone::tr808:
        {
            const auto amp = velocity * 0.75;
            gain.set (0, t);
            gain.linearTo (amp, t + 0.005);

            Param freq (f);

            if (glide)
            {
                freq.set (midiToHz (*prevPitch) * tune, t);
                freq.exponentialTo (f, t + 0.08);
            }
            else
            {
                freq.set (f * 2.4, t);
                freq.exponentialTo (f, t + 0.03);
            }

            gain.target (amp * 0.7, t + 0.05, 0.4);
            gain.target (0, end - 0.01, 0.02);
            tail = 0.15;
            direct.assign (length (tail), 0.0f);
            oscillator (direct, rate, t, Wave::sine, freq, 1.0, 0.0, 1.0f);

            for (auto& v : direct)
                v = std::tanh (2.2f * std::clamp (v, -1.0f, 1.0f));   // the page's wave shaper

            break;
        }

        case Tone::sub:
        {
            const auto amp = velocity * 0.8;
            gain.set (0, t);
            gain.linearTo (amp, t + 0.008);
            gain.target (amp * 0.85, t + 0.02, 0.3);
            gain.target (0, end - 0.012, 0.012);
            direct.assign (length (tail), 0.0f);
            oscillator (direct, rate, t, Wave::sine, pitchParam (1.0), 1.0, 0.0, 1.0f);
            break;
        }

        case Tone::reese:
        {
            const auto amp = velocity * 0.42;
            gain.set (0, t);
            gain.linearTo (amp, t + 0.02);
            gain.target (0, end - 0.02, 0.03);
            tail = 0.12;
            filtered.assign (length (tail), 0.0f);
            direct.assign (filtered.size(), 0.0f);
            const auto pitch = pitchParam (1.0);
            oscillator (filtered, rate, t, Wave::saw, pitch, 1.0, -18.0, 0.5f);
            oscillator (filtered, rate, t, Wave::saw, pitch, 1.0, 18.0, 0.5f);
            Param cutoff (420);
            cutoff.set (420, t);
            cutoff.linearTo (900, t + std::min (0.6, duration));
            lowPass (filtered, rate, t, cutoff, 2.0);
            oscillator (direct, rate, t, Wave::sine, pitch, 1.0, 0.0, 0.9f);
            break;
        }

        case Tone::pluck:
        {
            const auto amp = velocity * 0.6;
            gain.set (0, t);
            gain.linearTo (amp, t + 0.003);
            gain.target (amp * 0.25, t + 0.01, 0.12);
            gain.target (0, end - 0.01, 0.012);
            tail = 0.08;
            filtered.assign (length (tail), 0.0f);
            direct.assign (filtered.size(), 0.0f);
            const auto pitch = pitchParam (1.0);
            oscillator (filtered, rate, t, Wave::square, pitch, 1.0, 0.0, 1.0f);
            Param cutoff (2600);
            cutoff.set (2600, t);
            cutoff.exponentialTo (240, t + 0.09);
            lowPass (filtered, rate, t, cutoff, 7.0);
            oscillator (direct, rate, t, Wave::sine, pitch, 1.0, 0.0, 0.6f);
            break;
        }

        case Tone::synth:
        case Tone::byStyle:
        default:
        {
            const auto amp = velocity * 0.55;
            gain.set (0, t);
            gain.linearTo (amp, t + 0.005);
            gain.target (amp * 0.6, t + 0.03, 0.2);
            gain.target (0, end - 0.01, 0.015);
            filtered.assign (length (tail), 0.0f);
            const auto pitch = pitchParam (1.0);
            const auto half = pitchParam (0.5);
            oscillator (filtered, rate, t, Wave::saw, pitch, 1.0, -7.0, 0.6f);
            oscillator (filtered, rate, t, Wave::saw, pitch, 1.0, 7.0, 0.6f);
            oscillator (filtered, rate, t, Wave::square, half, 1.0, 0.0, 0.5f);
            Param cutoff (1600);
            cutoff.set (1600, t);
            cutoff.exponentialTo (320, t + 0.18);
            lowPass (filtered, rate, t, cutoff, 5.0);
            break;
        }
    }

    const auto count = std::max (filtered.size(), direct.size());
    const auto start = (int) std::lround (t * rate);

    for (size_t i = 0; i < count; ++i)
    {
        const auto index = start + (int) i;

        if (index < 0 || index >= dest.getNumSamples())
            continue;

        auto v = (i < filtered.size() ? filtered[i] : 0.0f) + (i < direct.size() ? direct[i] : 0.0f);
        v *= (float) gain.at (t + (double) i / rate);

        for (int ch = 0; ch < dest.getNumChannels(); ++ch)
            dest.addSample (ch, index, v);
    }
}

void writeVarLength (juce::MemoryOutputStream& out, juce::uint32 n)
{
    std::vector<juce::uint8> bytes { (juce::uint8) (n & 0x7f) };

    while ((n >>= 7) != 0)
        bytes.insert (bytes.begin(), (juce::uint8) ((n & 0x7f) | 0x80));

    for (auto b : bytes)
        out.writeByte ((char) b);
}
} // namespace

const std::vector<StyleInfo>& styles()
{
    static const auto list = []
    {
        std::vector<StyleInfo> l;

        for (auto& s : styleTable())
            l.push_back (s.info);

        return l;
    }();

    return list;
}

const char* characterName (int variant)  { return characters()[(size_t) juce::jlimit (0, 2, variant)].name; }

const char* toneName (Tone t)
{
    switch (t)
    {
        case Tone::tr808:   return "808";
        case Tone::sub:     return "sub";
        case Tone::synth:   return "synth";
        case Tone::reese:   return "reese";
        case Tone::pluck:   return "pluck";
        case Tone::byStyle: break;
    }

    return "auto";
}

Tone resolvedTone (const Settings& s)
{
    if (s.tone != Tone::byStyle)
        return s.tone;

    const juce::String id (styleTable()[(size_t) s.style].info.id);

    if (id == "trap" || id == "drill" || id == "reggaeton")  return Tone::tr808;
    if (id == "dnb")                                          return Tone::reese;
    if (id == "funk")                                         return Tone::pluck;
    return Tone::synth;
}

bool styleGlides (int style)  { return styleTable()[(size_t) style].glide; }

int totalBars (double duration, double downbeat, double bpm)
{
    const auto x = (duration - downbeat) / (240.0 / bpm);
    const auto r = std::round (x);

    if (r >= 1.0 && std::abs (x - r) < 0.06)
        return (int) r;

    return std::max (1, (int) std::floor (x + 0.25));
}

namespace
{
// The energy per note of generated bar `b` (the matching bar of the audio).
void barChroma (const Settings& g, const KeyAnalysis& analysis, double duration, int b,
                std::array<double, 12>& c, std::array<double, 12>& bs)
{
    const auto total = totalBars (duration, g.downbeat, g.bpm);
    const auto bar = 240.0 / g.bpm;
    const auto sourceBar = (g.startBar + b) % total;
    const auto t0 = g.downbeat + sourceBar * bar, t1 = t0 + bar;
    c.fill (0.0);
    bs.fill (0.0);

    for (auto& f : analysis.frames)
        if (f.time >= t0 && f.time < t1)
            for (size_t i = 0; i < 12; ++i)
            {
                c[i] += f.chroma[i];
                bs[i] += f.bass[i];
            }
}
} // namespace

int scaleThird (int root, int keyTonic, bool minor)
{
    const auto& sc = scaleSteps (minor);

    for (int i = 0; i < 7; ++i)
        if ((keyTonic + sc[(size_t) i]) % 12 == root)
            return ((sc[(size_t) ((i + 2) % 7)] - sc[(size_t) i]) % 12 + 12) % 12;

    return 4;
}

std::vector<int> computeThirds (const Settings& g, const KeyAnalysis* analysis, double duration, const std::vector<int>& roots)
{
    std::vector<int> thirds;

    for (int b = 0; b < (int) roots.size(); ++b)
    {
        const auto root = roots[(size_t) b];
        auto third = scaleThird (root, g.keyTonic, g.minor);

        if (g.followChords && analysis != nullptr)
        {
            std::array<double, 12> c {}, bs {};
            barChroma (g, *analysis, duration, b, c, bs);
            const auto minor3 = c[(size_t) ((root + 3) % 12)], major3 = c[(size_t) ((root + 4) % 12)];
            double sum = 0.0;

            for (auto v : c)
                sum += v;

            // Only a clear difference overrides the scale.
            if (sum > 0.0 && std::max (minor3, major3) > 0.04 * sum && std::abs (minor3 - major3) > 0.25 * std::max (minor3, major3))
                third = minor3 > major3 ? 3 : 4;
        }

        thirds.push_back (third);
    }

    return thirds;
}

std::vector<int> computeRoots (const Settings& g, const KeyAnalysis* analysis, double duration)
{
    const auto& sc = scaleSteps (g.minor);
    std::vector<int> roots;

    for (int b = 0; b < g.bars; ++b)
    {
        if (! g.followChords || analysis == nullptr)
        {
            roots.push_back (g.keyTonic);
            continue;
        }

        std::array<double, 12> c {}, bs {};
        barChroma (g, *analysis, duration, b, c, bs);
        double cs = 0.0, bss = 0.0;

        for (size_t i = 0; i < 12; ++i)
        {
            cs += c[i];
            bss += bs[i];
        }

        if (cs <= 0.0 && bss <= 0.0)
        {
            roots.push_back (g.keyTonic);
            continue;
        }

        auto best = g.keyTonic;
        auto bestValue = -1.0e9;
        auto cn = [&] (int x) { return cs > 0.0 ? c[(size_t) x] / cs : 0.0; };
        auto bn = [&] (int x) { return bss > 0.0 ? bs[(size_t) x] / bss : 0.0; };

        for (int i = 0; i < 7; ++i)
        {
            const auto root = (g.keyTonic + sc[(size_t) i]) % 12;
            const auto fifth = (g.keyTonic + sc[(size_t) ((i + 4) % 7)]) % 12;
            // the chord may be major or minor whatever the scale says (e.g. the major V in minor)
            const auto third = cn ((root + 3) % 12) >= cn ((root + 4) % 12) ? (root + 3) % 12 : (root + 4) % 12;
            const auto v = cn (root) + 0.7 * cn (third) + 0.7 * cn (fifth) + 1.6 * bn (root) - 0.4 * bn (third) + (i == 0 ? 0.03 : 0.0);

            if (v > bestValue)
            {
                bestValue = v;
                best = root;
            }
        }

        roots.push_back (best);
    }

    return roots;
}

std::array<std::vector<Note>, 3> generate (const Settings& g, const std::vector<int>& roots, const std::vector<int>& thirds)
{
    const auto& st = styleTable()[(size_t) g.style];
    Random pr (toUint32 ((double) g.seeds[0] * 31.0 + 7.0));
    std::vector<int> perm;

    for (int i = 0; i < (int) st.templates.size(); ++i)
        perm.push_back (i);

    for (int i = (int) perm.size() - 1; i > 0; --i)
        std::swap (perm[(size_t) i], perm[(size_t) std::floor (pr() * (i + 1))]);

    std::vector<int> barRoots (roots);
    barRoots.resize ((size_t) g.bars, g.keyTonic);

    std::vector<int> barThirds (thirds);

    if (! barThirds.empty())
        barThirds.resize ((size_t) g.bars, 0);

    return { buildVariant (g, barRoots, barThirds, 0, perm), buildVariant (g, barRoots, barThirds, 1, perm),
             buildVariant (g, barRoots, barThirds, 2, perm) };
}

juce::MemoryBlock midiFile (const std::vector<Note>& notes, double bpm, const juce::String& trackName, bool glide,
                            int tuningCents)
{
    constexpr int ticksPer16th = 120;
    struct Event { int tick; int order; juce::uint8 data[3]; };
    std::vector<Event> events;

    for (size_t i = 0; i < notes.size(); ++i)
    {
        const auto& n = notes[i];
        const auto on = n.start * ticksPer16th;
        auto off = on + n.length * ticksPer16th;

        // Slides overlap the next note a little (legato / portamento in most synths).
        if (glide && i + 1 < notes.size() && notes[i + 1].glide && notes[i + 1].start == n.start + n.length)
            off += 30;
        else
            off -= 2;

        events.push_back ({ on, 1, { 0x90, (juce::uint8) n.pitch, (juce::uint8) n.velocity } });
        events.push_back ({ off, 0, { 0x80, (juce::uint8) n.pitch, 0 } });
    }

    std::stable_sort (events.begin(), events.end(), [] (auto& a, auto& b) { return a.tick != b.tick ? a.tick < b.tick : a.order < b.order; });

    juce::MemoryOutputStream track;
    const auto name = trackName.toStdString();
    track.writeByte (0);
    track.writeByte ((char) 0xFF);
    track.writeByte (0x03);
    writeVarLength (track, (juce::uint32) name.size());
    track.write (name.data(), name.size());

    const auto microseconds = (juce::uint32) std::lround (60.0e6 / bpm);
    const juce::uint8 tempo[] { 0, 0xFF, 0x51, 3, (juce::uint8) (microseconds >> 16), (juce::uint8) (microseconds >> 8), (juce::uint8) microseconds,
                                0, 0xFF, 0x58, 4, 4, 2, 24, 8 };
    track.write (tempo, sizeof (tempo));

    if (tuningCents != 0)
    {
        // Bend range +-2 semitones (RPN 0), then the bend itself: 8192 = none, 4096 per semitone.
        const auto bend = juce::jlimit (0, 16383, 8192 + juce::roundToInt (tuningCents / 200.0 * 8192.0));
        const juce::uint8 tuning[] { 0, 0xB0, 101, 0,   0, 0xB0, 100, 0,   0, 0xB0, 6, 2,   0, 0xB0, 38, 0,
                                     0, 0xB0, 101, 127, 0, 0xB0, 100, 127,
                                     0, 0xE0, (juce::uint8) (bend & 0x7f), (juce::uint8) (bend >> 7) };
        track.write (tuning, sizeof (tuning));
    }

    int last = 0;

    for (auto& e : events)
    {
        writeVarLength (track, (juce::uint32) (e.tick - last));
        track.write (e.data, 3);
        last = e.tick;
    }

    const juce::uint8 endOfTrack[] { 0, 0xFF, 0x2F, 0 };
    track.write (endOfTrack, sizeof (endOfTrack));

    juce::MemoryOutputStream file;
    const auto length = (juce::uint32) track.getDataSize();
    const juce::uint8 header[] { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 0, 0, 1, 0x01, 0xE0, 'M', 'T', 'r', 'k',
                                 (juce::uint8) (length >> 24), (juce::uint8) (length >> 16), (juce::uint8) (length >> 8), (juce::uint8) length };
    file.write (header, sizeof (header));
    file.write (track.getData(), track.getDataSize());
    return file.getMemoryBlock();
}

juce::AudioBuffer<float> renderBass (const std::vector<Note>& notes, Tone tone, double bpm, int bars, double rate,
                                     int tuningCents)
{
    const auto sixteenth = 60.0 / bpm / 4.0;
    const auto length = (int) std::ceil (bars * 16 * sixteenth * rate);
    const auto tune = std::pow (2.0, tuningCents / 1200.0);

    // Rendered with room for the release of the last notes, which is then
    // wrapped round onto the start: the loop repeats seamlessly.
    juce::AudioBuffer<float> work (2, length + (int) std::ceil (0.5 * rate));
    work.clear();

    for (size_t i = 0; i < notes.size(); ++i)
        bassVoice (work, rate, notes[i], notes[i].start * sixteenth, notes[i].length * sixteenth,
                   i > 0 ? std::optional<int> (notes[i - 1].pitch) : std::nullopt, tone, tune);

    juce::AudioBuffer<float> out (2, length);

    for (int ch = 0; ch < 2; ++ch)
    {
        out.copyFrom (ch, 0, work, ch, 0, length);

        for (int from = length; from < work.getNumSamples(); from += length)
            out.addFrom (ch, 0, work, ch, from, std::min (length, work.getNumSamples() - from));
    }

    return out;
}

juce::AudioBuffer<float> sampleLoop (const juce::AudioBuffer<float>& audio, juce::Range<int> range, double rate, const Settings& g)
{
    range = range.getIntersectionWith ({ 0, audio.getNumSamples() });
    const auto bar = 240.0 / g.bpm;
    const auto duration = range.getLength() / rate;
    const auto total = totalBars (duration, g.downbeat, g.bpm);
    juce::AudioBuffer<float> out (audio.getNumChannels(), (int) std::ceil (g.bars * bar * rate));
    out.clear();

    // Contiguous runs of bars, wrapping round to the start of the audio.
    for (int k = 0; k < g.bars;)
    {
        const auto sourceBar = (g.startBar + k) % total;
        int run = 1;

        while (k + run < g.bars && (g.startBar + k + run) % total == sourceBar + run)
            ++run;

        const auto offset = (int) std::lround ((g.downbeat + sourceBar * bar) * rate);
        const auto destination = (int) std::lround (k * bar * rate);
        const auto count = std::min ({ (int) std::lround (run * bar * rate), range.getLength() - offset, out.getNumSamples() - destination });

        if (offset >= 0 && count > 0)
            for (int ch = 0; ch < out.getNumChannels(); ++ch)
                out.copyFrom (ch, destination, audio, ch, range.getStart() + offset, count);

        k += run;
    }

    return out;
}

juce::String fileTag (const Settings& g)
{
    const auto key = noteName (g.keyTonic, keyUsesFlats (g.keyTonic, g.minor)).replace ("#", "s") + (g.minor ? "m" : "");
    const auto bpm = juce::String (g.bpm, 2).trimCharactersAtEnd ("0").trimCharactersAtEnd (".").replace (".", "_");
    return "Bas_" + key + "_" + bpm + "bpm_" + styleTable()[(size_t) g.style].info.id;
}

int nextScaleRoot (int root, int keyTonic, bool minor)
{
    const auto& sc = scaleSteps (minor);

    for (int i = 0; i < 7; ++i)
        if ((keyTonic + sc[(size_t) i]) % 12 == root)
            return (keyTonic + sc[(size_t) ((i + 1) % 7)]) % 12;

    return keyTonic % 12;
}
} // namespace wf::bass
