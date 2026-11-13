/**************************************************************************
       This file is part of the RPU for Arduino Project.

    I, Dick Hamill, the author of this program disclaim all copyright
    in order to make this program freely available in perpetuity to
    anyone who would like to use it. Dick Hamill, 3/31/2023

    RPU is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    RPU is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    See <https://www.gnu.org/licenses/>.
*/



#include <Arduino.h>
#include <EEPROM.h>
#define RPU_CPP_FILE
#include "RPU_Config.h"
#include "RPU.h"

#if (RPU_OS_HARDWARE_REV==400) || (RPU_OS_HARDWARE_REV==401) || (RPU_OS_HARDWARE_REV==402)
// RP2350 (Rev 400 = 400-J5, Rev 401/402 = 400-IC): the AVR timer interrupt becomes an ordinary function,
// called from a repeating hardware alarm (see RPU_HookInterrupts and RPU_SetupInterrupt)
#include "hardware/gpio.h"
#include "hardware/irq.h"
#include "hardware/structs/sio.h"
#include "hardware/sync.h"
#include "pico/time.h"
#define ISR(vector) void RPU_TimerService_##vector()
// Games keep negative states in a char (Williams self-test: MachineState -1 to -8), as on the AVR, where
// char is signed. On the RP2350 it's unsigned, so -6 became 250 and "MachineState < 0" never held: the
// Scorpion's self-test ran as game play (found 2026-11-08). Build with -fsigned-char (and -Wno-narrowing:
// arduino-pico's TinyUSB tables then narrow; the bytes are the same). The PinNode 400 board sets both.
#include <limits.h>
#if CHAR_MIN == 0
#error "RPU Rev 400/401: build with -fsigned-char -Wno-narrowing (char must be signed, as on the Mega)"
#endif
// arduino-pico's delayMicroseconds() is sleep_us(), which waits for a timer alarm: inside the display
// timer's own interrupt that alarm never comes, and the RP hangs (found on the Star Trek, 2026-10-19).
// RPU's handlers need the AVR's behaviour, a plain busy wait.
#include "hardware/timer.h"
#define delayMicroseconds(us) busy_wait_us_32(us)
// The RP runs the code between bus cycles far faster than the Mega, so its lamp strobes come out
// shorter: on the Star Trek, one lamp address (the center BALLY letters) flickered in attract. RPU's
// own setting for fast boards slows the lamp strobe down.
#ifndef RPU_SLOW_DOWN_LAMP_STROBE
#define RPU_SLOW_DOWN_LAMP_STROBE
#endif
// Each lamp group's gate pulse is held this long after its data is written. The Mega's code between
// bus cycles makes its pulses longer than the RP's; LED bulbs on the SCR lamp drivers need the longer
// pulse to stay lit (the Star Trek's LED A and Y flickered or went out, 2026-10-19).
#ifndef RPU_400_LAMP_GATE_US
#define RPU_400_LAMP_GATE_US  20
#endif
#endif

#define DEBUG_MESSAGES  0

#ifndef RPU_OS_HARDWARE_REV
#define RPU_OS_HARDWARE_REV 1
#endif

#ifndef LISYOutputSerial
#define LISYOutputSerial  Serial3
#endif

/******************************************************
     The board type, MPU architecture, and supported
     features are all controlled through the
     RPU_Config.h file.
*/
#include "RPU_Config.h"


/******************************************************
     Defines and library variables
*/
#if !defined(RPU_MPU_BUILD_FOR_6800) || (RPU_MPU_BUILD_FOR_6800==1)
boolean UsesM6800Processor = true;
#if (RPU_MPU_ARCHITECTURE>11) && (RPU_OS_HARDWARE_REV<102)
#error "Architecture > 11 doesn't make sense with RPU_MPU_BUILD_FOR_6800=1. Set RPU_MPU_BUILD_FOR_6800 to 0 in RPU_Config.h or choose a different RPU_MPU_ARCHITECTURE"
#endif
#else
boolean UsesM6800Processor = false;
#endif

#ifndef RPU_NUMBER_OF_PLAYER_DISPLAYS
#define RPU_NUMBER_OF_PLAYER_DISPLAYS       4
#endif

#if (RPU_MPU_ARCHITECTURE<10)

#define RPU_NUM_SOLENOIDS               15

#if (RPU_MPU_ARCHITECTURE==5)
// Bally 6803: switch strobes 1-5 on PIA0 PA0-4, strobe 6 on PIA1 PB4. PB0-3 = 15 (the 74LS154's unused
// output), PB4 low, PB5-7 high: neon, flipper relay and backbox lights off (they're on when low)
#define NUM_SWITCH_BYTES                6
#define NUM_SWITCH_BYTES_ON_U10_PORT_A  5
#define MAX_NUM_SWITCHES                48
#define DEFAULT_SOLENOID_STATE          0xEF
#define ST5_CONTINUOUS_SOLENOID_BIT     0x10
#elif defined(RPU_USE_EXTENDED_SWITCHES_ON_PB4)
#define NUM_SWITCH_BYTES                6
#define NUM_SWITCH_BYTES_ON_U10_PORT_A  5
#define MAX_NUM_SWITCHES                48
#define DEFAULT_SOLENOID_STATE          0x8F
#define ST5_CONTINUOUS_SOLENOID_BIT     0x10
#elif defined(RPU_USE_EXTENDED_SWITCHES_ON_PB7)
#define NUM_SWITCH_BYTES                6
#define NUM_SWITCH_BYTES_ON_U10_PORT_A  5
#define MAX_NUM_SWITCHES                48
#define DEFAULT_SOLENOID_STATE          0x1F
#define ST5_CONTINUOUS_SOLENOID_BIT     0x80
#else
#define NUM_SWITCH_BYTES                5
#define NUM_SWITCH_BYTES_ON_U10_PORT_A  5
#define MAX_NUM_SWITCHES                40
#define DEFAULT_SOLENOID_STATE          0x9F
#endif

#if !defined(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS) || !defined(RPU_OS_TIMING_LOOP_PADDING_IN_MICROSECONDS)
#error "Must define RPU_OS_SWITCH_DELAY_IN_MICROSECONDS and RPU_OS_TIMING_LOOP_PADDING_IN_MICROSECONDS in RPU_Config.h"
#endif

#elif (RPU_MPU_ARCHITECTURE >= 10)
boolean GameOverLine = true;
#define RPU_NUM_SOLENOIDS             22
#define NUM_SWITCH_BYTES              8
#define MAX_NUM_SWITCHES              64
#ifndef INTERRUPT_OCR1A_COUNTER
#define INTERRUPT_OCR1A_COUNTER         16574
#endif

volatile byte BoardLEDs = 0;
volatile boolean UpDownSwitch = false;
unsigned short ContinuousSolenoidBits = 0;

volatile byte DisplayCreditDigits[2];
volatile byte DisplayCreditDigitEnable;
volatile byte DisplayBIPDigits[2];
volatile byte DisplayBIPDigitEnable;

#if (RPU_MPU_ARCHITECTURE>=13)
volatile byte DisplayCommas;
#endif

#if (RPU_MPU_ARCHITECTURE == 15)
volatile byte DisplayText[2][RPU_OS_NUM_DIGITS];
#endif

#endif // End of condition based on RPU_MPU_ARCHITECTURE

#if (RPU_MPU_ARCHITECTURE==5)
// Each 6803 player display shows either a number (DisplayDigits) or text (RPU_SetDisplayText)
volatile char DisplayText6803[4][RPU_OS_NUM_DIGITS];
volatile boolean DisplayIsText6803[4];
// Places set by RPU_SetDisplaySegments6803: one bit per place, and the segments it shows
volatile byte DisplayRawPlaces6803[4];
volatile byte DisplayRaw6803[4][RPU_OS_NUM_DIGITS];
#endif

// Global variables
volatile byte DisplayDigits[5][RPU_OS_NUM_DIGITS];
volatile byte DisplayDigitEnable[5];
volatile boolean DisplayOffCycle = false;
volatile byte CurrentDisplayDigit = 0;
volatile byte LampStates[RPU_NUM_LAMP_BANKS], LampDim1[RPU_NUM_LAMP_BANKS], LampDim2[RPU_NUM_LAMP_BANKS];
volatile byte LampFlashPeriod[RPU_MAX_LAMPS];
byte DimDivisor1 = 2;
byte DimDivisor2 = 3;
#if defined(RPU_OS_USE_LAMP_OVERLAYS)
volatile boolean UseLampsOnOverlay = false;
volatile boolean UseLampsOffOverlay = false;
volatile byte LampsOnOverlay[RPU_NUM_LAMP_BANKS];
volatile byte LampsOffOverlay[RPU_NUM_LAMP_BANKS];
#endif

#if (RPU_OS_HARDWARE_REV==200)
volatile byte OldLampStates[RPU_NUM_LAMP_BANKS];
unsigned long LISYLastWatchdog = 0;
volatile byte LISYISRPass = 0;
byte LISYNumSwitches = 0;
byte LISYNumSounds = 64;
byte LISYNumSimpleLamps = 0;
byte LISYSwitchStates[127];
char LISYAPIVersion[32];
char LISYFirmwareVersion[32];
// Forward function references
void RPU_LISYSendGameOverState(boolean gameOver);
void RPU_LISYSendScore(byte displayNumber, byte numDigits);
void RPU_LISYSetSolenoid(boolean solOn, byte solNum);
void RPU_LISYSendSoundCommand(byte soundNum);
#endif

volatile byte SwitchesMinus2[NUM_SWITCH_BYTES];
volatile byte SwitchesMinus1[NUM_SWITCH_BYTES];
volatile byte SwitchesNow[NUM_SWITCH_BYTES];
byte SwitchInverter[NUM_SWITCH_BYTES] = {0x00};

#ifdef RPU_STREAMLINED_IMMEDIATE_SOLENOIDS
#define MAX_IMMEDIATE_STREAMLINED_SOLENOIDS     10
byte ImmediateSolenoidSwitchByte[MAX_IMMEDIATE_STREAMLINED_SOLENOIDS]; // Can't imagine more than 10 immediate solenoids
byte ImmediateSolenoidSwitchFlag[MAX_IMMEDIATE_STREAMLINED_SOLENOIDS]; // Can't imagine more than 10 immediate solenoids
byte ImmediateSolenoidSwitchMask[NUM_SWITCH_BYTES];
#endif

#ifdef RPU_OS_USE_DIP_SWITCHES
byte DipSwitches[4];
#endif

#if (RPU_OS_HARDWARE_REV>2)
#define SOLENOID_STACK_SIZE 150
#else
#define SOLENOID_STACK_SIZE 60
#endif
#define SOLENOID_STACK_EMPTY 0xFF
volatile byte SolenoidStackFirst;
volatile byte SolenoidStackLast;
volatile byte SolenoidStack[SOLENOID_STACK_SIZE];
boolean SolenoidStackEnabled = true;
volatile byte CurrentSolenoidByte = 0xFF;
#if (RPU_MPU_ARCHITECTURE==5)
// The 6803's switch strobe 6 shares PIA1 PB with the solenoids: only the interrupt writes PB, so code
// outside it sets CurrentSolenoidByte and this flag
volatile boolean SolenoidByteChanged = false;
#define RPU_WRITE_SOLENOID_BYTE() (SolenoidByteChanged = true)
#else
#define RPU_WRITE_SOLENOID_BYTE() RPU_DataWrite(ADDRESS_U11_B, CurrentSolenoidByte)
#endif
volatile byte RevertSolenoidBit = 0x00;
volatile byte NumCyclesBeforeRevertingSolenoidByte = 0;

#define TIMED_SOLENOID_STACK_SIZE 30
struct TimedSolenoidEntry {
  byte inUse;
  unsigned long pushTime;
  byte solenoidNumber;
  byte numPushes;
  byte disableOverride;
};
TimedSolenoidEntry TimedSolenoidStack[TIMED_SOLENOID_STACK_SIZE] = {0, 0, 0, 0, 0};

#define SWITCH_STACK_SIZE   60
#define SWITCH_STACK_EMPTY  0xFF
volatile byte SwitchStackFirst;
volatile byte SwitchStackLast;
volatile byte SwitchStack[SWITCH_STACK_SIZE];


// The WTYPE1 and WTYPE2 sound cards can only play one sound at a time,
// so these structures allow the app to send in as many calls as they
// want, but with a priority and requested amount of time to let
// it play. This could be ported over to other architectures
// like S&T or -51, etc., but right now I've only implemented it for
// MPU Architecture > 9
#if (RPU_MPU_ARCHITECTURE >= 10)

#define SOUND_STACK_SIZE  64
#define SOUND_STACK_EMPTY 0x0000
volatile byte SoundStackFirst;
volatile byte SoundStackLast;
volatile unsigned short SoundStack[SOUND_STACK_SIZE];

#define TIMED_SOUND_STACK_SIZE  20
struct TimedSoundEntry {
  byte inUse;
  unsigned long pushTime;
  unsigned short soundNumber;
  byte numPushes;
};
TimedSoundEntry TimedSoundStack[TIMED_SOUND_STACK_SIZE] = {0, 0, 0, 0};
#endif

#if (RPU_OS_HARDWARE_REV==1)
#if (RPU_MPU_ARCHITECTURE!=1)
#error "RPU_OS_HARDWARE_REV 1 only works on machines with RPU_MPU_ARCHITECTURE of 1"
#endif
#define ADDRESS_U10_A           0x14
#define ADDRESS_U10_A_CONTROL   0x15
#define ADDRESS_U10_B           0x16
#define ADDRESS_U10_B_CONTROL   0x17
#define ADDRESS_U11_A           0x18
#define ADDRESS_U11_A_CONTROL   0x19
#define ADDRESS_U11_B           0x1A
#define ADDRESS_U11_B_CONTROL   0x1B
#define ADDRESS_SB100           0x10

#elif (RPU_OS_HARDWARE_REV==2)
#if (RPU_MPU_ARCHITECTURE!=1)
#error "RPU_OS_HARDWARE_REV 2 only works on machines with RPU_MPU_ARCHITECTURE of 1"
#endif
#define ADDRESS_U10_A           0x00
#define ADDRESS_U10_A_CONTROL   0x01
#define ADDRESS_U10_B           0x02
#define ADDRESS_U10_B_CONTROL   0x03
#define ADDRESS_U11_A           0x08
#define ADDRESS_U11_A_CONTROL   0x09
#define ADDRESS_U11_B           0x0A
#define ADDRESS_U11_B_CONTROL   0x0B
#define ADDRESS_SB100           0x10
#define ADDRESS_SB100_CHIMES    0x18
#define ADDRESS_SB300_SQUARE_WAVES  0x10
#define ADDRESS_SB300_ANALOG        0x18

#elif (RPU_OS_HARDWARE_REV==3) || (RPU_OS_HARDWARE_REV==4)
#if (RPU_MPU_ARCHITECTURE!=1)
#error "RPU_OS_HARDWARE_REV 3 and 4 only work on machines with RPU_MPU_ARCHITECTURE of 1"
#endif
#define ADDRESS_U10_A           0x88
#define ADDRESS_U10_A_CONTROL   0x89
#define ADDRESS_U10_B           0x8A
#define ADDRESS_U10_B_CONTROL   0x8B
#define ADDRESS_U11_A           0x90
#define ADDRESS_U11_A_CONTROL   0x91
#define ADDRESS_U11_B           0x92
#define ADDRESS_U11_B_CONTROL   0x93
#define ADDRESS_SB100           0xA0
#define ADDRESS_SB100_CHIMES    0xC0
#define ADDRESS_SB300_SQUARE_WAVES  0xA0
#define ADDRESS_SB300_ANALOG        0xC0

#elif (RPU_OS_HARDWARE_REV==402)
#if (RPU_MPU_ARCHITECTURE!=5)
#error "RPU_OS_HARDWARE_REV 402 (the 400-IC on a Bally 6803 MPU) needs RPU_MPU_ARCHITECTURE 5"
#endif
// The 6803 board's PIA0 (U8) does U10's jobs and PIA1 (U7) U11's: switches, lamps and display strobes on
// PIA0, display segments and solenoids on PIA1
#define ADDRESS_U10_A           0x20
#define ADDRESS_U10_A_CONTROL   0x21
#define ADDRESS_U10_B           0x22
#define ADDRESS_U10_B_CONTROL   0x23
#define ADDRESS_U11_A           0x40
#define ADDRESS_U11_A_CONTROL   0x41
#define ADDRESS_U11_B           0x42
#define ADDRESS_U11_B_CONTROL   0x43

#elif (RPU_OS_HARDWARE_REV>=100)
#if (RPU_MPU_ARCHITECTURE<10)
#define ADDRESS_U10_A           0x88
#define ADDRESS_U10_A_CONTROL   0x89
#define ADDRESS_U10_B           0x8A
#define ADDRESS_U10_B_CONTROL   0x8B
#define ADDRESS_U11_A           0x90
#define ADDRESS_U11_A_CONTROL   0x91
#define ADDRESS_U11_B           0x92
#define ADDRESS_U11_B_CONTROL   0x93
#define ADDRESS_SB100           0xA0
#define ADDRESS_SB100_CHIMES    0xC0
#define ADDRESS_SB300_SQUARE_WAVES  0xA0
#define ADDRESS_SB300_ANALOG        0xC0
#else
#define PIA_DISPLAY_PORT_A      0x2800
#define PIA_DISPLAY_CONTROL_A   0x2801
#define PIA_DISPLAY_PORT_B      0x2802
#define PIA_DISPLAY_CONTROL_B   0x2803
#define PIA_SWITCH_PORT_A       0x3000
#define PIA_SWITCH_CONTROL_A    0x3001
#define PIA_SWITCH_PORT_B       0x3002
#define PIA_SWITCH_CONTROL_B    0x3003
#define PIA_LAMPS_PORT_A        0x2400
#define PIA_LAMPS_CONTROL_A     0x2401
#define PIA_LAMPS_PORT_B        0x2402
#define PIA_LAMPS_CONTROL_B     0x2403
#define PIA_SOLENOID_PORT_A     0x2200
#define PIA_SOLENOID_CONTROL_A  0x2201
#define PIA_SOLENOID_PORT_B     0x2202
#define PIA_SOLENOID_CONTROL_B  0x2203
#if (RPU_MPU_ARCHITECTURE==13)
#define PIA_SOUND_COMMA_PORT_A      0x2100
#define PIA_SOUND_COMMA_CONTROL_A   0x2101
#define PIA_SOUND_COMMA_PORT_B      0x2102
#define PIA_SOUND_COMMA_CONTROL_B   0x2103
#endif
#if (RPU_MPU_ARCHITECTURE==15)
#define PIA_SOUND_11_PORT_A             0x2100
#define PIA_SOUND_11_CONTROL_A          0x2101
#define PIA_SOLENOID_11_PORT_B          0x2102
#define PIA_SOLENOID_11_CONTROL_B       0x2103
#define PIA_ALPHA_DISPLAY_PORT_A        0x2C00
#define PIA_ALPHA_DISPLAY_CONTROL_A     0x2C01
#define PIA_ALPHA_DISPLAY_PORT_B        0x2C02
#define PIA_ALPHA_DISPLAY_CONTROL_B     0x2C03
#define PIA_NUM_DISPLAY_PORT_A          0x3400
#define PIA_NUM_DISPLAY_CONTROL_A       0x3401
#define PIA_WIDGET_PORT_B               0x3402
#define PIA_WIDGET_CONTROL_B            0x3403
#endif
#endif

#endif


/******************************************************
     Hardware Interface Functions

     These functions have conditional compilation for different RPU_OS_HARDWARE_REVs

     RPU_OS_HARDWARE_REV 1 - Nano board that plugs into J5 (only works on -17, -35, 100, and 200 MPUs)
     RPU_OS_HARDWARE_REV 2 - Nano board that plugs into J5 (only works on -17, -35, 100, and 200 MPUs)
                             adds support for SB300 sound cards
     RPU_OS_HARDWARE_REV 3 - MEGA2560 PRO board that plugs into J5 (only works on -17, -35, 100, and 200 MPUs)
                             adds support for full address space
     RPU_OS_HARDWARE_REV 4 - MEGA2560 PRO board that plugs into J5 (only works on -17, -35, 100, and 200 MPUs)
                             adds support for OLED display, WIFI, and multiple serial ports
     RPU_OS_HARDWARE_REV 100 - MEGA2560 PRO board that plugs into processor socket (prototype)
     RPU_OS_HARDWARE_REV 101 - MEGA2560 PRO board that plugs into processor socket
                               adds support for multiple serial ports (limited release)
     RPU_OS_HARDWARE_REV 102 - MEGA2560 PRO board that plugs into processor socket (prototype)
                               adds support for OLED display, WIFI, autodetection of processor type
     RPU_OS_HARDWARE_REV 200 - MEGA2560 PRO embedded on System Alpha MPU
     RPU_OS_HARDWARE_REV 400 - RP2350B (PinNode 400-J5) that plugs into J5 (only works on -17, -35, 100, and 200 MPUs)
                               adds support for WAV sound from an SD card (RPU400Audio) and the PinNode network card
     RPU_OS_HARDWARE_REV 401 - RP2350B (PinNode 400-IC) that plugs into the processor socket of Williams System 4/6
                               (bus master, no CPU; never boots the original game)
     RPU_OS_HARDWARE_REV 402 - RP2350B (PinNode 400-IC) in the processor socket of a Bally 6803 MPU
                               (RPU_MPU_ARCHITECTURE 5; bus master, no CPU)
                                     
*******************************************************/

#if (RPU_OS_HARDWARE_REV==1) or (RPU_OS_HARDWARE_REV==2)

#if defined(__AVR_ATmega2560__)
#error "ATMega requires RPU_OS_HARDWARE_REV of 3, check RPU_Config.h and adjust settings"
#endif

// Rev 1 or 2
void RPU_DataWrite(int address, byte data) {

  // Set data pins to output
  // Make pins 5-7 output (and pin 3 for R/W)
  DDRD = DDRD | 0xE8;
  // Make pins 8-12 output
  DDRB = DDRB | 0x1F;

  // Set R/W to LOW
  PORTD = (PORTD & 0xF7);

  // Put data on pins
  // Put lower three bits on 5-7
  PORTD = (PORTD & 0x1F) | ((data & 0x07) << 5);
  // Put upper five bits on 8-12
  PORTB = (PORTB & 0xE0) | (data >> 3);

  // Set up address lines
  PORTC = (PORTC & 0xE0) | address;

  // Wait until clock is high and then 
  // move on after falling edge
  while (!(PIND & 0x10));
  while ((PIND & 0x10));

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTC = PORTC | 0x20;

  // Wait while clock is low
  while (!(PIND & 0x10));

  // Wait while clock is high
  while ((PIND & 0x10));

  // Wait while clock is low
  while (!(PIND & 0x10));

  // Set VMA OFF
  PORTC = PORTC & 0xDF;

  // Unset address lines
  PORTC = PORTC & 0xE0;

  // Set R/W back to HIGH
  PORTD = (PORTD | 0x08);

  // Set data pins to input
  // Make pins 5-7 input
  DDRD = DDRD & 0x1F;
  // Make pins 8-12 input
  DDRB = DDRB & 0xE0;
}

// Rev 1 or 2
byte RPU_DataRead(int address) {

  // Set data pins to input
  // Make pins 5-7 input
  DDRD = DDRD & 0x1F;
  // Make pins 8-12 input
  DDRB = DDRB & 0xE0;

  // Set R/W to HIGH
  DDRD = DDRD | 0x08;
  PORTD = (PORTD | 0x08);

  // Set up address lines
  PORTC = (PORTC & 0xE0) | address;

  // Wait for a falling edge of the clock
  while ((PIND & 0x10));

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTC = PORTC | 0x20;

  // Wait a full clock cycle to make sure data lines are ready
  // (important for faster clocks)
  // Wait while clock is low
  while (!(PIND & 0x10));

  // Wait for a falling edge of the clock
  while ((PIND & 0x10));

  // Wait while clock is low
  while (!(PIND & 0x10));

  byte inputData = (PIND >> 5) | (PINB << 3);

  // Set VMA OFF
  PORTC = PORTC & 0xDF;

  // Wait for a falling edge of the clock
  // Doesn't seem to help  while((PIND & 0x10));

  // Set R/W to LOW
  PORTD = (PORTD & 0xF7);

  // Clear address lines
  PORTC = (PORTC & 0xE0);

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==3)

// Rev 3 connections
// Pin D2 = IRQ
// Pin D3 = CLOCK
// Pin D4 = VMA
// Pin D5 = R/W
// Pin D6-12 = D0-D6
// Pin D13 = SWITCH
// Pin D14 = HALT
// Pin D15 = D7
// Pin D16-30 = A0-A14

#if defined(__AVR_ATmega328P__)
#error "RPU_OS_HARDWARE_REV 3 requires ATMega2560, check RPU_Config.h and adjust settings"
#endif

// Rev 3
void RPU_DataWrite(int address, byte data) {

  // Set data pins to output
  DDRH = DDRH | 0x78;
  DDRB = DDRB | 0x70;
  DDRJ = DDRJ | 0x01;

  // Set R/W to LOW
  PORTE = (PORTE & 0xF7);

  // Put data on pins
  // Lower Nibble goes on PortH3 through H6
  PORTH = (PORTH & 0x87) | ((data & 0x0F) << 3);
  // Bits 4-6 go on PortB4 through B6
  PORTB = (PORTB & 0x8F) | ((data & 0x70));
  // Bit 7 goes on PortJ0
  PORTJ = (PORTJ & 0xFE) | (data >> 7);

  // Set up address lines
  PORTH = (PORTH & 0xFC) | ((address & 0x0001) << 1) | ((address & 0x0002) >> 1); // A0-A1
  PORTD = (PORTD & 0xF0) | ((address & 0x0004) << 1) | ((address & 0x0008) >> 1) | ((address & 0x0010) >> 3) | ((address & 0x0020) >> 5); // A2-A5
  PORTA = ((address & 0x3FC0) >> 6); // A6-A13
  PORTC = (PORTC & 0x3F) | ((address & 0x4000) >> 7) | ((address & 0x8000) >> 9); // A14-A15

  // Wait until clock is high and then 
  // move on after falling edge
  while (!(PINE & 0x20));
  while ((PINE & 0x20));

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x20;

  // Wait while clock is low
  while (!(PINE & 0x20));

  // Wait while clock is high
  while ((PINE & 0x20));

  // Wait while clock is low
  while (!(PINE & 0x20));

  // Set VMA OFF
  PORTG = PORTG & 0xDF;

  // Unset address lines
  PORTH = (PORTH & 0xFC);
  PORTD = (PORTD & 0xF0);
  PORTA = 0;
  PORTC = (PORTC & 0x3F);

  // Set R/W back to HIGH
  PORTE = (PORTE | 0x08);

  // Set data pins to input
  DDRH = DDRH & 0x87;
  DDRB = DDRB & 0x8F;
  DDRJ = DDRJ & 0xFE;

}

// Rev 3
byte RPU_DataRead(int address) {

  // Set data pins to input
  DDRH = DDRH & 0x87;
  DDRB = DDRB & 0x8F;
  DDRJ = DDRJ & 0xFE;

  // Set R/W to HIGH
  DDRE = DDRE | 0x08;
  PORTE = (PORTE | 0x08);

  // Set up address lines
  PORTH = (PORTH & 0xFC) | ((address & 0x0001) << 1) | ((address & 0x0002) >> 1); // A0-A1
  PORTD = (PORTD & 0xF0) | ((address & 0x0004) << 1) | ((address & 0x0008) >> 1) | ((address & 0x0010) >> 3) | ((address & 0x0020) >> 5); // A2-A5
  PORTA = ((address & 0x3FC0) >> 6); // A6-A13
  PORTC = (PORTC & 0x3F) | ((address & 0x4000) >> 7) | ((address & 0x8000) >> 9); // A14-A15

  // Wait until clock is high and then 
  // move on after falling edge
  while (!(PINE & 0x20));
  while ((PINE & 0x20));

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x20;

  // Wait a full clock cycle to make sure data lines are ready
  // (important for faster clocks)
  // Wait while clock is low
  while (!(PINE & 0x20));

  // Wait for a falling edge of the clock
  while ((PINE & 0x20));

  // Wait while clock is low
  while (!(PINE & 0x20));

  byte inputData;
  inputData = (PINH & 0x78) >> 3;
  inputData |= (PINB & 0x70);
  inputData |= PINJ << 7;

  // Set VMA OFF
  PORTG = PORTG & 0xDF;

  // Set R/W to LOW
  PORTE = (PORTE & 0xF7);

  // Unset address lines
  PORTH = (PORTH & 0xFC);
  PORTD = (PORTD & 0xF0);
  PORTA = 0;
  PORTC = (PORTC & 0x3F);

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==4)

// Rev 3 connections
// Pin D2 = IRQ
// Pin D3 = CLOCK
// Pin D4 = VMA
// Pin D5 = R/W
// Pin D6-12 = D0-D6
// Pin D13 = SWITCH
// Pin D14 = HALT
// Pin D15 = D7
// Pin D16-30 = A0-A14

#if defined(__AVR_ATmega328P__)
#error "RPU_OS_HARDWARE_REV 4 requires ATMega2560, check RPU_Config.h and adjust settings"
#endif

#define RPU_VMA_PIN                   40
#define RPU_RW_PIN                    3
#define RPU_PHI2_PIN                  39
#define RPU_SWITCH_PIN                38
#define RPU_BUFFER_DISABLE            5
#define RPU_HALT_PIN                  41
#define RPU_RESET_PIN                 42
#define RPU_DIAGNOSTIC_PIN            44
#define RPU_PINS_OUTPUT true
#define RPU_PINS_INPUT false

// REVISION 4 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 16; count++) {
    pinMode(A0 + count, pinsOutput ? OUTPUT : INPUT);
  }
}

// REVISION 4 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 8; count++) {
    pinMode(22 + count, pinsOutput ? OUTPUT : INPUT);
  }
}


// REVISION 4 HARDWARE
void RPU_DataWrite(int address, byte data) {

  // Set data pins to output
  DDRA = 0xFF;

  // Set R/W to LOW
  PORTE = (PORTE & 0xDF);

  // Put data on pins
  PORTA = data;

  // Set up address lines
  PORTF = (byte)(address & 0x00FF);
  PORTK = (byte)(address / 256);

  if (UsesM6800Processor) {    
    // Wait until clock is high and then 
    // move on after falling edge
    while (!(PING & 0x04));
    while ((PING & 0x04));
  } else {
    // Set clock low (PG2) (if 6802/8)
    PORTG &= ~0x04;
  }

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x02;

  if (UsesM6800Processor) {
    // Wait while clock is low
    while (!(PING & 0x04));

    // Wait while clock is high
    while ((PING & 0x04));

    // Wait while clock is low
    while (!(PING & 0x04));
  } else {
    // Set clock high
    PORTG |= 0x04;

    // Set clock low
    PORTG &= ~0x04;

    // Set clock high
    PORTG |= 0x04;
  }

  // Set VMA OFF
  PORTG = PORTG & 0xFD;

  // Unset address lines
  PORTF = 0x00;
  PORTK = 0x00;

  // Set R/W back to HIGH
  PORTE = (PORTE | 0x20);

  // Set data pins to input
  DDRA = 0x00;

}

