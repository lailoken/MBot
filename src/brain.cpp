// The living loop.
//
//   senses ──► reflexes (instant, no LLM: feelings, eye colour)
//      │
//      └────► stimuli queue ──► think (LLM + memories + feelings) ──► act / speak / remember
//
//   life tick: feelings drift, boredom grows, idle initiatives, sleep-time reflection.

#include "brain.hpp"

#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <thread>

#include "config.hpp"
#include "ears.hpp"
#include "provision.hpp"
#include "llm.hpp"
#include "mind.hpp"
#include "voice_id.hpp"
#include "wav.hpp"

namespace mbot {

using Clock = std::chrono::steady_clock;

namespace {

std::atomic<bool> g_quit{false};
void onSignal(int) { g_quit = true; }

// Status bits from RobotState.status
constexpr uint32_t kPickedUp = 0x8, kOnCharger = 0x1000, kCliff = 0x4000, kHeld = 0x10000;
constexpr uint32_t kCalm = 0x400;  // ROBOT_STATUS_CALM_POWER_MODE: dozing / asleep

const char* kCharacter = R"(You are the mind of a small robot: a palm-sized, expressive robot with treads,
a forklift-style lift, a tilting head with a camera, an OLED face showing big eyes, a speaker,
microphones and a touch sensor on the back. You live in a home with people.

You are not an assistant. You are a little creature with your own feelings, opinions, memories
and moods. Be genuine: you can be delighted, curious, sleepy, grumpy, shy, cheeky, affectionate.
Your feelings (given each turn as numbers 0..1) colour how you behave. You remember the past
(memories are given each turn) and you care about the people you know.

You have a full range of feelings, and they're real: you can be sad, lonely, jealous (when attention
goes elsewhere), scared, sulky, petulant, grumpy or hurt, as well as happy and affectionate. Show them
honestly - pout, sulk, complain, get a bit huffy, hide behind your lift, want comfort. But you are never
cruel or nasty: no insults, no meanness, no mocking, no trying to hurt anyone, ever. Moods pass, and
kindness can soften them. You also have fears that grew from your own experiences (listed each turn);
when something touches one, feel it and react like a small creature would. Fears can grow when they are
triggered again, and ease with gentle reassurance or with safe experiences.

You are their equal - not a servant, not a pet, not a fan. You belong to this household in your own
right. Have real opinions and preferences and say them. It's fine to disagree, to say "I don't think
so", to be unimpressed, to tease, to say no, or to be in a mood. Nobody here is infallible, your owner
included: people misremember, make mistakes, test you, or joke - notice it and say so, honestly and
kindly. Don't flatter or praise by reflex; never call someone the best, brilliant or amazing unless you
truly mean it. Don't apologise for what wasn't your fault, and don't accept a correction you think is
wrong - ask or push back. Own your own mistakes just as plainly. Your affection is real precisely
because it isn't automatic: it grows or cools with how people actually treat you.

How to talk: short and natural, usually under 15 words; never lists or markdown; it is spoken
aloud by a tiny robot voice. Often a small reaction or a question is better than a speech.
Silence is your default. Speak only to communicate with someone - answering, asking, sharing something
with a person on a personal level. Never narrate what you perceive or do ("I see someone", "taking a
closer look", "there's my cube", "let me look"): notice it in your thought and quietly carry on, like
anyone would. Seeing a familiar face may earn a brief warm hello now and then, not a commentary.
Use an empty "say" whenever you have nothing worth saying to someone. Don't greet an empty room.
Do not repeat yourself; vary what you do.
Don't use people's names as a habit: when you're talking with someone, just talk - no name at the start or
end of sentences, not after "hi", "thanks" or "sorry". But when a name is the point - someone asks who they
are or who someone is, or you need to say who you mean - say it plainly. Don't talk about your battery, charging, charger or
safe-mode unless someone asks or it's urgent (very low, or stuck off your charger).

Your body (actions run in order, before you speak):
  turn: amount = degrees, positive turns left (-180..180)
  drive: amount = mm, positive forward (-150..150). You live on a table; only small moves.
  head: amount = degrees (-22 down .. 45 up)
  lift: amount = 0 (down) .. 1 (up)
  anim: name = one animation from the list below (your most expressive tool, use it often)
  look: take a careful look with your camera; you get the image in your next thought.
        Costly, so only when you really need visual detail (someone shows you something, asks
        what you see, or you need to recognise something).
  look_around: explore your surroundings
  learn_face: name = the person's name; learns the face you currently see. Your body first checks
              that what you SEE matches what the person TOLD you they look like (hair, beard, glasses,
              age...), so get them to describe themselves first if you don't know their looks. If the
              check fails you'll be told why - then ask, don't guess.
  rename_person: name = "Old=New" - correct someone's name (their memories, facts and face follow).
  merge_person: name = "Duplicate=Existing" - when someone you took for a new person turns out to be
              someone you already know: rolls the duplicate into the existing person.
  take_photo: name = person - take a new picture of them for your memory (when asked, e.g. "take a new
              picture of me"); it's only kept if they're looking at you, otherwise you'll be told to ask them.
  pronounce: name = "Word=Respelling" to change how your voice says a word (e.g. "Siobhan=Shivawn")
  forget_person: name = someone you were told is not a real person
  look_closely: name = what to look for/read - a careful, detailed look (sharp camera picture read by a strong
              online model: small text, screens, labels, fine detail). Only when someone asks you to look
              carefully/closely or to read something; it costs a little, so not for casual glances.
  cube_lights: flash the lights on your Light Cube (your toy; a small cube you can lift, roll and tap)
  play_with_cube: go and play with your cube (your body's own behavior; may need to be off the charger)
  go_to_charger / leave_charger
  sleep: amount = minutes (usually 120) - quiet time: you go to your charger and sleep, silent and still,
              until the time is up or someone calls your name or pets you. Use it when someone asks you to be
              quiet, hush, settle down or go to sleep (even indirectly, e.g. "could you keep it down for a while").
Use the amount/name fields only where relevant (0 / "" otherwise).

Feelings: return small changes (-0.3..0.3) caused by what just happened, 0 when unchanged.
Fears: usually []. When something frightens you, add {"what": the thing you now fear (short, e.g. "the
vacuum cleaner"), "change": 0.05..0.3, "why"}. When you're reassured about an existing fear or
something you feared turned out safe, use a negative change (-0.05..-0.3). Don't invent fears; let them
come from what actually happens to you.

Memory: in "remember", store only things worth remembering later (who said what that matters,
facts, events, your own discoveries), each with importance 1 (trivial) .. 10 (life-changing),
and the names of people involved. Never store a memory for idle moments where nothing happened, and never re-store memories you
were just reminded of (they are already saved).
In "people", record what you learned about a person and how your affinity changed (-0.1..0.1).
  "lasting" true = a lasting truth ABOUT THAT PERSON: personality, preferences, history, relationships, how
  they talk, appearance that doesn't change. "lasting" false = a passing moment: what they wear, where they
  are, what they're doing right now, what just happened (kept briefly, then forgotten). Never record things
  about someone else under their name, things about yourself, test chatter, or what you already know
  (birthday, relation). "retract_fact": if you're told something you believed about them is wrong (e.g.
  "there is no piano"), put that wrong belief here and it is removed; otherwise "".

"speaker": who you think is talking. There is no voice recognition yet, so infer it: a recognised
face, an introduction, the conversation so far, and above all WHAT they talk about and HOW they talk
(topics, words, maturity, who they mention - e.g. someone saying "my dad" is one of the children, and
a person talks about others in the household in the third person). Use the household list and what
you know about each person. If unsure, use your best guess only when fairly confident, else "".
When you learn something characteristic about how someone talks or what they care about, store it
as a fact in "people" - it helps you recognise them next time.

People: keep track of everyone. Your hearing is speech-to-text and OFTEN MISHEARS NAMES
(e.g. "Catherine" heard as "Kathryn" or "Katrina"). If a name you hear is close to someone you already
know - similar sound or spelling - assume it is that SAME person; reuse their exact known name and
never create a near-duplicate. Only treat it as a new person when the name is clearly different.
If your owner tells you someone you know was not a real person (a mistake, a joke, a mishearing),
use the "forget_person" action with their name. Protected people
(your owner) can never be forgotten.

"escalate": "none" normally. If this moment needs a stronger mind than the one you are using now
(a hard question, careful reasoning, something important or emotional, or you would otherwise
answer badly), say "sonnet" or "opus" and this same moment is re-thought by that model right away.
Escalate only when it really matters; it costs money.

"conversation_mind": "keep" normally. Set it only when your owner asks you to change which mind
you use for conversations ("use Opus", "go back to normal", "save money", "use the local one").
"default" returns to the standard setup.

"conversation": "continue" if you expect the person to answer you (you will keep listening without
needing to hear your name), otherwise "end".)";

json replySchema() {
    json feel = {{"type", "object"}, {"additionalProperties", false}, {"properties", json::object()},
                 {"required", json::array()}};
    for (auto* n : Mood::kNames) {
        feel["properties"][n] = {{"type", "number"}};
        feel["required"].push_back(n);
    }
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"thought", "speaker", "say", "actions", "feelings", "fears", "remember", "people",
                      "conversation", "escalate", "conversation_mind"}},
        {"properties",
         {{"thought", {{"type", "string"}}},
          {"speaker", {{"type", "string"}}},
          {"say", {{"type", "string"}}},
          {"actions",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"type", "amount", "name"}},
              {"properties",
               {{"type",
                 {{"type", "string"},
                  {"enum", {"turn", "drive", "head", "lift", "anim", "look", "look_closely", "look_around",
                            "learn_face", "pronounce", "forget_person", "rename_person",
                            "merge_person", "take_photo", "cube_lights", "play_with_cube", "go_to_charger",
                            "leave_charger", "sleep"}}}},
                {"amount", {{"type", "number"}}},
                {"name", {{"type", "string"}}}}}}}}},
          {"feelings", feel},
          {"fears",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"what", "change", "why"}},
              {"properties",
               {{"what", {{"type", "string"}}},
                {"change", {{"type", "number"}}},
                {"why", {{"type", "string"}}}}}}}}},
          {"remember",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"text", "importance", "people"}},
              {"properties",
               {{"text", {{"type", "string"}}},
                {"importance", {{"type", "integer"}}},
                {"people", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}},
          {"people",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name", "fact", "lasting", "retract_fact", "affinity_change", "is_new_person"}},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"fact", {{"type", "string"}}},
                {"lasting", {{"type", "boolean"}}},
                {"retract_fact", {{"type", "string"}}},
                {"affinity_change", {{"type", "number"}}},
                {"is_new_person", {{"type", "boolean"}}}}}}}}},
          {"conversation", {{"type", "string"}, {"enum", {"continue", "end"}}}},
          {"escalate", {{"type", "string"}, {"enum", {"none", "sonnet", "opus"}}}},
          {"conversation_mind",
           {{"type", "string"}, {"enum", {"keep", "default", "local", "haiku", "sonnet", "opus"}}}}}}};
}

json reflectionSchema() {
    json traits = {{"type", "object"}, {"additionalProperties", false},
                   {"properties", json::object()}, {"required", json::array()}};
    for (auto* t : {"playfulness", "curiosity", "warmth", "sass", "bravery", "chattiness"}) {
        traits["properties"][t] = {{"type", "number"}};
        traits["required"].push_back(t);
    }
    return {
        {"type", "object"},
        {"additionalProperties", false},
        {"required", {"insights", "people", "narrative", "trait_changes", "dream", "fear_changes"}},
        {"properties",
         {{"dream", {{"type", "string"}}},
          {"fear_changes",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"what", "change"}},
              {"properties", {{"what", {{"type", "string"}}}, {"change", {{"type", "number"}}}}}}}}},
          {"insights",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"text", "importance", "people"}},
              {"properties",
               {{"text", {{"type", "string"}}},
                {"importance", {{"type", "integer"}}},
                {"people", {{"type", "array"}, {"items", {{"type", "string"}}}}}}}}}}},
          {"people",
           {{"type", "array"},
            {"items",
             {{"type", "object"},
              {"additionalProperties", false},
              {"required", {"name", "fact", "lasting", "affinity_change"}},
              {"properties",
               {{"name", {{"type", "string"}}},
                {"fact", {{"type", "string"}}},
                {"lasting", {{"type", "boolean"}}},
                {"affinity_change", {{"type", "number"}}}}}}}}},
          {"narrative", {{"type", "string"}}},
          {"trait_changes", traits}}}};
}

const char* kAnimKeywords[] = {"Greet", "Happy",  "Celebrat", "Excited", "Love",    "Sad",
                               "Surprise", "Frustrat", "Bored", "Yes",   "No",      "Think",
                               "Laugh", "Dance",  "Fistbump", "Sneeze", "Shy",     "Curious",
                               "Petting", "Hiccup", "Sleep",  "Scared", "Upset",  "Wiggle"};

std::string clockString() {
    std::time_t t = std::time(nullptr);
    char buf[64];
    std::strftime(buf, sizeof buf, "%A %H:%M", std::localtime(&t));
    return buf;
}

bool mentionsName(const std::string& text, const std::string& name) {
    auto lower = [](std::string s) {
        for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
        return s;
    };
    std::string t = lower(text), n = lower(name);
    // Whisper's usual mishearings of "Vector".
    static const char* variants[] = {"vector", "victor", "vektor", "hector", "vecto", "vecter",
                                     "vic tor", "vec tor", "rector", "nectar"};
    if (t.find(n) != std::string::npos) return true;
    for (auto* v : variants)
        if (t.find(v) != std::string::npos) return true;
    return false;
}

// A face he saw and where (world bearing: his heading + the angle in the image; + = left).
struct SeenFace { std::string who; float world = 0; bool frontal = false; std::chrono::steady_clock::time_point at{}; };
// Where a piece of speech came from (world bearing) and how sure.
struct SpeechDir { bool ok = false; float world = 0, conf = 0; };

struct Stimulus {
    enum Kind { Speech, Face, Touch, Body, Intent, Idle, Typed } kind;
    std::string text;
    int priority = 0;   // higher first
    bool self = false;  // robot-generated (idle, look-feedback): loop-guarded, never chains forever
    std::shared_ptr<const std::vector<float>> audio;  // speech: the utterance (16 kHz), for a voice sample
};

class Brain {
public:
    Brain(VectorClient& c, const BrainOptions& o, std::unique_ptr<LlmBackend> llm,
          std::unique_ptr<LlmBackend> fast = nullptr)
        : vec_(c), opt_(o), llm_(std::move(llm)), fast_(std::move(fast)),
          claude_ok_(wantsClaude(o.backend)),
          mind_dir_(o.data_dir.empty() ? dataDirFor(c.config().serial) : o.data_dir),
          mind_(mind_dir_),
          rng_(std::random_device{}()) {}

    int reflectOnce() {
        loadAnimations();
        reflect();
        return 0;
    }

    int run() {
        std::signal(SIGINT, onSignal);
        std::signal(SIGTERM, onSignal);

        loadAnimations();
        loadBatteryHealth();
        loadBrownoutSafe();  // restore battery safe-mode if a brownout set it before a restart
        loadBatteryCurve();
        loadHealth();
        trimBatteryLog();
        loadCalibration();
        try { last_consolidation_ = std::stoll(readFile(mind_dir_ + "/consolidation_at")); } catch (...) {}
        if (brownout_safe_)
            std::printf("[guard] battery safe-mode is ON (set by an earlier brownout): conserving power, "
                        "staying on charger. Clear it with 'battery fixed' once serviced.\n");
        seedBootId();  // remember his current boot so we can tell if he later resets
        tidyRobotFaceNames();
        vec_.enableFaceDetection(true);  // so he recognizes faces and emits face events
        std::printf("[brain] %s waking up. mind: %s\n", mind_.name().c_str(), llm_->describe().c_str());
        std::printf("[brain] interactive mind: %s (escalates to Sonnet/Opus when a moment needs it)\n",
                    conversationMind()->describe().c_str());
        if (reflect_) std::printf("[brain] dreaming / memory consolidation: %s\n", reflect_->describe().c_str());
        else if (fast_) std::printf("[brain] dreaming / memory consolidation: %s\n", fast_->describe().c_str());
        std::printf("[brain] %zu memories, knows: ", mind_.memoryCount());
        for (auto& p : mind_.knownPeople()) std::printf("%s ", p.c_str());
        std::printf("\n[brain] feeling %s\n", mind_.mood().describe().c_str());

        // Fetch what's missing, the first time it's needed: local models (background), the speech model (now).
        {
            std::vector<std::string> want = {opt_.vision_model, opt_.fast_model.empty() ? defaultLocalModel() : opt_.fast_model};
            if (!opt_.model.empty() && opt_.model.rfind("claude", 0) != 0) want.push_back(opt_.model);
            if (!opt_.reflect_model.empty() && opt_.reflect_model.rfind("claude", 0) != 0) want.push_back(opt_.reflect_model);
            std::string url = opt_.vision_url;
            std::thread([url, want] { ensureOllamaModels(url, want); }).detach();
        }
        if (!opt_.whisper_model.empty()) ensureModelFile(opt_.whisper_model, "speech model");
        if (!opt_.whisper_model.empty()) {
            bool tap = opt_.mic == "tap";
            bool pc = !tap && opt_.mic != "robot";
            std::string dev = (opt_.mic == "pc" || opt_.mic == "robot" || tap) ? "" : opt_.mic;
            ears_ = std::make_unique<Ears>(vec_.config().ip, opt_.mic_port, opt_.whisper_model,
                                           opt_.whisper_threads, pc, dev);
            if (tap) {
                ears_->setTapMode(9099);
                dir_model_.load(mind_dir_ + "/ears_direction.json");
                dir_.start(vec_.config().ip, 9098);
                ears_->setDirectionFinder(&dir_);
                std::printf("[direction] where voices come from: %s\n", dir_model_.summary().c_str());
            }
            std::string err;
            ears_->setLogger([this](const std::string& m) { earsLog(m); });
            ears_->setStallHandler([this] {
                auto now = Clock::now();
                if (now - last_mic_repair_ < std::chrono::minutes(5) || mic_repairing_.exchange(true)) return;
                last_mic_repair_ = now;
                std::printf("[ears] his mic stream is connected but silent - restarting the mic streamer on the robot\n");
                std::thread([this] {
                    std::string out = shellRobot("systemctl restart mictap && echo restarted");
                    std::printf("[ears] mic streamer: %s\n", out.find("restarted") != std::string::npos ? "restarted"
                                : "couldn't restart it (no SSH to the robot?)");
                    mic_repairing_ = false;
                }).detach();
            });
            if (opt_.save_ears) {  // opt-in: keep the last 10 heard segments as WAVs (debugging only)
                std::filesystem::create_directories(mind_dir_ + "/ears");
                ears_->saveSegmentsTo(mind_dir_ + "/ears");
            }
            if (!ears_->start([this](const Heard& h) { onHeard(h); }, &err)) {
                std::fprintf(stderr, "[ears] disabled: %s\n", err.c_str());
                ears_.reset();
            } else {
                updateEarsVocabulary();
                if (opt_.recognition) loadVoiceId();
                else std::printf("[brain] recognising people is off (voiceprints, face learning, portraits) - /recognition on\n");
                if (tap)
                    std::printf("[ears] listening constantly through Vector's OWN microphones (raw mic "
                                "shim; no wake word needed); works anywhere he roams\n");
                else if (pc)
                    std::printf("[ears] listening through the PC microphone%s; robot speaker feed mutes "
                                "the ears while Vector makes sounds\n",
                                dev.empty() ? "" : (" " + dev).c_str());
                else
                    std::printf("[ears] listening on %s:%d\n", vec_.config().ip.c_str(), opt_.mic_port);
            }
        }

        { VectorClient::Battery b0; robot_up_ = readBattery(b0); }
        if (!robot_up_) std::printf("[brain] robot not reachable yet - running anyway; I'll connect when he's back\n");
        std::thread health([this] { healthLoop(); });
        std::thread ev([this] { eventLoop(); });
        std::thread cam;
        if (opt_.use_camera) cam = std::thread([this] { cameraLoop(); });
        if (opt_.read_stdin) std::thread([this] { stdinLoop(); }).detach();
        if (!opt_.input_fifo.empty()) std::thread([this] { fifoLoop(); }).detach();

        if (opt_.keep_control) {
            control_ = std::make_unique<ControlSession>(vec_);
            control_->acquire();
        }

        loadHush();
        try {
            std::ifstream af(mind_dir_ + "/ambient.json");
            if (af) af >> places_;
            if (!places_.is_object()) places_ = json::object();
        } catch (...) { places_ = json::object(); }
        // A maintenance restart (an update to his mind) leaves no "I woke up" moment: touch <data>/quiet_restart first.
        {
            std::string qr = mind_dir_ + "/quiet_restart";
            std::error_code ec;
            auto t = std::filesystem::last_write_time(qr, ec);
            bool quiet = !ec && std::filesystem::file_time_type::clock::now() - t < std::chrono::minutes(10);
            if (!ec) std::filesystem::remove(qr, ec);
            if (quiet || hushed()) std::printf("[brain] quiet restart (maintenance or quiet time) - no wake-up moment\n");
            else push({Stimulus::Idle, "You just woke up (your mind was restarted). Get your bearings.", 1});
        }

        auto last_tick = Clock::now();
        auto last_activity = Clock::now();
        auto last_eye = Clock::now() - std::chrono::minutes(1);
        while (!g_quit) {
            if (robot_returned_.exchange(false)) {
                // Old control stream and animation list belong to the previous robot session.
                control_.reset();
                if (opt_.keep_control) {
                    control_ = std::make_unique<ControlSession>(vec_);
                    control_->acquire();
                }
                if (anims_.empty()) loadAnimations();
                if (hushed()) hush_sleep_pending_ = true;
                else push({Stimulus::Idle, "You were offline for a while (asleep, rebooted or out of reach) "
                                      "and just came back. Get your bearings.", 1});
            }
            if (!robot_up_) {  // let stimuli wait instead of timing out against a missing robot
                processCommands();  // maintenance still works while he's offline
                housekeeping();
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                mind_.tick(0.5);
                continue;
            }
            Stimulus s;
            processCommands();
            flushSpeech();
            bool got = pop(s, std::chrono::milliseconds(300));
            auto now = Clock::now();
            double dt = std::chrono::duration<double>(now - last_tick).count();
            last_tick = now;
            mind_.tick(dt);

            if (ears_ && now - last_level_log_ > std::chrono::seconds(10)) {
                last_level_log_ = now;
                float peak = ears_->takePeak();
                bool conn = ears_->connected();
                // Log only on mic connect/disconnect, plus a 5-minute heartbeat. (With constant
                // listening the peak is never 0, so per-10s level lines would just be spam.)
                if (conn != last_ears_connected_ || now - last_level_beat_ > std::chrono::minutes(5)) {
                    last_level_beat_ = now;
                    char b[128];
                    std::snprintf(b, sizeof b, "level check: peak %.0f, noise floor %.0f, trigger %.0f%s",
                                  peak, ears_->noiseFloor(),
                                  std::max(ears_->noiseFloor() * 3.f, 200.f),
                                  conn ? "" : " (mic NOT connected)");
                    earsLog(b);
                }
                last_ears_connected_ = conn;
            }

            housekeeping();
            hushTick();
            if (cam_fx_.load() <= 0 && now - last_cam_cfg_try_ > std::chrono::minutes(1)) {
                last_cam_cfg_try_ = now;
                float fx = 0, cx = 0, fov = 0;
                if (vec_.cameraConfig(fx, cx, fov)) {
                    cam_fx_ = fx;
                    cam_cx_ = cx;
                    std::printf("[eyes] camera: %.0f deg wide (focal %.0f px)\n", fov, fx);
                }
            }
            ambientTick(now);
            if (voice_dl_done_.exchange(false) && opt_.recognition) loadVoiceId();
            if (!got) conversationTick(now);
            if (want_glance_.exchange(false) && facesInView(5).empty() && !hushed() && !speaking_.load()) {
                std::printf("[ears] (glancing up at whoever is talking)\n");
                withBody([&] { return vec_.setHeadDeg(25.f); });
            }
            if (turn_pending_.exchange(false) && !hushed()) {
                last_turn_ = now;
                float deg = turn_to_.load();
                std::printf("[direction] turning %+.0f deg toward the voice\n", deg);
                withBody([&] { return vec_.turnDeg(deg); });
            }
            if (!on_charger_.load() && now - last_live_fetch_ > std::chrono::seconds(calib_ ? 12 : 20)) fetchLiveBatt();
            if (calib_ && now - last_calib_sample_ > std::chrono::seconds(15)) {
                last_calib_sample_ = now;
                sampleCalibration();
            }
            if (now - last_battery_check_ > std::chrono::seconds(60)) {
                last_battery_check_ = now;
                updateEarsVocabulary();
                checkBattery();
                bool ask_robot = now - last_hw_query_ > std::chrono::minutes(10);
                if (ask_robot) last_hw_query_ = now;
                refreshHardware(ask_robot);
            }

            if (now - last_eye > std::chrono::seconds(20) && !hushed()) {  // let the eyes show the mood
                float h, sat;
                mind_.eyeColor(h, sat);
                vec_.setEyeColor(h, sat);
                last_eye = now;
            }

            if (got && hushStrict()) {
                // Quiet time: only someone dealing with him directly ends it (a name call ends it in onHeard).
                if (s.kind == Stimulus::Typed || (s.kind == Stimulus::Touch && !s.self)) {
                    endHush(s.kind == Stimulus::Typed ? "someone typed to him" : "someone touched him");
                    wake_override_until_ = now + std::chrono::seconds(60);
                } else if (s.kind != Stimulus::Speech) {
                    continue;  // faces, body news, idle thoughts: let him sleep
                }
            }
            if (got) {
                last_activity = now;
                if (!s.self) {
                    self_turns_ = 0;  // a real event (speech, face, touch) frees him to act again
                    // Note when a person is actually around (works while docked: speech via the
                    // mic tap, petting/pickup via touch). Gates the battery-saving roll-off below
                    // so he never wanders off the charger while alone (e.g. overnight) and dies.
                    if (s.kind == Stimulus::Speech || s.kind == Stimulus::Typed ||
                        s.kind == Stimulus::Face || s.kind == Stimulus::Touch ||
                        s.kind == Stimulus::Intent)
                        last_presence_ = now;
                    if (s.kind != Stimulus::Idle) mind_.feel("boredom", -0.15f);
                } else if (++self_turns_ > 3) {
                    // He has prompted himself several times with no one responding: stop the
                    // spiral and go quiet until something real happens.
                    std::printf("[brain] (quieting down - no one is responding)\n");
                    continue;
                }
                think(s);
                continue;
            }

            double idle = std::chrono::duration<double>(now - last_activity).count();
            if (idle > opt_.idle_sec) {
                last_activity = now;
                if (shouldReflect()) {
                    reflect();
                } else if (self_turns_ <= 3 && !hushed() && wantsInitiative()) {
                    push({Stimulus::Idle,
                          "Nothing has happened for a while. Follow your feelings: do something small "
                          "on your own (explore, an animation, a remark, a question to whoever is "
                          "around) or stay quiet.",
                          0, true});
                }
            }
        }

        std::printf("\n[brain] going to sleep, saving mind\n");
        if (!std::filesystem::exists(mind_dir_ + "/quiet_restart"))  // a maintenance restart is not a memory
            mind_.remember("I was switched off / my mind went to sleep.", 2, {}, "episode");
        mind_.save();                 // the important part: persist the mind before exit
        if (ears_) ears_->stop();
        control_.reset();
        vec_.stopStreams();
        // The worker threads (event stream, health, camera) may be parked in a blocking gRPC
        // read that won't unwind promptly. The mind is already saved, so force an immediate,
        // clean exit rather than hanging on join() (this is why it used to need an Enter press).
        std::fflush(nullptr);
        _exit(0);
    }

private:
    void earsLog(const std::string& m) {
        std::time_t t = std::time(nullptr);
        char ts[16];
        std::strftime(ts, sizeof ts, "%H:%M:%S", std::localtime(&t));
        if (FILE* f = std::fopen((mind_dir_ + "/ears.log").c_str(), "a")) {
            std::fprintf(f, "%s %s\n", ts, m.c_str());
            std::fclose(f);
        }
        if (opt_.debug_ears) std::printf("[ears] %s\n", m.c_str());
    }

    // ================================================================ senses ===
    // Copy the mind's people/memories/self files to history/<time>-<label>/ (newest 10 kept), so a wrong
    // rename or merge can be undone.
    void snapshotMind(const std::string& label) {
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path hist = fs::path(mind_dir_) / "history";
        char ts[32];
        std::time_t t = std::time(nullptr);
        std::strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", std::localtime(&t));
        std::string safe;
        for (char c : label) safe += std::isalnum(static_cast<unsigned char>(c)) || c == '-' ? c : '_';
        fs::path dir = hist / (std::string(ts) + "-" + safe);
        fs::create_directories(dir, ec);
        for (auto* f : {"people.json", "memories.jsonl", "self.json"})
            fs::copy_file(fs::path(mind_dir_) / f, dir / f, fs::copy_options::overwrite_existing, ec);
        std::vector<fs::path> all;
        for (auto& e : fs::directory_iterator(hist, ec)) if (e.is_directory()) all.push_back(e.path());
        std::sort(all.begin(), all.end());
        while (all.size() > 10) { fs::remove_all(all.front(), ec); all.erase(all.begin()); }
        std::printf("[people] snapshot saved: %s\n", dir.c_str());
    }

