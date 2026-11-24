#include "vector_client.hpp"

#include <cctype>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <sstream>

namespace mbot {

namespace {
constexpr float kDeg2Rad = 3.14159265f / 180.f;

// The engine only accepts SDK action tags in [2000001, 3000000].
int32_t nextActionTag() {
    static std::atomic<int32_t> tag{2000001};
    int32_t t = tag++;
    if (t >= 3000000) tag = 2000001;
    return t;
}

bool rpcOk(const grpc::Status& s, const char* what) {
    if (!s.ok()) {
        std::fprintf(stderr, "[vector] %s failed: %s (%d)\n", what, s.error_message().c_str(),
                     int(s.error_code()));
        return false;
    }
    return true;
}
}  // namespace

bool Image::isJpeg() const {
    return encoding == vpb::ImageChunk::JPEG_COLOR || encoding == vpb::ImageChunk::JPEG_GRAY;
}

VectorClient::VectorClient(const RobotConfig& cfg) : cfg_(cfg) {
    std::shared_ptr<grpc::ChannelCredentials> creds;
    grpc::ChannelArguments args;
    if (cfg_.insecure()) {
        // Unlocked firmware: plaintext HTTP/2 on :443, no cert, auth optional.
        creds = grpc::InsecureChannelCredentials();
    } else {
        grpc::SslCredentialsOptions ssl;
        ssl.pem_root_certs = cfg_.readCert();  // pin the robot's own self-signed cert
        creds = grpc::SslCredentials(ssl);
        if (!cfg_.guid.empty()) {
            // Sent as "authorization: Bearer <guid>" on every call.
            creds = grpc::CompositeChannelCredentials(creds,
                                                      grpc::AccessTokenCredentials(cfg_.guid));
        }
        args.SetSslTargetNameOverride(cfg_.name);  // cert CN is "Vector-XXXX", not the IP
    }
    args.SetInt(GRPC_ARG_KEEPALIVE_TIME_MS, 20000);
    args.SetMaxReceiveMessageSize(16 * 1024 * 1024);
    channel_ = grpc::CreateCustomChannel(cfg_.target(), creds, args);
    stub_ = vpb::ExternalInterface::NewStub(channel_);
}

VectorClient::~VectorClient() { stopStreams(); }

std::unique_ptr<grpc::ClientContext> VectorClient::ctx(int timeout_sec) const {
    auto c = std::make_unique<grpc::ClientContext>();
    if (cfg_.insecure() && !cfg_.guid.empty())
        c->AddMetadata("authorization", "Bearer " + cfg_.guid);
    if (timeout_sec > 0)
        c->set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(timeout_sec));
    return c;
}

bool VectorClient::waitReady(int timeout_sec, std::string* err) {
    auto deadline = std::chrono::system_clock::now() + std::chrono::seconds(timeout_sec);
    if (channel_->WaitForConnected(deadline)) return true;
    if (err) *err = "could not open a TLS/gRPC channel to " + cfg_.target() + " as " + cfg_.name;
    return false;
}

bool VectorClient::checkProtocol(std::string* err) {
    vpb::ProtocolVersionRequest req;
    req.set_client_version(vpb::PROTOCOL_VERSION_CURRENT);
    req.set_min_host_version(vpb::PROTOCOL_VERSION_MINIMUM);
    vpb::ProtocolVersionResponse resp;
    auto c = ctx();
    auto st = stub_->ProtocolVersion(c.get(), req, &resp);
    if (!st.ok()) {
        if (err) *err = st.error_message();
        return false;
    }
    if (resp.result() != vpb::ProtocolVersionResponse::SUCCESS) {
        if (err) *err = "robot protocol version " + std::to_string(resp.host_version()) +
                        " not supported";
        return false;
    }
    return true;
}

