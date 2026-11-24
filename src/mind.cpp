#include "mind.hpp"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "config.hpp"

namespace fs = std::filesystem;

namespace mbot {

int64_t nowUnix() { return int64_t(std::time(nullptr)); }

std::string dataDirFor(const std::string& serial) {
    const char* xdg = std::getenv("XDG_DATA_HOME");
    std::string base = xdg && *xdg ? xdg : homeDir() + "/.local/share";
    return base + "/mbot/" + (serial.empty() ? "default" : serial);
}

// ------------------------------------------------------------------ Mood ----

float& Mood::operator[](const std::string& name) {
    for (int i = 0; i < kCount; ++i)
        if (name == kNames[i]) return v[i];
    static float dummy;
    return dummy;
}

float Mood::get(const std::string& name) const {
    for (int i = 0; i < kCount; ++i)
        if (name == kNames[i]) return v[i];
    return 0.f;
}

json Mood::toJson() const {
    json j;
    for (int i = 0; i < kCount; ++i) j[kNames[i]] = std::round(double(v[i]) * 100.0) / 100.0;
    return j;
}

void Mood::fromJson(const json& j) {
    for (int i = 0; i < kCount; ++i)
        if (j.contains(kNames[i])) v[i] = std::clamp(j[kNames[i]].get<float>(), 0.f, 1.f);
}

std::string Mood::describe() const {
    auto level = [](float x) -> const char* {
        if (x > 0.85f) return "extremely";
        if (x > 0.7f) return "very";
        if (x > 0.55f) return "quite";
        if (x > 0.35f) return "somewhat";
        if (x > 0.15f) return "a little";
        return "not at all";
    };
    static const char* adj[] = {"happy",   "curious", "affectionate", "energetic", "bored",
                                "anxious", "sad",     "afraid",       "jealous",   "irritable"};
    std::string s;
    for (int i = 0; i < kCount; ++i) {
        if (i >= 6 && v[i] <= 0.15f) continue;  // the darker feelings only when actually present
        if (!s.empty()) s += ", ";
        s += std::string(level(v[i])) + " " + adj[i];
    }
    return s;
}

// ------------------------------------------------------------------ Fear ----

json Fear::toJson() const {
    return {{"what", what}, {"strength", std::round(double(strength) * 100.0) / 100.0}, {"origin", origin},
            {"since", since}, {"last", last}, {"times", times}};
}
Fear Fear::fromJson(const json& j) {
    Fear f;
    f.what = j.value("what", "");
    f.strength = std::clamp(j.value("strength", 0.f), 0.f, 1.f);
    f.origin = j.value("origin", "");
    f.since = j.value("since", int64_t(0));
    f.last = j.value("last", int64_t(0));
    f.times = j.value("times", 0);
    return f;
}

// ------------------------------------------------------- Memory / Person ----

json Memory::toJson() const {
    return {{"id", id},         {"time", time},     {"text", text}, {"importance", importance},
            {"people", people}, {"kind", kind},     {"feeling", feeling}};
}

Memory Memory::fromJson(const json& j) {
    Memory m;
    m.id = j.value("id", int64_t(0));
    m.time = j.value("time", int64_t(0));
    m.text = j.value("text", "");
    m.importance = j.value("importance", 3);
    m.people = j.value("people", std::vector<std::string>{});
    m.kind = j.value("kind", "episode");
    m.feeling = j.value("feeling", "");
    return m;
}

json Person::toJson() const {
    return {{"name", name},           {"relation", relation}, {"birthday", birthday},
            {"aliases", aliases},     {"protected", protected_},
            {"facts", facts},       {"affinity", affinity},
            {"first_seen", first_seen}, {"last_seen", last_seen}, {"encounters", encounters}};
}

Person Person::fromJson(const json& j) {
    Person p;
    p.name = j.value("name", "");
    p.relation = j.value("relation", "");
    p.birthday = j.value("birthday", "");
    p.aliases = j.value("aliases", std::vector<std::string>{});
    p.protected_ = j.value("protected", false);
    p.facts = j.value("facts", std::vector<std::string>{});
    p.affinity = j.value("affinity", 0.5f);
    p.first_seen = j.value("first_seen", int64_t(0));
    p.last_seen = j.value("last_seen", int64_t(0));
    p.encounters = j.value("encounters", 0);
    return p;
}

// ------------------------------------------------------------------ Mind ----

namespace {

bool atomicWrite(const std::string& path, const std::string& data) {
    std::string tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(tmp, path, ec);
    return !ec;
}

std::set<std::string> words(const std::string& s) {
    static const std::set<std::string> stop = {
        "the", "a",   "an",  "and", "or",   "to",   "of",  "in",   "on",  "is",  "it",
        "i",   "you", "me",  "my",  "your", "we",   "was", "are",  "be",  "that", "this",
        "for", "with", "at", "as",  "do",   "did",  "so",  "but",  "have", "has", "what",
        "just", "can", "he", "she", "they", "them", "his", "her",  "not", "im",  "its"};
    std::set<std::string> out;
    std::string w;
    for (char c : s + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            w += char(std::tolower(static_cast<unsigned char>(c)));
        } else if (!w.empty()) {
            if (w.size() > 2 && !stop.count(w)) out.insert(w);
            w.clear();
        }
    }
    return out;
}

std::string ago(int64_t t) {
    int64_t d = nowUnix() - t;
    char buf[64];
    if (d < 90) return "just now";
    if (d < 3600) std::snprintf(buf, sizeof buf, "%lld min ago", (long long)(d / 60));
    else if (d < 86400) std::snprintf(buf, sizeof buf, "%lld h ago", (long long)(d / 3600));
    else std::snprintf(buf, sizeof buf, "%lld days ago", (long long)(d / 86400));
    return buf;
}

}  // namespace