// REVISION 4 HARDWARE
byte RPU_DataRead(int address) {

  // Set data pins to input
  DDRA = 0x00;

  // Set R/W to HIGH
  DDRE = DDRE | 0x20;
  PORTE = (PORTE | 0x20);

  // Set up address lines
  PORTF = (byte)(address & 0x00FF);
  PORTK = (byte)(address / 256);

  if (UsesM6800Processor) {
    // Wait for a falling edge of the clock
    while (!(PING & 0x04));
    while ((PING & 0x04));
  } else {
    // Set clock low
    PORTG &= ~0x04;
  }

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x02;

  if (UsesM6800Processor) {
    // Wait a full clock cycle to make sure data lines are ready
    // (important for faster clocks)
    // Wait while clock is low
    while (!(PING & 0x04));

    // Wait for a falling edge of the clock
    while ((PING & 0x04));

    // Wait while clock is low
    while (!(PING & 0x04));
  } else {
    // Set clock high
    PORTG |= 0x04;

    // Set clock low
    PORTG &= ~0x04;

    // Set clock high
    PORTG |= 0x04;
  }

  byte inputData;
  inputData = PINA;

  // Set VMA OFF
  PORTG = PORTG & 0xFD;

  // Set R/W to LOW
  PORTE = (PORTE & 0xDF);

  // Unset address lines
  PORTF = 0x00;
  PORTK = 0x00;

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==100)

#if defined(__AVR_ATmega328P__)
#error "RPU_OS_HARDWARE_REV 100 requires ATMega2560, check RPU_Config.h and adjust settings"
#endif

#define RPU_VMA_PIN         4
#define RPU_RW_PIN          5
#define RPU_PHI2_PIN        3
#define RPU_SWITCH_PIN      13
#define RPU_BUFFER_DISABLE  2
#define RPU_HALT_PIN        14
#define RPU_RESET_PIN       14
#define RPU_PINS_OUTPUT true
#define RPU_PINS_INPUT false

// REV 100 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 16; count++) {
    pinMode(16 + count, pinsOutput ? OUTPUT : INPUT);
  }
}

// REV 100 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 7; count++) {
    pinMode(6 + count, pinsOutput ? OUTPUT : INPUT);
  }
  pinMode(15, pinsOutput ? OUTPUT : INPUT);
}

// REV 100 HARDWARE
void RPU_DataWrite(int address, byte data) {

  // Set data pins to output
  DDRH = DDRH | 0x78;
  DDRB = DDRB | 0x70;
  DDRJ = DDRJ | 0x01;

  // Set R/W to LOW
  PORTE = (PORTE & 0xF7);

  // Put data on pins
  // Lower Nibble goes on PortH3 through H6
  PORTH = (PORTH & 0x87) | ((data & 0x0F) << 3);
  // Bits 4-6 go on PortB4 through B6
  PORTB = (PORTB & 0x8F) | ((data & 0x70));
  // Bit 7 goes on PortJ0
  PORTJ = (PORTJ & 0xFE) | (data >> 7);

  // Set up address lines
  PORTH = (PORTH & 0xFC) | ((address & 0x0001) << 1) | ((address & 0x0002) >> 1); // A0-A1
  PORTD = (PORTD & 0xF0) | ((address & 0x0004) << 1) | ((address & 0x0008) >> 1) | ((address & 0x0010) >> 3) | ((address & 0x0020) >> 5); // A2-A5
  PORTA = ((address & 0x3FC0) >> 6); // A6-A13
  PORTC = (PORTC & 0x3F) | ((address & 0x4000) >> 7) | ((address & 0x8000) >> 9); // A14-A15

  // Set clock low
  PORTE &= ~0x20;

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x20;

  // Set clock high
  PORTE |= 0x20;

  // Set clock low
  PORTE &= ~0x20;

  // Set clock high
  PORTE |= 0x20;

  // Set VMA OFF
  PORTG = PORTG & 0xDF;

  // Unset address lines
  PORTH = (PORTH & 0xFC);
  PORTD = (PORTD & 0xF0);
  PORTA = 0;
  PORTC = (PORTC & 0x3F);

  // Set R/W back to HIGH
  PORTE = (PORTE | 0x08);

  // Set data pins to input
  DDRH = DDRH & 0x87;
  DDRB = DDRB & 0x8F;
  DDRJ = DDRJ & 0xFE;

}

// REV 100 HARDWARE
byte RPU_DataRead(int address) {

  // Set data pins to input
  DDRH = DDRH & 0x87;
  DDRB = DDRB & 0x8F;
  DDRJ = DDRJ & 0xFE;

  // Set R/W to HIGH
  DDRE = DDRE | 0x08;
  PORTE = (PORTE | 0x08);

  // Set up address lines
  PORTH = (PORTH & 0xFC) | ((address & 0x0001) << 1) | ((address & 0x0002) >> 1); // A0-A1
  PORTD = (PORTD & 0xF0) | ((address & 0x0004) << 1) | ((address & 0x0008) >> 1) | ((address & 0x0010) >> 3) | ((address & 0x0020) >> 5); // A2-A5
  PORTA = ((address & 0x3FC0) >> 6); // A6-A13
  PORTC = (PORTC & 0x3F) | ((address & 0x4000) >> 7) | ((address & 0x8000) >> 9); // A14-A15

  // Set clock low
  PORTE &= ~0x20;

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x20;

  // Set clock high
  PORTE |= 0x20;

  // Set clock low
  PORTE &= ~0x20;

  // Set clock high
  PORTE |= 0x20;

  byte inputData;
  inputData = (PINH & 0x78) >> 3;
  inputData |= (PINB & 0x70);
  inputData |= PINJ << 7;

  // Set VMA OFF
  PORTG = PORTG & 0xDF;

  // Set R/W to LOW
  PORTE = (PORTE & 0xF7);

  // Unset address lines
  PORTH = (PORTH & 0xFC);
  PORTD = (PORTD & 0xF0);
  PORTA = 0;
  PORTC = (PORTC & 0x3F);

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==101) || (RPU_OS_HARDWARE_REV==102)

#if defined(__AVR_ATmega328P__)
#error "RPU_OS_HARDWARE_REV >100 requires ATMega2560, check RPU_Config.h and adjust settings"
#endif

#define RPU_VMA_PIN                   40
#define RPU_RW_PIN                    3
#define RPU_PHI2_PIN                  39
#define RPU_SWITCH_PIN                38
#define RPU_BUFFER_DISABLE            5
#define RPU_HALT_PIN                  41
#define RPU_RESET_PIN                 42
#define RPU_BA_PIN                    43
#define RPU_DIAGNOSTIC_PIN            44
#define RPU_DISABLE_PHI_FROM_MPU      7
#define RPU_DISABLE_PHI_FROM_CPU      6
#define RPU_BOARD_SEL_0               30
#define RPU_BOARD_SEL_1               31
#define RPU_BOARD_SEL_2               32
#define RPU_BOARD_SEL_3               33
#define RPU_PINS_OUTPUT       true
#define RPU_PINS_INPUT        false

// REVISION 101/102 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 16; count++) {
    pinMode(A0 + count, pinsOutput ? OUTPUT : INPUT);
  }
}

// REVISION 101/102 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  for (int count = 0; count < 8; count++) {
    pinMode(22 + count, pinsOutput ? OUTPUT : INPUT);
  }
}

// REVISION 101/102 HARDWARE
void RPU_DataWrite(int address, byte data) {

  // Set data pins to output
  DDRA = 0xFF;

  // Set R/W to LOW
  PORTE = (PORTE & 0xDF);

  // Put data on pins
  PORTA = data;

  // Set up address lines
  PORTF = (byte)(address & 0x00FF);
  PORTK = (byte)(address / 256);

  if (UsesM6800Processor) {
    // Wait until clock is high and then 
    // move on after falling edge
    while (!(PING & 0x04));
    while ((PING & 0x04));
  } else {
    // Set clock low (PG2) (if 6802/8)
    PORTG &= ~0x04;
  }

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x02;

  if (UsesM6800Processor) {
    // Wait while clock is low
    while (!(PING & 0x04));

    // Wait while clock is high
    while ((PING & 0x04));

    // Wait while clock is low
    while (!(PING & 0x04));
  } else {
    // Set clock high
    PORTG |= 0x04;

    // Set clock low
    PORTG &= ~0x04;

    // Set clock high
    PORTG |= 0x04;
  }

  // Set VMA OFF
  PORTG = PORTG & 0xFD;

  // Unset address lines
  PORTF = 0x00;
  PORTK = 0x00;

  // Set R/W back to HIGH
  PORTE = (PORTE | 0x20);

  // Set data pins to input
  DDRA = 0x00;

}

// REVISION 101/102 HARDWARE
byte RPU_DataRead(int address) {

  // Set data pins to input
  DDRA = 0x00;

  // Set R/W to HIGH
  DDRE = DDRE | 0x20;
  PORTE = (PORTE | 0x20);

  // Set up address lines
  PORTF = (byte)(address & 0x00FF);
  PORTK = (byte)(address / 256);

  if (UsesM6800Processor) {
    // Wait until clock is high and then 
    // move on after falling edge
    while (!(PING & 0x04));
    while ((PING & 0x04));
  } else {
    // Set clock low
    PORTG &= ~0x04;
  }

  // Pulse VMA over one clock cycle
  // Set VMA ON
  PORTG = PORTG | 0x02;

  if (UsesM6800Processor) {
    // Wait a full clock cycle to make sure data lines are ready
    // (important for faster clocks)
    // Wait while clock is low
    while (!(PING & 0x04));

    // Wait for a falling edge of the clock
    while ((PING & 0x04));

    // Wait while clock is low
    while (!(PING & 0x04));
  } else {
    // Set clock high
    PORTG |= 0x04;

    // Set clock low
    PORTG &= ~0x04;

    // Set clock high
    PORTG |= 0x04;
  }

  byte inputData;
  inputData = PINA;

  // Set VMA OFF
  PORTG = PORTG & 0xFD;

  // Set R/W to LOW
  PORTE = (PORTE & 0xDF);

  // Unset address lines
  PORTF = 0x00;
  PORTK = 0x00;

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==400)

// PinNode 400-J5: RP2350B (Waveshare Core2350B) on J5 of a Bally/Stern 6800 MPU, Rev 4 style:
// the MPU's 6800 is held in HALT and the RP runs the bus cycles, timed from J5's phi2.
// GPIO0-7 = D0-D7, GPIO8-22 = A0-A14, GPIO24 = R/W, GPIO25 = VMA, GPIO26 = HALT (low = halted),
// GPIO27 = phi2, GPIO28 = IRQ, GPIO29 = RESET. Only HALT is held low while RPU runs: RESET is never driven,
// because on the J5 boards it also resets the PIAs (held low, they ignore every write and read 00: found on
// the Star Trek's Alltek, 2026-10-19). HALT is released by making it an input (its pull-up goes to 5 V).
// GPIO0-39 are 5 V tolerant; the bus pins keep their internal pulls off (RP2350 erratum E9).

#if !defined(ARDUINO_ARCH_RP2040)
#error "RPU_OS_HARDWARE_REV 400 is the RP2350 (PinNode 400): build for a Raspberry Pi RP2350 board"
#endif

#define RPU_DATA_MASK                 0x000000FFUL
#define RPU_ADDRESS_SHIFT             8
#define RPU_ADDRESS_MASK              (0x7FFFUL << RPU_ADDRESS_SHIFT)
#define RPU_RW_PIN                    24
#define RPU_VMA_PIN                   25
#define RPU_HALT_PIN                  26
#define RPU_PHI2_PIN                  27
#define RPU_IRQ_PIN                   28
#define RPU_RW_MASK                   (1UL << RPU_RW_PIN)
#define RPU_VMA_MASK                  (1UL << RPU_VMA_PIN)
#define RPU_PHI2_MASK                 (1UL << RPU_PHI2_PIN)
#define RPU_SWITCH_PIN                255   // no boot selector switch on the 400
#define RPU_BUFFER_DISABLE            255   // no buffers on the 400-J5
#define RPU_RESET_PIN                 255   // never driven: see above
#define RPU_DIAGNOSTIC_PIN            255
#define RPU_PINS_OUTPUT true
#define RPU_PINS_INPUT false

static inline uint32_t RPU_BusIn() { return sio_hw->gpio_in; }
static inline void RPU_WaitPhi2High() { while (!(sio_hw->gpio_in & RPU_PHI2_MASK)); }
static inline void RPU_WaitPhi2Low() { while (sio_hw->gpio_in & RPU_PHI2_MASK); }
// The bus cycles run from RAM: code fetched from the external flash can stall the RP for about a
// microsecond (a cache miss), about half a phi2 cycle at Bally's ~540 kHz. A read that stalls past
// the end of phi2-high samples a bus nobody drives ($FF): on the Star Trek that read as the
// self-test button and dropped the game into the operator menu (2026-10-19).
#define RPU_BUS_FUNC(name) __not_in_flash_func(name)

// REVISION 400 HARDWARE
// Halt the 6800 the moment the RP starts, before the sketch's setup() runs: GPIO26 low pulls /HALT
// down through 1k. RPU_InitializeMPU releases it again if the original game is chosen.
// Also: GPIO38 is the XIAO's D2 (its ATTN line to us). Pull it up, off the RP's reset pull-down, so
// it never sits low while the XIAO boots.
static struct RPU_EarlyHalt {
  RPU_EarlyHalt() {
    gpio_init(RPU_HALT_PIN); gpio_put(RPU_HALT_PIN, 0); gpio_set_dir(RPU_HALT_PIN, true);
    gpio_init(38); gpio_pull_up(38);
  }
} RPU_EarlyHaltAtStart;

// REVISION 400 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_ADDRESS_MASK;
  else sio_hw->gpio_oe_clr = RPU_ADDRESS_MASK;
}

// REVISION 400 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_DATA_MASK;
  else sio_hw->gpio_oe_clr = RPU_DATA_MASK;
}

// REVISION 400 HARDWARE
// One-time setup of every bus pin: SIO function, no pulls, inputs to start with
void RPU_InitBusPins() {
  for (int pin = 0; pin <= RPU_IRQ_PIN; pin++) {
    if (pin == 23 || pin == RPU_HALT_PIN) continue;      // GPIO23 unused; HALT is already driven low
    gpio_init(pin);
    gpio_disable_pulls(pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);  // the Mega drove this bus at 5 V; the RP's default is 4 mA
  }
}

// REVISION 400 HARDWARE
// Each bus cycle runs with interrupts held off (about 3 us): an interrupt that lands mid-cycle would
// run its own cycles while this one has VMA up, and glitch it. (Games call RPU_DataRead(0) from loop().)
void RPU_BUS_FUNC(RPU_DataWrite)(int address, byte data) {
  uint32_t irqState = save_and_disable_interrupts();
  // R/W low, data and address out
  sio_hw->gpio_clr = RPU_RW_MASK;
  sio_hw->gpio_oe_set = RPU_DATA_MASK;
  sio_hw->gpio_out = (sio_hw->gpio_out & ~(RPU_DATA_MASK | RPU_ADDRESS_MASK)) |
                     data | (((uint32_t)address << RPU_ADDRESS_SHIFT) & RPU_ADDRESS_MASK);

  // Wait until the clock is high, then move on after the falling edge
  RPU_WaitPhi2High();
  RPU_WaitPhi2Low();

  // Pulse VMA over one clock cycle
  sio_hw->gpio_set = RPU_VMA_MASK;
  RPU_WaitPhi2High();
  RPU_WaitPhi2Low();
  RPU_WaitPhi2High();
  sio_hw->gpio_clr = RPU_VMA_MASK;

  // Unset address lines, R/W back high, data pins to input
  sio_hw->gpio_clr = RPU_ADDRESS_MASK;
  sio_hw->gpio_set = RPU_RW_MASK;
  sio_hw->gpio_oe_clr = RPU_DATA_MASK;
  restore_interrupts(irqState);
}

// REVISION 400 HARDWARE
byte RPU_BUS_FUNC(RPU_DataRead)(int address) {
  uint32_t irqState = save_and_disable_interrupts();
  uint32_t sample;
  for (;;) {
  // Data pins in, R/W high, address out
  sio_hw->gpio_oe_clr = RPU_DATA_MASK;
  sio_hw->gpio_set = RPU_RW_MASK;
  sio_hw->gpio_out = (sio_hw->gpio_out & ~RPU_ADDRESS_MASK) | (((uint32_t)address << RPU_ADDRESS_SHIFT) & RPU_ADDRESS_MASK);

  // Wait for a falling edge of the clock
  RPU_WaitPhi2High();
  RPU_WaitPhi2Low();

  // Pulse VMA over one clock cycle. The RP is fast enough to read before the PIA drives the bus
  // (Rev 4 reads just after the rising edge), so sample continuously while phi2 is high and keep
  // the last sample taken before the falling edge, when the data is sure to be valid.
  sio_hw->gpio_set = RPU_VMA_MASK;
  RPU_WaitPhi2High();
  sample = RPU_BusIn();
  uint32_t now, highSamples = 0;
  while ((now = RPU_BusIn()) & RPU_PHI2_MASK) { sample = now; highSamples++; }
  RPU_WaitPhi2High();
  // Fewer than a handful of samples while phi2 was high: we arrived late, and the sample may be
  // from after the PIA let go of the bus. Finish this cycle and read again.
  if (highSamples >= 8) break;
  sio_hw->gpio_clr = RPU_VMA_MASK;
  sio_hw->gpio_clr = RPU_ADDRESS_MASK;
  sio_hw->gpio_out = (sio_hw->gpio_out & ~RPU_ADDRESS_MASK) | (((uint32_t)address << RPU_ADDRESS_SHIFT) & RPU_ADDRESS_MASK);
  RPU_WaitPhi2Low();
  }

  byte inputData = (byte)(sample & RPU_DATA_MASK);

  // VMA off, R/W low (as Rev 4 leaves it), address lines cleared
  sio_hw->gpio_clr = RPU_VMA_MASK;
  sio_hw->gpio_clr = RPU_RW_MASK;
  sio_hw->gpio_clr = RPU_ADDRESS_MASK;
  restore_interrupts(irqState);

  return inputData;
}

#elif (RPU_OS_HARDWARE_REV==401)

// PinNode 400-IC: RP2350B (Waveshare Core2350B) in the CPU socket of a Williams System 3-7 MPU, Rev 102
// style, with no CPU on the board at all: the RP is the bus master. Socket pin n goes to its own GPIO:
// GPIO0 = IRQ (pin 4), GPIO1 = VMA (5), GPIO2 = NMI (6), GPIO4-19 = A0-A15 (9-20, 22-25),
// GPIO20-27 = D7-D0 (26-33, reversed), GPIO28 = R/W (34), GPIO31 = E / phi2 (37), GPIO34 = RESET (40).
// On a 6802/6808 board nothing else makes a clock, so each bus cycle drives E itself, as Rev 102 does;
// on a 6800 board the MPU's own phi2 runs and the cycles follow it (CheckForMPUClock decides at boot).
// RESET is the MPU's own power-on reset, which also resets the PIAs: the RP only reads it.
// GPIO0-39 are 5 V tolerant; the bus pins keep their internal pulls off (RP2350 erratum E9).

#if !defined(ARDUINO_ARCH_RP2040)
#error "RPU_OS_HARDWARE_REV 401 is the RP2350 (PinNode 400-IC): build for a Raspberry Pi RP2350 board"
#endif
#if (RPU_MPU_ARCHITECTURE<10)
#error "RPU_OS_HARDWARE_REV 401 (the 400-IC) is for Williams MPUs so far: Bally and Stern use the 400-J5 (Rev 400)"
#endif

#define RPU_VMA_PIN                   1
#define RPU_ADDRESS_SHIFT             4
#define RPU_ADDRESS_MASK              (0xFFFFUL << RPU_ADDRESS_SHIFT)
#define RPU_DATA_MASK                 (0xFFUL << 20)
#define RPU_RW_PIN                    28
#define RPU_PHI2_PIN                  31
#define RPU_MPU_RESET_PIN             34
#define RPU_VMA_MASK                  (1UL << RPU_VMA_PIN)
#define RPU_RW_MASK                   (1UL << RPU_RW_PIN)
#define RPU_PHI2_MASK                 (1UL << RPU_PHI2_PIN)
#define RPU_SWITCH_PIN                255   // no boot selector switch on the 400
#define RPU_BUFFER_DISABLE            255   // no buffers
#define RPU_HALT_PIN                  255   // no CPU to halt
#define RPU_RESET_PIN                 255   // never driven: see above
#define RPU_BA_PIN                    255
#define RPU_DIAGNOSTIC_PIN            255
#define RPU_PINS_OUTPUT true
#define RPU_PINS_INPUT false

// E's half periods when the RP makes the clock: the MC6821's minimums are 450 ns high and 430 ns low
// at 1 MHz, so 500 ns each (the 6808's own E is about 560 ns each, from its 3.58 MHz crystal)
#ifndef RPU_401_E_HALF_NS
#define RPU_401_E_HALF_NS             500
#endif
#define RPU_401_CYCLES(ns)            ((uint32_t)((uint64_t)F_CPU * (ns) / 1000000000ULL))

// D7-D0 are on GPIO20-27, D0 on GPIO27: a bit reverse lines them up
static inline uint32_t RPU_Reverse32(uint32_t v) {
#if defined(__thumb__)
  uint32_t r;
  asm("rbit %0, %1" : "=r"(r) : "r"(v));
  return r;
#else
  uint32_t r = 0;                                       // the host test rig
  for (int i = 0; i < 32; i++) r |= ((v >> i) & 1) << (31 - i);
  return r;
#endif
}
static inline uint32_t RPU_DataToPins(byte data) { return RPU_Reverse32(data) >> 4; }
static inline byte RPU_PinsToData(uint32_t pins) { return (byte)RPU_Reverse32(pins << 4); }

static inline uint32_t RPU_BusIn() { return sio_hw->gpio_in; }
static inline void RPU_WaitPhi2High() { while (!(sio_hw->gpio_in & RPU_PHI2_MASK)); }
static inline void RPU_WaitPhi2Low() { while (sio_hw->gpio_in & RPU_PHI2_MASK); }
static inline void RPU_EHalf() { busy_wait_at_least_cycles(RPU_401_CYCLES(RPU_401_E_HALF_NS)); }
// The bus cycles run from RAM, as on Rev 400: a flash cache miss mid-cycle would stretch E
#define RPU_BUS_FUNC(name) __not_in_flash_func(name)

// REVISION 401 HARDWARE
// Quiet the bus the moment the RP starts, before the sketch's setup() runs: VMA low (nothing on the MPU
// board is selected), E low, R/W high (read). GPIO39 is the XIAO's ATTN line to us: pulled up, off the RP's
// reset pull-down, so it never sits low while the XIAO boots.
static struct RPU_EarlyQuiet {
  RPU_EarlyQuiet() {
    gpio_init(RPU_VMA_PIN); gpio_put(RPU_VMA_PIN, 0); gpio_set_dir(RPU_VMA_PIN, true);
    gpio_init(RPU_RW_PIN); gpio_put(RPU_RW_PIN, 1); gpio_set_dir(RPU_RW_PIN, true);
    gpio_init(RPU_PHI2_PIN); gpio_disable_pulls(RPU_PHI2_PIN);
    gpio_init(39); gpio_pull_up(39);
  }
} RPU_EarlyQuietAtStart;

// REVISION 401 HARDWARE
// Tells the PinNode library it's on the 400-IC, whose link to the XIAO uses different pins from the 400-J5's
extern "C" int PinNode400_Board() { return 401; }

// REVISION 401 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_ADDRESS_MASK;
  else sio_hw->gpio_oe_clr = RPU_ADDRESS_MASK;
}

// REVISION 401 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_DATA_MASK;
  else sio_hw->gpio_oe_clr = RPU_DATA_MASK;
}

// REVISION 401 HARDWARE
// One-time setup of every bus pin: SIO function, no pulls, inputs to start with (VMA and R/W are already
// driven). GPIO3 (socket pin 8, 5 V) and GPIO29/30 (pins 35/36, through 1k) are left alone.
void RPU_InitBusPins() {
  static const byte busPins[] = {0, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
                                 20, 21, 22, 23, 24, 25, 26, 27, RPU_PHI2_PIN, RPU_MPU_RESET_PIN};
  for (byte pin : busPins) {
    gpio_init(pin);
    gpio_disable_pulls(pin);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
  }
  gpio_set_drive_strength(RPU_VMA_PIN, GPIO_DRIVE_STRENGTH_8MA);
  gpio_set_drive_strength(RPU_RW_PIN, GPIO_DRIVE_STRENGTH_8MA);
}

// REVISION 401 HARDWARE
// A running clock on pin 37 means a 6800 board (its clock chip makes phi2); none means a 6802/6808 board,
// where the CPU made E and now the RP does
boolean CheckForMPUClock() {
  sio_hw->gpio_oe_clr = RPU_PHI2_MASK;
  unsigned long startTime = millis();
  int sawClockLow = 0, sawClockHigh = 0;
  while (millis() < (startTime + 10)) {
    if (RPU_BusIn() & RPU_PHI2_MASK) sawClockHigh += 1;
    else sawClockLow += 1;
  }
  return sawClockLow > 25 && sawClockHigh > 25;
}

// REVISION 401 HARDWARE
// The MPU's power-on reset holds the PIAs in reset: wait for it to let go (up to 2 s)
void RPU_WaitForMPUReset() {
  unsigned long startTime = millis();
  while (!gpio_get(RPU_MPU_RESET_PIN) && (millis() - startTime) < 2000);
}

// REVISION 401 HARDWARE
// Each bus cycle runs with interrupts held off, as on Rev 400, and is one 6800 bus cycle: address, R/W
// and VMA set up while E is low, the access while E is high, latched as E falls, then VMA down while E is
// low again. (Rev 102 gives the PIAs a second E pulse and drops VMA inside it; one whole cycle is the
// datasheet's.) The 6802/6808 board has no clock of its own, so here the RP makes this one E pulse.
void RPU_BUS_FUNC(RPU_DataWrite)(int address, byte data) {
  uint32_t irqState = save_and_disable_interrupts();
  // R/W low, data and address out
  sio_hw->gpio_clr = RPU_RW_MASK;
  sio_hw->gpio_oe_set = RPU_DATA_MASK;
  sio_hw->gpio_out = (sio_hw->gpio_out & ~(RPU_DATA_MASK | RPU_ADDRESS_MASK)) |
                     RPU_DataToPins(data) | (((uint32_t)address << RPU_ADDRESS_SHIFT) & RPU_ADDRESS_MASK);

  if (UsesM6800Processor) {
    // After a falling edge, VMA up for one whole phi2 cycle
    RPU_WaitPhi2High();
    RPU_WaitPhi2Low();
    sio_hw->gpio_set = RPU_VMA_MASK;
    RPU_WaitPhi2High();
    RPU_WaitPhi2Low();                                  // the write is latched on this falling edge
  } else {
    sio_hw->gpio_set = RPU_VMA_MASK; RPU_EHalf();
    sio_hw->gpio_set = RPU_PHI2_MASK; RPU_EHalf();
    sio_hw->gpio_clr = RPU_PHI2_MASK;                   // the write is latched on this falling edge
  }
  sio_hw->gpio_clr = RPU_VMA_MASK;

  // Unset address lines, R/W back high, data pins to input
  sio_hw->gpio_clr = RPU_ADDRESS_MASK;
  sio_hw->gpio_set = RPU_RW_MASK;
  sio_hw->gpio_oe_clr = RPU_DATA_MASK;
  restore_interrupts(irqState);
}

// REVISION 401 HARDWARE
byte RPU_BUS_FUNC(RPU_DataRead)(int address) {
  uint32_t irqState = save_and_disable_interrupts();
  uint32_t sample;
  // Data pins in, R/W high, address out
  sio_hw->gpio_oe_clr = RPU_DATA_MASK;
  sio_hw->gpio_set = RPU_RW_MASK;
  sio_hw->gpio_out = (sio_hw->gpio_out & ~RPU_ADDRESS_MASK) | (((uint32_t)address << RPU_ADDRESS_SHIFT) & RPU_ADDRESS_MASK);

  if (UsesM6800Processor) {
    // As Rev 400: keep the last sample taken before phi2 falls, and go round again if we arrived late
    for (;;) {
      RPU_WaitPhi2High();
      RPU_WaitPhi2Low();
      sio_hw->gpio_set = RPU_VMA_MASK;
      RPU_WaitPhi2High();
      sample = RPU_BusIn();
      uint32_t now, highSamples = 0;
      while ((now = RPU_BusIn()) & RPU_PHI2_MASK) { sample = now; highSamples++; }
      if (highSamples >= 8) break;
      sio_hw->gpio_clr = RPU_VMA_MASK;
    }
  } else {
    sio_hw->gpio_set = RPU_VMA_MASK; RPU_EHalf();
    sio_hw->gpio_set = RPU_PHI2_MASK; RPU_EHalf();
    sample = RPU_BusIn();                               // the end of E high: the PIA's data is valid
    sio_hw->gpio_clr = RPU_PHI2_MASK;
  }

  // VMA off, R/W low (as Rev 102 leaves it), address lines cleared
  sio_hw->gpio_clr = RPU_VMA_MASK;
  sio_hw->gpio_clr = RPU_RW_MASK;
  sio_hw->gpio_clr = RPU_ADDRESS_MASK;
  restore_interrupts(irqState);

  return RPU_PinsToData(sample);
}

#elif (RPU_OS_HARDWARE_REV==402)

// PinNode 400-IC in the CPU socket of a Bally 6803 MPU (A084-91786, Strange Science and later), with the
// 6803 out and the 400-IC set for a 6803 socket (JP-A bridged 2-3, JP-B bridged). The RP is the bus master
// and runs the 6803's multiplexed bus (mode 2): A15-A8 on their own pins, A7-A0 on AD7-AD0 latched by the
// board's 74LS373 on AS, then data on AD7-AD0 while E is high (E enables the '245, the ROMs, the RAM and
// the PIAs). Socket pin n -> GPIO: 4 NMI = 0, 5 IRQ1 = 1, 6 RST~ = 2, 8 P20 = 3 (through 1k), 9-12 P21-P24
// = 4-7, 13-20 P10-P17 = 8-15, 22-29 A15-A8 = 16-23, 30-37 AD7-AD0 = 24-31, 38 R/W = 32, 39 AS = 33,
// 40 E = 34. Both A15-A8 and AD7-AD0 come out bit-reversed: rbit lines them up.
// RST~ is the board's power-on reset, which also resets both PIAs: the RP only reads it.

#if !defined(ARDUINO_ARCH_RP2040)
#error "RPU_OS_HARDWARE_REV 402 is the RP2350 (PinNode 400-IC): build for a Raspberry Pi RP2350 board"
#endif

