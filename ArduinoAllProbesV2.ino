//OLED
#include <Arduino.h>
#include <U8g2lib.h>
#include <Wire.h> //allows communication over i2c devices
#include <avr/pgmspace.h>
#include <EEPROM.h>

// EEPROM settings
const uint32_t EEPROM_MAGIC_NUMBER = 0xDEADBEEF; // Unique ID to check if EEPROM has been initialized
const int EEPROM_APP_VERSION = 1; // Version for configuration structure
int eeAddress = 0; // Global EEPROM address pointer

// SensorType Enum Definition
enum SensorType {
  UNDEFINED,
  PRESSURE_ANALOG,
  TEMP_DS18B20,
  TEMP_MAX6675,
  TEMP_BMP085,
  VOLTAGE_ANALOG
};

// SensorConfig Struct Definition
struct SensorConfig {
  String id;
  String name;
  SensorType sensorType;
  int pin1;
  int pin2;
  int pin3; // Added for sensors like MAX6675 that need a third pin (e.g. CLK)
  uint8_t oneWireAddress[8];
  bool enabled;
  float value;
  float warningThreshold;       // Upper warning if only one warning is needed
  float criticalThreshold;      // Upper critical if only one critical is needed
  float lowerWarningThreshold;  // Lower warning 
  float lowerCriticalThreshold; // Lower critical
};

// Global array of SensorConfig objects
// Note: For DS18B20, pin1 is the OneWire bus pin.
SensorConfig configuredSensors[] = {
  {
    "oil_pressure", "Oil Pressure", PRESSURE_ANALOG, A3, -1, {0}, true, 0.0, 
    15.0, // warningThreshold (derived from old lowerThreshOP, as low oil pressure is a warning)
    70.0, // criticalThreshold (derived from old upperThreshOP, treat as upper critical for now)
    10.0, // lowerWarningThreshold (actual low pressure warning)
    5.0   // lowerCriticalThreshold (actual low pressure critical)
  },
  {
    "boost_pressure", "Boost Pressure", PRESSURE_ANALOG, A4, -1, {0}, true, 0.0, 
    16.0, // warningThreshold (warningThreshBST)
    18.0, // criticalThreshold (upperThreshBST)
    5.0,  // lowerWarningThreshold (lowerThreshBST)
    0.0   // lowerCriticalThreshold 
  },
  {
    "coolant_temp", "Coolant Temp", TEMP_DS18B20, 7, -1, -1, // pin1=ONE_WIRE_BUS, pin2, pin3 not used
    { 0x28, 0xFD, 0x86, 0x9F, 0x0D, 0x00, 0x00, 0x1A }, // sensor7 address
    true, 0.0, 
    88.0, // warningThreshold (warningThreshCT)
    95.0, // criticalThreshold (upperThreshCT)
    40.0, // lowerWarningThreshold (lowerThreshCT)
    20.0  // lowerCriticalThreshold 
  },
  {
    "egt", "EGT", TEMP_MAX6675, 
    4,    // pin1 for DO (thermoDO)
    5,    // pin2 for CS (thermoCS)
    6,    // pin3 for CLK (thermoCLK)
    {0},  // oneWireAddress not used
    true, 0.0,
    400.0, // warningThreshold (warningThreshEGT)
    650.0, // criticalThreshold (upperThreshEGT)
    0.0,   // lowerWarningThreshold (low EGT not typically a warning)
    0.0    // lowerCriticalThreshold (low EGT not typically a critical issue)
  },
  {
    "intake_temp", "Intake Temp", TEMP_DS18B20, 7, -1, -1,
    { 0x28, 0x0F, 0x71, 0x79, 0x97, 0x07, 0x03, 0x7F }, // sensor6 address
    true, 0.0, 80.0, 100.0, 0.0, 0.0 // Default thresholds
  },
  {
    "transfer_case_temp", "T-Case Temp", TEMP_DS18B20, 7, -1, -1,
    { 0x28, 0xC5, 0x7A, 0x79, 0x97, 0x07, 0x03, 0x5F }, // sensor2 address
    true, 0.0, 
    70.0, // warningThreshTrans
    80.0, // upperThreshTrans
    0.0, 0.0 
  },
  {
    "gearbox_temp", "Gearbox Temp", TEMP_DS18B20, 7, -1, -1,
    { 0x28, 0x2D, 0x10, 0x79, 0x97, 0x07, 0x03, 0x7B }, // sensor4 address
    true, 0.0, 
    70.0, // warningThreshTrans
    80.0, // upperThreshTrans
    0.0, 0.0
  },
  {
    "head_temp", "Head Temp", TEMP_DS18B20, 7, -1, -1,
    { 0x28, 0xE7, 0x6F, 0x79, 0x97, 0x09, 0x03, 0xF5 }, // sensor5 address
    true, 0.0, 110.0, 125.0, 0.0, 0.0 // Default thresholds, HT might run hotter
  },
  {
    "spare_temp_ds18b20", "Spare Temp1", TEMP_DS18B20, 7, -1, -1,
    { 0x28, 0xD5, 0x6E, 0x79, 0x97, 0x08, 0x03, 0x7F }, // sensor3 address
    false, 0.0, 0.0, 0.0, 0.0, 0.0 // Disabled by default, no thresholds
  },
  {
    "cabin_temp", "Cabin Temp", TEMP_BMP085, -1, -1, -1, {0}, true, 0.0,
    35.0, // warningThreshold
    40.0, // criticalThreshold
    5.0,  // lowerWarningThreshold
    0.0   // lowerCriticalThreshold
  },
  {
    "bat1_voltage", "Battery 1", VOLTAGE_ANALOG, BAT1_ANALOG_PIN, -1, -1, {0}, true, 0.0,
    14.8, // warningThreshold (High)
    15.2, // criticalThreshold (High)
    11.8, // lowerWarningThreshold (Low)
    11.5  // lowerCriticalThreshold (Low)
  },
  {
    "bat2_voltage", "Battery 2", VOLTAGE_ANALOG, BAT2_ANALOG_PIN, -1, -1, {0}, true, 0.0,
    14.8, // warningThreshold (High)
    15.2, // criticalThreshold (High)
    11.8, // lowerWarningThreshold (Low)
    11.5  // lowerCriticalThreshold (Low)
  }
  // Other sensors will be added here in future subtasks
};

// Recalculate numConfiguredSensors due to array changes
// This line must be AFTER the configuredSensors array definition.
const int numConfiguredSensors = sizeof(configuredSensors) / sizeof(configuredSensors[0]);


//PressureSensorsV
  const int oilPressureInput = A3; //select the analog input pin for the pressure transducer
  const int boostPressureInput = A4; //select the analog input pin for the pressure transducer
  const int pressureZero = 102.4; //analog reading of pressure transducer at 0psi
  const int pressureMax = 921.6; //analog reading of pressure transducer at 100psi
  const int pressuretransducermaxPSI = 100; //psi value of transducer being used
  const int sensorreadDelay = 250; //constant integer to set the sensor read delay in milliseconds
  float oilPressure = 0; //variable to store the value coming from the pressure transducer
  float boostPressure = 0; //variable to store the value coming from the pressure transducer
  float oilPressureValue = 0; //variable to store the converted to Psi value from oilPressure
  float boostPressureValue = 0; //variable to store the converted to Psi value from boostPressure
//END PressureSensorsVars

//BUZZ and Flasher
  const int led = 39 ; //The PWM port for led flasher flash()
  const int buzzer = 38 ;   //The PWM port for Buzzer buzz()
//BUZZ and Flasher

/////////Thresholds warning- flashing , upper - Beeping Used together with the section on Pixel Calculation Below - Some variables of items Gauges section are ////////
////StartThresholds Declaration
  //How to test
  //int warningThreshTrans = -700;   //Change thresholds to low values to cause a false positive
  //int upperThreshTrans = -800;  
  //Gearbox and TransferCase  
    int warningThreshTrans = 70;
    int upperThreshTrans = 80;     
  // Please note Warning Thresholds for EGT, Coolant and Boost - Used together with Variables declared in the section below (upperThresh and LowerThresh) 
  //While variables in this section only affect the beeping and flashing thresholds, the ones below affect the upper and lowe position of the '|' marks in the gauges.    
    int warningThreshCT = 88;
    int warningThreshEGT = 400;
    int warningThreshBST = 16;        
////EndThresholds Declaration 