Mind::Mind(const std::string& dir) : dir_(dir) {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    traits_ = {{"playfulness", 0.75}, {"curiosity", 0.8},  {"warmth", 0.7},
               {"sass", 0.45},        {"bravery", 0.55},   {"chattiness", 0.5}};
    load();
    if (asleep_since_ > 0) {
        // Feelings keep fading while the mind is switched off (capped at a day).
        double off = double(std::min<int64_t>(nowUnix() - asleep_since_, 86400));
        if (off > 0) {
            double k = 1.0 - std::exp(-off * std::log(2.0) / 480.0);
            for (int i = 0; i < Mood::kCount; ++i)
                mood_.v[i] += float((baseline_.v[i] - mood_.v[i]) * k);
        }
    }
    if (born_ == 0) {
        born_ = nowUnix();
        narrative_ =
            "I am a small robot who just woke up with a new mind. I don't remember much yet, "
            "but I am eager to learn about my home and the people in it.";
        saveLocked();
    }
}

void Mind::load() {
    std::lock_guard<std::mutex> lk(mu_);
    try {
        auto s = readFile(path("self.json"));
        if (!s.empty()) {
            auto j = json::parse(s);
            name_ = j.value("name", name_);
            if (j.contains("traits")) traits_.update(j["traits"]);
            narrative_ = j.value("narrative", "");
            born_ = j.value("born", int64_t(0));
            last_reflected_id_ = j.value("last_reflected_id", int64_t(0));
            if (j.contains("mood")) mood_.fromJson(j["mood"]);
            asleep_since_ = j.value("saved_at", int64_t(0));
            if (j.contains("baseline")) baseline_.fromJson(j["baseline"]);
            for (auto& f : j.value("fears", json::array())) fears_.push_back(Fear::fromJson(f));
            if (j.contains("dialogue"))
                for (auto& d : j["dialogue"])
                    dialogue_.push_back({d[0], d[1], d.size() > 2 ? int64_t(d[2]) : int64_t(0)});
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[mind] self.json unreadable (%s), starting fresh\n", e.what());
    }
    try {
        auto s = readFile(path("people.json"));
        if (!s.empty())
            for (auto& p : json::parse(s)) {
                auto person = Person::fromJson(p);
                if (isRealName(person.name)) people_[person.name] = person;
            }
    } catch (...) {
    }
    try {
        auto s = readFile(path("pronunciations.json"));
        if (!s.empty()) {
            json j = json::parse(s);
            for (auto& [k, v] : j.items()) say_as_[k] = v.get<std::string>();
        }
    } catch (...) {
    }
    // Merge people stored under one of someone else's aliases (e.g. "Kathryn" -> Catherine).
    {
        auto low = [](std::string x) {
            for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
            return x;
        };
        std::map<std::string, std::string> alias_to;
        for (auto& [n, p] : people_)
            for (auto& a : p.aliases) alias_to[low(a)] = n;
        for (auto it = people_.begin(); it != people_.end();) {
            auto t = alias_to.find(low(it->first));
            if (t != alias_to.end() && t->second != it->first && people_.count(t->second)) {
                auto& into = people_[t->second];
                for (auto& f : it->second.facts)
                    if (std::find(into.facts.begin(), into.facts.end(), f) == into.facts.end()) into.facts.push_back(f);
                into.encounters += it->second.encounters;
                into.last_seen = std::max(into.last_seen, it->second.last_seen);
                it = people_.erase(it);
            } else {
                ++it;
            }
        }
    }
    std::istringstream in(readFile(path("memories.jsonl")));
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        try {
            memories_.push_back(Memory::fromJson(json::parse(line)));
            next_id_ = std::max(next_id_, memories_.back().id + 1);
        } catch (...) {
        }
    }
    for (auto* f : {"self.json", "people.json", "memories.jsonl", "pronunciations.json"}) noteWritten(f);
}

int64_t Mind::fileMtime(const std::string& file) const {
    std::error_code ec;
    auto mt = fs::last_write_time(path(file.c_str()), ec);
    return ec ? 0 : int64_t(mt.time_since_epoch().count());
}

void Mind::noteWritten(const std::string& file) { mtimes_[file] = fileMtime(file); }

Mind::EditReport Mind::reloadExternalEdits() {
    std::lock_guard<std::mutex> lk(mu_);
    reloadLocked();
    EditReport r = std::move(pending_);
    pending_ = {};
    return r;
}

bool Mind::isBroken(const std::string& file) const {
    auto it = broken_.find(file);
    return it != broken_.end() && it->second == fileMtime(file);
}

namespace {
// "line 12, column 5: unexpected closing brace" from a JSON parse error, in plain words a robot can say.
std::string describeJsonError(const std::exception& e, int line_offset = 0) {
    std::string w = e.what(), where, why;
    auto at = w.find("at line ");
    if (at != std::string::npos) {
        int ln = 0, col = 0;
        if (std::sscanf(w.c_str() + at, "at line %d, column %d", &ln, &col) == 2)
            where = "line " + std::to_string(ln + line_offset) + ", column " + std::to_string(col);
    }
    auto dash = w.find(" - ");
    if (dash != std::string::npos) {
        why = w.substr(dash + 3);
        auto semi = why.find(';');
        if (semi != std::string::npos) why = why.substr(0, semi);
    }
    for (auto [from, to] : std::initializer_list<std::pair<const char*, const char*>>{
             {"'}'", "closing brace"}, {"'{'", "opening brace"}, {"']'", "closing bracket"},
             {"'['", "opening bracket"}, {"','", "comma"}, {"':'", "colon"},
             {"end of input", "the end of the file (something is missing)"}}) {
        for (size_t p; (p = why.find(from)) != std::string::npos;) why.replace(p, std::strlen(from), to);
    }
    if (why.find("unexpected string literal") != std::string::npos)
        why = "unexpected text - maybe a comma is missing just before it";
    else if (why.find("literal") != std::string::npos)
        why.replace(why.find("literal"), 7, "value");
    if (where.empty() && why.empty()) return "it doesn't make sense";
    return where + (where.empty() || why.empty() ? "" : ": ") + why;
}
}  // namespace

