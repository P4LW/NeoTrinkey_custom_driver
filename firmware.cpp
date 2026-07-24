#include <Adafruit_TinyUSB.h>
#include <Adafruit_NeoPixel.h>

#define PIN_NEOPIXEL 27
#define PIN_TOUCH    1

Adafruit_NeoPixel pixel(1, PIN_NEOPIXEL, NEO_GRB + NEO_KHZ800);

/* Shared state between the USB callback and the main loop.
 * led_colors is written by the host via CMD_SET_LED (0x01).
 * touch_state is written by loop() and read by the host via CMD_GET_TOUCH (0x02).
 */
uint8_t led_colors[3] = {0, 0, 0};
uint8_t touch_state   = 0;

/* TinyUSB vendor control transfer callback.
 * Called automatically for each stage of an incoming control request.
 */
extern "C" bool tud_vendor_control_xfer_cb(uint8_t rhport,
                                           uint8_t stage,
                                           tusb_control_request_t const *request)
{
  if (stage == CONTROL_STAGE_SETUP) {
    if (request->bRequest == 0x01)
      return tud_control_xfer(rhport, request, led_colors, 3);
    if (request->bRequest == 0x02)
      return tud_control_xfer(rhport, request, &touch_state, 1);
  }

  if (stage == CONTROL_STAGE_DATA && request->bRequest == 0x01) {
    pixel.setPixelColor(0, led_colors[0], led_colors[1], led_colors[2]);
    pixel.show();
  }

  return true;
}

void setup()
{
  pixel.begin();
  pixel.show();
}

void loop()
{
  /* Poll the capacitive sensor and update touch_state.
   * The threshold (500) may need tuning depending on environmental conditions.
   */
  touch_state = (touchRead(PIN_TOUCH) > 500) ? 1 : 0;
  delay(10);
}