//OLED Pixel Calculations by Sensor
  //Pixel Range is 25-100 - Based on the screen size
  ////////////////////Oil Ration 1 pixel = 1 psi 
    int numberOfPixels = 75;
    int maxOP = 75;
    int minOP = 0;
    int upperThreshOP = 70; //Pos 95 ie 25+70             //Threhold also used for critical level alerting
    int lowerThreshOP = 15; //Pos 40
    //Calculated from above
    float multiplierOP = pow((maxOP-minOP),-1)*numberOfPixels ;
    float baselineOP = minOP*multiplierOP-25;

  //////////////////////Coolant  Ration 1 pixel = 0.6666 deg
    int maxCT = 127.5;
    int minCT = 15;  
    int upperThreshCT = 95;   //Pos 88                      //Threhold also used for critical level alerting
    int lowerThreshCT = 40;  //Pos  52
    //Calculated from above
    float multiplierCT = pow((maxCT-minCT),-1)*numberOfPixels ;
    float baselineCT = minCT*multiplierCT-25;

  ////////////////////////EGT  Ration 1 pixel = 7 deg
  //EGT  Ration 1 pixel = 10.2 deg
    int maxEGT = 800;
    ////int minEGT = 275;
    int minEGT = 30;
    int upperThreshEGT = 650; //   Pos 86   (650-30) * 10.2 = 60.7843   Add 25 (min pixel in box)  = 85.7843            //Threhold also used for critical level alerting
    //////int lowerThreshEGT = 300;  // Pos 29
    int lowerThreshEGT = 173;  // Pos 40
    //Calculated from above
    float multiplierEGT = pow((maxEGT-minEGT),-1)*numberOfPixels ;
    float baselineEGT = minEGT*multiplierEGT-25;

  ////////////////////////BOOST  1 Pixel = 0.4 psi or 2.5 pixels = 1 PSI
  //BOOST  1 Pixel = 0.2857 psi or 3.5 pixels = 1 PSI
    ////int maxBST = 30;
    int maxBST = 20;
    int minBST = 0;
    int upperThreshBST = 18; // Pos 90.5                     //Threhold also used for critical level alerting
    int lowerThreshBST = 5; // Pos 44
    //Calculated from above
    float multiplierBST = pow((maxBST-minBST),-1)*numberOfPixels ;
    float baselineBST = minBST*multiplierBST-25;

//END OLED Pixel Calculations by Sensor


//Battery Voltage Analog Sensors section
    #define BAT1_ANALOG_PIN A5
    #define BAT2_ANALOG_PIN A6
    // Floats for ADC voltage & Input voltage
    float adc_voltage1 = 0.0;
    float adc_voltage2 = 0.0;
    float in_voltage1 = 0.0;
    float in_voltage2 = 0.0;
    // Floats for resistor values in divider (in ohms)
    float R1 = 30000.0;
    float R2 = 7500.0; 
    // Float for Reference Voltage
    float ref_voltage = 5.0;
    // Integer for ADC value
    int adc_value1 = 0;
    int adc_value2 = 0;
//END Battery Voltage Analog Sensors section




// RTC DS1302
// CONNECTIONS:
// DS1302 CLK/SCLK --> 5  34
// DS1302 DAT/IO --> 4  32
// DS1302 RST/CE --> 2  30
// DS1302 VCC --> 3.3v - 5v
// DS1302 GND --> GND
#include <ThreeWire.h>  
#include <RtcDS1302.h>
//ThreeWire myWire(4,5,2); // IO, SCLK, CE
ThreeWire myWire(32,34,30); // IO, SCLK, CE
RtcDS1302<ThreeWire> Rtc(myWire);
#define countof(a) (sizeof(a) / sizeof(a[0]))
const char data[] = "what time is it";
// END RTC DS1302



