/*
 * Logitech TrackMan Marble FX wheel driver
 * also supports Logitech TrackMan Marble model T-BC21 (in ps/2 mode)
 *
 * Copyright © 2018-2021 Stefan Seyfried <seife@tuxbox-git.slipkontur.de>
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://www.wtfpl.net/ for more details.
 *
 *  tested on: paradisetronic Pro Micro (mini leonardo-compatible board)
 *             CMCU beetle (nano USB-connector-only leonardo-compatible)
 *             Arduino Leonardo, probably a cheap clone board
 *
 *  PS2++ protocol specs from http://web.archive.org/web/20030714000535/http://dqcs.com/logitech/ps2ppspec.htm
 *
 * based on this:
 *   Arduino Forum > Topics > Device Hacking > Logitech TrackMan Marble FX USB converter
 *   https://forum.arduino.cc/index.php?topic=365472.0
 *
 * SAMPLE_RATE and stream_mode setting idea from
 *   https://github.com/dkao/Logitech_Trackman_Marble_FX_PS2_to_USB_converter
 *
 * interrupt routines inspired by the qmk firmware:
 *   https://github.com/qmk/qmk_firmware
 *
 * depends on these libraries:
 *   HID-Project  (https://github.com/NicoHood/HID, install via library manager)
 *   Arduino-GPIO (https://github.com/mikaelpatel/Arduino-GPIO, install from source)
 *
 * The T-BC21 trackball has four buttons. It has an USB cable, but can also speak PS/2
 * We use the PS/2 mode
 *
 *  default HW setup
 *   wire PS/2 connector to arduino PIN 2 (data) and 3 (clk)
 *   see: http://playground.arduino.cc/ComponentLib/Ps2mouse
 *   for the T-BC21, wire USB D- (next to VBUS) is data and USB D+ (next to GND) is clk
 *
 *  driver limitations:
 *   use at your own risk.
 *   super hack. tested on my own TrackMan Marble FX(T-CJ12) only
 *               now also tested on TrackMan Marble (model T-BC21)
 *
 *  functionality:
 *   Marble FX: press red button to emulate wheel movement with the ball
 *   T-BC21: left small button is "red button", scroll wheel emulation,
 *           right small button is "middle button" (button 3)
 */

//# define SERIALDEBUG 1
//# define LED_DEBUG

#define USE_INTERRUPT

#ifdef USE_LEGACY_HID
#include "Mouse.h"
#else
#include "HID-Project.h"
#endif
#include "GPIO.h"

#include <avr/wdt.h>
/*
 * Pin definitions
 */
#define DATA_PIN 2
#define CLK_PIN  3
/* configuration input switches */
#define LEFTHAND_PIN 8
#define JIGGLE_PIN   7

/* int2board(CLK_PIN) == BOARD::D3 */
#define B_CONCAT(x) BOARD::D##x
#define int2board(x) B_CONCAT(x)

GPIO<int2board(DATA_PIN)>     pin_DATA;
GPIO<int2board(CLK_PIN)>      pin_CLK;
GPIO<int2board(LEFTHAND_PIN)> pin_LEFTHAND;
GPIO<int2board(JIGGLE_PIN)>   pin_JIGGLE;
GPIO<int2board(LED_BUILTIN)>  pin_LED;

#ifdef LED_DEBUG
#define LED1_PIN 10
#define LED2_PIN 11
GPIO<int2board(LED1_PIN)> pin_LED1;
GPIO<int2board(LED2_PIN)> pin_LED2;
#define DBG(PIN, FUNC) do { PIN.FUNC; } while (0)
#else
#define DBG(PIN, FUNC) do {} while (0)
#endif

#define pin_high(PIN) do { PIN.input().pullup(); } while (0)
#define pin_low(PIN)  do { PIN.output(); PIN.low(); } while (0)
#define pin_set(PIN, STATE) do { if (STATE) pin_high(PIN); else pin_low(PIN); } while (0)