#define RPU_6803_NMI_PIN              0
#define RPU_6803_IRQ1_PIN             1
#define RPU_MPU_RESET_PIN             2
#define RPU_6803_P20_PIN              3     // zero-cross phase A, while P21 is low
#define RPU_6803_P21_PIN              4
#define RPU_6803_P24_PIN              7     // sound strobe (J5-15)
#define RPU_6803_P10_PIN              8     // P10-P17 on GPIO8-15: sound data (J5-7 to J5-14)
#define RPU_RW_PIN                    32
#define RPU_6803_AS_PIN               33
#define RPU_6803_E_PIN                34
#define RPU_6803_HI_MASK              (0xFFUL << 16)
#define RPU_6803_AD_MASK              (0xFFUL << 24)
#define RPU_PINS_OUTPUT true
#define RPU_PINS_INPUT false

static inline uint32_t RPU_Reverse32(uint32_t v) {
#if defined(__thumb__)
  uint32_t r;
  asm("rbit %0, %1" : "=r"(r) : "r"(v));
  return r;
#else
  uint32_t r = 0;                                       // the host test rig
  for (int i = 0; i < 32; i++) r |= ((v >> i) & 1) << (31 - i);
  return r;
#endif
}
static inline uint32_t RPU_6803_ADPins(byte v) { return RPU_Reverse32(v); }
static inline uint32_t RPU_6803_HiPins(byte v) { return RPU_Reverse32(v) >> 8; }
static inline byte RPU_6803_PinsToData(uint32_t pins) { return (byte)RPU_Reverse32(pins & RPU_6803_AD_MASK); }
static inline void RPU_6803_WaitNs(uint32_t ns) { busy_wait_at_least_cycles((uint32_t)((uint64_t)F_CPU * ns / 1000000000ULL)); }
#define RPU_BUS_FUNC(name) __not_in_flash_func(name)

// REVISION 402 HARDWARE
// Quiet the bus the moment the RP starts: R/W high (read), AS and E low. GPIO39 is the XIAO's ATTN line.
static struct RPU_EarlyQuiet {
  RPU_EarlyQuiet() {
    gpio_init(RPU_RW_PIN); gpio_put(RPU_RW_PIN, 1); gpio_set_dir(RPU_RW_PIN, true);
    gpio_init(RPU_6803_AS_PIN); gpio_put(RPU_6803_AS_PIN, 0); gpio_set_dir(RPU_6803_AS_PIN, true);
    gpio_init(RPU_6803_E_PIN); gpio_put(RPU_6803_E_PIN, 0); gpio_set_dir(RPU_6803_E_PIN, true);
    gpio_init(39); gpio_pull_up(39);
  }
} RPU_EarlyQuietAtStart;

// REVISION 402 HARDWARE
// The same 400-IC as Rev 401: the PinNode library's link to the XIAO uses its pins
extern "C" int PinNode400_Board() { return 401; }

// REVISION 402 HARDWARE
// Every socket pin a plain input, no pulls (erratum E9), then the bus outputs: A15-A8 driven, AD driven
// only inside a cycle, R/W, AS and E; P21 low so phase A reaches P20; the sound port (Port 1, P24) as the
// game leaves it. P22 (the LED) stays an input.
void RPU_InitBusPins() {
  for (int pin = 0; pin <= 31; pin++) {
    if (pin == RPU_6803_P21_PIN) continue;
    gpio_init(pin);
    gpio_set_dir(pin, false);
    gpio_disable_pulls(pin);
  }
  for (int pin = 16; pin <= 31; pin++) {
    gpio_put(pin, 0);
    gpio_set_drive_strength(pin, GPIO_DRIVE_STRENGTH_8MA);
  }
  sio_hw->gpio_oe_set = RPU_6803_HI_MASK;
  gpio_set_drive_strength(RPU_RW_PIN, GPIO_DRIVE_STRENGTH_8MA);
  gpio_set_drive_strength(RPU_6803_AS_PIN, GPIO_DRIVE_STRENGTH_8MA);
  gpio_set_drive_strength(RPU_6803_E_PIN, GPIO_DRIVE_STRENGTH_8MA);
  gpio_init(RPU_6803_P21_PIN); gpio_put(RPU_6803_P21_PIN, 0); gpio_set_dir(RPU_6803_P21_PIN, true);
  // The sound port, as the game leaves it between commands: Port 1 (P10-P17) at 0, P24 high
  for (int pin = RPU_6803_P10_PIN; pin < RPU_6803_P10_PIN + 8; pin++) {
    gpio_init(pin); gpio_put(pin, 0); gpio_set_dir(pin, true);
  }
  gpio_init(RPU_6803_P24_PIN); gpio_put(RPU_6803_P24_PIN, 1); gpio_set_dir(RPU_6803_P24_PIN, true);
}

// REVISION 402 HARDWARE
// A command to the sound board on J5 (a Turbo Cheap Squeak on Strange Science), sent as the game ROM sends
// it: the byte on Port 1, P24 low ~35 us then high (the strobe), ~150 us, the high nibble on Port 1, ~150 us.
// The board reads the command a nibble at a time. 0 isn't a command.
void RPU_PlaySound6803(byte soundByte) {
  if (!soundByte) return;
  const uint32_t port1Mask = 0xFFUL << RPU_6803_P10_PIN;
  // Interrupts off for the whole command, as the ROM does: RPU's 1 ms interrupt landing inside it stretched
  // the strobe or the gap before the high nibble, and the board sometimes played another sound
  uint32_t irqState = save_and_disable_interrupts();
  sio_hw->gpio_out = (sio_hw->gpio_out & ~port1Mask) | ((uint32_t)soundByte << RPU_6803_P10_PIN);
  gpio_put(RPU_6803_P24_PIN, 0);
  busy_wait_us_32(35);
  gpio_put(RPU_6803_P24_PIN, 1);
  busy_wait_us_32(150);
  sio_hw->gpio_out = (sio_hw->gpio_out & ~port1Mask) | ((uint32_t)(soundByte >> 4) << RPU_6803_P10_PIN);
  busy_wait_us_32(150);
  restore_interrupts(irqState);
}

// REVISION 402 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_6803_HI_MASK;
  else sio_hw->gpio_oe_clr = RPU_6803_HI_MASK;
}

// REVISION 402 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {
  if (pinsOutput) sio_hw->gpio_oe_set = RPU_6803_AD_MASK;
  else sio_hw->gpio_oe_clr = RPU_6803_AD_MASK;
}

// REVISION 402 HARDWARE
// The board's power-on reset holds the PIAs in reset: wait for it to let go (up to 2 s)
void RPU_WaitForMPUReset() {
  unsigned long startTime = millis();
  while (!gpio_get(RPU_MPU_RESET_PIN) && (millis() - startTime) < 2000);
}

// REVISION 402 HARDWARE
// One mode-2 bus cycle, with interrupts held off: R/W, then the address out with AS high 150 ns, AS falls
// (the '373 latches A7-A0), AD turns round (a write puts its data out), E high 500 ns (the 6803's own E
// is about 560 ns), the read sampled at the end of it, E falls (the PIA latches a write), then let go.
// Measured on Strange Science 2026-11-09: 0 bad reads in 2988.
static byte RPU_BUS_FUNC(RPU_6803_BusCycle)(int address, boolean write, byte data) {
  uint32_t irqState = save_and_disable_interrupts();
  gpio_put(RPU_RW_PIN, !write);
  sio_hw->gpio_out = (sio_hw->gpio_out & ~(RPU_6803_AD_MASK | RPU_6803_HI_MASK)) |
                     RPU_6803_ADPins(address & 0xFF) | RPU_6803_HiPins((address >> 8) & 0xFF);
  sio_hw->gpio_oe_set = RPU_6803_AD_MASK;
  gpio_put(RPU_6803_AS_PIN, 1); RPU_6803_WaitNs(150);
  gpio_put(RPU_6803_AS_PIN, 0); RPU_6803_WaitNs(60);
  if (write) sio_hw->gpio_out = (sio_hw->gpio_out & ~RPU_6803_AD_MASK) | RPU_6803_ADPins(data);
  else sio_hw->gpio_oe_clr = RPU_6803_AD_MASK;
  RPU_6803_WaitNs(250);
  gpio_put(RPU_6803_E_PIN, 1); RPU_6803_WaitNs(500);
  uint32_t sample = sio_hw->gpio_in;
  gpio_put(RPU_6803_E_PIN, 0); RPU_6803_WaitNs(60);
  sio_hw->gpio_oe_clr = RPU_6803_AD_MASK;
  gpio_put(RPU_RW_PIN, 1);
  restore_interrupts(irqState);
  return RPU_6803_PinsToData(sample);
}

// REVISION 402 HARDWARE
// An address below $1000 with A5 and A6 both set selects both PIAs at once (they'd fight on a read): such
// a cycle is never run, and a read of it returns $FF
static inline boolean RPU_6803_BothPIAs(int address) { return (address & 0xF000) == 0 && (address & 0x60) == 0x60; }

void RPU_DataWrite(int address, byte data) {
  if (RPU_6803_BothPIAs(address)) return;
  RPU_6803_BusCycle(address, true, data);
}

// REVISION 402 HARDWARE
byte RPU_DataRead(int address) {
  if (RPU_6803_BothPIAs(address)) return 0xFF;
  return RPU_6803_BusCycle(address, false, 0);
}

#elif (RPU_OS_HARDWARE_REV==200)

#if defined(__AVR_ATmega328P__)
#error "RPU_OS_HARDWARE_REV >100 requires ATMega2560, check RPU_Config.h and adjust settings"
#endif

#define RPU_DIAGNOSTIC_PIN            10
#define RPU_VMA_PIN                   255
#define RPU_RW_PIN                    255
#define RPU_PHI2_PIN                  255
#define RPU_SWITCH_PIN                11
#define RPU_BUFFER_DISABLE            255
#define RPU_HALT_PIN                  255
#define RPU_RESET_PIN                 255
#define RPU_BA_PIN                    255
#define RPU_DISABLE_PHI_FROM_MPU      255
#define RPU_DISABLE_PHI_FROM_CPU      255
#define RPU_BOARD_SEL_0               255
#define RPU_BOARD_SEL_1               255
#define RPU_BOARD_SEL_2               255
#define RPU_BOARD_SEL_3               255
#define RPU_PINS_OUTPUT       true
#define RPU_PINS_INPUT        false

// REVISION 200 HARDWARE
void RPU_SetAddressPinsDirection(boolean pinsOutput) {}
// REVISION 200 HARDWARE
void RPU_SetDataPinsDirection(boolean pinsOutput) {}
// REVISION 200 HARDWARE
void RPU_DataWrite(int address, byte data) {}
// REVISION 200 HARDWARE
byte RPU_DataRead(int address) { return 0; }


#else
#error "RPU Hardware Definition Not Recognized"
#endif


#if (RPU_MPU_ARCHITECTURE==5)

// RPU_MPU_ARCHITECTURE == 5 (Bally 6803)
// The order and values come from the bring-up on Strange Science (2026-11-09) and the game's own reset
void RPU_6803_InitializePIAs() {
  // PIA1 (U11's place): PB solenoids out, CB2 high (the 74LS154 off until a solenoid fires);
  // PA display segments out, blank (bit 0 is the inverted h+j), CA2 low as the game sets it
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U11_B, DEFAULT_SOLENOID_STATE);
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x38);
  RPU_DataWrite(ADDRESS_U11_B, 0xFF);
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x3C);
  CurrentSolenoidByte = DEFAULT_SOLENOID_STATE;
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, 0x34);
  RPU_DataWrite(ADDRESS_U11_A, 0x01);
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, 0x30);
  RPU_DataWrite(ADDRESS_U11_A, 0xFF);
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, 0x34);
  // PIA0 (U10's place): PB switch returns in. CB2 (the lamp 4514s' strobe) goes low while PA is still an
  // input pulled high, latching lamp column 15 (unconnected)
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x30);
  RPU_DataWrite(ADDRESS_U10_B, 0x00);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);
  // PA out: lamp banks inhibited (PA5-7 high), no switch strobe; CA2 high (displays shown);
  // CA1 flags the self-test switch (no IRQ: nothing pulls the 6803's IRQ1 line)
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U10_A, 0xEF);
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, 0x38);
  RPU_DataWrite(ADDRESS_U10_A, 0xFF);
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);
  RPU_DataRead(ADDRESS_U10_A);
  RPU_DataRead(ADDRESS_U10_B);
}

// RPU_MPU_ARCHITECTURE == 5
unsigned long RPU_TestPIAs() {
  unsigned long piaErrors = 0;
  // Bits 6-7 are the interrupt flags (CA1 self-test, CB1 zero crossing)
  if ((RPU_DataRead(ADDRESS_U10_A_CONTROL) & 0x3F) != 0x3C) piaErrors |= RPU_RET_U10_PIA_ERROR;
  if ((RPU_DataRead(ADDRESS_U10_B_CONTROL) & 0x3F) != 0x34) piaErrors |= RPU_RET_U10_PIA_ERROR;
  if ((RPU_DataRead(ADDRESS_U11_A_CONTROL) & 0x3F) != 0x34) piaErrors |= RPU_RET_U11_PIA_ERROR;
  if ((RPU_DataRead(ADDRESS_U11_B_CONTROL) & 0x3F) != 0x3C) piaErrors |= RPU_RET_U11_PIA_ERROR;
  return piaErrors;
}

#elif (RPU_MPU_ARCHITECTURE<10)

// RPU_MPU_ARCHITECTURE < 10
void TestLightOn() {
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) | 0x08);
}

// RPU_MPU_ARCHITECTURE < 10
void TestLightOff() {
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) & 0xF7);
}

// RPU_MPU_ARCHITECTURE < 10
void InitializeU10PIA() {
  // CA1 - Self Test Switch
  // CB1 - zero crossing detector
  // CA2 - NOR'd with display latch strobe
  // CB2 - lamp strobe 1
  // PA0-7 - output for switch bank, lamps, and BCD
  // PB0-7 - switch returns

  RPU_DataWrite(ADDRESS_U10_A_CONTROL, 0x38);
  // Set up U10A as output
  RPU_DataWrite(ADDRESS_U10_A, 0xFF);
  // Set bit 3 to write data
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, RPU_DataRead(ADDRESS_U10_A_CONTROL) | 0x04);
  // Store F0 in U10A Output
  RPU_DataWrite(ADDRESS_U10_A, 0xF0);

  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x33);
  // Set up U10B as input
  RPU_DataWrite(ADDRESS_U10_B, 0x00);
  // Set bit 3 so future reads will read data
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) | 0x04);

}

#ifdef RPU_OS_USE_DIP_SWITCHES
// RPU_MPU_ARCHITECTURE < 10
void ReadDipSwitches() {
  byte backupU10A = RPU_DataRead(ADDRESS_U10_A);
  byte backupU10BControl = RPU_DataRead(ADDRESS_U10_B_CONTROL);

  // Turn on Switch strobe 5 & Read Switches
  RPU_DataWrite(ADDRESS_U10_A, 0x20);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, backupU10BControl & 0xF7);
  // Wait for switch capacitors to charge
  delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);
  DipSwitches[0] = RPU_DataRead(ADDRESS_U10_B);

  // Turn on Switch strobe 6 & Read Switches
  RPU_DataWrite(ADDRESS_U10_A, 0x40);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, backupU10BControl & 0xF7);
  // Wait for switch capacitors to charge
  delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);
  DipSwitches[1] = RPU_DataRead(ADDRESS_U10_B);

  // Turn on Switch strobe 7 & Read Switches
  RPU_DataWrite(ADDRESS_U10_A, 0x80);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, backupU10BControl & 0xF7);
  // Wait for switch capacitors to charge
  delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);
  DipSwitches[2] = RPU_DataRead(ADDRESS_U10_B);

  // Turn on U10 CB2 (strobe 8) and read switches
  RPU_DataWrite(ADDRESS_U10_A, 0x00);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, backupU10BControl | 0x08);
  // Wait for switch capacitors to charge
  delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);
  DipSwitches[3] = RPU_DataRead(ADDRESS_U10_B);

  RPU_DataWrite(ADDRESS_U10_B_CONTROL, backupU10BControl);
  RPU_DataWrite(ADDRESS_U10_A, backupU10A);
}
#endif

// RPU_MPU_ARCHITECTURE < 10
void InitializeU11PIA() {
  // CA1 - Display interrupt generator
  // CB1 - test connector pin 32
  // CA2 - lamp strobe 2
  // CB2 - solenoid bank select
  // PA0-7 - display digit enable
  // PB0-7 - solenoid data

  RPU_DataWrite(ADDRESS_U11_A_CONTROL, 0x30);
  // Set up U11A as output
  RPU_DataWrite(ADDRESS_U11_A, 0xFF);
  // Set bit 3 to write data
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) | 0x04);
  // Store 00 in U11A Output
  RPU_DataWrite(ADDRESS_U11_A, 0x00);

  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x30);
  // Set up U11B as output
  RPU_DataWrite(ADDRESS_U11_B, 0xFF);
  // Set bit 3 so future reads will read data
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, RPU_DataRead(ADDRESS_U11_B_CONTROL) | 0x04);
  // Store 9F in U11B Output
  RPU_DataWrite(ADDRESS_U11_B, DEFAULT_SOLENOID_STATE);
  CurrentSolenoidByte = DEFAULT_SOLENOID_STATE;

}

// RPU_MPU_ARCHITECTURE < 10
unsigned long RPU_TestPIAs() {
  unsigned long piaErrors = 0;

  byte piaResult = RPU_DataRead(ADDRESS_U10_A_CONTROL);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_U10_PIA_ERROR;
  piaResult = RPU_DataRead(ADDRESS_U10_B_CONTROL);
  if (piaResult != 0x37) piaErrors |= RPU_RET_U10_PIA_ERROR;

  piaResult = RPU_DataRead(ADDRESS_U11_A_CONTROL);
  if (piaResult != 0x35) piaErrors |= RPU_RET_U11_PIA_ERROR;
  piaResult = RPU_DataRead(ADDRESS_U11_B_CONTROL);
  if (piaResult != 0x34) piaErrors |= RPU_RET_U11_PIA_ERROR;

  return piaErrors;
}

#else

// RPU_MPU_ARCHITECTURE >= 10
void RPU_InitializePIAs() {
  RPU_DataWrite(PIA_DISPLAY_CONTROL_A, 0x31);
  RPU_DataWrite(PIA_DISPLAY_PORT_A, 0xFF);
  RPU_DataWrite(PIA_DISPLAY_CONTROL_A, 0x3D);
  RPU_DataWrite(PIA_DISPLAY_PORT_A, 0xC0);

  RPU_DataWrite(PIA_DISPLAY_CONTROL_B, 0x31);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, 0xFF);
  RPU_DataWrite(PIA_DISPLAY_CONTROL_B, 0x3D);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, 0x00);

  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_A, 0x00);
  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);

  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0x00);

  RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_LAMPS_PORT_A, 0xFF);
  RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x3C);
  RPU_DataWrite(PIA_LAMPS_PORT_A, 0xFF);

  RPU_DataWrite(PIA_LAMPS_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_LAMPS_PORT_B, 0xFF);
  RPU_DataWrite(PIA_LAMPS_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_LAMPS_PORT_B, 0x00);

#if (RPU_MPU_ARCHITECTURE<15)
  RPU_DataWrite(PIA_SOLENOID_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SOLENOID_PORT_A, 0xFF);
  RPU_DataWrite(PIA_SOLENOID_CONTROL_A, 0x3C);
#endif
  RPU_DataWrite(PIA_SOLENOID_PORT_A, 0x00);

#if (RPU_MPU_ARCHITECTURE<15)
  RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x30);
  RPU_DataWrite(PIA_SOLENOID_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x34);
  RPU_DataWrite(PIA_SOLENOID_PORT_B, 0x00);
#endif

#if (RPU_MPU_ARCHITECTURE==15)
  RPU_DataWrite(PIA_SOLENOID_11_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SOLENOID_11_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SOLENOID_11_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SOLENOID_11_PORT_B, 0x00);

  RPU_DataWrite(PIA_ALPHA_DISPLAY_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_A, 0xFF);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_CONTROL_A, 0x3C);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_A, 0x00);

  RPU_DataWrite(PIA_ALPHA_DISPLAY_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_B, 0xFF);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_B, 0x00);

  RPU_DataWrite(PIA_NUM_DISPLAY_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_NUM_DISPLAY_PORT_A, 0xFF);
  RPU_DataWrite(PIA_NUM_DISPLAY_CONTROL_A, 0x3C);
  RPU_DataWrite(PIA_NUM_DISPLAY_PORT_A, 0x00);

  RPU_DataWrite(PIA_SOUND_11_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SOUND_11_PORT_A, 0xFF);
  RPU_DataWrite(PIA_SOUND_11_CONTROL_A, 0x3C);
  RPU_DataWrite(PIA_SOUND_11_PORT_A, 0x00);

  RPU_DataWrite(PIA_WIDGET_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_WIDGET_PORT_B, 0xFF);
  RPU_DataWrite(PIA_WIDGET_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_WIDGET_PORT_B, 0x00);
#endif

#if (RPU_MPU_ARCHITECTURE==13)
  RPU_DataWrite(PIA_SOUND_COMMA_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SOUND_COMMA_PORT_A, 0xFF);
  RPU_DataWrite(PIA_SOUND_COMMA_CONTROL_A, 0x3C);
  RPU_DataWrite(PIA_SOUND_COMMA_PORT_A, 0x00);

  RPU_DataWrite(PIA_SOUND_COMMA_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SOUND_COMMA_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SOUND_COMMA_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SOUND_COMMA_PORT_B, 0x00);
#endif

}

// RPU_MPU_ARCHITECTURE >= 10
unsigned long RPU_TestPIAs() {
  unsigned long piaErrors = 0;

  byte piaResult = RPU_DataRead(PIA_DISPLAY_CONTROL_A);
  if (piaResult != 0x3D) {
    piaErrors |= RPU_RET_PIA_1_ERROR;
//    if (DEBUG_MESSAGES) Serial.write("* Error with Display PIA\n");
  } else {
//    if (DEBUG_MESSAGES) Serial.write("* No error with Display PIA\n");
  }
  piaResult = RPU_DataRead(PIA_DISPLAY_CONTROL_B);
  if (piaResult != 0x3D) piaErrors |= RPU_RET_PIA_1_ERROR;

  piaResult = RPU_DataRead(PIA_SWITCH_CONTROL_A);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_2_ERROR;
  piaResult = RPU_DataRead(PIA_SWITCH_CONTROL_B);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_2_ERROR;

  piaResult = RPU_DataRead(PIA_LAMPS_CONTROL_A);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_3_ERROR;
  piaResult = RPU_DataRead(PIA_LAMPS_CONTROL_B);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_3_ERROR;

  piaResult = RPU_DataRead(PIA_SOLENOID_CONTROL_A);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_4_ERROR;
  piaResult = RPU_DataRead(PIA_SOLENOID_CONTROL_B);
#if (RPU_MPU_ARCHITECTURE<15)
  // RPU_InitializePIAs leaves CB2 (the flipper enable) low, flippers off: $34, not $3C
  if (piaResult != 0x34) piaErrors |= RPU_RET_PIA_4_ERROR;
#else
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_4_ERROR;
#endif

#if (RPU_MPU_ARCHITECTURE==13)
  piaResult = RPU_DataRead(PIA_SOUND_COMMA_CONTROL_A);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_5_ERROR;
  piaResult = RPU_DataRead(PIA_SOUND_COMMA_CONTROL_B);
  if (piaResult != 0x3C) piaErrors |= RPU_RET_PIA_5_ERROR;
#endif

  return piaErrors;
}

// RPU_MPU_ARCHITECTURE >= 10
void RPU_SetBoardLEDs(boolean LED1, boolean LED2, byte BCDValue) {
  BoardLEDs = 0;
  if (BCDValue == 0xFF) {
    if (LED1) BoardLEDs |= 0x20;
    if (LED2) BoardLEDs |= 0x10;
  } else {
    BoardLEDs = BCDValue * 16;
  }
}

#endif


/******************************************************
 * 
 * 
 *    Switch Handling Functions
 *    
 *    
*******************************************************/

int SpaceLeftOnSwitchStack() {
  if (SwitchStackFirst >= SWITCH_STACK_SIZE || SwitchStackLast >= SWITCH_STACK_SIZE) return 0;
  if (SwitchStackLast >= SwitchStackFirst) return ((SWITCH_STACK_SIZE - 1) - (SwitchStackLast - SwitchStackFirst));
  return (SwitchStackFirst - SwitchStackLast) - 1;
}

void PushToSwitchStack(byte switchNumber) {
  if (switchNumber == SWITCH_STACK_EMPTY) return;

  // If the switch stack last index is out of range, then it's an error - return
  if (SpaceLeftOnSwitchStack() == 0) return;

  // Self test is a special case - there's no good way to debounce it
  // so if it's already first on the stack, ignore it
  if (switchNumber == SW_SELF_TEST_SWITCH) {
    //if (SwitchStackLast != SwitchStackFirst && SwitchStack[SwitchStackFirst] == SW_SELF_TEST_SWITCH) return;
    int lastAddedIndex = (SwitchStackLast == 0) ? SWITCH_STACK_SIZE - 1 : SwitchStackLast - 1;
    if ((SwitchStackLast != SwitchStackFirst) && SwitchStack[lastAddedIndex] == SW_SELF_TEST_SWITCH) return;
  }

  SwitchStack[SwitchStackLast] = switchNumber;

  SwitchStackLast += 1;
  if (SwitchStackLast == SWITCH_STACK_SIZE) {
    // If the end index is off the end, then wrap
    SwitchStackLast = 0;
  }
}

void RPU_PushToSwitchStack(byte switchNumber) {
  PushToSwitchStack(switchNumber);
}

byte RPU_PullFirstFromSwitchStack() {
  // If first and last are equal, there's nothing on the stack
  if (SwitchStackFirst == SwitchStackLast) return SWITCH_STACK_EMPTY;

  byte retVal = SwitchStack[SwitchStackFirst];

  SwitchStackFirst += 1;
  if (SwitchStackFirst >= SWITCH_STACK_SIZE) SwitchStackFirst = 0;

  return retVal;
}

boolean RPU_ReadSingleSwitchState(byte switchNum) {
#if (RPU_OS_HARDWARE_REV==200)
  return LISYSwitchStates[switchNum] ? true : false;
#endif

  if (switchNum >= MAX_NUM_SWITCHES) return false;

  int switchByte = switchNum / 8;
  int switchBit = switchNum % 8;
  if ( ((SwitchesNow[switchByte]) >> switchBit) & 0x01 ) return true;
  else return false;
}

boolean RPU_SetSwitchInversion(byte switchNum) {
  if (switchNum >= MAX_NUM_SWITCHES) return false;
  byte oldSwitchInverter = SwitchInverter[switchNum / 8];
  SwitchInverter[switchNum / 8] = oldSwitchInverter | (0x01 << (switchNum % 8));
  return true;
}

byte RPU_GetDipSwitches(byte index) {
#ifdef RPU_OS_USE_DIP_SWITCHES
  if (index > 3) return 0x00;
  return DipSwitches[index];
#else
  return 0x00 & index;
#endif
}

void RPU_SetupGameSwitches(int s_numSwitches, int s_numPrioritySwitches, PlayfieldAndCabinetSwitch *s_gameSwitchArray) {
  NumGameSwitches = s_numSwitches;
  NumGamePrioritySwitches = s_numPrioritySwitches;
  GameSwitches = s_gameSwitchArray;
}


#if (RPU_MPU_ARCHITECTURE<10)
// RPU_MPU_ARCHITECTURE < 10
void RPU_ClearUpDownSwitchState() {
  return;
}

// RPU_MPU_ARCHITECTURE < 10
boolean RPU_GetUpDownSwitchState() {
  return true;
}
#else
// RPU_MPU_ARCHITECTURE >= 10
void RPU_ClearUpDownSwitchState() {
  UpDownSwitch = false;
}

// RPU_MPU_ARCHITECTURE >= 10
boolean RPU_GetUpDownSwitchState() {
#if (RPU_OS_HARDWARE_REV==200)
  return LISYSwitchStates[65] ? false : true;
#endif  
  return UpDownSwitch;
}
#endif



/******************************************************
 * 
 * 
 *    Solenoid Handling Functions
 *    
 *    
*******************************************************/

int SpaceLeftOnSolenoidStack() {
  if (SolenoidStackFirst >= SOLENOID_STACK_SIZE || SolenoidStackLast >= SOLENOID_STACK_SIZE) return 0;
  if (SolenoidStackLast >= SolenoidStackFirst) return ((SOLENOID_STACK_SIZE - 1) - (SolenoidStackLast - SolenoidStackFirst));
  return (SolenoidStackFirst - SolenoidStackLast) - 1;
}


void RPU_PushToSolenoidStack(byte solenoidNumber, byte numPushes, boolean disableOverride) {
  if (solenoidNumber >= RPU_NUM_SOLENOIDS) return;

  // if the solenoid stack is disabled and this isn't an override push, then return
  if (!disableOverride && !SolenoidStackEnabled) return;

  // If the solenoid stack last index is out of range, then it's an error - return
  if (SpaceLeftOnSolenoidStack() == 0) return;

#if (RPU_OS_HARDWARE_REV==200)
  // For SA LISY, we only need to push once to the stack
  // because the MPU will handle the actual pulse width.
  numPushes = 1; 
#endif

  for (int count = 0; count < numPushes; count++) {
    SolenoidStack[SolenoidStackLast] = solenoidNumber;

    SolenoidStackLast += 1;
    if (SolenoidStackLast == SOLENOID_STACK_SIZE) {
      // If the end index is off the end, then wrap
      SolenoidStackLast = 0;
    }
    // If the stack is now full, return
    if (SpaceLeftOnSolenoidStack() == 0) return;
  }
}

void PushToFrontOfSolenoidStack(byte solenoidNumber, byte numPushes) {
  // If the stack is full, return
  if (SpaceLeftOnSolenoidStack() == 0  || !SolenoidStackEnabled) return;

  for (int count = 0; count < numPushes; count++) {
    if (SolenoidStackFirst == 0) SolenoidStackFirst = SOLENOID_STACK_SIZE - 1;
    else SolenoidStackFirst -= 1;
    SolenoidStack[SolenoidStackFirst] = solenoidNumber;
    if (SpaceLeftOnSolenoidStack() == 0) return;
  }

}

byte PullFirstFromSolenoidStack() {
  // If first and last are equal, there's nothing on the stack
  if (SolenoidStackFirst == SolenoidStackLast) return SOLENOID_STACK_EMPTY;

  byte retVal = SolenoidStack[SolenoidStackFirst];

  SolenoidStackFirst += 1;
  if (SolenoidStackFirst >= SOLENOID_STACK_SIZE) SolenoidStackFirst = 0;

  return retVal;
}