// Reload files edited outside the brain. Each is parsed completely first; a file that doesn't parse
// (e.g. saved half-way through an edit) is left alone and retried next time.
bool Mind::reloadLocked() {
    bool any = false;
    auto changed = [&](const char* f) {
        int64_t m = fileMtime(f);
        if (m == 0 || m == mtimes_[f]) return false;
        return !(broken_.count(f) && broken_[f] == m);  // already reported this broken version
    };
    auto ok = [&](const char* f) {
        noteWritten(f);
        broken_.erase(f);
        any = true;
        pending_.reloaded.push_back(f);
    };
    auto bad = [&](const char* f, const std::string& why) {
        broken_[f] = fileMtime(f);
        std::fprintf(stderr, "[mind] %s was edited but can't be read (%s) - keeping my copy, not overwriting it\n", f,
                     why.c_str());
        pending_.errors.push_back({f, why});
    };
    if (changed("self.json")) {
        try {
            json j = json::parse(readFile(path("self.json")));
            std::vector<std::tuple<std::string, std::string, int64_t>> dlg;
            for (auto& d : j.value("dialogue", json::array()))
                dlg.push_back({d[0], d[1], d.size() > 2 ? int64_t(d[2]) : int64_t(0)});
            name_ = j.value("name", name_);
            if (j.contains("traits")) traits_.update(j["traits"]);
            narrative_ = j.value("narrative", narrative_);
            born_ = j.value("born", born_);
            last_reflected_id_ = j.value("last_reflected_id", last_reflected_id_);
            if (j.contains("mood")) mood_.fromJson(j["mood"]);
            if (j.contains("baseline")) baseline_.fromJson(j["baseline"]);
            if (j.contains("fears")) {
                fears_.clear();
                for (auto& f : j["fears"]) fears_.push_back(Fear::fromJson(f));
            }
            dialogue_ = dlg;
            ok("self.json");
        } catch (const std::exception& e) { bad("self.json", describeJsonError(e)); }
    }
    if (changed("people.json")) {
        try {
            std::map<std::string, Person> np;
            for (auto& pj : json::parse(readFile(path("people.json")))) {
                Person p = Person::fromJson(pj);
                if (isRealName(p.name)) np[p.name] = p;
            }
            people_ = np;
            ok("people.json");
        } catch (const std::exception& e) { bad("people.json", describeJsonError(e)); }
    }
    if (changed("memories.jsonl")) {
        try {
            std::vector<Memory> nm;
            int64_t next = 1;
            std::istringstream in(readFile(path("memories.jsonl")));
            std::string line;
            int lineno = 0;
            while (std::getline(in, line)) {
                ++lineno;
                if (line.empty()) continue;
                try {
                    nm.push_back(Memory::fromJson(json::parse(line)));
                } catch (const std::exception& e) {  // one memory per line: report the file line
                    throw std::runtime_error(describeJsonError(e, lineno - 1));
                }
                next = std::max(next, nm.back().id + 1);
            }
            memories_ = nm;
            next_id_ = std::max(next_id_, next);  // never reuse an id
            ok("memories.jsonl");
        } catch (const std::runtime_error& e) { bad("memories.jsonl", e.what());
        } catch (const std::exception& e) { bad("memories.jsonl", describeJsonError(e)); }
    }
    if (changed("pronunciations.json")) {
        try {
            std::map<std::string, std::string> sa;
            json pj = json::parse(readFile(path("pronunciations.json")));  // keep alive: .items() of a temporary dangles
            for (auto& [k, v] : pj.items()) sa[k] = v.get<std::string>();
            say_as_ = sa;
            ok("pronunciations.json");
        } catch (const std::exception& e) { bad("pronunciations.json", describeJsonError(e)); }
    }
    return any;
}

void Mind::save() {
    std::lock_guard<std::mutex> lk(mu_);
    saveLocked();
}

void Mind::saveLocked() {
    reloadLocked();  // someone edited a mind file outside the brain: take their version, don't overwrite it
    // Momentary observations (what someone wore, where they were, what they were doing) are kept as dated
    // memories and are the first to go: dropped after 2 days.
    {
        int64_t cutoff = nowUnix() - 2 * 86400;
        memories_.erase(std::remove_if(memories_.begin(), memories_.end(),
                                       [&](const Memory& m) { return m.kind == "observation" && m.time < cutoff; }),
                        memories_.end());
    }
    json d = json::array();
    for (auto& [who, text, t] : dialogue_) d.push_back({who, text, t});
    json self = {{"name", name_},
                 {"traits", traits_},
                 {"narrative", narrative_},
                 {"born", born_},
                 {"last_reflected_id", last_reflected_id_},
                 {"mood", mood_.toJson()},
                 {"saved_at", nowUnix()},
                 {"baseline", baseline_.toJson()},
                 {"fears", [&] { json a = json::array(); for (auto& f : fears_) a.push_back(f.toJson()); return a; }()},
                 {"dialogue", d}};
    if (!isBroken("self.json")) { atomicWrite(path("self.json"), self.dump(2)); noteWritten("self.json"); }
    json p = json::array();
    for (auto& [_, person] : people_) p.push_back(person.toJson());
    if (!isBroken("people.json")) { atomicWrite(path("people.json"), p.dump(2)); noteWritten("people.json"); }
    std::string mem;
    for (auto& m : memories_) mem += m.toJson().dump() + "\n";
    if (!isBroken("memories.jsonl")) { atomicWrite(path("memories.jsonl"), mem); noteWritten("memories.jsonl"); }
}

std::string Mind::selfDescription() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream ss;
    ss << "Your name is " << name_ << ". Personality traits (0..1): " << traits_.dump() << ".\n";
    int64_t age_days = (nowUnix() - born_) / 86400;
    ss << "Your current mind has existed for " << age_days << " day(s) and holds "
       << memories_.size() << " memories.\n";
    ss << "Your own story so far: " << narrative_;
    return ss.str();
}

void Mind::setNarrative(const std::string& n) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!n.empty()) narrative_ = n;
    saveLocked();
}

void Mind::nudgeTraits(const json& deltas) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!deltas.is_object()) return;
    for (auto& [k, v] : deltas.items()) {
        if (!traits_.contains(k) || !v.is_number()) continue;
        // Personality changes slowly: cap each nudge.
        float d = std::clamp(v.get<float>(), -0.05f, 0.05f);
        traits_[k] = std::clamp(traits_[k].get<float>() + d, 0.f, 1.f);
    }
    saveLocked();
}

Mood Mind::mood() const {
    std::lock_guard<std::mutex> lk(mu_);
    return mood_;
}