/* and non-standard SAMPLE_RATE setting is disabled by default.
 * while it sounds useful at first glance, it results in problems when
 * (really) fast moving the ball, and it does not give considerable
 * benefits in standard usage etiher
 * you can uncomment this define to use it if you want
 *
 * Set sample rate.
 * PS/2 default sample rate is 100Hz.
 * Valid sample rates: 10, 20, 40, 60, 80, 100, 200
 */
//#define SAMPLE_RATE 200

/* will be set from switches on pins 7 and 8 */
bool lefthanded = false;
bool jiggler = true;
/* global variables */
uint8_t xtrabutton = 0;
bool buttons[3] = { false, false, false };
// lucky us, the definitions of MOUSE_LEFT,_RIGHT,_MIDDLE are also 1,2,4...
uint8_t bmask[3] = { 0x01, 0x02, 0x04 };
int scroll_sum = 0;

bool stream_mode = false;

#ifdef USE_INTERRUPT
const uint8_t clk_interrupt = digitalPinToInterrupt(3);
uint8_t ps2_error = 0;

/* ring buffer for received bytes */
#define MBUF_SIZE 16
uint8_t mbuf[MBUF_SIZE];
uint8_t mbuf_h = 0;
uint8_t mbuf_t = 0;
void mbuf_push(uint8_t data)
{
  uint8_t sreg = SREG;
  cli();
  uint8_t next = (mbuf_h + 1) % MBUF_SIZE;
  if (next != mbuf_t) {
    mbuf[mbuf_h] = data;
    mbuf_h = next;
  }
  SREG = sreg;
}

uint8_t mbuf_pull(void)
{
  uint8_t ret = 0;
  uint8_t sreg = SREG;
  cli();
  if (mbuf_h != mbuf_t) {
    ret = mbuf[mbuf_t];
    mbuf_t = (mbuf_t + 1) % MBUF_SIZE;
  }
  SREG = sreg;
  return ret;
}

bool mbuf_empty(void)
{
  uint8_t sreg = SREG;
  cli();
  bool ret = (mbuf_h == mbuf_t);
  SREG = sreg;
  return ret;
}

enum {
  NONE, START,
  BIT0, BIT1, BIT2, BIT3, BIT4, BIT5, BIT6, BIT7,
  PARITY, STOP
};

void ps2_ISR(void)
{
  static uint8_t bit = NONE;
  static uint8_t data = 0;
  static bool parity = false;

  if (pin_CLK) /* ???, we trigger on FALLING */
    return;

  bit++;
  switch (bit) {
    case START:
      if (pin_DATA)
        goto error;
      break;
    case BIT0...BIT7:
      data >>= 1;
      if (pin_DATA) {
        data |= 0x80;
        parity = parity ^ 1;
      }
      break;
    case PARITY:
      if (pin_DATA == parity)
        goto error;
      break;
    case STOP:
      if (!pin_DATA)
        goto error;
      mbuf_push(data);
      DBG(pin_LED1, low());
      goto done;
      break;
    default:
      goto error;
  }
  return;
error:
  ps2_error = bit;
  DBG(pin_LED1, high());
done:
  bit = NONE;
  data = 0;
  parity = false;
  return;
}
#endif

void bus_idle(void)
{
  pin_high(pin_DATA);
  pin_high(pin_CLK);
}

bool die_if_timeout(unsigned long start, bool *ret = NULL)
{
  unsigned long timeout;
  if (stream_mode)
    timeout = 500;
  else
    timeout = 5000;

  if ((millis() - start) < timeout)
    return false;
  if (ret) {
    *ret = false;
    return true;
  }
#ifdef LED_DEBUG
  /* signal the error reset with one second flashing debug LEDs */
  DBG(pin_LED2, high());
  DBG(pin_LED1, low());
  for (int i = 0; i < 10; i++) {
    delay(100);
    DBG(pin_LED1, toggle());
    DBG(pin_LED2, toggle());
  }
#endif
  wdt_enable(WDTO_30MS);
  while(true) {} ;
}

