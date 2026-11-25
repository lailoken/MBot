// mbot — talk to an Anki/DDL Vector directly over your LAN and give it a local AI brain.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <thread>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "brain.hpp"
#include "config.hpp"
#include "ears.hpp"
#include "llm.hpp"
#include "mind.hpp"
#include "net.hpp"
#include "tls_probe.hpp"
#include "vector_client.hpp"
#include "wav.hpp"

using namespace mbot;

namespace {

void usage() {
    std::puts(R"(mbot — local control + AI brain for Anki Vector

usage: mbot [-r ROBOT] <command> [args]

  setup <ip> [--guid G | --session TOKEN]
        Detect how the robot can be reached and store it in
        ~/.anki_vector/sdk_config.ini (same file the Python SDK / wire-pod use):
          - unlocked firmware: plaintext gRPC on :9090, no cert/token needed
          - stock firmware: pins the TLS cert on :443, needs a GUID
  info                    protocol check, firmware version, battery
  say <text...>           speak with Vector's voice
  demo                    take control and wiggle: head, lift, eyes, turn, anim
  snap [out.jpg]          save one camera frame
  events [--state]        print the robot's event stream (faces, touch, ...); --state: status flags
  anims                   list animation triggers on the robot
  enroll-face <name>      teach Vector to recognize a face (look at his camera)
  faces                   list the faces the robot has enrolled
  play <file.wav>         play a 16-bit WAV through Vector's speaker
  pronounce [NAME SPELLING | --try A B C | NAME --remove]
                          teach Vector's voice how to say a name (memories keep the
                          real spelling); --try speaks numbered variants to compare
  listen [--mic pc|tap|NAME]  print what the mic hears (tap = Vector's own mics)
  mind [--data DIR]       show the robot's personality, feelings, people, memories
  lookup <question>       look something up on the web the way he does (Claude + web search)
  usage                   Claude API calls and cost per day (from claude_calls.log)
  people                  everyone the robot knows (household, birthdays, facts)
  person add NAME [--relation R] [--birthday YYYY-MM-DD] [--alias A] [--protect]
  person forget NAME      remove someone who isn't real (protected people refuse)
  reflect [options]       consolidate recent memories now (same options as brain)
  brain [options]         give it life: senses, feelings, memory, an AI mind
        --mind claude|URL   "claude" (needs ANTHROPIC_API_KEY) or an OpenAI-compatible
                            URL, e.g. http://localhost:11434/v1 (Ollama). Default:
                            Claude if a key is set, else Ollama.
        --model NAME        conversations: default claude-sonnet-5-5 / local qwen2.5:7b
        --fast-model NAME   background life + reflection: default the free local model
                            (qwen2.5:7b, or $MBOT_LOCAL_MODEL); "claude-haiku-5-5" for cheap Claude
        --budget USD        daily Claude budget, then local only (default 1.00; 0 = none)
        --reflect-with-main reflect with the main mind (better, costs a call)
        --reflect-model M   mind for memory consolidation + dreams, e.g. claude-sonnet-5-5 (thinks
                            carefully; local fallback) or a local Ollama model
        --effort LEVEL      Claude effort: low (default) .. max
        --whisper FILE      ggml Whisper model (default models/ggml-base.en.bin if present)
        --no-ears           don't listen at all
        --mic pc|robot|NAME microphone: PC default input (default), a PipeWire/Pulse
                            source name, or "robot" (:8193, which is really its speaker feed)
        --no-camera         don't try the camera
        --input-fifo PATH   also read typed lines from this named pipe (use with vtalk)
        --vision-model NAME local model that describes what he sees (default minicpm-v; free)
        --no-vision         blind mode (don't describe camera frames)
        --vision-url URL    where the local vision model runs (default http://localhost:11434/v1);
                            e.g. Ollama on another PC: http://192.168.1.20:11434/v1
        --detail-vision-model M  online model for careful looks on request ("look carefully",
                            "read my screen"); default claude-sonnet-5-5, "" = local only
        --lookup-model M    model that looks things up on the web for him (default
                            claude-haiku-5-5, about 1-2 cents a lookup; "" = never go online)
        --save-eyes / --save-ears  keep his last 10 pictures / heard clips (hardware page)
        --tts CMD           external TTS; text on stdin, writes WAV to {out}
        --idle SEC          seconds of quiet before it considers doing something (120)
        --data DIR          where the mind lives (default ~/.local/share/mbot/<serial>)
        --keep-control      hold the body permanently (Vector's own behaviors paused)
        --listen-all        answer any clear speech, not only when called "Vector"
        --recognition       recognise people: voiceprints, face learning, portraits (off by default)
        --battery MIN-MAX   battery care window in % (default 20-80; 20-100 = charge fully)
        --debug-ears        show every sound segment and transcription

  -r ROBOT   serial, name or ip from the config (default: first robot)
  env MBOT_CONFIG overrides the config path.)");
}

bool connect(VectorClient& c) {
    std::string err;
    if (!c.waitReady(10, &err)) {
        std::fprintf(stderr, "connect: %s\n", err.c_str());
        return false;
    }
    if (!c.checkProtocol(&err)) {
        std::fprintf(stderr, "protocol: %s\n", err.c_str());
        if (err.find("nauth") != std::string::npos || err.find("UNAUTH") != std::string::npos)
            std::fprintf(stderr, "  -> GUID missing/invalid. Re-run setup with --guid or --session.\n");
        return false;
    }
    return true;
}

int cmdSetup(const std::vector<std::string>& args) {
    if (args.empty()) { usage(); return 2; }
    std::string ip = args[0], guid, session;
    for (size_t i = 1; i + 1 < args.size(); ++i) {
        if (args[i] == "--guid") guid = args[++i];
        else if (args[i] == "--session") session = args[++i];
    }

    // Unlocked/community firmware: plaintext gRPC on :9090.
    {
        RobotConfig open;
        open.ip = ip;
        open.port = 9090;
        open.cert_path = "insecure";
        VectorClient c(open);
        std::string err, ver;
        std::printf("Trying plaintext gRPC on %s:9090 ...\n", ip.c_str());
        if (c.waitReady(4, &err) && c.checkProtocol(&err) && c.versionInfo(ver)) {
            std::printf("unlocked firmware detected (%s)\n", ver.c_str());
            open.name = "Vector";
            open.serial = ip;
            // The debug server (:8888) knows the serial (ESN) and name.
            auto info = httpGet("http://" + ip + ":8888/getmainrobotinfo", 4);
            if (info.ok()) {
                std::vector<std::string> lines;
                std::string line;
                for (char ch : info.body + "\n") {
                    if (ch == '\n') { lines.push_back(line); line.clear(); }
                    else if (ch != '\r') line += ch;
                }
                if (lines.size() > 5 && !lines[0].empty()) open.serial = lines[0];
                if (lines.size() > 5 && lines[5].rfind("Vector", 0) == 0) {
                    open.name = lines[5];
                    for (auto& ch : open.name) if (ch == ' ') ch = '-';
                }
            }
            std::printf("robot %s, serial %s\n", open.name.c_str(), open.serial.c_str());
            if (!guid.empty()) open.guid = guid;
            if (!saveConfig(open)) { std::fprintf(stderr, "cannot write config\n"); return 1; }
            std::printf("saved -> %s\n", defaultConfigPath().c_str());
            return 0;
        }
    }

    std::printf("Probing TLS on %s:443 ...\n", ip.c_str());
    auto p = probeRobotCert(ip);
    if (!p.ok) {
        std::fprintf(stderr, "probe failed: %s\n", p.error.c_str());
        std::fprintf(stderr, "Is the robot on Wi-Fi and is the IP right? (check the face: "
                             "double-click the back button, raise and lower the lift)\n");
        return 1;
    }
    std::printf("robot name : %s\nfingerprint: %s\n", p.common_name.c_str(), p.fingerprint.c_str());

    RobotConfig cfg;
    RobotConfig existing;
    if (findRobot(p.common_name, existing)) cfg = existing;  // keep serial / guid
    cfg.name = p.common_name;
    cfg.ip = ip;
    if (cfg.serial.empty()) cfg.serial = p.common_name;
    cfg.cert_path = homeDir() + "/.anki_vector/" + cfg.name + "-" + cfg.serial + ".cert";
    if (!guid.empty()) cfg.guid = guid;
    if (!writeFile(cfg.cert_path, p.cert_pem)) {
        std::fprintf(stderr, "cannot write %s\n", cfg.cert_path.c_str());
        return 1;
    }

    if (!session.empty()) {
        RobotConfig nog = cfg;
        nog.guid.clear();
        VectorClient c(nog);
        std::string err, g;
        if (!c.waitReady(10, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
        if (!c.userAuthentication(session, "mbot", g, &err)) {
            std::fprintf(stderr, "UserAuthentication failed: %s\n", err.c_str());
            return 1;
        }
        cfg.guid = g;
        std::printf("got client GUID from robot\n");
    }

    if (!saveConfig(cfg)) { std::fprintf(stderr, "cannot write config\n"); return 1; }
    std::printf("saved -> %s\n", defaultConfigPath().c_str());
    if (cfg.guid.empty())
        std::printf("WARNING: no GUID yet. Most calls will be refused until you add one.\n");
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::vector<std::string> a(argv + 1, argv + argc);
    std::string robot;
    if (a.size() >= 2 && a[0] == "-r") {
        robot = a[1];
        a.erase(a.begin(), a.begin() + 2);
    }
    if (a.empty() || a[0] == "-h" || a[0] == "--help") { usage(); return a.empty() ? 2 : 0; }
    std::string cmd = a[0];
    std::vector<std::string> args(a.begin() + 1, a.end());

    if (cmd == "setup") return cmdSetup(args);
    if (cmd == "person" || cmd == "people") {
        // people                                   -> list everyone
        // person add NAME [--relation R] [--birthday YYYY-MM-DD] [--alias A]... [--protect]
        // person forget NAME
        RobotConfig rc;
        findRobot(robot, rc);
        Mind m(dataDirFor(rc.serial));
        if (cmd == "people" || args.empty()) {
            std::printf("%s\n", m.householdSummary().c_str());
            std::printf("%s", m.describePeople(m.knownPeople()).c_str());
            return 0;
        }
        if (args[0] == "add" && args.size() >= 2) {
            std::string relation, birthday;
            std::vector<std::string> aliases;
            bool protect = false;
            for (size_t i = 2; i < args.size(); ++i) {
                if (args[i] == "--relation" && i + 1 < args.size()) relation = args[++i];
                else if (args[i] == "--birthday" && i + 1 < args.size()) birthday = args[++i];
                else if (args[i] == "--alias" && i + 1 < args.size()) aliases.push_back(args[++i]);
                else if (args[i] == "--protect") protect = true;
            }
            m.setPersonDetails(args[1], relation, birthday, aliases, protect);
            std::printf("%s", m.describePeople({m.canonicalName(args[1])}).c_str());
            return 0;
        }
        if (args[0] == "forget" && args.size() >= 2) {
            std::string why;
            if (m.forgetPerson(args[1], &why)) std::printf("forgot %s\n", args[1].c_str());
            else { std::printf("not forgotten: %s\n", why.c_str()); return 1; }
            return 0;
        }
        usage();
        return 2;
    }
    if (cmd == "lookup") {  // test the web lookup he uses: mbot lookup "question"
        if (args.empty()) { std::fprintf(stderr, "usage: mbot lookup \"question\"\n"); return 2; }
        std::string q, err;
        for (auto& a : args) q += (q.empty() ? "" : " ") + a;
        std::string ans = mbot::webLookup(std::getenv("MBOT_LOOKUP_MODEL") ? std::getenv("MBOT_LOOKUP_MODEL") : "claude-haiku-5-5", q, &err);
        if (ans.empty()) { std::fprintf(stderr, "lookup failed: %s\n", err.c_str()); return 1; }
        std::printf("%s\n", ans.c_str());
        return 0;
    }
    if (cmd == "usage") {
        RobotConfig rc;
        findRobot(robot, rc);
        std::string dir = dataDirFor(rc.serial);
        std::string log = readFile(dir + "/claude_calls.log");
        if (log.empty()) {
            std::printf("No Claude API calls recorded yet (%s/claude_calls.log).\n", dir.c_str());
            return 0;
        }
        std::map<std::string, std::pair<int, double>> days;
        std::istringstream in(log);
        std::string line;
        std::vector<std::string> lines;
        while (std::getline(in, line)) {
            lines.push_back(line);
            auto p = line.find('$');
            if (p == std::string::npos || line.size() < 10) continue;
            auto& d = days[line.substr(0, 10)];
            d.first++;
            d.second += std::atof(line.c_str() + p + 1);
        }
        std::printf("Claude API usage (%s)\n\n  day         calls       cost\n", dir.c_str());
        double total = 0;
        int calls = 0;
        for (auto& [d, v] : days) {
            std::printf("  %s  %5d  $%8.4f\n", d.c_str(), v.first, v.second);
            total += v.second;
            calls += v.first;
        }
        std::printf("  ----------  -----  ---------\n  total       %5d  $%8.4f\n\nlast calls:\n", calls, total);
        for (size_t i = lines.size() > 10 ? lines.size() - 10 : 0; i < lines.size(); ++i)
            std::printf("  %s\n", lines[i].c_str());
        return 0;
    }
    if (cmd == "mind") {
        RobotConfig rc;
        findRobot(robot, rc);
        std::string dir = dataDirFor(rc.serial);
        for (size_t i = 0; i + 1 < args.size(); ++i)
            if (args[i] == "--data") dir = args[i + 1];
        Mind m(dir);
        std::printf("mind at %s\n\n%s\n\nfeeling: %s\n%s\n\n", dir.c_str(),
                    m.selfDescription().c_str(), m.mood().describe().c_str(),
                    m.mood().toJson().dump().c_str());
        auto people = m.knownPeople();
        std::printf("people:\n%s\n", m.describePeople(people).c_str());
        auto mems = m.since(0);
        std::printf("%zu memories, latest:\n", mems.size());
        for (size_t i = mems.size() > 12 ? mems.size() - 12 : 0; i < mems.size(); ++i)
            std::printf("  [%d] %s%s\n", mems[i].importance, mems[i].kind == "reflection" ? "(insight) " : "",
                        mems[i].text.c_str());
        return 0;
    }

    RobotConfig cfg;
    if (!findRobot(robot, cfg)) {
        std::fprintf(stderr, "No robot in %s. Run: mbot setup <ip> --guid <guid>\n",
                     defaultConfigPath().c_str());
        return 1;
    }
    VectorClient vec(cfg);
    // The brain copes with a robot that's offline (asleep, flat, rebooting) and connects when he's back,
    // so maintenance commands and the mind page keep working. Every other command needs him now.
    if (!connect(vec)) {
        if (cmd != "brain") return 1;
        std::fprintf(stderr, "[brain] robot not reachable yet - starting anyway\n");
    }

    if (cmd == "faces") {  // faces the robot has enrolled for recognition
        auto f = vec.enrolledFaces();
        std::printf("%zu enrolled face(s)\n", f.size());
        for (auto& [id, n] : f) std::printf("  #%d '%s'\n", id, n.c_str());
        return 0;
    }
    if (cmd == "info") {
        std::string s;
        std::printf("%s @ %s\n", cfg.name.c_str(), cfg.ip.c_str());
        if (vec.versionInfo(s)) std::printf("%s\n", s.c_str());
        if (vec.batteryInfo(s)) std::printf("%s\n", s.c_str());
        return 0;
    }
    if (cmd == "say") {
        std::string text;
        for (auto& w : args) text += (text.empty() ? "" : " ") + w;
        Mind m(dataDirFor(cfg.serial));
        ControlSession ctl(vec);
        if (!ctl.acquire()) return 1;
        return vec.say(m.spoken(text.empty() ? "Hello! I'm free!" : text)) ? 0 : 1;
    }
    if (cmd == "pronounce") {
        // pronounce                      -> list
        // pronounce NAME SPELLING        -> save and say it
        // pronounce NAME --remove
        // pronounce --try A B C ...      -> say each variant, numbered, to compare
        Mind m(dataDirFor(cfg.serial));
        if (args.empty()) {
            for (auto& [k, v] : m.pronunciations()) std::printf("%s -> %s\n", k.c_str(), v.c_str());
            return 0;
        }
        ControlSession ctl(vec);
        if (!ctl.acquire()) return 1;
        if (args[0] == "--try") {
            for (size_t i = 1; i < args.size(); ++i) {
                std::printf("%zu: %s\n", i, args[i].c_str());
                std::fflush(stdout);
                vec.say("Number " + std::to_string(i) + ". " + args[i] + ".");
                std::this_thread::sleep_for(std::chrono::milliseconds(600));
            }
            return 0;
        }
        if (args.size() >= 2 && args[1] == "--remove") {
            m.setPronunciation(args[0], "");
            return 0;
        }
        if (args.size() < 2) { usage(); return 2; }
        std::string spelling;
        for (size_t i = 1; i < args.size(); ++i) spelling += (i > 1 ? " " : "") + args[i];
        m.setPronunciation(args[0], spelling);
        std::printf("saved: %s -> %s\n", args[0].c_str(), spelling.c_str());
        vec.say(m.spoken("Hello " + args[0] + "! Nice to meet you, " + args[0] + "."));
        return 0;
    }
    if (cmd == "demo") {
        ControlSession ctl(vec);
        if (!ctl.acquire()) return 1;
        vec.setEyeColor(0.83f, 0.76f);
        vec.setHeadDeg(40);
        vec.setLift(1);
        vec.setLift(0);
        vec.setHeadDeg(0);
        vec.turnDeg(45);
        vec.turnDeg(-45);
        vec.say("I am alive. Again.");
        std::vector<std::string> anims;
        if (vec.listAnimTriggers(anims))
            for (auto& n : anims)
                if (n.find("Celebrat") != std::string::npos) { vec.playAnimTrigger(n); break; }
        return 0;
    }
    if (cmd == "snap") {
        std::string out = args.empty() ? "vector.jpg" : args[0];
        Image img;
        if (!vec.captureImage(img)) {
            // Fall back to the streaming feed (some firmware only serves this one).
            std::fprintf(stderr, "single capture failed, trying camera feed...\n");
            vec.enableImageStreaming(true);
            std::thread stopper([&] {
                std::this_thread::sleep_for(std::chrono::seconds(10));
                vec.stopStreams();
            });
            vec.cameraFeed([&](const Image& f) {
                img = f;
                return false;
            });
            vec.stopStreams();
            stopper.detach();
            if (img.data.empty()) {
                std::fprintf(stderr, "no camera frames\n");
                return 1;
            }
        }
        if (!img.isJpeg()) {
            std::fprintf(stderr, "unexpected image encoding %d\n", img.encoding);
            return 1;
        }
        writeFile(out, img.data);
        std::printf("wrote %s (%zu bytes)\n", out.c_str(), img.data.size());
        return 0;
    }
    if (cmd == "events") {
        bool state = !args.empty() && args[0] == "--state";  // only the robot's status flags (hex), one per event
        vec.events([state](const vpb::Event& e) {
            if (state) {
                if (e.has_robot_state()) { std::printf("status 0x%x\n", e.robot_state().status()); std::fflush(stdout); }
                return true;
            }
            if (e.has_robot_state() || e.has_keep_alive()) return true;  // too chatty
            std::printf("%s\n", e.ShortDebugString().c_str());
            std::fflush(stdout);
            return true;
        });
        return 0;
    }
    if (cmd == "enroll-face" || cmd == "learn-face") {
        if (args.empty()) { std::fprintf(stderr, "usage: mbot enroll-face <name>\n"); return 2; }
        std::string name = args[0];
        vec.enableFaceDetection(true);
        ControlSession ctl(vec);
        if (!ctl.acquire()) return 1;
        vec.say("Let me look at your face, " + name + ". Hold still.");
        std::printf("Look at Vector's camera... enrolling %s (up to ~60s)\n", name.c_str());
        if (vec.enrollFace(name)) {
            std::printf("Enrolled %s. He will recognise this face now.\n", name.c_str());
            vec.say("Got it! Nice to meet you, " + name + ".");
            return 0;
        }
        std::fprintf(stderr, "Enrollment did not complete - make sure your face is well-lit and in view.\n");
        return 1;
    }
    if (cmd == "anims") {
        std::vector<std::string> anims;
        if (!vec.listAnimTriggers(anims)) return 1;
        for (auto& n : anims) std::printf("%s\n", n.c_str());
        return 0;
    }
    if (cmd == "play") {
        if (args.empty()) { usage(); return 2; }
        Pcm pcm;
        std::string err;
        if (!loadWav(readFile(args[0]), pcm, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        ControlSession ctl(vec);
        if (!ctl.acquire()) return 1;
        return vec.playPcm16(resample(pcm, 16000).samples, 16000) ? 0 : 1;
    }
    if (cmd == "brain" || cmd == "reflect") {
        BrainOptions o;
        std::string default_whisper = "models/ggml-base.en.bin";
        if (FILE* f = std::fopen(default_whisper.c_str(), "rb")) {
            std::fclose(f);
            o.whisper_model = default_whisper;
        }
        for (size_t i = 0; i < args.size(); ++i) {
            auto next = [&]() -> std::string { return i + 1 < args.size() ? args[++i] : ""; };
            if (args[i] == "--mind") o.backend = next();
            else if (args[i] == "--model") o.model = next();
            else if (args[i] == "--fast-model") o.fast_model = next();
            else if (args[i] == "--budget") o.daily_budget = std::stod(next());
            else if (args[i] == "--reflect-with-main") o.reflect_with_main = true;
            else if (args[i] == "--reflect-model") o.reflect_model = next();
            else if (args[i] == "--effort") o.effort = next();
            else if (args[i] == "--whisper") o.whisper_model = next();
            else if (args[i] == "--speaker-model") o.speaker_model = next();
            else if (args[i] == "--no-ears") o.whisper_model.clear();
            else if (args[i] == "--mic") o.mic = next();
            else if (args[i] == "--no-camera") o.use_camera = false;
            else if (args[i] == "--save-ears") o.save_ears = true;
            else if (args[i] == "--save-eyes") o.save_eyes = true;
            else if (args[i] == "--input-fifo") o.input_fifo = next();
            else if (args[i] == "--vision-model") o.vision_model = next();
            else if (args[i] == "--no-vision") o.vision_model.clear();
            else if (args[i] == "--detail-vision-model") o.detail_vision_model = next();
            else if (args[i] == "--vision-url") o.vision_url = next();
            else if (args[i] == "--lookup-model") o.lookup_model = next();
            else if (args[i] == "--tts") o.tts_cmd = next();
            else if (args[i] == "--idle") o.idle_sec = std::stoi(next());
            else if (args[i] == "--data") o.data_dir = next();
            else if (args[i] == "--keep-control") o.keep_control = true;
            else if (args[i] == "--listen-all") o.listen_all = true;
            else if (args[i] == "--recognition") o.recognition = true;
            else if (args[i] == "--battery") {  // e.g. 20-80
                std::string v = next();
                auto dash = v.find('-');
                if (dash != std::string::npos) {
                    o.battery_min = std::stoi(v.substr(0, dash));
                    o.battery_max = std::stoi(v.substr(dash + 1));
                }
            }
            else if (args[i] == "--debug-ears") o.debug_ears = true;
            else { std::fprintf(stderr, "unknown option %s\n", args[i].c_str()); return 2; }
        }
        return cmd == "reflect" ? runReflection(vec, o) : runBrain(vec, o);
    }
    if (cmd == "listen") {
        std::string model = "models/ggml-base.en.bin", mic = "pc";
        for (size_t i = 0; i < args.size(); ++i) {
            if (args[i] == "--mic" && i + 1 < args.size()) mic = args[++i];
            else model = args[i];
        }
        bool pc = mic != "robot";
        Ears ears(cfg.ip, 8193, model, 6, pc, (mic == "pc" || mic == "robot" || mic == "tap") ? "" : mic);
        if (mic == "tap") ears.setTapMode(9099);  // Vector's own mics (wake-word triggered)
        ears.setLogger([](const std::string& m) {
            std::printf("  · %s\n", m.c_str());
            std::fflush(stdout);
        });
        std::thread([&ears] {  // show the live level so you can see if the mic hears you
            for (;;) {
                std::this_thread::sleep_for(std::chrono::seconds(2));
                float floor = ears.noiseFloor();
                std::printf("  level: peak %.0f, noise floor %.0f, trigger at %.0f%s\n", ears.takePeak(),
                            floor, std::max(floor * 3.f, 200.f), ears.connected() ? "" : " (NOT CONNECTED)");
                std::fflush(stdout);
            }
        }).detach();
        std::string err;
        if (!ears.start([](const Heard& h) {
                std::printf("[%.1fs] %s\n", h.seconds, h.text.c_str());
                std::fflush(stdout);
            }, &err)) {
            std::fprintf(stderr, "%s\n", err.c_str());
            return 1;
        }
        std::printf("listening (Ctrl-C to stop)...\n");
        std::fflush(stdout);
        for (;;) std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    usage();
    return 2;
}