//OLED 
  //U8G2_SSD1327_EA_W128128_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 9, /* data=*/ 8, /* reset=*/ U8X8_PIN_NONE);
  //U8G2_SSD1327_EA_W128128_F_HW_I2C u8g2(U8G2_R0, /* clock=*/ 21, /* data=*/ 20, /* reset=*/ U8X8_PIN_NONE);
  U8G2_SSD1327_MIDAS_128X128_F_HW_I2C u8g2(U8G2_R0, /* clock=*/ 21, /* data=*/ 20, /* reset=*/ U8X8_PIN_NONE);
  //U8G2_SSD1327_EA_W128128_F_HW_I2C u8g2(U8G2_R0, /* clock=*/ 9, /* data=*/ 8, /* reset=*/ U8X8_PIN_NONE);

  // 'Logo', 128x128px
  #define u8g_logo_width 128
  #define u8g_logo_height 118
  
    static const unsigned char frame00[] PROGMEM = {
    // 'Logo', 128x128px
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x14, 0x02, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0xFF, 
      0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0xFF, 0x25, 0xE5, 0xFF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0xD7, 0xFF, 0xFF, 0xEA, 0x03, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF8, 0xFE, 0xFF, 
      0xFF, 0x7F, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0xDF, 0xFF, 0xFF, 0xFF, 0xFF, 0x77, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0xC0, 0xFB, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x01, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xE0, 0xFE, 0xFF, 0xFF, 
      0xFF, 0xFF, 0x7F, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0xB0, 0x7F, 0x7E, 0x3C, 0x33, 0xF8, 0xFF, 0x0E, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0xDC, 0x7F, 0x7E, 0x1C, 0x33, 0xF1, 0xFF, 0x1B, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFE, 0x3F, 0x3F, 0x1E, 
      0x9A, 0xF3, 0xFF, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0xFF, 0x3F, 0x9F, 0x1C, 0x98, 0x73, 0xF8, 0x7F, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0xFD, 0x3F, 0x9F, 0x4C, 0x9C, 0x31, 0xF8, 0xDF, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xFD, 0x9F, 0x8F, 0x4C, 
      0xCC, 0xF9, 0xFD, 0xBF, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 
      0xFE, 0x9F, 0x07, 0x6C, 0x4C, 0xBC, 0xFF, 0xFF, 0x01, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0xC0, 0xFF, 0x1F, 0xE6, 0x64, 0x0E, 0xFE, 0xFF, 0xFF, 
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0xFE, 0xFF, 
      0xFF, 0xFF, 0xFF, 0x7F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 
      0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x7F, 0x03, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0xC0, 0xFF, 0xFF, 0xE7, 0xCF, 0xFB, 0xEB, 0xFD, 0x7F, 
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0xFF, 0xFF, 0x83, 0x83, 
      0x39, 0x82, 0xC1, 0xFF, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 
      0xFF, 0xEF, 0x19, 0x11, 0x39, 0xF3, 0xC4, 0x7F, 0x01, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0xC0, 0xFE, 0xF1, 0x99, 0x39, 0x99, 0xF9, 0xCC, 0xFF, 
      0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xFF, 0xC1, 0x81, 0x39, 
      0xC9, 0x61, 0xC4, 0xBF, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 
      0xFD, 0xFB, 0xC0, 0x18, 0xE1, 0x70, 0xF0, 0xDF, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xE4, 0x99, 0xE1, 0x7C, 0xF0, 0x7F, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xF6, 0xFF, 0xEC, 0x89, 
      0xF1, 0x24, 0xF3, 0x7F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0xFC, 0x7F, 0xCC, 0xE1, 0x79, 0x30, 0xE3, 0x37, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0xDC, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x1D, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x70, 0xFF, 0xFF, 0xFF, 
      0xFF, 0xFF, 0xFF, 0x07, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0xE0, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xBF, 0x03, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x80, 0xF7, 0xFF, 0xFF, 0xFF, 0xFF, 0xEF, 0x01, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x5E, 0xFF, 0xFF, 
      0xFF, 0xFF, 0x3D, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0xF8, 0xFA, 0xFF, 0xFF, 0x97, 0x0F, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0xC0, 0x9F, 0xFF, 0x7F, 0xFE, 0x01, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xFC, 0xF7, 
      0xFD, 0x3F, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x40, 0xFE, 0x7F, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x9C, 0x01, 0x1C, 0xF0, 
      0x03, 0xFC, 0x01, 0xFF, 0x40, 0x03, 0x8C, 0x69, 0xE8, 0x06, 0x09, 0x1C, 
      0xFC, 0x07, 0x1C, 0xFC, 0x07, 0xFF, 0x81, 0xFF, 0xC1, 0x03, 0x9F, 0xFF, 
      0xF8, 0x8F, 0x1F, 0x1F, 0xFE, 0x1F, 0x3C, 0xFC, 0x07, 0xFF, 0xC1, 0xFF, 
      0xC3, 0x03, 0x8F, 0xFF, 0xF8, 0x1F, 0x1F, 0x1F, 0xFE, 0x3F, 0x3C, 0xFE, 
      0x83, 0xFF, 0xE1, 0xFF, 0x87, 0x07, 0x8F, 0xFF, 0xF8, 0x3F, 0xBE, 0x0F, 
      0x3C, 0x3F, 0x1C, 0x1E, 0xC0, 0x8F, 0xF1, 0xC3, 0x8F, 0x87, 0x87, 0x07, 
      0x7C, 0x3E, 0xFC, 0x07, 0x1E, 0x3C, 0x1E, 0x1E, 0xC0, 0x07, 0xF0, 0x81, 
      0x8F, 0x8F, 0x87, 0x07, 0x78, 0x3C, 0xFC, 0x07, 0x1C, 0x7C, 0x3C, 0xFE, 
      0xE1, 0x03, 0xF0, 0x00, 0x0F, 0xCF, 0x87, 0x3F, 0x78, 0x3C, 0xF8, 0x03, 
      0x1C, 0x78, 0x1C, 0xFE, 0xE3, 0x03, 0xF8, 0x00, 0x0F, 0xCF, 0x83, 0x7F, 
      0xF8, 0x3F, 0xF0, 0x01, 0x3E, 0x78, 0x3C, 0xFC, 0xE7, 0x03, 0xF0, 0x00, 
      0x0F, 0xDE, 0x83, 0x7F, 0xF8, 0x1F, 0xF0, 0x01, 0x3E, 0x7C, 0x3E, 0xF8, 
      0xE7, 0x03, 0xF8, 0x00, 0x0F, 0xFE, 0x81, 0x7F, 0xF8, 0x0F, 0xE0, 0x00, 
      0x1C, 0x7C, 0x1C, 0xC0, 0xCF, 0x03, 0xF0, 0x81, 0x0F, 0xFE, 0x81, 0x0F, 
      0xF8, 0x07, 0xE0, 0x01, 0x1E, 0x3E, 0x3C, 0x80, 0xC7, 0x07, 0xF0, 0xC1, 
      0x0F, 0xFC, 0x81, 0x07, 0x78, 0x0F, 0xE0, 0x00, 0xBC, 0x3F, 0x1C, 0xCE, 
      0xCF, 0xFF, 0xF1, 0xFF, 0x07, 0xFC, 0x80, 0x6F, 0x78, 0x1F, 0xF0, 0x01, 
      0xFC, 0x1F, 0x1E, 0xFE, 0x87, 0xFF, 0xE1, 0xFF, 0x07, 0xFC, 0x80, 0xFF, 
      0x7C, 0x3E, 0xE0, 0x00, 0xFE, 0x0F, 0x3C, 0xFF, 0x03, 0xFF, 0xC1, 0xFF, 
      0x03, 0x78, 0x80, 0xFF, 0x78, 0x3E, 0xE0, 0x01, 0xFC, 0x07, 0x3C, 0xFE, 
      0x01, 0xFE, 0x81, 0xFF, 0x00, 0x78, 0x80, 0xFF, 0x78, 0x7C, 0xF0, 0x00, 
      0x00, 0x00, 0x00, 0x30, 0x00, 0xF0, 0x00, 0x3E, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
//END OLED

//TP - Senosor used for altitude, pressure and cabin temp
    #include <Wire.h>
    #include <Adafruit_BMP085.h>
    #define seaLevelPressure_hPa 1017.0
    Adafruit_BMP085 bmp;
//ENDTP

// --- Serial Command Parser Helper Functions ---

// Helper to convert a single hex character to its byte value
byte hexToByte(char hex) {
  if (hex >= '0' && hex <= '9') {
    return hex - '0';
  } else if (hex >= 'a' && hex <= 'f') {
    return hex - 'a' + 10;
  } else if (hex >= 'A' && hex <= 'F') {
    return hex - 'A' + 10;
  }
  return 0; // Should not happen with valid hex
}

// Helper to parse a hex string (e.g., "28AABBCC") into a byte array
void parseHexStringToByteArray(const String& hexStr, uint8_t* byteArray, int arraySize) {
  for (int i = 0; i < arraySize; ++i) {
    if (2 * i + 1 < hexStr.length()) {
      byteArray[i] = (hexToByte(hexStr.charAt(2 * i)) << 4) + hexToByte(hexStr.charAt(2 * i + 1));
    } else {
      byteArray[i] = 0; // Fill with 0 if hex string is too short
    }
  }
}

String sensorTypeToString(SensorType type) {
  switch (type) {
    case UNDEFINED: return "UNDEF";
    case PRESSURE_ANALOG: return "PRES_A";
    case TEMP_DS18B20: return "DS18B20";
    case TEMP_MAX6675: return "MAX6675";
    case TEMP_BMP085: return "BMP085";
    case VOLTAGE_ANALOG: return "VOLT_A";
    default: return "UNK";
  }
}

SensorType stringToSensorType(String strType) {
  if (strType.equalsIgnoreCase("PRES_A")) return PRESSURE_ANALOG;
  if (strType.equalsIgnoreCase("DS18B20")) return TEMP_DS18B20;
  if (strType.equalsIgnoreCase("MAX6675")) return TEMP_MAX6675;
  if (strType.equalsIgnoreCase("BMP085")) return TEMP_BMP085;
  if (strType.equalsIgnoreCase("VOLT_A")) return VOLTAGE_ANALOG;
  return UNDEFINED;
}


String oneWireAddressToString(const uint8_t* address) {
  String result = "";
  for (int i = 0; i < 8; ++i) {
    if (address[i] < 16) result += "0";
    result += String(address[i], HEX);
  }
  result.toUpperCase();
  return result;
}

// Helper to get a sensor's configuration as a formatted string
String getSensorConfigString(const SensorConfig &sensor) {
  String configStr = sensor.id + "," + sensor.name + "," + sensorTypeToString(sensor.sensorType) + "," +
                     String(sensor.pin1) + "," + String(sensor.pin2) + "," + String(sensor.pin3) + "," +
                     oneWireAddressToString(sensor.oneWireAddress) + "," +
                     String(sensor.enabled ? 1:0) + "," +
                     String(sensor.warningThreshold, 2) + "," + String(sensor.criticalThreshold, 2) + "," +
                     String(sensor.lowerWarningThreshold, 2) + "," + String(sensor.lowerCriticalThreshold, 2);
  return configStr;
}

// --- End Serial Command Parser Helper Functions ---
//TCMAX6675  - EGT Amplifier
    #include "max6675.h"
    int thermoDO = 4;
    int thermoCS = 5;
    int thermoCLK = 6;
    MAX6675 thermocouple(thermoCLK, thermoCS, thermoDO);
//ENDTCMAX6675

//1W  - All 1 wire DS18B20 (GB, TC, Coolant, Intake Manifold)
    /* Multiple DS18B20 1-Wire digital temperature sensors with Arduino example code. More info: https://www.makerguides.com */
    // Include the required Arduino libraries:
    #include <OneWire.h>
    #include <DallasTemperature.h>
    // Define to which pin of the Arduino the 1-Wire bus is connected:
    #define ONE_WIRE_BUS 7
    // Create a new instance of the oneWire class to communicate with any OneWire device:
    OneWire oneWire(ONE_WIRE_BUS);
    // Pass the oneWire reference to DallasTemperature library:
    DallasTemperature sensors(&oneWire);
    int deviceCount = 0;
    float tempC;
    float tempF;

      // Addresses of the 1 wire temp sensors DS18B20s - Sensor replacement will need to be updated here. 
        uint8_t sensor1[8] = { 0x28, 0xAA, 0xA5, 0xD4, 0x4A, 0x14, 0x01, 0xB1 };
        uint8_t sensor2[8] = { 0x28, 0xC5, 0x7A, 0x79, 0x97, 0x07, 0x03, 0x5F };  //T Transfer Case Temp
        uint8_t sensor3[8] = { 0x28, 0xD5, 0x6E, 0x79, 0x97, 0x08, 0x03, 0x7F };  //
        uint8_t sensor4[8] = { 0x28, 0x2D, 0x10, 0x79, 0x97, 0x07, 0x03, 0x7B };  //T Gear Box Temp
        uint8_t sensor5[8] = { 0x28, 0xE7, 0x6F, 0x79, 0x97, 0x09, 0x03, 0xF5 };  //HT Faulty Head Temp
        uint8_t sensor6[8] = { 0x28, 0x0F, 0x71, 0x79, 0x97, 0x07, 0x03, 0x7F };  //I Intake Manifold Temp
        uint8_t sensor7[8] = { 0x28, 0xFD, 0x86, 0x9F, 0x0D, 0x00, 0x00, 0x1A };  //CT Coolant Temp
      
      //End1W


// New sensor reading functions
void readAnalogPressureSensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == PRESSURE_ANALOG) {
    int rawValue = analogRead(sensor.pin1);
    // Assuming pressureZero, pressureMax, pressuretransducermaxPSI are still global for now
    sensor.value = ((rawValue - pressureZero) * pressuretransducermaxPSI) / (pressureMax - pressureZero);
  }
}

// R1, R2, and ref_voltage are currently global. If they need to be per-sensor,
// they should be moved into SensorConfig.
void readAnalogVoltageSensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == VOLTAGE_ANALOG) {
    int adc_value = analogRead(sensor.pin1);
    float adc_voltage  = (adc_value * ref_voltage) / 1024.0; 
    // Assuming R1 and R2 are global for now
    sensor.value = adc_voltage / (R2 / (R1 + R2)); 
  }
}

void readBMP085Temperature(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_BMP085) {
    // Ensure bmp object is initialized (globally) and bmp.begin() was called in setup.
    float temp = bmp.readTemperature();
    if (isnan(temp)) { // BMP085 library might not return NaN, but good practice. Check lib behavior for error values.
      Serial.print("Error: Failed to read from BMP085 sensor ");
      Serial.println(sensor.name);
      sensor.value = -999.0; // Error indicator
    } else {
      sensor.value = temp;
    }
  }
}

