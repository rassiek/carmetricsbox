# CarMetrics — wiring

Status: 5.3.0. Module-based wiring (no custom PCB), for a vehicle, a boat or a
generator. Parts: `docs/PARTS.md`. Pins below are the **defaults**; every pin can
be changed in the web app (System → Hardware, or on each sensor / actuator), and
**System → Hardware → Pins** shows what each GPIO can do and what uses it.

> Never connect 12 V to an ESP32 pin. ESP32 pins take **3.3 V** at most.
> Fuse every wire that comes from the battery, at the battery end.

---

## 1. Power

| From | To | Notes |
|---|---|---|
| Ignition-switched 12 V → 2 A fuse | buck converter IN+ | wide-input 12/24 V → 5 V 3 A module |
| Chassis ground | buck IN−, every module's GND | one ground point per box |
| Buck 5 V OUT | ESP32 **5V** pin, ADS1115 VDD, level shifter HV, 1-Wire pull-up, senders' 5 V | add 1000 µF across 5 V / GND (cranking dips) |
| ESP32 **3V3** pin | level shifter LV, BME280, MPU-6050, OLED | the board's own regulator is enough for these |

A car USB charger with a cut USB cable works as the 5 V supply too.

## 2. Main board (ESP32-S3 N16R8 / N8R8)

| Function | Default GPIO | Wire to |
|---|---|---|
| I2C SDA / SCL | 8 / 9 | BME280, MPU-6050, OLED, ADS1115 (through the level shifter if it runs on 5 V) |
| 1-Wire | 15 | DS18B20 data, 4.7 kΩ to 3.3 V (or to 5 V through the shifter) |
| Buzzer | 16 | 3-pin active buzzer module IN (most sound on LOW) |
| Warning LED | 17 | 100 Ω → LED → GND |
| Built-in RGB | 48 (DevKitC v1.1: 38) | on the board |
| External RGB LED | 10 / 11 / 47 (R / G / B) | each through its resistor (150 Ω red, 100 Ω green / blue); common cathode to GND, or common anode to 3.3 V (tick *common anode*) |
| Analog (3.3 V max) | 4, 5, 6, 7 | only through a divider; prefer the ADS1115 |
| Spare outputs | 12, 13, 14, 18, 21 | relays, PWM, servo |

Avoid GPIO 0, 3, 45, 46 (boot pins), 19 / 20 (USB), 43 / 44 (serial console),
35–37 (PSRAM on R8 boards).

## 3. Engine-bay worker (classic ESP32 or S3)

### 3.1 ADS1115 at 5 V (0.5–4.5 V senders)

| ADS1115 | To |
|---|---|
| VDD / GND | 5 V / GND |
| SDA / SCL | level shifter HV1 / HV2; LV1 / LV2 → ESP32 SDA / SCL |
| ADDR | GND = 0x48 (VDD = 0x49 for a second module) |
| A0–A3 | sender signal wires |

Powering the ADS1115 and the senders from the **same 5 V** keeps readings right
when the 5 V moves a little (ratiometric senders). The firmware uses the ±6.144 V
range, so a 5 V input is read without clipping.

### 3.2 12 V analog signals (battery, 12 V senders)

Use a **B25 voltage sensor module** (divides by 5: 25 V → 5 V) into an ADS1115
input, and set *Divider ratio* = 5 (preset *12 V battery via B25*).
Home-made divider: 47 kΩ from the 12 V wire to the input, 10 kΩ from the input to
GND (÷ 5.7), plus a 4.7 V zener from the input to GND for spikes.
Directly into an ESP32 analog pin: 100 kΩ / 22 kΩ (÷ 5.5) keeps 16 V under 3 V.

### 3.3 Alternator W terminal (RPM) and 12 V switches

PC817 opto-coupler board, 12 V input version:

| PC817 board | To |
|---|---|
| IN1+ / IN1− | alternator **W** terminal / chassis ground |
| IN2+ / IN2− | ignition 12 V / ground (ignition sense) |
| IN3, IN4 | warning lights, handbrake, any 12 V switch |
| VCC / GND (output side) | 3.3 V / GND |
| OUT1–OUT4 | ESP32 input pins (sensor type *RPM* or *Switch*, "on when LOW") |

RPM: set *pulses per revolution*, or start the engine and use *Calibrate*
(Sensors → the RPM sensor) with the idle speed from a tachometer. If RPM shows
nothing at idle, use the 5 V-input PC817 board with a 1 kΩ resistor in series.

### 3.4 Temperatures

- DS18B20 probes: red 5 V (or 3.3 V), black GND, yellow data — all probes on one
  bus, one 4.7 kΩ pull-up. Use **Sensors → Discover** to see each probe's
  address and assign it.
- EGT: MAX6675 SCK / CS / SO to three GPIOs (defaults 12 / 13 / 14 on S3); use
  thermocouple extension wire, not copper, from the probe.

## 4. Between the boards (through the firewall)

Data goes over ESP-NOW by radio. When a cable is preferred (CAN):

| RJ45 pin (T568B colour) | Signal |
|---|---|
| 1 / 2 (orange pair) | CAN H / CAN L (SN65HVD230 transceivers, 120 Ω at both ends) |
| 3 / 6 (green pair) | GND |
| others | spare |

Power goes on a **separate** 3-core cable (5 V, 3.3 V, GND), not through the RJ45.
Label both ends **NOT ETHERNET**.

## 5. Actuators

| Output | Module | Settings |
|---|---|---|
| Lights, horn, compressor | relay module or the ESP32 Relay X8 | type Relay; most modules: *active LOW* |
| Fan / pump speed | D4184 MOSFET module (or a PWM fan controller for big radiator fans) | type PWM, 100–1000 Hz (25 kHz for 4-wire PC fans), *Lowest running %*, kick-start 500 ms |
| Winch, linear actuator, flap | BTS7960 H-bridge: RPWM → *forward pin*, LPWM → *reverse pin*, R_EN + L_EN → 3.3 V | type Motor both ways |
| Valve, vent | hobby servo: signal → pin, power from 5 V (not the ESP32) | type Servo, angle range |

Inductive loads (relay coils, motors) need a flyback diode; ready-made modules have one.

### ESP32 Relay X8 (8 relays, 7–30 V)

Relays 1–8 = GPIO 32, 33, 25, 26, 27, 14, 12, 13; LED GPIO 23. Flash it once with
a **3.3 V** USB-to-serial adapter: TX→RX, RX→TX, GND, hold the GPIO0 button while
powering up, flash `carmetrics-relay-x8-full-flash.bin`. It starts as a relay
worker: pair it on the main board's Boards tab. Check on the bench whether a relay
clicks on HIGH (default) or LOW and tick *active LOW* if needed.

## 6. Status light colours

Green OK · amber warning · amber flashing sensor fault · red flashing critical
(steady red when muted) · blue flashing: a worker lost its main board ·
purple: safe mode. Both the built-in and an external RGB LED show it at once.