void send_bit(unsigned long start, bool data)
{
  pin_set(pin_DATA, data);
  /* wait for clock cycle */
  while (!pin_CLK)
    die_if_timeout(start);
  while (pin_CLK)
    die_if_timeout(start);
}

void mouse_write(uint8_t data)
{
  uint8_t i;
  uint8_t parity = 1;
  unsigned long start = millis();

#ifdef USE_INTERRUPT
  detachInterrupt(clk_interrupt);
#endif
  /* put pins in output mode */
  bus_idle();
  delayMicroseconds(300);
  pin_low(pin_CLK);
  delayMicroseconds(300);
  pin_low(pin_DATA);
  delayMicroseconds(10);
  DBG(pin_LED1, high());
  /* start bit */
  pin_high(pin_CLK);
  delayMicroseconds(10); /* Arduino-GPIO is too fast, so wait until CLK is actually high */
  /* wait for mouse to take control of clock); */
  while (pin_CLK)
    die_if_timeout(start);
  /* clock is low, and we are clear to send data */
  for (i=0; i < 8; i++) {
    send_bit(start, data & 1);
    parity = parity ^ (data & 0x01);
    data >>= 1;
  }
  /* parity */
  send_bit(start, parity);
  /* stop bit */
  pin_high(pin_DATA);
  delayMicroseconds(50);
  while (pin_CLK)
    die_if_timeout(start);
  /* wait for mouse to switch modes */
  while (! pin_CLK || ! pin_DATA)
    die_if_timeout(start);
  DBG(pin_LED1, low());
#ifdef USE_INTERRUPT
  bus_idle();             /* enable incoming data, will be handled by ISR */
  delayMicroseconds(10);  /* to allow pin_CLK to settle */
  attachInterrupt(clk_interrupt, ps2_ISR, FALLING);
#else
  /* put a hold on the incoming data. */
  pin_low(pin_CLK);
#endif
}

#ifdef USE_INTERRUPT
/*
 * interrupt version of mouse_read():
 * wait for max ~50ms for ringbuffer to fill, then
 * fetch one byte from the ringbuffer (or return error)
 * actual read from mouse is done in the ISR
 * if *ret != NULL it will contain the return state (false = timeout)
 */
uint8_t mouse_read(bool *ret = NULL)
{
  uint8_t retry = 50;
  while (retry-- && mbuf_empty())
    delay(1);
  DBG(pin_LED2, high());
  if (mbuf_empty()) {
    if (ret)
      *ret = false;
    DBG(pin_LED2, low());
    return 0;
  }
  if (ret)
    *ret = true;
  return mbuf_pull();
}
#else
/*
 * Get a byte of data from the mouse
 * ret is the return code (true if data was delivered, false if timeout)
 * timeout reporting is needed so that we can block in stream mode, but
 * the mouse jiggler can still do its job ;-)
 */
uint8_t mouse_read(bool *ret = NULL)
{
  uint8_t data = 0x00;
  int i;
  uint8_t bit = 0x01;

  if (ret)
    *ret = true;
  bus_idle();
  delayMicroseconds(50);
  long start = millis();
  while (pin_CLK) {
    if (die_if_timeout(start, ret))
      goto out;
  }
  DBG(pin_LED2, high());
  while (! pin_CLK) { /* eat start bit */
    if (die_if_timeout(start, ret))
      goto out;
  }
  for (i=0; i < 8; i++) {
    while (pin_CLK) {
      if (die_if_timeout(start, ret))
        goto out;
    }
    if (pin_DATA) {
      data = data | bit;
    }
    while (! pin_CLK) {
      if (die_if_timeout(start, ret))
        goto out;
    }
    bit <<= 1;
  }
  /* eat parity bit, (ignored) */
  while (pin_CLK) {
    if (die_if_timeout(start, ret))
      goto out;
  }
  while (!pin_CLK) {
    if (die_if_timeout(start, ret))
      goto out;
  }
  /* eat stop bit */
  while (pin_CLK) {
    if (die_if_timeout(start, ret))
      goto out;
  }
  while (!pin_CLK) {
    if (die_if_timeout(start, ret))
      goto out;
  }

out:
  /* stop incoming data. */
  pin_low(pin_CLK);
  DBG(pin_LED2, low());
  return data;
}
#endif