void Mind::feel(const std::string& emotion, float delta) {
    std::lock_guard<std::mutex> lk(mu_);
    float& x = mood_[emotion];
    x = std::clamp(x + delta, 0.f, 1.f);
}

void Mind::feelAll(const json& deltas) {
    if (!deltas.is_object()) return;
    for (auto& [k, v] : deltas.items()) {
        if (!v.is_number()) continue;
        float x = v.get<float>();
        if (std::fabs(x) <= 0.3f) {
            feel(k, x);  // a change, as asked
        } else if (x > 0.3f && x <= 1.f) {
            // Smaller models often answer with an absolute level instead of a
            // change: drift a third of the way toward it.
            float cur = mood().get(k);
            feel(k, (x - cur) / 3.f);
        }
    }
}

void Mind::tick(double dt) {
    std::lock_guard<std::mutex> lk(mu_);
    // Exponential return to baseline, half-life ~8 minutes.
    double k = 1.0 - std::exp(-dt * std::log(2.0) / 480.0);
    for (int i = 0; i < Mood::kCount; ++i) mood_.v[i] += float((baseline_.v[i] - mood_.v[i]) * k);
    // Slow drives: boredom creeps up when nothing happens (reset by stimuli),
    // energy sags when the battery is low.
    mood_["boredom"] = std::min(1.f, mood_["boredom"] + float(dt / 1800.0));
    if (battery_low_) mood_["energy"] = std::max(0.f, mood_["energy"] - float(dt / 600.0));
    // Fears that aren't triggered slowly fade (a strong one takes a few weeks); faded ones are let go.
    // (~0.017/day: a mild fear fades in ~9 days, a strong one takes 6-7 weeks without being triggered)
    for (auto& f : fears_) f.strength = std::max(0.f, f.strength - float(dt / (30.0 * 86400.0)) * 0.5f);
    fears_.erase(std::remove_if(fears_.begin(), fears_.end(), [](const Fear& f) { return f.strength < 0.04f; }),
                 fears_.end());
}

void Mind::feelFear(const std::string& what, float delta, const std::string& why) {
    if (what.empty() || delta == 0.f) return;
    std::lock_guard<std::mutex> lk(mu_);
    delta = std::clamp(delta, -0.3f, 0.3f);
    auto w = words(what);
    Fear* hit = nullptr;
    for (auto& f : fears_) {  // same fear in other words? ("the table edge" / "the edge of the table")
        auto fw = words(f.what);
        size_t inter = 0;
        for (auto& x : w) inter += fw.count(x);
        size_t uni = w.size() + fw.size() - inter;
        if (uni > 0 && double(inter) / double(uni) >= 0.5) { hit = &f; break; }
    }
    if (!hit) {
        if (delta <= 0) return;  // can't be reassured about a fear he doesn't have
        fears_.push_back(Fear{what, 0.f, why, nowUnix(), 0, 0});
        hit = &fears_.back();
    }
    hit->strength = std::clamp(hit->strength + delta, 0.f, 1.f);
    if (delta > 0) {
        hit->last = nowUnix();
        hit->times++;
        mood_["fear"] = std::clamp(mood_["fear"] + delta * (0.5f + hit->strength), 0.f, 1.f);  // stronger fear, stronger jolt
    }
    if (hit->strength < 0.04f) fears_.erase(fears_.begin() + (hit - fears_.data()));
}

std::vector<Fear> Mind::fears() const {
    std::lock_guard<std::mutex> lk(mu_);
    return fears_;
}

std::string Mind::fearsSummary() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out;
    int64_t now = nowUnix();
    for (auto& f : fears_) {
        const char* lvl = f.strength > 0.7f ? "strong" : f.strength > 0.4f ? "moderate" : "mild";
        int64_t ago = f.last ? now - f.last : -1;
        std::string when = ago < 0 ? "" : ago < 3600 ? ", felt it just now" : ago < 86400 ? ", felt it today"
                                     : ", last felt " + std::to_string(ago / 86400) + " days ago";
        out += (out.empty() ? "" : "; ") + f.what + " (" + lvl + when + ")";
    }
    return out;
}

void Mind::eyeColor(float& hue, float& sat) const {
    Mood m = mood();
    // Hue wheel: 0.0 red, 0.08 orange, 0.16 yellow, 0.33 green, 0.5 cyan, 0.66 blue, 0.83 purple.
    // Pick the dominant feeling and blend toward its colour.
    struct { float w; float h; } c[] = {
        {m.get("happiness"), 0.13f},  // warm yellow-orange
        {m.get("curiosity"), 0.45f},  // teal
        {m.get("affection"), 0.90f},  // pink
        {m.get("anxiety") * 1.3f, 0.66f},  // blue
        {m.get("boredom") * 0.8f, 0.75f},  // violet
        {m.get("sadness") * 1.4f, 0.62f},  // deep blue
        {m.get("fear") * 1.5f, 0.55f},     // pale cyan
        {m.get("jealousy") * 1.4f, 0.30f}, // green
        {m.get("irritation") * 1.4f, 0.03f},  // orange-red
    };
    float best = -1;
    hue = 0.3f;
    for (auto& e : c)
        if (e.w > best) { best = e.w; hue = e.h; }
    sat = std::clamp(0.35f + 0.65f * m.get("energy"), 0.2f, 1.f);
}

int64_t Mind::remember(const std::string& text, int importance,
                       const std::vector<std::string>& people, const std::string& kind) {
    if (text.empty()) return 0;
    Memory m;
    {
        std::lock_guard<std::mutex> lk(mu_);
        reloadLocked();  // take in a hand edit first, so appending doesn't hide it
        // Skip near-duplicates (models like to re-store what they were reminded of);
        // instead let the existing memory gain importance.
        auto w = words(text);
        for (auto it = memories_.rbegin(); it != memories_.rend(); ++it) {
            auto mw = words(it->text);
            size_t inter = 0;
            for (auto& x : w) inter += mw.count(x);
            size_t uni = w.size() + mw.size() - inter;
            if (uni > 0 && double(inter) / double(uni) >= 0.6) {
                it->importance = std::max(it->importance, std::clamp(importance, 1, 10));
                return it->id;
            }
        }
        m.id = next_id_++;
        m.time = nowUnix();
        m.text = text;
        m.importance = std::clamp(importance, 1, 10);
        m.people = people;
        m.kind = kind;
        m.feeling = mood_.describe();
        memories_.push_back(m);
        // Append (cheap), then note it as our own write so it isn't mistaken for a hand edit. If the file is
        // a broken hand edit, leave it alone; the memory is written with the next save once it's fixed.
        if (!isBroken("memories.jsonl")) {
            { std::ofstream f(path("memories.jsonl"), std::ios::app); f << m.toJson().dump() << "\n"; }
            noteWritten("memories.jsonl");
        }
    }
    return m.id;
}