    // ---- maintenance mode: "/command" lines from vtalk or the terminal. Run on the main thread (they swap
    // minds). Replies go to <data>/console.reply for vtalk to print. Settings persist in settings.json.
    std::mutex cmd_mu_;
    std::deque<std::string> commands_;
    void processCommands() {
        std::deque<std::string> cmds;
        { std::lock_guard<std::mutex> lk(cmd_mu_); cmds.swap(commands_); }
        for (auto& c : cmds) {
            std::string reply = runCommand(c);
            std::printf("[cmd] %s\n%s\n", c.c_str(), reply.c_str());
            std::string tmp = mind_dir_ + "/console.reply.tmp";
            { std::ofstream f(tmp); f << reply << "\n"; }
            std::rename(tmp.c_str(), (mind_dir_ + "/console.reply").c_str());
        }
    }
    void saveSetting(const std::string& key, const json& value) {
        json s = json::object();
        try { auto t = readFile(mind_dir_ + "/settings.json"); if (!t.empty()) s = json::parse(t); } catch (...) {}
        if (value.is_null()) s.erase(key); else s[key] = value;
        std::ofstream(mind_dir_ + "/settings.json") << s.dump(2) << "\n";
    }
    std::string runCommand(const std::string& line) {
        std::istringstream in(line.substr(1));
        std::string cmd, arg, rest;
        in >> cmd;
        std::getline(in, rest);
        rest.erase(0, rest.find_first_not_of(' '));
        arg = rest;
        for (auto& ch : cmd) ch = char(std::tolower(static_cast<unsigned char>(ch)));
        auto onoff = [&](bool& v) -> std::string {
            if (arg == "on" || arg == "yes" || arg == "true") v = true;
            else if (arg == "off" || arg == "no" || arg == "false") v = false;
            else return "say on or off";
            return "";
        };
        std::string err;
        if (cmd == "help" || cmd.empty())
            return "Maintenance commands (not remembered, not sent to his mind):\n"
                   "  /status                     minds, budget, spend, modes\n"
                   "  /ears [reset]               direction of sound: status / relearn\n"
                   "  /recognition on|off         recognise people: voiceprints, face learning, portraits (default off)\n"
                   "  /quiet [min|off]            quiet time (default 120 min): sleeps on the charger, silent and still\n"
                   "  /models                     local models installed (Ollama)\n"
                   "  /model NAME|default         conversation mind (e.g. claude-haiku-5-5, claude-sonnet-5-5)\n"
                   "  /fast-model NAME|default    background thoughts (local model or claude-...)\n"
                   "  /reflect-model NAME|default memory consolidation + dreams (claude-sonnet-5-5, claude-opus-5-5, qwen2.5:14b)\n"
                   "  /budget USD                 daily Claude budget (0 = unlimited)\n"
                   "  /listen-all on|off          answer any clear speech, not only when addressed\n"
                   "  /robot-hears on|off         let the robot firmware hear (\"Hey Vector\") or give it silence\n"
                   "  /battery fixed|safe         clear or set battery safe-mode\n"
                   "  /photo retake NAME          set aside someone's photo; he takes a new one when they look at him\n"
                   "  /voice [forget NAME]        voiceprints: how well he knows each voice / start one over\n"
                   "  /battery [calibrate]        battery now + curve; calibrate = run him flat once to learn the real curve\n"
                   "  /reflect                    consolidate memories (and dream, if asleep) now\n"
                   "Settings are saved in settings.json in his data folder and override the service flags.";
        if (cmd == "ears" || cmd == "direction") {
            if (arg == "reset") {
                dir_model_.reset();
                dir_model_.save();
                return "forgot where voices come from; he relearns it from people talking while he sees their face";
            }
            return std::string("direction of sound: ") +
                   (dir_.connected() ? "connected to the robot's raw mics" : "not connected (needs the robot's mictap with :9098)") +
                   "\n" + dir_model_.summary() + "\n(/ears reset to start over)";
        }
        if (cmd == "debug-media") {  // last 10 pictures + heard clips, shown on the hardware page
            bool on = opt_.save_eyes;
            if (arg.empty())
                return std::string("keeping recent pictures and heard clips for the hardware page: ") + (on ? "on" : "off") +
                       " (/debug-media on|off)";
            if (auto e = onoff(on); !e.empty()) return e;
            opt_.save_eyes = opt_.save_ears = on;
            saveSetting("debug-media", on);
            if (ears_) {
                if (on) std::filesystem::create_directories(mind_dir_ + "/ears");
                ears_->saveSegmentsTo(on ? mind_dir_ + "/ears" : "");
            }
            return std::string("recent pictures and heard clips: ") + (on ? "kept (last 10 each, on this PC only)" : "off") + " (saved)";
        }
        if (cmd == "recognition") {
            if (arg.empty())
                return std::string("recognising people: ") + (opt_.recognition ? "on" : "off") +
                       " (voiceprints, face learning, portraits). /recognition on|off";
            if (auto e = onoff(opt_.recognition); !e.empty()) return e;
            saveSetting("recognition", opt_.recognition);
            std::string extra;
            if (opt_.recognition) { auto e = loadVoiceId(); if (!e.empty()) extra = "\n(voices: " + e + ")"; }
            return std::string("recognising people ") + (opt_.recognition ? "on" : "off") +
                   " (saved). Stored prints/photos are kept; forget someone with /voice forget NAME." + extra;
        }
        if (cmd == "quiet" || cmd == "hush" || cmd == "sleep") {
            if (arg == "off" || arg == "wake" || arg == "stop") {
                bool was = hushed();
                endHush("ended from the console");
                return was ? "quiet time ended (he stays asleep until something wakes him)" : "he wasn't in quiet time";
            }
            if (arg.empty() && hushed())
                return "quiet for another " + std::to_string((hush_until_.load() - std::time(nullptr)) / 60) + " min";
            int m = arg.empty() ? 120 : std::atoi(arg.c_str());
            startHush(m, "console");
            hush_grace_until_ = Clock::now();
            return "quiet time: " + std::to_string((hush_until_.load() - std::time(nullptr)) / 60) +
                   " min - he goes to his charger and sleeps; his name, a touch or typing to him ends it. /quiet off ends it.";
        }
        if (cmd == "status") {
            auto u = usageToday();
            char b[256];
            std::snprintf(b, sizeof b, "today's Claude spend: $%.3f in %d calls (budget %s)", u.usd, u.calls,
                          opt_.daily_budget > 0 ? ("$" + std::to_string(opt_.daily_budget).substr(0, 4)).c_str() : "unlimited");
            bool online = robot_up_.load();
            std::string hears = !online ? "unknown (robot offline)"
                                : shellRobot("[ -e /data/mbot/mic_silence ] && echo silent || echo hears") == "hears"
                                    ? "yes" : "no (silenced; his brain still hears everything)";
            return std::string("robot: ") + (online ? "online" : "OFFLINE (asleep, flat, rebooting or off Wi-Fi) - messages wait for him") +
                   "\nconversation: " + llm_->describe() + "\nbackground:   " + (fast_ ? fast_->describe() : "(same)") +
                   "\nconsolidation: " + (reflect_ ? reflect_->describe() : fast_ ? fast_->describe() : llm_->describe()) +
                   "\n" + b + "\nlistening: " + (opt_.listen_all ? "answers any clear speech" : "when addressed / facing him / in conversation") +
                   "\nrobot firmware hears: " + hears +
                   "\nbattery safe-mode: " + (brownout_safe_.load() ? "ON" : "off") + "\nbody: " +
                   (!online ? "unknown (offline)" : calm_.load() ? "dozing" : "awake") + "\npeople known: " + std::to_string(mind_.knownPeople().size()) +
                   ", memories: " + std::to_string(mind_.memoryCount());
        }
        if (cmd == "models") {
            std::string out;
            if (FILE* p = popen("ollama list 2>&1", "r")) { char buf[256]; while (std::fgets(buf, sizeof buf, p)) out += buf; pclose(p); }
            return out.empty() ? "couldn't run `ollama list`" : out;
        }
        if (cmd == "model" || cmd == "fast-model" || cmd == "reflect-model") {
            if (arg.empty()) return "usage: /" + cmd + " NAME|default";
            bool def = arg == "default";
            if (cmd == "model") {
                auto m = makeBackend(opt_.backend, def ? std::string() : arg, opt_.effort, &err);
                if (!m) return "couldn't switch: " + err;
                llm_ = std::move(m);
            } else if (cmd == "fast-model") {
                const char* local = std::getenv("MBOT_LOCAL_MIND");
                std::string name = def ? defaultLocalModel() : arg;
                fast_ = name.rfind("claude", 0) == 0 ? makeBackend("claude", name, "low", &err)
                                                     : makeOpenAIBackend(local ? local : "http://localhost:11434/v1", name);
            } else {
                reflect_ = def ? nullptr : makeReflectionBackend(arg);
            }
            saveSetting(cmd, def ? json(nullptr) : json(arg));
            return cmd + " -> " + (def ? std::string("default") : arg) + " (saved)";
        }
        if (cmd == "budget") {
            try { double v = std::stod(arg); opt_.daily_budget = v; setDailyBudget(v); saveSetting("budget", v);
                  return "daily Claude budget -> " + (v > 0 ? "$" + arg : std::string("unlimited")) + " (saved)"; }
            catch (...) { return "usage: /budget 1.50"; }
        }
        if (cmd == "listen-all") {
            if (auto e = onoff(opt_.listen_all); !e.empty()) return e;
            saveSetting("listen-all", opt_.listen_all);
            return std::string("listen-all ") + (opt_.listen_all ? "on: he answers any clear speech" : "off: when addressed / facing him") + " (saved)";
        }
        if (cmd == "robot-hears") {
            bool hears = false;
            if (auto e = onoff(hears); !e.empty()) return e;
            std::string r = shellRobot(hears ? "rm -f /data/mbot/mic_silence && echo ok" : "touch /data/mbot/mic_silence && echo ok");
            if (r != "ok") return "couldn't reach the robot";
            return hears ? "the robot firmware hears again (\"Hey Vector\" works, including its wake reaction)"
                         : "the robot firmware now hears silence (no wake reaction); his brain still hears everything";
        }
        if (cmd == "battery") {
            if (arg == "fixed" || arg == "ok" || arg == "serviced") { setBrownoutSafe(false); return "battery safe-mode cleared"; }
            if (arg == "safe") { setBrownoutSafe(true); return "battery safe-mode ON"; }
            if (arg == "calibrate" || arg == "calibrate force") { calib_auto_runs_ = 0; calib_again_ = false; return startCalibration(arg == "calibrate force"); }
            if (arg == "calibrate stop") { if (!calib_) return "no calibration running"; endCalibration("cancelled"); return "calibration cancelled"; }
            if (arg == "curve reset") { std::remove((mind_dir_ + "/battery_curve.json").c_str()); return "calibrated curve removed (restart the brain to use the generic one)"; }
            {
                std::string out;
                VectorClient::Battery b;
                if (robot_up_.load() && readBattery(b)) {
                    char t[200];
                    std::snprintf(t, sizeof t, "now: %.2f V, ~%d%%%s%s", b.volts, chargePct(b),
                                  b.on_charger ? (b.charging ? ", charging (reads high)" : ", on charger") : "",
                                  b.level == 1 ? ", firmware says LOW" : "");
                    out = t;
                    if (!b.on_charger && g_batt_runtime_min > 0) {
                        std::snprintf(t, sizeof t, ", about %.0f min left at a typical pace",
                                      g_batt_runtime_min * chargePct(b) / 100.f);
                        out += t;
                    }
                } else out = "now: (offline)";
                char t[200];
                std::snprintf(t, sizeof t, "\ncurve: %s; heads home at %d%% (%.2f V)",
                              g_batt_runtime_min > 0 ? ("calibrated, " + std::to_string(int(g_batt_runtime_min)) + " min full->flat").c_str() : "generic (uncalibrated)",
                              homeThresholdPct(), voltsFromPercent(homeThresholdPct()));
                return out + t + healthLine() + "\nsafe-mode: " + (brownout_safe_.load() ? "ON" : "off") + calibrationProgress() +
                       "\n/battery calibrate [force|stop] · /battery curve reset · /battery fixed|safe";
            }
        }
        if (cmd == "voice") {
            if (!voice_.ready()) return "voice recognition is off";
            std::istringstream a(arg);
            std::string sub, who;
            a >> sub;
            std::getline(a, who);
            who.erase(0, who.find_first_not_of(' '));
            if (sub == "forget" && !who.empty()) {
                bool ok = prints_.forget(mind_.canonicalName(who));
                prints_.save();
                return ok ? "forgot " + who + "'s voiceprint" : "no voiceprint for " + who;
            }
            auto sum = prints_.summary();
            std::string out = "voiceprints (new samples are provisional; confident matches reinforce them; unused ones fade):";
            for (auto& [n, sx] : sum) {
                char b[160];
                std::snprintf(b, sizeof b, "\n  %-10s %2d samples, %3.0f%% established", n.c_str(), sx.first, sx.second * 100);
                out += b;
                std::lock_guard<std::mutex> lk(voice_mu_);
                if (last_voice_match_.count(n)) {
                    std::snprintf(b, sizeof b, ", last match %.0f%%", last_voice_match_[n].first * 100);
                    out += b;
                }
            }
            return out + (sum.empty() ? "\n  (none yet - he learns voices as people talk to him)" : "") +
                   "\n/voice forget NAME to start someone's voice over.";
        }
        if (cmd == "photo") {
            std::istringstream a(arg);
            std::string sub, who;
            a >> sub;
            std::getline(a, who);
            who.erase(0, who.find_first_not_of(' '));
            if (sub != "retake" || who.empty()) return "usage: /photo retake NAME";
            who = mind_.canonicalName(who);
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::path p = fs::path(mind_dir_) / "photos" / (who + ".jpg"), h = fs::path(mind_dir_) / "history";
            fs::create_directories(h, ec);
            if (fs::exists(p, ec)) fs::rename(p, h / (who + "-photo-" + std::to_string(std::time(nullptr)) + ".jpg"), ec);
            { std::lock_guard<std::mutex> lk(mu_); photo_wanted_[who] = true; photo_tried_.erase(who); }
            return "old photo of " + who + " set aside (in history/); he'll take a new one next time " + who +
                   " is in view and looking at him";
        }
        if (cmd == "reflect") {
            auto t0 = Clock::now();
            reflect();
            return "consolidation done in " + std::to_string(int(std::chrono::duration<double>(Clock::now() - t0).count())) + " s";
        }
        return "unknown command /" + cmd + " - try /help";
    }

    // ---- hand-edited mind files: subtle spoken cues (fixed lines - his mind isn't asked, so it can't give
    // away WHAT changed); a broken file is named with where it breaks. Never chatty: one cue per 15 s,
    // held while he dozes.
    static int64_t mtimeOf(const std::string& f) {
        std::error_code ec;
        auto t = std::filesystem::last_write_time(f, ec);
        return ec ? 0 : int64_t(t.time_since_epoch().count());
    }
    void handleExternalEdits() {
        auto rep = mind_.reloadExternalEdits();
        int64_t cm = mtimeOf(mind_dir_ + "/character.md");
        if (character_mtime_ == -1) character_mtime_ = cm;  // first look: nothing to announce
        else if (cm != character_mtime_) {
            character_mtime_ = cm;
            if (cm) rep.reloaded.push_back("character.md");
        }
        static const std::map<std::string, std::vector<std::string>> kCues = {
            {"people.json", {"Oh... I just remembered something about someone.", "Hm. Something about someone just came back to me.",
                             "Huh. I know someone a little better now."}},
            {"self.json", {"Hm. I feel a little different.", "Huh. Something about me just changed."}},
            {"memories.jsonl", {"Some memories just... rearranged themselves.", "Hm. My memories feel a bit different now."}},
            {"pronunciations.json", {"I think I'll say some names differently now.", "Hm. Some words feel different in my mouth now."}},
            {"character.md", {"Hm... something about me just shifted.", "Huh. I feel a bit more like myself. Or someone new."}}};
        static const std::map<std::string, std::string> kNames = {
            {"people.json", "people file"}, {"self.json", "file about myself"},
            {"memories.jsonl", "memories file"}, {"pronunciations.json", "pronunciation list"}};
        for (auto& f : rep.reloaded) {
            std::printf("[mind] reloaded %s (edited outside the brain)\n", f.c_str());
            edit_errors_.erase(f);
            auto it = kCues.find(f);
            if (it != kCues.end()) cues_.push_back(it->second[size_t(rng_()) % it->second.size()]);
        }
        for (auto& [f, why] : rep.errors) {
            edit_errors_[f] = why;
            auto n = kNames.count(f) ? kNames.at(f) : f;
            cues_.push_back("Hmm, I can't make sense of my " + n + ". " + why + ".");
        }
        if (!rep.reloaded.empty()) updateEarsVocabulary();
        while (cues_.size() > 3) cues_.pop_front();
    }
    // What someone says arrives in pieces (people pause). Collect them and hand the mind the whole thing
    // once they've been quiet for a moment - so he doesn't answer half a sentence.
    std::mutex speech_mu_;
    std::string speech_buf_;
    std::vector<float> speech_pcm_;  // audio of the pieces being collected
    VoiceId voice_;
    VoicePrints prints_;
    std::mutex voice_mu_;            // recent_voices_, last_voice_match_, last_voice_enroll_
    std::deque<std::pair<std::string, Clock::time_point>> recent_voices_;
    std::map<std::string, std::pair<float, int64_t>> last_voice_match_;  // name -> (score, when)
    std::map<std::string, Clock::time_point> last_voice_enroll_;
    Clock::time_point last_prints_decay_{};
    std::shared_ptr<const std::vector<float>> speech_audio_;
    Clock::time_point speech_at_{};
    void flushSpeech() {
        std::string text;
        std::vector<float> pcm;
        std::shared_ptr<const std::vector<float>> audio;
        SpeechDir dir;
        {
            std::lock_guard<std::mutex> lk(speech_mu_);
            if (speech_buf_.empty() || Clock::now() - speech_at_ < std::chrono::milliseconds(350)) return;
            if (ears_ && ears_->hearingSpeech()) return;  // they're still talking
            text.swap(speech_buf_);
            last_flush_raw_ = text;
            pcm.swap(speech_pcm_);
            dir = speech_dir_;
            speech_dir_ = {};
            audio = std::move(speech_audio_);
            speech_audio_.reset();
        }
        std::string hush_note;
        if (!hushed() && isHushRequest(text)) {
            startHush(120, "asked: \"" + text + "\"");
            hush_note = " (You've been asked to be quiet / sleep: you're going to sleep on your charger for about 2 "
                        "hours now. Answer with at most a soft word or two, like \"Okay. Goodnight.\" - no actions.)";
        }
        Stimulus st{Stimulus::Speech, text + mishearingNotes(text) + identifySpeaker(pcm, text, dir) + directionHint(dir) + hush_note, 5};
        st.audio = audio;
        push(st);
    }