void mouse_init()
{
  bus_idle();
  delay(250);    /* allow mouse to power on */
  /* reset */
  mouse_write(0xff);
  mouse_read();  /* ack byte */
  mouse_read();  /* blank */
  mouse_read();  /* blank */
  pin_LED.toggle();  /* led off to see we passsed first init */
#ifdef SAMPLE_RATE
  mouse_write(0xf3);  /* sample rate */
  mouse_read();  /* ack */
  mouse_write(SAMPLE_RATE);
  mouse_read();  /* ack */
#endif
  if (!stream_mode) {
    mouse_write(0xf0);  /* remote mode */
    mouse_read();  /* ack */
    delayMicroseconds(100);
  }
}

void mouse_enable_report()
{
  mouse_write(0xf4); /* enable report */
  mouse_read(); /* ack */
  delayMicroseconds(100);
}

// PS2++, extended ps/2 protocol spec.
// http://web.archive.org/web/20030714000535/http://dqcs.com/logitech/ps2ppspec.htm
// also, linux kernel ps2 mouse drivers have extensive code to look up the protocol.
static uint8_t magic[] = { 0xe8, 0x00, 0xe8, 0x03, 0xe8, 0x02, 0xe8, 0x01, 0xe6, 0xe8, 0x03, 0xe8, 0x01, 0xe8, 0x02, 0xe8, 0x03 };
void ps2pp_write_magic_ping()
{
  /* e8 00 e8 03 e8 02 e8 01 e6 e8 03 e8 01 e8 02 e8 03 */
  for (uint8_t i = 0; i < sizeof(magic); i++)
    mouse_write(magic[i]);
}

void mouse_setup(void)
{
  mouse_init();
  ps2pp_write_magic_ping();
  if (stream_mode)
    mouse_enable_report();
}

bool ps2pp_decode(uint8_t b0, uint8_t b1, uint8_t b2)
{
  /* values from linux/drivers/input/mouse/logips2pp.c */
  if ((b0 & 0x48) != 0x48 || (b1 & 0x02) != 0x02)
    return false;
  // mouse extra info
  if ((b0 & 0x30) == 0x0 && (b1 & 0xf0) == 0xd0) {
    xtrabutton = (b2 & 0x30);
#ifdef SERIALDEBUG
    Serial.print("xtrabutton: ");
    Serial.println((int)xtrabutton, HEX);
#endif
  }
  return true;
}

/* the main() program code */
void setup()
{
  Mouse.begin(); /* does not actually "begin" anything but defines USB descriptors */
  pin_low(pin_DATA);
  pin_low(pin_CLK);
  pin_JIGGLE.input().pullup();
  pin_LEFTHAND.input().pullup();
  jiggler =    pin_JIGGLE;    /* default on if pin open */
  lefthanded = !pin_LEFTHAND; /* default off */
#ifdef SERIALDEBUG
  Serial.begin(115200); /* baudrate does not matter */
  delay(100);
  while(! Serial) {};
  Serial.println("HELLO!");
  Serial.print("Jiggler:\t");
  Serial.println(jiggler);
  Serial.print("Lefthanded:\t");
  Serial.println(lefthanded);
#endif
  pin_LED.output();
  pin_LED.high();
  DBG(pin_LED1, output());
  DBG(pin_LED2, output());
  DBG(pin_LED1, low());
  DBG(pin_LED2, low());
  mouse_setup();
}

long last_move = 0;
int jigglecount = 0;

void move(int8_t x, int8_t y, int8_t z)
{
  Mouse.move(x, y, z);
  last_move = millis();
  jigglecount = 0;
}