std::vector<Memory> Mind::recall(const std::string& context, const std::vector<std::string>& people,
                                 size_t k) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto ctx = words(context);
    int64_t now = nowUnix();
    std::vector<std::pair<double, const Memory*>> scored;
    for (auto& m : memories_) {
        double hours = double(now - m.time) / 3600.0;
        double recency = std::exp(-hours / 72.0);  // fades over days
        double imp = m.importance / 10.0;
        auto mw = words(m.text);
        int overlap = 0;
        for (auto& w : ctx) overlap += int(mw.count(w));
        double rel = ctx.empty() ? 0 : std::min(1.0, overlap / 3.0);
        double person = 0;
        for (auto& p : people)
            if (std::find(m.people.begin(), m.people.end(), p) != m.people.end()) person = 1;
        double bonus = m.kind == "reflection" ? 0.2 : 0;
        scored.push_back({recency + imp + 1.5 * rel + 1.0 * person + bonus, &m});
    }
    std::sort(scored.begin(), scored.end(), [](auto& a, auto& b) { return a.first > b.first; });
    std::vector<Memory> out;
    for (size_t i = 0; i < scored.size() && out.size() < k; ++i) out.push_back(*scored[i].second);
    // Present oldest first so they read like a story.
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.time < b.time; });
    return out;
}

std::vector<Memory> Mind::since(int64_t id) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<Memory> out;
    for (auto& m : memories_)
        if (m.id > id) out.push_back(m);
    return out;
}

void Mind::markReflected(int64_t id) {
    std::lock_guard<std::mutex> lk(mu_);
    last_reflected_id_ = id;
    saveLocked();
}

size_t Mind::memoryCount() const {
    std::lock_guard<std::mutex> lk(mu_);
    return memories_.size();
}

void Mind::sawPerson(const std::string& raw) {
    if (!isRealName(raw) || raw == name_) return;
    std::string name = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto& p = people_[name];
    int64_t now = nowUnix();
    if (p.name.empty()) {
        p.name = name;
        p.first_seen = now;
    }
    bool changed = p.first_seen == now;
    if (now - p.last_seen > 600) {  // a new "visit"
        p.encounters++;
        changed = true;
    }
    p.last_seen = now;
    if (changed) saveLocked();  // otherwise persisted with the next regular save
}

bool isRealName(const std::string& n) {
    if (n.size() < 2 || n.size() > 40) return false;
    // Model mistakes: placeholders, brackets, action words, numbered "unknown" people, and
    // sentence fragments ("Sam's cube tapped", "Sam or Alex").
    if (n.find_first_of("[]{}()<>/\\:=_|#@*\"',.!?;") != std::string::npos) return false;
    for (char c : n)
        if (std::isdigit(static_cast<unsigned char>(c))) return false;
    if (!std::isupper(static_cast<unsigned char>(n[0]))) return false;  // names are capitalised
    // Split into words; a real (possibly multi-word) name is <=3 words, each either capitalised
    // or a short lowercase particle (van, de, von...). A lowercase word like "or"/"cube"/"tapped"
    // means it's a phrase, not a name.
    static const std::set<std::string> particles = {"van", "von", "de", "der", "den", "da",
                                                     "di", "la", "le", "du", "del", "bin", "al"};
    std::vector<std::string> words;
    std::string cur;
    for (char c : n) {
        if (c == ' ') { if (!cur.empty()) words.push_back(cur); cur.clear(); }
        else cur += c;
    }
    if (!cur.empty()) words.push_back(cur);
    if (words.empty() || words.size() > 3) return false;
    for (auto& w : words) {
        std::string lw;
        for (char c : w) lw += char(std::tolower(static_cast<unsigned char>(c)));
        bool cap = std::isupper(static_cast<unsigned char>(w[0]));
        if (!cap && !particles.count(lw)) return false;  // a lowercase non-particle word = a phrase
    }
    std::string l;
    for (char c : n) l += char(std::tolower(static_cast<unsigned char>(c)));
    static const char* placeholders[] = {"someone", "someone unknown", "unknown", "you", "me",
                                         "user", "human", "person", "a person", "stranger",
                                         "vector", "the user", "nobody", "everyone", "they",
                                         "speaker", "owner", "friend", "unknown person", "look",
                                         "learn face", "learn_face", "anim", "turn", "drive",
                                         "none", "null", "nothing", "nan", "na", "true", "false",
                                         "yes", "no", "maybe", "someone else", "no one"};
    for (auto* p : placeholders)
        if (l == p) return false;
    if (l.rfind("unknown", 0) == 0 || l.rfind("someone", 0) == 0 || l.rfind("person", 0) == 0) return false;
    return true;
}

void Mind::knowPerson(const std::string& raw) {
    if (!isRealName(raw) || raw == name_) return;
    std::string name = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto& p = people_[name];
    if (p.name.empty()) {
        p.name = name;
        p.first_seen = nowUnix();
        saveLocked();
    }
}

namespace {
std::string replaceWord(std::string t, const std::string& from, const std::string& to);  // defined below
}