boolean RPU_PushToTimedSolenoidStack(byte solenoidNumber, byte numPushes, unsigned long whenToFire, boolean disableOverride) {
  for (int count = 0; count < TIMED_SOLENOID_STACK_SIZE; count++) {
    if (!TimedSolenoidStack[count].inUse) {
      TimedSolenoidStack[count].inUse = true;
      TimedSolenoidStack[count].pushTime = whenToFire;
      TimedSolenoidStack[count].disableOverride = disableOverride;
      TimedSolenoidStack[count].solenoidNumber = solenoidNumber;
      TimedSolenoidStack[count].numPushes = numPushes;
      return true;
    }
  }
  return false;
}

#if (RPU_OS_HARDWARE_REV==400) || (RPU_OS_HARDWARE_REV==401) || (RPU_OS_HARDWARE_REV==402)
extern "C" uint8_t _EEPROM_start;                                       // the EEPROM sector in flash
extern "C" void PinNode400_AudioHold(bool hold) __attribute__((weak));  // RPU400Audio: silence around it
#endif

void RPU_UpdateTimedSolenoidStack(unsigned long curTime) {
#if (RPU_OS_HARDWARE_REV==400) || (RPU_OS_HARDWARE_REV==401) || (RPU_OS_HARDWARE_REV==402)
  // Save EEPROM changes to flash every 5 s at most (commit() does nothing when nothing changed).
  // Writing flash stops interrupts for tens of ms, so only while no coil is firing. Here because
  // every game calls this each loop, through RPU_Update or (older games) directly.
  static unsigned long lastEEPROMCommit = 0;
  if ((curTime - lastEEPROMCommit) > 5000 && SolenoidStackFirst == SolenoidStackLast &&
      (CurrentSolenoidByte & 0x0F) == SOL_NONE) {
    lastEEPROMCommit = curTime;
    // Only when something changed (commit() would skip it anyway): writing flash pauses the core that
    // plays the sound, and a paused DAC pops, so the player goes silent around it
    if (EEPROM.length() && memcmp(EEPROM.getConstDataPtr(), &_EEPROM_start, EEPROM.length())) {
      if (PinNode400_AudioHold) PinNode400_AudioHold(true);
      EEPROM.commit();
      if (PinNode400_AudioHold) PinNode400_AudioHold(false);
    }
  }
#endif
  for (int count = 0; count < TIMED_SOLENOID_STACK_SIZE; count++) {
    if (TimedSolenoidStack[count].inUse && TimedSolenoidStack[count].pushTime < curTime) {
      RPU_PushToSolenoidStack(TimedSolenoidStack[count].solenoidNumber, TimedSolenoidStack[count].numPushes, TimedSolenoidStack[count].disableOverride);
      TimedSolenoidStack[count].inUse = false;
    }
  }
}

#if (RPU_MPU_ARCHITECTURE<10)

// RPU_MPU_ARCHITECTURE < 10
void RPU_SetCoinLockout(boolean lockoutOff, byte solbit) {
  noInterrupts();
  if (!lockoutOff) {
    CurrentSolenoidByte = CurrentSolenoidByte & ~solbit;
  } else {
    CurrentSolenoidByte = CurrentSolenoidByte | solbit;
  }
  RPU_WRITE_SOLENOID_BYTE();
  interrupts();
}

// RPU_MPU_ARCHITECTURE < 10
void RPU_SetDisableFlippers(boolean disableFlippers, byte solbit) {
  noInterrupts();
  if (disableFlippers) {
    CurrentSolenoidByte = CurrentSolenoidByte | solbit;
  } else {
    CurrentSolenoidByte = CurrentSolenoidByte & ~solbit;
  }

  RPU_WRITE_SOLENOID_BYTE();
  interrupts();
}

// RPU_MPU_ARCHITECTURE < 10
boolean RPU_GetDisableFlippers(byte solbit) {
  return  (CurrentSolenoidByte & solbit) ? true : false;
}

// RPU_MPU_ARCHITECTURE < 10
void RPU_SetContinuousSolenoidBit(boolean bitOn, byte solbit) {
  noInterrupts();
  if (bitOn) {
    CurrentSolenoidByte = CurrentSolenoidByte | solbit;
  } else {
    CurrentSolenoidByte = CurrentSolenoidByte & ~solbit;
  }
  RPU_WRITE_SOLENOID_BYTE();
  interrupts();
}

// RPU_MPU_ARCHITECTURE < 10
boolean RPU_FireContinuousSolenoid(byte solBit, byte numCyclesToFire) {
  if (NumCyclesBeforeRevertingSolenoidByte) return false;

  NumCyclesBeforeRevertingSolenoidByte = numCyclesToFire;

  RevertSolenoidBit = solBit;
  RPU_SetContinuousSolenoidBit(false, solBit);
  return true;
}

// RPU_MPU_ARCHITECTURE < 10
byte RPU_ReadContinuousSolenoids() {
#if (RPU_MPU_ARCHITECTURE==5)
  return CurrentSolenoidByte;
#else
  return RPU_DataRead(ADDRESS_U11_B);
#endif
}

// RPU_MPU_ARCHITECTURE < 10
void RPU_DisableSolenoidStack() {
  SolenoidStackEnabled = false;
}

// RPU_MPU_ARCHITECTURE < 10
void RPU_EnableSolenoidStack() {
  SolenoidStackEnabled = true;
}

void RPU_SetSolenoidDefaultPulse(byte solenoidNumber, byte pulseTimeMS) {
  (void)solenoidNumber;
  (void)pulseTimeMS;
}


// RPU_MPU_ARCHITECTURE < 10
boolean RPU_IsSolenoidStackEnabled() {
  return SolenoidStackEnabled;
}

#elif (RPU_MPU_ARCHITECTURE>=10)

// RPU_MPU_ARCHITECTURE >= 10
void RPU_SetDisableFlippers(boolean disableFlippers, byte solbit) {
  (void)solbit;
  GameOverLine = disableFlippers;
#if (RPU_OS_HARDWARE_REV==200)
  RPU_LISYSendGameOverState(disableFlippers);
#else
  if (disableFlippers) RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x34);
  else RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x3C);
#endif  
}

// RPU_MPU_ARCHITECTURE >= 10
boolean RPU_GetDisableFlippers(byte solbit) {
  (void)solbit;
  return GameOverLine;
}

// RPU_MPU_ARCHITECTURE >= 10
void RPU_SetContinuousSolenoid(boolean solOn, byte solNum) {
  unsigned short oldCont = ContinuousSolenoidBits;
  if (solOn) ContinuousSolenoidBits |= (1 << solNum);
  else ContinuousSolenoidBits &= ~(1 << solNum);

#if (RPU_OS_HARDWARE_REV==200)
  RPU_LISYSetSolenoid(solOn, solNum);
#else

  if (oldCont != ContinuousSolenoidBits) {
    byte origPortA = RPU_DataRead(PIA_SOLENOID_PORT_A);
    byte origPortB = RPU_DataRead(PIA_SOLENOID_PORT_B);
    if (origPortA != (ContinuousSolenoidBits & 0xFF)) RPU_DataWrite(PIA_SOLENOID_PORT_A, (ContinuousSolenoidBits & 0xFF));
    if (origPortB != (ContinuousSolenoidBits / 256)) RPU_DataWrite(PIA_SOLENOID_PORT_B, (ContinuousSolenoidBits / 256));
  }
#endif  
}

// RPU_MPU_ARCHITECTURE >= 10
byte RPU_ReadContinuousSolenoids() {
  return ContinuousSolenoidBits;
}

// RPU_MPU_ARCHITECTURE >= 10
void RPU_SetCoinLockout(boolean lockoutOn, byte solNum) {
  RPU_SetContinuousSolenoid(!lockoutOn, solNum);
}

// RPU_MPU_ARCHITECTURE >= 10
void RPU_DisableSolenoidStack() {
  SolenoidStackEnabled = false;
  RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x34);
}

// RPU_MPU_ARCHITECTURE >= 10
void RPU_EnableSolenoidStack() {
  SolenoidStackEnabled = true;
  RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x3C);
}

// RPU_MPU_ARCHITECTURE >= 10
boolean RPU_IsSolenoidStackEnabled() {
  return SolenoidStackEnabled;
}

#if (RPU_OS_HARDWARE_REV!=200)
void RPU_SetSolenoidDefaultPulse(byte solenoidNumber, byte pulseTimeMS) {
  (void)solenoidNumber;
  (void)pulseTimeMS;
}
#endif


#endif





/******************************************************
 * 
 * 
 *    Display Handling Functions
 *    
 *    
*******************************************************/
#if (RPU_MPU_ARCHITECTURE<15)



// RPU_MPU_ARCHITECTURE < 15
byte RPU_SetDisplay(int displayNumber, unsigned long value, boolean blankByMagnitude, byte minDigits, boolean showCommasByMagnitude) {
  if (displayNumber < 0 || displayNumber > 4) return 0;
#if (RPU_MPU_ARCHITECTURE==5)
  if (displayNumber < 4) { DisplayIsText6803[displayNumber] = false; DisplayRawPlaces6803[displayNumber] = 0; }
#endif

#if (RPU_OS_HARDWARE_REV==200)
#if (RPU_MPU_ARCHITECTURE>=13)
  byte oldDisplayCommas = DisplayCommas;
#endif  
  boolean digitsChanged = false;
  byte oldBlank = DisplayDigitEnable[displayNumber];
#endif

  byte blank = 0x00;
#if (RPU_MPU_ARCHITECTURE>=13)
  byte commaBit = 0x01 << (2 * displayNumber);
  if (!showCommasByMagnitude) {
    DisplayCommas &= ~(commaBit | (commaBit * 2));
  }
#endif

  for (int count = 0; count < RPU_OS_NUM_DIGITS; count++) {
    blank = blank * 2;
    if (value != 0 || count < minDigits) blank |= 1;

#if (RPU_MPU_ARCHITECTURE>=13)
    if (showCommasByMagnitude) {
      if (value) {
        if (count == 3) DisplayCommas |= commaBit;
        if (count == 6) DisplayCommas |= (commaBit * 2);
      } else {
        if (count == 3) DisplayCommas &= ~(commaBit);
        if (count == 6) DisplayCommas &= ~(commaBit * 2);
      }
    }
#else
    (void)showCommasByMagnitude;
#endif
#if (RPU_OS_HARDWARE_REV==200)
    byte lastDigit = DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count];
#endif
    DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = value % 10;
#if (RPU_OS_HARDWARE_REV==200)
    if (lastDigit!=DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count]) digitsChanged = true;
#endif
    value /= 10;
  }

  if (blankByMagnitude) DisplayDigitEnable[displayNumber] = blank;

#if (RPU_OS_HARDWARE_REV==200)
#if (RPU_MPU_ARCHITECTURE>=13)
  if (oldDisplayCommas!=DisplayCommas || digitsChanged || oldBlank!=DisplayDigitEnable[displayNumber]) RPU_LISYSendScore(displayNumber, RPU_OS_NUM_DIGITS);
#else
  if (digitsChanged || oldBlank!=DisplayDigitEnable[displayNumber]) RPU_LISYSendScore(displayNumber, RPU_OS_NUM_DIGITS);
#endif
#endif
  return blank;
}
#endif


#if (RPU_MPU_ARCHITECTURE<10)
// RPU_MPU_ARCHITECTURE < 10
void RPU_SetDisplayCredits(int value, boolean displayOn, boolean showBothDigits) {
#ifdef RPU_OS_USE_6_DIGIT_CREDIT_DISPLAY_WITH_7_DIGIT_DISPLAYS
  DisplayDigits[4][2] = (value % 100) / 10;
  DisplayDigits[4][3] = (value % 10);
#else
  DisplayDigits[4][1] = (value % 100) / 10;
  DisplayDigits[4][2] = (value % 10);
#endif
  byte enableMask = DisplayDigitEnable[4] & RPU_OS_MASK_SHIFT_1;

  if (displayOn) {
    if (value > 9 || showBothDigits) enableMask |= RPU_OS_MASK_SHIFT_2;
    else enableMask |= 0x04;
  }

  DisplayDigitEnable[4] = enableMask;
}

// RPU_MPU_ARCHITECTURE < 10
void RPU_SetDisplayBallInPlay(int value, boolean displayOn, boolean showBothDigits) {
#ifdef RPU_OS_USE_6_DIGIT_CREDIT_DISPLAY_WITH_7_DIGIT_DISPLAYS
  DisplayDigits[4][5] = (value % 100) / 10;
  DisplayDigits[4][6] = (value % 10);
#else
  DisplayDigits[4][4] = (value % 100) / 10;
  DisplayDigits[4][5] = (value % 10);
#endif
  byte enableMask = DisplayDigitEnable[4] & RPU_OS_MASK_SHIFT_2;

  if (displayOn) {
    if (value > 9 || showBothDigits) enableMask |= RPU_OS_MASK_SHIFT_1;
    else enableMask |= 0x20;
  }

  DisplayDigitEnable[4] = enableMask;
}

#elif (RPU_MPU_ARCHITECTURE<15)

// RPU_MPU_ARCHITECTURE < 15
void RPU_SetDisplayCredits(int value, boolean displayOn, boolean showBothDigits) {
#if (RPU_OS_HARDWARE_REV==200)
  byte previousDigits[2];
  previousDigits[0] = DisplayCreditDigits[0];
  previousDigits[1] = DisplayCreditDigits[1];
  byte previousBlank = DisplayCreditDigitEnable;
#endif   
  byte blank = 0x02;
  value = value % 100;
  if (value >= 10) {
    DisplayCreditDigits[0] = value / 10;
    blank |= 1;
  } else {
    DisplayCreditDigits[0] = 0;
    if (showBothDigits) blank |= 1;
  }
  DisplayCreditDigits[1] = value % 10;
  if (displayOn) DisplayCreditDigitEnable = blank;
  else DisplayCreditDigitEnable = 0;
#if (RPU_OS_HARDWARE_REV==200)
  if (  DisplayCreditDigits[0]!=previousDigits[0] || 
        DisplayCreditDigits[1]!=previousDigits[1] || 
        previousBlank!=DisplayCreditDigitEnable ) {

    RPU_LISYSendScore(4, 2);
  }
#endif   
}

// RPU_MPU_ARCHITECTURE < 15
void RPU_SetDisplayBallInPlay(int value, boolean displayOn, boolean showBothDigits) {
#if (RPU_OS_HARDWARE_REV==200)
  byte previousDigits[2];
  previousDigits[0] = DisplayBIPDigits[0];
  previousDigits[1] = DisplayBIPDigits[1];
  byte previousBlank = DisplayBIPDigitEnable;
#endif   
  byte blank = 0x02;
  value = value % 100;
  if (value >= 10) {
    DisplayBIPDigits[0] = value / 10;
    blank |= 1;
  } else {
    DisplayBIPDigits[0] = 0;
    if (showBothDigits) blank |= 1;
  }
  DisplayBIPDigits[1] = value % 10;
  if (displayOn) DisplayBIPDigitEnable = blank;
  else DisplayBIPDigitEnable = 0;
#if (RPU_OS_HARDWARE_REV==200)
  if (  DisplayBIPDigits[0]!=previousDigits[0] || 
        DisplayBIPDigits[1]!=previousDigits[1] || 
        previousBlank!=DisplayBIPDigitEnable ) {

    RPU_LISYSendScore(5, 2);
  }
#endif  
}

#endif

void RPU_CycleAllDisplays(unsigned long curTime, byte digitNum, byte digitValue) {
  int displayDigit;
  if (digitValue != 0xFF) {
    displayDigit = digitValue % 10;
  } else {
    displayDigit = (curTime / 250) % 10;
  }
  unsigned long value;
#if (RPU_OS_NUM_DIGITS==7)
  value = displayDigit * 1111111;
#else
  value = displayDigit * 111111;
#endif

  byte displayNumToShow = 0;
  byte displayBlank = RPU_OS_ALL_DIGITS_MASK;

  if (digitNum != 0) {
#if (RPU_OS_NUM_DIGITS==7)
    displayNumToShow = (digitNum - 1) / 7;
    displayBlank = (0x01) << ((digitNum - 1) % 7);

#ifdef RPU_OS_USE_6_DIGIT_CREDIT_DISPLAY_WITH_7_DIGIT_DISPLAYS
    if (displayNumToShow == 4) {
      displayBlank = (0x01) << ((digitNum - 1) % 6);
    }
#endif

#else
    displayNumToShow = (digitNum - 1) / 6;
    displayBlank = (0x01) << ((digitNum - 1) % 6);
#endif
  }

#if (RPU_NUMBER_OF_PLAYER_DISPLAYS==4)
  for (int count = 0; count < 5; count++) {
    if (digitNum) {
      RPU_SetDisplay(count, value);
      if (count == displayNumToShow) RPU_SetDisplayBlank(count, displayBlank);
      else RPU_SetDisplayBlank(count, 0);
    } else {
      RPU_SetDisplay(count, value, false);
    }
  }
#else
  // show the two player displays
  for (int count = 0; count < 2; count++) {
    if (digitNum) {
      RPU_SetDisplay(count, value);
      if (count == displayNumToShow) RPU_SetDisplayBlank(count, displayBlank);
      else RPU_SetDisplayBlank(count, 0);
    } else {
      RPU_SetDisplay(count, value, false);
    }
  }
  // show credit/bip display
  if (digitNum) {
    RPU_SetDisplay(4, value);
    if (displayNumToShow == 2) RPU_SetDisplayBlank(4, displayBlank);
    else RPU_SetDisplayBlank(4, 0);
  } else {
    RPU_SetDisplay(4, value, false);
  }
#endif

}

void RPU_SetDisplayMatch(int value, boolean displayOn, boolean showBothDigits) {
  RPU_SetDisplayBallInPlay(value, displayOn, showBothDigits);
}

// This is confusing -
// Digit mask is like this
//   bit=   b7 b6 b5 b4 b3 b2 b1 b0
//   digit=  x  x  6  5  4  3  2  1
//   (with digit 6 being the least-significant, 1's digit
//
// so, looking at it from left to right on the display
//   digit=  1  2  3  4  5  6
//   bit=   b0 b1 b2 b3 b4 b5
void RPU_SetDisplayBlank(int displayNumber, byte bitMask) {
  if (displayNumber < 0 || displayNumber > 4) return;

#if (RPU_OS_HARDWARE_REV==200)
  boolean sendScore = false;
#endif

#if (RPU_MPU_ARCHITECTURE>=13)
  if (bitMask == 0x00) {
    byte commaBit = 0x01 << (2 * displayNumber);
    byte oldCommas = DisplayCommas;
    DisplayCommas &= ~(commaBit | (commaBit * 2));
#if (RPU_OS_HARDWARE_REV==200)
    if (DisplayCommas!=oldCommas) sendScore = true;
#else
    (void)oldCommas;
#endif

  }
#endif
#if (RPU_OS_HARDWARE_REV==200)
  if (DisplayDigitEnable[displayNumber]!=bitMask) sendScore = true;
#endif

  DisplayDigitEnable[displayNumber] = bitMask;

#if (RPU_OS_HARDWARE_REV==200)
  if (sendScore) RPU_LISYSendScore(displayNumber, RPU_OS_NUM_DIGITS);
#endif

}

byte RPU_GetDisplayBlank(int displayNumber) {
  if (displayNumber < 0 || displayNumber > 4) return 0;
  return DisplayDigitEnable[displayNumber];
}

#if defined(RPU_OS_ADJUSTABLE_DISPLAY_INTERRUPT)
void RPU_SetDisplayRefreshConstant(int intervalConstant) {
  cli();
  //set timer1 interrupt at 1Hz
  TCCR1A = 0;// set entire TCCR1A register to 0
  TCCR1B = 0;// same for TCCR1B
  TCNT1  = 0;//initialize counter value to 0
  // set compare match register for selected increment
  OCR1A = intervalConstant;
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // Set CS10 and CS12 bits for 1024 prescaler
  TCCR1B |= (1 << CS12) | (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei();
}
#endif

void RPU_SetDisplayFlash(int displayNumber, unsigned long value, unsigned long curTime, int period, byte minDigits) {
  // A period of zero toggles display every other time
  if (period) {
    if ((curTime / period) % 2) {
      RPU_SetDisplay(displayNumber, value, true, minDigits);
    } else {
      RPU_SetDisplayBlank(displayNumber, 0);
    }
  }

}

void RPU_SetDisplayFlashCredits(unsigned long curTime, int period) {
  if (period) {
    if ((curTime / period) % 2) {
      DisplayDigitEnable[4] |= 0x06;
    } else {
      DisplayDigitEnable[4] &= 0x39;
    }
  }
}

#if (RPU_MPU_ARCHITECTURE==15)
// RPU_MPU_ARCHITECTURE = 15
byte RPU_SetDisplayText(int displayNumber, const char *text, boolean blankByLength) {
  if (displayNumber > 1 || displayNumber < 0) return 0;
  byte stringLength = 0xff;
  boolean writeSpace = false;
  byte blank = 0;
  byte placeMask = 0x01;

  for (stringLength = 0; stringLength < RPU_OS_NUM_DIGITS; stringLength++) {
    if (text[stringLength] == 0) writeSpace = true;
    if (!writeSpace) DisplayText[displayNumber][stringLength] = (byte)text[stringLength] - 0x20;
    else DisplayText[displayNumber][stringLength] = 0;

    if (DisplayText[displayNumber][stringLength]) blank |= placeMask;
    placeMask *= 2;
  }

  if (blankByLength) DisplayDigitEnable[displayNumber] = blank;

  return stringLength;
}

// Architectures with alpha store numbers as 7-seg
// RPU_MPU_ARCHITECTURE = 15
byte RPU_SetDisplay(int displayNumber, unsigned long value, boolean blankByMagnitude, byte minDigits, boolean showCommasByMagnitude) {
  if (displayNumber < 0 || displayNumber > 3) return 0;

  byte blank = 0x00;

  for (int count = 0; count < RPU_OS_NUM_DIGITS; count++) {
    blank = blank * 2;
    if (value != 0 || count < minDigits) {
      blank |= 1;
      if (displayNumber / 2) DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = SevenSegmentNumbers[value % 10];
      else DisplayText[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = (value % 10) + 16;
    } else {
      if (displayNumber / 2) DisplayDigits[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = 0;
      else DisplayText[displayNumber][(RPU_OS_NUM_DIGITS - 1) - count] = 0;
    }
    value /= 10;
  }

  if (blankByMagnitude) DisplayDigitEnable[displayNumber] = blank;

  return blank;
}

// RPU_MPU_ARCHITECTURE = 15
void RPU_SetDisplayCredits(int value, boolean displayOn, boolean showBothDigits) {
  byte blank = 0x02;
  value = value % 100;
  if (value >= 10) {
    DisplayCreditDigits[0] = SevenSegmentNumbers[value / 10];
    blank |= 1;
  } else {
    DisplayCreditDigits[0] = SevenSegmentNumbers[0];
    if (showBothDigits) blank |= 1;
  }
  DisplayCreditDigits[1] = SevenSegmentNumbers[value % 10];
  if (displayOn) DisplayCreditDigitEnable = blank;
  else DisplayCreditDigitEnable = 0;
}

// RPU_MPU_ARCHITECTURE = 15
void RPU_SetDisplayBallInPlay(int value, boolean displayOn, boolean showBothDigits) {
  byte blank = 0x02;
  value = value % 100;
  if (value >= 10) {
    DisplayBIPDigits[0] = SevenSegmentNumbers[value / 10];
    blank |= 1;
  } else {
    DisplayBIPDigits[0] = SevenSegmentNumbers[0];
    if (showBothDigits) blank |= 1;
  }
  DisplayBIPDigits[1] = SevenSegmentNumbers[value % 10];
  if (displayOn) DisplayBIPDigitEnable = blank;
  else DisplayBIPDigitEnable = 0;
}

#endif

#if (RPU_MPU_ARCHITECTURE==5)
// Bally 6803 displays: 7 segments plus a centre bar (its top and bottom halves, h and j, are one PIA line).
// a=0x01 b=0x02 c=0x04 d=0x08 e=0x10 f=0x20 g=0x40, bar=0x80. Digits and letters as the original Bally 6803
// draws them, recorded from Strange Science's scores and initials entry (2026-11-10); punctuation not yet
// checked against it. Lower case shows as upper case.
static const byte RPU_6803_Font[64] = {
  0x00, 0x00, 0x22, 0x00, 0x6D, 0x00, 0x00, 0x02, 0x39, 0x0F, 0x00, 0xC0, 0x00, 0x40, 0x00, 0x00,   //  !"#$%&'()*+,-./
  0x3F, 0x06, 0x5B, 0x4F, 0x66, 0x6D, 0x7D, 0x07, 0x7F, 0x67, 0x00, 0x00, 0x00, 0x48, 0x00, 0x53,   // 0123456789:;<=>?
  0x00, 0x77, 0xCF, 0x39, 0x8F, 0x79, 0x71, 0x6F, 0x76, 0x89, 0x1E, 0xF4, 0x38, 0xB7, 0x87, 0x3F,   // @ABCDEFGHIJKLMNO
  0x73, 0xEB, 0xC7, 0x6D, 0x81, 0x3E, 0x8E, 0xBE, 0xD2, 0x6E, 0x5B, 0x39, 0x00, 0x0F, 0x00, 0x08};  // PQRSTUVWXYZ[\]^_

static inline byte RPU_6803_Glyph(char c) {
  if (c >= 'a' && c <= 'z') c -= 32;
  if (c < 0x20 || c > 0x5F) return 0;
  return RPU_6803_Font[c - 0x20];
}

// RPU_MPU_ARCHITECTURE = 5
// Up to 7 characters on a player display (0-1 the left module, 2-3 the right one). With blankByLength,
// spaces and the positions after the text are dark.
byte RPU_SetDisplayText(int displayNumber, const char *text, boolean blankByLength) {
  if (displayNumber < 0 || displayNumber > 3) return 0;
  byte length = 0, blank = 0;
  boolean ended = false;
  for (byte place = 0; place < RPU_OS_NUM_DIGITS; place++) {
    if (!ended && !text[place]) ended = true;   // never read past the end of a short string
    char c = ended ? ' ' : text[place];
    if (!ended) length++;
    DisplayText6803[displayNumber][place] = c;
    if (c != ' ') blank |= (0x01 << place);
  }
  DisplayIsText6803[displayNumber] = true;
  DisplayRawPlaces6803[displayNumber] = 0;
  if (blankByLength) DisplayDigitEnable[displayNumber] = blank;
  else DisplayDigitEnable[displayNumber] = RPU_OS_ALL_DIGITS_MASK;
  return length;
}

// RPU_MPU_ARCHITECTURE = 5
// The font's segments for a character (the bits above), for a game that builds its own frames
byte RPU_GetGlyph6803(char c) {
  return RPU_6803_Glyph(c);
}

// RPU_MPU_ARCHITECTURE = 5
// Any segments at one place of a player display (bits as the font; 0 = dark). The place keeps them until the
// next RPU_SetDisplayText or RPU_SetDisplay on that display; the other places show what they did.
void RPU_SetDisplaySegments6803(int displayNumber, byte place, byte segments) {
  if (displayNumber < 0 || displayNumber > 3 || place >= RPU_OS_NUM_DIGITS) return;
  DisplayRaw6803[displayNumber][place] = segments;
  DisplayRawPlaces6803[displayNumber] |= (0x01 << place);
  if (segments) DisplayDigitEnable[displayNumber] |= (0x01 << place);
  else DisplayDigitEnable[displayNumber] &= ~(0x01 << place);
}
#endif


/******************************************************
 * 
 * 
 *    Lamp Handling Functions
 *    
 *    
*******************************************************/

void RPU_SetDimDivisor(byte level, byte divisor) {
  if (level == 1) DimDivisor1 = divisor;
  if (level == 2) DimDivisor2 = divisor;
}

// left shift is iterative on Arduinos, so a bit array is suprisingly faster
byte BitShiftValues[8] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80};

void RPU_SetLampState(int lampNum, byte s_lampState, byte s_lampDim, int s_lampFlashPeriod) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return;
  byte lampRow = lampNum % 8;
  byte lampCol = lampNum / 8;
  byte lampBit = BitShiftValues[lampRow];


  if (s_lampState) {
    int adjustedLampFlash = s_lampFlashPeriod / 50;

    if (s_lampFlashPeriod != 0 && adjustedLampFlash == 0) adjustedLampFlash = 1;
    if (adjustedLampFlash > 250) adjustedLampFlash = 250;

    // Only turn on the lamp if there's no flash, because if there's a flash
    // then the lamp will be turned on by the ApplyFlashToLamps function
    if (s_lampFlashPeriod == 0) LampStates[lampCol] &= ~(lampBit);
    LampFlashPeriod[lampNum] = adjustedLampFlash;
  } else {
    LampStates[lampCol] |= lampBit;
    LampFlashPeriod[lampNum] = 0;
  }

  if (s_lampDim & 0x01) {
    LampDim1[lampCol] |= lampBit;
  } else {
    LampDim1[lampCol] &= ~lampBit;
  }

  if (s_lampDim & 0x02) {
    LampDim2[lampCol] |= lampBit;
  } else {
    LampDim2[lampCol] &= ~lampBit;
  }

}

byte RPU_ReadLampState(int lampNum) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return 0x00;
  byte lampStateByte = LampStates[lampNum / 8];
  return (lampStateByte & (0x01 << (lampNum % 8))) ? 0 : 1;
}

byte RPU_ReadLampDim(int lampNum) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return 0x00;
  byte lampDim = 0;
  byte lampDimByte = LampDim1[lampNum / 8];
  if (lampDimByte & (0x01 << (lampNum % 8))) lampDim |= 1;

  lampDimByte = LampDim2[lampNum / 8];
  if (lampDimByte & (0x01 << (lampNum % 8))) lampDim |= 2;

  return lampDim;
}

int RPU_ReadLampFlash(int lampNum) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return 0;

  return LampFlashPeriod[lampNum] * 50;
}

void RPU_ApplyFlashToLamps(unsigned long curTime) {
  int curLampByte = 0;
  byte curLampBit = 0;
  int curLampNum = 0;

  for (curLampByte = 0; curLampByte < RPU_NUM_LAMP_BANKS; curLampByte++) {
    curLampBit = 0x01;
    for (byte curBit = 0; curBit < 8; curBit++) {
      // Bally has 60 lamps in 8 banks of 8: don't read past LampFlashPeriod for positions 60-63
      if ( curLampNum < RPU_MAX_LAMPS && LampFlashPeriod[curLampNum] != 0 ) {
        unsigned long adjustedLampFlash = (unsigned long)LampFlashPeriod[curLampNum] * (unsigned long)50;
        if ((curTime / adjustedLampFlash) % 2) {
          LampStates[curLampByte] &= ~(curLampBit);
        } else {
          LampStates[curLampByte] |= (curLampBit);
        }
      }

      curLampBit *= 2;
      curLampNum += 1;
    }
  }
}

void RPU_FlashAllLamps(unsigned long curTime) {
  for (int count = 0; count < RPU_MAX_LAMPS; count++) {
    RPU_SetLampState(count, 1, 0, 500);
  }

  RPU_ApplyFlashToLamps(curTime);
}

