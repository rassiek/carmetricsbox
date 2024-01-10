# carmetricsbox
This project uses  
1. 1 Wire Interface  (Digital Pin 7) => Four 1 Wire temp sensors (CoolantTemp, GearBox Temp, Transfer Case Temp and Intake Mainifold/ Air temp)
2. SPI Interface (Digital Pin 4,5,6) => One Thermocouple Amplifier with K-Type Thermocouple (EGT Temperature)
3. Analog Interface (Analog Pin 3 and 4) Two 5V 0-100psi Pressure Transducers (Boost Pressure 0-30psi  and Oil Pressure 0-100Psi 0.5 to 4.5V reading)
4. I2C Interface (Digital PIN 20 and 21) One OLED 128x128 Display (Metrics Display) + One BMP180 Barometric Pressure/Temperature/Altitude Sensor (Cabin Temp and Altitude Sensor) 
    To Add
6. Two B25 Voltage Sensor Detection Module (Starter Battery and Auxilliary Battery Sensors)

Other components 
1. 1 4.7 KOHM Resistor as a pull up between data/ signal Pin 7 and VCC.
2. 1 5V Power supply in the engine bay with a jumper cable to arduino's ground (Arduino is in the Cab)

All 1 Wire sensors, Pressure Transducers and Themocouple Amplifier get power from the secondary 5V power supply above in the engine bay.
All data/ signal pins terminate to a CAT6 wall outlet jack in the engine bay. 
The arduino powers the OLED and the BMP180 Barometric Pressure/Temperature/Altitude Sensor
All other signal Pins terminate on a second CAT6 Wall outlet jack attached to the box with Arduino. 
There is a cat6 cable connecting the two cat6 jacks in the cabin and the engine bay running through the firewall.
Arduino is powered indipendently using a 13.8 -9V buck converter. But it can also be powered using a normal 5v source.
The Arduino, Display and the Temp/Pressure sensors are in a small box that fits between the driver A Pilar and the Intrument Cluster. 

--------------------------------------------------------------------------------------------------
Notes 2024 Jan
Added integration to ESP32 via UART (Serial 2) -
ESP32 listents to serial 2 data coming from Serial2.print on Arduino and reads the last complete block of sensor and time data.
ESP32 converts that data to JSOn and exposes it on a /readings enpoint
This is then visualized on the root path / as gauges using Javascript, CSS and HTML
A second /charts page shows the trends of various metrics over time as long as the browser remains open (not stored anywhere in ESP32)


-----------------------------------------------------------------------------------------------------
Future Plans
Add a buttons page to enable buttons/ sliders and sen values to Arduino from ESP32 that are then used to trigger actions via a relay block and/or optocouplers.
Add a page on ESP32 to manage basic sensor configurations like allocate newly discovered one wire sensors or add an additional sensor to I2C, Analog Port etc. 




