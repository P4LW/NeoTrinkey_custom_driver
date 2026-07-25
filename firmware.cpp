#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>
#include <Adafruit_FreeTouch.h>

/* PIN_NEOPIXEL and PIN_TOUCH are defined by the board variant
 * (neokeytrinkey_m0/variant.h). The SAMD21 has no hardware touch
 * controller, so capacitive sensing is done via Adafruit_FreeTouch
 * (software oversampling), not touchRead().
 */
Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);
Adafruit_FreeTouch qt = Adafruit_FreeTouch(PIN_TOUCH, OVERSAMPLE_4, RESISTOR_50K, FREQ_MODE_NONE);

/* Vendor-defined HID report descriptor.
 * One interrupt IN endpoint, 1-byte report: 0 = released, 1 = pressed.
 */
uint8_t const desc_hid_report[] = {
  0x06, 0x00, 0xFF,
  0x09, 0x01,
  0xA1, 0x01,
  0x09, 0x02,
  0x15, 0x00,
  0x25, 0x01,
  0x75, 0x08,
  0x95, 0x01,
  0x81, 0x02,
  0xC0
};

Adafruit_USBD_HID usb_hid;

/* Shared state between the vendor control callback and the main loop. */
uint8_t led_colors[3]    = {0, 0, 0};
uint8_t touch_state      = 0;
uint8_t touch_state_prev = 0xFF; /* forces a report to be sent on first loop() */

// Vendor control transfer callback — handles CMD_SET_LED (0x01) only.
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                           uint8_t stage,
                                           tusb_control_request_t const *request)
{
  if (stage == CONTROL_STAGE_SETUP) {
    if (request->bRequest == 0x01)
      return tud_control_xfer(rhport, request, led_colors, 3);
  }
  if (stage == CONTROL_STAGE_DATA && request->bRequest == 0x01) {
    pixel.setPixelColor(0, led_colors[0], led_colors[1], led_colors[2]);
    pixel.show();
  }
  return true;
}

void setup()
{
  usb_hid.setPollInterval(2);
  usb_hid.setReportDescriptor(desc_hid_report, sizeof(desc_hid_report));
  usb_hid.begin();

  pixel.begin();
  pixel.show();

  if (!qt.begin()) {
    /* No serial console available on this configuration —
     * blink red to signal a touch initialisation failure.
     */
    pixel.setPixelColor(0, 255, 0, 0);
    pixel.show();
  }

  while (!TinyUSBDevice.mounted())
    delay(1);
}

void loop()
{
  /* The threshold (500) is typical for OVERSAMPLE_4 on this board but may need tuning.
   * The host receives a HID report only when the touch state changes.
   */
  touch_state = (qt.measure() > 500) ? 1 : 0;

  if (touch_state != touch_state_prev) {
    touch_state_prev = touch_state;
    if (usb_hid.ready())
      usb_hid.sendReport(0, &touch_state, 1);
  }

  delay(10);
}