void RPU_TurnOffAllLamps() {
  for (int count = 0; count < RPU_MAX_LAMPS; count++) {
    RPU_SetLampState(count, 0, 0, 0);
  }
}


#if defined(RPU_OS_USE_LAMP_OVERLAYS)
void RPU_ClearLampsOnOverlay() {
  UseLampsOnOverlay = false;
  for (byte count=0; count<RPU_NUM_LAMP_BANKS; count++) {
    LampsOnOverlay[count] = 0xFF;
  }
}
void RPU_ClearLampsOffOverlay() {
  UseLampsOffOverlay = false;
  for (byte count=0; count<RPU_NUM_LAMP_BANKS; count++) {
    LampsOffOverlay[count] = 0x00;
  }
}

void RPU_AddLampToOverlay(boolean lampsOnOverlay, int lampNum) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return;
  byte lampRow = lampNum % 8;
  byte lampCol = lampNum / 8;
  byte lampBit = BitShiftValues[lampRow];

  if (lampsOnOverlay) {
    LampsOnOverlay[lampCol] &= ~lampBit;
  } else {
    LampsOffOverlay[lampCol] |= lampBit;
  }
}

void RPU_RemoveLampFromOverlay(boolean lampsOnOverlay, int lampNum) {
  if (lampNum >= RPU_MAX_LAMPS || lampNum < 0) return;
  byte lampRow = lampNum % 8;
  byte lampCol = lampNum / 8;
  byte lampBit = BitShiftValues[lampRow];

  if (lampsOnOverlay) {
    LampsOnOverlay[lampCol] |= lampBit;
  } else {
    LampsOffOverlay[lampCol] &= ~lampBit;
  }
}

void RPU_AddLampByteToOverlay(boolean lampsOnOverlay, byte lampByte, byte lampValues) {
  if (lampByte>=RPU_NUM_LAMP_BANKS) return;

  if (lampsOnOverlay) {
    LampsOnOverlay[lampByte] &= ~lampValues;
  } else {
    LampsOffOverlay[lampByte] |= lampValues;
  }
}

void RPU_RemoveLampByteFromOverlay(boolean lampsOnOverlay, byte lampByte, byte lampValues) {
  if (lampByte>=RPU_NUM_LAMP_BANKS) return;

  if (lampsOnOverlay) {
    LampsOnOverlay[lampByte] |= lampValues;
  } else {
    LampsOffOverlay[lampByte] &= ~lampValues;
  }
}

void RPU_ApplyLampsOnOverlay() {
  UseLampsOnOverlay = true;
}
void RPU_ApplyLampsOffOverlay() {
  UseLampsOffOverlay = true;
}
#endif



/******************************************************
 * 
 * 
 *    Helper Functions
 *    
 *    
*******************************************************/

void RPU_ClearVariables() {
  // Reset solenoid stack
  SolenoidStackFirst = 0;
  SolenoidStackLast = 0;

  // Reset switch stack
  SwitchStackFirst = 0;
  SwitchStackLast = 0;

#if (RPU_MPU_ARCHITECTURE > 9)
  GameOverLine = true;
  // Reset sound stack
  SoundStackFirst = 0;
  SoundStackLast = 0;
#endif

  CurrentDisplayDigit = 0;

  // Set default values for the displays
  for (int displayCount = 0; displayCount < 5; displayCount++) {
    for (int digitCount = 0; digitCount < RPU_OS_NUM_DIGITS; digitCount++) {
      DisplayDigits[displayCount][digitCount] = 0;
    }
    DisplayDigitEnable[displayCount] = 0x00;
  }
#if (RPU_MPU_ARCHITECTURE>=13)
  DisplayCommas = 0x00;
#endif

  // Turn off all lamp states
  for (int lampBankCounter = 0; lampBankCounter < RPU_NUM_LAMP_BANKS; lampBankCounter++) {
    LampStates[lampBankCounter] = 0xFF;
    LampDim1[lampBankCounter] = 0x00;
    LampDim2[lampBankCounter] = 0x00;

#if (RPU_OS_HARDWARE_REV==200)
    OldLampStates[lampBankCounter] = 0xFF;
#endif    
  }

  for (int lampFlashCount = 0; lampFlashCount < RPU_MAX_LAMPS; lampFlashCount++) {
    LampFlashPeriod[lampFlashCount] = 0;
  }

  // Reset all the switch values
  // (set them as closed so that if they're stuck they don't register as new events)
  byte switchCount;
  for (switchCount = 0; switchCount < NUM_SWITCH_BYTES; switchCount++) {
    SwitchesMinus2[switchCount] = 0xFF;
    SwitchesMinus1[switchCount] = 0xFF;
    SwitchesNow[switchCount] = 0xFF;
    SwitchInverter[switchCount] = 0x00;
#ifdef RPU_STREAMLINED_IMMEDIATE_SOLENOIDS
    ImmediateSolenoidSwitchMask[switchCount] = 0x00;
#endif
  }

#ifdef RPU_STREAMLINED_IMMEDIATE_SOLENOIDS
  for (byte count = 0; count < MAX_IMMEDIATE_STREAMLINED_SOLENOIDS; count++) {
    ImmediateSolenoidSwitchByte[count] = 0x00;
    ImmediateSolenoidSwitchFlag[count] = 0x00;
  }

  if (GameSwitches) {
    for (byte count = 0; count < NumGameSwitches; count++) {
      if (GameSwitches[count].switchNum < MAX_NUM_SWITCHES) {
        ImmediateSolenoidSwitchMask[GameSwitches[count].switchNum / 8] |= (0x01 << (GameSwitches[count].switchNum % 8));
        ImmediateSolenoidSwitchByte[count] = GameSwitches[count].switchNum / 8;
        ImmediateSolenoidSwitchFlag[count] = (0x01 << (GameSwitches[count].switchNum % 8));
      }
    }
/*
    if (DEBUG_MESSAGES) {
      char buf[256];
      for (byte count = 0; count < NUM_SWITCH_BYTES; count++) {
        sprintf(buf, "Switch mask byte %d = 0x%02X\n", count, ImmediateSolenoidSwitchMask[count]);
        Serial.write(buf);
      }
      for (byte count = 0; count < NumGameSwitches; count++) {
        sprintf(buf, "Triggered sol switch=%d, byte=%d, mask=0x%02X\n", GameSwitches[count].switchNum, ImmediateSolenoidSwitchByte[count], ImmediateSolenoidSwitchFlag[count]);
        Serial.write(buf);
      }
    }
*/    

  }
#endif

  for (byte count = 0; count < TIMED_SOLENOID_STACK_SIZE; count++) {
    TimedSolenoidStack[count].inUse = 0;
    TimedSolenoidStack[count].pushTime = 0;
    TimedSolenoidStack[count].solenoidNumber = 0;
    TimedSolenoidStack[count].numPushes = 0;
    TimedSolenoidStack[count].disableOverride = 0;
  }

#if (RPU_MPU_ARCHITECTURE > 9)
  for (byte count = 0; count < TIMED_SOUND_STACK_SIZE; count++) {
    TimedSoundStack[count].inUse = 0;
    TimedSoundStack[count].pushTime = 0;
    TimedSoundStack[count].soundNumber = 0;
    TimedSoundStack[count].numPushes = 0;
  }
#endif

#if defined(RPU_OS_USE_LAMP_OVERLAYS)
  RPU_ClearLampsOnOverlay();
  RPU_ClearLampsOffOverlay();
#endif

}




/******************************************************
 * 
 * 
 *    Sound Handling Functions
 *    
 *    
*******************************************************/

#ifdef RPU_OS_USE_S_AND_T

void RPU_PlaySoundSAndT(byte soundByte) {

  byte oldSolenoidControlByte, soundLowerNibble, soundUpperNibble;

  // mask further zero-crossing interrupts during this
  noInterrupts();

  // Get the current value of U11:PortB - current solenoids
  oldSolenoidControlByte = RPU_DataRead(ADDRESS_U11_B);
  soundLowerNibble = (oldSolenoidControlByte & 0xF0) | (soundByte & 0x0F);
  soundUpperNibble = (oldSolenoidControlByte & 0xF0) | (soundByte / 16);

  // Put 1s on momentary solenoid lines
  RPU_DataWrite(ADDRESS_U11_B, oldSolenoidControlByte | 0x0F);

  // Put sound latch low
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x34);

  // Let the strobe stay low for a moment
  delayMicroseconds(32);

  // Put sound latch high
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x3C);

  // put the new byte on U11:PortB (the lower nibble is currently loaded)
  RPU_DataWrite(ADDRESS_U11_B, soundLowerNibble);

  // wait 138 microseconds
  delayMicroseconds(138);

  // put the new byte on U11:PortB (the uppper nibble is currently loaded)
  RPU_DataWrite(ADDRESS_U11_B, soundUpperNibble);

  // wait 76 microseconds
  delayMicroseconds(145);

  // Restore the original solenoid byte
  RPU_DataWrite(ADDRESS_U11_B, oldSolenoidControlByte);

  // Put sound latch low
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x34);

  interrupts();
}
#endif

// With hardware rev 1, this function relies on D13 being connected to A5 because it writes to address 0xA0
// A0  - A0   0
// A1  - A1   0
// A2  - n/c  0
// A3  - A2   0
// A4  - A3   0
// A5  - D13  1
// A6  - n/c  0
// A7  - A4   1
// A8  - n/c  0
// A9  - GND  0
// A10 - n/c  0
// A11 - n/c  0
// A12 - GND  0
// A13 - n/c  0
#ifdef RPU_OS_USE_SB100
void RPU_PlaySB100(byte soundByte) {

#if (RPU_OS_HARDWARE_REV==1)
  PORTB = PORTB | 0x20;
#endif

  RPU_DataWrite(ADDRESS_SB100, soundByte);

#if (RPU_OS_HARDWARE_REV==1)
  PORTB = PORTB & 0xDF;
#endif

}

#if (RPU_OS_HARDWARE_REV==2)
void RPU_PlaySB100Chime(byte soundByte) {

  RPU_DataWrite(ADDRESS_SB100_CHIMES, soundByte);

}
#endif
#endif


#if defined(RPU_OS_USE_DASH50) || defined(RPU_OS_USE_DASH51)
void RPU_PlaySoundDash51(byte soundByte) {

  // This device has 32 possible sounds, but they're mapped to
  // 0 - 15 and then 128 - 143 on the original card, with bits b4, b5, and b6 reserved
  // for timing controls.
  // For ease of use, I've mapped the sounds from 0-31

  byte oldSolenoidControlByte, soundLowerNibble, displayWithSoundBit4, oldDisplayByte;

  // mask further zero-crossing interrupts during this
  noInterrupts();

  // Get the current value of U11:PortB - current solenoids
  oldSolenoidControlByte = RPU_DataRead(ADDRESS_U11_B);
  oldDisplayByte = RPU_DataRead(ADDRESS_U11_A);
  soundLowerNibble = (oldSolenoidControlByte & 0xF0) | (soundByte & 0x0F);
  displayWithSoundBit4 = oldDisplayByte;
  if (soundByte & 0x10) displayWithSoundBit4 |= 0x02;
  else displayWithSoundBit4 &= 0xFD;

  // Put 1s on momentary solenoid lines
  RPU_DataWrite(ADDRESS_U11_B, oldSolenoidControlByte | 0x0F);

  // Put sound latch low
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x34);

  // Let the strobe stay low for a moment
  delayMicroseconds(68);

  // put bit 4 on Display Enable 7
  RPU_DataWrite(ADDRESS_U11_A, displayWithSoundBit4);

  // Put sound latch high
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x3C);

  // put the new byte on U11:PortB (the lower nibble is currently loaded)
  RPU_DataWrite(ADDRESS_U11_B, soundLowerNibble);

  // wait 180 microseconds
  delayMicroseconds(180);

  // Restore the original solenoid byte
  RPU_DataWrite(ADDRESS_U11_B, oldSolenoidControlByte);

  // Restore the original display byte
  RPU_DataWrite(ADDRESS_U11_A, oldDisplayByte);

  // Put sound latch low
  RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x34);

  interrupts();
}

#endif

#if (RPU_OS_HARDWARE_REV>=2 && defined(RPU_OS_USE_SB300))

void RPU_PlaySB300SquareWave(byte soundRegister, byte soundByte) {
  RPU_DataWrite(ADDRESS_SB300_SQUARE_WAVES + soundRegister, soundByte);
}

void RPU_PlaySB300Analog(byte soundRegister, byte soundByte) {
  RPU_DataWrite(ADDRESS_SB300_ANALOG + soundRegister, soundByte);
}

#endif


#if defined(RPU_OS_USE_WTYPE_1_SOUND) || defined(RPU_OS_USE_WTYPE_2_SOUND)
#if defined(RPU_OS_USE_WTYPE_2_SOUND)
unsigned short SoundLowerLimit = 0x0000;
unsigned short SoundUpperLimit = 0x0080;
#else
unsigned short SoundLowerLimit = 0x0100;
unsigned short SoundUpperLimit = 0x1F00;
#endif

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
void RPU_SetSoundValueLimits(unsigned short lowerLimit, unsigned short upperLimit) {
  SoundLowerLimit = lowerLimit;
  SoundUpperLimit = upperLimit;
}

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
int SpaceLeftOnSoundStack() {
  if (SoundStackFirst >= SOUND_STACK_SIZE || SoundStackLast >= SOUND_STACK_SIZE) return 0;
  if (SoundStackLast >= SoundStackFirst) return ((SOUND_STACK_SIZE - 1) - (SoundStackLast - SoundStackFirst));
  return (SoundStackFirst - SoundStackLast) - 1;
}

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
void RPU_PushToSoundStack(unsigned short soundNumber, byte numPushes) {
#if (RPU_OS_HARDWARE_REV==200)
#if (RPU_MPU_ARCHITECTURE<13)
  RPU_LISYSendSoundCommand(soundNumber/256);
#else
  RPU_LISYSendSoundCommand(soundNumber);
#endif
  // If the solenoid stack last index is out of range, then it's an error - return
  if (SpaceLeftOnSoundStack() == 0) return;
  if (soundNumber < SoundLowerLimit || soundNumber > SoundUpperLimit) return;

  for (int count = 0; count < numPushes; count++) {
    SoundStack[SoundStackLast] = soundNumber;

    SoundStackLast += 1;
    if (SoundStackLast == SOUND_STACK_SIZE) {
      // If the end index is off the end, then wrap
      SoundStackLast = 0;
    }
    // If the stack is now full, return
    if (SpaceLeftOnSoundStack() == 0) return;
  }
#endif  
}

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
unsigned short PullFirstFromSoundStack() {
  // If first and last are equal, there's nothing on the stack
  if (SoundStackFirst == SoundStackLast) {
    return SOUND_STACK_EMPTY;
  }

  unsigned short retVal = SoundStack[SoundStackFirst];

  SoundStackFirst += 1;
  if (SoundStackFirst >= SOUND_STACK_SIZE) SoundStackFirst = 0;

  return retVal;
}

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
boolean RPU_PushToTimedSoundStack(unsigned short soundNumber, byte numPushes, unsigned long whenToPlay) {
  for (int count = 0; count < TIMED_SOUND_STACK_SIZE; count++) {
    if (!TimedSoundStack[count].inUse) {
      TimedSoundStack[count].inUse = true;
      TimedSoundStack[count].pushTime = whenToPlay;
      TimedSoundStack[count].soundNumber = soundNumber;
      TimedSoundStack[count].numPushes = numPushes;
      return true;
    }
  }
  return false;
}

// RPU_OS_USE_WTYPE_1_SOUND or RPU_OS_USE_WTYPE_2_SOUND
void RPU_UpdateTimedSoundStack(unsigned long curTime) {
  for (int count = 0; count < TIMED_SOUND_STACK_SIZE; count++) {
    if (TimedSoundStack[count].inUse && TimedSoundStack[count].pushTime < curTime) {
      RPU_PushToSoundStack(TimedSoundStack[count].soundNumber, TimedSoundStack[count].numPushes);
      TimedSoundStack[count].inUse = false;
    }
  }
}
#endif

#ifdef RPU_OS_USE_WTYPE_11_SOUND
void RPU_PlayW11Sound(byte soundNum) {
  RPU_DataWrite(PIA_SOUND_11_PORT_A, soundNum);
  // Strobe CA2
  RPU_DataWrite(PIA_SOUND_11_CONTROL_A, 0x34);
  RPU_DataWrite(PIA_SOUND_11_CONTROL_A, 0x3C);
}

void RPU_PlayW11Music(byte songNum) {
  RPU_DataWrite(PIA_WIDGET_PORT_B, songNum);
  // Strobe CA2
  RPU_DataWrite(PIA_WIDGET_CONTROL_B, 0x34);
  RPU_DataWrite(PIA_WIDGET_CONTROL_B, 0x3C);
}
#endif



/******************************************************
 * 
 * 
 *    EEPROM Helper Functions
 *    
 *    
*******************************************************/

void RPU_WriteByteToEEProm(unsigned short startByte, byte value) {
  EEPROM.write(startByte, value);
}

byte RPU_ReadByteFromEEProm(unsigned short startByte) {
  byte value = EEPROM.read(startByte);

  // If this value is unset, set it
  if (value == 0xFF) {
    value = 0;
    RPU_WriteByteToEEProm(startByte, value);
  }
  return value;
}

unsigned long RPU_ReadULFromEEProm(unsigned short startByte, unsigned long defaultValue) {
  unsigned long value;

  value = (((unsigned long)EEPROM.read(startByte + 3)) << 24) |
          ((unsigned long)(EEPROM.read(startByte + 2)) << 16) |
          ((unsigned long)(EEPROM.read(startByte + 1)) << 8) |
          ((unsigned long)(EEPROM.read(startByte)));

  if (value == 0xFFFFFFFF) {
    value = defaultValue;
    RPU_WriteULToEEProm(startByte, value);
  }
  return value;
}

void RPU_WriteULToEEProm(unsigned short startByte, unsigned long value) {
  EEPROM.write(startByte + 3, (byte)(value >> 24));
  EEPROM.write(startByte + 2, (byte)((value >> 16) & 0x000000FF));
  EEPROM.write(startByte + 1, (byte)((value >> 8) & 0x000000FF));
  EEPROM.write(startByte, (byte)(value & 0x000000FF));
}




/******************************************************
 * 
 * 
 *    Initialization and ISR Functions
 *    
 *    
*******************************************************/
#if (RPU_OS_HARDWARE_REV==102)
boolean CheckForMPUClock() {
  pinMode(RPU_DISABLE_PHI_FROM_MPU, OUTPUT);
  digitalWrite(RPU_DISABLE_PHI_FROM_MPU, 1);
  pinMode(RPU_DISABLE_PHI_FROM_CPU, OUTPUT);
  digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 1);
  pinMode(RPU_PHI2_PIN, INPUT_PULLUP);

  unsigned long startTime = millis();
  int sawClockLow = 0;
  int sawClockHigh = 0;
  while (millis() < (startTime + 10)) {
    if (PING & 0x04) sawClockHigh += 1;
    else sawClockLow += 1;
  }

  if (sawClockLow > 25 && sawClockHigh > 25) {
    return true;
  }

  // At this point, since we didn't see the mpu clock, we
  // can assume that the clock is generated by the CPU,
  // but we can check if we want
  digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 0);

  sawClockLow = 0;
  sawClockHigh = 0;
  startTime = millis();
  byte lastState = 0;
  for (int count = 0; count < 1000; count++) {
    if (PING & 0x04 && !lastState) {
      sawClockHigh += 1;
      lastState = 1;
    } else if (lastState) {
      sawClockLow += 1;
      lastState = 0;
    }
  }

  digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 1);

  return false;
}
#endif

#if (RPU_MPU_ARCHITECTURE==5)

// RPU_MPU_ARCHITECTURE == 5 (Bally 6803, the 400-IC: Rev 402)
// Everything runs from one 1 ms interrupt. Found on Strange Science, 2026-11-09 (firmware/rp400/ss-hwtest):
//  - switches: one column per interrupt: strobe it, wait RPU_6803_SWITCH_SETTLE_US for the returns
//    (56k / 390 pF) to let go of the other columns' charge, read; closed = 1. Column 6's strobe is PIA1 PB4.
//    (Strobing at the end of one interrupt and reading at the start of the next gave phantom closures
//    once many lamps were lit: the interrupt then runs long and the gap between the two shrinks.)
//  - lamps: at each AC half-cycle (phase A = P20 high) each lamp column is latched into the three 4514s
//    (PIA0 CB2) and the SCRs of the banks that are on are gated by dropping their INH (PA5-7). LED bulbs
//    draw too little to keep an SCR on, so the gating repeats every interrupt for the first 6.5 ms of the
//    8.3 ms half (never into the next half: the other phase's lamp shares the driver)
//  - solenoids: as on the -35, one solenoid stack entry per half-cycle, its number on PB0-3 (74LS154)
//  - displays: one digit of both modules per interrupt, in the game's own order, ~6 us between writes
//    (shorter strobes don't survive the cable to the displays)
//  - flashers: the "bright lights" are lamp positions fired on the C/D supply (higher current). Held on like
//    an insert they blew both of their fuses (owner, 11-12), so the library owns them: the game lists them
//    (RPU_SetFlasherLamps6803) and strobes them (RPU_StrobeFlasher6803: count flashes of 50 ms, 25 ms dark
//    between). A listed lamp is never gated from its lamp state: one turned on there gets a flash on that
//    edge, and while it stays on a flash every 250 ms (a lamp test strobes them, never lights them: owner,
//    11-12; 50 ms in 250 is the original's own steady Volts / Atom pattern). The original's flashes are
//    40-59 ms (115,000 of them in 211 bench logs), and 25 ms is its commonest gap inside a burst. However a
//    game toggles one, it's lit at most half the time over a second or so: a new flash waits while a flasher
//    is over that budget (the original's highest is 42% in any second, bench)
#define RPU_6803_LAMP_GATE_US       20
#define RPU_6803_REGATE_US          6500
#define RPU_6803_DISPLAY_STEP_US    6
#define RPU_6803_SWITCH_SETTLE_US   120
#define RPU_6803_FLASH_ON_PASSES    6     // 50 ms in half-cycles (120 a second)
#define RPU_6803_FLASH_OFF_PASSES   3     // 25 ms
#define RPU_6803_FLASH_HELD_PASSES  30    // 250 ms: a lamp state held on flashes this often
#define RPU_6803_FLASH_BUDGET       60    // +2 a lit half-cycle, -1 every half-cycle: over 50% on, it fills
#define RPU_6803_MAX_FLASHERS       16

volatile byte SwitchColumn6803 = 0;
volatile byte DisplayDigit6803 = 0;
volatile unsigned long ZeroCrossPasses6803 = 0;
volatile boolean PhaseA6803 = false;
volatile uint32_t PhaseEdgeUs6803 = 0;
volatile uint32_t RPU_6803_MaxTickUs = 0;    // the longest interrupt so far (for tuning)
volatile byte FlasherLamps6803[RPU_6803_MAX_FLASHERS];
volatile byte NumFlashers6803 = 0;
volatile byte FlasherFlashesLeft6803[RPU_6803_MAX_FLASHERS];   // flashes still to give, this one included
volatile byte FlasherPasses6803[RPU_6803_MAX_FLASHERS];        // half-cycles left in this flash or gap
volatile boolean FlasherLit6803[RPU_6803_MAX_FLASHERS];        // in a flash (else in a gap or idle)
volatile boolean FlasherStateWasOn6803[RPU_6803_MAX_FLASHERS]; // its lamp state last half-cycle
volatile byte FlasherHeldPasses6803[RPU_6803_MAX_FLASHERS];    // half-cycles since its held state's last flash
volatile byte FlasherBudget6803[RPU_6803_MAX_FLASHERS];        // its recent on time (RPU_6803_FLASH_BUDGET)
volatile byte FlasherMask6803[RPU_NUM_LAMP_BANKS];             // a 1 = a listed flasher
volatile byte FlasherLitMask6803[RPU_NUM_LAMP_BANKS];          // a 1 = in a flash now

// RPU_MPU_ARCHITECTURE == 5
// The lamps wired to the C/D ("bright light") supply. Without this list they're gated like any lamp
void RPU_SetFlasherLamps6803(const byte *lamps, byte count) {
  if (count > RPU_6803_MAX_FLASHERS) count = RPU_6803_MAX_FLASHERS;
  NumFlashers6803 = 0;
  for (byte count2 = 0; count2 < RPU_NUM_LAMP_BANKS; count2++) FlasherMask6803[count2] = FlasherLitMask6803[count2] = 0x00;
  for (byte count2 = 0; count2 < count; count2++) {
    FlasherLamps6803[count2] = lamps[count2];
    FlasherFlashesLeft6803[count2] = 0;
    FlasherPasses6803[count2] = 0;
    FlasherLit6803[count2] = false;
    FlasherStateWasOn6803[count2] = false;
    FlasherHeldPasses6803[count2] = 0;
    FlasherBudget6803[count2] = 0;
    if (lamps[count2] / 8 < RPU_NUM_LAMP_BANKS) FlasherMask6803[lamps[count2] / 8] |= 0x01 << (lamps[count2] % 8);
  }
  NumFlashers6803 = count;
}

// RPU_MPU_ARCHITECTURE == 5
// count flashes of a listed flasher (50 ms on, 25 ms dark between). A strobe already running is replaced,
// so a call can never stack flashes into a longer one; count 0 stops it
void RPU_StrobeFlasher6803(byte lampNum, byte count) {
  for (byte index = 0; index < NumFlashers6803; index++) {
    if (FlasherLamps6803[index] != lampNum) continue;
    noInterrupts();
    if (count == 0) {
      FlasherFlashesLeft6803[index] = 0;
    } else if (!FlasherLit6803[index]) {
      // Idle or in a gap: the first flash starts once any gap is over (25 ms after the last flash at least)
      FlasherFlashesLeft6803[index] = count;
    } else {
      // In a flash: it finishes as it is, then count more
      FlasherFlashesLeft6803[index] = count + 1;
    }
    interrupts();
    return;
  }
}

// RPU_MPU_ARCHITECTURE == 5
// At each zero crossing: each flasher's flash and gap timing. A lamp state turned on gives a flash, and
// held on, another every 250 ms
static void RPU_6803_UpdateFlashers() {
  for (byte index = 0; index < NumFlashers6803; index++) {
    byte lampNum = FlasherLamps6803[index];
    byte lampByte = lampNum / 8, lampBit = 0x01 << (lampNum % 8);
    if (lampByte >= RPU_NUM_LAMP_BANKS) continue;
    boolean stateOn = !(LampStates[lampByte] & lampBit);
    if (stateOn && !FlasherStateWasOn6803[index]) {
      FlasherHeldPasses6803[index] = 0;
      if (!FlasherFlashesLeft6803[index]) FlasherFlashesLeft6803[index] = 1;
    } else if (stateOn && ++FlasherHeldPasses6803[index] >= RPU_6803_FLASH_HELD_PASSES) {
      FlasherHeldPasses6803[index] = 0;
      if (!FlasherFlashesLeft6803[index]) FlasherFlashesLeft6803[index] = 1;
    }
    FlasherStateWasOn6803[index] = stateOn;

    if (FlasherPasses6803[index]) FlasherPasses6803[index] -= 1;
    if (!FlasherPasses6803[index]) {
      if (FlasherLit6803[index]) {
        // A flash ends: always a gap after it, even with none to follow
        FlasherLit6803[index] = false;
        if (FlasherFlashesLeft6803[index]) FlasherFlashesLeft6803[index] -= 1;
        FlasherPasses6803[index] = RPU_6803_FLASH_OFF_PASSES;
      } else if (FlasherFlashesLeft6803[index] && FlasherBudget6803[index] < RPU_6803_FLASH_BUDGET) {
        FlasherLit6803[index] = true;
        FlasherPasses6803[index] = RPU_6803_FLASH_ON_PASSES;
      }
    }
    if (FlasherLit6803[index]) FlasherBudget6803[index] += 2;
    if (FlasherBudget6803[index]) FlasherBudget6803[index] -= 1;
    if (FlasherLit6803[index]) FlasherLitMask6803[lampByte] |= lampBit;
    else FlasherLitMask6803[lampByte] &= ~lampBit;
  }
}

// RPU_MPU_ARCHITECTURE == 5
// One switch column's debouncing and its solenoids, as the -35 code does for all of them at a zero crossing
static void RPU_6803_ProcessSwitchColumn(byte column, byte returns) {
  SwitchesMinus2[column] = SwitchesMinus1[column];
  SwitchesMinus1[column] = SwitchesNow[column];
  SwitchesNow[column] = returns;

  // A closure starting (off, on): fire its solenoid once now (bumpers, slings)
  byte startingClosures = SwitchesNow[column] & ~SwitchesMinus1[column];
  boolean immediateSolenoidFired = false;
  for (byte bitCount = 0; bitCount < 8 && startingClosures && !immediateSolenoidFired; bitCount++) {
    if (!(startingClosures & (0x01 << bitCount))) continue;
    byte switchNum = column * 8 + bitCount;
    for (int count = 0; count < NumGamePrioritySwitches; count++) {
      if (GameSwitches && GameSwitches[count].switchNum == switchNum) {
        PushToFrontOfSolenoidStack(GameSwitches[count].solenoid, 1);
        immediateSolenoidFired = true;
        break;
      }
    }
  }

  // A valid closure (off, on, on): the switch's solenoid, then the switch to the game
  immediateSolenoidFired = false;
  byte validClosures = SwitchesNow[column] & SwitchesMinus1[column] & ~SwitchesMinus2[column];
  for (byte bitCount = 0; bitCount < 8 && validClosures; bitCount++) {
    if (!(validClosures & (0x01 << bitCount))) continue;
    byte switchNum = column * 8 + bitCount;
    for (int count = 0; count < NumGameSwitches; count++) {
      if (GameSwitches && GameSwitches[count].switchNum == switchNum && GameSwitches[count].solenoid != SOL_NONE) {
        if (count < NumGamePrioritySwitches && !immediateSolenoidFired) {
          PushToFrontOfSolenoidStack(GameSwitches[count].solenoid, GameSwitches[count].solenoidHoldTime);
        } else {
          RPU_PushToSolenoidStack(GameSwitches[count].solenoid, GameSwitches[count].solenoidHoldTime);
        }
      }
    }
    PushToSwitchStack(switchNum);
  }
}

