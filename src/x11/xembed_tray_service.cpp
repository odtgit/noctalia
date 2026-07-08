#include "x11/xembed_tray_service.h"

#include "core/log.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <memory>

namespace {

  constexpr Logger kLog("xembed");

  // freedesktop system tray spec, _NET_SYSTEM_TRAY_OPCODE message data32[1].
  constexpr std::uint32_t kSystemTrayRequestDock = 0;
  // XEmbed spec, message code sent after an icon is embedded.
  constexpr std::uint32_t kXEmbedEmbeddedNotify = 0;
  constexpr std::uint32_t kXEmbedVersion = 0;

  // Reconnect cadence when $DISPLAY is set but the X server is unreachable
  // (or the selection is owned elsewhere).
  constexpr auto kRetryInterval = std::chrono::seconds(30);

  // X core protocol button state masks for synthesized release events.
  constexpr std::uint16_t kButton1Mask = 0x100;
  constexpr std::uint16_t kButton3Mask = 0x400;

  struct FreeDeleter {
    void operator()(void* p) const { std::free(p); }
  };
  template <typename T> using XcbReply = std::unique_ptr<T, FreeDeleter>;

  XcbReply<xcb_get_property_reply_t>
  getProperty(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t type, std::uint32_t words) {
    return XcbReply<xcb_get_property_reply_t>(
        xcb_get_property_reply(conn, xcb_get_property(conn, 0, window, property, type, 0, words), nullptr)
    );
  }

  std::string propertyString(xcb_connection_t* conn, xcb_window_t window, xcb_atom_t property, xcb_atom_t type) {
    const auto reply = getProperty(conn, window, property, type, 256);
    if (reply == nullptr || xcb_get_property_value_length(reply.get()) <= 0) {
      return {};
    }
    return std::string(
        static_cast<const char*>(xcb_get_property_value(reply.get())),
        static_cast<std::size_t>(xcb_get_property_value_length(reply.get()))
    );
  }

  // _NET_WM_ICON: array of (width, height, width*height 0xAARRGGBB pixels)
  // blocks; pick the largest and convert to the A,R,G,B byte order used by
  // TrayItemInfo::iconArgb32 (PixmapFormat::ARGB).
  bool readNetWmIcon(
      xcb_connection_t* conn, xcb_window_t window, xcb_atom_t atomNetWmIcon, std::vector<std::uint8_t>& outArgb,
      std::int32_t& outW, std::int32_t& outH
  ) {
    outArgb.clear();
    outW = 0;
    outH = 0;
    const auto reply = getProperty(conn, window, atomNetWmIcon, XCB_ATOM_CARDINAL, 1024 * 1024);
    if (reply == nullptr || reply->format != 32) {
      return false;
    }
    const auto* data = static_cast<const std::uint32_t*>(xcb_get_property_value(reply.get()));
    const auto count = static_cast<std::size_t>(xcb_get_property_value_length(reply.get())) / 4U;

    std::size_t bestOffset = 0;
    std::uint64_t bestArea = 0;
    std::uint32_t bestW = 0;
    std::uint32_t bestH = 0;
    for (std::size_t i = 0; i + 2 <= count;) {
      const std::uint32_t w = data[i];
      const std::uint32_t h = data[i + 1];
      const std::uint64_t area = static_cast<std::uint64_t>(w) * h;
      if (w == 0 || h == 0 || area > count - i - 2) {
        break;
      }
      if (area > bestArea) {
        bestArea = area;
        bestOffset = i + 2;
        bestW = w;
        bestH = h;
      }
      i += 2 + area;
    }
    if (bestArea == 0) {
      return false;
    }

    outArgb.resize(bestArea * 4U);
    for (std::size_t i = 0; i < bestArea; ++i) {
      const std::uint32_t px = data[bestOffset + i];
      outArgb[i * 4 + 0] = static_cast<std::uint8_t>(px >> 24); // A
      outArgb[i * 4 + 1] = static_cast<std::uint8_t>(px >> 16); // R
      outArgb[i * 4 + 2] = static_cast<std::uint8_t>(px >> 8);  // G
      outArgb[i * 4 + 3] = static_cast<std::uint8_t>(px);       // B
    }
    outW = static_cast<std::int32_t>(bestW);
    outH = static_cast<std::int32_t>(bestH);
    return true;
  }

} // namespace

