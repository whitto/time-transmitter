#ifndef RADIO_WIFI_ACCESS_WINDOW_H
#define RADIO_WIFI_ACCESS_WINDOW_H

// Daily web-access windows use civil minutes in their independently selected
// timezone. They recur every day; no completion state or flash writes are
// needed at a window boundary. Overnight windows cross local midnight.
namespace RadioWifiAccessWindow {

inline bool validMinutes(int start, int end) {
  return start >= 0 && start < 1440 && end >= 0 && end < 1440 && start != end;
}

inline bool containsMinute(int current, int start, int end) {
  if (current < 0 || current >= 1440 || !validMinutes(start, end)) return false;
  return start < end ? current >= start && current < end
                     : current >= start || current < end;
}

}  // namespace RadioWifiAccessWindow

#endif