// RPU_MPU_ARCHITECTURE == 5
// Gate this half-cycle's lamps, column by column, then park the 4514s on column 15
static void RPU_6803_FireLamps(boolean phaseB) {
  byte firstByte = phaseB ? 6 : 0;
  unsigned long lampCycles = ZeroCrossPasses6803 / 2;   // each lamp gets one half of each AC cycle
  for (byte column = 0; column < 15; column++) {
    byte banksOn = 0;
    byte lampBit = 0x01 << (column & 0x07);
    for (byte bank = 0; bank < 3; bank++) {
      byte lampByte = firstByte + bank * 2 + (column >> 3);
      byte state = LampStates[lampByte];   // a 1 is off
      if (lampCycles % DimDivisor1) state |= LampDim1[lampByte];
      if (lampCycles % DimDivisor2) state |= LampDim2[lampByte];
#if defined(RPU_OS_USE_LAMP_OVERLAYS)
      if (UseLampsOffOverlay) state |= LampsOffOverlay[lampByte];
      if (UseLampsOnOverlay) state &= LampsOnOverlay[lampByte];
#endif
      // Listed flashers: only the strobe lights them, never their lamp state, dim or overlays
      state = (state & ~FlasherMask6803[lampByte]) | (FlasherMask6803[lampByte] & ~FlasherLitMask6803[lampByte]);
      if (!(state & lampBit)) banksOn |= (0x01 << bank);
    }
    if (!banksOn) continue;
    RPU_DataWrite(ADDRESS_U10_A, 0xE0 | column);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x3C);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);
    RPU_DataWrite(ADDRESS_U10_A, ((~banksOn & 0x07) << 5) | column);
    delayMicroseconds(RPU_6803_LAMP_GATE_US);
    RPU_DataWrite(ADDRESS_U10_A, 0xE0 | column);
  }
  RPU_DataWrite(ADDRESS_U10_A, 0xEF);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);
}

// RPU_MPU_ARCHITECTURE == 5
// At each zero crossing: the next momentary solenoid (or none), and a continuous bit's timed revert
static void RPU_6803_ZeroCrossSolenoids() {
  if (NumCyclesBeforeRevertingSolenoidByte != 0) {
    NumCyclesBeforeRevertingSolenoidByte -= 1;
    if (NumCyclesBeforeRevertingSolenoidByte == 0) {
      CurrentSolenoidByte |= RevertSolenoidBit;
      RevertSolenoidBit = 0x00;
    }
  }
  byte solenoid = PullFirstFromSolenoidStack();
  if (solenoid == SOLENOID_STACK_EMPTY || solenoid > SOL_NONE) solenoid = SOL_NONE;
  CurrentSolenoidByte = (CurrentSolenoidByte & 0xE0) | solenoid;
  // CB2 low enables the 74LS154: high before the number goes back to 15, low after a new number is out
  if (solenoid == SOL_NONE) RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U11_B, CurrentSolenoidByte);
  if (solenoid != SOL_NONE) RPU_DataWrite(ADDRESS_U11_B_CONTROL, 0x34);
  SolenoidByteChanged = false;
}

// RPU_MPU_ARCHITECTURE == 5
// The segments for one place of a module (row 0 = left), in PIA1 PA's form: bits 1-7 = a-g, bit 0 = the
// centre bar, inverted
static byte RPU_6803_Segments(byte row, byte place) {
  byte display = row * 2 + place / 7, digit = place % 7;
  if (!((DisplayDigitEnable[display] >> digit) & 0x01)) return 0x01;
  byte glyph;
  if ((DisplayRawPlaces6803[display] >> digit) & 0x01) glyph = DisplayRaw6803[display][digit];
  else glyph = RPU_6803_Glyph(DisplayIsText6803[display] ? DisplayText6803[display][digit] : ('0' + DisplayDigits[display][digit] % 10));
  return ((glyph & 0x7F) << 1) | ((glyph & 0x80) ? 0x00 : 0x01);
}

static inline void RPU_6803_DisplayWrite(int address, byte data) {
  RPU_DataWrite(address, data);
  delayMicroseconds(RPU_6803_DISPLAY_STEP_US);
}

// RPU_MPU_ARCHITECTURE == 5
// The game's order (its timer interrupt): blank (CA2 low lets the 4502 pass PA0/PA1), clear both modules'
// latches, the right module on PA1, the left one and the digit select on PA0, the commas (PIA1 PA again,
// unlatched: none), show. Digit select d shows place (20 - d) % 14 counting from the left (measured).
static void RPU_6803_ShowDigit(byte digitSelect) {
  byte place = (20 - digitSelect) % 14, high = digitSelect << 4;
  RPU_6803_DisplayWrite(ADDRESS_U10_A, 0xEF);
  RPU_6803_DisplayWrite(ADDRESS_U10_A_CONTROL, 0x34);
  RPU_6803_DisplayWrite(ADDRESS_U11_A, 0x01);
  RPU_6803_DisplayWrite(ADDRESS_U10_A, 0xFC);
  RPU_6803_DisplayWrite(ADDRESS_U10_A, 0xFF);
  RPU_6803_DisplayWrite(ADDRESS_U11_A, RPU_6803_Segments(1, place));
  RPU_6803_DisplayWrite(ADDRESS_U10_A, 0x0D);
  RPU_6803_DisplayWrite(ADDRESS_U10_A, 0x0F);
  RPU_6803_DisplayWrite(ADDRESS_U11_A, RPU_6803_Segments(0, place));
  RPU_6803_DisplayWrite(ADDRESS_U10_A, high | 0x0E);
  RPU_6803_DisplayWrite(ADDRESS_U10_A, high | 0x0F);
  RPU_6803_DisplayWrite(ADDRESS_U11_A, 0x00);
  RPU_6803_DisplayWrite(ADDRESS_U10_A_CONTROL, 0x3C);
  RPU_DataWrite(ADDRESS_U10_A, 0xE0);
}

// INTERRUPT SERVICE ROUTINE
// RPU_MPU_ARCHITECTURE == 5, every 1 ms
ISR(TIMER1_COMPA_vect) {
  // Nothing while the board's reset holds the PIAs
  if (!gpio_get(RPU_MPU_RESET_PIN)) return;

  uint32_t tickStart = time_us_32();

  // 1. One switch column: strobe, settle, read, strobe off
  byte column = SwitchColumn6803;
  if (column < NUM_SWITCH_BYTES_ON_U10_PORT_A) {
    RPU_DataWrite(ADDRESS_U10_A, 0xE0 | (0x01 << column));
  } else {
    RPU_DataWrite(ADDRESS_U10_A, 0xE0);
    RPU_DataWrite(ADDRESS_U11_B, (CurrentSolenoidByte & 0xEF) | ST5_CONTINUOUS_SOLENOID_BIT);
  }
  delayMicroseconds(RPU_6803_SWITCH_SETTLE_US);
  byte returns = RPU_DataRead(ADDRESS_U10_B) ^ SwitchInverter[column];
  if (column == 5 || SolenoidByteChanged) {
    RPU_DataWrite(ADDRESS_U11_B, CurrentSolenoidByte & 0xEF);
    SolenoidByteChanged = false;
  }
  RPU_DataWrite(ADDRESS_U10_A, 0xE0);
  RPU_6803_ProcessSwitchColumn(column, returns);
  SwitchColumn6803 = (column + 1) % NUM_SWITCH_BYTES;

  // 2. The self-test switch (PIA0 CA1's flag; reading PA clears it)
  if (RPU_DataRead(ADDRESS_U10_A_CONTROL) & 0x80) {
    PushToSwitchStack(SW_SELF_TEST_SWITCH);
    RPU_DataRead(ADDRESS_U10_A);
  }

  // 3. Zero crossings (P20 changes): solenoids and this half's lamps; the lamps again inside the half
  boolean phaseA = gpio_get(RPU_6803_P20_PIN);
  uint32_t now = time_us_32();
  if (phaseA != PhaseA6803) {
    PhaseA6803 = phaseA;
    PhaseEdgeUs6803 = now;
    ZeroCrossPasses6803 += 1;
    RPU_6803_UpdateFlashers();
    RPU_6803_ZeroCrossSolenoids();
    RPU_6803_FireLamps(!phaseA);
  } else if ((now - PhaseEdgeUs6803) < RPU_6803_REGATE_US) {
    RPU_6803_FireLamps(!phaseA);
  }

  // 4. One display digit
  RPU_6803_ShowDigit(DisplayDigit6803);
  DisplayDigit6803 = (DisplayDigit6803 + 1) % 14;

  uint32_t tickUs = time_us_32() - tickStart;
  if (tickUs > RPU_6803_MaxTickUs) RPU_6803_MaxTickUs = tickUs;
}

// RPU_MPU_ARCHITECTURE == 5
// As Rev 401: the AVR's timer interrupt becomes a repeating alarm, the handler masked as an AVR ISR
static repeating_timer_t RPU_6803Timer;
static bool RPU_6803TimerCallback(repeating_timer_t *) {
  noInterrupts();
  RPU_TimerService_TIMER1_COMPA_vect();
  interrupts();
  return true;
}

// RPU_MPU_ARCHITECTURE == 5
unsigned long RPU_InitializeMPU6803(unsigned long initOptions, byte creditResetSwitch) {
  (void)creditResetSwitch;
  // The 400-IC takes the 6803's place, so there's no original game to hand over to (that needs the 6803
  // back in its socket): RPU runs whatever was asked, as if a selector switch were set to RPU
  unsigned long retResult = RPU_RET_SELECTOR_SWITCH_ON;

  // The RP2350 keeps "EEPROM" in flash (see Rev 400)
  if (EEPROM.length() == 0) EEPROM.begin(4096);
  RPU_InitBusPins();
  RPU_WaitForMPUReset();
  delay(100);

  RPU_ClearVariables();
  RPU_6803_InitializePIAs();
  if (initOptions & RPU_CMD_PERFORM_MPU_TEST) retResult |= RPU_TestPIAs();

  PhaseA6803 = gpio_get(RPU_6803_P20_PIN);
  PhaseEdgeUs6803 = time_us_32();
  add_repeating_timer_us(-1000, RPU_6803TimerCallback, NULL, &RPU_6803Timer);
  return retResult;
}

#elif (RPU_MPU_ARCHITECTURE<10)

volatile int numberOfU10Interrupts = 0;
volatile int numberOfU11Interrupts = 0;
volatile byte InsideZeroCrossingInterrupt = 0;

// INTERRUPT SERVICE ROUTINE
// for ARCH 1 (B/S)
// RPU_MPU_ARCHITECTURE < 10
ISR(TIMER1_COMPA_vect) {    //This is the interrupt request

// Intercept the flag before the backup wipes it
  if (RPU_DataRead(ADDRESS_U10_A_CONTROL) & 0x80) {
    PushToSwitchStack(SW_SELF_TEST_SWITCH);
  }
  
  // Backup U10A
  byte backupU10A = RPU_DataRead(ADDRESS_U10_A);

  // Disable lamp decoders & strobe latch
  RPU_DataWrite(ADDRESS_U10_A, 0xFF);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) | 0x08);
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) & 0xF7);
#ifdef RPU_OS_USE_AUX_LAMPS
  // Also park the aux lamp board
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) | 0x08);
  RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) & 0xF7);
#endif

  // Blank Displays
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, RPU_DataRead(ADDRESS_U10_A_CONTROL) & 0xF7);
  // Set all 5 display latch strobes high
  RPU_DataWrite(ADDRESS_U11_A, (RPU_DataRead(ADDRESS_U11_A)) | 0x01);
  RPU_DataWrite(ADDRESS_U10_A, 0x0F);

  byte displayStrobeMask = 0x01;
  byte displayDigitsMask;
#ifdef RPU_OS_USE_7_DIGIT_DISPLAYS
  displayDigitsMask = (0x02 << CurrentDisplayDigit);
#else
  displayDigitsMask = RPU_DataRead(ADDRESS_U11_A) & 0x02;
  displayDigitsMask |= (0x04 << CurrentDisplayDigit);
#endif

  // Write current display digits to 5 displays
  for (int displayCount = 0; displayCount < 5; displayCount++) {

    // The BCD for this digit is in b4-b7, and the display latch strobes are in b0-b3 (and U11A:b0)
    byte displayDataByte = ((DisplayDigits[displayCount][CurrentDisplayDigit]) << 4) | 0x0F;
    byte displayEnable = ((DisplayDigitEnable[displayCount]) >> CurrentDisplayDigit) & 0x01;

    // if this digit shouldn't be displayed, then set data lines to 0xFX so digit will be blank
    if (!displayEnable) displayDataByte = 0xFF;

    // Calculate which bit needs to be dropped
    if (displayCount < 4) {
      displayDataByte &= ~(displayStrobeMask);
    }

    // Write out the digit & strobe (if it's 0-3)
    // The current number to display is the upper nibble of displayDataByte,
    // and the lower nibble is the strobe lines for the four score displays.
    // The strobe for the four score displays is high here because then the strobes
    // are NOR'd with U10:CA2 (which mutes the signals during other actions).
    // Only one strobe is low (from the above line.
    RPU_DataWrite(ADDRESS_U10_A, displayDataByte);
    if (displayCount == 4) {
      // Strobe #5 latch on U11A:b0
      RPU_DataWrite(ADDRESS_U11_A, displayDigitsMask & 0xFE);
    }

    // Right now the "Display Latch Strobe" is high

    // Put the latch strobe bits back high (low on the port)
    delayMicroseconds(16);
    if (displayCount < 4) {
      displayDataByte |= 0x0F;
      // Need to delay a little to make sure the strobe is low (high on the port) for long enough
      RPU_DataWrite(ADDRESS_U10_A, displayDataByte);
    } else {
      RPU_DataWrite(ADDRESS_U11_A, displayDigitsMask | 0x01);
    }

    displayStrobeMask *= 2;
  }

  // While the data is being strobed, we need to enable the current digit
  RPU_DataWrite(ADDRESS_U11_A, displayDigitsMask | 0x01);

  CurrentDisplayDigit = CurrentDisplayDigit + 1;
  if (CurrentDisplayDigit >= RPU_OS_NUM_DIGITS) {
    CurrentDisplayDigit = 0;
    DisplayOffCycle ^= true;
  }

  // Stop Blanking (current digits are all latched and ready)
  RPU_DataWrite(ADDRESS_U10_A_CONTROL, RPU_DataRead(ADDRESS_U10_A_CONTROL) | 0x08);

  // Restore 10A from backup
  RPU_DataWrite(ADDRESS_U10_A, backupU10A);

}

// RPU_MPU_ARCHITECTURE < 10
void InterruptService3() {
  byte u10AControl = RPU_DataRead(ADDRESS_U10_A_CONTROL);
  if (u10AControl & 0x80) {
    // self test switch
    if (RPU_DataRead(ADDRESS_U10_A_CONTROL) & 0x80) PushToSwitchStack(SW_SELF_TEST_SWITCH);
    RPU_DataRead(ADDRESS_U10_A);
  }

  // If we get a weird interupt from U11B, clear it
  byte u11BControl = RPU_DataRead(ADDRESS_U11_B_CONTROL);
  if (u11BControl & 0x80) {
    RPU_DataRead(ADDRESS_U11_B);
  }

  byte u11AControl = RPU_DataRead(ADDRESS_U11_A_CONTROL);
  byte u10BControl = RPU_DataRead(ADDRESS_U10_B_CONTROL);

  // If the interrupt bit on the display interrupt is on, do the display refresh
  if (u11AControl & 0x80) {
    RPU_DataRead(ADDRESS_U11_A);
    numberOfU11Interrupts += 1;
  }

  // If the IRQ bit of U10BControl is set, do the Zero-crossing interrupt handler
  if ((u10BControl & 0x80)) {
    // Read U10B to clear interrupt
    RPU_DataRead(ADDRESS_U10_B);
  }
  if ((u10BControl & 0x80) && (InsideZeroCrossingInterrupt == 0)) {

    InsideZeroCrossingInterrupt = InsideZeroCrossingInterrupt + 1;
    // Read U10B to clear interrupt
    RPU_DataRead(ADDRESS_U10_B);

    byte u10BControlLatest = RPU_DataRead(ADDRESS_U10_B_CONTROL);

    // Backup contents of U10A
    byte backup10A = RPU_DataRead(ADDRESS_U10_A);

    // Latch 0xFF separately without interrupt clear
    RPU_DataWrite(ADDRESS_U10_A, 0xFF);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) | 0x08);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) & 0xF7);

    // Turn off U10BControl interrupts
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x30);

    // Copy old switch values
    byte switchCount;
    byte startingClosures;
    byte validClosures;
    byte tempSwitchesNow[NUM_SWITCH_BYTES];
    
    for (switchCount = 0; switchCount < NUM_SWITCH_BYTES; switchCount++) {
      // Enable switch strobe
#if defined(RPU_USE_EXTENDED_SWITCHES_ON_PB4) or defined(RPU_USE_EXTENDED_SWITCHES_ON_PB7)
      if (switchCount < NUM_SWITCH_BYTES_ON_U10_PORT_A) {
        RPU_DataWrite(ADDRESS_U10_A, 0x01 << switchCount);
      } else {
        RPU_SetContinuousSolenoidBit(true, ST5_CONTINUOUS_SOLENOID_BIT);
      }
#else
      RPU_DataWrite(ADDRESS_U10_A, 0x01 << switchCount);
#endif

      // Turn off U10:CB2 if it's on (because it strobes the last bank of dip switches
      RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);

      // Delay for switch capacitors to charge
      delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);

      // Read the switches
      tempSwitchesNow[switchCount] = RPU_DataRead(ADDRESS_U10_B) ^ SwitchInverter[switchCount];

      //Unset the strobe
      RPU_DataWrite(ADDRESS_U10_A, 0x00);
#if defined(RPU_USE_EXTENDED_SWITCHES_ON_PB4) or defined(RPU_USE_EXTENDED_SWITCHES_ON_PB7)
      RPU_SetContinuousSolenoidBit(false, ST5_CONTINUOUS_SOLENOID_BIT);
#endif

      // There are no port reads or writes for the rest of the loop, 
      // so we can allow the display interrupt to fire
      interrupts();
      
      // Wait so total delay will allow lamp SCRs to get to the proper voltage
      delayMicroseconds(RPU_OS_TIMING_LOOP_PADDING_IN_MICROSECONDS);
      
      noInterrupts();
    }
    RPU_DataWrite(ADDRESS_U10_A, backup10A);

#ifndef RPU_STREAMLINED_IMMEDIATE_SOLENOIDS

    for (switchCount = 0; (switchCount < NUM_SWITCH_BYTES); switchCount++) {

      SwitchesMinus2[switchCount] = SwitchesMinus1[switchCount];
      SwitchesMinus1[switchCount] = SwitchesNow[switchCount];
      SwitchesNow[switchCount] = tempSwitchesNow[switchCount];

      // Some switches need to trigger immediate closures (bumpers & slings)
      startingClosures = (SwitchesNow[switchCount]) & (~SwitchesMinus1[switchCount]);
      boolean immediateSolenoidFired = false;
      // If one of the switches is starting to close (off, on)
      if (startingClosures) {
        // Loop on bits of switch byte
        for (byte bitCount = 0; bitCount < 8 && immediateSolenoidFired == false; bitCount++) {
          // If this switch bit is closed
          if (startingClosures & 0x01) {
            byte startingSwitchNum = switchCount * 8 + bitCount;
            // Loop on immediate switch data
            for (int immediateSwitchCount = 0; immediateSwitchCount < NumGamePrioritySwitches && immediateSolenoidFired == false; immediateSwitchCount++) {
              // If this switch requires immediate action
              if (GameSwitches && startingSwitchNum == GameSwitches[immediateSwitchCount].switchNum) {
                // Start firing this solenoid (just one until the closure is validate
                PushToFrontOfSolenoidStack(GameSwitches[immediateSwitchCount].solenoid, 1);
                immediateSolenoidFired = true;
              }
            }
          }
          startingClosures = startingClosures >> 1;
        }
      }

      immediateSolenoidFired = false;
      validClosures = (SwitchesNow[switchCount] & SwitchesMinus1[switchCount]) & ~SwitchesMinus2[switchCount];
      // If there is a valid switch closure (off, on, on)
      if (validClosures) {
        // Loop on bits of switch byte
        for (byte bitCount = 0; bitCount < 8; bitCount++) {
          // If this switch bit is closed
          if (validClosures & 0x01) {
            byte validSwitchNum = switchCount * 8 + bitCount;
            // Loop through all switches and see what's triggered
            for (int validSwitchCount = 0; validSwitchCount < NumGameSwitches; validSwitchCount++) {

              // If we've found a valid closed switch
              if (GameSwitches && GameSwitches[validSwitchCount].switchNum == validSwitchNum) {

                // If we're supposed to trigger a solenoid, then do it
                if (GameSwitches[validSwitchCount].solenoid != SOL_NONE) {
                  if (validSwitchCount < NumGamePrioritySwitches && immediateSolenoidFired == false) {
                    PushToFrontOfSolenoidStack(GameSwitches[validSwitchCount].solenoid, GameSwitches[validSwitchCount].solenoidHoldTime);
                  } else {
                    RPU_PushToSolenoidStack(GameSwitches[validSwitchCount].solenoid, GameSwitches[validSwitchCount].solenoidHoldTime);
                  }
                } // End if this is a real solenoid
              } // End if this is a switch in the switch table
            } // End loop on switches in switch table
            // Push this switch to the game rules stack
            PushToSwitchStack(validSwitchNum);
          }
          validClosures = validClosures >> 1;
        }
      }

#else

    for (switchCount = 0; (switchCount < NUM_SWITCH_BYTES); switchCount++) {

      SwitchesMinus2[switchCount] = SwitchesMinus1[switchCount];
      SwitchesMinus1[switchCount] = SwitchesNow[switchCount];
      SwitchesNow[switchCount] = tempSwitchesNow[switchCount];

      // Streamlined version of solenoid handling
      boolean immediateSolenoidFired = false;
      // Some switches need to trigger immediate closures (bumpers & slings)
      startingClosures = (SwitchesNow[switchCount]) & (~SwitchesMinus1[switchCount]);
      if (startingClosures & ImmediateSolenoidSwitchMask[switchCount]) {
        // This switch requires an immediate solenoid response
        for (byte immediateTrigger = 0; immediateTrigger < NumGamePrioritySwitches; immediateTrigger++) {
          if (ImmediateSolenoidSwitchByte[immediateTrigger] == switchCount && (ImmediateSolenoidSwitchFlag[immediateTrigger]&startingClosures)) {
            // Start firing this solenoid (just one until the closure is validate
            PushToFrontOfSolenoidStack(GameSwitches[immediateTrigger].solenoid, 1);
            immediateSolenoidFired = true;
            break;
          }
        }
      }

      immediateSolenoidFired = false;
      validClosures = (SwitchesNow[switchCount] & SwitchesMinus1[switchCount]) & ~SwitchesMinus2[switchCount];
      // If there is a valid switch closure (off, on, on)
      if (validClosures) {

        // Fire solenoid, if it's registered to this switch
        if (validClosures & ImmediateSolenoidSwitchMask[switchCount]) {
          for (byte immediateTrigger = 0; immediateTrigger < NumGameSwitches; immediateTrigger++) {
            if (ImmediateSolenoidSwitchByte[immediateTrigger] == switchCount && (ImmediateSolenoidSwitchFlag[immediateTrigger]&validClosures)) {
              if (immediateTrigger < NumGamePrioritySwitches && immediateSolenoidFired == false) {
                PushToFrontOfSolenoidStack(GameSwitches[immediateTrigger].solenoid, GameSwitches[immediateTrigger].solenoidHoldTime);
              } else {
                RPU_PushToSolenoidStack(GameSwitches[immediateTrigger].solenoid, GameSwitches[immediateTrigger].solenoidHoldTime);
              }
            }
          }
        }

        // Now push any switches to the stack
        byte validSwitchNum = switchCount * 8;
        for (byte count = 0; count < 8; count++) {
          if (validClosures & 0x01) {
            PushToSwitchStack(validSwitchNum);
          }
          validSwitchNum += 1;
          validClosures /= 2;
        }

      }

#endif

    }

    if (NumCyclesBeforeRevertingSolenoidByte != 0) {
      NumCyclesBeforeRevertingSolenoidByte -= 1;
      if (NumCyclesBeforeRevertingSolenoidByte == 0) {
        CurrentSolenoidByte |= RevertSolenoidBit;
        RevertSolenoidBit = 0x00;
      }
    }

    // If we need to turn off momentary solenoids, do it first
    byte momentarySolenoidAtStart = PullFirstFromSolenoidStack();
    if (momentarySolenoidAtStart != SOLENOID_STACK_EMPTY) {
      CurrentSolenoidByte = (CurrentSolenoidByte & 0xF0) | momentarySolenoidAtStart;
      RPU_DataWrite(ADDRESS_U11_B, CurrentSolenoidByte);
    } else {
      CurrentSolenoidByte = (CurrentSolenoidByte & 0xF0) | SOL_NONE;
      RPU_DataWrite(ADDRESS_U11_B, CurrentSolenoidByte);
    }

    for (int lampByteCount = 0; lampByteCount < 8; lampByteCount++) {
      for (byte nibbleCount = 0; nibbleCount < 2; nibbleCount++) {

        // We skip iteration number 16 because the last position is to park the lamps
        if (lampByteCount == (7) && nibbleCount) continue;

        byte lampData = 0xF0 + (lampByteCount * 2) + nibbleCount;

        interrupts();
        __asm__ __volatile__("nop\n\t");
        noInterrupts();
        RPU_DataWrite(ADDRESS_U10_A, 0xFF);

        // Latch address & strobe
        RPU_DataWrite(ADDRESS_U10_A, lampData);
#ifdef RPU_SLOW_DOWN_LAMP_STROBE
        delayMicroseconds(2);
#endif

        RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x38);
#ifdef RPU_SLOW_DOWN_LAMP_STROBE
        delayMicroseconds(2);
#endif

        RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x30);
#ifdef RPU_SLOW_DOWN_LAMP_STROBE
        delayMicroseconds(2);
#endif

        byte curLampByte = LampStates[lampByteCount];
        // Every other time through the cycle, we OR in the dim variable
        // in order to dim those lights
        if (numberOfU10Interrupts % DimDivisor1) curLampByte |= (LampDim1[lampByteCount]);
        if (numberOfU10Interrupts % DimDivisor2) curLampByte |= (LampDim2[lampByteCount]);

#if defined(RPU_OS_USE_LAMP_OVERLAYS)
        if (UseLampsOffOverlay) curLampByte |= LampsOffOverlay[lampByteCount];
        if (UseLampsOnOverlay) curLampByte &= LampsOnOverlay[lampByteCount];
#endif

        // Use the inhibit lines to set the actual data to the lamp SCRs
        // (here, we don't care about the lower nibble because the address was already latched)
        byte nibbleOffset = (nibbleCount) ? 1 : 16;
        byte lampOutput = (curLampByte * nibbleOffset);

        RPU_DataWrite(ADDRESS_U10_A, lampOutput | 0x0F);
#ifdef RPU_SLOW_DOWN_LAMP_STROBE
        delayMicroseconds(2);
#endif
#if (RPU_OS_HARDWARE_REV==400)
        delayMicroseconds(RPU_400_LAMP_GATE_US);   // hold the gate pulse (see RPU_400_LAMP_GATE_US)
#endif
      } // end loop on nibble
    } // end loop on lamp bytes


#ifdef RPU_OS_USE_AUX_LAMPS
    // Latch 0xFF separately without interrupt clear
    // to park 0xFF in main lamp board
    RPU_DataWrite(ADDRESS_U10_A, 0xFF);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) | 0x08);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) & 0xF7);

    // For the first four bits of lamps, we're going to look at LampStates[7] again
    // and use those top 4 bits that we didn't use before. Then we're going
    // to move on with bytes 8, 9, and 10 for the remaining 24 bits of data
    byte auxBankNum = 0;
    for (int lampByteCount = 7; lampByteCount < RPU_NUM_LAMP_BANKS; lampByteCount++) {
      for (byte nibbleCount = 0; nibbleCount < 2; nibbleCount++) {
        if (lampByteCount == 7) nibbleCount = 1; // skip the first nibble of byte 7 because it belongs to primary lamps
        byte nibbleOffset = (nibbleCount) ? 1 : 16;

        byte curLampByte = LampStates[lampByteCount];
        // Every other time through the cycle, we OR in the dim variable
        // in order to dim those lights
        if (numberOfU10Interrupts % DimDivisor1) curLampByte |= (LampDim1[lampByteCount]);
        if (numberOfU10Interrupts % DimDivisor2) curLampByte |= (LampDim2[lampByteCount]);

#if defined(RPU_OS_USE_LAMP_OVERLAYS)
        if (UseLampsOffOverlay) curLampByte |= LampsOffOverlay[lampByteCount];
        if (UseLampsOnOverlay) curLampByte &= LampsOnOverlay[lampByteCount];
#endif

        byte lampOutput = (curLampByte * nibbleOffset);

        // The data will be in the upper nibble, but we need the bank count in the lower
        lampOutput &= 0xF0;
        lampOutput += auxBankNum;

        interrupts();
        __asm__ __volatile__("nop\n\t");
        noInterrupts();
        RPU_DataWrite(ADDRESS_U10_A, 0xFF);

        RPU_DataWrite(ADDRESS_U10_A, lampOutput | 0xF0);
        RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) | 0x08);
        RPU_DataWrite(ADDRESS_U11_A_CONTROL, RPU_DataRead(ADDRESS_U11_A_CONTROL) & 0xF7);
        RPU_DataWrite(ADDRESS_U10_A, lampOutput);

        auxBankNum += 1;
      }
    }
#endif

    // Latch 0xFF separately without interrupt clear
    RPU_DataWrite(ADDRESS_U10_A, 0xFF);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) | 0x08);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, RPU_DataRead(ADDRESS_U10_B_CONTROL) & 0xF7);

    interrupts();
    __asm__ __volatile__("nop\n\t");
    noInterrupts();

    InsideZeroCrossingInterrupt = 0;
    RPU_DataWrite(ADDRESS_U10_A, backup10A);
    RPU_DataWrite(ADDRESS_U10_B_CONTROL, u10BControlLatest);

    // Read U10B to clear interrupt
    RPU_DataRead(ADDRESS_U10_B);
    numberOfU10Interrupts += 1;
  }
}