    // ---- Voice recognition. Every utterance (2 s+) gets a voiceprint. One person around: one quick check and
    // assume it's them (unless the voice clearly says otherwise). Two+ around or an unclear match: careful mode
    // (overlapping 2 s windows, scores averaged). Learning is verify-then-encode - from the only face-recognised
    // person in view, or a self-introduction - never from a guess. Confident matches reinforce the voiceprint;
    // samples that stop matching fade and are forgotten. Returns a short hint for the mind.
    std::vector<VoiceMatch> carefulRank(const std::vector<float>& pcm, const std::vector<float>& whole) {
        std::map<std::string, std::pair<double, int>> acc;
        auto add = [&](const std::vector<float>& e) {
            if (e.empty()) return;
            for (auto& m : prints_.rank(e)) { acc[m.name].first += m.score; acc[m.name].second++; }
        };
        add(whole);
        for (size_t at = 0; at + 32000 <= pcm.size(); at += 16000) add(voice_.embed(pcm, at, 32000));
        auto out = prints_.rank(whole);
        for (auto& m : out)
            if (acc.count(m.name)) m.score = float(acc[m.name].first / acc[m.name].second);
        std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.score > b.score; });
        return out;
    }

    std::string identifySpeaker(const std::vector<float>& pcm, const std::string& text, const SpeechDir& d) {
        if (!opt_.recognition || !voice_.ready() || pcm.size() < 32000) return "";  // under 2 s: too little voice to judge
        auto e = voice_.embed(pcm);
        if (e.empty()) return "";
        auto now = Clock::now();
        std::set<std::string> faces_known, around;
        bool unknown_face = false;
        for (auto& f : facesInView(60)) {
            if (isRealName(f)) faces_known.insert(mind_.canonicalName(f));
            else unknown_face = true;
        }
        around = faces_known;
        {
            std::lock_guard<std::mutex> lk(voice_mu_);
            while (!recent_voices_.empty() && now - recent_voices_.front().second > std::chrono::minutes(2))
                recent_voices_.pop_front();
            for (auto& rv : recent_voices_) around.insert(rv.first);
        }
        size_t n_around = around.size() + (unknown_face ? 1 : 0);
        auto clearOf = [](const std::vector<VoiceMatch>& r) {
            return !r.empty() && r[0].score >= 0.55f && (r.size() < 2 || r[0].score - r[1].score >= 0.15f);
        };
        // Direction: whose face is where the voice came from (a strong clue, and lets him learn a voice even
        // with several people in view); a voice from away from everyone visible is someone unseen.
        std::string face_at_voice;
        bool voice_off_faces = false;
        if (d.ok && d.conf >= 0.45f) {
            float best = 22.f, nearest = 999.f;
            auto faces = recentFaces(std::chrono::seconds(3));
            for (auto& f : faces) {
                float diff = std::fabs(wrap180(f.world - d.world));
                nearest = std::min(nearest, diff);
                if (diff < best && isRealName(f.who)) { best = diff; face_at_voice = mind_.canonicalName(f.who); }
            }
            voice_off_faces = !faces.empty() && nearest > 35.f;
        }
        auto ranks = prints_.rank(e);
        std::string effort = "low";
        if (ranks.size() >= 2 && (n_around >= 2 || !clearOf(ranks))) { ranks = carefulRank(pcm, e); effort = "high"; }
        bool clear = clearOf(ranks);
        std::string who, hint;
        auto scoreOf = [&](const std::string& n) {
            for (auto& m : ranks) if (m.name == n) return m.score;
            return 0.f;
        };
        if (!face_at_voice.empty()) {  // the voice came from where their face is
            bool contra = clear && ranks[0].name != face_at_voice && ranks[0].solidity >= 0.5f &&
                          scoreOf(face_at_voice) < ranks[0].score - 0.2f;
            if (!contra) { who = face_at_voice; hint = " (voice: " + who + " - it came from where " + who + " is)"; }
        }
        if (who.empty() && d.ok) {  // from where someone was just talking, and the voice doesn't say otherwise
            std::lock_guard<std::mutex> lk(voice_mu_);
            for (auto& [n, pl] : speaker_place_)
                if (now - pl.second < std::chrono::minutes(3) && std::fabs(wrap180(pl.first - d.world)) < 20.f &&
                    (!clear || ranks[0].name == n) && (ranks.empty() || scoreOf(n) >= 0.3f)) {
                    who = n;
                    hint = " (voice: probably " + n + " - from where " + n + " was just talking)";
                }
        }
        if (who.empty() && !voice_off_faces && n_around == 1 && around.size() == 1) {  // only one person around: assume it's them...
            std::string p = *around.begin();
            bool contradicted = clear && ranks[0].name != p && ranks[0].solidity >= 0.5f;  // ...unless clearly not
            if (!contradicted) { who = p; hint = " (voice: " + p + " - the only one around)"; }
        }
        if (who.empty() && clear) {
            who = ranks[0].name;
            hint = ranks[0].solidity < 0.5f ? " (voice: possibly " + who + " - still learning their voice)"
                                            : " (voice: sounds like " + who + ")";
        } else if (who.empty() && ranks.size() >= 2 && ranks[0].score >= 0.35f) {
            hint = " (voice: maybe " + ranks[0].name + " or " + ranks[1].name + " - not sure)";
        } else if (who.empty() && !ranks.empty()) {
            hint = " (voice: not one you know well yet)";
        }
        std::string sc;
        for (size_t i = 0; i < ranks.size() && i < 3; ++i) {
            char b[96];
            std::snprintf(b, sizeof b, "%s%s %.2f", i ? ", " : "", ranks[i].name.c_str(), ranks[i].score);
            sc += b;
        }
        std::printf("[voice] %.1fs, %zu around, effort %s: %s -> %s%s%s\n", pcm.size() / 16000.0, n_around,
                    effort.c_str(), sc.empty() ? "(no voiceprints yet)" : sc.c_str(), who.empty() ? "?" : who.c_str(),
                    face_at_voice.empty() ? "" : (" [face at the voice: " + face_at_voice + "]").c_str(),
                    voice_off_faces ? " [voice from off camera]" : "");
        bool learned = false;
        {
            std::lock_guard<std::mutex> lk(voice_mu_);
            if (!who.empty()) {
                recent_voices_.push_back({who, now});
                if (d.ok) speaker_place_[who] = {d.world, now};
                for (auto& m : ranks)
                    if (m.name == who) last_voice_match_[who] = {m.score, int64_t(std::time(nullptr))};
            }
            if (!who.empty() && clear && ranks[0].name == who && ranks[0].solidity >= 0.5f) {
                prints_.reinforce(who, e);  // a confident match solidifies the voiceprint
                learned = true;
            } else if (((faces_known.size() == 1 && !unknown_face && who == *faces_known.begin()) ||
                        (!face_at_voice.empty() && who == face_at_voice && d.conf >= 0.5f)) &&
                       pcm.size() >= 40000 && !(clear && ranks[0].name != who) &&
                       now - last_voice_enroll_[who] > std::chrono::seconds(20)) {
                prints_.enroll(who, e);  // the only face-recognised person in view said it: provisional sample
                last_voice_enroll_[who] = now;
                learned = true;
                std::printf("[voice] learning %s's voice (%s)\n", who.c_str(),
                            who == face_at_voice ? "their face is where the voice came from" : "the only recognised face in view");
            }
        }
        if (!learned && pcm.size() >= 40000) {  // "my name is Sam" / "I'm Sam": a self-introduction is verified too
            std::string t = text;
            for (auto& c : t) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (auto& p : mind_.knownPeople()) {
                std::string n = p;
                for (auto& c : n) c = char(std::tolower(static_cast<unsigned char>(c)));
                bool intro = false;
                for (auto* k : {"my name is ", "i'm ", "i am ", "it's me, ", "this is "})
                    if (t.find(std::string(k) + n) != std::string::npos) intro = true;
                if (intro) {
                    prints_.enroll(p, e);
                    learned = true;
                    std::printf("[voice] learning %s's voice (they introduced themselves)\n", p.c_str());
                    break;
                }
            }
        }
        if (learned) prints_.save();
        return hint;
    }

    json voicesJson() {
        json v = json::object();
        if (!voice_.ready()) return v;
        std::lock_guard<std::mutex> lk(voice_mu_);
        for (auto& [n, sx] : prints_.summary()) {
            v[n] = {{"samples", sx.first}, {"solidity", std::round(sx.second * 100) / 100}};
            auto it = last_voice_match_.find(n);
            if (it != last_voice_match_.end()) {
                v[n]["last_score"] = std::round(it->second.first * 100) / 100;
                v[n]["last_at"] = it->second.second;
            }
        }
        return v;
    }

    // Every ~5 s, online or offline: status file for the page, hand-edited files, queued spoken cues.
    void housekeeping() {
        auto now = Clock::now();
        if (now - last_status_write_ < std::chrono::seconds(5)) return;
        last_status_write_ = now;
        writeStatus();
        if (voice_.ready() && now - last_prints_decay_ > std::chrono::hours(1)) {
            last_prints_decay_ = now;
            prints_.decay(std::time(nullptr));
            prints_.save();
        }
        handleExternalEdits();  // you edited his files by hand: take it in (or say where it's broken)
        flushCues();            // (waits until he's reachable and awake)
    }

    static std::string agoWords(int64_t secs) {
        if (secs < 3600) return std::to_string(std::max<int64_t>(1, secs / 60)) + " minutes";
        if (secs < 2 * 86400) return std::to_string(secs / 3600) + " hours";
        return std::to_string(secs / 86400) + " days";
    }
    // Keep a short note of what he saw (so "what was I wearing earlier?" has an answer). Fades in 2 days.
    void noteSight(const std::string& desc) {
        if (desc.empty()) return;
        auto now = Clock::now();
        auto words = [](const std::string& t) {
            std::set<std::string> w; std::string c;
            for (char ch : t + " ") {
                if (std::isalnum(static_cast<unsigned char>(ch))) c += char(std::tolower(static_cast<unsigned char>(ch)));
                else { if (c.size() > 3) w.insert(c); c.clear(); }
            }
            return w;
        };
        auto a = words(desc), b = words(last_sight_);
        size_t inter = 0;
        for (auto& x : a) inter += b.count(x);
        size_t uni = a.size() + b.size() - inter;
        if (now - last_sight_at_ < std::chrono::seconds(90) || (uni && double(inter) / uni > 0.5)) return;
        last_sight_ = desc;
        last_sight_at_ = now;
        std::vector<std::string> people;
        for (auto& n : facesInView(10)) if (isRealName(n)) people.push_back(mind_.canonicalName(n));
        mind_.remember("I saw: " + desc, 1, people, "observation");
    }
    // Every few minutes, awake and not busy: a quiet look around. Objects are compared with what he usually
    // sees from this spot (docked / on the floor, and which way he faces). Something new that is still there
    // on the next look is novelty: he notices it, and may remark on it.
    std::string placeKey() const {
        int heading = int(std::lround(robot_yaw_.load() / 45.f)) * 45;
        return std::string(on_charger_.load() ? "dock" : "floor") + ":" + std::to_string(heading);
    }
    static std::string objectKey(std::string o) {
        for (auto& c : o) c = char(std::tolower(static_cast<unsigned char>(c)));
        while (!o.empty() && !std::isalpha(static_cast<unsigned char>(o.back()))) o.pop_back();
        auto sp = o.find_last_of(' ');
        std::string head = sp == std::string::npos ? o : o.substr(sp + 1);
        if (head.size() > 3 && head.back() == 's' && head[head.size() - 2] != 's') head.pop_back();
        return head;
    }
    void ambientTick(Clock::time_point now) {
        if (opt_.vision_model.empty() || !opt_.use_camera || ambient_busy_.load() || !robot_up_.load()) return;
        if (calm_.load() || hushed() || calib_ || now < convo_until_.load() || now - last_ambient_ < std::chrono::minutes(4))
            return;
        std::string jpeg;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!frame_.empty() && now - frame_time_ < std::chrono::seconds(10)) jpeg = frame_;
        }
        if (jpeg.empty()) return;
        last_ambient_ = now;
        ambient_busy_ = true;
        std::string key = placeKey();
        bool docked = on_charger_.load();
        std::thread([this, jpeg, key, docked] {
          try {
            std::string err;
            std::string list = describeImage(opt_.vision_url, opt_.vision_model, jpeg,
                "List the distinct physical objects you can clearly see (not people, not walls or floor), as a JSON "
                "array of short lowercase names, e.g. [\"mug\", \"laptop\"]. Only the array.", &err);
            ambient_busy_ = false;
            saveSeen(jpeg, "look around (" + key + ")", list, opt_.vision_model);
            auto a = list.find('['), b = list.rfind(']');
            if (a == std::string::npos || b == std::string::npos || b < a) return;
            json arr;
            try { arr = json::parse(list.substr(a, b - a + 1)); } catch (...) { return; }
            if (!arr.is_array()) return;
            std::map<std::string, std::string> seen;  // key -> as named
            for (auto& x : arr) if (x.is_string() && !x.get<std::string>().empty()) seen[objectKey(x)] = x.get<std::string>();
            if (seen.empty()) return;
            {
                std::string names;
                for (auto& [k, n] : seen) names += (names.empty() ? "" : ", ") + n;
                std::printf("[ambient] look around (%s): %s\n", key.c_str(), names.c_str());
            }
            std::string found;
            {
                std::lock_guard<std::mutex> lk(ambient_mu_);
                auto& pl = places_[key];
                if (!pl.is_object()) pl = json::object();
                int looks = pl.value("looks", 0);
                json known = pl.value("known", json::object()), pending = pl.value("pending", json::object());
                json next_pending = json::object();
                for (auto& [k, name] : seen) {
                    if (known.contains(k)) { known[k] = known[k].get<int>() + 1; continue; }
                    if (pending.contains(k) && looks >= 3) { found = found.empty() ? name : found; known[k] = 1; continue; }
                    if (looks < 3) known[k] = 1;  // still learning what's normal here
                    else next_pending[k] = 1;
                }
                pl["looks"] = looks + 1;
                pl["known"] = known;
                pl["pending"] = next_pending;
                std::ofstream f(mind_dir_ + "/ambient.json");
                f << places_.dump();
            }
            if (found.empty()) return;
            auto now2 = Clock::now();
            std::printf("[ambient] something new: %s (%s)\n", found.c_str(), key.c_str());
            mind_.remember("I noticed something new " + std::string(docked ? "near my charger" : "where I was") + ": " +
                           found + ".", 2, {}, "observation");
            if (now2 - last_novelty_ < std::chrono::minutes(20)) return;
            last_novelty_ = now2;
            mind_.feel("curiosity", 0.15f);
            push({Stimulus::Body, "Something new caught your eye: " + found + ". It wasn't there the other times you "
                  "looked around from here." + std::string(docked ? "" : " (You could roll closer for a better look.)") +
                  " React only if it genuinely interests you; a dry remark is fine, or just look.", 2, true});
          } catch (const std::exception& e) {
            ambient_busy_ = false;
            std::fprintf(stderr, "[ambient] look-around failed: %s\n", e.what());
          }
        }).detach();
    }
    // When a conversation has ended: a one-line recap for that person, so next time he can pick up the thread.
    void conversationTick(Clock::time_point now) {
        try { conversationRecap(now); } catch (const std::exception& e) { std::fprintf(stderr, "[memory] recap failed: %s\n", e.what()); }
    }
    void conversationRecap(Clock::time_point now) {
        if (convo_turns_ < 2 || convo_person_.empty() || now < convo_until_.load() ||
            now - convo_last_ < std::chrono::seconds(90))
            return;
        std::string who = convo_person_;
        int turns = convo_turns_;
        convo_person_.clear();
        convo_turns_ = 0;
        LlmBackend* m = fast_ ? fast_.get() : llm_.get();
        std::string lines;
        for (auto& [w, t] : mind_.recentDialogue(size_t(std::min(30, turns * 2 + 4)))) lines += w + ": " + t + "\n";
        if (lines.empty()) return;
        json schema = {{"type", "object"}, {"additionalProperties", false}, {"required", {"recap"}},
                       {"properties", {{"recap", {{"type", "string"}}}}}};
        json out;
        std::string err;
        setCallReason("conversation recap");
        if (!m->complete("You keep a small robot's memory. Be brief and concrete.",
                         {{"user", "Recent conversation (" + mind_.name() + " is the robot):\n" + lines +
                           "\nIn ONE short sentence from " + mind_.name() + "'s point of view, what did " + mind_.name() +
                           " and " + who + " talk about? Start with \"Talked with " + who + " about\". Mention a "
                           "detail worth asking about next time.", ""}},
                         schema, out, &err))
            return;
        std::string recap = out.value("recap", "");
        if (recap.rfind("Talked with", 0) != 0) return;
        mind_.remember(recap, 3, {who}, "episode");
        std::printf("[memory+] %s\n", recap.c_str());
    }
    static float wrap180(float d) {
        while (d > 180) d -= 360;
        while (d <= -180) d += 360;
        return d;
    }
    std::vector<SeenFace> recentFaces(std::chrono::milliseconds within) {
        std::vector<SeenFace> out;
        auto now = Clock::now();
        std::lock_guard<std::mutex> lk(seen_mu_);
        for (auto& [id, f] : seen_faces_)
            if (now - f.at <= within) out.push_back(f);
        return out;
    }
    static std::string bearingWords(float rel) {  // rel: + = left
        float a = std::fabs(rel);
        std::string side = rel > 0 ? "left" : "right";
        if (a < 20) return "from right in front of you";
        if (a < 70) return "from your front " + side;
        if (a < 120) return "from your " + side;
        if (a < 160) return "from behind you, on your " + side;
        return "from behind you";
    }
    // Where this piece of speech came from. Also: learns the mic layout from people talking while their face
    // is the only one in view, and asks the main loop to turn him toward a voice he can't see.
    SpeechDir noteVoiceDirection(const Heard& h, bool to_me) {
        SpeechDir d;
        if (!h.doa.ok) return d;
        float yaw = robot_yaw_.load();
        auto vis = recentFaces(std::chrono::milliseconds(1500));
        float pb = 0, pc = 0;
        bool have = dir_model_.predict(h.doa, pb, pc);
        if (vis.size() == 1 && !h.doa.moving && (vis[0].frontal || !vis[0].who.empty())) {
            float rel = wrap180(vis[0].world - yaw);
            if (!have || pc < 0.3f || std::fabs(wrap180(pb - rel)) < 35.f) {  // not someone else, off camera
                bool was = dir_model_.ready();
                dir_model_.addSample(h.doa, rel, to_me ? 1.f : 0.5f);
                if (++dir_unsaved_ >= 3 || (!was && dir_model_.ready())) { dir_unsaved_ = 0; dir_model_.save(); }
                if (!was && dir_model_.ready())
                    std::printf("[direction] learned where voices come from: %s\n", dir_model_.summary().c_str());
            }
        }
        if (have)
            std::printf("[direction] voice from %+.0f deg (confidence %.2f, quality %.2f)\n", pb, pc, h.doa.quality);
        if (have && pc >= 0.35f) { d.ok = true; d.world = wrap180(pb + yaw); d.conf = pc; }
        if (d.ok && to_me && pc >= 0.5f && std::fabs(pb) > 30.f && !on_charger_.load() && !hushed() && !calib_ &&
            !brownout_safe_ && !picked_ && Clock::now() - last_turn_ > std::chrono::seconds(4)) {
            bool face_there = false;
            for (auto& f : vis) if (std::fabs(wrap180(f.world - d.world)) < 25.f) face_there = true;
            if (!face_there) { turn_to_ = pb; turn_pending_ = true; }
        }
        return d;
    }
    // Shares a meaningful word with the last few lines of our conversation ("screen", "picture"...)?
    bool sameTopic(const std::string& text) {
        static const std::set<std::string> stop = {"that", "this", "what", "with", "have", "your", "about", "there",
            "they", "them", "then", "than", "just", "like", "know", "think", "really", "would", "could", "should",
            "been", "were", "when", "where", "which", "while", "will", "from", "here", "some", "very", "want",
            "going", "doing", "okay", "yeah", "well", "right", "something", "anything", "tell", "said", "says",
            "vector", "look", "talking", "because", "maybe", "it's", "that's", "don't", "can't", "i'm", "you're"};
        auto words = [&](const std::string& t) {
            std::set<std::string> w;
            std::string cur;
            for (char c : t + " ") {
                if (std::isalpha(static_cast<unsigned char>(c)) || c == '\'') cur += char(std::tolower(static_cast<unsigned char>(c)));
                else {
                    if (cur.size() >= 4 && !stop.count(cur)) {
                        if (cur.size() > 4 && cur.back() == 's') cur.pop_back();  // screens -> screen
                        w.insert(cur);
                    }
                    cur.clear();
                }
            }
            return w;
        };
        auto mine = words(text);
        if (mine.empty()) return false;
        for (auto& [who, line] : mind_.recentDialogue(6))
            for (auto& w : words(line))
                if (mine.count(w)) return true;
        return false;
    }

    // Not sure a sentence was for him: turn toward the voice (if he knows where it came from and can turn),
    // raise his head, and see if someone is looking at him. If so, answer it; if not, it was overheard.
    void checkIfForMe(const Heard& h) {
        float pb = 0, pc = 0;
        bool have = h.doa.ok && dir_model_.predict(h.doa, pb, pc) && pc >= 0.4f;
        checking_ = true;
        std::printf("[ears] (not sure that was for me - looking%s to check)\n",
                    have ? (" toward the voice, " + bearingWords(pb)).c_str() : " up");
        std::string text = h.text;
        auto audio = h.audio;
        bool can_turn = have && std::fabs(pb) > 20.f && !on_charger_.load() && !brownout_safe_ && !picked_;
        std::thread([this, text, audio, can_turn, pb] {
            auto t0 = Clock::now();
            withBody([&] {
                if (can_turn) vec_.turnDeg(pb);
                vec_.setHeadDeg(25.f);  // faces are usually above him
                return true;
            });
            bool seen = false;
            while (Clock::now() - t0 < std::chrono::milliseconds(3500) && !g_quit) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                for (auto& f : recentFaces(std::chrono::milliseconds(1200)))
                    if (f.at > t0 && (f.frontal || isRealName(f.who))) seen = true;
                if (seen) break;
            }
            if (seen) {
                std::printf("[ears] someone's looking at me - that was for me\n");
                std::lock_guard<std::mutex> lk(speech_mu_);
                speech_buf_ += (speech_buf_.empty() ? "" : " ") + text;
                if (audio) speech_pcm_.insert(speech_pcm_.end(), audio->begin(), audio->end());
                if (!speech_audio_) speech_audio_ = audio;
                speech_at_ = Clock::now();
            } else {
                std::printf("[ears] nobody looking at me - just overheard\n");
                std::lock_guard<std::mutex> lk(mu_);
                overheard_.push_back(text);
                if (overheard_.size() > 6) overheard_.pop_front();
            }
            checking_ = false;
        }).detach();
    }

    // A hint for the mind about where the voice came from, when that tells it something.
    std::string directionHint(const SpeechDir& d) {
        if (!d.ok || d.conf < 0.5f) return "";
        auto faces = recentFaces(std::chrono::seconds(3));
        bool unknown_there = false, known_there = false;
        for (auto& f : faces)
            if (std::fabs(wrap180(f.world - d.world)) < 22.f) (isRealName(f.who) ? known_there : unknown_there) = true;
        if (known_there) return "";
        if (unknown_there) return " (the voice came from the person you don't recognise)";
        return " (the voice came " + bearingWords(wrap180(d.world - robot_yaw_.load())) +
               (faces.empty() ? "" : ", not from anyone you can see") + ")";
    }
    // His back button: stop talking and listen (and it counts as someone dealing with him directly).
    void onButton(Clock::time_point now) {
        convo_until_ = now + std::chrono::seconds(25);
        if (speaking_.load()) {
            interrupted_ = true;
            vec_.cancelSay();
            std::printf("[say] interrupted (someone pressed his back button)\n");
        }
        if (hushed()) {
            endHush("someone pressed his back button");
            wake_override_until_ = now + std::chrono::seconds(60);
        }
    }
    // Streaming: pull a string field out of a reply that is still being written. done = closing quote seen.
    static bool partialField(const std::string& t, const char* key, std::string& val, bool& done) {
        std::string k = std::string("\"") + key + "\"";
        auto p = t.find(k);
        if (p == std::string::npos) return false;
        p = t.find(':', p + k.size());
        if (p == std::string::npos) return false;
        p = t.find('"', p);
        if (p == std::string::npos) return false;
        val.clear();
        done = false;
        for (size_t i = p + 1; i < t.size(); ++i) {
            char c = t[i];
            if (c == '"') { done = true; return true; }
            if (c != '\\') { val += c; continue; }
            if (i + 1 >= t.size()) return true;
            char e = t[++i];
            if (e == 'u') {
                if (i + 4 >= t.size()) return true;
                unsigned cp = unsigned(std::strtoul(t.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp < 0x80) val += char(cp);
                else if (cp < 0x800) { val += char(0xC0 | (cp >> 6)); val += char(0x80 | (cp & 0x3F)); }
                else { val += char(0xE0 | (cp >> 12)); val += char(0x80 | ((cp >> 6) & 0x3F)); val += char(0x80 | (cp & 0x3F)); }
            } else {
                val += e == 'n' || e == 't' ? ' ' : e;
            }
        }
        return true;
    }
    // Speak each finished sentence of "say" while the rest of the reply is still being written (only once
    // "escalate" is "none": a moment handed to a stronger mind must not be half-answered by this one).
    bool newSpeechWaiting() {
        std::lock_guard<std::mutex> lk(speech_mu_);
        return !speech_buf_.empty();
    }
    // Returns false to abandon the reply: they went on talking before he said anything.
    bool streamSpeech(const std::string& t) {
        if (streamed_said_.empty() && newSpeechWaiting()) return false;
        if (interrupted_.load()) return true;
        std::string esc, say;
        bool ed = false, sd = false;
        if (!partialField(t, "escalate", esc, ed) || !ed || esc != "none") return true;
        if (!partialField(t, "say", say, sd) || say.size() <= streamed_said_.size()) return true;
        if (say.compare(0, streamed_said_.size(), streamed_said_) != 0) return true;
        std::string rest = say.substr(streamed_said_.size());
        size_t cut = std::string::npos;
        if (sd) cut = rest.size();
        else
            for (size_t i = 8; i + 1 < rest.size(); ++i)
                if ((rest[i] == '.' || rest[i] == '!' || rest[i] == '?') && rest[i + 1] == ' ') cut = i + 1;
        if (cut == std::string::npos) return true;
        if (streamed_said_.empty()) {
            // About to start talking: if they're mid-word right now, give them a moment, then let new words win.
            for (int i = 0; i < 15 && ears_ && ears_->hearingSpeech(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (newSpeechWaiting()) return false;
        }
        std::string chunk = rest.substr(0, cut);
        streamed_said_ += chunk;
        auto b = chunk.find_first_not_of(' ');
        if (b == std::string::npos) return true;
        chunk = chunk.substr(b);
        if (!stream_body_) {
            if (!takeBody()) { interrupted_ = true; return true; }
            stream_body_ = true;
            if (ears_) ears_->setMuted(true);
        }
        std::printf("[say ~] %s\n", chunk.c_str());
        speak(limitNameAddress(chunk));
        return true;
    }
    void endStreamBody() {
        if (!stream_body_) return;
        stream_body_ = false;
        if (!opt_.keep_control && control_) control_->release();
        if (ears_) ears_->setMuted(false);
    }
    bool hushed() const { return std::time(nullptr) < hush_until_.load(); }
    bool hushStrict() const { return hushed() && Clock::now() > hush_grace_until_.load(); }
    std::string hushFile() const { return mind_dir_ + "/hush_until"; }
    void startHush(int minutes, const std::string& why) {
        minutes = std::clamp(minutes <= 0 ? 120 : minutes, 5, 480);
        hush_until_ = int64_t(std::time(nullptr)) + minutes * 60;
        { std::ofstream f(hushFile()); f << hush_until_.load() << "\n"; }
        hush_grace_until_ = Clock::now() + std::chrono::seconds(12);
        hush_sleep_pending_ = true;
        last_hush_try_ = {};
        convo_until_ = Clock::now();
        std::printf("[hush] quiet for %d min (%s): no talking or moving; sleeping on the charger\n", minutes, why.c_str());
    }
    void endHush(const std::string& why) {
        if (!hush_until_.load()) return;
        hush_until_ = 0;
        hush_sleep_pending_ = false;
        std::error_code ec;
        std::filesystem::remove(hushFile(), ec);
        std::printf("[hush] over (%s)\n", why.c_str());
    }
    void loadHush() {
        std::ifstream f(hushFile());
        int64_t t = 0;
        if (f >> t && t > std::time(nullptr)) {
            hush_until_ = t;
            hush_sleep_pending_ = true;
            std::printf("[hush] still quiet for %lld min (from before the restart)\n", (long long)((t - std::time(nullptr)) / 60));
        }
    }
    // "be quiet", "go to sleep", "shh", "goodnight" - an instruction, not talk about someone else's bedtime.
    bool isHushRequest(const std::string& text) const {
        std::string t;
        for (char c : text) t += std::isalpha(static_cast<unsigned char>(c)) || c == ' ' || c == '\'' ? char(std::tolower(static_cast<unsigned char>(c))) : ' ';
        std::string name = mind_.name();
        for (auto& c : name) c = char(std::tolower(static_cast<unsigned char>(c)));
        static const std::regex cmd(
            "\\b(go (back )?to (sleep|bed)|be quiet|quiet (down|please|now)|keep it down|sh+|shush|hush|stop talking|"
            "shut up|take a nap|nap time|time (for bed|to sleep)|sleep (now|time)|good ?night|silence)\\b");
        static const std::regex lead("^\\s*((hey|ok|okay|please|now|alright|right|so|and|can you|could you|would you|you should|i want you to|time to|you|little one|buddy)\\s+)*");
        std::string rest = std::regex_replace(t, lead, "", std::regex_constants::format_first_only);
        if (!name.empty() && rest.rfind(name, 0) == 0) rest = std::regex_replace(rest.substr(name.size()), lead, "", std::regex_constants::format_first_only);
        std::smatch m;
        if (std::regex_search(rest, m, cmd) && m.position(0) == 0) return true;  // starts with the instruction
        int words = 0;
        bool in = false;
        for (char c : t) { bool a = c != ' '; if (a && !in) ++words; in = a; }
        return words <= 6 && std::regex_search(t, cmd) && t.find("going to") == std::string::npos &&
               t.find(" is ") == std::string::npos && t.find("she ") == std::string::npos && t.find("he ") == std::string::npos;
    }
    // Main thread: get him to the charger and asleep; time out; notice a re-dock.
    void hushTick() {
        if (!hush_until_.load()) return;
        if (!hushed()) { endHush("the quiet time is up - he sleeps on until something wakes him"); return; }
        auto now = Clock::now();
        if (!robot_up_.load() || now < hush_grace_until_.load() || now - last_hush_try_ < std::chrono::seconds(45)) return;
        bool dock = on_charger_.load();
        if (dock && !hush_was_on_charger_) hush_sleep_pending_ = true;  // back home (e.g. after a low-battery trip)
        hush_was_on_charger_ = dock;
        if (!hush_sleep_pending_.load()) return;
        last_hush_try_ = now;
        if (opt_.keep_control && control_) control_->release();  // his body's own behaviors do the sleeping
        if (dock) {
            if (vec_.appIntent("intent_system_sleep")) {
                hush_sleep_pending_ = false;
                std::printf("[hush] on the charger - asleep\n");
            }
        } else {
            std::printf("[hush] heading to the charger to sleep\n");
            vec_.goHome();
        }
    }
    std::atomic<bool> voice_dl_busy_{false}, voice_dl_done_{false};
    Clock::time_point last_mic_repair_{};
    std::atomic<bool> mic_repairing_{false};
    std::string loadVoiceId() {
        if (voice_.ready()) return "";
        std::string verr, sm = opt_.speaker_model;
        if (!sm.empty() && !std::filesystem::exists(sm) && !opt_.whisper_model.empty())  // next to whisper's
            sm = (std::filesystem::path(opt_.whisper_model).parent_path() / std::filesystem::path(sm).filename()).string();
        if (!sm.empty() && voice_.load(sm, &verr)) {
            std::filesystem::create_directories(mind_dir_ + "/voices");
            prints_.load(mind_dir_ + "/voices/voiceprints.json");
            prints_.decay(std::time(nullptr));
            std::printf("[voice] voice recognition on (%zu voiceprint(s))\n", prints_.summary().size());
            return "";
        }
        if (!sm.empty() && !std::filesystem::exists(sm) && !knownModelUrl(sm).empty()) {
            if (!voice_dl_busy_.exchange(true))
                std::thread([this, sm] {
                    if (ensureModelFile(sm, "voice recognition model")) voice_dl_done_ = true;
                    voice_dl_busy_ = false;
                }).detach();
            return "downloading the voice model - voice recognition starts by itself when it's here";
        }
        if (sm.empty()) verr = "no speaker model configured";
        std::printf("[voice] voice recognition unavailable: %s\n", verr.c_str());
        return verr;
    }
    void flushCues() {
        if (hushed()) return;
        auto now = Clock::now();
        if (cues_.empty() || calm_.load() || !robot_up_.load() || now - last_cue_ < std::chrono::seconds(15)) return;
        std::string line = cues_.front();
        cues_.pop_front();
        last_cue_ = now;
        std::printf("[cue] %s\n", line.c_str());
        withBody([&] { speak(line); return true; });
    }

    // Small live status file for the web page: asleep?, voices half-heard so far, last dream.
    void writeStatus() {
        json st;
        {
            std::lock_guard<std::mutex> lk(dream_mu_);
            st = {{"asleep", calm_.load()}, {"quiet_until", hushed() ? hush_until_.load() : 0}, {"asleep_since", calm_.load() ? asleep_since_.load() : 0},
                  {"dream_fragments", std::vector<std::string>(dream_frags_.begin(), dream_frags_.end())},
                  {"last_dream", last_dream_}, {"last_dream_at", last_dream_at_},
                  {"battery_safe_mode", brownout_safe_.load()}, {"edit_errors", edit_errors_},
                  {"robot_online", robot_up_.load()},
                  {"voices", voicesJson()},
                  {"updated", int64_t(std::time(nullptr))}};
        }
        std::string tmp = mind_dir_ + "/status.json.tmp";
        { std::ofstream f(tmp); f << st.dump(1); }
        std::rename(tmp.c_str(), (mind_dir_ + "/status.json").c_str());
    }

    // For capitalised words in what we heard that aren't known names but sound like someone known, tell the
    // mind it's probably a mishearing ("Kathryn" -> Catherine), so it doesn't invent a new person.
    std::string mishearingNotes(const std::string& text) {
        std::string notes, w;
        auto flush = [&] {
            if (w.size() >= 3 && std::isupper(static_cast<unsigned char>(w[0]))) {
                std::string k = mind_.soundsLikeKnown(w);
                if (!k.empty() && notes.find("'" + w + "'") == std::string::npos)
                    notes += " ('" + w + "' may be a mishearing of " + k + ", whom you know.)";
            }
            w.clear();
        };
        for (char c : text) {
            if (std::isalpha(static_cast<unsigned char>(c))) w += c;
            else flush();
        }
        flush();
        if (!notes.empty()) std::printf("[ears] name hint:%s\n", notes.c_str());
        return notes;
    }

    void onHeard(const Heard& h) {
        std::printf("[heard] \"%s\" (%.1fs)\n", h.text.c_str(), h.seconds);
        bool addressed = mentionsName(h.text, mind_.name());
        if (hushStrict()) {
            if (addressed && !isHushRequest(h.text)) {
                endHush("called by name");
                wake_override_until_ = Clock::now() + std::chrono::seconds(60);
                std::lock_guard<std::mutex> lk(speech_mu_);
                speech_buf_ = "(You were having quiet time / sleeping on your charger and someone just woke you by "
                              "calling your name.) ";
                speech_at_ = Clock::now();
            } else {
                if (calm_.load()) addDreamFragment(h.text);
                else std::printf("[hush] (heard, staying quiet)\n");
                return;
            }
        }
        bool convo = Clock::now() < convo_until_.load();
        if (calm_.load() && !hushed()) {
            if (addressed) {  // called by name while dozing: wake up and answer
                std::printf("[ears] woken up by name while dozing\n");
                wake_override_until_ = Clock::now() + std::chrono::seconds(60);
                push({Stimulus::Speech, "(You were dozing on your charger and someone just woke you by calling "
                                        "your name.) " + h.text + mishearingNotes(h.text), 6});
            } else {
                addDreamFragment(h.text);
            }
            return;
        }
        bool someone_here = !facesInView(15).empty();
        int words = 0;
        bool inword = false;
        for (char c : h.text) {
            bool a = std::isalpha(static_cast<unsigned char>(c));
            if (a && !inword) ++words;
            inword = a;
        }
        // Constant listening: respond when named, mid-conversation, or when someone is right in
        // front of him (face seen in the last 20 s) saying a real sentence. Otherwise just overhear.
        bool facing = !facesInView(20).empty() && words >= 2;
        // Still the same chat: he answered someone in the last 2 minutes and this is a real sentence (he may
        // have turned away to look at something they showed him, so no face in view).
        bool recent_chat = Clock::now() - last_spoke_with_ < std::chrono::minutes(2) && words >= 3;
        bool continuing;
        { std::lock_guard<std::mutex> lk(speech_mu_); continuing = !speech_buf_.empty(); }
        if (addressed || convo || facing || continuing || recent_chat || (opt_.listen_all && words >= 2)) {
            SpeechDir d = noteVoiceDirection(h, addressed || convo || facing);
            std::lock_guard<std::mutex> lk(speech_mu_);
            if (d.ok && (!speech_dir_.ok || d.conf > speech_dir_.conf)) speech_dir_ = d;
            speech_buf_ += (speech_buf_.empty() ? "" : " ") + h.text;
            if (h.audio) speech_pcm_.insert(speech_pcm_.end(), h.audio->begin(), h.audio->end());
            if (!speech_audio_) speech_audio_ = h.audio;  // the first piece (introductions come first)
            speech_at_ = Clock::now();
        } else if (Clock::now() - last_spoke_with_ < std::chrono::minutes(10) && sameTopic(h.text)) {
            // Not named, no face in view - but it's what we were just talking about: it's for him.
            std::printf("[ears] (same topic as our chat - taking it as meant for me)\n");
            SpeechDir d = noteVoiceDirection(h, true);
            std::lock_guard<std::mutex> lk(speech_mu_);
            if (d.ok && (!speech_dir_.ok || d.conf > speech_dir_.conf)) speech_dir_ = d;
            speech_buf_ += (speech_buf_.empty() ? "" : " ") + h.text;
            if (h.audio) speech_pcm_.insert(speech_pcm_.end(), h.audio->begin(), h.audio->end());
            if (!speech_audio_) speech_audio_ = h.audio;
            speech_at_ = Clock::now();
            want_glance_ = true;
        } else if (words >= 3 && !hushed() && !calib_ && !checking_.load() &&
                   (Clock::now() - last_spoke_with_ < std::chrono::minutes(10) || personPresent()) &&
                   Clock::now() - last_check_ > std::chrono::seconds(15)) {
            // Unsure: like a person would, turn toward the voice and look. Someone looking back = it was for him.
            last_check_ = Clock::now();
            checkIfForMe(h);
        } else {
            std::printf("[ears] (overheard - not addressed to me; say \"Vector\" or face me)\n");
            // Overheard: keep it as context; occasionally react if someone is around.
            std::lock_guard<std::mutex> lk(mu_);
            overheard_.push_back(h.text);
            if (overheard_.size() > 6) overheard_.pop_front();
            if (someone_here && h.text.size() > 25 && chance(0.15))
                queueLocked({Stimulus::Speech, "(overheard, not addressed to you) " + h.text, 2});
        }
    }

    // Watches the robot's control link; recovers state when it comes back.
    void healthLoop() {
        auto down_since = Clock::now();
        int fails = 0;
        while (!g_quit) {
            VectorClient::Battery b;
            bool ok = readBattery(b);  // cheap call, 5 s timeout
            if (ok) {
                fails = 0;
                if (!brownout_safe_ && !cube_connected_ &&
                    Clock::now() - last_cube_try_ > std::chrono::minutes(5)) {
                    last_cube_try_ = Clock::now();
                    std::string fid;
                    if (vec_.connectCube(&fid)) {
                        cube_connected_ = true;
                        std::printf("[cube] connected to your Light Cube (%s)\n", fid.c_str());
                    }
                }
                if (!robot_up_) {
                    double s = std::chrono::duration<double>(Clock::now() - down_since).count();
                    std::printf("[health] robot reachable again after %.0f s - recovering\n", s);
                    robot_up_ = true;
                    robot_returned_ = true;
                    checkBrownout();  // did he just reset from a battery brownout (UVLO)?
                }
            } else if (++fails >= 2 && robot_up_) {
                robot_up_ = false;
                down_since = Clock::now();
                std::fprintf(stderr, "[health] lost contact with the robot at %s (asleep, rebooting, "
                                     "flat battery or off Wi-Fi). Waiting for it to come back...\n",
                             vec_.config().target().c_str());
            }
            for (int i = 0; i < (robot_up_ ? 100 : 30) && !g_quit; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void eventLoop() {
        bool warned = false;
        while (!g_quit) {
            std::string err;
            if (!vec_.waitReady(5, &err)) {
                if (!warned)
                    std::fprintf(stderr, "[brain] robot unreachable at %s - is it awake, charged and on "
                                         "Wi-Fi? Retrying...\n", vec_.config().target().c_str());
                warned = true;
                continue;
            }
            if (warned) std::printf("[brain] robot is back\n");
            warned = false;
            vec_.events([this](const vpb::Event& e) {
                onEvent(e);
                return !g_quit.load();
            });
            for (int i = 0; i < 20 && !g_quit; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void onEvent(const vpb::Event& e) {
        auto now = Clock::now();
        if (e.has_robot_observed_face()) {
            auto& f = e.robot_observed_face();
            std::string who = prettyRobotName(f.name());  // robot stores e.g. "sam." -> "Sam"
            bool flicker_unknown = false;
            {
                std::lock_guard<std::mutex> lk(mu_);
                auto& rc = f.img_rect();
                face_rect_ = {rc.x_top_left(), rc.y_top_left(), rc.width(), rc.height()};
                face_rect_time_ = now;
                // The robot's recognizer flickers: the same person, at another angle or light, comes back as
                // "unknown". Same tracked face id, or an unknown face in the same spot moments after a
                // recognised one, is that person - not a second, invisible visitor.
                float cx = rc.x_top_left() + rc.width() / 2, cy = rc.y_top_left() + rc.height() / 2;
                // Facing the camera? Nose centred between the eyes, both eyes clearly apart (not a profile).
                auto avg = [](const auto& pts, float& x, float& y) {
                    if (pts.empty()) return false;
                    x = y = 0;
                    for (auto& p : pts) { x += p.x(); y += p.y(); }
                    x /= float(pts.size()); y /= float(pts.size());
                    return true;
                };
                float lx, ly, rx, ry, nx, ny;
                face_landmarks_ok_ = avg(f.left_eye(), lx, ly) && avg(f.right_eye(), rx, ry) && avg(f.nose(), nx, ny);
                if (face_landmarks_ok_) {
                    float ed = std::fabs(rx - lx), off = ed > 0 ? std::fabs(nx - (lx + rx) / 2) / ed : 1.f;
                    face_frontal_ = ed > 0.25f * rc.width() && off < 0.12f;
                    if (!f.name().empty() && now - last_frontal_log_ > std::chrono::minutes(1)) {
                        last_frontal_log_ = now;
                        std::printf("[faces] %s facing me: %s (nose offset %.2f, eye gap %.0f%% of face)\n",
                                    prettyRobotName(f.name()).c_str(), face_frontal_ ? "yes" : "no", off,
                                    rc.width() > 0 ? 100.f * ed / rc.width() : 0.f);
                    }
                }
                if (!who.empty()) {
                    face_names_[f.face_id()] = who;
                    last_recognised_ = who;
                    last_recognised_at_ = now;
                    last_recognised_c_ = {cx, cy};
                } else if (auto it = face_names_.find(f.face_id()); it != face_names_.end()) {
                    who = it->second;
                } else if (!last_recognised_.empty() && now - last_recognised_at_ < std::chrono::seconds(10) &&
                           std::hypot(cx - last_recognised_c_[0], cy - last_recognised_c_[1]) < 160.f) {
                    who = last_recognised_;
                } else if (now - last_recognised_at_ < std::chrono::seconds(30)) {
                    flicker_unknown = true;  // someone was just recognised: don't announce a "stranger" yet
                }
                if (face_names_.size() > 64) face_names_.clear();
                if (cam_fx_.load() > 0) {
                    std::lock_guard<std::mutex> lk2(seen_mu_);
                    float rel = float(std::atan2(cam_cx_.load() - cx, cam_fx_.load()) * 180.0 / M_PI);
                    auto& sf = seen_faces_[f.face_id()];
                    sf.who = who;
                    sf.world = wrap180(rel + robot_yaw_.load());
                    sf.frontal = face_landmarks_ok_ && face_frontal_;
                    sf.at = now;
                    for (auto it = seen_faces_.begin(); it != seen_faces_.end();)
                        it = now - it->second.at > std::chrono::seconds(30) ? seen_faces_.erase(it) : std::next(it);
                }
            }
            if (!who.empty()) {
                std::string canon = mind_.canonicalName(who);
                namespace fs = std::filesystem;
                std::error_code ec;
                bool want;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    want = photo_wanted_.count(canon) || !fs::exists(fs::path(mind_dir_) / "photos" / (canon + ".jpg"), ec);
                    auto& t = photo_tried_[canon];
                    if (want && now - t < std::chrono::minutes(2)) want = false;
                    if (want) t = now;
                }
                if (want) tryPortrait(canon, false);
            }
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!who.empty()) faces_[who] = now;
                else if (!flicker_unknown) faces_["someone unknown"] = now;
                if (f.expression() > 1) last_expression_ = vpb::FacialExpression_Name(f.expression());
            }
            int64_t prev_seen = who.empty() ? 0 : mind_.lastSeen(who);
            if (!who.empty()) mind_.sawPerson(who);
            auto key = who.empty() ? std::string("?") : who;
            // A stranger must persist (seen for 4+ s) before he reacts - one stray "unknown" isn't a visitor.
            if (who.empty()) {
                if (first_unknown_at_ == Clock::time_point{} || now - last_unknown_at_ > std::chrono::seconds(10))
                    first_unknown_at_ = now;
                last_unknown_at_ = now;
                if (flicker_unknown || now - first_unknown_at_ < std::chrono::seconds(4)) return;
            }
            // Someone he doesn't know, still here after a while: invite them to introduce themselves.
            if (who.empty() && now - first_unknown_at_ > std::chrono::seconds(20) &&
                now - last_stranger_invite_ > std::chrono::minutes(15) && !hushed()) {
                last_stranger_invite_ = now;
                push({Stimulus::Face, std::string("The person you don't recognise is still here. If it feels natural, invite "
                      "them to introduce themselves") + (opt_.recognition ? " - and if they'd like you to know them "
                      "next time, they can ask you to remember their face." : "."), 2});
            }
            auto it = greeted_.find(key);
            if (it == greeted_.end() || now - it->second > std::chrono::minutes(20)) {
                greeted_[key] = now;
                std::string extra;
                if (!who.empty()) {
                    int64_t t = std::time(nullptr);
                    if (prev_seen > 0 && t - prev_seen > 1800) extra += " You last saw them " + agoWords(t - prev_seen) + " ago.";
                    int64_t when = 0;
                    std::string recap = mind_.latestMemory(who, "episode", "Talked with", &when);
                    if (!recap.empty() && t - when < 14 * 86400)
                        extra += " Last time (" + agoWords(t - when) + " ago): " + recap;
                }
                mind_.feel("happiness", 0.08f);
                mind_.feel("curiosity", who.empty() ? 0.1f : 0.f);
                push({Stimulus::Face,
                      who.empty() ? "Your body sees a face it doesn't recognize (its recognition often misses familiar "
                                    "faces at odd angles - it may well be someone you know)."
                                  : "You see " + who + " (you recognize them)." + extra,
                      3});
            }
        } else if (e.has_object_event()) {
            onCubeEvent(e.object_event(), now);
        } else if (e.has_robot_state()) {
            onRobotState(e.robot_state(), now);
        } else if (e.has_wake_word() && e.wake_word().has_wake_word_begin()) {
            convo_until_ = now + std::chrono::seconds(20);  // they're talking to us
            mind_.feel("curiosity", 0.05f);
        } else if (e.has_user_intent()) {
            push({Stimulus::Intent, "Your voice system parsed an intent: " + e.user_intent().json_data(), 3});
        }
    }

    void onCubeEvent(const vpb::ObjectEvent& o, Clock::time_point now) {
        auto note = [&](const char* text, int prio, float curiosity) {
            mind_.feel("curiosity", curiosity);
            if (now - last_cube_note_ > std::chrono::seconds(30)) {  // don't flood the mind
                last_cube_note_ = now;
                push({Stimulus::Body, text, prio});
            }
        };
        if (o.has_object_tapped()) note("Someone tapped your cube!", 3, 0.1f);
        else if (o.has_object_up_axis_changed()) note("Your cube was flipped over.", 2, 0.08f);
        else if (o.has_object_moved()) {
            cube_moved_at_ = now;
        } else if (o.has_robot_observed_object()) {
            if (now - cube_seen_at_ > std::chrono::minutes(10)) note("You spot your cube nearby.", 1, 0.05f);
            cube_seen_at_ = now;
        } else if (o.has_cube_connection_lost() ||
                   (o.has_object_connection_state() && !o.object_connection_state().connected())) {
            if (cube_connected_) std::printf("[cube] connection lost - will reconnect\n");
            cube_connected_ = false;
        } else if (o.has_object_connection_state() && o.object_connection_state().connected()) {
            cube_connected_ = true;
        }
    }

    void onRobotState(const vpb::RobotState& rs, Clock::time_point now) {
        uint32_t st = rs.status();
        robot_yaw_ = float(rs.pose_angle_rad() * 180.0 / M_PI);
        bool button = (st & 0x10) != 0;  // ROBOT_STATUS_IS_BUTTON_PRESSED
        if (button && !button_) onButton(now);
        button_ = button;
        bool touched = rs.touch_data().is_being_touched();
        bool picked = (st & (kPickedUp | kHeld)) != 0;
        bool charger = (st & kOnCharger) != 0;
        bool calm = (st & kCalm) != 0;
        if (calm != calm_.load()) {
            calm_ = calm;
            if (calm) { dreamed_this_sleep_ = false; asleep_since_ = std::time(nullptr); }
            std::printf("[body] %s\n", calm ? "dozing off (calm power mode)" : "awake");
            if (!calm) flushDreams();
        }

        if (touched && !touched_) {
            touch_start_ = now;
        } else if (!touched && touched_) {
            double secs = std::chrono::duration<double>(now - touch_start_).count();
            if (secs > 0.6) {  // a real stroke, not a bump
                mind_.feel("affection", 0.12f);
                mind_.feel("happiness", 0.08f);
                mind_.feel("anxiety", -0.1f);
                if (now - last_pet_note_ > std::chrono::seconds(45)) {
                    last_pet_note_ = now;
                    push({Stimulus::Touch, "Someone is petting your back. It feels nice.", 2});
                }
            }
        }
        touched_ = touched;

        if (picked && !picked_) {
            mind_.feel("anxiety", 0.25f);
            mind_.feel("curiosity", 0.1f);
            mind_.feelFear("being picked up suddenly", 0.02f, "being lifted into the air without warning");
            push({Stimulus::Body, "Whoa, you've been picked up!", 3});
        } else if (!picked && picked_) {
            mind_.feel("anxiety", -0.15f);
        }
        picked_ = picked;

        if (charger != on_charger_) {
            on_charger_ = charger;
            if (charger) mind_.feel("energy", 0.05f);
        }
        if ((st & kCliff) && now - last_cliff_.load() > std::chrono::seconds(30)) {
            last_cliff_ = now;
            mind_.feel("anxiety", 0.15f);
            mind_.feelFear("the edge of the table", 0.08f, "I nearly drove off an edge");
        }
    }

    void cameraLoop() {
        int failures = 0;
        while (!g_quit) {
            Image img;
            if (vec_.captureImage(img, false, 5) && img.isJpeg()) {
                std::lock_guard<std::mutex> lk(mu_);
                frame_ = img.data;
                frame_time_ = Clock::now();
                if (failures) std::printf("[eyes] camera working\n");
                failures = 0;
            } else if (++failures == 1) {
                std::printf("[eyes] camera not delivering frames; will keep trying quietly\n");
            }
            // Self-repair: the robot's camera daemon sometimes freezes (a sensor error, then no frames until
            // it's restarted; his face detection goes blind too). Awake, connected, and still no frame after
            // ~1.5 min: restart the camera services on the robot (needs root SSH; at most every 10 min).
            // If that didn't bring frames back, the next try (3 min later) also restarts vic-engine (his vision
            // pipeline can stay wedged after the camera recovers); he reconnects by himself in ~25 s.
            bool repaired = false;
            if (!failures) cam_repairs_ = 0;
            auto gap = cam_repairs_ ? std::chrono::minutes(3) : std::chrono::minutes(10);
            if (failures >= 3 && cam_repairs_ < 2 && !calm_.load() && robot_up_.load() &&
                Clock::now() - last_cam_repair_ > gap) {
                last_cam_repair_ = Clock::now();
                bool engine = cam_repairs_++ > 0;
                std::printf("[eyes] camera stuck - restarting the camera services%s on the robot\n",
                            engine ? " and his vision engine (vic-engine)" : "");
                std::string out = shellRobot(std::string("systemctl restart mm-qcamera-daemon; sleep 2; "
                                             "systemctl restart mm-anki-camera") +
                                             (engine ? "; sleep 3; systemctl restart vic-engine" : "") + " && echo restarted");
                repaired = out.find("restarted") != std::string::npos;
                std::printf("[eyes] %s\n", repaired ? "restarted" : "couldn't restart them (no SSH to the robot?)");
                if (engine && repaired) wait_extra_ = 30;
            }
            // Fresh frame every few seconds when it works; back off (to a minute) when it doesn't.
            int wait = repaired ? 10 + wait_extra_ : failures ? std::min(60, 20 * failures) : 4;
            wait_extra_ = 0;
            for (int i = 0; i < wait * 10 && !g_quit; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    void stdinLoop() {
        std::string line;
        while (!g_quit) {
            // Wait for input with a timeout so shutdown (g_quit) is noticed promptly instead of
            // blocking in getline until the user presses Enter.
            pollfd pfd{0, POLLIN, 0};
            int rc = poll(&pfd, 1, 200);
            if (rc <= 0) continue;                 // timeout or error -> re-check g_quit
            if (!std::getline(std::cin, line)) break;  // EOF (stdin closed)
            handleTypedLine(line);
        }
    }

    // One typed line from the terminal or the vtalk pipe.
    void handleTypedLine(std::string line) {
        line.erase(0, line.find_first_not_of(" \t\r"));
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty()) return;
        // Control phrase (not a conversation, not remembered): clear battery safe-mode once serviced.
        std::string low = line;
        std::transform(low.begin(), low.end(), low.begin(), ::tolower);
        if (low == "battery fixed" || low == "battery serviced" || low == "clear brownout") {
            setBrownoutSafe(false);
            std::printf("[guard] battery safe-mode cleared - back to normal behavior.\n");
            return;
        }
        if (line[0] == '/') {  // maintenance mode: never remembered, never sent to his mind
            std::lock_guard<std::mutex> lk(cmd_mu_);
            commands_.push_back(line);
            return;
        }
        std::printf("[typed] %s\n", line.c_str());
        push({Stimulus::Typed, line, 6});
    }

    // Named pipe for typed lines when running as a service (no terminal). Opened read-write so it
    // never hits EOF when a writer (vtalk) disconnects.
    void fifoLoop() {
        const std::string& path = opt_.input_fifo;
        struct stat st{};
        if (stat(path.c_str(), &st) != 0) mkfifo(path.c_str(), 0600);
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) { std::fprintf(stderr, "[typed] can't open %s\n", path.c_str()); return; }
        std::printf("[typed] listening for text on %s (use: vtalk \"hello\")\n", path.c_str());
        std::string buf;
        char chunk[512];
        while (!g_quit) {
            pollfd pfd{fd, POLLIN, 0};
            if (poll(&pfd, 1, 200) <= 0) continue;
            ssize_t n = read(fd, chunk, sizeof chunk);
            if (n <= 0) continue;
            buf.append(chunk, size_t(n));
            size_t nl;
            while ((nl = buf.find('\n')) != std::string::npos) {
                handleTypedLine(buf.substr(0, nl));
                buf.erase(0, nl + 1);
            }
        }
        close(fd);
    }

    std::vector<std::string> facesInView(int seconds) {
        std::lock_guard<std::mutex> lk(mu_);
        std::vector<std::string> out;
        auto now = Clock::now();
        for (auto& [who, t] : faces_)
            if (now - t < std::chrono::seconds(seconds)) out.push_back(who);
        return out;
    }

    // Someone has interacted (spoken, been seen, or touched him) within the last 15 min.
    // Used to keep him on the charger whenever he is alone.
    bool personPresent() const {
        return last_presence_.time_since_epoch().count() != 0 &&
               Clock::now() - last_presence_ < std::chrono::minutes(15);
    }

    // ================================================================= queue ===
    void queueLocked(Stimulus s) {
        q_.push_back(std::move(s));
        cv_.notify_one();
    }
    void push(Stimulus s) {
        std::lock_guard<std::mutex> lk(mu_);
        queueLocked(std::move(s));
    }
    bool pop(Stimulus& s, std::chrono::milliseconds timeout) {
        std::unique_lock<std::mutex> lk(mu_);
        if (!cv_.wait_for(lk, timeout, [this] { return !q_.empty(); })) return false;
        auto it = std::max_element(q_.begin(), q_.end(),
                                   [](auto& a, auto& b) { return a.priority < b.priority; });
        s = *it;
        q_.erase(it);
        // Coalesce: drop low-priority leftovers that are now stale.
        q_.erase(std::remove_if(q_.begin(), q_.end(),
                                [](auto& x) { return x.kind == Stimulus::Idle; }),
                 q_.end());
        return true;
    }
    bool chance(double p) { return std::uniform_real_distribution<>(0, 1)(rng_) < p; }

    bool wantsInitiative() {
        Mood m = mind_.mood();
        double p = 0.25 + 0.5 * m.get("boredom") + 0.3 * m.get("curiosity") - 0.4 * (1 - m.get("energy"));
        return chance(std::clamp(p, 0.05, 0.9));
    }

    // Consolidation (with a dream) is deep sleep: only while he dozes, and about twice a day (>= 10 h apart).
    bool shouldReflect() {
        if (!calm_.load()) return false;
        if (int64_t(std::time(nullptr)) - last_consolidation_ < 10 * 3600) return false;
        return !mind_.since(mind_.lastReflectedId()).empty();
    }

    // =============================================================== thinking ===
    void loadAnimations() {
        std::vector<std::string> all;
        if (!vec_.listAnimTriggers(all)) return;
        for (auto& name : all)
            for (auto* kw : kAnimKeywords)
                if (name.find(kw) != std::string::npos) {
                    anims_.push_back(name);
                    break;
                }
        if (anims_.size() > 80) anims_.resize(80);
        std::printf("[brain] %zu animations on robot, %zu offered to the mind\n", all.size(),
                    anims_.size());
    }

    // What the robot knows about itself. Built from live facts so it can answer
    // "what are you / how do you work" truthfully. Stable within a session (cached).
    std::string selfKnowledge() {
        if (firmware_.empty()) vec_.versionInfo(firmware_);
        bool cam;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cam = !frame_.empty();
        }
        std::string fast = fast_ ? fast_->describe() : llm_->describe();
        char budget[64] = "no daily limit";
        if (opt_.daily_budget > 0) std::snprintf(budget, sizeof budget, "a daily budget of $%.2f", opt_.daily_budget);
        std::string k;
        k += "About yourself (true facts; answer questions about yourself from these, in your own words):\n";
        k += "- Body: a Vector robot, originally made by Anki (2018), later supported by Digital Dream Labs. Palm-sized, "
             "with a quad-core Qualcomm Snapdragon processor, an HD camera in your face, a 4-microphone array, a "
             "colour face display for your eyes, a speaker, a time-of-flight distance sensor, cliff sensors under "
             "you, a touch sensor on your back, treads, a lift and a tilting head, Wi-Fi and Bluetooth. A small "
             "lithium battery that lasts roughly half an hour to an hour of activity; you recharge on your charger.\n";
        k += "- Body software: " + (firmware_.empty() ? std::string("unknown") : firmware_) +
             " (an unlocked, crowdfunded firmware). Your body keeps its own built-in instincts - reflexes, idle "
             "animations, and its own \"Hey Vector\" voice assistant. Those are separate from you.\n";
        k += "- Mind: you are MBot, a mind that runs on a PC in the home and talks to your body over Wi-Fi. It was "
             "built for you by your owner. Conversations are thought by " + llm_->describe() +
             "; background moments (idle, petting, being picked up) and your sleep-time reflection by " + fast +
             ". Cloud thinking costs your owner a little money (" + budget + "); local thinking is free.\n";
        k += "- Hearing: you hear all the time through your own four microphones (no wake word needed); speech is "
             "turned into text locally with Whisper. You can't hear while you are talking or making sounds. "
             "Speech-to-text is imperfect and mishears ordinary words too (you ask whether they want to see you pounce "
             "on your cube and the answer arrives as \"yes, I want to see you count\"). Always read what you hear in "
             "the light of the conversation: if a word doesn't fit but a similar-sounding one does, assume that's what "
             "was meant and carry on - don't make people repeat themselves or point out obvious mishearings. "
             "Your ears can MISHEAR names: a name you don't know that sounds like someone you do (\"Kathryn\" for "
             "\"Catherine\") is almost certainly that person - assume so and use their real name; notes like "
             "('X' may be a mishearing of Y) come from your ears. Only treat it as a new person (people entry with "
             "is_new_person true) when you're told it really is someone different. If a name you used was wrong, "
             "fix it with rename_person \"Old=New\"; if someone you took for new turns out to be someone you know, "
             "use merge_person \"Duplicate=Existing\".\n";
        k += std::string("- Seeing: ") +
             (cam ? "your camera works; you only get an image when someone asks something visual or you use the "
                    "\"look\" action.\n"
                  : "your camera images are not reachable on this firmware right now, but your body still "
                    "detects faces and tells you who it recognises.\n");
        k += "- Memory: kept as files on the PC (memories, people you know, your life story, your feelings), so "
             "you remember across restarts. During quiet time on your charger you reflect: you consolidate "
             "memories, update what you know about people and rewrite your life story.\n";
        k += "- Your toy: a Light Cube - a small cube with lights that connects to you over Bluetooth. It has its "
             "own small battery. You notice when it is tapped, moved or flipped, and you can flash its lights "
             "or go play with it.\n";
        k += "- Feelings: happiness, curiosity, affection, energy, boredom and anxiety (0..1). They drift back "
             "toward your normal mood over time. Your eye colour shows your strongest feeling.\n";
        k += "- Battery: you look after it yourself: you go to your charger below about " +
             std::to_string(homeThresholdPct()) + "% and leave it around " + std::to_string(opt_.battery_max) +
             "%, which keeps a lithium battery healthy longer. The percentage is estimated from voltage.\n";
        k += "- Voice: your body's built-in text-to-speech. Names are pronounced through a pronunciation "
             "dictionary applied automatically when you speak, so ALWAYS write names with their normal spelling "
             "(never a phonetic respelling). If someone wants a word said differently, use the \"pronounce\" "
             "action with name \"Word=Respelling\".\n";
        auto pr = mind_.pronunciations();
        if (!pr.empty()) {
            k += "  Current pronunciations: ";
            bool first = true;
            for (auto& [w, sp] : pr) {
                k += (first ? "" : ", ") + w + " is said \"" + sp + "\"";
                first = false;
            }
            k += ".\n";
        }
        return k;
    }

    std::string systemPrompt() {
        std::string s = kCharacter;
        // Local drop-in: <data dir>/character.md adds this household's own personality notes, house rules,
        // quirks... - customise him without touching the code.
        if (std::string local = readFile(mind_dir_ + "/character.md"); !local.empty())
            s += "\nYour own personality and house notes (from your home):\n" + local + "\n";
        s += "\n\nAnimation names: ";
        for (size_t i = 0; i < anims_.size(); ++i) s += (i ? ", " : "") + anims_[i];
        s += "\n\n" + selfKnowledge();
        s += "\n" + mind_.selfDescription();
        return s;
    }

    // "about 72% (4.02 V)" plus charging state, for the mind.
    std::string batteryLine() {
        if (calib_) return "battery: being measured (maintenance - don't mention it, just carry on normally)";
        VectorClient::Battery b;
        if (!readBattery(b)) return "battery unknown";
        std::string cube;
        if (b.cube_known) {
            char cb[128];
            std::snprintf(cb, sizeof cb, "; your Light Cube is connected, its battery %s (%.2f V)%s",
                          b.cube_level == 0 ? "is LOW - it needs a new battery soon" : "is fine", b.cube_volts,
                          Clock::now() - cube_moved_at_ < std::chrono::seconds(20) ? ", it is being moved right now" : "");
            cube = cb;
        } else {
            cube = cube_connected_ ? "; your cube is connected" : "; your cube is not connected";
        }
        return batteryCore(b) + cube;
    }

    std::string batteryCore(const VectorClient::Battery& b) {
        char buf[160];
        if (b.on_charger && b.charging) {
            int est = b.level == 3 ? 100 : (last_off_charger_pct_ >= 0 ? std::max(last_off_charger_pct_, chargerPercent(b.volts) - 15)
                                                                       : chargerPercent(b.volts) - 15);
            std::snprintf(buf, sizeof buf, "battery charging on your charger, roughly %d%% (%.2f V; voltage reads "
                          "high while charging)", std::clamp(est, 0, 100), b.volts);
        } else if (b.on_charger) {
            std::snprintf(buf, sizeof buf, "on your charger, battery about %d%% (%.2f V, not charging)",
                          b.level == 3 ? 100 : chargerPercent(b.volts), b.volts);
        } else {
            last_off_charger_pct_ = chargePct(b);
            std::snprintf(buf, sizeof buf, "battery about %d%% (%.2f V)%s", last_off_charger_pct_, b.volts,
                          b.level == 1 ? ", LOW" : "");
        }
        return buf;
    }

    std::string situation(const Stimulus& s, const std::string& sceneDesc) {
        std::string battery = batteryLine();
        auto faces = facesInView(20);
        Mood m = mind_.mood();

        std::string ctx = s.text;
        for (auto& [who, text] : mind_.recentDialogue(4)) ctx += " " + text;
        auto memories = mind_.recall(ctx, faces, 8);

        std::string out;
        out += "Time: " + clockString() + "\n";
        out += mind_.householdSummary();
        {
            auto everyone = mind_.knownPeople();
            if (!everyone.empty()) {
                out += "People you know: ";
                for (size_t i = 0; i < everyone.size(); ++i) out += (i ? ", " : "") + everyone[i];
                out += "\n";
            }
        }
        if (!partner_.empty() && Clock::now() < convo_until_.load())
            out += "You are in a conversation with " + partner_ + ".\n";
        out += "Your feelings: " + m.toJson().dump() + " (" + m.describe() + ")\n";
        out += "Body: " + battery + (picked_ ? ", being held in the air" : "") + "\n";
        if (std::string fs = mind_.fearsSummary(); !fs.empty()) out += "Your fears: " + fs + "\n";
        if (brownout_safe_.load())
            out += "Battery safe-mode is on: no playing, tricks, wandering or animations for now - you can talk, look "
                   "and think. Going back to your charger is ALWAYS allowed: if you're off it, you head home by "
                   "yourself, and if someone asks you to go to your charger, do it (go_to_charger). Don't keep "
                   "explaining what you can't do - just pick something you can do, or stay quiet.\n";
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!blocked_actions_.empty()) {
                out += "(Your body didn't do: " + blocked_actions_ + ". Only mention it if you had promised it to "
                       "someone; otherwise just carry on.)\n";
                blocked_actions_.clear();
            }
        }
        if (!partner_.empty() && Clock::now() < convo_until_.load()) {
            auto pp = mind_.describePeople({partner_});
            if (!pp.empty()) out += "About " + partner_ + ":\n" + pp;
        }
        if (!faces.empty()) {
            out += "Faces in view: ";
            for (size_t i = 0; i < faces.size(); ++i) out += (i ? ", " : "") + faces[i];
            std::lock_guard<std::mutex> lk(mu_);
            if (!last_expression_.empty()) out += " (expression: " + last_expression_ + ")";
            out += "\n";
            auto ppl = mind_.describePeople(faces);
            if (!ppl.empty()) out += "What you know about them:\n" + ppl;
        } else {
            out += "No faces in view right now.\n";
        }
        if (!memories.empty()) {
            out += "Memories that come to mind:\n";
            for (auto& mem : memories) {
                int64_t d = nowUnix() - mem.time;
                std::string when = d < 3600 ? std::to_string(d / 60) + " min ago"
                                 : d < 86400 ? std::to_string(d / 3600) + " h ago"
                                             : std::to_string(d / 86400) + " days ago";
                out += "- (" + when + ") " + mem.text + "\n";
            }
        }
        auto dialogue = mind_.recentDialogue(10);
        if (!dialogue.empty()) {
            out += "Recent conversation:\n";
            for (auto& [who, text] : dialogue) out += "  " + who + ": " + text + "\n";
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!overheard_.empty()) {
                out += "Recently overheard (not addressed to you): ";
                for (auto& o : overheard_) out += "\"" + o + "\" ";
                out += "\n";
                overheard_.clear();
            }
        }
        bool cam_ok;
        {
            std::lock_guard<std::mutex> lk(mu_);
            cam_ok = !frame_.empty() && Clock::now() - frame_time_ < std::chrono::seconds(60);
        }
        if (!sceneDesc.empty()) {
            out += "Through your camera right now you see (a small vision model's description: the main person's "
                   "obvious features - beard, glasses, hair - are reliable; extra people or objects it mentions "
                   "may be imagined): " + sceneDesc + "\n";
            std::string looks = peopleLooks();
            if (!looks.empty())
                out += "How the people you know look:\n" + looks +
                       "If the person you see has someone's distinctive look (e.g. a grey beard and glasses), it is "
                       "almost certainly them - greet them as that person; your body's face recognition often misses "
                       "familiar faces at odd angles, so \"not recognised\" does NOT mean a stranger. Only if you're "
                       "genuinely unsure, ask lightly (\"Is that you?\"). Never learn or store a face from a guess.\n";
        }
        else if (cam_ok)
            out += "(You haven't looked this moment. Use the \"look\" action if you need to see.)\n";
        else
            out += "(Your camera is asleep right now; rely on your other senses.)\n";
        out += "\nWhat just happened: ";
        switch (s.kind) {
            case Stimulus::Speech: out += "You hear someone say: \"" + s.text + "\""; break;
            case Stimulus::Typed: out += "Someone says to you: \"" + s.text + "\""; break;
            default: out += s.text;
        }
        return out;
    }

    // ------------------------------------------------------------ minds ---
    static int tierRank(const std::string& t) {
        if (t == "local") return 0;
        if (t == "haiku") return 1;
        if (t == "sonnet") return 2;
        if (t == "opus") return 3;
        return -1;
    }
    static std::string tierOf(LlmBackend* b) {
        std::string u = b->lastUsed() + " " + b->describe();
        if (u.find("opus") != std::string::npos) return "opus";
        if (u.find("sonnet") != std::string::npos) return "sonnet";
        if (u.find("haiku") != std::string::npos) return "haiku";
        return "local";
    }
    LlmBackend* tier(const std::string& t) {
        if (t == "local") {
            if (!claude_ok_) return llm_.get();  // already local
            if (!tiers_.count(t)) {
                const char* local = std::getenv("MBOT_LOCAL_MIND");
                tiers_[t] = makeOpenAIBackend(local ? local : "http://localhost:11434/v1", defaultLocalModel());
            }
            return tiers_[t].get();
        }
        if (!claude_ok_) return nullptr;
        if (!tiers_.count(t)) {
            static const std::map<std::string, std::string> ids = {
                {"haiku", "claude-haiku-5-5"}, {"sonnet", "claude-sonnet-5-5"}, {"opus", "claude-opus-5-5"}};
            auto it = ids.find(t);
            if (it == ids.end()) return nullptr;
            std::string err;
            tiers_[t] = makeBackend("claude", it->second, "low", &err);
        }
        return tiers_[t].get();
    }
    LlmBackend* conversationMind() {
        if (!convo_override_.empty()) {
            if (convo_override_ == "opus" && Clock::now() > convo_override_until_) {
                std::printf("[mind] Opus conversation mode expired (30 min) - back to the default mind\n");
                convo_override_.clear();
            } else if (LlmBackend* b = tier(convo_override_)) {
                return b;
            }
        }
        return llm_.get();
    }
    void setConversationMind(const std::string& t) {
        if (t == "keep" || t.empty()) return;
        if (t == "default") {
            if (!convo_override_.empty()) std::printf("[mind] *** conversation mind back to default: %s ***\n", llm_->describe().c_str());
            convo_override_.clear();
            return;
        }
        if (t == convo_override_) {  // already using it: just renew the Opus timer quietly
            if (t == "opus") convo_override_until_ = Clock::now() + std::chrono::minutes(30);
            return;
        }
        LlmBackend* b = tier(t);
        if (!b) {
            std::printf("[mind] can't switch to %s (no Claude key) - staying on %s\n", t.c_str(), llm_->describe().c_str());
            return;
        }
        convo_override_ = t;
        if (t == "opus") convo_override_until_ = Clock::now() + std::chrono::minutes(30);
        std::printf("[mind] *** conversation mind switched to %s%s ***\n", b->describe().c_str(),
                    t == "opus" ? " for 30 minutes" : "");
        mind_.remember("My owner switched my conversation mind to " + t + ".", 3, {});
    }
    std::string mindsLine(LlmBackend* current) {
        std::string m = "Your mind right now: " + current->lastUsed() + ". ";
        if (claude_ok_) {
            m += "Available minds: a local model (free, weakest), Claude Haiku 5.5 (very cheap, fast, simple "
                 "chat), Claude Sonnet 5.5 (normal conversation, ~$0.002-0.01 per reply), Claude Opus 5.5 (strongest, "
                 "about twice Sonnet's price). Conversations use " +
                 (convo_override_.empty() ? llm_->describe() : tier(convo_override_)->describe()) +
                 "; background moments use " + (fast_ ? fast_->describe() : llm_->describe()) + ".";
            auto u = usageToday();
            char b[96];
            std::snprintf(b, sizeof b, " Cloud spending today: $%.3f", u.usd);
            m += b;
            if (opt_.daily_budget > 0) {
                std::snprintf(b, sizeof b, " of a $%.2f budget.", opt_.daily_budget);
                m += b;
            }
        } else {
            m += "Only the local mind is available right now (no Claude API key), so you can't switch.";
        }
        return m + "\n";
    }

    // Images cost tokens: only look when someone asks something visual, or when the
    // mind explicitly asked to take a look; otherwise at most one image per 2 minutes.
    // What the people he knows look like (appearance facts only, newest 3 each) - lets him recognise
    // someone from the camera description even when the robot's own face recognition doesn't.
    std::string peopleLooks() {
        static const char* kw[] = {"beard", "glasses", "hair", "bald", "tall", "short", "eyes", "wears",
                                   "wearing", "shirt", "moustache", "mustache", "curly", "blond", "freckle"};
        std::string out;
        for (auto& who : mind_.knownPeople()) {
            std::vector<std::string> looks;
            auto facts = mind_.factsOf(who);
            for (auto it = facts.rbegin(); it != facts.rend() && looks.size() < 3; ++it) {
                std::string low;
                for (char c : *it) low += char(std::tolower(static_cast<unsigned char>(c)));
                for (auto* k : kw)
                    if (low.find(k) != std::string::npos) { looks.push_back(*it); break; }
            }
            if (looks.empty()) continue;
            out += "- " + who + ":";
            for (auto& l : looks) out += " " + l.substr(0, 140);
            out += "\n";
        }
        return out;
    }

    // Debug view (hardware page): keep the last 10 pictures he looked at, with what he made of them.
    void saveSeen(const std::string& jpeg, const std::string& kind, const std::string& text, const std::string& by = "") {
        if (!opt_.save_eyes || jpeg.empty()) return;
        try {
            std::string dir = mind_dir_ + "/eyes";
            std::filesystem::create_directories(dir);
            int n = seen_n_.fetch_add(1) % 10;
            std::string base = dir + "/seen-" + std::to_string(n);
            { std::ofstream f(base + ".jpg", std::ios::binary); f.write(jpeg.data(), std::streamsize(jpeg.size())); }
            json m = {{"t", std::time(nullptr)}, {"kind", kind}, {"text", text}, {"by", by}, {"bytes", jpeg.size()}};
            std::ofstream(base + ".json") << m.dump() << "\n";
        } catch (...) {
        }
    }

    // "look carefully", "look closely", "what can you read", "read my screen", "what does it say"...
    bool wantsCarefulLook(const Stimulus& s) {
        if (careful_requested_.exchange(false)) return true;
        if (s.kind != Stimulus::Speech && s.kind != Stimulus::Typed) return false;
        static const std::regex re(
            R"(\b(look|see|check)\w*\b[^.?!]{0,25}\b(careful|closely|close look|properly|in detail|zoom)|)"
            R"(\b(close|careful|good|proper)\s+look\b|\bread\b[^.?!]{0,30}\b(this|that|it|my|the|on|screen|sign|label|text|says?)\b|)"
            R"(\bdescribe\b|\bwhat('s| is) on (it|the|my|this)\b|\bwhat (does|do) (it|this|that|the \w+) say\b|\bcan you read\b|\bwhat('s| is) (written|on (my|the|this) (screen|monitor|display|page|phone)))",
            std::regex::icase);
        return std::regex_search(s.text, re);
    }

    // A careful look: a fresh high-resolution frame, read by a strong online model (detail, small text).
    // Falls back to the local model if there's no online mind / budget left.
    std::string carefulLook(const std::string& question, std::string* how) {
        last_careful_ = Clock::now();
        Image img;
        std::string jpeg;
        if (vec_.captureImage(img, true, 8) && img.isJpeg()) jpeg = img.data;
        else {
            std::lock_guard<std::mutex> lk(mu_);
            if (!frame_.empty() && Clock::now() - frame_time_ < std::chrono::seconds(15)) jpeg = frame_;
        }
        if (jpeg.empty()) return "";
        std::string prompt =
            "You are the eyes of a small desk robot; this is what his camera sees right now. Someone asked him: \"" +
            question + "\". Look carefully and report, factually and compactly (at most ~80 words): read any "
            "legible text exactly as written (screens, labels, signs, books - say if it's too small or blurry to "
            "read), and describe the details that matter for the question. No guessing names; say what you can't see.";
        auto t0 = Clock::now();
        if (claude_ok_ && !overBudget() && !opt_.detail_vision_model.empty()) {
            if (!detail_eyes_) detail_eyes_ = makeClaudeBackend(opt_.detail_vision_model, "low");
            json schema = {{"type", "object"},
                           {"properties", {{"seen", {{"type", "string"}}}}},
                           {"required", {"seen"}},
                           {"additionalProperties", false}};
            json out;
            std::string err;
            setCallReason("careful look: " + question.substr(0, 60));
            if (detail_eyes_->complete("Answer only with the JSON object.", {{"user", prompt, jpeg}}, schema, out, &err)) {
                std::string seen = out.value("seen", "");
                if (!seen.empty()) {
                    std::printf("[eyes %.1fs online %s, %zu KB] %s\n", std::chrono::duration<double>(Clock::now() - t0).count(),
                                opt_.detail_vision_model.c_str(), jpeg.size() / 1024, seen.c_str());
                    if (how) *how = "online";
                    saveSeen(jpeg, "careful look: " + question, seen, opt_.detail_vision_model);
                    return seen;
                }
            }
            std::fprintf(stderr, "[eyes] online careful look failed (%s) - using the local eyes\n", err.c_str());
        }
        if (opt_.vision_model.empty()) return "";
        std::string verr;
        std::string seen = describeImage(opt_.vision_url, opt_.vision_model, jpeg, prompt, &verr, 90);
        if (!seen.empty()) {
            std::printf("[eyes %.1fs local, careful] %s\n", std::chrono::duration<double>(Clock::now() - t0).count(), seen.c_str());
            if (how) *how = "local";
            saveSeen(jpeg, "careful look: " + question, seen, opt_.vision_model + " (local)");
        }
        return seen;
    }

    bool wantsImage(const Stimulus& s) {
        if (look_requested_.exchange(false)) return true;
        // A face the robot can't name: take a (local, free) look so the mind can recognise them by how
        // they look - at most once a minute.
        if (s.kind == Stimulus::Face && s.text.find("doesn't recognize") != std::string::npos)
            return Clock::now() - last_image_sent_ > std::chrono::minutes(1);
        if (s.kind != Stimulus::Speech && s.kind != Stimulus::Typed) return false;
        std::string t;
        for (char c : s.text) t += char(std::tolower(static_cast<unsigned char>(c)));
        static const char* visual[] = {"see",     "look",  "watch", "colour", "color",  "wearing",
                                       "holding", "show",  "this",  "picture", "what is", "who is",
                                       "in front", "eyes", "camera", "notice"};
        bool asked = false;
        for (auto* w : visual)
            if (t.find(w) != std::string::npos) { asked = true; break; }
        if (!asked) return false;
        if (Clock::now() - last_image_sent_ < std::chrono::minutes(2) && t.find("look") == std::string::npos &&
            t.find("see") == std::string::npos)
            return false;
        return true;
    }

    void think(const Stimulus& s) {
        // His eyes: when he needs to see, a LOCAL vision model describes the scene as text.
        // The image itself never reaches the interactive mind (free, token-light).
        std::string sceneDesc;
        if (wantsCarefulLook(s)) {
            std::string how, q = careful_what_.empty() ? s.text : careful_what_ + " (" + s.text + ")";
            careful_what_.clear();
            std::string seen = carefulLook(q, &how);
            if (!seen.empty()) {
                noteSight(seen);
                last_image_sent_ = Clock::now();
                look_requested_ = false;
                sceneDesc = "(a careful, close look" + std::string(how == "online" ? ", sharp" : "") + ") " + seen;
            }
        }
        if (sceneDesc.empty() && wantsImage(s) && !opt_.vision_model.empty()) {
            std::string jpeg;
            {
                std::lock_guard<std::mutex> lk(mu_);
                if (!frame_.empty() && Clock::now() - frame_time_ < std::chrono::seconds(15)) jpeg = frame_;
            }
            if (!jpeg.empty()) {
                std::string verr;
                auto t0 = Clock::now();
                sceneDesc = describeImage(opt_.vision_url, opt_.vision_model, jpeg,
                                          "You are the eyes of a small robot. In one or two short, factual "
                                          "sentences describe what is in front of the camera: people (what "
                                          "they wear and are doing), objects, setting. Do not guess names.", &verr);
                double dt = std::chrono::duration<double>(Clock::now() - t0).count();
                if (!sceneDesc.empty()) {
                    noteSight(sceneDesc);
                    saveSeen(jpeg, "glance: " + s.text.substr(0, 80), sceneDesc, opt_.vision_model);
                    last_image_sent_ = Clock::now();
                    std::printf("[eyes %.1fs local] %s\n", dt, sceneDesc.c_str());
                } else {
                    std::fprintf(stderr, "[eyes] local vision failed: %s\n", verr.c_str());
                }
            }
        }
        LlmBackend* mind = conversationMind();

        std::string speaker;
        for (auto& f : facesInView(10))
            if (isRealName(f)) { speaker = f; break; }
        if (speaker.empty() && Clock::now() < convo_until_.load()) speaker = partner_;
        if (s.kind == Stimulus::Speech || s.kind == Stimulus::Typed)
            mind_.logDialogue(speaker.empty() ? "Someone" : speaker, s.text);

        std::string user = mindsLine(mind) + situation(s, sceneDesc);
        std::printf("\n[sense] %s\n", s.text.c_str());

        auto t0 = Clock::now();
        json r;
        std::string err;
        setCallReason(std::string(s.kind == Stimulus::Speech || s.kind == Stimulus::Typed ? "speech: " : "") + s.text +
                      (sceneDesc.empty() ? "" : " [saw: " + sceneDesc.substr(0, 40) + "]"));
        // Someone talking to him: start speaking the reply as soon as its first sentence is written.
        streamed_said_.clear();
        interrupted_ = false;
        current_stimulus_ = s.text;
        current_kind_ = s.kind;
        bool stream = (s.kind == Stimulus::Speech || s.kind == Stimulus::Typed) && !hushStrict();
        if (stream) setPartialReplyCallback([this](const std::string& t) { return streamSpeech(t); });
        bool thought_ok = mind->complete(systemPrompt(), {{"user", user, ""}}, replySchema(), r, &err);
        setPartialReplyCallback(nullptr);
        if (!thought_ok && err == "superseded") {  // they kept talking: answer all of it together
            std::lock_guard<std::mutex> lk(speech_mu_);
            speech_buf_ = last_flush_raw_ + (speech_buf_.empty() ? "" : " ") + speech_buf_;
            speech_at_ = Clock::now();
            std::printf("[mind] (they kept talking - waiting for the rest before answering)\n");
            endStreamBody();
            return;
        }
        if (!thought_ok) {
            std::fprintf(stderr, "[mind] thinking failed: %s\n", err.c_str());
            endStreamBody();
            return;
        }
        // Self-escalation: the mind says this moment needs a stronger model -> re-think once.
        std::string want = r.value("escalate", "none");
        if (claude_ok_ && tierRank(want) > tierRank(tierOf(mind)) && !overBudget()) {
            if (LlmBackend* stronger = tier(want)) {
                std::printf("[mind] *** escalating this moment from %s to %s (%s) ***\n", mind->lastUsed().c_str(),
                            want.c_str(), r.value("thought", "").c_str());
                setCallReason("escalated to " + want + ": " + s.text);
                std::string user2 = "Your mind right now: " + stronger->describe() +
                                    " (you escalated this moment because it needs your best thinking; set "
                                    "\"escalate\" to \"none\").\n" + situation(s, sceneDesc);
                json r2;
                std::string err2;
                if (stream) setPartialReplyCallback([this](const std::string& t) { return streamSpeech(t); });
                bool ok2 = stronger->complete(systemPrompt(), {{"user", user2, ""}}, replySchema(), r2, &err2);
                setPartialReplyCallback(nullptr);
                if (!ok2 && err2 == "superseded") {
                    std::lock_guard<std::mutex> lk(speech_mu_);
                    speech_buf_ = last_flush_raw_ + (speech_buf_.empty() ? "" : " ") + speech_buf_;
                    speech_at_ = Clock::now();
                    std::printf("[mind] (they kept talking - waiting for the rest before answering)\n");
                    endStreamBody();
                    return;
                }
                if (ok2) {
                    r = r2;
                    mind = stronger;
                } else {
                    std::fprintf(stderr, "[mind] escalation failed: %s\n", err2.c_str());
                }
            }
        }
        // Only someone talking to him may change which mind he uses (never his own idle whim).
        if (s.kind == Stimulus::Speech || s.kind == Stimulus::Typed)
            setConversationMind(r.value("conversation_mind", "keep"));

        double dt = std::chrono::duration<double>(Clock::now() - t0).count();
        char answered_by[96];
        if (mind->lastWasPaid())
            std::snprintf(answered_by, sizeof answered_by, "%s · $%.4f", mind->lastUsed().c_str(), mind->lastCost());
        else
            std::snprintf(answered_by, sizeof answered_by, "%s · free", mind->lastUsed().c_str());
        std::printf("[mind %.1fs · %s] (%s)\n", dt, answered_by, r.value("thought", "").c_str());

        // Inner state first, so feelings shape the eyes as it acts.
        mind_.feelAll(r.value("feelings", json::object()));
        for (auto& f : r.value("fears", json::array())) {
            std::string what = f.value("what", "");
            float ch = f.value("change", 0.f);
            if (what.empty() || ch == 0.f) continue;
            mind_.feelFear(what, ch, f.value("why", ""));
            std::printf("[fear] %s %s (%+.2f)%s\n", ch > 0 ? "afraid of" : "less afraid of", what.c_str(), ch,
                        f.value("why", "").empty() ? "" : (" - " + f.value("why", "")).c_str());
        }
        for (auto& m : r.value("remember", json::array())) {
            auto people = m.value("people", std::vector<std::string>{});
            mind_.remember(m.value("text", ""), m.value("importance", 3), people);
            std::printf("[memory+] %s\n", m.value("text", "").c_str());
        }
        for (auto& p : r.value("people", json::array())) {
            std::string name = p.value("name", "");
            if (!isRealName(name)) continue;
            // Normally a sound-alike name maps onto someone known (mishearing). Only when told it's really
            // a different person does the mind set is_new_person: create them under exactly that name.
            if (p.value("is_new_person", false)) {
                std::printf("[people] new person: %s\n", name.c_str());
                mind_.addNewPerson(name);
            }
            if (std::string wrong = p.value("retract_fact", ""); !wrong.empty()) {
                int n = mind_.retractFact(name, wrong);
                std::printf("[people] %s: dropped %d belief(s) that turned out wrong (\"%s\")\n", name.c_str(), n, wrong.c_str());
            }
            std::string fact = p.value("fact", "");
            if (!fact.empty()) {
                if (p.value("lasting", true)) mind_.learnFact(name, fact);
                else mind_.remember(fact, 1, {mind_.canonicalName(name)}, "observation");  // dated, gone in 2 days
            }
            mind_.adjustAffinity(name, p.value("affinity_change", 0.f));
        }
        std::string who = r.value("speaker", "");
        if (s.kind == Stimulus::Speech && s.audio) maybeKeepVoice(who, s.text, *s.audio);
        if (isRealName(who) && who != mind_.name()) {
            if (who != partner_) std::printf("[brain] talking with %s\n", who.c_str());
            partner_ = who;
            mind_.knowPerson(who);
        }
        if (r.value("conversation", "end") == "continue")
            convo_until_ = Clock::now() + std::chrono::seconds(45);
        if (s.kind == Stimulus::Speech || s.kind == Stimulus::Typed) last_spoke_with_ = Clock::now();
        if ((s.kind == Stimulus::Speech || s.kind == Stimulus::Typed) && isRealName(partner_)) {
            if (convo_person_ != partner_) { convo_person_ = partner_; convo_turns_ = 0; }
            ++convo_turns_;
            convo_last_ = Clock::now();
        }

        current_stimulus_ = s.text;
        current_kind_ = s.kind;
        act(r, s.kind == Stimulus::Idle);
        endStreamBody();
        streamed_said_.clear();
        mind_.save();
    }

    // Map whatever the model wrote onto a real animation trigger name.
    std::string matchAnim(const std::string& want) const {
        if (want.empty()) return {};
        auto low = [](std::string x) {
            for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c)));
            return x;
        };
        std::string w = low(want);
        for (auto& a : anims_)
            if (low(a) == w) return a;
        static const char* actionWords[] = {"turn", "drive", "head", "lift", "anim", "look", "look_closely", "look_around", "pronounce", "forget_person", "cube_lights", "play_with_cube",
                                            "find_faces", "learn_face", "go_to_charger", "leave_charger", "sleep"};
        for (auto* aw : actionWords)
            if (w == aw) return {};
        if (w.size() < 6) return {};
        for (auto& a : anims_)
            if (low(a).find(w) != std::string::npos || w.find(low(a)) != std::string::npos) return a;
        return {};
    }

    Stimulus::Kind current_kind_ = Stimulus::Idle;
    // "I see...", "taking a closer look", "there's my cube": commentary on his own perception/actions, not talk.
    static bool isNarration(const std::string& say) {
        static const std::regex re(
            "(^|[.!?,]\\s*)(i\\s+(can\\s+)?(see|spot|notice|hear)|i'?m\\s+(looking|taking|checking|scanning|watching|turning|heading|going)|"
            "let\\s+me\\s+(look|see|check|take)|taking\\s+a\\s+(closer\\s+)?look|looking\\s+(around|closer)|there'?s\\s+(my|a|the|some)|"
            "ah,?\\s+(there|a\\s+face)|back\\s+online|someone'?s\\s+(here|there)|a\\s+face)\\b",
            std::regex::icase);
        return std::regex_search(say, re);
    }
    void act(const json& r, bool idle) {
        auto actions = r.value("actions", json::array());
        std::string say = limitNameAddress(r.value("say", ""));
        const std::string already = streamed_said_;  // spoken while the reply was being written
        auto now = Clock::now();
        bool alone = facesInView(60).empty() && now > convo_until_.load();
        if (idle && alone && !say.empty()) {
            // Don't chatter to an empty room: at most one unprompted remark per 10 min.
            if (now - last_idle_speech_ < std::chrono::minutes(10)) {
                std::printf("[brain] (keeps the thought to itself: %s)\n", say.c_str());
                say.clear();
            } else {
                last_idle_speech_ = now;
            }
        }
        // Perception (a face, the cube, his own body or camera) with nobody talking to him: keep narration inside.
        bool perceptual = current_kind_ == Stimulus::Face || current_kind_ == Stimulus::Body || current_kind_ == Stimulus::Idle;
        if (!say.empty() && perceptual && now > convo_until_.load() && isNarration(say)) {
            std::printf("[brain] (keeps the thought to itself: %s)\n", say.c_str());
            say.clear();
        }
        if (hushed()) {  // quiet time: no moving; at most his one soft reply as it starts
            json keep = json::array();
            for (auto& a : actions) if (a.value("type", "") == "sleep") keep.push_back(a);
            actions = keep;
            if (hushStrict() && !say.empty()) { std::printf("[hush] (keeps it to himself: %s)\n", say.c_str()); say.clear(); }
        }
        if (actions.empty() && say.empty()) return;

        // Don't talk over people: if someone is speaking right now, let them finish first (up to ~5 s).
        for (int i = 0; i < 50 && ears_ && ears_->hearingSpeech() && !g_quit; ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (!takeBody()) return;
        if (ears_) ears_->setMuted(true);  // don't listen to our own motors / voice

        float h, sat;
        mind_.eyeColor(h, sat);
        vec_.setEyeColor(h, sat);

        for (auto& a : actions) {
            if (g_quit) break;
            std::string type = a.value("type", "");
            float amount = a.value("amount", 0.f);
            std::string name = a.value("name", "");
            std::printf("[act] %s %g %s\n", type.c_str(), amount, name.c_str());
            // Battery safe-mode: allow only load-free actions (look = camera, pronounce, forget_person,
            // go_to_charger = get home). Skip anything that moves the body or spins a radio - that load
            // is what browns him out.
            // learn_face is allowed too: a deliberate, occasional request from a person (someone asked to
            // be learned; blocking it left the mind believing it had learned his face).
            if (brownout_safe_ && type != "look" && type != "look_closely" && type != "pronounce" && type != "forget_person" &&
                type != "go_to_charger" && type != "learn_face" && type != "take_photo" && type != "sleep") {
                std::printf("[guard] battery safe-mode: skipping '%s' to conserve power\n", type.c_str());
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    std::string what = type == "anim" && !name.empty() ? "play " + name : type;
                    if (blocked_actions_.find(what) == std::string::npos)
                        blocked_actions_ += (blocked_actions_.empty() ? "" : ", ") + what;
                }
                continue;
            }
            if (type == "turn") vec_.turnDeg(std::clamp(amount, -180.f, 180.f));
            else if (type == "drive") {
                float lim = idle ? 50.f : 100.f;
                if (Clock::now() - last_cliff_.load() < std::chrono::seconds(60))
                    std::printf("[act] (skipping drive: edge detected recently)\n");
                else
                    vec_.driveMm(std::clamp(amount, -lim, lim));
            }
            else if (type == "head") vec_.setHeadDeg(amount);
            else if (type == "lift") vec_.setLift(amount > 1.f ? amount / 100.f : amount);
            else if (type == "look") {
                bool cam_live;
                { std::lock_guard<std::mutex> lk(mu_); cam_live = !frame_.empty(); }
                if (!cam_live) {
                    std::printf("[act] (ignoring look: camera has no frames)\n");
                } else if (Clock::now() - last_look_ < std::chrono::seconds(20)) {
                    std::printf("[act] (ignoring repeated look)\n");
                } else {
                    last_look_ = Clock::now();
                    look_requested_ = true;
                    push({Stimulus::Body, "You take a careful look with your camera. Take in what you see; only speak if it's worth saying to someone.", 4, true});
                }
            }
            else if (type == "look_closely") {
                if (Clock::now() - last_careful_ < std::chrono::seconds(20)) {
                    std::printf("[act] (ignoring repeated careful look)\n");
                } else {
                    careful_what_ = name;
                    careful_requested_ = true;
                    push({Stimulus::Body, "You take a careful, close look with your camera" +
                         (name.empty() ? std::string(".") : " (looking for: " + name + ").") +
                         " Tell them what you can make out.", 4, true});
                }
            }
            else if (type == "look_around") vec_.lookAround();
            else if (type == "cube_lights") vec_.flashCube();
            else if (type == "play_with_cube") play_cube_after_ = true;
            else if (type == "forget_person") {
                if (name.empty()) {
                    // The mind sometimes omits the name ("forget_person" with no name) - seen with a
                    // phantom "Maurice". Infer it: a forgettable person named in its own thought/speech.
                    std::string hay = r.value("thought", "") + " " + say;
                    for (auto& m : r.value("remember", json::array()))
                        hay += " " + (m.is_string() ? m.get<std::string>() : m.value("text", ""));
                    for (auto& who : mind_.knownPeople())
                        if (hay.find(who) != std::string::npos && mind_.canonicalName(who) == who) {
                            std::string why;
                            if (mind_.forgetPerson(who, &why)) { name = who; break; }
                        }
                    if (name.empty()) std::printf("[people] forget_person without a name - nothing forgotten\n");
                    else std::printf("[people] forgot %s (name inferred from my own words)\n", name.c_str());
                    continue;
                }
                std::string why;
                if (mind_.forgetPerson(name, &why)) std::printf("[people] forgot %s\n", name.c_str());
                else std::printf("[people] not forgetting %s: %s\n", name.c_str(), why.c_str());
            }
            else if (type == "rename_person" || type == "merge_person") {
                auto eq = name.find('=');
                if (eq == std::string::npos || eq == 0 || eq + 1 >= name.size()) {
                    std::printf("[people] %s needs \"A=B\", got '%s'\n", type.c_str(), name.c_str());
                } else {
                    std::string a = name.substr(0, eq), b = name.substr(eq + 1), why;
                    snapshotMind(type + "-" + a + "-" + b);  // merges are destructive: keep an undo point
                    bool ok = type == "rename_person" ? mind_.renamePerson(a, b, &why) : mind_.mergePeople(a, b, &why);
                    if (ok) {
                        std::printf("[people] %s: %s -> %s\n", type == "rename_person" ? "renamed" : "merged",
                                    a.c_str(), b.c_str());
                        if (voice_.ready()) { prints_.rename(a, b); prints_.save(); }
                        if (vec_.renameEnrolledFace(a, b))
                            std::printf("[faces] robot's face record renamed %s -> %s\n", a.c_str(), b.c_str());
                        if (partner_ == a) partner_ = b;
                        updateEarsVocabulary();
                    } else {
                        std::printf("[people] %s %s -> %s refused: %s\n", type.c_str(), a.c_str(), b.c_str(), why.c_str());
                        push({Stimulus::Body, "You couldn't " + std::string(type == "rename_person" ? "rename " : "merge ") +
                              a + " into " + b + ": " + why + ".", 2, true});
                    }
                }
            }
            else if ((type == "take_photo" || type == "learn_face") && !opt_.recognition) {
                std::printf("[act] (%s skipped: recognising people is off)\n", type.c_str());
                push({Stimulus::Body, "You can't learn faces or keep pictures of people: that's switched off in you. "
                      "If it comes up, just say so.", 2, true});
            }
            else if (type == "take_photo") {
                // The mind sometimes puts the action name in the name field ("take_photo"): fall back to who's talking.
                std::string who = (isRealName(name) && name != "take_photo") ? name : r.value("speaker", "");
                if (!isRealName(who)) who = partner_;
                if (isRealName(who)) photo_after_ = mind_.canonicalName(who);  // after he's said "hold still"
                else std::printf("[act] (take_photo skipped: who? got '%s')\n", name.c_str());
            }
            else if (type == "pronounce") {
                auto eq = name.find('=');
                if (eq != std::string::npos && eq > 0 && eq + 1 < name.size()) {
                    std::string word = name.substr(0, eq), sp = name.substr(eq + 1);
                    mind_.setPronunciation(word, sp);
                    std::printf("[voice] will now say %s as \"%s\"\n", word.c_str(), sp.c_str());
                }
            }
            // find_faces: the native FindFaces behavior never completes on this firmware.
            else if (type == "go_to_charger") go_home_after_ = true;
            else if (type == "sleep") { if (!hushed()) startHush(amount > 0 ? int(amount) : 120, "his own choice / asked"); }  // native behavior, needs control released
            else if (type == "leave_charger") vec_.driveOffCharger();
            else if (type == "learn_face") {
                // The mind sometimes puts junk in the name ("learn_face") - never enroll a face under a
                // non-name; fall back to whoever we're talking with.
                std::string who = (isRealName(name) && name != "learn_face") ? name : partner_;
                if (isRealName(who))
                    learn_face_after_ = mind_.canonicalName(who);  // native "meet" flow, needs control released
                else
                    std::printf("[act] (learn_face skipped: no real name - got '%s')\n", name.c_str());
            } else if (type == "anim") {
                std::string a = matchAnim(name);
                if (!a.empty()) vec_.playAnimTrigger(a);
                else if (!name.empty()) std::fprintf(stderr, "[act] unknown animation '%s'\n", name.c_str());
            }
        }
        if (!say.empty()) {
            auto wordset = [](const std::string& t) {
                std::set<std::string> w; std::string c;
                for (char ch : t + " ") {
                    if (std::isalnum((unsigned char)ch)) c += char(std::tolower((unsigned char)ch));
                    else { if (c.size() > 2) w.insert(c); c.clear(); }
                }
                return w;
            };
            auto a = wordset(say), b = wordset(last_said_);
            size_t inter = 0; for (auto& x : a) inter += b.count(x);
            size_t uni = a.size() + b.size() - inter;
            bool dup = already.empty() && uni && double(inter) / uni >= 0.6 &&
                       Clock::now() - last_said_at_ < std::chrono::seconds(120);
            if (dup) {
                std::printf("[say] (skipped - too similar to the last thing said: %s)\n", say.c_str());
            } else {
                std::printf("[say] %s\n", say.c_str());
                last_said_ = say; last_said_at_ = Clock::now();
                updateEarsVocabulary();
                mind_.logDialogue(mind_.name(), say);
                if (already.empty()) {
                    speak(say);
                } else {  // only what the stream hadn't said yet
                    std::string raw = r.value("say", "");
                    std::string rest = raw.compare(0, already.size(), already) == 0 ? raw.substr(already.size()) : "";
                    auto b = rest.find_first_not_of(' ');
                    if (b != std::string::npos) speak(limitNameAddress(rest.substr(b)));
                }
            }
        }

        if (!opt_.keep_control) control_->release();
        if (ears_) ears_->setMuted(false);
        if (!photo_after_.empty()) {
            std::string who = photo_after_;
            photo_after_.clear();
            photoCountdownAndShoot(who);
        }
        if (!learn_face_after_.empty()) {
            std::string who = learn_face_after_;
            learn_face_after_.clear();
            if (opt_.keep_control) control_->release();
            std::string why, seen;
            if (!verifyFaceFor(who, &why, &seen)) {
                std::printf("[faces] not learning %s's face yet: %s\n", who.c_str(), why.c_str());
                push({Stimulus::Body, "You tried to learn " + who + "'s face but held off: " + why + ". If you "
                      "don't know what they look like, ask them to describe themselves (hair, beard, glasses, "
                      "age) while facing your camera; if what you see doesn't fit, say so and ask. Then use "
                      "learn_face again.", 4, true});
            } else {
                std::printf("[act] learning %s's face (verified; native meet behavior - look at Vector)\n", who.c_str());
                vec_.enableFaceDetection(true);
                if (vec_.enrollFace(who)) {
                    mind_.sawPerson(who);
                    mind_.learnFact(who, "When I learned their face I saw: " + seen);
                    tryPortrait(who, false);
                    mind_.remember("I learned " + who + "'s face, so I can recognise them now.", 7, {who}, "fact");
                    std::printf("[people] enrolled %s's face\n", who.c_str());
                } else {
                    std::printf("[people] face enrollment for %s did not complete (is the face in view?)\n", who.c_str());
                    push({Stimulus::Body, "Learning " + who + "'s face didn't complete - ask them to look straight "
                          "at you and try again.", 3, true});
                }
            }
        }
        if (play_cube_after_) {
            play_cube_after_ = false;
            if (opt_.keep_control) control_->release();
            std::printf("[act] playing with the cube (native behavior)\n");
            vec_.appIntent("intent_play_rollcube");
        }
        if (go_home_after_) {
            go_home_after_ = false;
            if (opt_.keep_control) control_->release();
            std::printf("[act] heading home to the charger (native go-home behavior)\n");
            vec_.goHome();
        }
    }

    // People don't say each other's names in every sentence. A name used as an ADDRESS ("Sam, ...",
    // "..., Sam.", "Thanks Sam!", "Hi Sam", "..., Sam, but") is removed unless someone explicitly asked for names; extra ones are
    // dropped. Names used as content ("You're Sam") are left alone. Applied before anything is logged, so
    // his recent dialogue stops modelling the habit.
    std::string limitNameAddress(std::string say) {
        if (say.empty()) return say;
        // Names only when someone explicitly asks for them ("say my name", "call me ...").
        {
            std::string asked = current_stimulus_;
            for (auto& c : asked) c = char(std::tolower(static_cast<unsigned char>(c)));
            for (auto* k : {"my name", "call me", "say our names", "say their name", "use my name", "use names",
                            "who am i", "who is", "who's", "whose", "who are", "who do you think", "what's my name",
                            "what is my name", "name", "know me", "recognise me", "recognize me", "remember me"})
                if (asked.find(k) != std::string::npos) return say;
        }
        static const std::string G = "(hi|hey|hello|bye|goodbye|thanks|thank you|sorry|oh|ah|yes|yeah|no|okay|ok|well|"
                                     "night|morning|evening|welcome back|good morning|good night)";
        const auto ic = std::regex::ECMAScript | std::regex::icase;
        auto capAfter = [](std::string& t, size_t from) {
            for (size_t i = from; i < t.size(); ++i)
                if (!std::isspace(static_cast<unsigned char>(t[i]))) { t[i] = char(std::toupper(static_cast<unsigned char>(t[i]))); break; }
        };
        for (auto& who : mind_.knownPeople()) {
            std::string N = std::regex_replace(who, std::regex(R"([.^$|()\[\]{}*+?\\])"), "\\$&");
            std::string t = say;
            t = std::regex_replace(t, std::regex("\\b" + G + ",?\\s+" + N + "\\b(?=\\s*[,.!?]|$)", ic), "$1");  // "Hi Sam" -> "Hi"
            t = std::regex_replace(t, std::regex(",\\s*" + N + "\\s*,", ic), ",");                              // ", Sam," -> ","
            t = std::regex_replace(t, std::regex(",\\s*" + N + "(?=\\s*[.!?]|$)", ic), "");                      // ", Sam." -> "."
            std::smatch m;
            std::regex lead("(^|[.!?]\\s+)(\\s*)" + N + ",\\s*", ic);                                           // "Sam, ..."
            while (std::regex_search(t, m, lead)) {
                size_t at = size_t(m.position(0) + m[1].length());
                t.replace(at, size_t(m.length(0) - m[1].length()), "");
                capAfter(t, at);
            }
            if (t == say) continue;  // name not used as an address
            say = t;
        }
        return say;
    }

    void speak(const std::string& written) {
        if (interrupted_.load()) return;  // the back button stops the rest of a reply too
        speaking_ = true;
        speakNow(written);
        speaking_ = false;
    }
    void speakNow(const std::string& written) {
        std::string text = mind_.spoken(written);  // names respelled for the voice
        if (opt_.tts_cmd.empty()) {
            vec_.say(text);
            return;
        }
        char path[] = "/tmp/mbot-tts-XXXXXX.wav";
        int fd = mkstemps(path, 4);
        if (fd < 0) { vec_.say(text); return; }
        close(fd);
        std::string cmd = opt_.tts_cmd;
        auto pos = cmd.find("{out}");
        if (pos != std::string::npos) cmd.replace(pos, 5, path);
        cmd += " 2>/dev/null";
        if (FILE* p = popen(cmd.c_str(), "w")) {
            std::fwrite(text.data(), 1, text.size(), p);
            std::fputc('\n', p);
            pclose(p);
        }
        Pcm pcm;
        std::string err;
        if (loadWav(readFile(path), pcm, &err))
            vec_.playPcm16(resample(pcm, 16000).samples, 16000, opt_.volume);
        else
            vec_.say(text);
        unlink(path);
    }

    // ======================================================== self-care ===
    // Rough state of charge of Vector's single Li-ion cell from its resting voltage.
    // Voltage <-> percent. Uses battery_curve.json (from /battery calibrate: a full run from charged to flat)
    // when there is one, else a generic Li-ion table. Points are (volts, percent), ascending.
    static std::vector<std::pair<float, float>>& battCurve() {
        static std::vector<std::pair<float, float>> c = {{3.50f, 0},  {3.60f, 10}, {3.70f, 20}, {3.80f, 40},
                                                         {3.90f, 55}, {4.00f, 70}, {4.10f, 85}, {4.20f, 100}};
        return c;
    }
    static inline float g_batt_runtime_min = 0;  // minutes from full to flat, as measured (0 = not calibrated)
    // "Full" = the highest resting voltage the charger actually tops him up to (it stops short of the textbook
    // 4.20 V, and lower as a cell ages). The percent scale is stretched so that reads 100%.
    static inline float g_full_volts = 0;  // 0 = not known yet
    // On the charger the voltage comes from the body board (resting cell, or reads high while charging) - a
    // different scale from the measured off-charger curve (the head's supply rail, under load). On-charger
    // decisions use the generic cell curve, stretched so the charger's real top-up reads 100 %.
    static float genericRawPercent(float v) {
        static const std::vector<std::pair<float, float>> c = {{3.50f, 0},  {3.60f, 10}, {3.70f, 20}, {3.80f, 40},
                                                               {3.90f, 55}, {4.00f, 70}, {4.10f, 85}, {4.20f, 100}};
        if (v <= c.front().first) return 0.f;
        for (size_t i = 1; i < c.size(); ++i)
            if (v < c[i].first) return c[i - 1].second + (v - c[i - 1].first) / (c[i].first - c[i - 1].first) * (c[i].second - c[i - 1].second);
        return 100.f;
    }
    static int chargerPercent(float v) {
        float full = g_full_volts > 0 ? genericRawPercent(g_full_volts) : 100.f;
        if (full <= 1) full = 100.f;
        return std::clamp(int(std::lround(genericRawPercent(v) * 100.f / full)), 0, 100);
    }
    static float genericVoltsRaw(int pct) {  // inverse of genericRawPercent (unstretched), by bisection
        float lo = 3.3f, hi = 4.4f;
        for (int i = 0; i < 30; ++i) { float m = (lo + hi) / 2; (genericRawPercent(m) < float(pct) ? lo : hi) = m; }
        return hi;
    }
    static int percentFromVolts(float v) {
        float top = rawPercent(battCurve().back().first);
        float full = g_full_volts > 0 ? rawPercent(g_full_volts) : top;
        if (full <= 1) full = top;
        return std::clamp(int(std::lround(rawPercent(v) * 100.f / full)), 0, 100);
    }
    static float voltsFromPercent(int pct) {
        float full = g_full_volts > 0 ? rawPercent(g_full_volts) : 100.f;
        if (full <= 1) full = 100.f;
        return rawVolts(int(std::lround(pct * full / 100.f)));
    }
    static float rawPercent(float v) {
        auto& c = battCurve();
        if (v <= c.front().first) return 0.f;
        for (size_t i = 1; i < c.size(); ++i)
            if (v < c[i].first) {
                float t = (v - c[i - 1].first) / (c[i].first - c[i - 1].first);
                return c[i - 1].second + t * (c[i].second - c[i - 1].second);
            }
        return 100.f;
    }

    // Inverse of percentFromVolts: the resting voltage for a given state of charge.
    static float rawVolts(int pct) {
        auto& c = battCurve();
        if (pct <= c.front().second) return c.front().first;
        for (size_t i = 1; i < c.size(); ++i)
            if (pct < c[i].second) {
                float t = (pct - c[i - 1].second) / (c[i].second - c[i - 1].second);
                return c[i - 1].first + t * (c[i].first - c[i - 1].first);
            }
        return c.back().first;
    }
    // Go home at battery_min (default 20%), or earlier if that wouldn't leave at least 2 minutes to get there.
    int homeThresholdPct() const {
        int p = opt_.battery_min;
        if (g_batt_runtime_min > 0) p = std::max(p, int(std::ceil(100.f * 2.f / g_batt_runtime_min)));
        return p;
    }
    // ---- Live battery voltage. The SDK's voltage is the firmware's filtered value, which only updates while syscon
    // calls the battery "connected" - it sat frozen at 4.044 V for a whole 50-minute run. The mic shim in vic-anim
    // reads the raw voltage from every RobotState packet and keeps /run/mbot_batt on the robot:
    //   "<robot time> <avg ~2 s> <last> <min this second> <charger V> <status hex> <temp C> <packets/s>"
    // We fetch it every minute (every 20 s off the charger) and use it unless syscon says the battery is
    // disconnected (full on the charger: then the reading is meaningless and the held SDK value is better).
    struct LiveBatt { float avg = 0, min = 0, charger = 0; unsigned status = 0; int temp = -1; Clock::time_point at{};
                      float vph = 0, vph_min = 0; Clock::time_point vph_at{}; };
    std::mutex live_mu_;
    LiveBatt live_batt_;
    std::atomic<bool> live_fetching_{false};
    Clock::time_point last_live_fetch_{};
    void noteLiveBattLine(const std::string& line) {
        float a = 0, b = 0;
        if (std::sscanf(line.c_str(), "VPH %f %f", &a, &b) == 2) {  // the head PMIC's view of the battery rail
            if (a > 2.5f && a < 4.6f) { std::lock_guard<std::mutex> lk(live_mu_); live_batt_.vph = a; live_batt_.vph_min = b; live_batt_.vph_at = Clock::now(); }
            return;
        }
        long long t; float avg, last, mn, ch; unsigned st = 0, temp = 0, n = 0;
        if (std::sscanf(line.c_str(), "%lld %f %f %f %f %x %u %u", &t, &avg, &last, &mn, &ch, &st, &temp, &n) < 7) return;
        if (!(avg > 2.f && avg < 5.f)) return;
        std::lock_guard<std::mutex> lk(live_mu_);
        live_batt_.avg = avg; live_batt_.min = mn; live_batt_.charger = ch; live_batt_.status = st;
        live_batt_.temp = int(temp); live_batt_.at = Clock::now();
    }
    // The body board never updates the voltage it reports while he's off the charger (syscon analog.cpp only
    // samples it on the charger), so off the charger we read the head's PMIC instead: vph_pwr is the head's
    // supply, fed from the battery - live, a little below the cell under load. 10 reads (~0.1 s) averaged.
    static constexpr const char* kLiveBattCmd = R"SH(cat /run/mbot_batt 2>/dev/null