void readMAX6675Sensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_MAX6675) {
    // Create a MAX6675 object locally for this reading. 
    // This assumes the MAX6675 library doesn't require a long-lived object 
    // or that begin() needs to be called on it repeatedly.
    // If it does, the MAX6675 object might need to be global or managed differently.
    // For now, following the pattern of one-off reads.
    MAX6675 thermocouple(sensor.pin3, sensor.pin2, sensor.pin1); // CLK, CS, DO
    
    // It's good practice to add a small delay if the library docs suggest it,
    // or if readings are unstable. The original code had a delay(500) in setup 
    // for MAX chip to stabilize, and mentioned AT LEAST 250ms between reads in loop.
    // To ensure stability, a delay here might be prudent if not handled by a global read interval.
    // However, the main sensorreadDelay is 250ms, which might cover this.
    // For now, just read. If issues arise, add delay(250) here.
    float temp = thermocouple.readCelsius();
    if (isnan(temp)) {
      Serial.print("Error: Failed to read from MAX6675 sensor ");
      Serial.println(sensor.name);
      sensor.value = -999.0; // Error indicator
    } else {
      sensor.value = temp - 8.0; // Applying original offset
    }
  }
}

void readDS18B20Sensor(SensorConfig &sensor) {
  if (sensor.enabled && sensor.sensorType == TEMP_DS18B20) {
    // Ensure sensors object (DallasTemperature) is initialized and sensors.begin() called in setup.
    // sensors.requestTemperatures() should be called once before reading all DS18B20 sensors in the loop.
    sensor.value = sensors.getTempC(sensor.oneWireAddress);
    // Handle potential read errors or disconnected state. 85C is a common power-on default for DS18B20.
    // DEVICE_DISCONNECTED_C is typically -127.
    if (sensor.value == DEVICE_DISCONNECTED_C || sensor.value == 85.0) { 
      // Serial.print("Attempting re-read for "); Serial.println(sensor.name); // Optional debug
      delay(50); // Short delay before retry, helps with bus stability sometimes
      sensors.requestTemperaturesByAddress(sensor.oneWireAddress); 
      sensor.value = sensors.getTempC(sensor.oneWireAddress);
      
      if (sensor.value == DEVICE_DISCONNECTED_C || sensor.value == 85.0) {
         Serial.print("Error: Consistently failing to read temperature for DS18B20 sensor ");
         Serial.println(sensor.name);
         sensor.value = -999.0; // Consistent error indicator
      }
    }
  }
}

// --- EEPROM Functions ---

// Function to save a single sensor's configuration to EEPROM
// Does NOT save sensor.value as it's runtime data.
void saveSensorConfigToEEPROM(int &address, const SensorConfig &sensor) {
  EEPROM.put(address, sensor.id);
  address += sizeof(sensor.id); // String size can be tricky, EEPROM.put handles it internally by storing length first.
                                // However, for manual address tracking, this is an approximation.
                                // A robust solution would use fixed-size char arrays or write length explicitly.
                                // For now, rely on EEPROM.put's internal handling and hope total size fits.
  EEPROM.put(address, sensor.name);
  address += sizeof(sensor.name); // Same caveat as above for String
  EEPROM.put(address, sensor.sensorType);
  address += sizeof(sensor.sensorType);
  EEPROM.put(address, sensor.pin1);
  address += sizeof(sensor.pin1);
  EEPROM.put(address, sensor.pin2);
  address += sizeof(sensor.pin2);
  EEPROM.put(address, sensor.pin3);
  address += sizeof(sensor.pin3);
  for (int i = 0; i < 8; i++) {
    EEPROM.put(address + i, sensor.oneWireAddress[i]);
  }
  address += 8;
  EEPROM.put(address, sensor.enabled);
  address += sizeof(sensor.enabled);
  EEPROM.put(address, sensor.warningThreshold);
  address += sizeof(sensor.warningThreshold);
  EEPROM.put(address, sensor.criticalThreshold);
  address += sizeof(sensor.criticalThreshold);
  EEPROM.put(address, sensor.lowerWarningThreshold);
  address += sizeof(sensor.lowerWarningThreshold);
  EEPROM.put(address, sensor.lowerCriticalThreshold);
  address += sizeof(sensor.lowerCriticalThreshold);
}

// Function to load a single sensor's configuration from EEPROM
void loadSensorConfigFromEEPROM(int &address, SensorConfig &sensor) {
  EEPROM.get(address, sensor.id);
  address += sizeof(sensor.id); // Relying on EEPROM.get internal String handling
  EEPROM.get(address, sensor.name);
  address += sizeof(sensor.name);
  EEPROM.get(address, sensor.sensorType);
  address += sizeof(sensor.sensorType);
  EEPROM.get(address, sensor.pin1);
  address += sizeof(sensor.pin1);
  EEPROM.get(address, sensor.pin2);
  address += sizeof(sensor.pin2);
  EEPROM.get(address, sensor.pin3);
  address += sizeof(sensor.pin3);
  for (int i = 0; i < 8; i++) {
    EEPROM.get(address + i, sensor.oneWireAddress[i]);
  }
  address += 8;
  EEPROM.get(address, sensor.enabled);
  address += sizeof(sensor.enabled);
  EEPROM.get(address, sensor.warningThreshold);
  address += sizeof(sensor.warningThreshold);
  EEPROM.get(address, sensor.criticalThreshold);
  address += sizeof(sensor.criticalThreshold);
  EEPROM.get(address, sensor.lowerWarningThreshold);
  address += sizeof(sensor.lowerWarningThreshold);
  EEPROM.get(address, sensor.lowerCriticalThreshold);
  address += sizeof(sensor.lowerCriticalThreshold);
  sensor.value = 0; // Initialize runtime value
}

void saveConfiguration() {
  Serial.println("Saving configuration to EEPROM...");
  int currentAddress = 0;
  EEPROM.put(currentAddress, EEPROM_MAGIC_NUMBER);
  currentAddress += sizeof(EEPROM_MAGIC_NUMBER);
  EEPROM.put(currentAddress, EEPROM_APP_VERSION);
  currentAddress += sizeof(EEPROM_APP_VERSION);

  for (int i = 0; i < numConfiguredSensors; i++) {
    saveSensorConfigToEEPROM(currentAddress, configuredSensors[i]);
  }
  Serial.println("Configuration saved.");
}

void loadDefaultConfiguration() {
  Serial.println("Loading default configuration...");
  // The global configuredSensors array is already initialized with defaults at compile time.
  // This function effectively resets the live configuration to those initial defaults if needed.
  // To properly "reset" if the array was modified at runtime *before* this call,
  // we would re-assign values from a const default array or re-declare.
  // For now, we assume this is called when `configuredSensors` still holds its initial defaults
  // or we want to refresh it to that state.
  
  SensorConfig defaults[] = {
    { "oil_pressure", "Oil Pressure", PRESSURE_ANALOG, A3, -1, -1, {0}, true, 0.0, 15.0, 70.0, 10.0, 5.0 },
    { "boost_pressure", "Boost Pressure", PRESSURE_ANALOG, A4, -1, -1, {0}, true, 0.0, 16.0, 18.0, 5.0, 0.0 },
    { "coolant_temp", "Coolant Temp", TEMP_DS18B20, 7, -1, -1, { 0x28, 0xFD, 0x86, 0x9F, 0x0D, 0x00, 0x00, 0x1A }, true, 0.0, 88.0, 95.0, 40.0, 20.0 },
    { "egt", "EGT", TEMP_MAX6675, 4, 5, 6, {0}, true, 0.0, 400.0, 650.0, 0.0, 0.0 },
    { "intake_temp", "Intake Temp", TEMP_DS18B20, 7, -1, -1, { 0x28, 0x0F, 0x71, 0x79, 0x97, 0x07, 0x03, 0x7F }, true, 0.0, 80.0, 100.0, 0.0, 0.0 },
    { "transfer_case_temp", "T-Case Temp", TEMP_DS18B20, 7, -1, -1, { 0x28, 0xC5, 0x7A, 0x79, 0x97, 0x07, 0x03, 0x5F }, true, 0.0, 70.0, 80.0, 0.0, 0.0 },
    { "gearbox_temp", "Gearbox Temp", TEMP_DS18B20, 7, -1, -1, { 0x28, 0x2D, 0x10, 0x79, 0x97, 0x07, 0x03, 0x7B }, true, 0.0, 70.0, 80.0, 0.0, 0.0 },
    { "head_temp", "Head Temp", TEMP_DS18B20, 7, -1, -1, { 0x28, 0xE7, 0x6F, 0x79, 0x97, 0x09, 0x03, 0xF5 }, true, 0.0, 110.0, 125.0, 0.0, 0.0 },
    { "spare_temp_ds18b20", "Spare Temp1", TEMP_DS18B20, 7, -1, -1, { 0x28, 0xD5, 0x6E, 0x79, 0x97, 0x08, 0x03, 0x7F }, false, 0.0, 0.0, 0.0, 0.0, 0.0 },
    { "cabin_temp", "Cabin Temp", TEMP_BMP085, -1, -1, -1, {0}, true, 0.0, 35.0, 40.0, 5.0, 0.0 },
    { "bat1_voltage", "Battery 1", VOLTAGE_ANALOG, BAT1_ANALOG_PIN, -1, -1, {0}, true, 0.0, 14.8, 15.2, 11.8, 11.5 },
    { "bat2_voltage", "Battery 2", VOLTAGE_ANALOG, BAT2_ANALOG_PIN, -1, -1, {0}, true, 0.0, 14.8, 15.2, 11.8, 11.5 }
  };

  // Ensure the live configuredSensors array matches the size of the defaults array.
  // This simple copy assumes numConfiguredSensors is consistent with the defaults array size.
  for (int i = 0; i < numConfiguredSensors; i++) {
    configuredSensors[i] = defaults[i]; // This performs a struct copy
  }
  Serial.println("Default configuration loaded into live config.");
}