bool VectorClient::say(const std::string& text, float duration_scalar, float pitch) {
    vpb::SayTextRequest req;
    req.set_text(text);
    req.set_use_vector_voice(true);
    req.set_duration_scalar(duration_scalar);
    req.set_pitch_scalar(pitch);
    vpb::SayTextResponse resp;
    auto c = ctx(60);
    { std::lock_guard<std::mutex> lk(say_mu_); say_ctx_ = c.get(); }
    auto st = stub_->SayText(c.get(), req, &resp);
    { std::lock_guard<std::mutex> lk(say_mu_); say_ctx_ = nullptr; }
    if (st.error_code() == grpc::StatusCode::CANCELLED) return false;  // interrupted on purpose
    return rpcOk(st, "SayText");
}

void VectorClient::cancelSay() {
    std::lock_guard<std::mutex> lk(say_mu_);
    if (say_ctx_) say_ctx_->TryCancel();
}

bool VectorClient::cameraConfig(float& focal_x, float& center_x, float& fov_x_deg) {
    vpb::CameraConfigRequest req;
    vpb::CameraConfigResponse resp;
    auto c = ctx(5);
    if (!rpcOk(stub_->GetCameraConfig(c.get(), req, &resp), "GetCameraConfig")) return false;
    focal_x = resp.focal_length_x();
    center_x = resp.center_x();
    fov_x_deg = resp.fov_x();
    return focal_x > 0;
}

bool VectorClient::setHeadDeg(float deg) {
    vpb::SetHeadAngleRequest req;
    req.set_id_tag(nextActionTag());
    req.set_angle_rad(std::clamp(deg, -22.f, 45.f) * kDeg2Rad);
    req.set_max_speed_rad_per_sec(10.f);
    req.set_accel_rad_per_sec2(10.f);
    vpb::SetHeadAngleResponse resp;
    auto c = ctx();
    return rpcOk(stub_->SetHeadAngle(c.get(), req, &resp), "SetHeadAngle");
}

bool VectorClient::setLift(float fraction) {
    // Lift height range on Vector is ~32mm (down) .. ~92mm (up).
    vpb::SetLiftHeightRequest req;
    req.set_id_tag(nextActionTag());
    req.set_height_mm(32.f + std::clamp(fraction, 0.f, 1.f) * 60.f);
    req.set_max_speed_rad_per_sec(10.f);
    req.set_accel_rad_per_sec2(10.f);
    vpb::SetLiftHeightResponse resp;
    auto c = ctx();
    return rpcOk(stub_->SetLiftHeight(c.get(), req, &resp), "SetLiftHeight");
}

bool VectorClient::turnDeg(float deg) {
    vpb::TurnInPlaceRequest req;
    req.set_id_tag(nextActionTag());
    req.set_angle_rad(deg * kDeg2Rad);
    req.set_speed_rad_per_sec(2.f);
    req.set_accel_rad_per_sec2(5.f);
    req.set_tol_rad(2.f * kDeg2Rad);
    vpb::TurnInPlaceResponse resp;
    auto c = ctx(30);
    return rpcOk(stub_->TurnInPlace(c.get(), req, &resp), "TurnInPlace");
}

bool VectorClient::driveMm(float mm, float speed_mmps) {
    vpb::DriveStraightRequest req;
    req.set_id_tag(nextActionTag());
    req.set_dist_mm(mm);
    req.set_speed_mmps(std::fabs(speed_mmps));
    req.set_should_play_animation(true);
    vpb::DriveStraightResponse resp;
    auto c = ctx(30);
    return rpcOk(stub_->DriveStraight(c.get(), req, &resp), "DriveStraight");
}

bool VectorClient::driveWheels(float l, float r) {
    vpb::DriveWheelsRequest req;
    req.set_left_wheel_mmps(l);
    req.set_right_wheel_mmps(r);
    vpb::DriveWheelsResponse resp;
    auto c = ctx();
    return rpcOk(stub_->DriveWheels(c.get(), req, &resp), "DriveWheels");
}

bool VectorClient::stopMotors() {
    vpb::StopAllMotorsRequest req;
    vpb::StopAllMotorsResponse resp;
    auto c = ctx();
    return rpcOk(stub_->StopAllMotors(c.get(), req, &resp), "StopAllMotors");
}

