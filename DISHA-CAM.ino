/*
  DISHA-CAM - camera firmware for DISHA GLASS OS
  ESP32-S3-WROOM CAM board (ESP32-S3-N16R8), GC2145 camera ("RHYX M21-45")

  The camera talks to the Android phone over the board's native USB port ("USB-OTG")
  with a custom bulk protocol (docs/PROTOCOL.md). Bluetooth stays off. Wi-Fi stays off too
  unless the user switches on the optional Wi-Fi web mode (hold BOOT 5 s, or "wifi on" on the
  serial console): then the live view and a Disha page also open in a browser, and the RGB LED
  blinks white. The "USB-UART" port is used for flashing and the debug log only.

  Arduino IDE 2.x, boards package "esp32 by Espressif Systems" 3.3.12, Tools menu:
    Board            : ESP32S3 Dev Module
    USB Mode         : USB-OTG (TinyUSB)          <- primary transport
                       (Hardware CDC and JTAG     <- fallback transport, same app)
    USB CDC On Boot  : Disabled                   <- Serial must stay on the USB-UART port
    PSRAM            : OPI PSRAM                  <- required
    Flash Size       : 16MB (128Mb)
    Partition Scheme : 16M Flash (3MB APP/9.9MB FATFS)
    CPU Frequency    : 240MHz (WiFi)
    Flash Mode       : QIO 80MHz
    Upload Mode      : UART0 / Hardware CDC
    Core Debug Level : None
    Port             : the COM port of the "USB-UART" connector (CH343/CH340)

  Serial Monitor: 115200 baud, "Newline". Type "help".

  microSD (FAT32, never formatted by this firmware): photos /DISHA/IMG_nnnnn.jpg and MJPEG AVI
  videos /DISHA/VID_nnnnn.avi. BOOT button: short press = photo, hold 1.5-4 s and let go =
  start/stop video, hold 5 s = Wi-Fi web mode on/off (the board restarts).
  Wi-Fi network for web mode: src/config/disha_config.h (DISHA_WIFI_SSID / _PASSWORD).
*/

#ifndef BOARD_HAS_PSRAM
#error "Tools > PSRAM must be \"OPI PSRAM\": the camera frames live in the 8 MB PSRAM"
#endif
#if ARDUINO_USB_CDC_ON_BOOT
#error "Tools > USB CDC On Boot must be \"Disabled\": the native USB port carries the camera link"
#endif

#include "esp_task_wdt.h"
#include "src/config/disha_config.h"
#include "src/camera/camera_service.h"
#include "src/command/command_handler.h"
#include "src/diagnostics/log.h"
#include "src/diagnostics/serial_console.h"
#include "src/diagnostics/status_led.h"
#include "src/protocol/crc.h"
#include "src/protocol/protocol_io.h"
#include "src/status/status_report.h"
#include "src/storage/sd_storage.h"
#include "src/usb/usb_link.h"
#include "src/web/web_mode.h"

static void switchWifi(bool on) {
  WebMode::setEnabled(on);
  Log::printf("[wifi] web mode %s: restarting", on ? "ON" : "OFF");
  Serial.flush();
  delay(200);
  ESP.restart();
}

// BOOT button: short press = photo to microSD; hold 1.5-4 s and let go = start/stop microSD
// video; hold 5 s = Wi-Fi web mode on/off.
static void pollBootButton() {
  static bool down = false;
  static bool wifiDone = false;
  static uint32_t downAt = 0;
  static uint32_t lastChange = 0;
  const bool pressed = digitalRead(Pins::BOOT_BTN) == LOW;
  const uint32_t now = millis();
  if (pressed != down && now - lastChange > 40) {  // debounce
    lastChange = now;
    down = pressed;
    if (pressed) {
      downAt = now;
      wifiDone = false;
    } else if (!wifiDone) {
      const uint32_t held = now - downAt;
      if (held < 1000) {
        Log::printf("[btn] photo");
        CameraService::requestPhoto(0, 0, true);
      } else if (held >= 1500) {
        Log::printf("[btn] start/stop video");
        CameraService::toggleRecordingLocal();
      }
    }
  }
  if (down && !wifiDone && now - downAt >= Cfg::BOOT_HOLD_WIFI_MS) {
    wifiDone = true;
    switchWifi(!WebMode::enabled());
  }
}

