#pragma once
// The robot's persistent inner life: personality, feelings, memories and the
// people it knows. Everything lives in plain JSON files under
//   ~/.local/share/mbot/<serial>/
// so it survives restarts, can be inspected/edited by hand, and is independent
// of which LLM is currently doing the thinking.

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

#include <nlohmann/json.hpp>

namespace mbot {

using nlohmann::json;

// Feelings, each 0..1. They drift back toward the personality's baseline.
struct Mood {
    static constexpr const char* kNames[] = {"happiness", "curiosity", "affection", "energy",   "boredom",
                                             "anxiety",   "sadness",   "fear",      "jealousy", "irritation"};
    static constexpr int kCount = 10;
    float v[kCount] = {0.6f, 0.6f, 0.5f, 0.7f, 0.2f, 0.1f, 0.05f, 0.05f, 0.03f, 0.05f};

    float& operator[](const std::string& name);
    float get(const std::string& name) const;
    json toJson() const;
    void fromJson(const json& j);
    std::string describe() const;  // "quite happy, very curious, a bit tired"
};

// A fear or phobia he has developed: grows each time it's triggered, fades when it isn't (or with
// reassurance). Seeded by body events (edges, being lifted, blacking out) and by his mind.
struct Fear {
    std::string what;        // "the edge of the table", "the vacuum cleaner"
    float strength = 0.f;    // 0..1
    std::string origin;      // how it started
    int64_t since = 0, last = 0;
    int times = 0;           // how often it was triggered
    json toJson() const;
    static Fear fromJson(const json& j);
};

struct Memory {
    int64_t id = 0;
    int64_t time = 0;          // unix seconds
    std::string text;
    int importance = 3;        // 1..10
    std::vector<std::string> people;
    std::string kind = "episode";  // episode | reflection | fact
    std::string feeling;       // mood summary at the time
    json toJson() const;
    static Memory fromJson(const json& j);
};

struct Person {
    std::string name;
    std::string relation;              // "owner", "Sam's partner", ...
    std::string birthday;              // YYYY-MM-DD (or "" unknown)
    std::vector<std::string> aliases;  // other names they go by / were born with
    bool protected_ = false;           // can never be forgotten (e.g. the owner)
    std::vector<std::string> facts;
    float affinity = 0.5f;     // 0 dislike .. 1 adore
    int64_t first_seen = 0, last_seen = 0;
    int encounters = 0;
    json toJson() const;
    static Person fromJson(const json& j);
};

class Mind {
public:
    explicit Mind(const std::string& dir);

    // ---- identity ----
    const std::string& name() const { return name_; }
    std::string selfDescription() const;  // personality + narrative, for the prompt
    void setNarrative(const std::string& n);
    void nudgeTraits(const json& deltas);  // {"playfulness": +0.02, ...}

    // ---- feelings ----
    Mood mood() const;
    void feel(const std::string& emotion, float delta);  // clamps to 0..1
    void feelAll(const json& deltas);
    void tick(double dt_sec);  // decay toward baseline, energy follows battery etc.
    // Fears: delta > 0 = triggered/strengthened, < 0 = reassured. A new fear needs a positive delta.
    void feelFear(const std::string& what, float delta, const std::string& why = "");
    std::vector<Fear> fears() const;
    std::string fearsSummary() const;  // for the prompt: "the edge of the table (strong, last felt 2 h ago)"
    void setBatteryLow(bool low) { battery_low_ = low; }
    // Eye hue/saturation that expresses the current mood.
    void eyeColor(float& hue, float& sat) const;

    // ---- memory ----
    int64_t remember(const std::string& text, int importance,
                     const std::vector<std::string>& people, const std::string& kind = "episode");
    // Most relevant memories for the given context (recency + importance + overlap).
    std::vector<Memory> recall(const std::string& context, const std::vector<std::string>& people,
                               size_t k = 8) const;
    std::vector<Memory> since(int64_t memory_id) const;
    int64_t lastReflectedId() const { return last_reflected_id_; }
    void markReflected(int64_t id);
    size_t memoryCount() const;