XEmbedTrayService::~XEmbedTrayService() {
  // The change callback reaches into shell objects that may already be gone
  // during application teardown.
  m_changeCallback = nullptr;
  teardown();
}

xcb_atom_t XEmbedTrayService::internAtom(const char* name) {
  const XcbReply<xcb_intern_atom_reply_t> reply(xcb_intern_atom_reply(
      m_conn, xcb_intern_atom(m_conn, 0, static_cast<std::uint16_t>(std::strlen(name)), name), nullptr
  ));
  return reply != nullptr ? reply->atom : XCB_NONE;
}

void XEmbedTrayService::start() {
  if (m_conn != nullptr) {
    return;
  }
  m_startRequested = true;
  m_nextRetry = std::chrono::steady_clock::now() + kRetryInterval;
  const char* display = std::getenv("DISPLAY");
  if (display == nullptr || display[0] == '\0') {
    return;
  }

  int screenNum = 0;
  m_conn = xcb_connect(nullptr, &screenNum);
  if (xcb_connection_has_error(m_conn) != 0) {
    kLog.debug("xembed tray unavailable: cannot connect to X display {}", display);
    teardown();
    return;
  }

  const xcb_setup_t* setup = xcb_get_setup(m_conn);
  xcb_screen_iterator_t screenIt = xcb_setup_roots_iterator(setup);
  for (int i = 0; i < screenNum && screenIt.rem > 0; ++i) {
    xcb_screen_next(&screenIt);
  }
  if (screenIt.rem <= 0) {
    teardown();
    return;
  }
  const xcb_screen_t* screen = screenIt.data;
  m_root = screen->root;

  const std::string selectionName = std::format("_NET_SYSTEM_TRAY_S{}", screenNum);
  m_atomTraySelection = internAtom(selectionName.c_str());
  m_atomTrayOpcode = internAtom("_NET_SYSTEM_TRAY_OPCODE");
  m_atomTrayOrientation = internAtom("_NET_SYSTEM_TRAY_ORIENTATION");
  m_atomManager = internAtom("MANAGER");
  m_atomXEmbed = internAtom("_XEMBED");
  m_atomNetWmName = internAtom("_NET_WM_NAME");
  m_atomNetWmIcon = internAtom("_NET_WM_ICON");
  m_atomUtf8String = internAtom("UTF8_STRING");
  m_atomWineHwndStyle = internAtom("_WINE_HWND_STYLE");
  if (m_atomTraySelection == XCB_NONE || m_atomTrayOpcode == XCB_NONE || m_atomManager == XCB_NONE) {
    teardown();
    return;
  }

  {
    const XcbReply<xcb_get_selection_owner_reply_t> owner(
        xcb_get_selection_owner_reply(m_conn, xcb_get_selection_owner(m_conn, m_atomTraySelection), nullptr)
    );
    if (owner != nullptr && owner->owner != XCB_NONE) {
      kLog.info("xembed tray disabled: {} already owned by 0x{:x}", selectionName, owner->owner);
      teardown();
      return;
    }
  }

  m_host = xcb_generate_id(m_conn);
  const std::uint32_t eventMask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_SUBSTRUCTURE_NOTIFY;
  xcb_create_window(
      m_conn, XCB_COPY_FROM_PARENT, m_host, m_root, -1, -1, 1, 1, 0, XCB_WINDOW_CLASS_INPUT_OUTPUT,
      XCB_COPY_FROM_PARENT, XCB_CW_EVENT_MASK, &eventMask
  );
  const std::uint32_t orientationHorizontal = 0;
  xcb_change_property(
      m_conn, XCB_PROP_MODE_REPLACE, m_host, m_atomTrayOrientation, XCB_ATOM_CARDINAL, 32, 1, &orientationHorizontal
  );

  xcb_set_selection_owner(m_conn, m_host, m_atomTraySelection, XCB_CURRENT_TIME);
  {
    const XcbReply<xcb_get_selection_owner_reply_t> owner(
        xcb_get_selection_owner_reply(m_conn, xcb_get_selection_owner(m_conn, m_atomTraySelection), nullptr)
    );
    if (owner == nullptr || owner->owner != m_host) {
      kLog.info("xembed tray disabled: failed to acquire {}", selectionName);
      teardown();
      return;
    }
  }
  m_selectionOwned = true;

  // Announce the new tray manager so clients (re-)dock their icons.
  xcb_client_message_event_t manager{};
  manager.response_type = XCB_CLIENT_MESSAGE;
  manager.format = 32;
  manager.window = m_root;
  manager.type = m_atomManager;
  manager.data.data32[0] = XCB_CURRENT_TIME;
  manager.data.data32[1] = m_atomTraySelection;
  manager.data.data32[2] = m_host;
  xcb_send_event(m_conn, 0, m_root, XCB_EVENT_MASK_STRUCTURE_NOTIFY, reinterpret_cast<const char*>(&manager));
  xcb_flush(m_conn);

  kLog.info("xembed tray host active on {} ({})", display, selectionName);

  adoptOrphanedWineTrayWindows();
}

