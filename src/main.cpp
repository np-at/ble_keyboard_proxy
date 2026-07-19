// BLE Keyboard Proxy — ESP32-S3-DevKitC-1-N16R8
//
//   computer --USB CDC serial--> ESP32-S3 --BLE HID--> phone/tablet
//
// Milestone 1: drive it by hand from the serial monitor. See serial_proto.h for the
// wire format. Milestone 2 replaces the human at the keyboard with a macOS CGEventTap
// daemon emitting the same lines.

#include <Arduino.h>

#include "ble_hid_sink.h"
#include "serial_proto.h"

namespace {

constexpr char kDeviceName[] = "Proxy Keyboard";
constexpr char kManufacturer[] = "ble_keyboard_proxy";

BleHidSink sink(kDeviceName, kManufacturer);
SerialProto proto;

void handle(const Command &cmd) {
  switch (cmd.kind) {
    // "SENT" deliberately, not "OK". HID input reports are fire-and-forget GATT
    // notifications: the firmware can confirm it handed the report to the BLE stack
    // and nothing more. Saying "OK" would imply the host received and applied the
    // keystroke, which we cannot know — and that false signal is exactly what made a
    // totally dead BLE link look healthy from this side for several rounds of
    // debugging. Callers that need real confirmation must observe the target device.
    case CommandKind::Key:
      sink.sendKey(cmd.key);
      Serial.println(sink.isConnected() ? "SENT" : "ERR not connected");
      break;

    case CommandKind::Consumer:
      sink.sendConsumer(cmd.consumer);
      Serial.println(sink.isConnected() ? "SENT" : "ERR not connected");
      break;

    case CommandKind::ReleaseAll:
      sink.releaseAll();
      Serial.println(sink.isConnected() ? "SENT" : "ERR not connected");
      break;

    // "connected" here means a BLE link exists, NOT that the peer subscribed to the
    // HID report characteristic. Those came apart badly under Bluedroid.
    case CommandKind::Ping:
      Serial.printf("OK %s\n", sink.isConnected() ? "connected" : "advertising");
      break;

    case CommandKind::Error:
      Serial.printf("ERR %s\n", cmd.error ? cmd.error : "parse error");
      break;

    case CommandKind::None:
      break;
  }
}

}  // namespace

void setup() {
  Serial.begin(115200);

  // Give the USB CDC a moment to enumerate so the banner isn't lost, but don't
  // block forever — the board must still run when nothing is attached.
  const unsigned long start = millis();
  while (!Serial && millis() - start < 2000) {
    delay(10);
  }

  Serial.println();
  Serial.printf("ble_keyboard_proxy — advertising as \"%s\"\n", kDeviceName);
  Serial.println("commands: K <mod> <k1..k6> | C <b0> <b1> | R | P");

  sink.begin();
}

void loop() {
  if (sink.poll()) {
    Serial.printf("[ble] %s\n", sink.isConnected() ? "connected" : "disconnected");
  }

  while (Serial.available() > 0) {
    const Command cmd = proto.feed((char)Serial.read());
    if (cmd.kind != CommandKind::None) handle(cmd);
  }

  delay(1);
}
