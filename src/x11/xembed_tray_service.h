#pragma once

#include "app/poll_source.h"
#include "dbus/tray/tray_service.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>
#include <xcb/xcb.h>

// XEmbed system tray host for legacy X11 tray icons (Wine/Proton apps, old
// toolkits). Owns the _NET_SYSTEM_TRAY_S<n> selection on the X display named
// by $DISPLAY, accepts SYSTEM_TRAY_REQUEST_DOCK, and surfaces docked icons as
// TrayItemInfo entries merged into TrayService::items().
//
// Icon windows are reparented under an unmapped host window and never mapped:
// their pixels are read from the _NET_WM_ICON property rather than composited,
// and clicks are forwarded as synthetic X button events. This sidesteps
// XComposite entirely, which matters under rootless Xwayland where any mapped
// X toplevel would surface as a compositor window.
class XEmbedTrayService : public PollSource {
public:
  using ChangeCallback = std::function<void()>;

  XEmbedTrayService() = default;
  ~XEmbedTrayService() override;
  XEmbedTrayService(const XEmbedTrayService&) = delete;
  XEmbedTrayService& operator=(const XEmbedTrayService&) = delete;

  // Connects to $DISPLAY and acquires the tray selection. Safe to call when no
  // X display exists (stays inactive) or another tray owns the selection.
  void start();
  [[nodiscard]] bool active() const noexcept { return m_conn != nullptr && m_selectionOwned; }
  void setChangeCallback(ChangeCallback callback) { m_changeCallback = std::move(callback); }

  [[nodiscard]] static bool isXEmbedItemId(std::string_view itemId) { return itemId.starts_with(kItemIdPrefix); }
  [[nodiscard]] std::size_t itemCount() const noexcept { return m_icons.size(); }
  [[nodiscard]] std::vector<TrayItemInfo> items() const;
  bool activateItem(const std::string& itemId);
  bool openContextMenu(const std::string& itemId);

  // PollSource
  [[nodiscard]] int pollTimeoutMs() const override;
  void dispatch(const std::vector<pollfd>& fds, std::size_t startIdx) override;

protected:
  void doAddPollFds(std::vector<pollfd>& fds) override;

private:
  static constexpr std::string_view kItemIdPrefix = "xembed:";

  struct Icon {
    TrayItemInfo info;
  };

  void teardown();
  void processEvents();
  void handleClientMessage(const xcb_client_message_event_t& event);
  void dockIcon(xcb_window_t window);
  void removeIcon(xcb_window_t window);
  void refreshIconMetadata(xcb_window_t window);
  [[nodiscard]] xcb_window_t windowFromItemId(std::string_view itemId) const;
  bool sendClick(xcb_window_t window, std::uint8_t button);
  [[nodiscard]] xcb_atom_t internAtom(const char* name);
  void emitChanged();

  xcb_connection_t* m_conn = nullptr;
  xcb_window_t m_root = XCB_NONE;
  xcb_window_t m_host = XCB_NONE;
  bool m_selectionOwned = false;
  bool m_startRequested = false;
  std::chrono::steady_clock::time_point m_nextRetry;

  xcb_atom_t m_atomTraySelection = XCB_NONE;
  xcb_atom_t m_atomTrayOpcode = XCB_NONE;
  xcb_atom_t m_atomTrayOrientation = XCB_NONE;
  xcb_atom_t m_atomManager = XCB_NONE;
  xcb_atom_t m_atomXEmbed = XCB_NONE;
  xcb_atom_t m_atomNetWmName = XCB_NONE;
  xcb_atom_t m_atomNetWmIcon = XCB_NONE;
  xcb_atom_t m_atomUtf8String = XCB_NONE;

  std::map<xcb_window_t, Icon> m_icons;
  ChangeCallback m_changeCallback;
};