void XEmbedTrayService::adoptOrphanedWineTrayWindows() {
  // Wine only docks a tray icon at creation time: when no tray manager owns
  // the selection at that moment it maps a standalone tray-strip window on
  // the desktop instead, and current Wine/Proton builds never react to a
  // later MANAGER broadcast (winex11 only handles it on the desktop hwnd,
  // which rootless setups don't deliver root broadcasts to). Those strips
  // are what show up as tiny floating windows under Wayland compositors.
  // They are unmistakable - direct root children, viewable, not
  // override-redirect, Wine-owned (_WINE_HWND_STYLE), icon-bearing
  // (_NET_WM_ICON) and tray-strip sized - so adopt them like a dock
  // request. Wine treats clicks on the strip as tray icon clicks, which is
  // exactly what our forwarded button events deliver.
  const XcbReply<xcb_query_tree_reply_t> tree(xcb_query_tree_reply(m_conn, xcb_query_tree(m_conn, m_root), nullptr));
  if (tree == nullptr) {
    return;
  }

  auto hasProperty = [this](xcb_window_t window, xcb_atom_t property) {
    const auto reply = getProperty(m_conn, window, property, XCB_ATOM_ANY, 0);
    return reply != nullptr && reply->type != XCB_NONE;
  };

  const xcb_window_t* children = xcb_query_tree_children(tree.get());
  for (int i = 0; i < xcb_query_tree_children_length(tree.get()); ++i) {
    const xcb_window_t window = children[i];
    const XcbReply<xcb_get_window_attributes_reply_t> attributes(
        xcb_get_window_attributes_reply(m_conn, xcb_get_window_attributes(m_conn, window), nullptr)
    );
    if (attributes == nullptr
        || attributes->map_state != XCB_MAP_STATE_VIEWABLE
        || attributes->override_redirect != 0) {
      continue;
    }
    if (!hasProperty(window, m_atomWineHwndStyle) || !hasProperty(window, m_atomNetWmIcon)) {
      continue;
    }
    const XcbReply<xcb_get_geometry_reply_t> geometry(
        xcb_get_geometry_reply(m_conn, xcb_get_geometry(m_conn, window), nullptr)
    );
    if (geometry == nullptr || geometry->height > 48 || geometry->width > 1024 || geometry->width < 8) {
      continue;
    }
    kLog.info("adopting orphaned Wine tray window 0x{:x} ({}x{})", window, geometry->width, geometry->height);
    dockIcon(window);
  }
}

void XEmbedTrayService::teardown() {
  if (m_conn != nullptr) {
    // Let the host window die with the connection, destroying docked icon
    // windows with it. That DestroyNotify is the signal XEmbed clients (Wine
    // in particular) key their recovery on: they recreate the icon and dock
    // to whichever tray owns the selection next. Politely reparenting icons
    // back to root instead leaves clients believing they are still embedded,
    // and they then ignore the next MANAGER broadcast.
    xcb_disconnect(m_conn);
    m_conn = nullptr;
  }
  m_root = XCB_NONE;
  m_host = XCB_NONE;
  m_selectionOwned = false;
  if (!m_icons.empty()) {
    m_icons.clear();
    emitChanged();
  }
}