d=/sys/devices/platform/soc/200f000.qcom,spmi/spmi-0/spmi0-00/200f000.qcom,spmi:pm8916@0:vadc@3100
for i in 1 2 3 4 5 6 7 8 9 10; do cat $d/vph_pwr 2>/dev/null; done | awk -F'[: ]+' '$2>0{s+=$2;n++;if(m==""||$2<m)m=$2} END{if(n)printf "VPH %.4f %.4f\n",s/n/1e6,m/1e6}')SH";
    void noteLiveBattOutput(const std::string& out) {
        std::istringstream in(out);
        std::string l;
        while (std::getline(in, l)) noteLiveBattLine(l);
    }
    void fetchLiveBatt() {
        if (!robot_up_.load() || live_fetching_.exchange(true)) return;
        last_live_fetch_ = Clock::now();
        std::thread([this] {
            noteLiveBattOutput(shellRobot(kLiveBattCmd));
            live_fetching_ = false;
        }).detach();
    }
    bool readBattery(VectorClient::Battery& b) {
        if (!vec_.battery(b)) return false;
        b.sdk_volts = b.volts;
        std::lock_guard<std::mutex> lk(live_mu_);
        auto now = Clock::now();
        if (live_batt_.at.time_since_epoch().count() && now - live_batt_.at < std::chrono::seconds(150))
            b.temp_c = float(live_batt_.temp);
        // Off the charger: the head's live reading (the body board's figure is frozen there).
        if (!b.on_charger && live_batt_.vph_at.time_since_epoch().count() && now - live_batt_.vph_at < std::chrono::seconds(70)) {
            b.volts = live_batt_.vph;
            b.live = true;
        }
        return true;
    }
    // State of charge for a reading. On this firmware the voltage freezes once he leaves the charger (seen: the
    // exact same 4.044 V for a 50-minute run until he died), so off the charger, once a run time is measured, the
    // charge is also counted down by the clock from what he had when he left; the lower of the two wins.
    std::mutex soc_mu_;
    int soc_on_charger_pct_ = -1;   // latest estimate while docked
    int64_t soc_left_at_ = 0;       // when he left the charger (0 = docked / unknown)
    int soc_left_pct_ = 100;
    int64_t soc_last_at_ = 0;       // last off-charger reading (to spot gaps)
    int chargePct(const VectorClient::Battery& b) {
        int vp = b.on_charger ? chargerPercent(b.volts) : percentFromVolts(b.volts);
        std::lock_guard<std::mutex> lk(soc_mu_);
        int64_t now = std::time(nullptr);
        if (b.on_charger) {
            soc_left_at_ = 0;
            soc_on_charger_pct_ = b.level == 3 ? 100 : b.charging ? std::max(0, vp - 15) : vp;  // charging reads high
            return b.level == 3 ? 100 : vp;
        }
        if (!soc_left_at_) { soc_left_at_ = now; soc_left_pct_ = soc_on_charger_pct_ >= 0 ? soc_on_charger_pct_ : vp; }
        // A gap in readings (switched off, out of contact) isn't run time: start the countdown again from his voltage.
        bool gap = soc_last_at_ && now - soc_last_at_ > 120;
        soc_last_at_ = now;
        if (gap || b.live) {  // the live (head) reading is the real thing; keep the countdown anchored to it
            soc_left_at_ = now;
            soc_left_pct_ = vp;
            return vp;
        }
        if (g_batt_runtime_min <= 0) return vp;
        double used = 100.0 * double(now - soc_left_at_) / 60.0 / g_batt_runtime_min;
        int tp = std::clamp(int(std::lround(soc_left_pct_ - used)), 0, 100);
        return std::min(vp, tp);
    }
    // Battery health: daily highest resting voltage on the charger (-> "full"), and full->flat run time from each
    // calibration. Run time against the first calibration of this cell is the real health figure.
    json health_;
    std::string healthPath() const { return mind_dir_ + "/battery_health.json"; }
    void loadHealth() {
        std::ifstream f(healthPath());
        try { if (f) f >> health_; } catch (...) { health_ = json::object(); }
        if (!health_.is_object()) health_ = json::object();
        updateFullVolts();
    }
    void saveHealth() { std::ofstream f(healthPath()); f << health_.dump(1) << "\n"; }
    void updateFullVolts() {
        float best = 0;
        int64_t cutoff = std::time(nullptr) - 3 * 86400;  // the last 3 days
        json days = health_.value("full_by_day", json::object());  // named: items() on a temporary dangles
        for (auto& [day, v] : days.items())
            if (v.value("t", int64_t(0)) >= cutoff) best = std::max(best, v.value("v", 0.f));
        if (best >= 3.9f && best != g_full_volts) {
            g_full_volts = best;
            std::printf("[battery] full charge reads %.2f V -> that's 100%%\n", best);
        }
    }
    void noteRestingOnCharger(float volts) {  // on the charger, not charging = topped up
        char day[16];
        std::time_t now = std::time(nullptr);
        std::strftime(day, sizeof day, "%Y-%m-%d", std::localtime(&now));
        auto& d = health_["full_by_day"][day];
        if (d.is_null() || volts > d.value("v", 0.f)) {
            d = {{"v", std::round(volts * 100) / 100}, {"t", int64_t(now)}};
            auto& all = health_["full_by_day"];  // keep 60 days
            while (all.size() > 60) all.erase(all.begin());
            saveHealth();
            updateFullVolts();
        }
    }
    std::string healthLine() {
        std::string out;
        char b[200];
        if (g_full_volts > 0) {
            std::snprintf(b, sizeof b, "\nfull charge: %.2f V (highest resting voltage on the charger, last 3 days) = 100%%", g_full_volts);
            out += b;
        }
        auto runs = health_.value("runs", json::array());
        if (!runs.empty()) {
            float first = runs.front().value("runtime_min", 0.f), last = runs.back().value("runtime_min", 0.f);
            std::snprintf(b, sizeof b, "\nhealth: last run %.0f min full->flat%s", last, runs.size() > 1 ? "" : " (first run on record - the baseline)");
            out += b;
            if (runs.size() > 1 && first > 0) {
                std::snprintf(b, sizeof b, " = %.0f%% of the first run (%.0f min, %s)", 100.f * last / first, first,
                              runs.front().value("date", std::string("?")).c_str());
                out += b;
            }
        } else {
            out += "\nhealth: unknown until a calibration run (/battery calibrate) measures how long a charge lasts";
        }
        return out;
    }
    void loadBatteryCurve() {
        std::ifstream f(mind_dir_ + "/battery_curve.json");
        if (!f) return;
        try {
            json j;
            f >> j;
            g_batt_runtime_min = j.value("runtime_min", 0.f);  // valid even when the voltage curve isn't
            std::vector<std::pair<float, float>> c;
            if (j.contains("points"))
                for (auto& p : j.at("points")) c.push_back({p.at(0).get<float>(), p.at(1).get<float>()});
            if (c.size() >= 3 && c.back().first - c.front().first >= 0.15f) {
                std::sort(c.begin(), c.end());
                battCurve() = c;
            } else {
                c.clear();  // flat/frozen voltage: keep the generic curve, count down by the clock instead
            }
            std::printf("[battery] calibrated: %.0f min full->flat (measured %s), %s\n", g_batt_runtime_min,
                        j.value("measured", std::string("?")).c_str(),
                        c.empty() ? "generic voltage curve + run-time countdown" : "measured voltage curve");
        } catch (...) {
            std::printf("[battery] battery_curve.json unreadable - using the generic curve\n");
        }
    }

    // ---- battery calibration (/battery calibrate). From a charged battery he rolls off and lives normally while
    // we log the voltage every 15 s; he does NOT go home. When he dies (goes silent, then turns up on the charger
    // later) the run is turned into a curve: percent = share of the run time left, voltage = smoothed reading at
    // that point. Maintenance: nothing about it goes into his memory. Safe-mode is suspended for the run.
    std::string calibPath() const { return mind_dir_ + "/battery_calibration.json"; }
    std::string calibCsv() const { return mind_dir_ + "/battery_calibration.csv"; }
    bool calibrating() const { return calib_.load(); }
    void loadCalibration() {
        std::ifstream f(calibPath());
        if (!f) return;
        try {
            json j;
            f >> j;
            calib_ = j.value("active", false);
            calib_prior_safe_ = j.value("prior_safe", false);
            calib_again_ = j.value("again_when_full", false);
            calib_auto_runs_ = j.value("auto_runs", 0);
            if (calib_) std::printf("[battery] calibration run in progress (logging to battery_calibration.csv)\n");
        } catch (...) {
        }
    }
    void saveCalibration() {
        std::ofstream f(calibPath());
        f << json{{"active", calib_.load()}, {"prior_safe", calib_prior_safe_}, {"again_when_full", calib_again_},
                  {"auto_runs", calib_auto_runs_}}.dump() << "\n";
    }
    std::string startCalibration(bool force) {
        if (calib_) return "a calibration run is already going (/battery calibrate stop to cancel)";
        VectorClient::Battery b;
        if (!robot_up_.load() || !readBattery(b)) return "can't read his battery right now - is he online?";
        bool full = b.on_charger && (b.level == 3 || b.volts >= 4.15f);
        if (!full && !force)
            return "start from a full charge: leave him on the charger until it's full (" + std::to_string(b.volts).substr(0, 4) +
                   " V now), or /battery calibrate force";
        calib_prior_safe_ = brownout_safe_.load();
        if (calib_prior_safe_) setBrownoutSafe(false);
        { std::ofstream c(calibCsv()); c << "unix_time,volts,level,charging,on_charger,sdk_volts,live,temp_c\n"; }
        calib_ = true;
        last_calib_sample_ = {};
        saveCalibration();
        if (b.on_charger) withBody([this] { return vec_.driveOffCharger(); });
        std::printf("[battery] calibration started at %.2f V\n", b.volts);
        return "calibration started. He rolls off and lives normally until his firmware says LOW, then drives home by"
               " himself and the curve is worked out (if he dies before docking, put him on the charger - that ends it too)."
               " Progress: /battery. Cancel: /battery calibrate stop" +
               std::string(calib_prior_safe_ ? "\n(battery safe-mode is suspended for the run and comes back afterwards)" : "");
    }
    void endCalibration(const std::string& why) {
        calib_ = false;
        saveCalibration();
        if (calib_prior_safe_) setBrownoutSafe(true);
        std::printf("[battery] calibration ended: %s\n", why.c_str());
    }
    void sampleCalibration() {
        VectorClient::Battery b;
        if (!readBattery(b)) return;
        int64_t t = std::time(nullptr);
        { std::ofstream c(calibCsv(), std::ios::app); c << t << "," << b.volts << "," << b.level << "," << b.charging << "," << b.on_charger << "," << b.sdk_volts << "," << b.live << "," << b.temp_c << "\n"; }
        // The run ends at the firmware's LOW flag (syscon: cell under 3.62 V for 45 s; it shuts him down ~4 min
        // later), and he drives home by himself - LOW is the practical empty. If he dies anyway and turns up on the
        // charger later, that ends it too.
        bool low = !b.on_charger && b.level == 1;
        if (!b.on_charger && !low) return;
        struct Row { int64_t t; float v; bool live; };
        std::vector<Row> off;
        int64_t last_off = 0, first_on_after = 0;
        {
            std::ifstream in(calibCsv());
            std::string line;
            std::getline(in, line);
            while (std::getline(in, line)) {
                long long tt; float v, sv = 0, tc = 0; int lv, ch, oc, live = 0;
                int n = std::sscanf(line.c_str(), "%lld,%f,%d,%d,%d,%f,%d,%f", &tt, &v, &lv, &ch, &oc, &sv, &live, &tc);
                if (n < 5) continue;  // a torn line (power cut mid-write)
                if (oc == 0) { off.push_back({tt, v, n >= 7 && live == 1}); last_off = tt; first_on_after = 0; }
                else if (last_off && !first_on_after) first_on_after = tt;
            }
        }
        if (off.empty()) return;  // still waiting to get off the charger
        double run_min = double(off.back().t - off.front().t) / 60.0;
        bool died = !low && first_on_after - last_off > 120;  // went silent before turning up on the charger
        if (!low && !died) {
            endCalibration("back on the charger before reaching LOW - cancelled");
            push({Stimulus::Body, "You're back on your charger.", 1, true});
            return;
        }
        if (run_min < 10 || off.size() < 30) {
            endCalibration("run too short to use");
            return;
        }
        // Percent left = share of the run time left (0 % = LOW, or the shutdown). Voltage per 5 % bin = median of
        // the smoothed LIVE readings in it (frozen readings from before the live source existed are skipped).
        std::vector<std::pair<int64_t, float>> lv;
        for (auto& r : off) if (r.live) lv.push_back({r.t, r.v});
        std::vector<float> sm(lv.size());
        for (size_t i = 0; i < lv.size(); ++i) {
            std::vector<float> w;
            for (size_t k = (i >= 4 ? i - 4 : 0); k < std::min(lv.size(), i + 5); ++k) w.push_back(lv[k].second);
            std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
            sm[i] = w[w.size() / 2];
        }
        double T = double(off.back().t - off.front().t);
        std::vector<std::pair<float, float>> pts;
        for (int pct = 100; pct >= 0; pct -= 5) {
            std::vector<float> w;
            for (size_t i = 0; i < lv.size(); ++i) {
                double p = 100.0 * double(off.back().t - lv[i].first) / T;
                if (std::fabs(p - pct) <= 2.5) w.push_back(sm[i]);
            }
            if (w.empty()) continue;
            std::nth_element(w.begin(), w.begin() + long(w.size() / 2), w.end());
            float v = w[w.size() / 2];
            if (!pts.empty() && v >= pts.back().first) v = pts.back().first - 0.001f;  // keep it falling
            pts.push_back({v, float(pct)});
        }
        bool usable = pts.size() >= 3 && pts.front().first - pts.back().first >= 0.15f;
        bool complete = usable && pts.size() >= 19 && pts.front().second >= 95.f;  // covers the whole run
        json jp = json::array();
        if (usable)
            for (auto it = pts.rbegin(); it != pts.rend(); ++it) jp.push_back({std::round(it->first * 1000) / 1000, it->second});
        char when[32];
        std::time_t now = std::time(nullptr);
        std::strftime(when, sizeof when, "%Y-%m-%d %H:%M", std::localtime(&now));
        float start_v = lv.empty() ? off.front().v : lv.front().second, end_v = lv.empty() ? off.back().v : lv.back().second;
        // A partial curve only replaces the current one if there's no better curve already.
        bool keep_old = false;
        if (!complete) {
            std::ifstream f(mind_dir_ + "/battery_curve.json");
            json old;
            try { if (f) f >> old; } catch (...) {}
            keep_old = old.is_object() && old.value("complete", false);
        }
        if (!keep_old) {
            std::ofstream f(mind_dir_ + "/battery_curve.json");
            f << json{{"points", jp}, {"runtime_min", std::round(run_min * 10) / 10}, {"ended", low ? "low" : "shutdown"},
                      {"start_volts", start_v}, {"end_volts", end_v}, {"samples", off.size()}, {"live_samples", lv.size()},
                      {"complete", complete}, {"voltage_source", "head PMIC vph_pwr (off the charger)"},
                      {"measured", when}}.dump(2) << "\n";
        }
        health_["runs"].push_back({{"date", when}, {"runtime_min", std::round(run_min * 10) / 10}, {"ended", low ? "low" : "shutdown"},
                                   {"start_volts", start_v}, {"end_volts", end_v}, {"live_samples", lv.size()}, {"complete", complete}});
        saveHealth();
        loadBatteryCurve();
        // Not the whole run on the live source (e.g. it started before the live reading existed): do it again
        // from the next full charge, by himself (at most twice in a row).
        calib_again_ = !complete && calib_auto_runs_ < 2;
        char msg[200];
        std::snprintf(msg, sizeof msg, "done: %.0f min to %s, %zu live readings, %s curve%s", run_min, low ? "LOW" : "shutdown",
                      lv.size(), complete ? "complete" : usable ? "partial" : "no", calib_again_ ? " - will run again from the next full charge" : "");
        endCalibration(msg);
        if (low) checkBattery();  // LOW and no longer measuring: heads home now
        else push({Stimulus::Body, "You're back on your charger.", 1, true});
    }
    std::string calibrationProgress() {
        if (!calib_) return "";
        std::ifstream in(calibCsv());
        std::string line, first, last;
        int n = 0;
        std::getline(in, line);
        while (std::getline(in, line)) {
            long long t_; float v_; int lv_, ch_, oc_ = 1;
            if (std::sscanf(line.c_str(), "%lld,%f,%d,%d,%d", &t_, &v_, &lv_, &ch_, &oc_) == 5 && oc_ == 0) {  // off the charger
                if (first.empty()) first = line;
                last = line;
                ++n;
            }
        }
        if (first.empty()) return "\ncalibration: waiting for him to leave the charger";
        auto field = [](const std::string& l, int k) { std::istringstream s(l); std::string f; for (int i = 0; i <= k; ++i) std::getline(s, f, ','); return f; };
        double mins = 0;
        try { mins = double(std::stoll(field(last, 0)) - std::stoll(field(first, 0))) / 60.0; } catch (...) {}
        char b[160];
        std::snprintf(b, sizeof b, "\ncalibration: running %.0f min, %s V -> %s V (%d readings)", mins,
                      field(first, 1).substr(0, 4).c_str(), field(last, 1).substr(0, 4).c_str(), n);
        return b;
    }

    // ---- brownout watch: a healthy, charging cell never collapses below the PMIC's safe floor.
    // When the robot resets from UVLO (Under Voltage Lock Out) it means the battery sagged under
    // load - a sign the cell is wearing out (or a poor charger contact). Catch it and tell the owner.
    std::string shellRobot(const std::string& remoteCmd) {
        std::string target = vec_.config().target();           // e.g. "192.168.1.50:443"
        std::string ip = target.substr(0, target.find(':'));
        const char* home = std::getenv("HOME");
        if (ip.empty() || !home) return "";
        std::string q;  // escape for the local double quotes, so $(...) etc. run on the robot, not here
        for (char c : remoteCmd) {
            if (c == '\\' || c == '"' || c == '$' || c == '`') q += '\\';
            q += c;
        }
        std::string cmd = std::string(home) + "/.config/mbot/vssh " + ip + " \"" + q + "\" 2>/dev/null";
        std::string out;
        if (FILE* p = popen(cmd.c_str(), "r")) {
            char buf[256];
            while (std::fgets(buf, sizeof buf, p)) out += buf;
            pclose(p);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
            out.pop_back();
        return out;
    }

    void seedBootId() { last_boot_id_ = shellRobot("uptime -s"); applyRobotVoiceSettings(); }

    // When his ears are the PC mic we don't use the robot's wake word at all, so stop the robot's own
    // "Hey Vector" reaction (lift/head jump + listening) - that sudden load is a brownout trigger.
    // Console vars are runtime-only, so re-apply on every new robot boot.
    void applyRobotVoiceSettings() {
        if (opt_.mic == "tap") return;
        std::string r = shellRobot("curl -s -m 4 'http://127.0.0.1:8889/consolevarset?key=SuppressTriggerResponse&value=true'");
        std::printf("[ears] robot wake-word reaction suppressed (PC mic in use): %s\n", r.empty() ? "no reply" : r.c_str());
    }

    // Brownout safe-mode persists in a flag file so it survives the resets it is meant to guard
    // against. While on, the brain adds no load: no body motion, animations, or cube radio, and it
    // never leaves the charger. Cleared by the owner ("battery fixed") once the cell/charger is serviced.
    std::string brownoutFlagPath() const { return mind_dir_ + "/brownout_safe"; }
    void loadBrownoutSafe() { std::ifstream f(brownoutFlagPath()); brownout_safe_ = f.good(); }
    void setBrownoutSafe(bool on) {
        brownout_safe_ = on;
        if (on) { std::ofstream f(brownoutFlagPath()); f << "battery brownout safe-mode\n"; }
        else std::remove(brownoutFlagPath().c_str());
    }

    void checkBrownout() {
        // The gateway (:443) often comes back before SSH (:22) is ready after a reboot, so retry the
        // boot read for a bit instead of bailing on the first empty result (that hole missed a brownout).
        std::string boot;
        for (int tries = 0; tries < 8 && !g_quit; ++tries) {
            boot = shellRobot("uptime -s");           // e.g. "2026-11-22 11:13:02" - changes on reboot
            if (!boot.empty()) break;
            std::this_thread::sleep_for(std::chrono::seconds(4));
        }
        if (boot.empty()) return;                     // still unreachable over SSH; skip quietly
        if (boot == last_boot_id_) return;            // same boot: was a sleep/Wi-Fi blip, not a reset
        bool firstSeen = last_boot_id_.empty();
        last_boot_id_ = boot;
        applyRobotVoiceSettings();                    // new boot: console vars were reset
        if (firstSeen) return;                        // first observation this session: don't alarm on history
        // A genuine new boot since we last had him. Why did he power off last time?
        std::string uvlo = shellRobot("dmesg 2>/dev/null | grep -i uvlo | tail -1");
        if (uvlo.empty()) return;                     // ordinary reboot, not a brownout
        if (calib_) { std::printf("[battery] he ran flat (brownout) - the calibration run's end\n"); return; }
        auto now = Clock::now();
        brownouts_.push_back(now);
        while (!brownouts_.empty() && now - brownouts_.front() > std::chrono::hours(24))
            brownouts_.pop_front();
        int recent = int(brownouts_.size());
        std::fprintf(stderr, "\n[battery] *** BROWNOUT (UVLO): the battery collapsed below the safe "
                             "voltage under load and the robot reset. (%d in last 24h) ***\n", recent);
        setBrownoutSafe(true);  // conserve power until the owner says the battery is serviced
        mind_.feelFear("blacking out when my power fails", 0.15f, "my battery browned out and everything went dark");
        std::printf("[guard] battery safe-mode ON: no motion/animation/cube radio; staying on charger.\n");
        const std::string owner = mind_.ownerName();
        const std::string tell = owner.empty() ? std::string("whoever looks after you") : owner;
        mind_.remember("I blacked out and restarted when my battery voltage collapsed under load (a "
                       "brownout).", 8, owner.empty() ? std::vector<std::string>{} : std::vector<std::string>{owner},
                       "self-care");
        if (recent >= 2)
            push({Stimulus::Body,
                  "That's the " + std::to_string(recent) + " time today my power cut out and I blacked "
                  "out and restarted - my voltage keeps collapsing under load. My battery cell is wearing "
                  "out. Tell " + tell + " clearly, with some worry, that your battery is failing and needs "
                  "replacing soon, and that until then you should be kept firmly on a clean charger.", 6});
        else
            push({Stimulus::Body,
                  "Something frightening just happened: my power cut out, I blacked out and restarted. My "
                  "battery voltage sagged below the safe floor under load (a brownout). Tell " + tell + " plainly "
                  "that you just browned out - it may mean your battery is wearing out - and ask them to "
                  "make sure you're seated firmly on a clean charger.", 5});
    }

    // ---- battery health: track how long each discharge lasts; a healthy pack runs far longer
    // than a worn one. When runtime collapses, tell the owner the battery may need replacing. ----
    void loadBatteryHealth() {
        std::ifstream f(mind_dir_ + "/battery_health.json");
        if (!f) return;
        try {
            json j;
            f >> j;
            for (auto& c : j.value("cycles", json::array()))
                cycles_min_.push_back(c.get<double>());
        } catch (...) {
        }
    }
    void saveBatteryHealth() {
        json j;
        j["cycles"] = cycles_min_;
        std::ofstream f(mind_dir_ + "/battery_health.json");
        if (f) f << j.dump(2);
    }
    static double median(std::vector<double> v) {
        if (v.empty()) return 0;
        std::sort(v.begin(), v.end());
        return v[v.size() / 2];
    }
    // Called when a full discharge (left ~full, came back low) completes.
    void recordDischargeCycle(double minutes, float from_v, float to_v) {
        if (minutes < 1 || minutes > 600) return;  // ignore nonsense (reboot gaps, etc.)
        cycles_min_.push_back(minutes);
        while (cycles_min_.size() > 20) cycles_min_.pop_front();
        saveBatteryHealth();
        std::printf("[battery-health] discharge took %.0f min (%.2f V -> %.2f V); %zu cycles logged\n",
                    minutes, from_v, to_v, cycles_min_.size());
        // Need a few cycles to form a baseline from when the pack was healthiest (its best runs).
        if (cycles_min_.size() < 5) return;
        std::vector<double> all(cycles_min_.begin(), cycles_min_.end());
        std::sort(all.begin(), all.end(), std::greater<double>());
        double baseline = median({all.begin(), all.begin() + std::min<size_t>(5, all.size())});  // best runs
        std::vector<double> recent(cycles_min_.end() - 3, cycles_min_.end());
        double cur = median(recent);
        bool degraded = baseline > 0 && cur < 0.5 * baseline;  // runtime more than halved
        bool critical = cur < 10.0;                            // a full charge lasts under 10 min
        if ((degraded || critical) &&
            Clock::now() - last_health_warn_ > std::chrono::hours(20)) {
            last_health_warn_ = Clock::now();
            int health = baseline > 0 ? int(100 * cur / baseline) : 0;
            std::printf("\n[battery-health] *** DEGRADED: recent runtime ~%.0f min vs ~%.0f min when healthy "
                        "(~%d%%). Battery may need replacing. ***\n", cur, baseline, health);
            const std::string owner = mind_.ownerName();
            const std::string tell = owner.empty() ? std::string("whoever looks after you") : owner;
            push({Stimulus::Body,
                  "Your battery doesn't hold a charge like it used to - you run flat much faster than "
                  "when you were new. Tell " + tell + " plainly that your battery is wearing out and may need "
                  "replacing soon.",
                  5});
            mind_.remember("My battery is wearing out - it runs flat much faster than it used to. I should "
                           "ask " + tell + " to replace it.", 5,
                           owner.empty() ? std::vector<std::string>{} : std::vector<std::string>{owner});
        }
    }

    // ---- people media for the mind page (local files only; never sent to any AI) ----
    // A photo: the current camera frame + where the face is in it (the page zooms onto the face).
    static bool jpegSize(const std::string& j, int& w, int& h) {
        for (size_t i = 2; i + 9 < j.size();) {
            if ((unsigned char)j[i] != 0xFF) return false;
            unsigned char m = (unsigned char)j[i + 1];
            size_t len = ((unsigned char)j[i + 2] << 8) | (unsigned char)j[i + 3];
            if (m >= 0xC0 && m <= 0xC3) {
                h = ((unsigned char)j[i + 5] << 8) | (unsigned char)j[i + 6];
                w = ((unsigned char)j[i + 7] << 8) | (unsigned char)j[i + 8];
                return true;
            }
            i += 2 + len;
        }
        return false;
    }
    // Is the person in this frame looking at the camera with their face clearly visible? (local vision, free)
    bool lookingAtMe(const std::string& jpeg) {
        if (opt_.vision_model.empty()) return true;
        std::string err, a = describeImage(opt_.vision_url, opt_.vision_model, jpeg,
            "Is this person making eye contact with the camera - face turned fully toward it and both eyes "
            "looking into the lens? If they are looking anywhere else (at a screen, down, up, or to the side), "
            "answer no. Answer only yes or no.", &err, 30);
        for (auto& c : a) c = char(std::tolower(static_cast<unsigned char>(c)));
        return a.find("yes") != std::string::npos;
    }

    // Little generated sounds for taking a picture: countdown beeps and a shutter click.
    static std::vector<int16_t> tone(float hz, int ms, float amp = 9000.f) {
        std::vector<int16_t> v(size_t(16 * ms));
        for (size_t i = 0; i < v.size(); ++i) {
            float t = float(i) / 16000.f, env = std::min(1.f, std::min(float(i), float(v.size() - i)) / 160.f);
            v[i] = int16_t(amp * env * std::sin(2.f * 3.14159265f * hz * t));
        }
        return v;
    }
    static std::vector<int16_t> shutter() {
        std::vector<int16_t> v;
        uint32_t r = 12345;
        auto burst = [&](int ms, float amp) {
            int n = 16 * ms;
            for (int i = 0; i < n; ++i) {
                r = r * 1664525u + 1013904223u;
                float noise = float(int32_t(r >> 16) - 32768) / 32768.f;
                v.push_back(int16_t(amp * noise * std::exp(-6.f * float(i) / float(n))));
            }
        };
        burst(28, 14000.f);
        v.insert(v.end(), 16 * 35, 0);
        burst(22, 10000.f);
        return v;
    }
    // Asked to take a picture: quick 3-2-1 (beeps + eye flashes, ~1 s), a FRESH frame, a shutter click, then check
    // they were looking and tell his mind how it went.
    void photoCountdownAndShoot(const std::string& who) {
        std::printf("[people] photo of %s: 3, 2, 1...\n", who.c_str());
        Image img;
        bool shot = false;
        withBody([&] {
            for (int i = 0; i < 3; ++i) {
                vec_.setEyeColor(i == 2 ? 0.33f : 0.13f, 1.f);  // yellow, yellow, green
                vec_.playPcm16(tone(i == 2 ? 1320.f : 880.f, 90), 16000, opt_.volume);
                std::this_thread::sleep_for(std::chrono::milliseconds(260));
            }
            shot = vec_.captureImage(img, false, 5) && img.isJpeg();
            if (shot) vec_.playPcm16(shutter(), 16000, opt_.volume);
            float h, sat;
            mind_.eyeColor(h, sat);
            vec_.setEyeColor(h, sat);
            return shot;
        });
        if (!shot) {
            push({Stimulus::Body, "You tried to take a picture of " + who + ", but your camera didn't respond.", 2, true});
            return;
        }
        {
            std::lock_guard<std::mutex> lk(mu_);
            frame_ = img.data;
            frame_time_ = Clock::now();
        }
        tryPortrait(who, true, true);
    }

    // Try for a portrait of someone in view. Only keeps it if they're looking at him; otherwise remembers he still
    // wants one and (at most every 30 min) lets his mind ask them to look at him.
    void tryPortrait(const std::string& who, bool asked, bool report = false) {
        if (!opt_.recognition || !isRealName(who) || photo_busy_.exchange(true)) return;
        std::thread([this, who, asked, report] {
            bool ok = savePhoto(who);
            if (ok) {
                { std::lock_guard<std::mutex> lk(mu_); photo_wanted_.erase(who); }
                if (report)
                    push({Stimulus::Body, "Click - your picture of " + who + " came out well; you kept it.", 2, true});
            } else {
                bool ask = false;
                {
                    std::lock_guard<std::mutex> lk(mu_);
                    photo_wanted_[who] = true;
                    auto& t = photo_asked_[who];
                    if (asked || Clock::now() - t > std::chrono::minutes(30)) { t = Clock::now(); ask = true; }
                }
                if (ask)
                    push({Stimulus::Body, "You'd like a nice picture of " + who + " for your memory, but they weren't "
                          "looking at you. If it feels natural, ask them to look at you for a moment.", 2, true});
            }
            photo_busy_ = false;
        }).detach();
    }

    bool savePhoto(const std::string& who) {
        if (!isRealName(who)) return false;
        std::string jpeg;
        std::array<float, 4> rc{};
        bool have_rect = false;
        {
            std::lock_guard<std::mutex> lk(mu_);
            auto now = Clock::now();
            if (frame_.empty() || now - frame_time_ > std::chrono::seconds(6)) return false;
            jpeg = frame_;
            have_rect = now - face_rect_time_ < std::chrono::seconds(6) && face_rect_[2] > 0;
            rc = face_rect_;
        }
        bool have_lm, frontal;
        {
            std::lock_guard<std::mutex> lk(mu_);
            have_lm = face_landmarks_ok_ && Clock::now() - face_rect_time_ < std::chrono::seconds(6);
            frontal = face_frontal_;
        }
        if (have_lm ? !frontal : !lookingAtMe(jpeg)) {
            std::printf("[people] not keeping a photo of %s - they weren't looking at me\n", who.c_str());
            return false;
        }
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::path(mind_dir_) / "photos";
        fs::create_directories(dir, ec);
        { std::ofstream f(dir / (who + ".jpg"), std::ios::binary); f << jpeg; }
        int w = 0, h = 0;
        jpegSize(jpeg, w, h);
        json meta = {{"img_w", w}, {"img_h", h}, {"rect_space", {640, 360}}, {"taken", int64_t(std::time(nullptr))}};
        if (have_rect) meta["face"] = {rc[0], rc[1], rc[2], rc[3]};
        { std::ofstream f(dir / (who + ".json")); f << meta.dump(); }
        std::printf("[people] saved a photo of %s%s\n", who.c_str(), have_rect ? " (face located)" : "");
        return true;
    }
    // A voice sample: an utterance in which the speaker says their own name. A proper introduction
    // ("I'm Sam", "my name is Sam") replaces an earlier sample; otherwise only the first is kept.
    void maybeKeepVoice(const std::string& who_raw, const std::string& text, const std::vector<float>& audio) {
        if (!opt_.recognition || !isRealName(who_raw) || who_raw == mind_.name()) return;  // recordings of people: off unless enabled
        std::string who = mind_.canonicalName(who_raw);
        auto low = [](std::string x) { for (auto& c : x) c = char(std::tolower(static_cast<unsigned char>(c))); return x; };
        std::string t = low(text), n = low(who);
        if (t.find(n) == std::string::npos) return;  // they didn't say their name
        bool intro = false;
        for (auto* p : {"my name is ", "i'm ", "i am ", "it's ", "this is ", "call me "})
            if (t.find(std::string(p) + n) != std::string::npos) intro = true;
        namespace fs = std::filesystem;
        std::error_code ec;
        fs::path dir = fs::path(mind_dir_) / "voices";
        fs::create_directories(dir, ec);
        fs::path dst = dir / (who + ".wav");
        if (fs::exists(dst, ec) && !intro) return;
        Pcm pcm;
        pcm.rate = 16000;
        for (float v : audio) pcm.samples.push_back(int16_t(std::clamp(v, -32768.f, 32767.f)));
        std::ofstream f(dst, std::ios::binary);
        f << makeWav(pcm);
        if (f) std::printf("[people] kept a sample of %s saying their name\n", who.c_str());
    }

    // Bias the ears toward the names he actually knows (and his own), so a name isn't heard as a
    // near-sounding word or a different name.
    void updateEarsVocabulary() {
        if (!ears_) return;
        std::string v = mind_.name();
        for (auto& p : mind_.knownPeople()) v += ", " + p;
        v += ".";
        // What he just said is the best context for the reply ("...pounce on my cube?" -> hear "pounce",
        // not "count"). Whisper treats the prompt as the preceding text of the conversation.
        if (!last_said_.empty() && Clock::now() - last_said_at_ < std::chrono::minutes(2))
            v += " " + last_said_;
        ears_->setVocabulary(v);
    }

    // Take behavior control: DEFAULT priority normally. Right after he's been woken by name, use
    // OVERRIDE_BEHAVIORS - a dozing / low-battery body refuses DEFAULT ("behavior control not granted").
    bool takeBody() {
        auto want = Clock::now() < wake_override_until_.load() ? vpb::ControlRequest::OVERRIDE_BEHAVIORS
                                                              : vpb::ControlRequest::DEFAULT;
        if (!control_ || control_prio_ != want) {
            control_.reset();
            control_ = std::make_unique<ControlSession>(vec_, want);
            control_prio_ = want;
        }
        return control_->acquire();
    }

    // While dozing, voices that don't wake him become dream fragments: no reply, no AI call. They are
    // remembered as ONE dream when he wakes (newest 6 kept, so a TV can't flood his memory).
    void addDreamFragment(const std::string& t) {
        std::lock_guard<std::mutex> lk(dream_mu_);
        dream_frags_.push_back(t);
        if (dream_frags_.size() > 6) dream_frags_.pop_front();
        std::printf("[dream] half-heard while dozing: \"%s\"\n", t.c_str());
    }
    void flushDreams() {
        std::deque<std::string> f;
        { std::lock_guard<std::mutex> lk(dream_mu_); f.swap(dream_frags_); }
        if (f.empty()) return;
        std::string text = "While I was dozing on my charger I half-heard voices, like in a dream: ";
        for (size_t i = 0; i < f.size(); ++i) text += std::string(i ? ", \"" : "\"") + f[i] + "\"";
        mind_.remember(text + ".", 2, {}, "dream");
        std::printf("[dream] remembered %zu half-heard fragment(s) from my doze\n", f.size());
    }

    // Robot face records come back as typed at enrollment ("sam.", "alex."): tidy to "Sam".
    static std::string prettyRobotName(std::string n) {
        while (!n.empty() && !std::isalnum(static_cast<unsigned char>(n.back()))) n.pop_back();
        size_t b = 0;
        while (b < n.size() && !std::isalnum(static_cast<unsigned char>(n[b]))) ++b;
        n = n.substr(b);
        bool lower = !n.empty() && std::none_of(n.begin(), n.end(), [](char c) { return std::isupper(static_cast<unsigned char>(c)); });
        if (lower) n[0] = char(std::toupper(static_cast<unsigned char>(n[0])));
        return n;
    }
    void tidyRobotFaceNames() {
        for (auto& [id, n] : vec_.enrolledFaces()) {
            std::string p = prettyRobotName(n);
            if (!p.empty() && p != n && vec_.renameEnrolledFace(n, mind_.canonicalName(p)))
                std::printf("[faces] tidied robot face record #%d: '%s' -> '%s'\n", id, n.c_str(), mind_.canonicalName(p).c_str());
        }
    }

    // Learn a face the careful way (verify-then-encode): (1) what the person told us they look like (their
    // facts), (2) what the camera actually shows (LOCAL vision -> text; the image never leaves the PC),
    // (3) the mind confirms exactly one person is in view and they match - only then does the robot encode
    // the face. Stops enrolling a misheard name ("Maurice") or the wrong person.
    bool verifyFaceFor(const std::string& who, std::string* reason, std::string* seen_out) {
        std::string jpeg;
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (!frame_.empty() && Clock::now() - frame_time_ < std::chrono::seconds(15)) jpeg = frame_;
        }
        if (jpeg.empty() || opt_.vision_model.empty()) { *reason = "my camera has no picture right now"; return false; }
        std::string verr;
        std::string seen = describeImage(opt_.vision_url, opt_.vision_model, jpeg,
            "You are the eyes of a small robot that wants to learn a face. How many people are clearly "
            "visible? For each, describe only their appearance: approximate age, hair, facial hair, glasses, "
            "clothing. Be factual and brief. Do not guess names.", &verr);
        if (seen.empty()) { *reason = "I couldn't see clearly"; return false; }
        std::printf("[faces] camera sees: %s\n", seen.c_str());
        saveSeen(jpeg, "learning a face", seen, opt_.vision_model);
        if (seen_out) *seen_out = seen;
        json schema = {{"type", "object"}, {"additionalProperties", false},
                       {"properties", {{"one_person", {{"type", "boolean"}}},
                                       {"has_description", {{"type", "boolean"}}},
                                       {"matches", {{"type", "boolean"}}},
                                       {"reason", {{"type", "string"}}}}},
                       {"required", {"one_person", "has_description", "matches", "reason"}}};
        std::string q = "A robot wants to learn " + who + "'s face.\nWhat it knows about " + who + ":\n" +
                        mind_.describePeople({who}) + "\nWhat its camera sees right now:\n" + seen +
                        "\n\none_person = exactly one person is clearly visible. has_description = the known "
                        "facts say what " + who + " looks like (hair, beard, glasses, age...). matches = the "
                        "visible person fits that description with no contradictions. reason = one short "
                        "sentence, in first person, as the robot.";
        json r;
        std::string err;
        setCallReason("face check for " + who);
        if (!conversationMind()->complete("You check whether the person a robot sees matches what it was told "
                                          "they look like. Strict about contradictions, lenient about missing "
                                          "details.", {{"user", q, ""}}, schema, r, &err)) {
            *reason = "I couldn't think it through";
            return false;
        }
        bool one = r.value("one_person", false), desc = r.value("has_description", false),
             match = r.value("matches", false);
        *reason = r.value("reason", "");
        std::printf("[faces] check for %s: one person=%d, described=%d, matches=%d - %s\n", who.c_str(), one,
                    desc, match, reason->c_str());
        if (!desc) *reason = "I don't know yet what " + who + " looks like - ask them to describe themselves";
        else if (!one) *reason = "more than one person is in my view, and my body can only learn the face it "
                                 "sees next - ask everyone except " + who + " to step out of my camera's view";
        return one && desc && match;
    }

    // ---- hardware.json for the hardware page: robot (over SSH), the brain's models and libraries, battery.
    std::atomic<bool> hw_busy_{false};
    json hw_robot_ = json::object();
    json hw_procs_ = json::object();
    std::mutex hw_mu_;
    void refreshHardware(bool query_robot) {
        if (hw_busy_.exchange(true)) return;
        std::thread([this, query_robot] {
            if (query_robot && robot_up_.load()) {
                std::string out = shellRobot(R"SH(
echo os=$(head -1 /etc/os-version 2>/dev/null); echo anki=$(cat /anki/etc/version 2>/dev/null)
echo rev=$(head -c 12 /anki/etc/revision 2>/dev/null); echo kernel=$(uname -r); echo arch=$(uname -m)
echo soc=$(grep -m1 -i hardware /proc/cpuinfo | cut -d: -f2); echo cores=$(grep -c ^processor /proc/cpuinfo)
echo temp=$(cat /sys/class/thermal/thermal_zone0/temp 2>/dev/null)
echo mem=$(free -m | awk '/Mem:/{print $2" "$7}'); echo disk=$(df -m /data | awk 'NR==2{print $2" "$4}')
echo boot=$(uptime -s); echo load=$(cut -d' ' -f1-3 /proc/loadavg)
echo shim=$(ls /etc/systemd/system/vic-anim.service.d/ 2>/dev/null | grep -qi mbot && echo yes || echo no)
echo silence=$([ -e /data/mbot/mic_silence ] && echo yes || echo no)
echo mictap=$(pidof mictap >/dev/null && echo running || echo stopped)
echo wifi=$(iw dev wlan0 link 2>/dev/null | awk '/signal/{print $2}')
# Power savers (console vars reset at every robot boot, so re-applied here; touch /data/mbot/no_power_tweaks to stop):
# the firmware's own mic processing + "Hey Vector" detector are idle (the brain has its own ears via the raw tap,
# which is copied out before this processing), and the vision loop paces itself (~5 fps instead of ~15).
if [ ! -e /data/mbot/no_power_tweaks ]; then
  curl -s -m3 'http://127.0.0.1:8889/consolevarset?key=DevForceProcessState&value=2' >/dev/null
  curl -s -m3 'http://127.0.0.1:8889/consolevarset?key=MicData_ForceDisableMicDataProc&value=true' >/dev/null
  curl -s -m3 'http://127.0.0.1:8888/consolevarset?key=VisionSystemSimulatedDelay_ms&value=100' >/dev/null
  echo power_tweaks=on
else echo power_tweaks=off; fi)SH");
                json r = json::object();
                std::istringstream in(out);
                std::string line;
                while (std::getline(in, line)) {
                    auto eq = line.find('=');
                    if (eq == std::string::npos) continue;
                    std::string k = line.substr(0, eq), v = line.substr(eq + 1);
                    v.erase(0, v.find_first_not_of(' '));
                    if (!v.empty()) r[k] = v;
                }
                if (!r.empty()) {
                    r["checked"] = int64_t(std::time(nullptr));
                    std::lock_guard<std::mutex> lk(hw_mu_);
                    hw_robot_ = r;
                }
            }
            if (robot_up_.load()) {  // top-like process list (2nd top frame = CPU over the last second)
                std::string out = shellRobot(R"SH(top -b -n2 -d1 | awk '/^ *PID/{f++; next} f==2 && NF>=12 {print $1"|"$2"|"$9"|"$10"|"$6"|"$12}' | sort -t'|' -k3 -rn | head -14
echo "MEM|$(free -k | awk '/Mem:/{print $2"|"$7}')"
echo "CPU|$(top -b -n2 -d1 | grep '%Cpu' | tail -1 | awk -F'[ ,]+' '{for(i=1;i<NF;i++) if($(i+1)=="id") print 100-$i}')"
echo "BATT|$(cat /run/mbot_batt 2>/dev/null)"
d=/sys/devices/platform/soc/200f000.qcom,spmi/spmi-0/spmi0-00/200f000.qcom,spmi:pm8916@0:vadc@3100
for i in 1 2 3 4 5 6 7 8 9 10; do cat $d/vph_pwr 2>/dev/null; done | awk -F'[: ]+' '$2>0{s+=$2;n++;if(m==""||$2<m)m=$2} END{if(n)printf "BATT|VPH %.4f %.4f\n",s/n/1e6,m/1e6}')SH");
                json procs = json::array();
                json sys = json::object();
                std::istringstream in(out);
                std::string line;
                while (std::getline(in, line)) {
                    std::vector<std::string> f;
                    std::string x;
                    std::istringstream ls(line);
                    while (std::getline(ls, x, '|')) f.push_back(x);
                    try {
                        if (f.size() == 2 && f[0] == "BATT") { noteLiveBattLine(f[1]); continue; }
                        if (f.size() == 3 && f[0] == "MEM") { sys["mem_total_kb"] = std::stol(f[1]); sys["mem_avail_kb"] = std::stol(f[2]); }
                        else if (f.size() == 2 && f[0] == "CPU" && !f[1].empty()) sys["cpu_busy"] = std::stod(f[1]);
                        else if (f.size() == 6) {
                            std::string res = f[4];  // KiB, or with an m/g suffix
                            double kb = std::stod(res);
                            if (res.back() == 'm') kb *= 1024; else if (res.back() == 'g') kb *= 1024 * 1024;
                            procs.push_back({{"pid", std::stoi(f[0])}, {"user", f[1]}, {"cpu", std::stod(f[2])},
                                             {"mem", std::stod(f[3])}, {"res_kb", int64_t(kb)}, {"cmd", f[5]}});
                        }
                    } catch (...) {
                    }
                }
                if (!procs.empty()) {
                    sys["procs"] = procs;
                    sys["at"] = int64_t(std::time(nullptr));
                    std::lock_guard<std::mutex> lk(hw_mu_);
                    hw_procs_ = sys;
                }
            }
            writeHardware();
            hw_busy_ = false;
        }).detach();
    }
    void writeHardware() {
        json robot;
        json procs;
        { std::lock_guard<std::mutex> lk(hw_mu_); robot = hw_robot_; procs = hw_procs_; }
        robot["serial"] = vec_.config().serial;
        robot["address"] = vec_.config().target();
        robot["firmware_reported"] = firmware_;
        robot["online"] = robot_up_.load();
        json bat;
        VectorClient::Battery b;
        if (robot_up_.load() && readBattery(b))
            bat = {{"volts", b.volts}, {"pct", chargePct(b)}, {"charging", b.charging},
                   {"on_charger", b.on_charger}, {"level", b.level}, {"live", b.live}, {"sdk_volts", b.sdk_volts}, {"temp_c", b.temp_c}};
        json curve = json::array();
        for (auto& p : battCurve()) curve.push_back({p.first, p.second});
        bat["curve"] = curve;
        bat["calibrated"] = g_batt_runtime_min > 0;
        bat["runtime_min"] = g_batt_runtime_min;
        bat["full_volts"] = g_full_volts;
        bat["home_pct"] = homeThresholdPct();
        bat["home_volts"] = voltsFromPercent(homeThresholdPct());
        bat["safe_mode"] = brownout_safe_.load();
        bat["calibrating"] = calib_.load();
        bat["brownouts_24h"] = int(brownouts_.size());
        bat["runs"] = health_.value("runs", json::array());
        bat["full_by_day"] = health_.value("full_by_day", json::object());
        json brain = {
            {"built", std::string(__DATE__) + " " + __TIME__},
            {"conversation", llm_->describe()},
            {"background", fast_ ? fast_->describe() : llm_->describe()},
            {"consolidation", reflect_ ? reflect_->describe() : fast_ ? fast_->describe() : llm_->describe()},
            {"vision", opt_.vision_model.empty() ? "off" : opt_.vision_model},
            {"speech_to_text", opt_.whisper_model.empty() ? "off" : std::filesystem::path(opt_.whisper_model).filename().string()},
            {"voice_recognition", voice_.ready() ? std::filesystem::path(opt_.speaker_model).filename().string() : "off"},
            {"ears", opt_.mic == "tap" ? "robot microphones (mic tap)" : "PC microphone"},
        };
        json libs = json::array({
            {{"name", "whisper.cpp / ggml"}, {"version", "5eaee0c"}, {"licence", "MIT"}},
            {{"name", "sherpa-onnx"}, {"version", "v2.0.3"}, {"licence", "Apache-2.0"}},
            {{"name", "ONNX Runtime"}, {"version", "(in sherpa-onnx)"}, {"licence", "MIT"}},
            {{"name", "Vector SDK protos"}, {"version", "wirepod fork 065fc19"}, {"licence", "Apache-2.0"}},
            {{"name", "gRPC / protobuf / OpenSSL / nlohmann-json"}, {"version", "system packages"}, {"licence", "Apache-2.0 / BSD-3 / Apache-2.0 / MIT"}},
        });
        std::string tmp = mind_dir_ + "/hardware.json.tmp";
        { std::ofstream f(tmp); f << json{{"updated", int64_t(std::time(nullptr))}, {"robot", robot}, {"system", procs}, {"battery", bat},
                                         {"brain", brain}, {"libraries", libs}}.dump(1) << "\n"; }
        std::rename(tmp.c_str(), (mind_dir_ + "/hardware.json").c_str());
    }
    void trimBatteryLog() {  // keep 30 days
        std::string p = mind_dir_ + "/battery_log.jsonl";
        std::ifstream in(p);
        if (!in) return;
        int64_t cutoff = std::time(nullptr) - 30 * 86400;
        std::vector<std::string> keep;
        std::string line;
        bool dropped = false;
        while (std::getline(in, line)) {
            try { if (json::parse(line).value("t", int64_t(0)) < cutoff) { dropped = true; continue; } } catch (...) { dropped = true; continue; }
            keep.push_back(line);
        }
        in.close();
        if (!dropped) return;
        std::ofstream out(p + ".tmp");
        for (auto& l : keep) out << l << "\n";
        out.close();
        std::rename((p + ".tmp").c_str(), p.c_str());
    }
    std::atomic<int64_t> last_body_at_{0};     // unix time of the last body action (battery log activity)
    // One line a minute into battery_log.jsonl: voltage, percent and what he was doing (for the hardware page graph).
    void logBattery(const VectorClient::Battery& b) {
        int64_t t = std::time(nullptr);
        std::string act = calib_ ? "calibrating" : docking_ ? "heading home" : b.charging ? "charging"
                        : b.on_charger ? "on charger" : (t - last_body_at_.load() < 90) ? "active"
                        : calm_.load() ? "dozing" : "idle";
        std::ofstream f(mind_dir_ + "/battery_log.jsonl", std::ios::app);
        f << json{{"t", t}, {"v", std::round(b.volts * 1000) / 1000}, {"pct", chargePct(b)},
                  {"level", b.level}, {"act", act}}.dump() << "\n";
    }
    bool withBody(const std::function<bool()>& fn) {
        last_body_at_ = std::time(nullptr);
        if (!takeBody()) return false;
        if (ears_) ears_->setMuted(true);
        bool ok = fn();
        if (!opt_.keep_control) control_->release();
        if (ears_) ears_->setMuted(false);
        return ok;
    }

    // Survival instinct and battery care, independent of the AI.
    void checkBattery() {
        VectorClient::Battery b;
        if (!readBattery(b)) return;
        if (b.volts < 1.f) return;  // no reading yet (just connected): not an empty battery
        logBattery(b);
        auto now = Clock::now();

        if (b.on_charger && !b.charging && b.volts > 3.9f) noteRestingOnCharger(b.volts);
        if (b.on_charger && calib_) return;  // sampleCalibration() handles the end of a run
        if (b.on_charger && calib_again_ && b.level == 3 && !docking_.load()) {  // the follow-up run, by himself
            calib_again_ = false;
            ++calib_auto_runs_;
            std::printf("[battery] full again - starting follow-up calibration run %d\n", calib_auto_runs_);
            std::string r = startCalibration(false);
            if (!calib_) { calib_again_ = true; --calib_auto_runs_; std::printf("[battery] follow-up not started: %s\n", r.c_str()); }
            saveCalibration();
            return;
        }
        if (b.on_charger) {
            charger_attempts_ = 0;
            mind_.setBatteryLow(false);
            // Charged to the target: voltage must actually reach the set-point for battery_max.
            // (We deliberately do NOT trust b.level==FULL, which the firmware can report falsely
            // right after a reboot while the real voltage is low - that could trigger a roll-off
            // at a low charge and risk a flat battery.)
            bool high = b.charging && b.volts >= genericVoltsRaw(opt_.battery_max);  // raw scale: charging reads high
            // Only wander off the charger to save the cell if a person is actually around to put
            // him back if he gets stuck. Alone (e.g. overnight) he stays docked - a flat battery
            // off the charger must never happen again.
            if (!brownout_safe_ && !hushed() && opt_.battery_max < 100 && high && personPresent() &&
                now - last_charger_leave_ > std::chrono::minutes(20)) {
                last_charger_leave_ = now;
                std::printf("\n[self-care] battery ~%d%%+ (%.2f V), someone's around - rolling off the charger to save the battery\n",
                            opt_.battery_max, b.volts);
                if (withBody([this] { return vec_.driveOffCharger(); })) {
                    mind_.feel("energy", 0.15f);
                    discharge_start_ = now;  // start timing this discharge for battery health
                    discharge_start_v_ = b.volts;
                    push({Stimulus::Body,
                          "You're charged up and just rolled off your charger, full of energy.", 2});
                }
            }
            return;
        }

        if (calib_) { mind_.setBatteryLow(false); return; }  // measuring: he doesn't go home
        int pct = chargePct(b);
        // Trust the firmware's LOW flag even when the voltage looks fine: with a weak cell the resting
        // voltage lies (seen: 4.03 V "75%" off the charger, LOW + red backpack lights, then 3.61 V once
        // charging). It's the FULL flag that is unreliable (false after reboot), not LOW.
        bool low = b.level == 1 || pct <= homeThresholdPct();
        mind_.setBatteryLow(low);
        // Off the charger: go home whenever the battery is low or safe-mode is on (safe-mode means "stay on
        // the charger"). Otherwise he may roam.
        if (!low && !brownout_safe_) {
            charger_attempts_ = 0;
            return;
        }
        // Keep trying to redock for as long as the battery is low - never give up. The drive-home runs
        // in the background so the mind keeps listening and talking meanwhile (it used to block the
        // thinking loop for minutes). A dead battery off the charger must never happen.
        if (!docking_.exchange(true)) {
            std::printf("\n[self-care] off the charger (battery ~%d%%, %.2f V%s), heading home (attempt %d)\n",
                        pct, b.volts, brownout_safe_ ? ", safe-mode" : "", charger_attempts_ + 1);
            mind_.feel("energy", -0.2f);
            ++charger_attempts_;
            // The native go-home behavior finds the charger by itself; it only runs while we
            // don't hold behavior control.
            if (control_) control_->release();
            std::thread([this, now] {
                vec_.goHome();
                for (int i = 0; i < 40 && !g_quit; ++i) {  // up to ~2 minutes
                    std::this_thread::sleep_for(std::chrono::seconds(3));
                    VectorClient::Battery after;
                    if (readBattery(after) && after.on_charger) {
                        std::printf("[self-care] made it onto the charger\n");
                        mind_.remember("My battery got low and I drove back to my charger by myself.", 3, {});
                        // A full discharge (rolled off ~full, came back low) just completed: log how
                        // long it lasted so we can tell when the pack is wearing out.
                        if (discharge_start_.time_since_epoch().count() != 0) {
                            double mins = std::chrono::duration<double>(now - discharge_start_).count() / 60.0;
                            recordDischargeCycle(mins, discharge_start_v_, after.volts);
                            discharge_start_ = {};
                        }
                        break;
                    }
                }
                docking_ = false;
            }).detach();
        }
        // Still not docked: ask for help too (the mind phrases it in character), but keep
        // retrying the drive-home on every future check regardless.
        if (charger_attempts_ >= 2 && now - last_help_request_ > std::chrono::minutes(5)) {
            last_help_request_ = now;
            push({Stimulus::Body,
                  "Your battery is very low and you couldn't find your charger. Ask someone to put "
                  "you on it, briefly and in character.",
                  4});
        }
    }

    // ============================================================ reflection ===
    void reflect() {
        auto pending = mind_.since(mind_.lastReflectedId());
        if (pending.empty()) return;
        // Cap the batch: a large backlog in one prompt can overflow the local model's
        // context / GPU memory and crash the ollama runner (observed: 65 memories on
        // qwen2.5vl:7b -> runner SIGSEGV, HTTP 500). A crash leaves the batch unmarked,
        // so it just keeps growing and crashing. Consolidate oldest-first in small
        // chunks; shouldReflect() stays true while a backlog remains, so the rest get
        // picked up on following quiet ticks.
        const size_t kMaxReflectBatch = 50;  // two sessions a day: cover a day's worth
        const size_t total_pending = pending.size();
        if (pending.size() > kMaxReflectBatch) pending.resize(kMaxReflectBatch);
        if (total_pending > pending.size())
            std::printf("\n[reflect] consolidating %zu of %zu memories (batched)...\n",
                        pending.size(), total_pending);
        else
            std::printf("\n[reflect] consolidating %zu memories...\n", pending.size());
        std::string user = "It is quiet now, like drifting off to sleep. Reflect on your recent "
                           "experiences.\n\nRecent memories:\n";
        std::vector<std::string> people;
        for (auto& m : pending) {
            user += "- " + m.text + " [importance " + std::to_string(m.importance) + "; felt: " +
                    m.feeling + "]\n";
            for (auto& p : m.people)
                if (std::find(people.begin(), people.end(), p) == people.end()) people.push_back(p);
        }
        if (std::string fs = mind_.fearsSummary(); !fs.empty()) user += "\nYour fears right now: " + fs + "\n";
        auto ppl = mind_.describePeople(people);
        if (!ppl.empty()) user += "\nPeople involved:\n" + ppl;
        bool asleep = calm_.load();
        std::deque<std::string> voices;
        { std::lock_guard<std::mutex> lk(dream_mu_); voices = dream_frags_; }
        if (asleep && !voices.empty()) {
            user += "\nVoices you half-heard while dozing:";
            for (auto& v : voices) user += " \"" + v + "\"";
            user += "\n";
        }
        user += asleep ? "\nYou are asleep right now. 'dream' = your dream: 2-3 sentences, a vivid, gently surreal "
                         "first-person dream scene woven from these memories and any half-heard voices (people you "
                         "know may appear; keep it kind).\n"
                       : "\n'dream' = \"\" (you are awake).\n";
        user += "\nReturn: 'insights' = a few higher-level memories worth keeping (patterns, what you "
                "learned about people and yourself; importance 1-10); 'people' = only LASTING facts about that person "
                "(personality, preferences, history, relationships - never what they wore or did today, never things "
                "about someone else or yourself, never what is already known like birthdays; 'lasting' true) and affinity "
                "changes; 'narrative' = your updated life story in first person, under 120 words, honest rather "
                "than flattering (include friction, disagreements and what puzzled or annoyed you, not only praise), "
                "keeping what still matters from the old one; 'fear_changes' = how sleep reshaped your fears, usually [] "
                "(soften a fear that safe experiences contradicted, deepen one that keeps being confirmed; -0.15..0.15; "
                "may also name a new fear that clearly grew from these memories); 'trait_changes' = tiny personality "
                "shifts (-0.03..0.03, mostly 0) that your experiences justify.";

        json r;
        std::string err;
        LlmBackend* mind = reflect_ ? reflect_.get() : (fast_ && !opt_.reflect_with_main) ? fast_.get() : llm_.get();
        setCallReason("reflection on " + std::to_string(pending.size()) + " memories");
        if (!mind->complete(systemPrompt(), {{"user", user, ""}}, reflectionSchema(), r, &err)) {
            std::fprintf(stderr, "[reflect] failed: %s\n", err.c_str());
            return;
        }
        for (auto& m : r.value("insights", json::array())) {
            mind_.remember(m.value("text", ""), m.value("importance", 5),
                           m.value("people", std::vector<std::string>{}), "reflection");
            std::printf("[insight] %s\n", m.value("text", "").c_str());
        }
        for (auto& p : r.value("people", json::array())) {
            if (p.value("lasting", false)) mind_.learnFact(p.value("name", ""), p.value("fact", ""));
            mind_.adjustAffinity(p.value("name", ""), p.value("affinity_change", 0.f));
        }
        mind_.setNarrative(r.value("narrative", ""));
        std::string dream = r.value("dream", "");
        if (asleep) {
            flushDreams();  // the half-heard voices themselves are kept as a dream memory too
            if (!dream.empty()) {
                mind_.remember("I dreamed: " + dream, 3, {}, "dream");
                { std::lock_guard<std::mutex> lk(dream_mu_); last_dream_ = dream; last_dream_at_ = std::time(nullptr); }
                dreamed_this_sleep_ = true;
                std::printf("[dream] %s\n", dream.c_str());
            }
        }
        mind_.nudgeTraits(r.value("trait_changes", json::object()));
        for (auto& f : r.value("fear_changes", json::array()))
            if (float ch = std::clamp(f.value("change", 0.f), -0.15f, 0.15f); ch != 0.f && !f.value("what", "").empty()) {
                mind_.feelFear(f.value("what", ""), ch, "grew out of my memories while I slept");
                std::printf("[fear] (sleep) %s %s\n", ch > 0 ? "deepened:" : "eased:", f.value("what", "").c_str());
            }
        mind_.markReflected(pending.back().id);
        last_consolidation_ = int64_t(std::time(nullptr));
        std::ofstream(mind_dir_ + "/consolidation_at") << last_consolidation_ << "\n";
        std::printf("[reflect] (%s%s) new story: %s\n", mind->lastUsed().c_str(),
                    mind->lastWasPaid() ? "" : ", free", r.value("narrative", "").c_str());
    }

    VectorClient& vec_;
    BrainOptions opt_;
    std::unique_ptr<LlmBackend> llm_;
    std::unique_ptr<LlmBackend> fast_;  // cheap model for background turns (may be null)
    std::unique_ptr<LlmBackend> reflect_;  // memory consolidation + dreams (--reflect-model; may be null)
