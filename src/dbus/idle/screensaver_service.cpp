#include "dbus/idle/screensaver_service.h"

#include "core/log.h"
#include "dbus/system_bus.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <fstream>
#include <map>
#include <utility>
#include <vector>

namespace {

  constexpr Logger kLog("screensaver");

  const sdbus::ServiceName kBusName{"org.freedesktop.ScreenSaver"};
  const sdbus::ServiceName kLogindBusName{"org.freedesktop.login1"};
  const sdbus::ObjectPath kLogindObjectPath{"/org/freedesktop/login1"};
  constexpr auto kInterface = "org.freedesktop.ScreenSaver";
  constexpr auto kLogindManagerInterface = "org.freedesktop.login1.Manager";
  constexpr auto kPropertiesInterface = "org.freedesktop.DBus.Properties";
  constexpr auto kFileExistsError = "org.freedesktop.DBus.Error.FileExists";

  constexpr int kMaxEventsPerDispatch = 16;
  constexpr auto kMaxDispatchBudget = std::chrono::milliseconds(4);

  const sdbus::ObjectPath kObjectPaths[] = {
      sdbus::ObjectPath{"/org/freedesktop/ScreenSaver"},
      sdbus::ObjectPath{"/ScreenSaver"},
  };

} // namespace

ScreenSaverService::ScreenSaverService(SystemBus* systemBus) {
  try {
    m_connection = sdbus::createSessionBusConnection(kBusName);
    registerScreenSaver();
  } catch (const sdbus::Error& e) {
    if (e.getName() != kFileExistsError) {
      kLog.warn("screensaver D-Bus registration failed: {}", e.what());
    }
    m_connection.reset();
    m_objects.clear();
    m_dbusProxy.reset();
  } catch (const std::exception& e) {
    kLog.warn("screensaver D-Bus registration failed: {}", e.what());
    m_connection.reset();
    m_objects.clear();
    m_dbusProxy.reset();
  }

  registerLogindIdleMonitor(systemBus);
  if (!m_active && m_logindProxy == nullptr) {
    kLog.warn("no idle inhibit sources available");
  }
}

ScreenSaverService::~ScreenSaverService() {
  m_logindProxy.reset();
  m_dbusProxy.reset();
  m_objects.clear();
  m_connection.reset();
}

std::vector<std::pair<std::string, std::string>> ScreenSaverService::inhibitorList() const {
  std::vector<std::pair<std::string, std::string>> result;
  result.reserve(m_cookies.size());
  for (const auto& cookie : m_cookies) {
    result.emplace_back(cookie.ignored ? cookie.app + " (ignored)" : cookie.app, cookie.reason);
  }
  return result;
}

void ScreenSaverService::setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

void ScreenSaverService::emitActiveChanged(bool active) {
  if (!m_active) {
    return;
  }
  for (const auto& object : m_objects) {
    try {
      object->emitSignal("ActiveChanged").onInterface(kInterface).withArguments(active);
    } catch (const std::exception& e) {
      kLog.debug("ActiveChanged emit failed: {}", e.what());
    }
  }
}

sdbus::IConnection::PollData ScreenSaverService::getPollData() const {
  if (m_connection == nullptr) {
    return {};
  }
  return m_connection->getEventLoopPollData();
}

void ScreenSaverService::processPendingEvents() {
  if (m_connection == nullptr) {
    m_hasPendingEvents = false;
    return;
  }

  m_hasPendingEvents = false;
  const auto batchStart = std::chrono::steady_clock::now();
  for (int processed = 0; processed < kMaxEventsPerDispatch; ++processed) {
    if (!m_connection->processPendingEvent()) {
      return;
    }
    if (processed + 1 >= kMaxEventsPerDispatch || std::chrono::steady_clock::now() - batchStart >= kMaxDispatchBudget) {
      m_hasPendingEvents = true;
      return;
    }
  }
}