void XEmbedTrayService::emitChanged() {
  if (m_changeCallback) {
    m_changeCallback();
  }
}

std::vector<TrayItemInfo> XEmbedTrayService::items() const {
  std::vector<TrayItemInfo> out;
  out.reserve(m_icons.size());
  for (const auto& [_, icon] : m_icons) {
    out.push_back(icon.info);
  }
  return out;
}

xcb_window_t XEmbedTrayService::windowFromItemId(std::string_view itemId) const {
  if (!isXEmbedItemId(itemId)) {
    return XCB_NONE;
  }
  const auto hex = itemId.substr(kItemIdPrefix.size());
  const auto window = static_cast<xcb_window_t>(std::strtoul(std::string(hex).c_str(), nullptr, 16));
  return m_icons.contains(window) ? window : XCB_NONE;
}

bool XEmbedTrayService::activateItem(
    const std::string& itemId, std::int32_t x, std::int32_t y, wl_output* output, std::string_view barEdge
) {
  const xcb_window_t window = windowFromItemId(itemId);
  return window != XCB_NONE && sendClick(window, XCB_BUTTON_INDEX_1, mapClickToXRoot(x, y, output, barEdge));
}

bool XEmbedTrayService::openContextMenu(
    const std::string& itemId, std::int32_t x, std::int32_t y, wl_output* output, std::string_view barEdge
) {
  const xcb_window_t window = windowFromItemId(itemId);
  return window != XCB_NONE && sendClick(window, XCB_BUTTON_INDEX_3, mapClickToXRoot(x, y, output, barEdge));
}

std::optional<std::pair<std::int16_t, std::int16_t>>
XEmbedTrayService::mapClickToXRoot(std::int32_t x, std::int32_t y, wl_output* output, std::string_view barEdge) const {
  if (!m_outputResolver || output == nullptr) {
    return std::nullopt;
  }
  const auto geometry = m_outputResolver(output);
  if (!geometry.has_value() || geometry->logicalWidth <= 0 || geometry->logicalHeight <= 0) {
    return std::nullopt;
  }

  // The click coordinates are local to the bar surface, which spans its
  // output along the bar edge. Along the bar they track the output; across
  // the bar they are only a few pixels, so snap the cross-axis to the edge
  // the bar sits on. The result only anchors context menus, so edge-of-bar
  // precision is enough.
  double lx = std::clamp<double>(x, 0.0, geometry->logicalWidth - 1);
  double ly = std::clamp<double>(y, 0.0, geometry->logicalHeight - 1);
  if (barEdge == "bottom") {
    ly = geometry->logicalHeight - 1;
  } else if (barEdge == "right") {
    lx = geometry->logicalWidth - 1;
  }

  const double rootX = geometry->logicalX + lx * geometry->scale;
  const double rootY = geometry->logicalY + ly * geometry->scale;
  auto toInt16 = [](double value) {
    return static_cast<std::int16_t>(std::clamp<double>(value, INT16_MIN, INT16_MAX));
  };
  return std::pair{toInt16(rootX), toInt16(rootY)};
}

