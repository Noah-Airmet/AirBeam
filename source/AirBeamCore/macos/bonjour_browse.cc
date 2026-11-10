// Copyright (c) 2025 ChenKS12138

#include "bonjour_browse.h"

#include "helper/logger.h"

namespace AirBeamCore {
namespace macos {
bool BonjourBrowse::ServiceInfo::operator==(const ServiceInfo& other) const {
  return name == other.name && fullname == other.fullname && ip == other.ip &&
         port == other.port;
}

BonjourBrowse::BonjourBrowse() : browseRef_(nullptr), running_(false) {}

BonjourBrowse::~BonjourBrowse() { stop(); }

bool BonjourBrowse::startBrowse(const std::string& serviceType,
                                ServiceFoundCallback callback) {
  if (running_) return false;
  callback_ = callback;

  ABDebugLog("DNSServiceBrowse");
  DNSServiceErrorType err =
      DNSServiceBrowse(&browseRef_, 0, 0, serviceType.c_str(), nullptr,
                       &BonjourBrowse::BrowseCallback, this);

  if (err != kDNSServiceErr_NoError) {
    ABDebugLog("DNSServiceBrowse failed: %d", err);
    return false;
  }

  running_ = true;
  browseThread_ = std::thread(&BonjourBrowse::browseLoop, this);
  return true;
}

void BonjourBrowse::stop() {
  if (!running_) return;

  running_ = false;

  if (browseRef_) {
    DNSServiceRefDeallocate(browseRef_);
    browseRef_ = nullptr;
  }

  if (browseThread_.joinable()) {
    browseThread_.join();
  }
}

void BonjourBrowse::browseLoop() {
  ABDebugLog("start browse loop");
  int fd = DNSServiceRefSockFD(browseRef_);
  if (fd == -1) {
    ABDebugLog("DNSServiceRefSockFD failed");
    return;
  }

  while (running_) {
    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(fd, &readfds);

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;

    int result = select(fd + 1, &readfds, nullptr, nullptr, &tv);
    if (result > 0 && FD_ISSET(fd, &readfds)) {
      DNSServiceProcessResult(browseRef_);
    }
  }
}

void DNSSD_API BonjourBrowse::BrowseCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* serviceName, const char* regtype,
    const char* replyDomain, void* context) {
  ABDebugLog("browse callback %d", errorCode);
  if (errorCode != kDNSServiceErr_NoError) return;

  BonjourBrowse* self = static_cast<BonjourBrowse*>(context);

  if (flags & kDNSServiceFlagsAdd) {
    DNSServiceRef resolveRef;
    DNSServiceErrorType err =
        DNSServiceResolve(&resolveRef, 0, interfaceIndex, serviceName, regtype,
                          replyDomain, &BonjourBrowse::ResolveCallback, self);

    if (err == kDNSServiceErr_NoError) {
      int fd = DNSServiceRefSockFD(resolveRef);
      if (fd != -1) {
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(fd, &readfds);
        struct timeval tv;
        tv.tv_sec = 5;
        tv.tv_usec = 0;

        int result = select(fd + 1, &readfds, nullptr, nullptr, &tv);
        if (result > 0 && FD_ISSET(fd, &readfds)) {
          DNSServiceProcessResult(resolveRef);
        }
      }
      DNSServiceRefDeallocate(resolveRef);
    }
  } else {
    BonjourBrowse* self = static_cast<BonjourBrowse*>(context);
    if (self && self->callback_) {
      ServiceInfo service_info;
      service_info.name = extractShortName(serviceName);
      service_info.fullname = serviceName;
      service_info.ip = "";
      service_info.port = 0;
      self->callback_(EventType::kServiceOffline, service_info);
    }
  }
}

void DNSSD_API BonjourBrowse::ResolveCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* fullname, const char* hosttarget,
    uint16_t port, uint16_t txtLen, const unsigned char* txtRecord,
    void* context) {
  if (errorCode != kDNSServiceErr_NoError) return;

  BonjourBrowse* self = static_cast<BonjourBrowse*>(context);
  if (!self) return;

  // Only expose HomePods (model "AudioAccessory*"), not every Mac / Apple TV
  // that advertises RAOP.
  uint8_t amLen = 0;
  const void* am = TXTRecordGetValuePtr(txtLen, txtRecord, "am", &amLen);
  static const char kHomePodPrefix[] = "AudioAccessory";
  if (!am || amLen < sizeof(kHomePodPrefix) - 1 ||
      std::memcmp(am, kHomePodPrefix, sizeof(kHomePodPrefix) - 1) != 0) {
    return;
  }

  port = ntohs(port);

  ContextForAddr ctx{self, "", fullname, port};

  DNSServiceRef addrRef;
  DNSServiceErrorType err = DNSServiceGetAddrInfo(
      &addrRef, 0, interfaceIndex, kDNSServiceProtocol_IPv4, hosttarget,
      &BonjourBrowse::GetAddrInfoCallback, &ctx);

  if (err == kDNSServiceErr_NoError) {
    int fd = DNSServiceRefSockFD(addrRef);
    if (fd != -1) {
      fd_set readfds;
      FD_ZERO(&readfds);
      FD_SET(fd, &readfds);
      struct timeval tv;
      tv.tv_sec = 5;
      tv.tv_usec = 0;

      int result = select(fd + 1, &readfds, nullptr, nullptr, &tv);
      if (result > 0 && FD_ISSET(fd, &readfds)) {
        DNSServiceProcessResult(addrRef);
      }
    }
    DNSServiceRefDeallocate(addrRef);
  }
}