// RPU_MPU_ARCHITECTURE < 10
#if (RPU_OS_HARDWARE_REV==400)
// On the AVR an interrupt starts with interrupts masked, and RPU's handlers open a window with
// interrupts()/noInterrupts() where the IRQ may run. Arduino-pico's interrupts() is a stack, so each
// handler is wrapped in noInterrupts()/interrupts() to start masked the same way.
void RPU_TimerService_TIMER1_COMPA_vect();
static repeating_timer_t RPU_DisplayTimer;
static bool RPU_DisplayTimerCallback(repeating_timer_t *) {
  noInterrupts();
  RPU_TimerService_TIMER1_COMPA_vect();
  interrupts();
  return true;
}
static void RPU_IRQService() {
  noInterrupts();
  InterruptService3();
  interrupts();
}
void RPU_HookInterrupts() {
  // Timer1 on the Mega: 16 MHz / 1024 prescaler, compare match every (OCR1A + 1) ticks of 64 us
  add_repeating_timer_us(-64L * (RPU_OS_SOFTWARE_DISPLAY_INTERRUPT_INTERVAL + 1), RPU_DisplayTimerCallback, NULL, &RPU_DisplayTimer);
  // The MPU's IRQ (zero crossing, displays) may preempt the timer handler inside its windows, as on the AVR
  irq_set_priority(IO_IRQ_BANK0, PICO_HIGHEST_IRQ_PRIORITY);
  attachInterrupt(digitalPinToInterrupt(RPU_IRQ_PIN), RPU_IRQService, LOW);
}
#else
void RPU_HookInterrupts() {
  // Hook up the interrupt
  /*
    cli();
    TCCR2A|=(1<<WGM21);     //Set the CTC mode
    OCR2A=0xBA;            //Set the value for 3ms
    TIMSK2|=(1<<OCIE2A);   //Set the interrupt request
    TCCR2B|=(1<<CS22);     //Set the prescale 1/64 clock
    sei();                 //Enable interrupt
  */

  cli();
  //set timer1 interrupt at 1Hz
  TCCR1A = 0;// set entire TCCR1A register to 0
  TCCR1B = 0;// same for TCCR1B
  TCNT1  = 0;//initialize counter value to 0
  // set compare match register for selected increment
  OCR1A = RPU_OS_SOFTWARE_DISPLAY_INTERRUPT_INTERVAL;
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // Set CS10 and CS12 bits for 1024 prescaler
  TCCR1B |= (1 << CS12) | (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei();

  attachInterrupt(digitalPinToInterrupt(2), InterruptService3, LOW);
}
#endif


// RPU_MPU_ARCHITECTURE < 10
boolean LookFor6800Activity() {
  // Assume Arduino pins all start as input
  unsigned long startTime = millis();
  boolean sawHigh = false;
  boolean sawLow = false;
  // for one second, look for activity on the VMA line (A5)
  // If we see anything, then the MPU is active so we shouldn't run
  while ((millis() - startTime) < 1000) {
#if (RPU_OS_HARDWARE_REV==400)
    if (gpio_get(RPU_VMA_PIN)) sawHigh = true;
#else
    if (PINC & 0x20) sawHigh = true;
#endif
    else sawLow = true;
  }
  // If we saw both a high and low signal, then someone is toggling the
  // VMA line, so we should hang here forever (until reset)
  if (sawHigh && sawLow) {
    return true;
  }
  return false;
}

// RPU_MPU_ARCHITECTURE < 10
void SetupArduinoPorts() {
#if (RPU_OS_HARDWARE_REV==1)
  // Arduino A0 = MPU A0
  // Arduino A1 = MPU A1
  // Arduino A2 = MPU A3
  // Arduino A3 = MPU A4
  // Arduino A4 = MPU A7
  // Arduino A5 = MPU VMA
  // Set up the address lines A0-A7 as output
  DDRC = DDRC | 0x3F;
  // Set up D13 as address line A5 (and set it low)
  DDRB = DDRB | 0x20;
  PORTB = PORTB & 0xDF;
  // Set up control lines & data lines
  DDRD = DDRD & 0xEB;
  DDRD = DDRD | 0xE8;
  // Set VMA OFF
  PORTC = PORTC & 0xDF;
  // Set R/W to HIGH
  PORTD = (PORTD | 0x08);
#elif (RPU_OS_HARDWARE_REV==2)
  // Set up the address lines A0-A7 as output
  DDRC = DDRC | 0x3F;
  // Set up D13 as address line A7 (and set it high)
  DDRB = DDRB | 0x20;
  PORTB = PORTB | 0x20;
  // Set up control lines & data lines
  DDRD = DDRD & 0xEB;
  DDRD = DDRD | 0xE8;
  // Set VMA OFF
  PORTC = PORTC & 0xDF;
  // Set R/W to HIGH
  PORTD = (PORTD | 0x08);
#elif (RPU_OS_HARDWARE_REV==3)
  pinMode(3, INPUT); // CLK
  pinMode(4, OUTPUT); // VMA
  pinMode(5, OUTPUT); // R/W
  for (byte count = 6; count < 13; count++) pinMode(count, INPUT); // D0-D6
  pinMode(13, INPUT); // Switch
  pinMode(14, OUTPUT); // Halt
  pinMode(15, INPUT); // D7
  for (byte count = 16; count < 32; count++) pinMode(count, OUTPUT); // Address lines are output
  digitalWrite(5, HIGH);  // Set R/W line high (Read)
  digitalWrite(4, LOW);  // Set VMA line LOW
#elif (RPU_OS_HARDWARE_REV==4)
#endif

}

// RPU_MPU_ARCHITECTURE < 10
boolean CheckCreditResetSwitchArch1(byte creditResetSwitch) {
  // Check for credit button
  InitializeU10PIA();
  InitializeU11PIA();

  byte strobeNum = 0x01 << (creditResetSwitch / 8);
  byte switchNum = 0x01 << (creditResetSwitch % 8);
  RPU_DataWrite(ADDRESS_U10_A, strobeNum);
  // Turn off U10:CB2 if it's on (because it strobes the last bank of dip switches
  RPU_DataWrite(ADDRESS_U10_B_CONTROL, 0x34);

  // Delay for switch capacitors to charge
  delayMicroseconds(RPU_OS_SWITCH_DELAY_IN_MICROSECONDS);

  // Read the switches
  byte curSwitchByte = RPU_DataRead(ADDRESS_U10_B);

  //Unset the strobe
  RPU_DataWrite(ADDRESS_U10_A, 0x00);

  if (curSwitchByte & switchNum) {
    return true;
  }
  return false;
}

// RPU_MPU_ARCHITECTURE < 10
unsigned long RPU_InitializeMPUArch1(unsigned long initOptions, byte creditResetSwitch) {
  unsigned long retResult = RPU_RET_NO_ERRORS;
#if (RPU_OS_HARDWARE_REV==400)
  // The RP2350 keeps "EEPROM" in flash: a RAM copy, saved by RPU_Update. Open it unless someone
  // already has (PinNode can start first): opening it again would drop changes not yet saved.
  if (EEPROM.length() == 0) EEPROM.begin(4096);
#endif
  // Wait for board to boot
  delayMicroseconds(50000);
  delayMicroseconds(50000);

#if (RPU_OS_HARDWARE_REV==1) or (RPU_OS_HARDWARE_REV==2)
  (void)creditResetSwitch;

  if (initOptions & ( RPU_CMD_BOOT_ORIGINAL | RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET |
                      RPU_CMD_BOOT_ORIGINAL_IF_SWITCH_CLOSED | RPU_CMD_AUTODETECT_ARCHITECTURE ) ) {
    retResult |= RPU_RET_OPTION_NOT_SUPPORTED;
  }

  if (LookFor6800Activity()) {
    if (initOptions & RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN) {
      retResult |= RPU_RET_ORIGINAL_CODE_REQUESTED;
      return retResult;
    } else {
      while (1);
    }
  }
#elif (RPU_OS_HARDWARE_REV==3)
  (void)creditResetSwitch;

//  if (DEBUG_MESSAGES) Serial.write("* Starting Setup for Rev 3\n");

  if (initOptions & ( RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET |
                      RPU_CMD_AUTODETECT_ARCHITECTURE ) ) {
    retResult |= RPU_RET_OPTION_NOT_SUPPORTED;
  }

  pinMode(13, INPUT);
  boolean switchStateClosed = digitalRead(13) ? false : true;
  boolean bootToOriginal = false;

  if (switchStateClosed) {
    retResult |= RPU_RET_SELECTOR_SWITCH_ON;
  }

  if (  (initOptions & RPU_CMD_BOOT_ORIGINAL) ||
        (switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_SWITCH_CLOSED)) ||
        (!switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_SWITCH_CLOSED)) ) {
    bootToOriginal = true;
  }

  if (bootToOriginal) {

//    if (DEBUG_MESSAGES) Serial.write("* Asked to boot to original\n");
//    if (DEBUG_MESSAGES) delay(100);

    // Let the 680X run
    pinMode(14, OUTPUT); // Halt
    digitalWrite(14, HIGH);
    if (initOptions & RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN) {
      retResult |= RPU_RET_ORIGINAL_CODE_REQUESTED;
      return retResult;
    } else {
      while (1);
    }
  } else {
    // Switch indicates the Arduino should run, so HALT the 680X
    pinMode(14, OUTPUT); // Halt
    digitalWrite(14, LOW);
  }


#elif (RPU_OS_HARDWARE_REV==4) || (RPU_OS_HARDWARE_REV>=101)
#if (RPU_OS_HARDWARE_REV==400)
  RPU_InitBusPins();
#endif
  // put the 680X buffers into tri-state
  pinMode(RPU_BUFFER_DISABLE, OUTPUT);
  digitalWrite(RPU_BUFFER_DISABLE, 1);

  // Set /HALT low so the processor doesn't come online
  // (on some hardware, HALT & RESET are combined)
  pinMode(RPU_HALT_PIN, OUTPUT);
  digitalWrite(RPU_HALT_PIN, 0);
  pinMode(RPU_RESET_PIN, OUTPUT);
  digitalWrite(RPU_RESET_PIN, 0);

  // Set VMA, R/W to OUTPUT
  pinMode(RPU_VMA_PIN, OUTPUT);
  pinMode(RPU_RW_PIN, OUTPUT);
  RPU_SetAddressPinsDirection(RPU_PINS_OUTPUT);

#if (RPU_OS_HARDWARE_REV==102)
  if (CheckForMPUClock()) UsesM6800Processor = true;
  else UsesM6800Processor = false;
#endif

  // Set PHI2 depending on processor type
  if (UsesM6800Processor) {
    pinMode(RPU_PHI2_PIN, INPUT);
  } else {
    pinMode(RPU_PHI2_PIN, OUTPUT);
  }

  delay(1000);
  //  RPU_DataWrite(ADDRESS_SB100, 0x01);
  boolean switchStateClosed = false;
#if (RPU_OS_HARDWARE_REV==400)
  // The 400 has no selector switch: it's as if the switch were set to run RPU
  switchStateClosed = true;
  retResult |= RPU_RET_SELECTOR_SWITCH_ON;
#else
  pinMode(RPU_SWITCH_PIN, INPUT);
  if (digitalRead(RPU_SWITCH_PIN)) {
    switchStateClosed = true;
    retResult |= RPU_RET_SELECTOR_SWITCH_ON;
  }
#endif

  boolean creditResetButtonHit = false;
  if ( creditResetSwitch != 0xFF && (initOptions & (RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET))) {
    // We have to check the credit/reset button to honor the init request
    creditResetButtonHit = CheckCreditResetSwitchArch1(creditResetSwitch);
    if (creditResetButtonHit) {
      retResult |= RPU_RET_CREDIT_RESET_BUTTON_HIT;
    }
  }

  boolean bootToOriginal = false;
  if (  (initOptions & RPU_CMD_BOOT_ORIGINAL) ||
        (switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_SWITCH_CLOSED)) ||
        (!switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_SWITCH_CLOSED)) ||
        (creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET)) ||
        (!creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET)) ) {
    bootToOriginal = true;
  }

#if (RPU_OS_HARDWARE_REV==102)
  // We need to make sure the clock direction
  // buffers are set the correct direction
  if (UsesM6800Processor) {
    pinMode(RPU_DISABLE_PHI_FROM_MPU, OUTPUT);
    digitalWrite(RPU_DISABLE_PHI_FROM_MPU, 0);
    pinMode(RPU_DISABLE_PHI_FROM_CPU, OUTPUT);
    digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 1);
    retResult |= RPU_RET_6800_DETECTED;
  } else {
    pinMode(RPU_DISABLE_PHI_FROM_MPU, OUTPUT);
    digitalWrite(RPU_DISABLE_PHI_FROM_MPU, 1);
    pinMode(RPU_DISABLE_PHI_FROM_CPU, OUTPUT);
    digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 0);
    retResult |= RPU_RET_6802_OR_8_DETECTED;
  }
#endif

  if (bootToOriginal) {
    // If the options guide us to original code, boot to original
    pinMode(RPU_BUFFER_DISABLE, OUTPUT); // IRQ
    // Turn on the tri-state buffers
    digitalWrite(RPU_BUFFER_DISABLE, 0);

    pinMode(RPU_PHI2_PIN, INPUT); // CLOCK
    pinMode(RPU_VMA_PIN, INPUT); // VMA
    pinMode(RPU_RW_PIN, INPUT); // R/W

    // Set all the pins to input so they'll stay out of the way
    RPU_SetDataPinsDirection(RPU_PINS_INPUT);
    RPU_SetAddressPinsDirection(RPU_PINS_INPUT);

    // Set /HALT high
#if (RPU_OS_HARDWARE_REV==400)
    // Release by letting go: the MPU's own pull-ups bring HALT and RESET high
    pinMode(RPU_HALT_PIN, INPUT);
    pinMode(RPU_RESET_PIN, INPUT);
#else
    pinMode(RPU_HALT_PIN, OUTPUT);
    digitalWrite(RPU_HALT_PIN, 1);
    pinMode(RPU_RESET_PIN, OUTPUT);
    digitalWrite(RPU_RESET_PIN, 1);
#endif

    retResult |= RPU_RET_ORIGINAL_CODE_REQUESTED;
    if (!(initOptions & RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN)) while (1);
    else return retResult;
  }

#endif
/*
  if (DEBUG_MESSAGES) {
    Serial.write("* About to init Arduino ports\n");
    delay(100);
  }
*/  
  SetupArduinoPorts();

  // Prep the address bus (all lines zero)
/*  
  if (DEBUG_MESSAGES) {
    Serial.write("* About to data read\n");
    delay(100);
  }
*/  
  RPU_DataRead(0);

/*
  if (DEBUG_MESSAGES) {
    Serial.write("* DataRead(0) done\n");
    delay(100);
  }
*/
  // Set up the PIAs
  InitializeU10PIA();
  InitializeU11PIA();

  // Read values from MPU dip switches
#ifdef RPU_OS_USE_DIP_SWITCHES
  ReadDipSwitches();
#endif

#if (RPU_OS_HARDWARE_REV==4) || (RPU_OS_HARDWARE_REV>100)
  pinMode(RPU_DIAGNOSTIC_PIN, INPUT);
  if (digitalRead(RPU_DIAGNOSTIC_PIN) == 1) retResult |= RPU_RET_DIAGNOSTIC_REQUESTED;
#endif

  // Reset address bus
  RPU_DataRead(0);
  RPU_ClearVariables();

/*
  if (DEBUG_MESSAGES) {
    Serial.write("* About to hook interrupts\n");
    delay(100);
  }
*/  

  RPU_HookInterrupts();
  RPU_DataRead(0);  // Reset address bus

  // Clear all possible interrupts by reading the registers
  RPU_DataRead(ADDRESS_U11_A);
  RPU_DataRead(ADDRESS_U11_B);
  RPU_DataRead(ADDRESS_U10_A);
  RPU_DataRead(ADDRESS_U10_B);
  if (initOptions & RPU_CMD_PERFORM_MPU_TEST) retResult |= RPU_TestPIAs();
  RPU_DataRead(0);  // Reset address bus

  return retResult;
}

#endif




#if (RPU_MPU_ARCHITECTURE>=10)
// RPU_MPU_ARCHITECTURE >= 10
boolean CheckSwitchStack(byte switchNum) {
  for (byte stackIndex = SwitchStackFirst; stackIndex != SwitchStackLast; stackIndex++) {
    if (stackIndex >= SWITCH_STACK_SIZE) stackIndex = 0;
    if (SwitchStack[stackIndex] == switchNum) return true;
  }
  return false;
}


volatile unsigned long LampPass = 0;
volatile byte LampStrobe = 0;
volatile byte DisplayStrobe = 0;
volatile byte InterruptPass = 0;
boolean NeedToTurnOffTriggeredSolenoids = true;
#if (RPU_OS_NUM_DIGITS==6)
byte BlankingBit[16] = {0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x01, 0x02, 0x01, 0x02, 0x04, 0x08, 0x010, 0x20, 0x01, 0x02};
#elif (RPU_OS_NUM_DIGITS==7)
byte BlankingBit[16] = {0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x02, 0x01, 0x02, 0x04, 0x08, 0x010, 0x20, 0x40};
#endif
volatile byte UpDownPassCounter = 0;

// INTERRUPT HANDLER
// RPU_MPU_ARCHITECTURE >= 10 and RPU_OS_HARDWARE_REV < 200 (and the 400-IC, which drives the PIAs itself)
#if (RPU_OS_HARDWARE_REV<200) || (RPU_OS_HARDWARE_REV==401)
ISR(TIMER1_COMPA_vect) {    //This is the interrupt request (running at 965.3 Hz)

  byte displayControlPortB = RPU_DataRead(PIA_DISPLAY_CONTROL_B);
  if (displayControlPortB & 0x80) {
    UpDownSwitch = true;
    UpDownPassCounter = 0;
    // Clear the interrupt
    RPU_DataRead(PIA_DISPLAY_PORT_B);
  } else {
    UpDownPassCounter += 1;
    if (UpDownPassCounter == 50) {
      UpDownSwitch = false;
      UpDownPassCounter = 0;
    }
  }

#if (RPU_MPU_ARCHITECTURE==15)
  // Create display data
  unsigned int digit1 = 0x0000;
  byte digit2 = 0x00;
  byte blankingBit = BlankingBit[DisplayStrobe];
  if (DisplayStrobe == 0) {
    if (DisplayBIPDigitEnable & blankingBit) digit1 = DisplayBIPDigits[0];
    if (DisplayCreditDigitEnable & blankingBit) digit2 = DisplayCreditDigits[0];
  } else if (DisplayStrobe < 8) {
    if (DisplayDigitEnable[0]&blankingBit) digit1 = FourteenSegmentASCII[DisplayText[0][DisplayStrobe - 1]];
    if (DisplayDigitEnable[2]&blankingBit) digit2 = DisplayDigits[2][DisplayStrobe - 1];
  } else if (DisplayStrobe == 8) {
    if (DisplayBIPDigitEnable & blankingBit) digit1 = DisplayBIPDigits[1];
    if (DisplayCreditDigitEnable & blankingBit) digit2 = DisplayCreditDigits[1];
  } else {
    if (DisplayDigitEnable[1]&blankingBit) digit1 = FourteenSegmentASCII[DisplayText[1][DisplayStrobe - 9]];
    if (DisplayDigitEnable[3]&blankingBit) digit2 = DisplayDigits[3][DisplayStrobe - 9];
  }
  // Show current display digit
  RPU_DataWrite(PIA_DISPLAY_PORT_A, BoardLEDs | DisplayStrobe);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_A, (digit1 >> 7) & 0x7F);
  RPU_DataWrite(PIA_ALPHA_DISPLAY_PORT_B, digit1 & 0x7F);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, digit2 & 0x7F);
#elif (RPU_MPU_ARCHITECTURE==13)
  // Create display data
  byte digit1 = 0x0F, digit2 = 0x0F;
  byte blankingBit = BlankingBit[DisplayStrobe];
  boolean comma12 = false, comma34 = false;

  if (DisplayStrobe == 0) {
    if (DisplayBIPDigitEnable & blankingBit) digit1 = DisplayBIPDigits[0];
    if (DisplayCreditDigitEnable & blankingBit) digit2 = DisplayCreditDigits[0];
  } else if (DisplayStrobe < 8) {
    if (DisplayDigitEnable[0]&blankingBit) digit1 = DisplayDigits[0][DisplayStrobe - 1];
    if (DisplayDigitEnable[2]&blankingBit) digit2 = DisplayDigits[2][DisplayStrobe - 1];

    if (DisplayStrobe == 1) {
      if (DisplayCommas & 0x02) comma12 = true;
      if (DisplayCommas & 0x20) comma34 = true;
    } else if (DisplayStrobe == 4) {
      if (DisplayCommas & 0x01) comma12 = true;
      if (DisplayCommas & 0x10) comma34 = true;
    }

  } else if (DisplayStrobe == 8) {
    if (DisplayBIPDigitEnable & blankingBit) digit1 = DisplayBIPDigits[1];
    if (DisplayCreditDigitEnable & blankingBit) digit2 = DisplayCreditDigits[1];
  } else {
    if (DisplayDigitEnable[1]&blankingBit) digit1 = DisplayDigits[1][DisplayStrobe - 9];
    if (DisplayDigitEnable[3]&blankingBit) digit2 = DisplayDigits[3][DisplayStrobe - 9];

    if (DisplayStrobe == 9) {
      if (DisplayCommas & 0x08) comma12 = true;
      if (DisplayCommas & 0x80) comma34 = true;
    } else if (DisplayStrobe == 12) {
      if (DisplayCommas & 0x04) comma12 = true;
      if (DisplayCommas & 0x40) comma34 = true;
    }

  }
  // Show current display digit
  RPU_DataWrite(PIA_DISPLAY_PORT_A, BoardLEDs | DisplayStrobe);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, digit1 * 16 | (digit2 & 0x0F));

  // show commas
  byte commaByte = RPU_DataRead(PIA_SOUND_COMMA_PORT_B) & 0x3F;
  if (comma12) commaByte |= 0x80;
  if (comma34) commaByte |= 0x40;
  RPU_DataWrite(PIA_SOUND_COMMA_PORT_B, commaByte);

#else
  // Create display data
  byte digit1 = 0x0F, digit2 = 0x0F;
  byte blankingBit = BlankingBit[DisplayStrobe];
  if (DisplayStrobe < 6) {
    if (DisplayDigitEnable[0]&blankingBit) digit1 = DisplayDigits[0][DisplayStrobe];
    if (DisplayDigitEnable[2]&blankingBit) digit2 = DisplayDigits[2][DisplayStrobe];
  } else if (DisplayStrobe < 8) {
    if (DisplayBIPDigitEnable & blankingBit) digit1 = DisplayBIPDigits[DisplayStrobe - 6];
  } else if (DisplayStrobe < 14) {
    if (DisplayDigitEnable[1]&blankingBit) digit1 = DisplayDigits[1][DisplayStrobe - 8];
    if (DisplayDigitEnable[3]&blankingBit) digit2 = DisplayDigits[3][DisplayStrobe - 8];
  } else {
    if (DisplayCreditDigitEnable & blankingBit) digit1 = DisplayCreditDigits[DisplayStrobe - 14];
  }
  // Show current display digit
  //  if (RPU_DataRead(PIA_DISPLAY_CONTROL_B) & 0x80) SawInterruptOnDisplayPortB1 = true;
  RPU_DataWrite(PIA_DISPLAY_PORT_A, /*BoardLEDs | */DisplayStrobe);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, 0xFF);
  RPU_DataWrite(PIA_DISPLAY_PORT_B, digit1 * 16 | (digit2 & 0x0F));

#endif

  DisplayStrobe += 1;
  if (DisplayStrobe >= 16) DisplayStrobe = 0;

  if (InterruptPass == 0) {

    // Show lamps
    byte curLampByte = LampStates[LampStrobe];
    if (LampPass % DimDivisor1) curLampByte |= LampDim1[LampStrobe];
    if (LampPass % DimDivisor2) curLampByte |= LampDim2[LampStrobe];

#if defined(RPU_OS_USE_LAMP_OVERLAYS)
    if (UseLampsOffOverlay) curLampByte |= LampsOffOverlay[LampStrobe];
    if (UseLampsOnOverlay) curLampByte &= LampsOnOverlay[LampStrobe];
#endif
    
    RPU_DataWrite(PIA_LAMPS_PORT_B, 0x01 << (LampStrobe));
    RPU_DataWrite(PIA_LAMPS_PORT_A, curLampByte);

    LampStrobe += 1;
    if ((LampStrobe) >= RPU_NUM_LAMP_BANKS) {
      LampStrobe = 0;
      LampPass += 1;
    }

    // Check coin door switches
    byte displayControlPortA = RPU_DataRead(PIA_DISPLAY_CONTROL_A);
    if (displayControlPortA & 0x80) {
      // If the diagnostic switch isn't on the stack already, put it there
      if (!CheckSwitchStack(SW_SELF_TEST_SWITCH)) PushToSwitchStack(SW_SELF_TEST_SWITCH);
      // Clear the interrupt
      RPU_DataRead(PIA_DISPLAY_PORT_A);
    }

    // Check switches
    byte switchColStrobe = 1;
    for (byte switchCol = 0; switchCol < 8; switchCol++) {
      // Cycle the debouncing variables
      SwitchesMinus2[switchCol] = SwitchesMinus1[switchCol];
      SwitchesMinus1[switchCol] = SwitchesNow[switchCol];
      // Turn on the strobe
      RPU_DataWrite(PIA_SWITCH_PORT_B, switchColStrobe);
      // Hold it up for 30 us
      delayMicroseconds(12);
      // Read switch input
      SwitchesNow[switchCol] = RPU_DataRead(PIA_SWITCH_PORT_A);
      switchColStrobe *= 2;
    }
    RPU_DataWrite(PIA_SWITCH_PORT_B, 0);

    // If there are any closures, add them to the switch stack
    for (byte switchCol = 0; switchCol < NUM_SWITCH_BYTES; switchCol++) {
      byte validClosures = (SwitchesNow[switchCol] & SwitchesMinus1[switchCol]) & ~SwitchesMinus2[switchCol];
      // If there is a valid switch closure (off, on, on)
      if (validClosures) {
        // Loop on bits of switch byte
        for (byte bitCount = 0; bitCount < 8; bitCount++) {
          // If this switch bit is closed
          if (validClosures & 0x01) {
            byte validSwitchNum = switchCol * 8 + bitCount;
            PushToSwitchStack(validSwitchNum);
          }
          validClosures = validClosures >> 1;
        }
      }
    }

  } else {
    // See if any solenoids need to be switched
    byte solenoidOn = PullFirstFromSolenoidStack();
    byte portA = ContinuousSolenoidBits & 0xFF;
    byte portB = ContinuousSolenoidBits / 256;
    if (solenoidOn != SOLENOID_STACK_EMPTY) {
      if (solenoidOn < 16) {
        unsigned short newSolenoidBytes = (1 << solenoidOn);
        portA |= (newSolenoidBytes & 0xFF);
        portB |= (newSolenoidBytes / 256);
        if (NeedToTurnOffTriggeredSolenoids) {
          RPU_DataWrite(PIA_LAMPS_CONTROL_B, 0x3C);
          RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x3C);
          RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
          RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);
          RPU_DataWrite(PIA_SOLENOID_CONTROL_A, 0x3C);
          RPU_DataWrite(PIA_DISPLAY_CONTROL_B, 0x3D);
          NeedToTurnOffTriggeredSolenoids = false;
        }
      } else {
        if (solenoidOn == 16) RPU_DataWrite(PIA_LAMPS_CONTROL_B, 0x34);
        if (solenoidOn == 17) RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x34);
        if (solenoidOn == 18) RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x34);
        if (solenoidOn == 19) RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x34);
        if (solenoidOn == 20) RPU_DataWrite(PIA_SOLENOID_CONTROL_A, 0x34);
        if (solenoidOn == 21) RPU_DataWrite(PIA_DISPLAY_CONTROL_B, 0x35);
        NeedToTurnOffTriggeredSolenoids = true;
      }
    } else if (NeedToTurnOffTriggeredSolenoids) {
      NeedToTurnOffTriggeredSolenoids = false;
      RPU_DataWrite(PIA_LAMPS_CONTROL_B, 0x3C);
      RPU_DataWrite(PIA_LAMPS_CONTROL_A, 0x3C);
      RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
      RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);
      RPU_DataWrite(PIA_SOLENOID_CONTROL_A, 0x3C);
      RPU_DataWrite(PIA_DISPLAY_CONTROL_B, 0x3D);
    }


#if defined(RPU_OS_USE_WTYPE_1_SOUND)
    // See if any sounds need to be added
    // (these are handled through solenoid lines)
    unsigned short soundOn = PullFirstFromSoundStack();
    if (soundOn != SOUND_STACK_EMPTY) {
      portA |= (soundOn & 0xFF);
      portB |= (soundOn / 256);
    }
#elif defined(RPU_OS_USE_WTYPE_2_SOUND)
    unsigned short soundOn = PullFirstFromSoundStack();
    if (soundOn != SOUND_STACK_EMPTY) {
      RPU_DataWrite(PIA_SOUND_COMMA_PORT_A, (~soundOn) & 0x7F);
    } else {
      RPU_DataWrite(PIA_SOUND_COMMA_PORT_A, 0x7F);
    }
#endif

    RPU_DataWrite(PIA_SOLENOID_PORT_A, portA);
#if (RPU_MPU_ARCHITECTURE==15)
    RPU_DataWrite(PIA_SOLENOID_11_PORT_B, portB);
#else
    RPU_DataWrite(PIA_SOLENOID_PORT_B, portB);
#endif
  }

  //  RPU_DataWrite(PIA_SOLENOID_11_PORT_B, InterruptPass);
  InterruptPass ^= 1;

}
#endif

// RPU_MPU_ARCHITECTURE >= 10
// This is for Arch >= 10
#if (RPU_OS_HARDWARE_REV==401)
// Timer1 on the Mega: 16 MHz, no prescaler, compare match every (OCR1A + 1) ticks: 965.3 Hz for 16574.
// The handler starts with interrupts masked, as an AVR ISR does (see Rev 400's RPU_HookInterrupts).
void RPU_TimerService_TIMER1_COMPA_vect();
static repeating_timer_t RPU_WilliamsTimer;
static bool RPU_WilliamsTimerCallback(repeating_timer_t *) {
  noInterrupts();
  RPU_TimerService_TIMER1_COMPA_vect();
  interrupts();
  return true;
}
void RPU_SetupInterrupt() {
  add_repeating_timer_us(-(int64_t)(((INTERRUPT_OCR1A_COUNTER + 1) * 1000000ULL) / 16000000ULL), RPU_WilliamsTimerCallback, NULL, &RPU_WilliamsTimer);
}
#else
void RPU_SetupInterrupt() {
  cli();
  //set timer1 interrupt at 1Hz
  TCCR1A = 0;// set entire TCCR1A register to 0
  TCCR1B = 0;// same for TCCR1B
  TCNT1  = 0;//initialize counter value to 0
  // set compare match register for selected increment
  // OCR1A = 30000; // very slow test value
  OCR1A = INTERRUPT_OCR1A_COUNTER;
  // turn on CTC mode
  TCCR1B |= (1 << WGM12);
  // Set CS10 and CS12 bits for 1024 prescaler
  TCCR1B |= (0 << CS12) | (0 << CS11) | (1 << CS10);
  // enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A);
  sei();
}
#endif


boolean RPU_DiagnosticModeRequested() {
  boolean bootToDiagnostics = false;
#if (RPU_OS_HARDWARE_REV==4) || (RPU_OS_HARDWARE_REV>100)
  pinMode(RPU_DIAGNOSTIC_PIN, INPUT);
  if (digitalRead(RPU_DIAGNOSTIC_PIN) == 1) bootToDiagnostics = true;
#endif

  return bootToDiagnostics;
}

// RPU_MPU_ARCHITECTURE >= 10
boolean CheckCreditResetSwitchArch10(byte creditResetButton) {
  byte strobeLine = 0x01 << (creditResetButton / 8);
  byte returnLine = 0x01 << (creditResetButton % 8);

  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_A, 0x00);
  RPU_DataWrite(PIA_SWITCH_CONTROL_A, 0x3C);

  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x38);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0xFF);
  RPU_DataWrite(PIA_SWITCH_CONTROL_B, 0x3C);
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0x00);

  RPU_DataWrite(PIA_SWITCH_PORT_B, strobeLine);
  // Hold it up for 30 us
  delayMicroseconds(12);

  // Read switch input
  byte switchValues = RPU_DataRead(PIA_SWITCH_PORT_A);