bool XEmbedTrayService::sendClick(
    xcb_window_t window, std::uint8_t button, std::optional<std::pair<std::int16_t, std::int16_t>> rootPos
) {
  if (!active()) {
    return false;
  }

  std::int16_t rootX = 0;
  std::int16_t rootY = 0;
  if (rootPos.has_value()) {
    rootX = rootPos->first;
    rootY = rootPos->second;
  } else {
    // Without a mapped click position, fall back to wherever the X pointer
    // last was. Wine-style clients update their cursor belief from the
    // event's root coordinates and pop context menus there.
    const XcbReply<xcb_query_pointer_reply_t> pointer(
        xcb_query_pointer_reply(m_conn, xcb_query_pointer(m_conn, m_root), nullptr)
    );
    if (pointer != nullptr) {
      rootX = pointer->root_x;
      rootY = pointer->root_y;
    }
  }

  xcb_button_press_event_t press{};
  press.response_type = XCB_BUTTON_PRESS;
  press.detail = button;
  press.time = XCB_CURRENT_TIME;
  press.root = m_root;
  press.event = window;
  press.child = XCB_NONE;
  press.root_x = rootX;
  press.root_y = rootY;
  press.event_x = 1;
  press.event_y = 1;
  press.same_screen = 1;
  xcb_send_event(m_conn, 0, window, XCB_EVENT_MASK_BUTTON_PRESS, reinterpret_cast<const char*>(&press));

  xcb_button_release_event_t release{};
  std::memcpy(&release, &press, sizeof(release));
  release.response_type = XCB_BUTTON_RELEASE;
  release.state = button == XCB_BUTTON_INDEX_1 ? kButton1Mask : kButton3Mask;
  xcb_send_event(m_conn, 0, window, XCB_EVENT_MASK_BUTTON_RELEASE, reinterpret_cast<const char*>(&release));
  xcb_flush(m_conn);
  kLog.debug("forwarded button {} click to xembed icon 0x{:x}", button, window);
  return true;
}

int XEmbedTrayService::pollTimeoutMs() const {
  if (!m_startRequested || active()) {
    return -1;
  }
  // Disconnected (no X server yet, it went away, or the selection was owned
  // elsewhere): retry on a slow cadence.
  const auto remaining =
      std::chrono::duration_cast<std::chrono::milliseconds>(m_nextRetry - std::chrono::steady_clock::now()).count();
  return static_cast<int>(std::clamp<long long>(remaining, 0, std::chrono::milliseconds(kRetryInterval).count()));
}

void XEmbedTrayService::doAddPollFds(std::vector<pollfd>& fds) {
  if (m_conn != nullptr) {
    fds.push_back({.fd = xcb_get_file_descriptor(m_conn), .events = POLLIN, .revents = 0});
  }
}

void XEmbedTrayService::dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) {
  (void)fds;
  (void)startIdx;
  if (m_conn == nullptr || !m_selectionOwned) {
    // Retry deadline elapsed.
    if (m_startRequested && std::chrono::steady_clock::now() >= m_nextRetry) {
      teardown();
      start();
    }
    return;
  }
  processEvents();
}

void XEmbedTrayService::processEvents() {
  bool changed = false;
  while (xcb_generic_event_t* raw = xcb_poll_for_event(m_conn)) {
    const XcbReply<xcb_generic_event_t> event(raw);
    switch (event->response_type & 0x7F) {
    case XCB_CLIENT_MESSAGE:
      handleClientMessage(*reinterpret_cast<const xcb_client_message_event_t*>(event.get()));
      break;
    case XCB_DESTROY_NOTIFY: {
      const auto& destroy = *reinterpret_cast<const xcb_destroy_notify_event_t*>(event.get());
      removeIcon(destroy.window);
      break;
    }
    case XCB_REPARENT_NOTIFY: {
      const auto& reparent = *reinterpret_cast<const xcb_reparent_notify_event_t*>(event.get());
      if (reparent.parent != m_host) {
        removeIcon(reparent.window);
      }
      break;
    }
    case XCB_PROPERTY_NOTIFY: {
      const auto& property = *reinterpret_cast<const xcb_property_notify_event_t*>(event.get());
      if (m_icons.contains(property.window)
          && (property.atom == XCB_ATOM_WM_NAME
              || property.atom == XCB_ATOM_WM_CLASS
              || property.atom == m_atomNetWmName
              || property.atom == m_atomNetWmIcon)) {
        refreshIconMetadata(property.window);
        changed = true;
      }
      break;
    }
    case XCB_SELECTION_CLEAR: {
      const auto& clear = *reinterpret_cast<const xcb_selection_clear_event_t*>(event.get());
      if (clear.selection == m_atomTraySelection) {
        kLog.info("xembed tray selection taken over by another manager");
        teardown();
        return;
      }
      break;
    }
    default:
      break;
    }
  }

  if (xcb_connection_has_error(m_conn) != 0) {
    kLog.info("xembed tray X connection lost");
    teardown();
    return;
  }
  if (changed) {
    emitChanged();
  }
}