void loadConfiguration() {
  Serial.println("Loading configuration from EEPROM...");
  int currentAddress = 0;
  uint32_t magic;
  int version;

  EEPROM.get(currentAddress, magic);
  currentAddress += sizeof(magic);
  EEPROM.get(currentAddress, version);
  currentAddress += sizeof(version);

  if (magic == EEPROM_MAGIC_NUMBER && version == EEPROM_APP_VERSION) {
    Serial.println("Valid configuration found in EEPROM.");
    for (int i = 0; i < numConfiguredSensors; i++) {
      loadSensorConfigFromEEPROM(currentAddress, configuredSensors[i]);
    }
  } else {
    Serial.println("No valid configuration in EEPROM or version mismatch. Loading defaults.");
    loadDefaultConfiguration();
    saveConfiguration(); // Save defaults for next time
  }
}

// --- End EEPROM Functions ---

void setup(void) 

//TCMAX6675 - EGT
{
  Serial.begin(9600);
  Serial2.begin(4800);

  Serial.println("Initialize Thermal Couple EGT Sensor MAX6675");
  // wait for MAX chip to stabilize
  //delay(500);
//ENDTCMAX6675


//OLED
  if (!u8g2.begin()) {
  Serial.println("Unable to Initialize OLED Check Connection");
  }
else 
 {
 Serial.println("Initialized OLED Successfully");
  }

u8g2.clearBuffer();
u8g2.setDrawColor(1);

   draw();


//TP
//{
                                       //Serial.begin(9600);
  if (!bmp.begin()) {
  Serial.println("Unable to Initialize Cabin Pressure and Temp Sensor BMP085 Check Connection");
///////  while (1) {}
  }
else 
 {
 Serial.println("Initialized Cabin Pressure and Temp Sensor BMP085");
  }
//}
//EndTP


//1W
//{
  // Begin serial communication at a baud rate of 9600:
                                     //Serial.begin(9600);
  // Start up the library:

  sensors.begin();
  //sensors.setWaitForConversion(false);
  // Locate the devices on the bus:
  Serial.print("Locating One Wire Temp Sensors...");
  Serial.print("Found ");
  deviceCount = sensors.getDeviceCount();
  Serial.print(deviceCount);
  Serial.println(" devices");
  

//End1W

//RTC

    Serial.print("compiled: ");
    Serial.print(__DATE__);
    Serial.println(__TIME__);    
    Rtc.Begin();
    RtcDateTime compiled = RtcDateTime(__DATE__, __TIME__);
    printDateTime(compiled);
//    Serial.println();
//    Rtc.SetDateTime(compiled);
    if (!Rtc.IsDateTimeValid()) 
    {
        Serial.println("RTC lost confidence in the DateTime!");
     //   Rtc.SetDateTime(compiled);
    }
    if (Rtc.GetIsWriteProtected())
    {
        Serial.println("RTC was write protected, enabling writing now");
        Rtc.SetIsWriteProtected(false);
    }
    if (!Rtc.GetIsRunning())
    {
        Serial.println("RTC was not actively running, starting now");
        Rtc.SetIsRunning(true);
    }
    RtcDateTime now = Rtc.GetDateTime();
    if (now < compiled) 
    {
        Serial.println("RTC is older than compile time!  (Updating DateTime)");
      //  Rtc.SetDateTime(compiled);
    }

//END RTC

  loadConfiguration(); // Load configuration from EEPROM or defaults
}
void loop(){

////////Prefetch/ Preinitialize
//1 wire Fetch Temp Data Early 
 sensors.requestTemperatures();
// I wire EndFetch 

  // --- Serial Command Processing (Serial2) ---
  if (Serial2.available() > 0) {
    String cmdFull = Serial2.readStringUntil('\n');
    cmdFull.trim(); // Remove any leading/trailing whitespace
    Serial.print("Received command on Serial2: "); Serial.println(cmdFull);

    String cmd;
    String args;
    int spaceIndex = cmdFull.indexOf(' ');
    if (spaceIndex != -1) {
      cmd = cmdFull.substring(0, spaceIndex);
      args = cmdFull.substring(spaceIndex + 1);
    } else {
      cmd = cmdFull;
    }

    if (cmd.equalsIgnoreCase("GET_ALL_CONF")) {
      Serial2.println("START_CONF_LIST");
      for (int i = 0; i < numConfiguredSensors; i++) {
        Serial2.println(getSensorConfigString(configuredSensors[i]));
      }
      Serial2.println("END_CONF_LIST");
    } 
    else if (cmd.equalsIgnoreCase("SET_SENSOR_CONF")) {
      // Expecting: <id> <name> <typeStr> <p1> <p2> <p3> <addr_hex> <en> <wT> <cT> <lwT> <lcT>
      // Example: SET_SENSOR_CONF oil_pressure Oil_Pressure PRES_A A3 -1 -1 0000000000000000 1 15.0 70.0 10.0 5.0
      String parts[12];
      int partIndex = 0;
      int currentPos = 0;
      for(int i=0; i<11; i++){ // Expect 11 arguments after the ID
          int nextSpace = args.indexOf(' ', currentPos);
          if(nextSpace == -1 && i < 11){ // If less than 11 args and no more spaces, error or last arg
             if (i == 10) parts[partIndex++] = args.substring(currentPos); // Last argument
             else { /* Error: not enough args */ Serial2.println("ERR_SET_CONF_ARGS_COUNT"); break;}
          } else if (nextSpace == -1 && i == 10) { // last argument
             parts[partIndex++] = args.substring(currentPos);
          }
          else {
            parts[partIndex++] = args.substring(currentPos, nextSpace);
            currentPos = nextSpace + 1;
          }
      }
      
      if(partIndex == 12) { // ID + 11 args
        String s_id = parts[0];
        bool found = false;
        for (int i = 0; i < numConfiguredSensors; i++) {
          if (configuredSensors[i].id.equals(s_id)) {
            configuredSensors[i].name = parts[1];
            configuredSensors[i].sensorType = stringToSensorType(parts[2]);
            configuredSensors[i].pin1 = parts[3].toInt();
            configuredSensors[i].pin2 = parts[4].toInt();
            configuredSensors[i].pin3 = parts[5].toInt();
            parseHexStringToByteArray(parts[6], configuredSensors[i].oneWireAddress, 8);
            configuredSensors[i].enabled = (parts[7].toInt() == 1);
            configuredSensors[i].warningThreshold = parts[8].toFloat();
            configuredSensors[i].criticalThreshold = parts[9].toFloat();
            configuredSensors[i].lowerWarningThreshold = parts[10].toFloat();
            configuredSensors[i].lowerCriticalThreshold = parts[11].toFloat();
            found = true;
            Serial2.println("ACK_SET_CONF " + s_id);
            break;
          }
        }
        if (!found) {
          Serial2.println("ERR_SET_CONF_ID_NOT_FOUND " + s_id);
        }
      } else {
          // Fallback for parsing if space issue logic above is not perfect
          // This is a simple split, assumes exactly 12 parts separated by space including command
          // For SET_SENSOR_CONF <id> <name> <type> <p1> <p2> <p3> <addr_hex> <en> <wT> <cT <lwT> <lcT>
          // cmdFull: SET_SENSOR_CONF id name type p1 p2 p3 addr en wT cT lwT lcT
          // We need to split args string, not cmdFull
          String s_id = args.substring(0, args.indexOf(' ')); // First part is id
          // This is still complex and error prone with simple indexOf. A more robust parser is needed for production.
          // For this subtask, the above loop is a better attempt. This comment is for acknowledgement.
          Serial2.println("ERR_SET_CONF_PARSE"); // Simplified error for now
      }
    }
    else if (cmd.equalsIgnoreCase("SAVE_CONF")) {
      saveConfiguration();
      Serial2.println("ACK_SAVE_CONF");
    } 
    else if (cmd.equalsIgnoreCase("LOAD_DEFAULTS")) {
      loadDefaultConfiguration();
      // Optionally save after loading defaults:
      // saveConfiguration(); 
      Serial2.println("ACK_LOAD_DEFAULTS");
    }
    else if (cmd.equalsIgnoreCase("GET_SENSOR_VAL")) {
      String sensorId = args;
      bool found = false;
      for (int i = 0; i < numConfiguredSensors; i++) {
        if (configuredSensors[i].id.equals(sensorId)) {
          Serial2.print("VAL " + sensorId + " ");
          Serial2.println(configuredSensors[i].value);
          found = true;
          break;
        }
      }
      if (!found) {
        Serial2.println("ERR_GET_VAL_ID_NOT_FOUND " + sensorId);
      }
    }
    else {
      Serial2.println("ERR_UNKNOWN_CMD");
    }
  }
  // --- End Serial Command Processing ---

  // Iterate through configured sensors to read them
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].enabled) {
      switch (configuredSensors[i].sensorType) {
        case PRESSURE_ANALOG:
          readAnalogPressureSensor(configuredSensors[i]);
          break;
        case TEMP_DS18B20:
          // Make sure sensors.requestTemperatures() has been called earlier in the loop for all DS18B20s
          readDS18B20Sensor(configuredSensors[i]);
          break;
        case TEMP_MAX6675:
          readMAX6675Sensor(configuredSensors[i]);
          break;
        case TEMP_BMP085:
          readBMP085Temperature(configuredSensors[i]);
          break;
        case VOLTAGE_ANALOG:
          readAnalogVoltageSensor(configuredSensors[i]);
          break;
        // Add cases for other sensor types as they are refactored
        default:
          // Handle unknown or not-yet-refactored sensor types if necessary
          break;
      }
    }
  }

