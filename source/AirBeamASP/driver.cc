// Copyright (c) 2025 ChenKS12138

#include "aspl/Driver.hpp"

#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreAudio/CoreAudio.h>
#include <arpa/inet.h>
#include <fmt/core.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <pthread.h>
#include <stdarg.h>
#include <sys/time.h>
#include <syslog.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

#include "aspl/ControlRequestHandler.hpp"
#include "aspl/Device.hpp"
#include "aspl/IORequestHandler.hpp"
#include "aspl/Plugin.hpp"
#include "helper/logger.h"
#include "macos/bonjour_browse.h"
#include "macos/volume_observer.h"
#include "raop/codec.h"
#include "raop/fifo.h"
#include "raop/raop.h"

namespace {

void airbeam_log(const char* fmt, ...) {
  FILE* f = fopen("/tmp/airbeam.log", "a");
  if (!f) return;
  struct timeval tv;
  gettimeofday(&tv, nullptr);
  struct tm tm;
  localtime_r(&tv.tv_sec, &tm);
  fprintf(f, "%02d:%02d:%02d.%03d ", tm.tm_hour, tm.tm_min, tm.tm_sec,
          (int)(tv.tv_usec / 1000));
  va_list args;
  va_start(args, fmt);
  vfprintf(f, fmt, args);
  va_end(args);
  fprintf(f, "\n");
  fclose(f);
}

using namespace AirBeamCore::raop;
using namespace AirBeamCore::helper;
using namespace AirBeamCore::macos;

constexpr UInt32 SampleRate = 44100;
constexpr UInt32 ChannelCount = 2;

const size_t kFiFOCapacity = 30 * 1024 * 1024;  // 30MB buffer

class RaopHandler : public aspl::ControlRequestHandler,
                    public aspl::IORequestHandler {
 public:
  explicit RaopHandler(aspl::Device& device, const std::string& ip,
                       uint32_t port)
      : ip_(ip), port_(port), fifo_(kFiFOCapacity), device_(device) {
    auto volume_control =
        device_.GetVolumeControlByIndex(kAudioObjectPropertyScopeOutput, 0);
    volume_control->SetScalarValue(0.5);
  }

  OSStatus OnStartIO() override {
    Prepare();
    return kAudioHardwareNoError;
  }

  void OnStopIO() override {}

  void OnWriteMixedOutput(const std::shared_ptr<aspl::Stream>& stream,
                          Float64 zeroTimestamp, Float64 timestamp,
                          const void* buff, UInt32 buffBytesSize) override {
    if (!connected_) {
      // Don't queue audio while disconnected, but note real (non-silent)
      // audio so we know to reconnect.
      const uint8_t* b = reinterpret_cast<const uint8_t*>(buff);
      for (UInt32 i = 0; i < buffBytesSize; ++i) {
        if (b[i] != 0) { wants_audio_ = true; break; }
      }
      return;
    }
    size_t written = fifo_.Write(reinterpret_cast<const uint8_t*>(buff),
                                 buffBytesSize, std::chrono::milliseconds(5));
    if (written < buffBytesSize) {
      syslog(LOG_WARNING,
             "[AirBeam] FIFO DROP: wrote %zu/%u bytes (FIFO full)",
             written, buffBytesSize);
    }
  }

 public:
  std::unique_ptr<VolumeObserver> volume_observer_ = nullptr;

 private:
  const std::string ip_;
  const uint32_t port_;
  std::shared_ptr<Raop> raop_;
  std::mutex raop_mutex_;
  std::atomic<bool> connected_{false};
  std::atomic<bool> wants_audio_{true};
  std::atomic<uint8_t> volume_percent_{50};
  ConcurrentByteFIFO fifo_;
  aspl::Device& device_;

  std::unique_ptr<std::thread> consumer_thread_;

  std::shared_ptr<Raop> Current() {
    std::lock_guard<std::mutex> lock(raop_mutex_);
    return raop_;
  }

  // Retire a session: stop its threads, free it once they have exited.
  static void Retire(std::shared_ptr<Raop> r) {
    if (!r) return;
    r->Stop();
    std::thread([r]() { std::this_thread::sleep_for(std::chrono::seconds(8)); })
        .detach();
  }

  // Connect (or reconnect) until the HomePod accepts a session.
  std::shared_ptr<Raop> Connect() {
    int attempt = 0;
    while (true) {
      auto r = std::make_shared<Raop>(ip_, port_);
      if (r->Start()) {
        r->SetVolume(volume_percent_);
        {
          std::lock_guard<std::mutex> lock(raop_mutex_);
          raop_ = r;
        }
        airbeam_log("CONNECTED to %s:%u", ip_.c_str(), port_);
        return r;
      }
      Retire(r);
      attempt++;
      airbeam_log("CONNECT FAILED (attempt %d), retrying", attempt);
      std::this_thread::sleep_for(std::chrono::seconds(attempt < 5 ? 1 : 5));
    }
  }

  void Prepare() {
    if (consumer_thread_) {
      return;
    }

    consumer_thread_ = std::make_unique<std::thread>([this]() {
      // Real-time constraint: declare 2ms of CPU needed within a 4ms window,
      // every 8ms period. The FIFO read blocks off-CPU (doesn't count).
      mach_timebase_info_data_t timebase;
      mach_timebase_info(&timebase);
      uint32_t chunk_ns = 352 * 1'000'000'000U / SampleRate;  // ~8ms
      uint32_t chunk_abs = chunk_ns * timebase.denom / timebase.numer;
      thread_time_constraint_policy_data_t policy;
      policy.period = chunk_abs;
      policy.computation = chunk_abs / 4;
      policy.constraint = chunk_abs / 2;
      policy.preemptible = 1;
      thread_policy_set(mach_thread_self(), THREAD_TIME_CONSTRAINT_POLICY,
                        reinterpret_cast<thread_policy_t>(&policy),
                        THREAD_TIME_CONSTRAINT_POLICY_COUNT);

      // Anything beyond this much queued audio is dropped, so latency can't
      // creep up after a stall.
      const size_t kMaxQueuedBytes = SampleRate * 4 / 10;  // ~100 ms
      const size_t kCushionBytes = SampleRate * 4 / 50;    // ~20 ms
      bool active = false;

      std::shared_ptr<Raop> raop;
      while (true) {
        if (!raop || !raop->Healthy()) {
          connected_ = false;
          if (raop) {
            // Usually another sender (a phone) took the HomePod. Don't grab
            // it back until the Mac actually has something to play.
            airbeam_log("SESSION LOST, waiting for audio to reconnect");
            Retire(raop);
            raop = nullptr;
            wants_audio_ = false;
            while (!wants_audio_) {
              std::this_thread::sleep_for(std::chrono::milliseconds(50));
            }
          }
          raop = Connect();
          // Discard anything queued while we were disconnected.
          uint8_t junk[16384];
          while (fifo_.Read(junk, sizeof(junk), std::chrono::milliseconds(1)) > 0) {}
          connected_ = true;
        }

        // The stream never pauses: when nothing is playing we send silence,
        // the way Apple's own senders do. Pace first (one 8 ms slot per
        // packet, in real time), then take whatever audio is queued, so the
        // RTP clock never falls behind the wall clock.
        raop->AcceptFrame();

        RtpAudioPacketChunk chunk, encoded;
        const size_t want = sizeof(chunk.data_);
        size_t queued = fifo_.Size();
        // Start consuming only once ~20 ms is queued, so CoreAudio's bursty
        // writes never leave a half-empty packet in the middle of a sound.
        if (!active && queued >= kCushionBytes) active = true;
        size_t got = 0;
        if (active) {
          got = fifo_.Read(chunk.data_, want, std::chrono::milliseconds(1));
          if (got < want) active = false;  // underrun: back to silence
        }
        if (got < want) memset(chunk.data_ + got, 0, want - got);
        chunk.len_ = want;

        // Trim backlog beyond ~100 ms.
        uint8_t junk[4096];
        while (fifo_.Size() > kMaxQueuedBytes) {
          size_t excess = fifo_.Size() - kMaxQueuedBytes;
          fifo_.Read(junk, std::min(excess, sizeof(junk)),
                     std::chrono::milliseconds(1));
        }

        PCMCodec::Encode(chunk, encoded);
        encoded.len_ = chunk.len_;

        raop->PrepareChunk(encoded);
        raop->SendPreparedChunk();
      }
    });
    consumer_thread_->detach();

    (new std::thread([=]() {
      auto device_uid = device_.GetDeviceUID();

      AudioObjectID output_device_id = 0;
      while (true) {
        OSStatus status =
            VolumeObserver::FindAudioDeviceByUID(device_uid, output_device_id);

        if (status == noErr) {
          break;
        }
        if (status == kAudioHardwareUnknownPropertyError) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
          continue;
        }
        return;
      }

      volume_observer_ = std::make_unique<VolumeObserver>(
          output_device_id, [=](Float32 volume) {
            uint8_t volume_percent = volume * 100;
            volume_percent_ = volume_percent;
            if (auto r = Current()) r->SetVolume(volume_percent);
          });
    }))->detach();
  }
};