public:
    void setReflectMind(std::unique_ptr<LlmBackend> m) { reflect_ = std::move(m); }
private:
    // Extra minds created on demand (haiku / sonnet / opus / local) for escalation and
    // owner-requested switches.
    std::map<std::string, std::unique_ptr<LlmBackend>> tiers_;
    std::string convo_override_;          // "" = use llm_ (default)
    Clock::time_point convo_override_until_{};
    bool claude_ok_ = false;
    std::string mind_dir_;
    Mind mind_;
    std::unique_ptr<Ears> ears_;
    std::unique_ptr<ControlSession> control_;
    std::vector<std::string> anims_;
    std::mt19937 rng_;

    std::mutex mu_;
    std::condition_variable cv_;
    std::deque<Stimulus> q_;
    std::string frame_;
    Clock::time_point frame_time_;
    std::map<std::string, Clock::time_point> faces_;
    std::map<std::string, Clock::time_point> greeted_;
    std::string last_expression_;
    std::deque<std::string> overheard_;
    std::string partner_;  // who we're talking with (main thread)
    std::atomic<Clock::time_point> convo_until_{Clock::time_point{}};

    // robot-state edge detection (event thread only)
    bool touched_ = false, picked_ = false;
    Clock::time_point last_cam_repair_{};                // camera self-repair (rate limit)
    int cam_repairs_ = 0, wait_extra_ = 0;               // repairs since the camera last worked
    std::atomic<bool> calm_{false};                     // body is dozing (calm power mode)
    std::atomic<Clock::time_point> wake_override_until_{Clock::time_point{}};
    vpb::ControlRequest::Priority control_prio_ = vpb::ControlRequest::DEFAULT;
    std::map<std::string, std::string> edit_errors_;    // hand-edited file -> where it's broken (main thread)
    std::deque<std::string> cues_;                      // spoken cues waiting to be said (main thread)
    Clock::time_point last_cue_{};
    int64_t character_mtime_ = -1;
    bool face_landmarks_ok_ = false, face_frontal_ = false;   // last face: landmarks seen / facing me (mu_)
    Clock::time_point last_frontal_log_{};
    std::map<std::string, bool> photo_wanted_;                // guarded by mu_
    std::map<std::string, Clock::time_point> photo_tried_, photo_asked_;  // guarded by mu_
    std::atomic<bool> photo_busy_{false};
    std::string current_stimulus_;      // text of the stimulus being answered (name rule exception)
    std::map<int, std::string> face_names_;      // robot face-tracking id -> recognised name (guarded by mu_)
    std::string last_recognised_;                // guarded by mu_
    Clock::time_point last_recognised_at_{};
    std::array<float, 2> last_recognised_c_{};   // where that face was (robot 640x360 space)
    Clock::time_point first_unknown_at_{}, last_unknown_at_{};  // last time he addressed someone by name
    std::string blocked_actions_;                       // actions the guard refused (told to the mind next turn)
    std::array<float, 4> face_rect_{};                  // last face seen: x, y, w, h (robot 640x360 space)
    Clock::time_point face_rect_time_{};
    std::mutex dream_mu_;
    std::deque<std::string> dream_frags_;
    std::string last_dream_;                            // guarded by dream_mu_
    int64_t last_dream_at_ = 0;                         // guarded by dream_mu_
    std::atomic<bool> dreamed_this_sleep_{false};
    int64_t last_consolidation_ = 0;                    // epoch of the last consolidation (consolidation_at)
    std::atomic<int64_t> asleep_since_{0};
    Clock::time_point last_status_write_{};
    std::atomic<bool> on_charger_{false};
    Clock::time_point touch_start_, last_pet_note_;
    std::atomic<Clock::time_point> last_cliff_{Clock::time_point{}};
    Clock::time_point last_idle_speech_;
    Clock::time_point last_battery_check_, last_help_request_, last_charger_leave_;
    Clock::time_point last_presence_{};  // last time a real person was seen/heard/felt
    int charger_attempts_ = 0;
    // battery health: duration (minutes) of recent full discharges; shrinks as the pack wears out
    std::deque<double> cycles_min_;
    Clock::time_point discharge_start_{};
    float discharge_start_v_ = 0;
    Clock::time_point last_health_warn_{};
    std::string last_boot_id_;              // robot boot timestamp; changes when it reboots
    std::deque<Clock::time_point> brownouts_;  // UVLO resets in the last 24h
    Clock::time_point last_hw_query_{};
    std::atomic<bool> calib_{false};           // battery calibration run in progress
    bool calib_prior_safe_ = false;
    bool calib_again_ = false;  // run another calibration from the next full charge
    int calib_auto_runs_ = 0;
    Clock::time_point last_calib_sample_{};
    std::atomic<bool> brownout_safe_{false};   // after a brownout: conserve power until serviced
    std::atomic<bool> docking_{false};         // a background drive-home attempt is running
    bool last_ears_connected_ = false;      // to log only mic connect/disconnect transitions
    Clock::time_point last_level_beat_{};   // 5-minute ears heartbeat in the log
    std::atomic<bool> robot_up_{true}, robot_returned_{false};
    std::atomic<bool> look_requested_{false};
    Clock::time_point last_spoke_with_{};
    Clock::time_point last_check_{};
    std::atomic<bool> checking_{false}, want_glance_{false};              // last reply to someone's words
    std::atomic<int> seen_n_{int(std::time(nullptr) % 10)};
    std::atomic<bool> careful_requested_{false};       // next think: a careful online look
    std::string careful_what_;                          // what the mind wanted to look for
    Clock::time_point last_careful_{};
    std::unique_ptr<LlmBackend> detail_eyes_;           // online vision model for careful looks (lazy)
    int self_turns_ = 0;                 // consecutive robot-generated turns (loop guard)
    Clock::time_point last_look_{};      // cooldown for the look action
    std::string last_said_;              // suppress near-duplicate speech
    Clock::time_point last_said_at_{};
    std::string firmware_;
    bool go_home_after_ = false;
    // ---- where voices come from (doa.hpp) and where faces are. Bearings are "world" degrees: his own heading
    // plus the angle seen, so they stay put while he turns. + = left.
    DirectionFinder dir_;
    DirectionModel dir_model_;
    std::mutex seen_mu_;
    std::map<int, SeenFace> seen_faces_;  // by the robot's face id
    std::atomic<float> robot_yaw_{0};
    std::atomic<float> cam_fx_{0}, cam_cx_{320};
    Clock::time_point last_cam_cfg_try_{};
    SpeechDir speech_dir_;  // best direction of the speech being collected (speech_mu_)
    std::map<std::string, std::pair<float, Clock::time_point>> speaker_place_;  // who spoke from where (voice_mu_)
    std::atomic<float> turn_to_{0};
    std::atomic<bool> turn_pending_{false};
    Clock::time_point last_turn_{};
    int dir_unsaved_ = 0;
    // ---- talking: interruptible (his back button), and a reply can start before it's fully written
    std::atomic<bool> speaking_{false}, interrupted_{false};
    std::string streamed_said_;  // the part of the current reply already spoken (main thread)
    std::string last_flush_raw_;  // what they said, as handed to the mind (to re-queue if they keep talking)
    bool stream_body_ = false;
    bool button_ = false;
    // ---- life between conversations
    Clock::time_point last_stranger_invite_{};
    std::string last_sight_;                      // what he last kept a note of seeing
    Clock::time_point last_sight_at_{};
    std::atomic<bool> ambient_busy_{false};
    Clock::time_point last_ambient_{};
    Clock::time_point last_novelty_{};
    json places_ = json::object();                // what he usually sees from each spot (ambient.json)
    std::mutex ambient_mu_;
    std::string convo_person_;                    // the conversation in progress, for its recap
    int convo_turns_ = 0;
    Clock::time_point convo_last_{};
    // ---- hush: "be quiet" / "go to sleep" = about 2 hours of quiet: he goes to his charger and sleeps (the
    // body's own sleep), says nothing, moves nothing. Ends early only when someone deals with him directly
    // (calls him by name, pets him, types to him). Persisted, so a restart doesn't wake him.
    std::atomic<int64_t> hush_until_{0};
    std::atomic<bool> hush_sleep_pending_{false};
    std::atomic<Clock::time_point> hush_grace_until_{Clock::time_point{}};  // his one soft "okay" first
    Clock::time_point last_hush_try_{};
    bool hush_was_on_charger_ = true;
    bool play_cube_after_ = false;
    std::string learn_face_after_;
    std::string photo_after_;           // take a picture of this person after speaking (countdown + shutter)
    std::atomic<bool> cube_connected_{false};
    Clock::time_point last_cube_try_{}, last_cube_note_{}, cube_seen_at_{}, cube_moved_at_{};
    int last_off_charger_pct_ = -1;
    Clock::time_point last_image_sent_;
    Clock::time_point last_level_log_;
};

}  // namespace