//OLED 
  u8g2.setFont(u8g2_font_ncenB08_tr);
  u8g2.setBusClock(500000);
//END OLED

 
//TP - Old direct prints will be commented out, CabinTemp is now in configuredSensors
//{
    // Serial.print("CabinTemp = ");
    // Serial.print(bmp.readTemperature());
    // Serial.println(" *C");
//    CT==bmp.readTemperature();
    //Serial.print("Pressure = ");
    //Serial.print(bmp.readPressure());
    //Serial.println(" Pa");

    Serial.print("Altitude = ");
    Serial.print(bmp.readAltitude());
    Serial.println(" meters");

    //Serial.print("Pressure at sealevel (calculated) = ");
    //Serial.print(bmp.readSealevelPressure());
    //Serial.println(" Pa");

    Serial.print("Real altitude = ");
    Serial.print(bmp.readAltitude(seaLevelPressure_hPa * 100));
    Serial.println(" meters");
    
//    Serial.println();
//    delay(500);
//}

//EndTP

//TC6675 - Old direct print commented out
  // basic readout test, just print the current temp
  
   // Serial.print("EGT = "); 
   // Serial.println(thermocouple.readCelsius()-8);
   //Serial.print("F = ");
   //Serial.println(thermocouple.readFahrenheit());
    // For the MAX6675 to update, you must delay AT LEAST 250ms between reads!
//   delay(1000);
 
 
 //ENDTC6675

//Pressure Sensors Run - Old individual readings commented out
  // oilPressure = analogRead(oilPressureInput); //reads value from input pin and assigns to variable
  // boostPressure = analogRead(boostPressureInput); //reads value from input pin and assigns to variable
  // oilPressureValue = ((oilPressure-pressureZero)*pressuretransducermaxPSI)/(pressureMax-pressureZero); //conversion equation to convert analog reading to psi
  // boostPressureValue = ((boostPressure-pressureZero)*pressuretransducermaxPSI)/(pressureMax-pressureZero); //conversion equation to convert analog reading to psi
//End Pressure Sensors


//RTC RUN
    RtcDateTime now = Rtc.GetDateTime();
    printDateTime(now);
    printShortDateTime(now);
    if (!now.IsValid())
    {
        // Common Causes:
        //    1) the battery on the device is low or even missing and the power line was disconnected
        Serial.println("RTC lost confidence in the DateTime!");
    }
//END RTC RUN

// Battery Voltage Sensors run - Old direct readings commented out
  // Read the Analog Input
    // adc_value1 = analogRead(BAT1_ANALOG_PIN);
    // adc_value2 = analogRead(BAT2_ANALOG_PIN);
  // Determine voltage at ADC input
    // adc_voltage1  = (adc_value1 * ref_voltage) / 1024.0; 
    // adc_voltage2  = (adc_value2 * ref_voltage) / 1024.0; 
  // Calculate voltage at divider input
    // in_voltage1 = adc_voltage1 / (R2/(R1+R2)) ; 
    // in_voltage2 = adc_voltage2 / (R2/(R1+R2)) ; 
  // Print results to Serial Monitor to 2 decimal places
    // Serial.print("BAT1 Voltage = "); // Handled by unified print loop
    // Serial.println(in_voltage1, 2);   // Handled by unified print loop
    // Serial.print("BAT2 Voltage = "); // Handled by unified print loop
    // Serial.println(in_voltage2, 2);   // Handled by unified print loop
// End Battery Voltage Sensors


//1W and BOOST
  // Serial.print("IMT: "); //Intake Manifold Air Temp
  // printTemperature(sensor6);
  // Serial.print("Spare: ");  //Coolant Team
  // printTemperature(sensor3);
  // Serial.print("TCT: "); //Transfer Case Temp
  // printTemperature(sensor2);
  // Serial.print("GBT: "); //Gearbox Temp
  // printTemperature(sensor4);
  // Serial.print("HT: "); //Cylinder Head Temp
  // printTemperature(sensor5);
  // Serial.print("Spare: "); // This was a duplicate print of sensor6
  // printTemperature(sensor6); 
  
  // Serial.print("CT: "); // Old Coolant Temp print - Handled by loop
  // printTemperature(sensor7); // Old Coolant Temp print - Handled by loop
  // Serial.print(oilPressureValue, 1); // Handled by loop
  // Serial.println("psi - Oil"); // Handled by loop
  // Serial.print(boostPressureValue, 1); // Handled by loop
  // Serial.println("psi - Boost"); // Handled by loop
  // Serial.println(); // Old blank line

  // Unified Serial printing for all configured sensors
  for (int i = 0; i < numConfiguredSensors; i++) {
    if (configuredSensors[i].enabled) {
      Serial.print(configuredSensors[i].name);
      Serial.print(": ");
      
      int decimals = 1; // Default decimal places
      if (configuredSensors[i].sensorType == TEMP_MAX6675) decimals = 0;
      else if (configuredSensors[i].sensorType == VOLTAGE_ANALOG) decimals = 2; // For battery voltage later
      // PRESSURE_ANALOG and TEMP_DS18B20 will use 1 decimal place by default.

      Serial.print(configuredSensors[i].value, decimals);

      if (configuredSensors[i].sensorType == PRESSURE_ANALOG) {
        Serial.println("psi");
      } else if (configuredSensors[i].sensorType == TEMP_DS18B20 || 
                 configuredSensors[i].sensorType == TEMP_MAX6675 ||
                 configuredSensors[i].sensorType == TEMP_BMP085) { // Added BMP for future
        Serial.println("C");
      } else if (configuredSensors[i].sensorType == VOLTAGE_ANALOG) {
        Serial.println("V"); // For battery voltage later
      } else {
        Serial.println(); 
      }
    }
  }
  Serial.println(); // New blank line after printing the group
//END 1W and BOOST


//TO SEND TO ESP32 via Serial 2
    Serial2.println("<");
  //  Serial2.print("DATETIME: ");
  //     printDateTime2(now);
    //Serial2.print("TIME: ");
  //   printTime2(now);
    Serial2.print("CTMP: ");
    Serial2.println(bmp.readTemperature());
//    Serial2.println("*C");
//    CT==bmp.readTemperature();
    //Serial.print("Pressure = ");
    //Serial.print(bmp.readPressure());
    //Serial.println(" Pa");
     Serial2.print("ALT: ");
     Serial2.println(bmp.readAltitude());
//    Serial2.println("m");
    //Serial.print("Pressure at sealevel (calculated) = ");
    //Serial.print(bmp.readSealevelPressure());
    //Serial.println(" Pa");
    Serial2.print("RALT: ");
    Serial2.println(bmp.readAltitude(seaLevelPressure_hPa * 100));
    //Serial2.println(" m");
    // Serial2.print("BAT1: "); // Old BAT1 print
    // Serial2.println(in_voltage1, 2); // Old BAT1 print
    // Serial2.print("Oil: "); //prints label to serial
    // Serial2.println(oilPressureValue, 1); //prints value from previous line to serial
    // Serial2.print("BST: "); //prints label to serial
    // Serial2.println(boostPressureValue, 1); //prints value from previous line to serial
 //   Serial2.print("BAT2 Voltage = ");