void ScreenSaverService::registerScreenSaver() {
  if (m_connection == nullptr) {
    return;
  }

  m_dbusProxy = sdbus::createProxy(
      *m_connection, sdbus::ServiceName{"org.freedesktop.DBus"}, sdbus::ObjectPath{"/org/freedesktop/DBus"}
  );
  m_dbusProxy->uponSignal("NameOwnerChanged")
      .onInterface("org.freedesktop.DBus")
      .call([this](const std::string& /*name*/, const std::string& oldOwner, const std::string& newOwner) {
        if (!newOwner.empty()) {
          return;
        }
        const std::size_t removed = unregisterOwnerCookies(oldOwner);
        if (removed > 0) {
          kLog.info("screensaver: cleared {} inhibit(s) after client disconnect", removed);
        }
      });

  for (const auto& path : kObjectPaths) {
    try {
      auto object = sdbus::createObject(*m_connection, path);
      object
          ->addVTable(
              sdbus::registerMethod("Inhibit")
                  .withInputParamNames("application_name", "reason_for_inhibit")
                  .withOutputParamNames("cookie")
                  .implementedAs([this, objectPtr = object.get()](std::string app, std::string reason) {
                    return onInhibit(
                        std::move(app), std::move(reason), objectPtr->getCurrentlyProcessedMessage().getSender()
                    );
                  }),
              sdbus::registerMethod("UnInhibit")
                  .withInputParamNames("cookie")
                  .implementedAs([this, objectPtr = object.get()](std::uint32_t cookie) {
                    onUninhibit(cookie, objectPtr->getCurrentlyProcessedMessage().getSender());
                  }),
              sdbus::registerSignal("ActiveChanged").withParameters<bool>("active")
          )
          .forInterface(kInterface);
      m_objects.push_back(std::move(object));
    } catch (const std::exception& e) {
      kLog.warn("failed to register ScreenSaver at {}: {}", std::string{path}, e.what());
    }
  }

  if (m_objects.empty()) {
    throw sdbus::Error(sdbus::Error::Name{kFileExistsError}, "no ScreenSaver object paths registered");
  }

  m_active = true;
  kLog.info("listening on org.freedesktop.ScreenSaver ({} object path(s))", m_objects.size());
}

void ScreenSaverService::registerLogindIdleMonitor(SystemBus* systemBus) {
  if (systemBus == nullptr) {
    return;
  }
  if (!systemBus->nameHasOwner("org.freedesktop.login1")) {
    kLog.debug("logind unavailable; systemd idle inhibit monitor disabled");
    return;
  }

  try {
    m_logindProxy = sdbus::createProxy(systemBus->connection(), kLogindBusName, kLogindObjectPath);
    m_logindProxy->uponSignal("PropertiesChanged")
        .onInterface(kPropertiesInterface)
        .call([this](
                  const std::string& interfaceName, const std::map<std::string, sdbus::Variant>& changedProperties,
                  const std::vector<std::string>& /*invalidatedProperties*/
              ) {
          if (interfaceName != kLogindManagerInterface) {
            return;
          }
          if (!changedProperties.contains("BlockInhibited")) {
            return;
          }
          // Any peer can send this signal directly to us, so read the value from logind instead.
          applyLogindBlockInhibited(
              m_logindProxy->getProperty("BlockInhibited").onInterface(kLogindManagerInterface).get<std::string>()
          );
        });

    const auto blockInhibited =
        m_logindProxy->getProperty("BlockInhibited").onInterface(kLogindManagerInterface).get<std::string>();
    applyLogindBlockInhibited(blockInhibited);
    kLog.info("logind idle inhibit monitor active");
  } catch (const std::exception& e) {
    kLog.warn("logind idle inhibit monitor disabled: {}", e.what());
    m_logindProxy.reset();
  }
}

void ScreenSaverService::applyLogindBlockInhibited(const std::string& inhibits) {
  const std::string tagged = ":" + inhibits + ":";
  const bool idleBlocked = tagged.contains(":idle:");
  if (idleBlocked && !m_logindIdleInhibited) {
    m_logindIdleInhibited = true;
    recomputeLocks();
    kLog.info("systemd idle inhibit active");
  } else if (!idleBlocked && m_logindIdleInhibited) {
    m_logindIdleInhibited = false;
    recomputeLocks();
    kLog.info("systemd idle inhibit released");
  }
}

