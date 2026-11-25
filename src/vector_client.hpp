#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include <grpcpp/grpcpp.h>

#include "anki_vector/messaging/external_interface.grpc.pb.h"
#include "config.hpp"

namespace mbot {

namespace vpb = Anki::Vector::external_interface;

struct Image {
    std::string data;  // JPEG bytes when encoding is JPEG_COLOR / JPEG_GRAY
    uint32_t id = 0;
    uint32_t timestamp = 0;
    int encoding = 0;  // vpb::ImageChunk::ImageEncoding
    bool isJpeg() const;
};

// Thin, synchronous wrapper around Vector's on-robot gRPC server (port 443).
// Everything here talks to the robot directly over your LAN — no cloud.
class VectorClient {
public:
    explicit VectorClient(const RobotConfig& cfg);
    ~VectorClient();

    bool waitReady(int timeout_sec, std::string* err = nullptr);
    bool checkProtocol(std::string* err = nullptr);
    std::unique_ptr<grpc::ClientContext> ctx(int timeout_sec = 15) const;
    vpb::ExternalInterface::Stub& stub() { return *stub_; }

    // --- one-shot commands (most need behavior control, see ControlSession) ---
    bool say(const std::string& text, float duration_scalar = 1.0f, float pitch = 0.0f);
    // Stop whatever say() is doing right now (someone pressed his back button to interrupt him).
    void cancelSay();
    // Camera intrinsics (pixels) - used to turn a face's position in the image into a bearing.
    bool cameraConfig(float& focal_x, float& center_x, float& fov_x_deg);
    bool setHeadDeg(float deg);         // -22 .. 45
    bool setLift(float fraction);       // 0 (down) .. 1 (up)
    bool turnDeg(float deg);            // + = left
    bool driveMm(float mm, float speed_mmps = 80.f);
    bool driveWheels(float left_mmps, float right_mmps);
    bool stopMotors();
    bool setEyeColor(float hue, float saturation);  // 0..1 each
    bool playAnimTrigger(const std::string& trigger);
    bool listAnimTriggers(std::vector<std::string>& out);
    bool captureImage(Image& out, bool high_res = false, int timeout_sec = 20);
    // Native behaviors (need behavior control; they block until done).
    bool enrollFace(const std::string& name);   // learn the face in view
    // Rename a face the robot has enrolled (names compared case/punctuation-insensitively).
    // Returns false if no enrolled face has that name.
    bool renameEnrolledFace(const std::string& old_name, const std::string& new_name);
    // Faces the robot has enrolled: (face_id, name as stored).
    std::vector<std::pair<int, std::string>> enrolledFaces();
    bool findFaces();
    bool enableFaceDetection(bool on);
    bool lookAround();
    bool driveOnCharger();      // SDK behavior; hangs on this firmware (needs SDK vision)
    // Trigger a native voice-command behavior, e.g. "intent_system_charger" (go home).
    // Runs only while nobody holds behavior control.
    bool appIntent(const std::string& intent, const std::string& param = "");
    bool goHome() { return appIntent("intent_system_charger"); }
    // Light Cube (Bluetooth, connected by the robot).
    bool connectCube(std::string* factory_id = nullptr);
    bool flashCube();
    bool driveOffCharger();
    bool enableImageStreaming(bool on);
    bool batteryInfo(std::string& out);
    struct Battery {
        int level = 0; float volts = 0; bool charging = false, on_charger = false;
        bool cube_known = false; int cube_level = -1; float cube_volts = 0;  // cube: 0 low, 1 normal
        // Filled in by the brain from the robot's live state (the mic shim): volts is then the live reading.
        bool live = false; float sdk_volts = 0, temp_c = -1;
        float raw_volts = 0;  // off the charger: the latest reading before the brain holds the lowest one
    };
    bool battery(Battery& out);  // level: 1 low, 2 nominal, 3 full
    bool versionInfo(std::string& out);
    // 16-bit mono PCM, 8000..16025 Hz. Plays through Vector's speaker.
    bool playPcm16(const std::vector<int16_t>& samples, uint32_t rate, uint32_t volume = 75);

    // Exchange a session token for a client GUID (needs the robot's token server:
    // wire-pod / escape pod / cloud). Works with an empty GUID in the config.
    bool userAuthentication(const std::string& session_token, const std::string& client_name,
                            std::string& guid_out, std::string* err = nullptr);

    // --- streams; block until cb returns false, stopStreams() or error ---
    void cameraFeed(const std::function<bool(const Image&)>& cb);
    void events(const std::function<bool(const vpb::Event&)>& cb);
    void stopStreams();

    const RobotConfig& config() const { return cfg_; }

private:
    void track(grpc::ClientContext* c);
    void untrack(grpc::ClientContext* c);

    RobotConfig cfg_;
    std::shared_ptr<grpc::Channel> channel_;
    std::unique_ptr<vpb::ExternalInterface::Stub> stub_;
    std::mutex say_mu_;
    grpc::ClientContext* say_ctx_ = nullptr;
    std::mutex streams_mu_;
    std::set<grpc::ClientContext*> streams_;
};

// Holds behavior control for its lifetime. While held, Vector's own behaviors
// (wandering, reacting) are suppressed and our commands drive the body.
// Releasing it lets Vector's native personality take over again, which is a
// nice way to keep him "alive" between agent actions.
class ControlSession {
public:
    explicit ControlSession(VectorClient& client,
                            vpb::ControlRequest::Priority prio = vpb::ControlRequest::DEFAULT);
    ~ControlSession();
    bool acquire(int timeout_ms = 10000);
    bool has() const { return granted_; }
    void release();

private:
    void readLoop();

    VectorClient& client_;
    vpb::ControlRequest::Priority prio_;
    std::unique_ptr<grpc::ClientContext> ctx_;
    std::unique_ptr<grpc::ClientReaderWriter<vpb::BehaviorControlRequest,
                                             vpb::BehaviorControlResponse>> stream_;
    std::thread reader_;
    std::mutex mu_;
    std::condition_variable cv_;
    std::atomic<bool> granted_{false};
    std::atomic<bool> closed_{false};
};

}  // namespace mbot