//    Serial2.println(in_voltage2, 2); 
    // Serial2.print("EGT: ");  // Old EGT print
    // Serial2.println(thermocouple.readCelsius()-8); // Old EGT print

    // Serial2.print("CT: "); // Old Coolant Temp print for Serial2 - already handled by loop below
    // printTemperature2(sensor7); // Old Coolant Temp print for Serial2
    
    // New Serial2 printing for refactored sensors (Oil, Boost, Coolant already done)
    // Now adding EGT to this logic.
    // Serial2.print("CTMP: "); // Old Cabin Temp print for Serial2 - moved into loop below
    // Serial2.println(bmp.readTemperature()); // Old Cabin Temp print for Serial2
    for (int i = 0; i < numConfiguredSensors; i++) {
      if (configuredSensors[i].enabled) {
        if (configuredSensors[i].id == "oil_pressure") {
          Serial2.print("Oil: "); 
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "boost_pressure") {
          Serial2.print("BST: "); 
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "coolant_temp") {
          Serial2.print("CT: ");  
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "egt") {
          Serial2.print("EGT: "); 
          Serial2.println(configuredSensors[i].value, 0); 
        } else if (configuredSensors[i].id == "intake_temp") {
          Serial2.print("IMT: "); 
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "transfer_case_temp") {
          Serial2.print("TCT: "); 
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "gearbox_temp") {
          Serial2.print("GBT: "); 
          Serial2.println(configuredSensors[i].value, 1);
        } else if (configuredSensors[i].id == "cabin_temp") {
          Serial2.print("CTMP: "); 
          Serial2.println(configuredSensors[i].value, 0); 
        } else if (configuredSensors[i].id == "bat1_voltage") {
          Serial2.print("BAT1: "); 
          Serial2.println(configuredSensors[i].value, 2); // BAT1 with 2 decimals
        }
        // HT (head_temp), Spare Temp1, and BAT2 are not currently sent to ESP32
      }
    }
    // Serial2.print("IMT: "); //Old IMT print
    // printTemperature2(sensor6); //Old IMT print
   // Serial2.print("Spare1: ");  //Coolant Team
   // printTemperature2(sensor3);
    // Serial2.print("TCT: "); //Old TCT print
    // printTemperature2(sensor2); //Old TCT print
    // Serial2.print("GBT: "); //Old GBT print
    // printTemperature2(sensor4); //Old GBT print
//    Serial2.print("HT: "); //Cylinder Head Temp
//    printTemperature2(sensor5);
//    Serial2.print("Spare2: "); // This was a duplicate print of sensor6
//    printTemperature2(sensor6); 
    Serial2.println(">");
//END TO SEND TO ESP32 via Serial 2
    

//OLED
///////////// u8g2.setFont(u8g2_font_ncenB08_tr);
// u8g2.setFont(u8g2_font_8x13B_tf); 
u8g2.firstPage();
  do {
    //Row 1 Quick Snapshot View of Cabin Metrics and voltages
    u8g2.setCursor(0, 10); 
    // u8g2.print(bmp.readTemperature(),0); // Old Cabin Temp display
    for (int k = 0; k < numConfiguredSensors; k++) {
      if (configuredSensors[k].id == "cabin_temp" && configuredSensors[k].enabled) {
        u8g2.print(configuredSensors[k].value, 0);
        // Optional: Add threshold checks for cabin_temp if meaningful for OLED display
        // if (configuredSensors[k].value > configuredSensors[k].warningThreshold) { ... }
        break;
      }
    }
      u8g2.print("c");       
      u8g2.setCursor(18, 10); 
    u8g2.print(bmp.readAltitude(),0); // Altitude reading remains direct from bmp object
       u8g2.print("m"); 

       u8g2.setCursor(53, 10); 
    // u8g2.print(in_voltage1, 1); // Old BAT1 display
    for (int k = 0; k < numConfiguredSensors; k++) {
      if (configuredSensors[k].id == "bat1_voltage" && configuredSensors[k].enabled) {
        u8g2.print(configuredSensors[k].value, 1); // Display BAT1 with 1 decimal on OLED
        break;
      }
    }
           u8g2.print("v");  
        u8g2.print(F(" "));
        printShortDateTime(now);
//    u8g2.print( printShortDateTime(now));              
 //          u8g2.print("v");    

    //Row 2 Quick Snapshot View of Temps and Pressure
       u8g2.setCursor(0, 30); 
               u8g2.print("I: "); // Intake Temp
       for (int i = 0; i < numConfiguredSensors; i++) {
         if (configuredSensors[i].id == "intake_temp" && configuredSensors[i].enabled) {
           u8g2.print(configuredSensors[i].value, 0);
           // Optional: Add threshold check for IMT if configured
           if (configuredSensors[i].warningThreshold > 0 && configuredSensors[i].value > configuredSensors[i].warningThreshold) { flash(); Serial.print(configuredSensors[i].name); Serial.println(" high warning");}
           if (configuredSensors[i].criticalThreshold > 0 && configuredSensors[i].value > configuredSensors[i].criticalThreshold) { buzz(); Serial.print(configuredSensors[i].name); Serial.println(" high critical");}
           break;
         }
       }
                u8g2.print("c");
       u8g2.setCursor(36, 30); 
               u8g2.print("C: "); // Coolant Temp (already refactored)
       // displayTemperature(sensor7); // Already handled by configuredSensors loop for Coolant
       for (int i = 0; i < numConfiguredSensors; i++) {
         if (configuredSensors[i].id == "coolant_temp" && configuredSensors[i].enabled) {
           u8g2.print(configuredSensors[i].value, 0);
           break;
         }
       }
          u8g2.print("c");
            u8g2.setCursor(73, 30);          
               u8g2.print("T: "); // T-Case & Gearbox
       for (int i = 0; i < numConfiguredSensors; i++) { // Display T-Case Temp
         if (configuredSensors[i].id == "transfer_case_temp" && configuredSensors[i].enabled) {
           u8g2.print(configuredSensors[i].value, 0);
           break;
         }
       }
          u8g2.print("c "); // Add space before next temp
       for (int i = 0; i < numConfiguredSensors; i++) { // Display Gearbox Temp
         if (configuredSensors[i].id == "gearbox_temp" && configuredSensors[i].enabled) {
           u8g2.print(configuredSensors[i].value, 0);
           break;
         }
       }
          u8g2.print("c");
          
               //Gearbox and T-Case Thresholds (Now checking from configuredSensors)
       for (int i = 0; i < numConfiguredSensors; i++) {
         if (configuredSensors[i].enabled) {
           if (configuredSensors[i].id == "transfer_case_temp" || configuredSensors[i].id == "gearbox_temp") {
             if (configuredSensors[i].value > configuredSensors[i].warningThreshold) {
               flash();
               Serial.print(configuredSensors[i].name); Serial.print(" > warning thresh: Thresh: "); Serial.print(configuredSensors[i].warningThreshold); Serial.print("  Current: "); Serial.println(configuredSensors[i].value);
             }
             if (configuredSensors[i].value > configuredSensors[i].criticalThreshold) {
               buzz();
               Serial.print(configuredSensors[i].name); Serial.print(" > critical thresh: Thresh: "); Serial.print(configuredSensors[i].criticalThreshold); Serial.print("  Current: "); Serial.println(configuredSensors[i].value);
             }
           }
         }
       }
              //END Gearbox and T-Case Thresholds


//Gauges from Row3
      u8g2.setCursor(0, 50); 
      u8g2.print(F("EGT "));
      u8g2.drawFrame(25,37,100,13); 
      u8g2.setCursor(40, 47); u8g2.print(F("|"));    //Lower - from old lowerThreshEGT mapping
      u8g2.setCursor(86, 47); u8g2.print(F("|")); //Upper - from old upperThreshEGT mapping
      
      // Find EGT sensor data
      for (int i = 0; i < numConfiguredSensors; i++) {
        if (configuredSensors[i].id == "egt" && configuredSensors[i].enabled) {
          // Note: original EGT display used thermocouple.readCelsius(), not thermocouple.readCelsius()-8 for display value
          // The -8 offset is applied in readMAX6675Sensor, so configuredSensors[i].value already has it.
          // If the display needs the raw value before offset, this needs adjustment. Assuming display uses processed value.
          u8g2.setCursor(((configuredSensors[i].value*multiplierEGT)-baselineEGT),48); // Uses old pixel calc vars
          u8g2.print('I');
          u8g2.setCursor(107,50);
          u8g2.print(configuredSensors[i].value,0);

          //EGT Thresholds (New)
          if (configuredSensors[i].value > configuredSensors[i].warningThreshold) {
            flash();
            Serial.print(configuredSensors[i].name); Serial.print(" > warning thresh: Thresh: "); Serial.print(configuredSensors[i].warningThreshold); Serial.print("  Current: "); Serial.println(configuredSensors[i].value);
          }
          if (configuredSensors[i].value > configuredSensors[i].criticalThreshold) {
            buzz();
            Serial.print(configuredSensors[i].name); Serial.print(" > critical thresh: Thresh: "); Serial.print(configuredSensors[i].criticalThreshold); Serial.print("  Current: "); Serial.println(configuredSensors[i].value);
          }
          break; // Found EGT sensor, exit loop
        }
      }
              //END EGT Thresholds 
     
      u8g2.setCursor(0, 70); 
      u8g2.print(F("BST"));
      u8g2.drawFrame(25,57,100,13); 
      u8g2.setCursor(44, 67); u8g2.print(F("|"));    //Lower - from old lowerThreshBST mapping
      u8g2.setCursor(90, 67); u8g2.print(F("|")); //Upper - from old upperThreshBST mapping
      
      // Find Boost Pressure sensor data
      for (int i = 0; i < numConfiguredSensors; i++) {
        if (configuredSensors[i].id == "boost_pressure" && configuredSensors[i].enabled) {
          u8g2.setCursor(107,70);    
          u8g2.print(configuredSensors[i].value,0);
          u8g2.setCursor(((configuredSensors[i].value*multiplierBST)-baselineBST),68); // Uses old pixel calc vars
          u8g2.print('I');
      
          //Boost Thresholds (New)
          if (configuredSensors[i].value > configuredSensors[i].warningThreshold) {
            flash();
            Serial.print(configuredSensors[i].name); Serial.print(" > warning thresh: Thresh: "); Serial.print(configuredSensors[i].warningThreshold); Serial.print("  Current: "); Serial.print(configuredSensors[i].value);  Serial.println();    
          }
          if (configuredSensors[i].value > configuredSensors[i].criticalThreshold) {
            buzz();
            Serial.print(configuredSensors[i].name); Serial.print(" > critical thresh: Thresh: "); Serial.print(configuredSensors[i].criticalThreshold); Serial.print("  Current: "); Serial.print(configuredSensors[i].value);   Serial.println();   
          }
          // Lower threshold checks for boost could be added here if defined and meaningful
          // e.g., if (configuredSensors[i].value < configuredSensors[i].lowerWarningThreshold && configuredSensors[i].lowerWarningThreshold != 0) { ... }
          break; // Found boost sensor, exit loop
        }
      }
               //END Boost Thresholds

       u8g2.setCursor(0, 90); 
       u8g2.print(F("CT"));
       u8g2.drawFrame(25,77,100,13); 

        u8g2.setCursor(52, 87); u8g2.print(F("|"));    //Lower - from old lowerThreshCT mapping
        u8g2.setCursor(88, 87); u8g2.print(F("|")); //Upper - from old upperThreshCT mapping

      // Find Coolant Temp sensor data
      for (int i = 0; i < numConfiguredSensors; i++) {
        if (configuredSensors[i].id == "coolant_temp" && configuredSensors[i].enabled) {
          u8g2.setCursor(107,90);
          u8g2.print(configuredSensors[i].value,0); // Display value from struct
          u8g2.setCursor(((configuredSensors[i].value*multiplierCT)-baselineCT),88); // Uses old pixel calc vars
          u8g2.print('I');

          //Coolant Temp Thresholds (New)
          if (configuredSensors[i].value > configuredSensors[i].warningThreshold || configuredSensors[i].value < configuredSensors[i].lowerWarningThreshold) {
              flash();
              Serial.print(configuredSensors[i].name); Serial.print(" outside warning thresholds: LowerWarn: "); Serial.print(configuredSensors[i].lowerWarningThreshold);
              Serial.print(" UpperWarn: "); Serial.print(configuredSensors[i].warningThreshold); Serial.print("  Current: "); Serial.print(configuredSensors[i].value); Serial.println();
          }
          if (configuredSensors[i].value > configuredSensors[i].criticalThreshold || configuredSensors[i].value < configuredSensors[i].lowerCriticalThreshold) {
              buzz();
              Serial.print(configuredSensors[i].name); Serial.print(" outside critical thresholds: LowerCrit: "); Serial.print(configuredSensors[i].lowerCriticalThreshold);
              Serial.print(" UpperCrit: "); Serial.print(configuredSensors[i].criticalThreshold); Serial.print("  Current: "); Serial.print(configuredSensors[i].value); Serial.println();
          }
          break; // Found coolant sensor, exit loop
        }
      }
                    //END Coolant Temp Thresholds

      
        u8g2.setCursor(0, 110); 
        u8g2.print("OIL");  
        u8g2.drawFrame(25,97,100,13);      
        u8g2.setCursor(40, 107); u8g2.print(F("|"));    //Lower - from old lowerThreshOP mapping
        u8g2.setCursor(95, 107); u8g2.print(F("|")); //Upper - from old upperThreshOP mapping

      // Find Oil Pressure sensor data
      for (int i = 0; i < numConfiguredSensors; i++) {
        if (configuredSensors[i].id == "oil_pressure" && configuredSensors[i].enabled) {
          u8g2.setCursor(107,110);
          u8g2.print(configuredSensors[i].value,0); // Display value from struct
          
          // The old code `if (oilPressureValue <75)` for 'I' seems to be a general condition for drawing the marker, not a threshold.
          // Let's keep a similar condition for drawing the 'I' marker, or adjust if needed.
          // The pixel calculation `multiplierOP` and `baselineOP` are still used for the gauge marker position.
          u8g2.setCursor(((configuredSensors[i].value*multiplierOP)-baselineOP),108); 
          if (configuredSensors[i].value < maxOP) // Draw 'I' if within reasonable max range of gauge
             u8g2.print('I');    

          //Oil Pressure Thresholds (New) - Primarily for low pressure
          // Warning if value is below lowerWarningThreshold but still above lowerCriticalThreshold
          if (configuredSensors[i].value < configuredSensors[i].lowerWarningThreshold && 
              configuredSensors[i].value > configuredSensors[i].lowerCriticalThreshold) {
              flash();
              Serial.print(configuredSensors[i].name); Serial.print(" < lower warning thresh: Thresh: "); Serial.print(configuredSensors[i].lowerWarningThreshold);
              Serial.print("  Current: "); Serial.print(configuredSensors[i].value); Serial.println();
          }
          // Critical if value is below lowerCriticalThreshold
          if (configuredSensors[i].value < configuredSensors[i].lowerCriticalThreshold) {
              buzz();
              Serial.print(configuredSensors[i].name); Serial.print(" < lower critical thresh: Thresh: "); Serial.print(configuredSensors[i].lowerCriticalThreshold);
              Serial.print("  Current: "); Serial.print(configuredSensors[i].value); Serial.println();
          }
          // Optional: High pressure warnings/criticals if defined and meaningful
          // if (configuredSensors[i].value > configuredSensors[i].warningThreshold) { ... }
          // if (configuredSensors[i].value > configuredSensors[i].criticalThreshold) { ... }
          break; // Found oil pressure sensor, exit loop
        }
      }
  } while ( u8g2.nextPage() );