void Mind::learnFact(const std::string& raw, const std::string& fact) {
    if (!isRealName(raw) || fact.empty() || raw == name_) return;
    std::string person = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    // A "fact" that is really about someone else (names them, not this person) belongs to them.
    {
        auto has = [&](const std::string& n) { return replaceWord(fact, n, "\x01") != fact; };
        // ...or about himself ("Vector just rolled off the charger"): that's a moment, not a fact about them.
        if (!has(person) && has(name_)) return;
        if (!has(person)) {
            std::string other;
            int n_others = 0;
            for (auto& [k, _] : people_)
                if (k != person && has(k)) { other = k; ++n_others; }
            if (n_others == 1) person = other;
        }
    }
    // Skip near-duplicates of something already known (word overlap).
    {
        auto w = words(fact);
        for (auto& f : people_[person].facts) {
            auto fw = words(f);
            size_t inter = 0;
            for (auto& x : w) inter += fw.count(x);
            size_t uni = w.size() + fw.size() - inter;
            if (uni > 0 && double(inter) / double(uni) >= 0.6) return;
        }
    }
    auto& p = people_[person];
    if (p.name.empty()) {
        p.name = person;
        p.first_seen = nowUnix();
    }
    if (std::find(p.facts.begin(), p.facts.end(), fact) == p.facts.end()) p.facts.push_back(fact);
    if (p.facts.size() > 40) p.facts.erase(p.facts.begin());
    saveLocked();
}

void Mind::adjustAffinity(const std::string& raw, float delta) {
    std::string person = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = people_.find(person);
    if (it == people_.end()) return;
    it->second.affinity = std::clamp(it->second.affinity + std::clamp(delta, -0.1f, 0.1f), 0.f, 1.f);
    saveLocked();
}

std::string Mind::describePeople(const std::vector<std::string>& names) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::ostringstream ss;
    for (auto& n : names) {
        auto it = people_.find(n);
        if (it == people_.end()) continue;
        auto& p = it->second;
        ss << "- " << p.name;
        if (!p.relation.empty()) ss << " (" << p.relation << ")";
        if (!p.aliases.empty()) {
            ss << " a.k.a.";
            for (auto& a : p.aliases) ss << " " << a;
        }
        ss << ": affinity " << std::round(double(p.affinity) * 100.0) / 100.0;
        if (p.last_seen > 0)
            ss << ", seen in person " << p.encounters << " time(s), last " << ago(p.last_seen);
        else
            ss << ", known from conversation (face not yet seen)";
        ss << ".";
        if (!p.facts.empty()) {
            ss << " Known facts: ";
            for (size_t i = 0; i < p.facts.size(); ++i) ss << (i ? "; " : "") << p.facts[i];
            ss << ".";
        }
        ss << "\n";
    }
    return ss.str();
}

std::vector<std::string> Mind::knownPeople() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::string> out;
    for (auto& [n, _] : people_) out.push_back(n);
    return out;
}

void Mind::logDialogue(const std::string& who, const std::string& text) {
    if (text.empty()) return;
    {
        std::lock_guard<std::mutex> lk(mu_);
        dialogue_.push_back({who, text, int64_t(std::time(nullptr))});
        if (dialogue_.size() > 30) dialogue_.erase(dialogue_.begin());
    }
    std::ofstream f(path("journal.log"), std::ios::app);
    std::time_t t = std::time(nullptr);
    char ts[32];
    std::strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    f << ts << "  " << who << ": " << text << "\n";
}

void Mind::setPronunciation(const std::string& word, const std::string& spoken) {
    std::lock_guard<std::mutex> lk(mu_);
    if (spoken.empty()) say_as_.erase(word);
    else say_as_[word] = spoken;
    json j = json::object();
    for (auto& [k, v] : say_as_) j[k] = v;
    if (!isBroken("pronunciations.json")) { atomicWrite(path("pronunciations.json"), j.dump(2)); noteWritten("pronunciations.json"); }
}

std::map<std::string, std::string> Mind::pronunciations() const {
    std::lock_guard<std::mutex> lk(mu_);
    return say_as_;
}

std::string Mind::spoken(const std::string& text) const {
    std::lock_guard<std::mutex> lk(mu_);
    std::string out = text;
    auto isWordChar = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    for (auto& [word, say] : say_as_) {
        size_t pos = 0;
        while ((pos = out.find(word, pos)) != std::string::npos) {
            bool left = pos == 0 || !isWordChar(out[pos - 1]);
            size_t end = pos + word.size();
            bool right = end >= out.size() || !isWordChar(out[end]);  // "Sam's" still matches
            if (left && right) {
                out.replace(pos, word.size(), say);
                pos += say.size();
            } else {
                pos = end;
            }
        }
    }
    return out;
}

namespace {
int levenshtein(const std::string& a, const std::string& b) {
    std::vector<int> d(b.size() + 1);
    for (size_t j = 0; j <= b.size(); ++j) d[j] = int(j);
    for (size_t i = 1; i <= a.size(); ++i) {
        int prev = d[0];
        d[0] = int(i);
        for (size_t j = 1; j <= b.size(); ++j) {
            int cur = d[j];
            d[j] = std::min({d[j] + 1, d[j - 1] + 1, prev + (a[i - 1] == b[j - 1] ? 0 : 1)});
            prev = cur;
        }
    }
    return d[b.size()];
}
}  // namespace

namespace {
// Rough sound key for names, so a mishearing maps to someone known: "Catherine" and "Kathryn" -> "ktrn",
// "Victor" and "Vector" -> "fktr". Vowels/h/w dropped after the first letter, similar consonants merged.
std::string soundKey(const std::string& raw) {
    std::string s;
    for (char c : raw)
        if (std::isalpha(static_cast<unsigned char>(c))) s += char(std::tolower(static_cast<unsigned char>(c)));
    std::string k;
    auto vowel = [](char c) { return c && std::strchr("aeiouy", c) != nullptr; };
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i], nx = i + 1 < s.size() ? s[i + 1] : 0, out;
        if (i == 0 && vowel(c)) out = 'a';
        else if (vowel(c) || c == 'h' || c == 'w') continue;
        else if (c == 'p' && nx == 'h') { out = 'f'; ++i; }
        else if (c == 'c' && nx == 'k') { out = 'k'; ++i; }
        else if (c == 'c') out = (nx == 'e' || nx == 'i' || nx == 'y') ? 's' : 'k';
        else if (c == 'q') out = 'k';
        else if (c == 'z') out = 's';
        else if (c == 'v') out = 'f';
        else if (c == 'x') { k += 'k'; out = 's'; }
        else out = c;
        if (k.empty() || k.back() != out) k += out;
    }
    return k;
}