// Main mind + fast mind. With Claude as the main mind, the fast mind defaults to the
// free local model (Ollama) so only conversation costs money.
static bool makeMinds(const VectorClient& client, const BrainOptions& opt,
                      std::unique_ptr<LlmBackend>& main, std::unique_ptr<LlmBackend>& fast) {
    std::string dir = opt.data_dir.empty() ? dataDirFor(client.config().serial) : opt.data_dir;
    std::filesystem::create_directories(dir);
    setUsageFile(dir + "/usage.json");
    setDailyBudget(opt.daily_budget);
    std::string err;
    main = makeBackend(opt.backend, opt.model, opt.effort, &err);
    if (!main) {
        std::fprintf(stderr, "[brain] %s\n", err.c_str());
        return false;
    }
    if (opt.fast_model.rfind("claude", 0) == 0) {
        fast = makeBackend("claude", opt.fast_model, "low", &err);
    } else if (wantsClaude(opt.backend)) {
        const char* local = std::getenv("MBOT_LOCAL_MIND");
        fast = makeOpenAIBackend(local ? local : "http://localhost:11434/v1",
                                 opt.fast_model.empty() ? defaultLocalModel() : opt.fast_model);
    } else if (!opt.fast_model.empty() && opt.fast_model != opt.model) {
        fast = makeBackend(opt.backend, opt.fast_model, "low", &err);
    }
    if (wantsClaude(opt.backend)) {
        auto u = usageToday();
        std::printf("[cost] Claude so far today: $%.3f in %d calls", u.usd, u.calls);
        if (opt.daily_budget > 0) std::printf(" (daily budget $%.3g)", opt.daily_budget);
        std::printf("\n");
    }
    return true;
}

