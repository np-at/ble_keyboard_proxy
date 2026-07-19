#include "ble_hid_sink.h"

BleHidSink::BleHidSink(const char *deviceName, const char *manufacturer)
    : kb_(deviceName, manufacturer, 100) {}

void BleHidSink::begin() { kb_.begin(); }

bool BleHidSink::isConnected() { return kb_.isConnected(); }

void BleHidSink::sendKey(const HidReport &report) {
  if (!kb_.isConnected()) return;

  KeyReport hid;
  hid.modifiers = report.modifiers;
  hid.reserved = 0;
  memcpy(hid.keys, report.keys, sizeof(hid.keys));

  kb_.sendReport(&hid);
}

void BleHidSink::sendConsumer(const ConsumerReport &report) {
  if (!kb_.isConnected()) return;

  MediaKeyReport media;
  media[0] = report.bytes[0];
  media[1] = report.bytes[1];

  kb_.sendReport(&media);
}

void BleHidSink::releaseAll() {
  if (kb_.isConnected()) kb_.releaseAll();
}

bool BleHidSink::poll() {
  // No held-key state is tracked here on purpose. Reports are full state, so the
  // producer always sends the complete current picture and a stale shadow copy
  // would only be a second source of truth to keep in sync. A host that drops
  // mid-chord simply sees no further reports; on reconnect the next report
  // re-establishes the full state.
  const bool connected = kb_.isConnected();
  const bool changed = (connected != wasConnected_);
  wasConnected_ = connected;
  return changed;
}