bool VectorClient::setEyeColor(float hue, float sat) {
    vpb::SetEyeColorRequest req;
    req.set_hue(std::clamp(hue, 0.f, 1.f));
    req.set_saturation(std::clamp(sat, 0.f, 1.f));
    vpb::SetEyeColorResponse resp;
    auto c = ctx();
    return rpcOk(stub_->SetEyeColor(c.get(), req, &resp), "SetEyeColor");
}

bool VectorClient::playAnimTrigger(const std::string& trigger) {
    vpb::PlayAnimationTriggerRequest req;
    req.mutable_animation_trigger()->set_name(trigger);
    req.set_loops(1);
    vpb::PlayAnimationResponse resp;
    auto c = ctx(30);
    return rpcOk(stub_->PlayAnimationTrigger(c.get(), req, &resp), "PlayAnimationTrigger");
}

bool VectorClient::listAnimTriggers(std::vector<std::string>& out) {
    vpb::ListAnimationTriggersRequest req;
    vpb::ListAnimationTriggersResponse resp;
    auto c = ctx(30);
    if (!rpcOk(stub_->ListAnimationTriggers(c.get(), req, &resp), "ListAnimationTriggers"))
        return false;
    for (auto& t : resp.animation_trigger_names()) out.push_back(t.name());
    return true;
}

bool VectorClient::captureImage(Image& out, bool high_res, int timeout_sec) {
    vpb::CaptureSingleImageRequest req;
    req.set_enable_high_resolution(high_res);
    vpb::CaptureSingleImageResponse resp;
    auto c = ctx(timeout_sec);
    if (!rpcOk(stub_->CaptureSingleImage(c.get(), req, &resp), "CaptureSingleImage"))
        return false;
    out.data = resp.data();
    out.id = resp.image_id();
    out.timestamp = resp.frame_time_stamp();
    out.encoding = resp.image_encoding();
    return true;
}

std::vector<std::pair<int, std::string>> VectorClient::enrolledFaces() {
    std::vector<std::pair<int, std::string>> out;
    vpb::RequestEnrolledNamesRequest req;
    vpb::RequestEnrolledNamesResponse resp;
    auto c = ctx(10);
    if (rpcOk(stub_->RequestEnrolledNames(c.get(), req, &resp), "RequestEnrolledNames"))
        for (auto& f : resp.faces()) out.push_back({f.face_id(), f.name()});
    return out;
}

bool VectorClient::enrollFace(const std::string& name) {
    // Re-learning someone already enrolled ADDS samples to their existing record (better recognition)
    // instead of creating a second face with the same name.
    auto norm = [](const std::string& s) {
        std::string o;
        for (char ch : s)
            if (std::isalnum(static_cast<unsigned char>(ch))) o += char(std::tolower(static_cast<unsigned char>(ch)));
        return o;
    };
    int save_id = 0;
    for (auto& [id, n] : enrolledFaces())
        if (norm(n) == norm(name)) save_id = id;
    {
        vpb::SetFaceToEnrollRequest req;
        req.set_name(name);
        req.set_observed_id(0);  // next face seen
        req.set_save_id(save_id);
        req.set_save_to_robot(true);
        req.set_say_name(true);
        vpb::SetFaceToEnrollResponse resp;
        auto c = ctx(10);
        if (!rpcOk(stub_->SetFaceToEnroll(c.get(), req, &resp), "SetFaceToEnroll")) return false;
    }
    vpb::EnrollFaceRequest req;
    vpb::EnrollFaceResponse resp;
    auto c = ctx(60);
    return rpcOk(stub_->EnrollFace(c.get(), req, &resp), "EnrollFace");
}