class DriverHelper {
 protected:
  struct DeviceInfo {
    std::shared_ptr<aspl::Device> device_;
    BonjourBrowse::ServiceInfo service_info_;
  };

 public:
  DriverHelper()
      : context_(std::make_shared<aspl::Context>()),
        plugin_(std::make_shared<aspl::Plugin>(context_)),
        driver_(std::make_shared<aspl::Driver>(context_, plugin_)),
        devices_mapping_() {
    static const std::string kServiceType = "_raop._tcp";
    using namespace std::placeholders;
    ABDebugLog("DriverHelper startBrowse %s", kServiceType.c_str());
    browse_.startBrowse(
        kServiceType,
        std::bind(&DriverHelper::HandleBrowseCallback, this, _1, _2));
  }

  std::shared_ptr<aspl::Driver> GetDriver() { return driver_; }

 private:
  void HandleBrowseCallback(BonjourBrowse::EventType event_type,
                            const BonjourBrowse::ServiceInfo& service_info) {
    using namespace std::chrono;

    std::lock_guard<std::mutex> guard(device_mapping_mutex_);

    if (event_type == BonjourBrowse::EventType::kServiceOnline) {
      ABDebugLog("service online %s %s %s %lu", service_info.fullname.c_str(),
                 service_info.name.c_str(), service_info.ip.c_str(),
                 service_info.port);
      // Cancel any pending removal — the service came back before the
      // grace period expired. This is the common case for WiFi flaps.
      auto pending = pending_removals_.find(service_info.fullname);
      if (pending != pending_removals_.end()) {
        pending_removals_.erase(pending);
        airbeam_log("FLAP ABSORBED: %s came back online", service_info.fullname.c_str());
      }
      auto it = devices_mapping_.find(service_info.fullname);
      if (it != devices_mapping_.end()) {
        if (it->second.service_info_ == service_info) {
          ABDebugLog("device already exists %s", service_info.fullname.c_str());
          return;
        }
        ABDebugLog("device exists service changed %s",
                   service_info.fullname.c_str());
        RemoveDevice(it->second.service_info_);
      }

      AddDevice(service_info);
      ABDebugLog("device %s added", service_info.fullname.c_str());
      return;
    }

    if (event_type == BonjourBrowse::EventType::kServiceOffline) {
      ABDebugLog("kServiceOffline %s", service_info.fullname.c_str());
      // Don't remove immediately — Bonjour flaps on WiFi when mDNS packets
      // are lost. Wait 10 seconds; if the service doesn't come back online,
      // then remove it. This prevents the music app from seeing the device
      // vanish during brief network hiccups.
      auto fullname = service_info.fullname;
      pending_removals_[fullname] = std::chrono::steady_clock::now();
      (new std::thread([this, fullname, service_info]() {
        std::this_thread::sleep_for(std::chrono::seconds(10));
        std::lock_guard<std::mutex> guard(device_mapping_mutex_);
        auto it = pending_removals_.find(fullname);
        if (it != pending_removals_.end()) {
          // Still pending after 10s — actually remove
          pending_removals_.erase(it);
          if (devices_mapping_.find(fullname) != devices_mapping_.end()) {
            RemoveDevice(service_info);
            airbeam_log("DEVICE REMOVED: %s (offline >10s)", fullname.c_str());
          }
        }
      }))->detach();
      return;
    }
  }