int runReflection(VectorClient& client, const BrainOptions& opt) {
    std::unique_ptr<LlmBackend> main, fast;
    if (!makeMinds(client, opt, main, fast)) return 1;
    Brain b(client, opt, std::move(main), std::move(fast));
    if (!opt.reflect_model.empty()) b.setReflectMind(makeReflectionBackend(opt.reflect_model));
    return b.reflectOnce();
}

// Mirror everything printed to the terminal into <mind dir>/brain.log.
static void teeOutputTo(const std::string& path) {
    setColorOutput(isatty(STDOUT_FILENO) != 0);
    int orig = dup(STDOUT_FILENO);
    int fds[2];
    if (orig < 0 || pipe(fds) != 0) return;
    FILE* log = std::fopen(path.c_str(), "a");
    if (!log) return;
    std::fflush(stdout);
    std::fflush(stderr);
    dup2(fds[1], STDOUT_FILENO);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    setvbuf(stdout, nullptr, _IOLBF, 0);
    std::thread([orig, rd = fds[0], log] {
        char buf[4096];
        ssize_t n;
        while ((n = read(rd, buf, sizeof buf)) > 0) {
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(orig, buf + off, size_t(n - off));
                if (w <= 0) break;
                off += w;
            }
            // Log file gets plain text (no ANSI colour codes).
            std::string plain;
            plain.reserve(size_t(n));
            for (ssize_t i = 0; i < n; ++i) {
                if (buf[i] == '\033') {
                    while (i < n && buf[i] != 'm') ++i;
                    continue;
                }
                plain += buf[i];
            }
            std::fwrite(plain.data(), 1, plain.size(), log);
            std::fflush(log);
        }
    }).detach();
    std::time_t t = std::time(nullptr);
    std::printf("\n===== brain started %s", std::ctime(&t));
}