/*  
  if (DEBUG_MESSAGES) {
    char buf[128];
    sprintf(buf, "* switch return = 0x%02X\n", switchValues);
    Serial.write(buf);
  }
*/  
  RPU_DataWrite(PIA_SWITCH_PORT_B, 0);

  if (switchValues & returnLine) return true;
  return false;
}

/*****************************************************
 * 
 * 
 *   Initialization for Architecture 10 or greater
 *
 *
 *******************************************************/

// RPU_MPU_ARCHITECTURE >= 10
unsigned long RPU_InitializeMPUArch10(unsigned long initOptions, byte creditResetSwitch) {
  unsigned long retResult = RPU_RET_NO_ERRORS;

//  if (DEBUG_MESSAGES) Serial.write("* Init start\n");

  // put the 680X buffers into tri-state
  pinMode(RPU_BUFFER_DISABLE, OUTPUT);
  digitalWrite(RPU_BUFFER_DISABLE, 1);

  // Set /HALT low so the processor doesn't come online
  // (on some hardware, HALT & RESET are combined)
  pinMode(RPU_HALT_PIN, OUTPUT);
  digitalWrite(RPU_HALT_PIN, 0);
  pinMode(RPU_RESET_PIN, OUTPUT);
  digitalWrite(RPU_RESET_PIN, 0);
  pinMode(RPU_BA_PIN, OUTPUT);
  digitalWrite(RPU_BA_PIN, 0);

#if (RPU_OS_HARDWARE_REV==401)
  // The RP2350 keeps "EEPROM" in flash (see Rev 400), and the bus starts from the MPU's reset
  if (EEPROM.length() == 0) EEPROM.begin(4096);
  RPU_InitBusPins();
  RPU_WaitForMPUReset();
#endif

  // Determine if we can detect a
  // 6800 or 6802/8 and possibly override
  // value for UsesM6800Processor
#if (RPU_OS_HARDWARE_REV==102) || (RPU_OS_HARDWARE_REV==401)
  if (CheckForMPUClock()) UsesM6800Processor = true;
  else UsesM6800Processor = false;
#endif

  // Set VMA, R/W, and PHI2 to OUTPUT
  pinMode(RPU_VMA_PIN, OUTPUT);
  pinMode(RPU_RW_PIN, OUTPUT);
  if (!UsesM6800Processor) {
    pinMode(RPU_PHI2_PIN, OUTPUT);
//    if (DEBUG_MESSAGES) Serial.write("* compiled for 6802 or 6808\n");
  } else {
    pinMode(RPU_PHI2_PIN, INPUT);
//    if (DEBUG_MESSAGES) Serial.write("* compiled for 6800\n");
  }
  // Make sure PIA IV (solenoid) CB2 is off so that solenoids are off
  RPU_SetAddressPinsDirection(RPU_PINS_OUTPUT);
  RPU_DataWrite(PIA_SOLENOID_CONTROL_B, 0x30);
  GameOverLine = true;

  delay(1000);
  boolean switchStateClosed = false;
#if (RPU_OS_HARDWARE_REV==400) || (RPU_OS_HARDWARE_REV==401)
  // The 400 has no selector switch: it's as if the switch were set to run RPU
  switchStateClosed = true;
  retResult |= RPU_RET_SELECTOR_SWITCH_ON;
#else
  pinMode(RPU_SWITCH_PIN, INPUT);
  if (digitalRead(RPU_SWITCH_PIN)) {
    switchStateClosed = true;
    retResult |= RPU_RET_SELECTOR_SWITCH_ON;
  }
#endif

  boolean creditResetButtonHit = false;
  if ( creditResetSwitch != 0xFF && (initOptions & (RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET))) {
    // We have to check the credit/reset button to honor the init request
    creditResetButtonHit = CheckCreditResetSwitchArch10(creditResetSwitch);
    if (creditResetButtonHit) {
      retResult |= RPU_RET_CREDIT_RESET_BUTTON_HIT;
    }
  }

  boolean bootToOriginal = false;

  if (  (initOptions & RPU_CMD_BOOT_ORIGINAL) ||
        (switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_SWITCH_CLOSED))  ||
        (!switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_SWITCH_CLOSED))  ||
        (creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET))  ||
        (!creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET)) ) {
/*
    if (DEBUG_MESSAGES) {
      char buf[128];
      sprintf(buf, "* Booting to original (switch=%d, CR=%d)\n", switchStateClosed, creditResetButtonHit);
      Serial.write(buf);
    }
*/    
    bootToOriginal = true;
  }
#if (RPU_OS_HARDWARE_REV==401)
  // The 400-IC takes the CPU's place, so there's no original game to hand over to: the original needs
  // the CPU back in the socket. Run RPU whatever was asked.
  bootToOriginal = false;
#endif

  if (bootToOriginal) {
    // If the switch is off, allow 6808 to boot
    pinMode(RPU_BUFFER_DISABLE, OUTPUT);
    // Turn on the tri-state buffers
    digitalWrite(RPU_BUFFER_DISABLE, 0);

    pinMode(RPU_PHI2_PIN, INPUT); // CLOCK
    pinMode(RPU_VMA_PIN, INPUT); // VMA
    pinMode(RPU_RW_PIN, INPUT); // R/W
    pinMode(RPU_BA_PIN, INPUT);

#if (RPU_OS_HARDWARE_REV==102)
    // We need to make sure the clock direction
    // buffers are set the correct direction
    if (UsesM6800Processor) {
      pinMode(RPU_DISABLE_PHI_FROM_MPU, OUTPUT);
      digitalWrite(RPU_DISABLE_PHI_FROM_MPU, 0);
      pinMode(RPU_DISABLE_PHI_FROM_CPU, OUTPUT);
      digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 1);
      retResult |= RPU_RET_6800_DETECTED;
    } else {
      pinMode(RPU_DISABLE_PHI_FROM_MPU, OUTPUT);
      digitalWrite(RPU_DISABLE_PHI_FROM_MPU, 1);
      pinMode(RPU_DISABLE_PHI_FROM_CPU, OUTPUT);
      digitalWrite(RPU_DISABLE_PHI_FROM_CPU, 0);
      retResult |= RPU_RET_6802_OR_8_DETECTED;
    }
#endif

    // Set all the pins to input so they'll stay out of the way
    RPU_SetDataPinsDirection(RPU_PINS_INPUT);
    RPU_SetAddressPinsDirection(RPU_PINS_INPUT);

    // Set /HALT high
#if (RPU_OS_HARDWARE_REV==400)
    // Release by letting go: the MPU's own pull-ups bring HALT and RESET high
    pinMode(RPU_HALT_PIN, INPUT);
    pinMode(RPU_RESET_PIN, INPUT);
#else
    pinMode(RPU_HALT_PIN, OUTPUT);
    digitalWrite(RPU_HALT_PIN, 1);
    pinMode(RPU_RESET_PIN, OUTPUT);
    digitalWrite(RPU_RESET_PIN, 1);
#endif

    if (initOptions & RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN) {
      retResult |= RPU_RET_ORIGINAL_CODE_REQUESTED;
      return retResult;
    } else {
      while (1);
    }
  }

#if (RPU_OS_HARDWARE_REV>100)
  pinMode(RPU_DIAGNOSTIC_PIN, INPUT);
  if (digitalRead(RPU_DIAGNOSTIC_PIN) == 1) retResult |= RPU_RET_DIAGNOSTIC_REQUESTED;
#endif

  RPU_ClearVariables();
  RPU_SetAddressPinsDirection(RPU_PINS_OUTPUT);
  RPU_InitializePIAs();
  if (initOptions & RPU_CMD_PERFORM_MPU_TEST) {
//    if (DEBUG_MESSAGES) Serial.write("* Going to test PIAs\n");
    retResult |= RPU_TestPIAs();
  } else {
//    if (DEBUG_MESSAGES) Serial.write("* Not asked to test PIAs\n");
  }
  RPU_SetupInterrupt();

  return retResult;
}

#if (RPU_OS_HARDWARE_REV==200)

#define LISY_CMD_RESET                            0x64
#define LISY_CMD_GET_CONNECTED_HW                 0x00
#define LISY_CMD_GET_FIRMWARE_VER                 0x01
#define LISY_CMD_GET_API_VER                      0x02
#define LISY_CMD_GET_SIMPLE_LAMP_COUNT            0x03
#define LISY_CMD_GET_SOLENOID_COUNT               0x04
#define LISY_CMD_GET_SOUND_COUNT                  0x05
#define LISY_CMD_GET_SEGMENT_DISPLAY_COUNT        0x06
#define LISY_CMD_GET_SEGMENT_DISPLAY_DETAILS      0x07
#define LISY_CMD_GET_GAME_INFO                    0x08
#define LISY_CMD_GET_SWITCH_COUNT                 0x09
#define LISY_CMD_SET_SIMPLE_LAMP_ON               0x0B
#define LISY_CMD_SET_SIMPLE_LAMP_OFF              0x0C
#define LISY_CMD_ENABLE_SOLENOID_FULL_POWER       0x15
#define LISY_CMD_DISABLE_SOLENOID                 0x16
#define LISY_CMD_PULSE_SOLENOID                   0x17
#define LISY_CMD_SET_SOLENOID_PULSE_TIME          0x18
#define LISY_CMD_SET_SEGMENT_DISPLAY              0x1E
#define LISY_CMD_GET_STATUS_OF_SWITCH             0x28
#define LISY_CMD_GET_CHANGED_SWITCHES             0x29
#define LISY_CMD_PLAY_SOUND                       0x32
#define LISY_CMD_SET_SOUND_VOLUME                 0x36
#define LISY_CMD_PET_WATCHDOG                     0x65

#define LISY_RESPONSE_IDLE                        0x00
#define LISY_RESPONSE_SWITCHES                    0x01
#define LISY_RESPONSE_WATCHDOG                    0x02

#define LISY_GAME_OVER_SOLENOID                   23

#define LISY_EXPECT_QUEUE_SIZE 16

struct LISYExpectation {
  byte ResponseType;
  unsigned long SendTime;
};

struct LISYExpectation LISYExpectQueue[LISY_EXPECT_QUEUE_SIZE];
byte LISYExpectHead = 0;
byte LISYExpectTail = 0;
unsigned long LISYLastTimeSoundSent = 0;

// Push an expected response type and timestamp onto the queue
void RPU_LISYPushExpectation(byte responseType, unsigned long currentTime) {
  byte nextHead = (LISYExpectHead + 1) % LISY_EXPECT_QUEUE_SIZE;
  if (nextHead != LISYExpectTail) { // Prevent overflow
    LISYExpectQueue[LISYExpectHead].ResponseType = responseType;
    LISYExpectQueue[LISYExpectHead].SendTime = currentTime;
    LISYExpectHead = nextHead;
  }
}

// Pop the oldest expected response type
byte RPU_LISYPopExpectation() {
  if (LISYExpectHead == LISYExpectTail) return LISY_RESPONSE_IDLE; // Queue empty
  byte expected = LISYExpectQueue[LISYExpectTail].ResponseType;
  LISYExpectTail = (LISYExpectTail + 1) % LISY_EXPECT_QUEUE_SIZE;
  return expected;
}



boolean RPU_LISYRequestValue(byte command, byte &value) {
  // Flush the RX buffer to ensure we only read the response to this specific command
  while (LISYOutputSerial.available() > 0) {
    LISYOutputSerial.read();
  }

  // Send the single-byte request
  LISYOutputSerial.write(command);

  unsigned long startTime = millis();
  
  // Block for up to 20ms waiting for the 1-byte reply
  while (millis() - startTime < 20) {
    if (LISYOutputSerial.available() > 0) {
      value = LISYOutputSerial.read();
      return true; // Success, exit early
    }
  }
  
  return false;
}


boolean RPU_LISYRequestString(byte command, char *value, int maxLength) {
  // Flush the RX buffer
  while (LISYOutputSerial.available() > 0) {
    LISYOutputSerial.read();
  }

  // Send the single-byte request
  LISYOutputSerial.write(command);

  unsigned long startTime = millis();
  int currentIndex = 0;

  // Block for up to 50ms waiting for the complete string
  while (millis() - startTime < 50) {
    if (LISYOutputSerial.available() > 0) {
      char c = (char)LISYOutputSerial.read();
      
      // Prevent buffer overflow, leaving room for a forced null terminator
      if (currentIndex < maxLength - 1) {
        value[currentIndex++] = c;
        
        // Check if we hit the end of the string
        if (c == '\0') {
          return true;
        }
      } else {
        // We hit the maximum length without seeing a null terminator
        value[maxLength - 1] = '\0';
        return false;
      }
    }
  }
  
  // Timeout reached before finishing the string
  if (maxLength > 0) {
    value[0] = '\0'; // Safely empty the string on failure
  }
  return false;
}


// RPU_MPU_ARCHITCTURE < 15
void RPU_LISYSendScore(byte displayNumber, byte numDigits) {

#if (RPU_MPU_ARCHITECTURE>=13)
  // The score could have commas
  if (displayNumber<4) {
    // The score is just BCD
    noInterrupts();
    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      byte outputDigit = 0x0F;
      if (DisplayDigitEnable[displayNumber]&blankMask) outputDigit = DisplayDigits[displayNumber][digit];
      byte commaBit = 0x01 << (2 * displayNumber);
      if (digit==3 && (DisplayCommas&commaBit)) outputDigit |= 0x80;
      if (digit==0 && (DisplayCommas&(commaBit*2))) outputDigit |= 0x80;
      LISYOutputSerial.write(outputDigit);
      blankMask *= 2;
    }
    interrupts();
  } else if (displayNumber==4) {
    noInterrupts();

    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      if (DisplayCreditDigitEnable&blankMask) LISYOutputSerial.write(DisplayCreditDigits[digit]);
      else LISYOutputSerial.write(0x0F); // display a blank

      blankMask *= 2;
    }
    interrupts();
  } else if (displayNumber==5) {
    noInterrupts();
    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      if (DisplayBIPDigitEnable&blankMask) LISYOutputSerial.write(DisplayBIPDigits[digit]);
      else LISYOutputSerial.write(0x0F); // display a blank

      blankMask *= 2;
    }
    interrupts();
  }  
#else
  if (displayNumber<4) {
    // The score is just BCD
    noInterrupts();
    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      if (DisplayDigitEnable[displayNumber]&blankMask) LISYOutputSerial.write(DisplayDigits[displayNumber][digit]);
      else LISYOutputSerial.write(0x0F); // display a blank

      blankMask *= 2;
    }
    interrupts();
  } else if (displayNumber==4) {
    noInterrupts();
    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      if (DisplayCreditDigitEnable&blankMask) LISYOutputSerial.write(DisplayCreditDigits[digit]);
      else LISYOutputSerial.write(0x0F); // display a blank

      blankMask *= 2;
    }
    interrupts();
  } else if (displayNumber==5) {
    noInterrupts();
    LISYOutputSerial.write(LISY_CMD_SET_SEGMENT_DISPLAY+displayNumber);
    LISYOutputSerial.write(numDigits);
    byte blankMask = 0x01;
    for (byte digit=0; digit<numDigits; digit++) {
      if (DisplayBIPDigitEnable&blankMask) LISYOutputSerial.write(DisplayBIPDigits[digit]);
      else LISYOutputSerial.write(0x0F); // display a blank

      blankMask *= 2;
    }
    interrupts();
  }
#endif

}


void RPU_LISYSetSolenoidPulsetime(byte solNum, byte pulseTime) {
  noInterrupts();
  LISYOutputSerial.write(LISY_CMD_SET_SOLENOID_PULSE_TIME);
  LISYOutputSerial.write(solNum);
  LISYOutputSerial.write(pulseTime);
  interrupts();
}

void RPU_SetSolenoidDefaultPulse(byte solenoidNumber, byte pulseTimeMS) {
  RPU_LISYSetSolenoidPulsetime(solenoidNumber, pulseTimeMS);
}

volatile byte LISYMessageError = 0;

void RPU_LISYSetSimpleLampState(byte lampNum, byte lampOn) {
  if (lampOn) {
    if (LISYOutputSerial.write(LISY_CMD_SET_SIMPLE_LAMP_ON)==0) LISYMessageError += 1;
  } else {
    if (LISYOutputSerial.write(LISY_CMD_SET_SIMPLE_LAMP_OFF)==0) LISYMessageError += 1;
  }
  if (LISYOutputSerial.write(lampNum)==0) LISYMessageError += 1;
}

void RPU_LISYSendSolenoidPulse(byte solNum) {
  if (LISYOutputSerial.write(LISY_CMD_PULSE_SOLENOID)==0) LISYMessageError += 1;
  if (LISYOutputSerial.write(solNum)==0) LISYMessageError += 1;
}


void RPU_LISYSetSolenoid(boolean solOn, byte solNum) {
  noInterrupts();
  if (solOn) {
    LISYOutputSerial.write(LISY_CMD_ENABLE_SOLENOID_FULL_POWER);
    LISYOutputSerial.write(solNum);
  } else {
    LISYOutputSerial.write(LISY_CMD_DISABLE_SOLENOID);
    LISYOutputSerial.write(solNum);
  }
  interrupts();
}

void RPU_LISYSendSoundClearCommand() {
  noInterrupts();
  LISYOutputSerial.write(LISY_CMD_PLAY_SOUND);
  LISYOutputSerial.write(1); // track 1
  LISYOutputSerial.write(0); // Clear sound lines
  LISYLastTimeSoundSent = 0;
  interrupts();  
}

void RPU_LISYSendSoundCommand(byte soundNum) {
  if (soundNum>=LISYNumSounds) return;
  if (LISYLastTimeSoundSent) RPU_LISYSendSoundClearCommand();
  noInterrupts();
  LISYOutputSerial.write(LISY_CMD_PLAY_SOUND);
  LISYOutputSerial.write(1); // track 1
  LISYOutputSerial.write(soundNum); // sound to play on track 1
  LISYLastTimeSoundSent = millis();
  interrupts();  
}

volatile byte CurrentLampByte = 0;
// Rev 200 ISR for passing commands to LISY
ISR(TIMER1_COMPA_vect) {    //This is the interrupt request (running at 965.3 Hz)

  byte curLampState = LampStates[CurrentLampByte];
#if defined(RPU_OS_USE_LAMP_OVERLAYS)
  if (UseLampsOffOverlay) curLampState |= LampsOffOverlay[CurrentLampByte];
  if (UseLampsOnOverlay) curLampState &= LampsOnOverlay[CurrentLampByte];
#endif

  byte changedLamps = OldLampStates[CurrentLampByte] ^ curLampState;
  if (changedLamps) {
    byte curLampBit = 0x01;
    for (byte curBit = 0; curBit < 8; curBit++) {
      if (changedLamps & curLampBit) {
        byte lampNum = curBit + CurrentLampByte*8;
        RPU_LISYSetSimpleLampState(lampNum, (curLampState&curLampBit) ? false : true);
      }
      curLampBit *= 2;
    }
    OldLampStates[CurrentLampByte] = curLampState;
  }
  CurrentLampByte += 1;
  if (CurrentLampByte>=RPU_NUM_LAMP_BANKS) {
    CurrentLampByte = 0;
  }

  byte solenoidOn = PullFirstFromSolenoidStack();
  if (solenoidOn!=SOLENOID_STACK_EMPTY) {
    RPU_LISYSendSolenoidPulse(solenoidOn);
  }

}

void RPU_LISYFlushBuffer() {
  while (LISYOutputSerial.available()) LISYOutputSerial.read();
}

boolean RPU_LISYResetAndGetConnectedHardware(unsigned long numTicksToWait) {
  LISYOutputSerial.write(LISY_CMD_RESET);
  LISYOutputSerial.write(LISY_CMD_GET_CONNECTED_HW);

  unsigned long startTime = millis();
  boolean resetAckSeen = false;
  boolean hwStrSeen = false;
  uint8_t dat;
  while (millis()<(startTime+numTicksToWait)) {
    if (LISYOutputSerial.available() > 0) {
      dat = LISYOutputSerial.read();
      if (!resetAckSeen) {
        resetAckSeen = true;
      } else {
        if (dat==0) hwStrSeen = true;
      }
    }
  }

  // flush the buffer, just in case
  RPU_LISYFlushBuffer();

  return hwStrSeen;
}

void RPU_LISYGetFirmwareVersion() {
  RPU_LISYRequestString(LISY_CMD_GET_FIRMWARE_VER, LISYFirmwareVersion, 32);
}

void RPU_LISYGetAPIVersion() {
  RPU_LISYRequestString(LISY_CMD_GET_API_VER, LISYAPIVersion, 32);
}

void RPU_LISYGetSimpleLamps() {
  if (!RPU_LISYRequestValue(LISY_CMD_GET_SIMPLE_LAMP_COUNT, LISYNumSimpleLamps)) {
    LISYNumSimpleLamps = 64;
  }
}

void RPU_LISYGetNumSwitches() {
  if (!RPU_LISYRequestValue(LISY_CMD_GET_SWITCH_COUNT, LISYNumSwitches)) {
    LISYNumSwitches = 64;
  }
}


void RPU_LISYSetSoundVolume(byte newVolume) {
  if (newVolume>100) newVolume = 100;
    LISYOutputSerial.write(LISY_CMD_SET_SOUND_VOLUME);
    LISYOutputSerial.write(1);
    LISYOutputSerial.write(newVolume);
}


void RPU_LISYGetSoundCount() {
  if (!RPU_LISYRequestValue(LISY_CMD_GET_SOUND_COUNT, LISYNumSounds)) {
    LISYNumSounds = 32;
  }
}


void RPU_LISYSendGameOverState(boolean gameOver) {
  noInterrupts();
  if (!gameOver) {
    LISYOutputSerial.write(LISY_CMD_ENABLE_SOLENOID_FULL_POWER);
    LISYOutputSerial.write(LISY_GAME_OVER_SOLENOID);
  } else {
    LISYOutputSerial.write(LISY_CMD_DISABLE_SOLENOID);
    LISYOutputSerial.write(LISY_GAME_OVER_SOLENOID);
  }
  interrupts();
}



void RPU_LISYReadAllSwitches() {
  // Clear any lingering bytes in the RX buffer before starting
  while (LISYOutputSerial.available() > 0) {
    LISYOutputSerial.read();
  }

  for (byte switchId = 0; switchId < LISYNumSwitches; switchId++) {
    bool success = false;
    byte retries = 3; // Failsafe to prevent an infinite hang on a dead bus

    while (!success && retries > 0) {
      // Send the two-byte command
      LISYOutputSerial.write(LISY_CMD_GET_STATUS_OF_SWITCH);
      LISYOutputSerial.write(switchId);

      unsigned long startTime = millis();
      
      // Block for up to 10ms waiting for the 1-byte reply
      while (millis() - startTime < 10) {
        if (LISYOutputSerial.available() > 0) {
          byte response = LISYOutputSerial.read();
          
          // 0=Off, 1=On, 2=Not existing
          if (response == 1) {
            LISYSwitchStates[switchId] = 1;
          } else {
            LISYSwitchStates[switchId] = 0;
          }
          
          success = true;
          break; // Exit the 10ms wait loop
        }
      }
      
      if (!success) {
        retries--;
      }
    }
  }
}


void RPU_LISYProcessIncoming(unsigned long currentTime) {
  // 1. Prune dead expectations older than 50ms to recover from lost bytes
  while (LISYExpectHead != LISYExpectTail) {
    if (currentTime - LISYExpectQueue[LISYExpectTail].SendTime > 50) {
      LISYExpectTail = (LISYExpectTail + 1) % LISY_EXPECT_QUEUE_SIZE;
    } else {
      // The oldest item is valid, so all subsequent ones are too
      break; 
    }
  }

  // 2. Process all available bytes in the hardware buffer
  while (LISYOutputSerial.available() > 0) {
    uint8_t response = LISYOutputSerial.read();
    
    // Discard unexpected bytes immediately to keep the queue aligned
    if (LISYExpectHead == LISYExpectTail) {
      continue;
    }
    
    byte currentExpectation = RPU_LISYPopExpectation();
    
    if (currentExpectation == LISY_RESPONSE_SWITCHES) {
      if (response != 127) {
        // A switch changed state; update game logic
        uint8_t switchId = response & 0x7F;
        bool isClosed = (response & 0x80) != 0;

//        char buf[128];
//        sprintf(buf, "SW=0x%02X\n", switchID);
//        Serial.write(buf);

        if (isClosed) {
          if (switchId<64) PushToSwitchStack(switchId);
          else if (switchId==64) PushToSwitchStack(SW_SELF_TEST_SWITCH);
          LISYSwitchStates[switchId] = 1;
        } else {
          LISYSwitchStates[switchId] = 0;
        }
      }
    } 
    // LISY_RESPONSE_WATCHDOG is implicitly handled by simply popping it
  }
}

void RPU_LISYUpdate(unsigned long currentTime) {
  RPU_LISYProcessIncoming(currentTime);
  
  if (currentTime > (LISYLastWatchdog + 250)) {
    LISYLastWatchdog = currentTime;
    LISYOutputSerial.write(LISY_CMD_PET_WATCHDOG);
    RPU_LISYPushExpectation(LISY_RESPONSE_WATCHDOG, currentTime);
  } else if (LISYLastTimeSoundSent && (currentTime-LISYLastTimeSoundSent)>100) {
    RPU_LISYSendSoundClearCommand();
  } else {
    // Burst requests to clear out any backlog on Pete's board
    noInterrupts();
    for (byte i = 0; i < 3; i++) {
      LISYOutputSerial.write(LISY_CMD_GET_CHANGED_SWITCHES);
      RPU_LISYPushExpectation(LISY_RESPONSE_SWITCHES, currentTime);
    }
    interrupts();
  }
}



// RPU_MPU_ARCHITECTURE >= 10 (through LISY)
unsigned long RPU_InitializeMPUArch10ThroughLISY(unsigned long initOptions, byte creditResetSwitch) {
  unsigned long retResult = RPU_RET_NO_ERRORS;
  GameOverLine = true;

  boolean switchStateClosed = false;
  pinMode(RPU_SWITCH_PIN, INPUT_PULLUP);
  if (digitalRead(RPU_SWITCH_PIN)) {
    switchStateClosed = true;
    retResult |= RPU_RET_SELECTOR_SWITCH_ON;
  }

  boolean creditResetButtonHit = false;
/*  
  if ( creditResetSwitch != 0xFF && (initOptions & (RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET | RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET))) {
    // We have to check the credit/reset button to honor the init request
    creditResetButtonHit = CheckCreditResetSwitchArch10(creditResetSwitch);
    if (creditResetButtonHit) {
      retResult |= RPU_RET_CREDIT_RESET_BUTTON_HIT;
    }
  }
*/

  boolean bootToOriginal = false;

  // LISY version doesn't have a way to check credit/reset switch during
  // boot yet, so we're not going to honor that
  if (  (initOptions & RPU_CMD_BOOT_ORIGINAL) ||
        (switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_SWITCH_CLOSED))  ||
        (!switchStateClosed && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_SWITCH_CLOSED))  /* ||
        (creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_CREDIT_RESET))  ||
        (!creditResetButtonHit && (initOptions & RPU_CMD_BOOT_ORIGINAL_IF_NOT_CREDIT_RESET)) */ ) {
/*
    if (DEBUG_MESSAGES) {
      char buf[128];
      sprintf(buf, "* Booting to original (switch=%d, CR=%d)\n", switchStateClosed, creditResetButtonHit);
      Serial.write(buf);
    }
*/    
    bootToOriginal = true;
  }

  if (bootToOriginal) {
    if (initOptions & RPU_CMD_INIT_AND_RETURN_EVEN_IF_ORIGINAL_CHOSEN) {
      retResult |= RPU_RET_ORIGINAL_CODE_REQUESTED;
      return retResult;
    } else {
      while (1);
    }
  }

  pinMode(RPU_DIAGNOSTIC_PIN, INPUT);
  if (digitalRead(RPU_DIAGNOSTIC_PIN) == 1) retResult |= RPU_RET_DIAGNOSTIC_REQUESTED;

  for (byte count=0; count<127; count++) {
    LISYSwitchStates[count] = 0;
  }

  // Init LISY serial port
  LISYOutputSerial.begin(115200);
  if (!RPU_LISYResetAndGetConnectedHardware(3000)) {
    retResult |= RPU_RET_HOST_NOT_DETECTED;
  } else {
    RPU_LISYGetFirmwareVersion();
    RPU_LISYGetAPIVersion();
    RPU_LISYGetFirmwareVersion();
    RPU_LISYGetSimpleLamps();
    RPU_LISYGetNumSwitches();
    RPU_LISYGetSoundCount();
    RPU_LISYSetSoundVolume(100);

    // Read the current switch states
    RPU_LISYReadAllSwitches();

    LISYOutputSerial.write(LISY_CMD_GET_CHANGED_SWITCHES);
    RPU_LISYPushExpectation(LISY_RESPONSE_SWITCHES, millis());
  }

  RPU_ClearVariables();
  RPU_SetupInterrupt();
  return retResult;

}
#endif // RPU_OS_HARDWARE_REV==200

#endif // (RPU_MPU_ARCHITECTURE>=10)

#if (DEBUG_MESSAGES==1)
unsigned long LastSwitchReport = 0;
#endif

void RPU_Update(unsigned long currentTime) {

  if (RPU_MPU_ARCHITECTURE == 1) {
//    RPU_DataRead(0);
  }

  RPU_ApplyFlashToLamps(currentTime);
  RPU_UpdateTimedSolenoidStack(currentTime);
#if (RPU_MPU_ARCHITECTURE>=10) && (defined(RPU_OS_USE_WTYPE_1_SOUND) || defined(RPU_OS_USE_WTYPE_2_SOUND))
  RPU_UpdateTimedSoundStack(currentTime);
#endif

#if (RPU_OS_HARDWARE_REV==200)
  RPU_LISYUpdate(currentTime);
#endif

}





// This function should eventually support auto-detect and initialize the appropriate
// ISRs for the detected architecture.
unsigned long RPU_InitializeMPU(unsigned long initOptions, byte creditResetSwitch) {

  unsigned long retVal = 0;

#if (RPU_OS_HARDWARE_REV==200)
  retVal = RPU_InitializeMPUArch10ThroughLISY(initOptions, creditResetSwitch);
#elif (RPU_MPU_ARCHITECTURE==5)
  retVal = RPU_InitializeMPU6803(initOptions, creditResetSwitch);
#elif (RPU_MPU_ARCHITECTURE<10)
  retVal = RPU_InitializeMPUArch1(initOptions, creditResetSwitch);
#else
  retVal = RPU_InitializeMPUArch10(initOptions, creditResetSwitch);
#endif

  return retVal;
}