bool VectorClient::renameEnrolledFace(const std::string& old_name, const std::string& new_name) {
    auto norm = [](const std::string& s) {
        std::string o;
        for (char c : s)
            if (std::isalnum(static_cast<unsigned char>(c))) o += char(std::tolower(static_cast<unsigned char>(c)));
        return o;
    };
    vpb::RequestEnrolledNamesRequest req;
    vpb::RequestEnrolledNamesResponse resp;
    {
        auto c = ctx(10);
        if (!rpcOk(stub_->RequestEnrolledNames(c.get(), req, &resp), "RequestEnrolledNames")) return false;
    }
    bool any = false;
    for (auto& f : resp.faces()) {
        if (norm(f.name()) != norm(old_name)) continue;
        vpb::UpdateEnrolledFaceByIDRequest u;
        u.set_face_id(f.face_id());
        u.set_old_name(f.name());
        u.set_new_name(new_name);
        vpb::UpdateEnrolledFaceByIDResponse ur;
        auto c = ctx(10);
        if (rpcOk(stub_->UpdateEnrolledFaceByID(c.get(), u, &ur), "UpdateEnrolledFaceByID")) any = true;
    }
    return any;
}

#define SIMPLE_BEHAVIOR(Method, Rpc, secs)                     \
    bool VectorClient::Method() {                              \
        vpb::Rpc##Request req;                                 \
        vpb::Rpc##Response resp;                               \
        auto c = ctx(secs);                                    \
        return rpcOk(stub_->Rpc(c.get(), req, &resp), #Rpc);   \
    }
SIMPLE_BEHAVIOR(findFaces, FindFaces, 20)
SIMPLE_BEHAVIOR(lookAround, LookAroundInPlace, 30)
SIMPLE_BEHAVIOR(driveOnCharger, DriveOnCharger, 90)
SIMPLE_BEHAVIOR(driveOffCharger, DriveOffCharger, 30)
#undef SIMPLE_BEHAVIOR

bool VectorClient::connectCube(std::string* factory_id) {
    vpb::ConnectCubeRequest req;
    vpb::ConnectCubeResponse resp;
    auto c = ctx(25);
    if (!rpcOk(stub_->ConnectCube(c.get(), req, &resp), "ConnectCube")) return false;
    if (factory_id) *factory_id = resp.factory_id();
    return resp.success();
}

bool VectorClient::flashCube() {
    vpb::FlashCubeLightsRequest req;
    vpb::FlashCubeLightsResponse resp;
    auto c = ctx(10);
    return rpcOk(stub_->FlashCubeLights(c.get(), req, &resp), "FlashCubeLights");
}

bool VectorClient::appIntent(const std::string& intent, const std::string& param) {
    vpb::AppIntentRequest req;
    req.set_intent(intent);
    req.set_param(param);
    vpb::AppIntentResponse resp;
    auto c = ctx(10);
    return rpcOk(stub_->AppIntent(c.get(), req, &resp), "AppIntent");
}

bool VectorClient::enableFaceDetection(bool on) {
    vpb::EnableFaceDetectionRequest req;
    req.set_enable(on);
    req.set_enable_expression_estimation(on);
    vpb::EnableFaceDetectionResponse resp;
    auto c = ctx(10);
    return rpcOk(stub_->EnableFaceDetection(c.get(), req, &resp), "EnableFaceDetection");
}

bool VectorClient::enableImageStreaming(bool on) {
    vpb::EnableImageStreamingRequest req;
    req.set_enable(on);
    vpb::EnableImageStreamingResponse resp;
    auto c = ctx(5);
    return rpcOk(stub_->EnableImageStreaming(c.get(), req, &resp), "EnableImageStreaming");
}

bool VectorClient::batteryInfo(std::string& out) {
    vpb::BatteryStateRequest req;
    vpb::BatteryStateResponse resp;
    auto c = ctx();
    if (!rpcOk(stub_->BatteryState(c.get(), req, &resp), "BatteryState")) return false;
    std::ostringstream ss;
    ss << "battery " << vpb::BatteryLevel_Name(resp.battery_level()) << " " << resp.battery_volts()
       << "V" << (resp.is_charging() ? " charging" : "")
       << (resp.is_on_charger_platform() ? " on-charger" : "");
    out = ss.str();
    return true;
}

bool VectorClient::battery(Battery& b) {
    vpb::BatteryStateRequest req;
    vpb::BatteryStateResponse resp;
    auto c = ctx(5);
    if (!stub_->BatteryState(c.get(), req, &resp).ok()) return false;
    b.level = resp.battery_level();
    b.volts = resp.battery_volts();
    b.charging = resp.is_charging();
    b.on_charger = resp.is_on_charger_platform();
    if (resp.has_cube_battery() && !resp.cube_battery().factory_id().empty()) {
        b.cube_known = true;
        b.cube_level = resp.cube_battery().level();
        b.cube_volts = resp.cube_battery().battery_volts();
    }
    return true;
}

bool VectorClient::versionInfo(std::string& out) {
    vpb::VersionStateRequest req;
    vpb::VersionStateResponse resp;
    auto c = ctx();
    if (!rpcOk(stub_->VersionState(c.get(), req, &resp), "VersionState")) return false;
    out = "os " + resp.os_version() + ", engine " + resp.engine_build_id();
    return true;
}

bool VectorClient::playPcm16(const std::vector<int16_t>& samples, uint32_t rate, uint32_t volume) {
    if (rate < 8000 || rate > 16025) {
        std::fprintf(stderr, "[vector] PCM rate %u out of range 8000..16025\n", rate);
        return false;
    }
    auto c = ctx(0);
    auto stream = stub_->ExternalAudioStreamPlayback(c.get());
    if (!stream) return false;

    vpb::ExternalAudioStreamRequest prep;
    prep.mutable_audio_stream_prepare()->set_audio_frame_rate(rate);
    prep.mutable_audio_stream_prepare()->set_audio_volume(std::min<uint32_t>(volume, 100));
    if (!stream->Write(prep)) return false;

    // Max 1024 bytes per chunk; stay no more than ~0.5s ahead of playback,
    // like the official SDK, or the robot's buffer overruns.
    constexpr size_t kFrames = 512;
    auto start = std::chrono::steady_clock::now();
    for (size_t off = 0; off < samples.size(); off += kFrames) {
        size_t n = std::min(kFrames, samples.size() - off);
        vpb::ExternalAudioStreamRequest chunk;
        auto* ch = chunk.mutable_audio_stream_chunk();
        ch->set_audio_chunk_size_bytes(uint32_t(n * 2));
        ch->set_audio_chunk_samples(reinterpret_cast<const char*>(samples.data() + off), n * 2);
        if (!stream->Write(chunk)) break;

        double sent_sec = double(off + n) / rate;
        double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
        if (sent_sec - elapsed > 0.5)
            std::this_thread::sleep_for(std::chrono::duration<double>(sent_sec - elapsed - 0.5));
    }
    vpb::ExternalAudioStreamRequest done;
    done.mutable_audio_stream_complete();
    stream->Write(done);
    stream->WritesDone();

    vpb::ExternalAudioStreamResponse resp;
    bool ok = false;
    while (stream->Read(&resp)) {
        if (resp.has_audio_stream_playback_complete()) { ok = true; break; }
        if (resp.has_audio_stream_buffer_overrun())
            std::fprintf(stderr, "[vector] audio buffer overrun\n");
        if (resp.has_audio_stream_playback_failyer()) break;  // sic, typo is in the proto
    }
    if (!ok) {
        // Some firmware just closes the stream after playing instead of sending
        // a completion message; a clean close still means it played.
        auto st = stream->Finish();
        if (!st.ok())
            std::fprintf(stderr, "[vector] audio playback failed: %s (%d)\n",
                         st.error_message().c_str(), int(st.error_code()));
        return st.ok();
    }
    c->TryCancel();
    stream->Finish();
    return ok;
}

bool VectorClient::userAuthentication(const std::string& session_token,
                                      const std::string& client_name, std::string& guid_out,
                                      std::string* err) {
    vpb::UserAuthenticationRequest req;
    req.set_user_session_id(session_token);
    req.set_client_name(client_name);
    vpb::UserAuthenticationResponse resp;
    auto c = ctx(30);
    auto st = stub_->UserAuthentication(c.get(), req, &resp);
    if (!st.ok()) {
        if (err) *err = st.error_message();
        return false;
    }
    if (resp.code() != vpb::UserAuthenticationResponse::AUTHORIZED) {
        if (err) *err = "robot (or its token server) refused the session token";
        return false;
    }
    guid_out = resp.client_token_guid();
    return true;
}

void VectorClient::track(grpc::ClientContext* c) {
    std::lock_guard<std::mutex> lk(streams_mu_);
    streams_.insert(c);
}
void VectorClient::untrack(grpc::ClientContext* c) {
    std::lock_guard<std::mutex> lk(streams_mu_);
    streams_.erase(c);
}
void VectorClient::stopStreams() {
    std::lock_guard<std::mutex> lk(streams_mu_);
    for (auto* c : streams_) c->TryCancel();
}

void VectorClient::cameraFeed(const std::function<bool(const Image&)>& cb) {
    auto c = ctx(0);
    track(c.get());
    vpb::CameraFeedRequest req;
    auto reader = stub_->CameraFeed(c.get(), req);
    vpb::CameraFeedResponse resp;
    while (reader->Read(&resp)) {
        Image img;
        img.data = resp.data();
        img.id = resp.image_id();
        img.timestamp = resp.frame_time_stamp();
        img.encoding = resp.image_encoding();
        if (!cb(img)) { c->TryCancel(); break; }
    }
    auto st = reader->Finish();
    untrack(c.get());
    if (!st.ok() && st.error_code() != grpc::StatusCode::CANCELLED)
        std::fprintf(stderr, "[vector] camera feed ended: %s\n", st.error_message().c_str());
}

void VectorClient::events(const std::function<bool(const vpb::Event&)>& cb) {
    auto c = ctx(0);
    track(c.get());
    vpb::EventRequest req;  // no filter = everything
    auto reader = stub_->EventStream(c.get(), req);
    vpb::EventResponse resp;
    while (reader->Read(&resp)) {
        if (!cb(resp.event())) { c->TryCancel(); break; }
    }
    auto st = reader->Finish();
    untrack(c.get());
    if (!st.ok() && st.error_code() != grpc::StatusCode::CANCELLED)
        std::fprintf(stderr, "[vector] event stream ended: %s\n", st.error_message().c_str());
}

// ---------------------------------------------------------------------------

ControlSession::ControlSession(VectorClient& client, vpb::ControlRequest::Priority prio)
    : client_(client), prio_(prio) {}

ControlSession::~ControlSession() { release(); }

bool ControlSession::acquire(int timeout_ms) {
    if (granted_) return true;
    release();
    closed_ = false;
    ctx_ = client_.ctx(0);
    stream_ = client_.stub().BehaviorControl(ctx_.get());
    vpb::BehaviorControlRequest req;
    req.mutable_control_request()->set_priority(prio_);
    if (!stream_->Write(req)) {
        std::fprintf(stderr, "[vector] could not request behavior control\n");
        return false;
    }
    reader_ = std::thread([this] { readLoop(); });
    std::unique_lock<std::mutex> lk(mu_);
    cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms),
                 [this] { return granted_.load() || closed_.load(); });
    if (!granted_) std::fprintf(stderr, "[vector] behavior control not granted\n");
    return granted_;
}

void ControlSession::readLoop() {
    vpb::BehaviorControlResponse resp;
    while (stream_->Read(&resp)) {
        if (resp.has_control_granted_response()) {
            granted_ = true;
            cv_.notify_all();
        } else if (resp.has_control_lost_event() || resp.has_reserved_control_lost_event()) {
            // A higher-priority behavior (picked up, low battery, ...) took over.
            std::fprintf(stderr, "[vector] behavior control lost\n");
            granted_ = false;
        }
        // keep_alive: nothing to do, reading it is enough
    }
    granted_ = false;
    closed_ = true;
    cv_.notify_all();
}

void ControlSession::release() {
    if (!stream_) return;
    vpb::BehaviorControlRequest req;
    req.mutable_control_release();
    stream_->Write(req);
    stream_->WritesDone();
    ctx_->TryCancel();
    if (reader_.joinable()) reader_.join();
    stream_->Finish();
    stream_.reset();
    ctx_.reset();
    granted_ = false;
}

}  // namespace mbot