// Replace whole-word occurrences ("Maurice" but not "Mauricette"); "Maurice's" still matches.
std::string replaceWord(std::string t, const std::string& from, const std::string& to) {
    if (from.empty()) return t;
    auto wc = [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0; };
    size_t pos = 0;
    while ((pos = t.find(from, pos)) != std::string::npos) {
        size_t end = pos + from.size();
        if ((pos == 0 || !wc(t[pos - 1])) && (end >= t.size() || !wc(t[end]))) {
            t.replace(pos, from.size(), to);
            pos += to.size();
        } else {
            pos = end;
        }
    }
    return t;
}
}  // namespace

std::string Mind::canonicalName(const std::string& name) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto low = [](std::string x) {
        for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
        return x;
    };
    std::string n = low(name);
    // 1. exact match on a known name or alias.
    for (auto& [key, p] : people_) {
        if (low(key) == n) return key;
        for (auto& a : p.aliases)
            if (low(a) == n) return key;
    }
    // 2. fuzzy match: speech-to-text mishears names (Catherine -> Catharine/Katherine).
    //    Map a heard name to an existing person when it's clearly a mishearing:
    //    same first letter and a small edit distance relative to length.
    std::string best;
    int bestD = 1000;
    auto consider = [&](const std::string& cand, const std::string& key) {
        std::string c = low(cand);
        if (c.empty() || n.empty() || c[0] != n[0]) return;  // same first sound
        size_t maxlen = std::max(n.size(), c.size());
        if (maxlen < 4) return;
        int d = levenshtein(n, c);
        int limit = maxlen <= 5 ? 1 : 2;  // "anna" vs "alex" won't match; "jhon" vs "john" will
        if (d <= limit && d < bestD) {
            bestD = d;
            best = key;
        }
    };
    for (auto& [key, p] : people_) {
        consider(key, key);
        for (auto& a : p.aliases) consider(a, key);
    }
    if (!best.empty()) return best;
    // 3. sound-alike: "Kathryn" for "Catherine" is several edits apart but sounds the same. Only when the
    //    sound key is long enough to mean something, and exactly one known person matches.
    std::string hk = soundKey(name);
    if (hk.size() >= 3) {
        std::string match;
        for (auto& [key, p] : people_) {
            bool hit = soundKey(key) == hk;
            for (auto& a : p.aliases) hit = hit || soundKey(a) == hk;
            if (!hit) continue;
            if (!match.empty() && match != key) return name;  // ambiguous: don't guess
            match = key;
        }
        if (!match.empty()) return match;
    }
    return name;
}

int Mind::retractFact(const std::string& raw, const std::string& wrong) {
    if (wrong.empty()) return 0;
    std::string person = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = people_.find(person);
    if (it == people_.end()) return 0;
    auto low = [](std::string x) { for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c))); return x; };
    auto w = words(wrong);
    auto& facts = it->second.facts;
    size_t before = facts.size();
    facts.erase(std::remove_if(facts.begin(), facts.end(), [&](const std::string& f) {
                    if (low(f).find(low(wrong)) != std::string::npos) return true;
                    auto fw = words(f);
                    size_t inter = 0;
                    for (auto& x : w) inter += fw.count(x);
                    return !w.empty() && double(inter) / double(w.size()) >= 0.6;  // most of the wrong claim is in it
                }),
                facts.end());
    int removed = int(before - facts.size());
    if (removed) saveLocked();
    return removed;
}

std::string Mind::ownerName() const {
    std::lock_guard<std::mutex> lk(mu_);
    for (auto& [n, p] : people_) {
        std::string r = p.relation;
        for (auto& c : r) c = char(std::tolower(static_cast<unsigned char>(c)));
        if (r.find("owner") != std::string::npos) return n;
    }
    for (auto& [n, p] : people_)
        if (p.protected_) return n;
    return "";
}

std::vector<std::string> Mind::factsOf(const std::string& raw) const {
    std::string name = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = people_.find(name);
    return it == people_.end() ? std::vector<std::string>{} : it->second.facts;
}

std::string Mind::soundsLikeKnown(const std::string& heard) const {
    if (!isRealName(heard) || heard == name_) return "";
    std::string c = canonicalName(heard);
    if (c == heard) return "";
    std::lock_guard<std::mutex> lk(mu_);
    auto low = [](std::string x) { for (auto& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch))); return x; };
    auto it = people_.find(c);
    if (it == people_.end()) return "";
    for (auto& a : it->second.aliases)
        if (low(a) == low(heard)) return "";  // a real alias, not a mishearing
    return low(c) == low(heard) ? "" : c;
}

void Mind::addNewPerson(const std::string& name) {
    if (!isRealName(name) || name == name_) return;
    std::lock_guard<std::mutex> lk(mu_);
    if (people_.count(name)) return;
    Person p;
    p.name = name;
    p.first_seen = nowUnix();
    people_[name] = p;
    saveLocked();
}

// Swap a person's name everywhere it appears (callers hold mu_).
static void renameEverywhere(std::vector<Memory>& memories,
                             std::vector<std::tuple<std::string, std::string, int64_t>>& dialogue,
                             std::map<std::string, Person>& people, std::string& narrative,
                             const std::string& from, const std::string& to) {
    for (auto& m : memories) {
        for (auto& n : m.people) if (n == from) n = to;
        std::sort(m.people.begin(), m.people.end());
        m.people.erase(std::unique(m.people.begin(), m.people.end()), m.people.end());
        m.text = replaceWord(m.text, from, to);
    }
    for (auto& d : dialogue)
        if (std::get<0>(d) == from) std::get<0>(d) = to;  // speaker label; the words stay verbatim
    for (auto& [_, p] : people)
        for (auto& f : p.facts) f = replaceWord(f, from, to);
    narrative = replaceWord(narrative, from, to);
}

bool Mind::renamePerson(const std::string& fromRaw, const std::string& to, std::string* why) {
    if (!isRealName(to) || to == name_) { if (why) *why = "'" + to + "' isn't a real name"; return false; }
    std::string from = canonicalName(fromRaw);
    std::string existing;
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!people_.count(from)) { if (why) *why = "I don't know anyone called " + fromRaw; return false; }
        if (from == to) { if (why) *why = "that's already their name"; return false; }
        if (people_.count(to)) existing = to;
    }
    if (!existing.empty()) return mergePeople(from, existing, why);  // renaming onto someone known = merge
    std::lock_guard<std::mutex> lk(mu_);
    Person p = people_[from];
    people_.erase(from);
    p.name = to;
    people_[to] = p;
    renameEverywhere(memories_, dialogue_, people_, narrative_, from, to);
    saveLocked();
    return true;
}