void XEmbedTrayService::handleClientMessage(const xcb_client_message_event_t& event) {
  if (event.type != m_atomTrayOpcode || event.format != 32) {
    return;
  }
  if (event.data.data32[1] == kSystemTrayRequestDock) {
    dockIcon(static_cast<xcb_window_t>(event.data.data32[2]));
  }
  // Balloon message opcodes (1, 2) are intentionally unsupported.
}

void XEmbedTrayService::dockIcon(xcb_window_t window) {
  if (window == XCB_NONE || m_icons.contains(window)) {
    return;
  }

  // Validate the window and subscribe to its lifetime/metadata before
  // reparenting; a checked request surfaces already-destroyed windows.
  const std::uint32_t eventMask = XCB_EVENT_MASK_PROPERTY_CHANGE | XCB_EVENT_MASK_STRUCTURE_NOTIFY;
  const xcb_void_cookie_t cookie = xcb_change_window_attributes_checked(m_conn, window, XCB_CW_EVENT_MASK, &eventMask);
  if (const XcbReply<xcb_generic_error_t> error(xcb_request_check(m_conn, cookie)); error != nullptr) {
    kLog.debug("xembed dock request for dead window 0x{:x}", window);
    return;
  }

  // Park the icon under the host: reparenting pulls it out of the
  // compositor's toplevel set (this is what removes Wine's stray floating
  // tray window). Map it as the XEmbed spec asks — the host itself is never
  // mapped, so the icon stays unviewable and nothing renders on screen, but
  // clients that verify their icon got mapped after docking stay happy.
  xcb_reparent_window(m_conn, window, m_host, 0, 0);
  xcb_map_window(m_conn, window);

  xcb_client_message_event_t notify{};
  notify.response_type = XCB_CLIENT_MESSAGE;
  notify.format = 32;
  notify.window = window;
  notify.type = m_atomXEmbed;
  notify.data.data32[0] = XCB_CURRENT_TIME;
  notify.data.data32[1] = kXEmbedEmbeddedNotify;
  notify.data.data32[3] = m_host;
  notify.data.data32[4] = kXEmbedVersion;
  xcb_send_event(m_conn, 0, window, XCB_EVENT_MASK_NO_EVENT, reinterpret_cast<const char*>(&notify));
  xcb_flush(m_conn);

  m_icons.emplace(window, Icon{});
  refreshIconMetadata(window);
  kLog.info("xembed icon docked window=0x{:x} title='{}'", window, m_icons.at(window).info.title);
  emitChanged();
}

void XEmbedTrayService::removeIcon(xcb_window_t window) {
  if (m_icons.erase(window) > 0) {
    kLog.info("xembed icon removed window=0x{:x}", window);
    emitChanged();
  }
}

void XEmbedTrayService::refreshIconMetadata(xcb_window_t window) {
  const auto it = m_icons.find(window);
  if (it == m_icons.end()) {
    return;
  }
  TrayItemInfo& info = it->second.info;
  info.id = std::format("{}0x{:x}", kItemIdPrefix, window);

  std::string title = propertyString(m_conn, window, m_atomNetWmName, m_atomUtf8String);
  if (title.empty()) {
    title = propertyString(m_conn, window, XCB_ATOM_WM_NAME, XCB_ATOM_ANY);
  }

  // WM_CLASS is two NUL-terminated strings: instance, class.
  const std::string wmClass = propertyString(m_conn, window, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
  std::string instance = wmClass.substr(0, wmClass.find('\0'));
  std::string className;
  if (const auto nul = wmClass.find('\0'); nul != std::string::npos && nul + 1 < wmClass.size()) {
    className = wmClass.substr(nul + 1, wmClass.find('\0', nul + 1) - (nul + 1));
  }

  info.itemName = instance;
  info.processName = className;
  info.title = !title.empty() ? title : (!instance.empty() ? instance : info.id);
  info.statusNotifierTitle = title;
  info.status = "Active";
  readNetWmIcon(m_conn, window, m_atomNetWmIcon, info.iconArgb32, info.iconWidth, info.iconHeight);
}