    // ---- people ----
    void sawPerson(const std::string& name);    // face recognized in person
    int64_t lastSeen(const std::string& name) const;  // unix time he last saw them in person (0 = never)
    // The newest memory of `kind` about this person whose text starts with `prefix` ("" if none).
    std::string latestMemory(const std::string& name, const std::string& kind, const std::string& prefix,
                             int64_t* when = nullptr) const;
    void knowPerson(const std::string& name);   // known from conversation
    void learnFact(const std::string& person, const std::string& fact);
    // Remove facts about someone that turned out to be wrong ("there is no piano"). Returns how many.
    int retractFact(const std::string& person, const std::string& wrong);
    void adjustAffinity(const std::string& person, float delta);
    // Household / identity details (set by the owner via `mbot person`).
    void setPersonDetails(const std::string& name, const std::string& relation,
                          const std::string& birthday, const std::vector<std::string>& aliases,
                          bool protect);
    // Remove someone who turned out not to be real. Refuses protected people.
    // Their solo memories are deleted; shared memories just lose the name.
    bool forgetPerson(const std::string& name, std::string* why = nullptr);
    // Correct someone's name; profile, memories, facts, story and dialogue follow. If the new name is
    // already a known person, this becomes a merge.
    bool renamePerson(const std::string& from, const std::string& to, std::string* why = nullptr);
    // Roll a duplicate into an existing person (facts, aliases, details, memories, dialogue).
    bool mergePeople(const std::string& from, const std::string& into, std::string* why = nullptr);
    // Create a person under exactly this name (no sound-alike matching): told it's really someone new.
    void addNewPerson(const std::string& name);
    // If `heard` isn't a known name but sounds/looks like one, the known name (else "").
    std::string soundsLikeKnown(const std::string& heard) const;
    std::string canonicalName(const std::string& name) const;  // resolves aliases
    // Compact roster of everyone with a relation/birthday, ages and upcoming birthdays.
    std::string householdSummary() const;
    std::string describePeople(const std::vector<std::string>& names) const;
    std::vector<std::string> knownPeople() const;
    std::vector<std::string> factsOf(const std::string& name) const;
    // Reload any mind file (self/people/memories/pronunciations) edited outside the brain. Returns the
    // names of the files reloaded ("" if none). Called every few seconds and before every save.
    struct EditReport {
        std::vector<std::string> reloaded;                         // files taken in from disk
        std::vector<std::pair<std::string, std::string>> errors;   // file -> "line 12, column 5: ..."
    };
    EditReport reloadExternalEdits();
    // A hand-edited file that doesn't parse: reported once (per save of that file); while it stays broken
    // the brain does NOT overwrite it, so the edit isn't lost.
    // The robot's owner: relation "owner", else the protected person, else "".
    std::string ownerName() const;

    // ---- conversation transcript (short-term) ----
    void logDialogue(const std::string& who, const std::string& text);
    std::vector<std::pair<std::string, std::string>> recentDialogue(size_t n) const;
    int64_t lastDialogueTime() const;  // unix seconds of the last line (0 = none)

    // ---- pronunciation (applied only when speaking; memories keep real spelling) ----
    void setPronunciation(const std::string& word, const std::string& spoken);  // "" removes
    std::map<std::string, std::string> pronunciations() const;
    std::string spoken(const std::string& text) const;

    void save();  // atomic writes; also called internally after changes

private:
    void load();
    void saveLocked();
    std::string path(const char* file) const { return dir_ + "/" + file; }

    mutable std::mutex mu_;
    std::string dir_;
    std::string name_ = "Vector";
    json traits_;              // openness, playfulness, warmth, sass, bravery, chattiness (0..1)
    std::string narrative_;    // evolving self-story written during reflection
    Mood mood_, baseline_;
    std::vector<Fear> fears_;
    bool battery_low_ = false;
    std::vector<Memory> memories_;
    int64_t next_id_ = 1;
    int64_t last_reflected_id_ = 0;
    std::map<std::string, Person> people_;
    std::map<std::string, std::string> say_as_;
    // who, text, epoch-seconds (0 = legacy entry with no timestamp)
    std::vector<std::tuple<std::string, std::string, int64_t>> dialogue_;
    int64_t born_ = 0;
    int64_t asleep_since_ = 0;
    // External edits: modification time of each mind file as we last wrote/read it. A file changed
    // behind our back (you editing people.json in an editor) is reloaded - your version wins.
    std::map<std::string, int64_t> mtimes_;
    int64_t fileMtime(const std::string& file) const;
    void noteWritten(const std::string& file);
    bool reloadLocked();
    EditReport pending_;   // reload results not yet collected by the brain (saves reload too)
    std::map<std::string, int64_t> broken_;   // file -> mtime of the broken version (reported, not overwritten)
    bool isBroken(const std::string& file) const;
};

std::string dataDirFor(const std::string& serial);
bool isRealName(const std::string& n);  // filters "someone", "unknown", ...
int64_t nowUnix();

}  // namespace mbot