static void configureWatchdog() {
  // A hung camera or link task (subscribed in their task functions) panics and reboots the board.
  esp_task_wdt_config_t cfg = {};
  cfg.timeout_ms = Cfg::WATCHDOG_TIMEOUT_MS;
  cfg.idle_core_mask = (1 << 0);
  cfg.trigger_panic = true;
  esp_err_t err = esp_task_wdt_reconfigure(&cfg);
  if (err == ESP_ERR_INVALID_STATE) err = esp_task_wdt_init(&cfg);
  if (err != ESP_OK) Log::printf("[wdt] watchdog setup failed (0x%x)", (unsigned)err);
}

static void printSummary() {
  CameraService::Info c;
  CameraService::getInfo(c);
  char fps[24] = "";
  if (c.streaming) snprintf(fps, sizeof(fps), ", %.1f fps", c.fpsActual);
  const SdStorage::Status sd = SdStorage::status();
  char card[48];
  if (sd.mounted) {
    snprintf(card, sizeof(card), "%u MB free%s", (unsigned)((sd.totalBytes - sd.usedBytes) >> 20),
             sd.recording ? ", REC" : "");
  } else {
    strlcpy(card, "no card", sizeof(card));
  }
  Log::printf("[stat] camera %s %s %ux%u | usb %s, session %s%s | sent %u frames | SD %s | heap %u KB, psram %u KB | %.1f C",
              CameraService::stateName(c.state), c.sensorName, c.width, c.height,
              UsbLink::mounted() ? "attached" : "no host", CommandHandler::sessionActive() ? "active" : "none",
              c.streaming ? fps : "", (unsigned)c.framesSent, card, (unsigned)(ESP.getFreeHeap() >> 10),
              (unsigned)(ESP.getFreePsram() >> 10), temperatureRead());
}

void setup() {
  Log::begin(Cfg::SERIAL_BAUD);
  StatusLed::begin();
  pinMode(Pins::BOOT_BTN, INPUT_PULLUP);

  Log::printf("\n===== %s %s =====", DISHA_FW_NAME, DISHA_FW_VERSION);
  Log::printf("chip %s rev %u, %u MHz, flash %u MB, PSRAM %u KB, reset reason: %s", ESP.getChipModel(),
              (unsigned)ESP.getChipRevision(), (unsigned)ESP.getCpuFreqMHz(), (unsigned)(ESP.getFlashChipSize() >> 20),
              (unsigned)(ESP.getPsramSize() >> 10), StatusReport::resetReason());
  if ((ESP.getFlashChipSize() >> 20) != 16) Log::printf("[!] expected 16 MB flash: check Tools > Flash Size");
  if (!psramFound()) Log::printf("[!] no PSRAM: set Tools > PSRAM to \"OPI PSRAM\"");
  Log::printf("CRC self-test: %s, protocol self-test: %s", Crc::selfTest() ? "OK" : "FAILED",
              ProtocolIO::selfTest() ? "OK" : "FAILED");

  configureWatchdog();
  ProtocolIO::begin();

  // Native USB first, so the phone sees the device while the camera is still starting.
  if (UsbLink::begin()) {
    Log::printf("[usb] native USB started: %s on the USB-OTG port", UsbLink::transportName());
  } else {
    Log::printf("[usb] could not start native USB (%s)", UsbLink::transportName());
  }
  if (!CommandHandler::begin()) Log::printf("[!] could not start the link task");
  if (!CameraService::begin()) Log::printf("[!] could not start the camera task");
  if (WebMode::enabled()) {
    WebMode::begin();
  } else {
    Log::printf("Wi-Fi and Bluetooth are off (web mode: hold BOOT 5 s or type \"wifi on\").");
  }
  Log::printf("Type \"help\" for serial commands.");
}

void loop() {
  static uint32_t lastSummary = 0;

  SerialConsole::poll();
  pollBootButton();
  WebMode::poll();

  CameraService::Info c;
  CameraService::getInfo(c);
  StatusLed::Mode mode;
  if (c.state == CameraService::State::Error) mode = StatusLed::Mode::CameraError;
  else if (c.state != CameraService::State::Ready) mode = StatusLed::Mode::Boot;
  else if (c.recording) mode = StatusLed::Mode::Recording;
  else if (c.streaming) mode = StatusLed::Mode::Streaming;
  else if (CommandHandler::sessionActive()) mode = StatusLed::Mode::Connected;
  else mode = StatusLed::Mode::WaitingForPhone;
  const bool framesLeaving = c.streaming || c.recording || (c.lastFrameSentAt && millis() - c.lastFrameSentAt < 400);
  const bool wifiBlink = WebMode::active() && millis() % 2000 < 150;  // Wi-Fi web mode is on
  StatusLed::show(mode, framesLeaving, wifiBlink);

  if (millis() - lastSummary > 10000) {
    lastSummary = millis();
    printSummary();
  }
  delay(20);
}