bool Mind::mergePeople(const std::string& fromRaw, const std::string& intoRaw, std::string* why) {
    std::string into = canonicalName(intoRaw);
    std::lock_guard<std::mutex> lk(mu_);
    // The duplicate is matched exactly (case-insensitive) - sound-alike matching would map it onto `into`.
    std::string from;
    auto low = [](std::string x) { for (auto& ch : x) ch = char(std::tolower(static_cast<unsigned char>(ch))); return x; };
    for (auto& [k, _] : people_) if (low(k) == low(fromRaw)) from = k;
    if (from.empty() || !people_.count(into)) {
        if (why) *why = "I need two people I know (" + fromRaw + ", " + intoRaw + ")";
        return false;
    }
    if (from == into) { if (why) *why = "they're already the same person"; return false; }
    Person& a = people_[from];
    Person& b = people_[into];
    if (a.protected_ && !b.protected_) { if (why) *why = from + " is protected"; return false; }
    for (auto& f : a.facts)
        if (std::find(b.facts.begin(), b.facts.end(), f) == b.facts.end()) b.facts.push_back(f);
    while (b.facts.size() > 40) b.facts.erase(b.facts.begin());
    for (auto& al : a.aliases)
        if (al != into && std::find(b.aliases.begin(), b.aliases.end(), al) == b.aliases.end()) b.aliases.push_back(al);
    if (b.relation.empty()) b.relation = a.relation;
    if (b.birthday.empty()) b.birthday = a.birthday;
    b.affinity = std::max(a.affinity, b.affinity);
    if (a.first_seen && (!b.first_seen || a.first_seen < b.first_seen)) b.first_seen = a.first_seen;
    b.last_seen = std::max(a.last_seen, b.last_seen);
    b.encounters += a.encounters;
    people_.erase(from);
    renameEverywhere(memories_, dialogue_, people_, narrative_, from, into);
    saveLocked();
    return true;
}

void Mind::setPersonDetails(const std::string& raw, const std::string& relation,
                            const std::string& birthday, const std::vector<std::string>& aliases,
                            bool protect) {
    std::string name = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto& p = people_[name];
    if (p.name.empty()) {
        p.name = name;
        p.first_seen = nowUnix();
    }
    if (!relation.empty()) p.relation = relation;
    if (!birthday.empty()) p.birthday = birthday;
    for (auto& a : aliases)
        if (std::find(p.aliases.begin(), p.aliases.end(), a) == p.aliases.end()) p.aliases.push_back(a);
    if (protect) p.protected_ = true;
    saveLocked();
}

bool Mind::forgetPerson(const std::string& raw, std::string* why) {
    std::string name = canonicalName(raw);
    std::lock_guard<std::mutex> lk(mu_);
    auto it = people_.find(name);
    if (it == people_.end()) {
        if (why) *why = "I don't know anyone called " + raw;
        return false;
    }
    if (it->second.protected_) {
        if (why) *why = name + " is protected and can never be forgotten";
        return false;
    }
    people_.erase(it);
    memories_.erase(std::remove_if(memories_.begin(), memories_.end(),
                                   [&](const Memory& m) {
                                       return m.people.size() == 1 && m.people[0] == name;
                                   }),
                    memories_.end());
    for (auto& m : memories_)
        m.people.erase(std::remove(m.people.begin(), m.people.end(), name), m.people.end());
    saveLocked();
    return true;
}

std::string Mind::householdSummary() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::time_t t = std::time(nullptr);
    std::tm now = *std::localtime(&t);
    std::ostringstream ss, soon;
    for (auto& [n, p] : people_) {
        if (p.relation.empty() && p.birthday.empty() && !p.protected_) continue;
        ss << (ss.tellp() > 0 ? "; " : "") << n;
        std::string extra = p.relation;
        int y = 0, mo = 0, d = 0;
        if (std::sscanf(p.birthday.c_str(), "%d-%d-%d", &y, &mo, &d) == 3) {
            int age = now.tm_year + 1900 - y - ((now.tm_mon + 1 < mo || (now.tm_mon + 1 == mo && now.tm_mday < d)) ? 1 : 0);
            extra += (extra.empty() ? "" : ", ") + std::to_string(age) + " years old";
            // days until next birthday
            std::tm b = now;
            b.tm_mon = mo - 1;
            b.tm_mday = d;
            b.tm_hour = 12;
            std::time_t bt = std::mktime(&b);
            double days = std::difftime(bt, t) / 86400.0;
            if (days < -0.6) {
                b.tm_year += 1;
                bt = std::mktime(&b);
                days = std::difftime(bt, t) / 86400.0;
            }
            int di = int(std::lround(days));
            if (di == 0) soon << "TODAY is " << n << "'s birthday (turning " << age << ")! ";
            else if (di <= 45) soon << n << "'s birthday is in " << di << " day(s) (turning " << age + 1 << "). ";
        }
        if (!p.aliases.empty()) {
            extra += (extra.empty() ? "" : ", ") + std::string("also called");
            for (auto& a : p.aliases) extra += " " + a;
        }
        if (!extra.empty()) ss << " (" << extra << ")";
    }
    std::string out = ss.str();
    if (out.empty()) return "";
    return "Household: " + out + ".\n" + (soon.tellp() > 0 ? "Birthdays: " + soon.str() + "\n" : "");
}

std::vector<std::pair<std::string, std::string>> Mind::recentDialogue(size_t n) const {
    std::lock_guard<std::mutex> lk(mu_);
    size_t start = dialogue_.size() > n ? dialogue_.size() - n : 0;
    std::vector<std::pair<std::string, std::string>> out;
    for (size_t i = start; i < dialogue_.size(); ++i)
        out.push_back({std::get<0>(dialogue_[i]), std::get<1>(dialogue_[i])});
    return out;
}

}  // namespace mbot