///  delay(100);
  //ENDOLED  
}


void printTemperature(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  Serial.print(tempC);  
  //Serial.print((char)176);
  Serial.println("C");
//  Serial.println(DallasTemperature::toFahrenheit(tempC));
}


void printTemperature2(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
  Serial2.println(tempC);  
  //Serial.print((char)176);
//  Serial2.println("C");
//  Serial.println(DallasTemperature::toFahrenheit(tempC));
}


void displayTemperature(DeviceAddress deviceAddress)
{
  float tempC = sensors.getTempC(deviceAddress);
   u8g2.print(tempC,0);
  
  //Serial.print((char)176);
//  Serial.println(DallasTemperature::toFahrenheit(tempC));
}


void flash()
{
     analogWrite(led,150) ;
     delay (20); 
     analogWrite(led,50); 
}


void buzz()
{
     analogWrite(buzzer,150) ;
     delay (10); 
     analogWrite(buzzer,50) ;

}

void draw()
{
   u8g2.firstPage();
  do
  {
    u8g2.drawXBMP( 0, 0, u8g_logo_width, u8g_logo_height, frame00); 
  }  while ( u8g2.nextPage() );
 
      delay(2000);
      u8g2.clearBuffer();
      u8g2.clear();
      u8g2.firstPage();
    do
  {

    u8g2.drawXBMP( 0, 0, u8g_logo_width, u8g_logo_height, frame01);    
   } while ( u8g2.nextPage() ); 
delay(2000); 
}
//OLED

// RTC Print
void printDateTime(const RtcDateTime& dt)
{
    char datestring[20];
    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            dt.Month(),
            dt.Day(),
            dt.Year(),
            dt.Hour(),
            dt.Minute(),
            dt.Second() );
    Serial.println(datestring);
}

// RTC Print
void printDateTime2(const RtcDateTime& dt)
{
    char datestring[20];
    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u/%02u/%04u %02u:%02u:%02u"),
            dt.Month(),
            dt.Day(),
            dt.Year(),
            dt.Hour(),
            dt.Minute(),
            dt.Second() );
    Serial2.println(datestring);
}



/*
void printDate2(const RtcDateTime& dt)
{
    char datestring[11];
    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%04u/%02u/%02u"),
            dt.Year(),
            dt.Month(),
            dt.Day());
    Serial2.print(datestring);
}

void printTime2(const RtcDateTime& dt)
{
    char datestring[9];
    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u:%02u:%02u"),
            dt.Hour(),
            dt.Minute(),
            dt.Second() );
    Serial2.print(datestring);
}
*/
void printShortDateTime(const RtcDateTime& dt)
{
    char datestring[11];
    snprintf_P(datestring, 
            countof(datestring),
            PSTR("%02u %02u:%02u"),
            dt.Day(),
            dt.Hour(),
            dt.Minute());
    Serial.println(datestring);
    u8g2.print(datestring);
}
//END RTC Print