void DNSSD_API BonjourBrowse::GetAddrInfoCallback(
    DNSServiceRef sdRef, DNSServiceFlags flags, uint32_t interfaceIndex,
    DNSServiceErrorType errorCode, const char* hostname,
    const struct sockaddr* address, uint32_t ttl, void* context) {
  if (errorCode != kDNSServiceErr_NoError) return;
  if (!context) return;

  ContextForAddr* ctx = static_cast<ContextForAddr*>(context);
  BonjourBrowse* self = ctx->browser;

  char ipStr[INET6_ADDRSTRLEN] = {0};
  if (address->sa_family == AF_INET) {
    inet_ntop(AF_INET, &((struct sockaddr_in*)address)->sin_addr, ipStr,
              sizeof(ipStr));
  } else if (address->sa_family == AF_INET6) {
    inet_ntop(AF_INET6, &((struct sockaddr_in6*)address)->sin6_addr, ipStr,
              sizeof(ipStr));
  }

  if (self && self->callback_) {
    ServiceInfo service_info;
    service_info.name = extractShortName(ctx->fullName);
    service_info.fullname = ctx->fullName;
    service_info.ip = ipStr;
    service_info.port = ctx->port;
    self->callback_(EventType::kServiceOnline, service_info);
  }

}

std::string BonjourBrowse::extractShortName(const std::string& fullname) {
  std::string name = fullname;
  size_t pos = name.find("._");
  if (pos != std::string::npos) name = name.substr(0, pos);
  // RAOP names look like "2E597EF8E27C@Kitchen"; drop the MAC prefix.
  pos = name.find('@');
  if (pos != std::string::npos) name = name.substr(pos + 1);
  // Unescape DNS-SD "\032" style decimal escapes.
  std::string out;
  for (size_t i = 0; i < name.size(); ++i) {
    if (name[i] == '\\' && i + 3 < name.size() && isdigit(name[i + 1]) &&
        isdigit(name[i + 2]) && isdigit(name[i + 3])) {
      out += static_cast<char>(std::stoi(name.substr(i + 1, 3)));
      i += 3;
    } else {
      out += name[i];
    }
  }
  return out + " (HomePod)";
}
}  // namespace macos
}  // namespace AirBeamCore