/*
 * mstat bit 0 = 0x01 left;
 * mstat bit 1 = 0x02 right;
 * xtra  bit 4 = 0x10 small left on Marble T-BC21, red button on Marble FX;
 * xtra  bit 5 = 0x20 small right on Marble T-BC21;
 * red button will be scroll, 0x20 will be mapped to 0x04 => middle button
 * return value: bit 0,1,2 = left, right, middle, bit 4 = scroll
 * if lefthanded == true, then buttons will be swapped (only useful with T-BC21)
 */
uint8_t map_buttons(uint8_t mstat, uint8_t xtra)
{
  uint8_t ret = 0;
  if (! lefthanded) {
    ret = mstat & 0x07; /* standard left/right/middle buttons */
    if (xtra & 0x20)
      ret |= 0x04;
    if (xtra & 0x10)
      ret |= 0x10; /* scroll button */
  } else { /* invert */
    if (mstat & 0x01)
      ret = 0x02;
    if (mstat & 0x02)
      ret |= 0x01;
    if (xtra & 0x10)
      ret |= 0x04;
    if (xtra & 0x20)
      ret |= 0x10; /* scroll button */
  }
  return ret;
}

void loop()
{
  bool ret;
  /* update the switch state.
     Does this even make sense at run time? but it does not hurt anyway ;-) */
  jiggler =    pin_JIGGLE;    /* default on if pin open */
  lefthanded = !pin_LEFTHAND; /* default off */
  pin_LED.toggle();
  if (!stream_mode) {
    mouse_write(0xeb);  /* give me data! */
    mouse_read();      /* ignore ack */
  }
  uint8_t mstat = mouse_read(&ret);

  if (ret || !stream_mode) { /* no timeout */
    int8_t mx    = (int8_t)mouse_read();
    int8_t my    = (int8_t)mouse_read();
#ifdef SERIALDEBUG
    Serial.print((int)mstat, HEX);
    Serial.print("\t");
    Serial.print((int)mx);
    Serial.print("\t");
    Serial.println((int)my);
#endif
    if (ps2pp_decode(mstat, mx, my) || USBDevice.isSuspended())
      return; // do nothing.

    uint8_t btn = map_buttons(mstat, xtrabutton);
    bool redbutton = btn & 0x10;
    if (redbutton) { /* translate y scroll into wheel-scroll */
      int8_t scroll = my / 8;
      if (! scroll) {
        scroll_sum += my;
        scroll = scroll_sum / 8;
      }
      if (scroll != 0) {
        scroll_sum = 0;
#ifdef SERIALDEBUG
        Serial.print("SCRL ");
        Serial.println((int)scroll);
#endif
        move(0, 0, scroll);
      }
    } else {
      /* -my to get the direction right... */
      if (mx != 0 || my != 0) {
        move(mx, -my, 0);
#ifdef SERIALDEBUG
        Serial.print("MOVE ");
        Serial.print((int)mx);
        Serial.print(" ");
        Serial.println((int)my);
#endif
      }
      scroll_sum = 0;
    }

    /* handle normal buttons */
    for (uint8_t i = 0; i < sizeof(buttons); i++) {
      bool button = btn & bmask[i];
      if (!buttons[i] && button)
        Mouse.press(bmask[i]);
      else if (buttons[i] && !button)
        Mouse.release(bmask[i]);
      buttons[i] = button;
    }

    if (!stream_mode)
      delay(20);
  }
  if (! jiggler)
    return;

  long  jiggle = (millis() - last_move);
  if (jiggle > 30000L * (jigglecount + 1) && jiggle < 1800000) {
    jigglecount++;
    if (!USBDevice.isSuspended()) {
#ifdef SERIALDEBUG
      Serial.print("JIGGLE! ");
      Serial.print(jiggle);
      Serial.print(" ");
      Serial.println(jigglecount);
#endif
      Mouse.move(1,0,0);
      Mouse.move(-1,0,0);
    }
  }
}