  void AddDevice(const BonjourBrowse::ServiceInfo& service_info) {
    aspl::DeviceParameters deviceParams;
    deviceParams.Name = service_info.name;
    deviceParams.SampleRate = SampleRate;
    deviceParams.ChannelCount = ChannelCount;
    deviceParams.EnableMixing = true;

    auto device = std::make_shared<aspl::Device>(context_, deviceParams);
    device->AddStreamWithControlsAsync(aspl::Direction::Output);
    auto raop_handler = std::make_shared<RaopHandler>(*device, service_info.ip,
                                                      service_info.port);

    device->SetControlHandler(raop_handler);
    device->SetIOHandler(raop_handler);
    plugin_->AddDevice(device);
    devices_mapping_[service_info.fullname] = {device, service_info};
  }

  void RemoveDevice(const BonjourBrowse::ServiceInfo& service_info) {
    auto it = devices_mapping_.find(service_info.fullname);
    plugin_->RemoveDevice(it->second.device_);
    devices_mapping_.erase(it);
  }

 private:
  std::shared_ptr<aspl::Context> context_;
  std::shared_ptr<aspl::Plugin> plugin_;
  std::shared_ptr<aspl::Driver> driver_;
  std::map<std::string, DeviceInfo> devices_mapping_;
  std::map<std::string, std::chrono::steady_clock::time_point> pending_removals_;
  std::mutex device_mapping_mutex_;
  BonjourBrowse browse_;
};

std::shared_ptr<aspl::Driver> CreateRaopDriver() {
  static DriverHelper helper;
  return helper.GetDriver();
}

}  // namespace

extern "C" void* AirBeamASPEntryPoint(CFAllocatorRef allocator,
                                      CFUUIDRef typeUUID) {
  if (!CFEqual(typeUUID, kAudioServerPlugInTypeUUID)) {
    return nullptr;
  }

  static std::shared_ptr<aspl::Driver> driver = CreateRaopDriver();

  return driver->GetReference();
}