namespace {

  std::string lowerCopy(std::string s) {
    std::ranges::transform(s, s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
  }

} // namespace

std::string ScreenSaverService::senderComm(const char* sender) const {
  if (sender == nullptr || m_dbusProxy == nullptr) {
    return {};
  }
  try {
    std::uint32_t pid = 0;
    m_dbusProxy->callMethod("GetConnectionUnixProcessID")
        .onInterface("org.freedesktop.DBus")
        .withArguments(std::string(sender))
        .storeResultsTo(pid);
    std::ifstream in("/proc/" + std::to_string(pid) + "/comm");
    std::string comm;
    std::getline(in, comm);
    return comm;
  } catch (const std::exception& e) {
    kLog.debug("sender pid lookup failed for {}: {}", sender, e.what());
    return {};
  }
}

bool ScreenSaverService::matchesIgnore(const std::string& app, const std::string& comm) const {
  const std::string a = lowerCopy(app);
  const std::string c = lowerCopy(comm);
  return std::ranges::any_of(m_ignore, [&](const std::string& entry) {
    const std::string e = lowerCopy(entry);
    return !e.empty() && (a.contains(e) || c.contains(e));
  });
}

void ScreenSaverService::setIgnoreList(std::vector<std::string> ignore) {
  m_ignore = std::move(ignore);
  for (auto& c : m_cookies) {
    c.ignored = matchesIgnore(c.app, c.comm);
  }
  recomputeLocks();
}

std::uint32_t ScreenSaverService::onInhibit(std::string app, std::string reason, const char* sender) {
  std::string comm = senderComm(sender);
  const bool ignored = matchesIgnore(app, comm);
  kLog.info("inhibit app='{}' comm='{}' reason='{}' ignored={}", app, comm, reason, ignored);

  const auto cookie = m_nextCookieId++;
  m_cookies.push_back(
      InhibitCookie{
          .cookie = cookie,
          .app = std::move(app),
          .reason = std::move(reason),
          .ownerId = sender != nullptr ? std::string(sender) : std::string{},
          .comm = std::move(comm),
          .ignored = ignored,
      }
  );
  recomputeLocks();
  return cookie;
}

void ScreenSaverService::onUninhibit(std::uint32_t cookie, const char* sender) {
  const auto it = std::ranges::find(m_cookies, cookie, &InhibitCookie::cookie);
  if (it == m_cookies.end()) {
    kLog.warn("screensaver uninhibit: unknown cookie {} from {}", cookie, sender != nullptr ? sender : "?");
    return;
  }

  const InhibitCookie released = std::move(*it);
  m_cookies.erase(it);
  kLog.info(
      "uninhibit app='{}' comm='{}' reason='{}' ignored={}", released.app, released.comm, released.reason,
      released.ignored
  );
  recomputeLocks();
}

void ScreenSaverService::recomputeLocks() {
  const auto counted = std::ranges::count(m_cookies, false, &InhibitCookie::ignored);
  const std::int64_t next = static_cast<std::int64_t>(counted) + (m_logindIdleInhibited ? 1 : 0);
  if (next == m_inhibitLocks) {
    return;
  }
  m_inhibitLocks = next;
  if (m_changeCallback) {
    m_changeCallback(m_inhibitLocks);
  }
}

std::size_t ScreenSaverService::unregisterOwnerCookies(const std::string& ownerId) {
  if (ownerId.empty()) {
    return 0;
  }

  std::size_t removed = 0;
  for (auto it = m_cookies.begin(); it != m_cookies.end();) {
    if (it->ownerId != ownerId) {
      ++it;
      continue;
    }
    ++removed;
    kLog.info(
        "uninhibit app='{}' comm='{}' reason='{}' ignored={} (client gone)", it->app, it->comm, it->reason, it->ignored
    );
    it = m_cookies.erase(it);
  }
  recomputeLocks();
  return removed;
}