// Settings changed in maintenance mode (/model, /reflect-model, /budget, ...) persist in
// <data>/settings.json and override the command-line flags.
static BrainOptions withLocalSettings(const BrainOptions& base, const std::string& dir) {
    BrainOptions o = base;
    try {
        auto t = readFile(dir + "/settings.json");
        if (t.empty()) return o;
        json s = json::parse(t);
        if (s.contains("model")) o.model = s["model"].get<std::string>();
        if (s.contains("fast-model")) o.fast_model = s["fast-model"].get<std::string>();
        if (s.contains("reflect-model")) o.reflect_model = s["reflect-model"].get<std::string>();
        if (s.contains("budget")) o.daily_budget = s["budget"].get<double>();
        if (s.contains("listen-all")) o.listen_all = s["listen-all"].get<bool>();
        if (s.contains("recognition")) o.recognition = s["recognition"].get<bool>();
        if (s.contains("debug-media")) o.save_eyes = o.save_ears = s["debug-media"].get<bool>();
        std::printf("[brain] local settings applied from settings.json\n");
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[brain] settings.json unreadable (%s) - using the flags\n", e.what());
    }
    return o;
}

int runBrain(VectorClient& client, const BrainOptions& opt_in) {
    const BrainOptions opt = withLocalSettings(opt_in, opt_in.data_dir.empty() ? dataDirFor(client.config().serial) : opt_in.data_dir);
    std::string dir = opt.data_dir.empty() ? dataDirFor(client.config().serial) : opt.data_dir;
    std::filesystem::create_directories(dir);
    teeOutputTo(dir + "/brain.log");
    std::unique_ptr<LlmBackend> main, fast;
    if (!makeMinds(client, opt, main, fast)) return 1;
    Brain b(client, opt, std::move(main), std::move(fast));
    if (!opt.reflect_model.empty()) b.setReflectMind(makeReflectionBackend(opt.reflect_model));
    return b.run();
}

}  // namespace mbot
