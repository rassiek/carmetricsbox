'use strict';
// Settings forms are built from these tables (app.js CM.form). Adding a sensor or
// actuator type to the firmware = one entry here (docs/EXTENDING.md).
const SCHEMA = (() => {
  const is = (...t) => o => t.includes(o.type);
  const num = (k, label, extra = {}) => ({ k, label, type: 'num', ...extra });
  const hex = v => '0x' + Number(v).toString(16).toUpperCase();

  const sensorTypes = [
    ['ds18b20', 'DS18B20 temperature (1-Wire)'], ['ads1115', 'ADS1115 analog input (5 V senders, 12 V via divider)'],
    ['adc', 'ESP32 analog pin'], ['max6675', 'MAX6675 thermocouple (EGT)'], ['mcp9600', 'MCP9600 thermocouple (I2C)'],
    ['bmp', 'BMP280 / BME280 (air)'], ['ina226', 'INA226 current / voltage'], ['imu', 'MPU-6050 tilt'],
    ['rpm', 'RPM / pulses (alternator W)'], ['switch', 'Switch / warning light'], ['gps', 'GPS'], ['computed', 'Computed from other sensors'],
  ];
  const analog = is('adc', 'ads1115');
  const sensor = [
    { k: 'name', label: 'Name' },
    { k: 'type', label: 'Type', type: 'select', opts: sensorTypes, redraw: true },
    { k: 'unit', label: 'Unit', placeholder: '°C, bar, V…' },
    { k: 'enabled', label: 'Enabled', type: 'bool' },
    // where it is connected
    { k: 'address', label: '1-Wire address', show: is('ds18b20'), help: 'Pick one from Discover below, or leave empty to assign later' },
    { k: 'pin', label: 'Analog pin', type: 'pin', input: true, show: is('adc') },
    { k: 'pin', label: 'Input pin', type: 'pin', input: true, show: is('rpm', 'switch') },
    { k: 'pin', label: 'CS pin', type: 'pin', show: is('max6675') },
    { k: 'sckPin', label: 'SCK pin', type: 'pin', show: is('max6675') },
    { k: 'soPin', label: 'SO pin', type: 'pin', input: true, show: is('max6675') },
    { k: 'adsAddress', label: 'ADS1115 address', type: 'select', num: true, opts: [0x48, 0x49, 0x4A, 0x4B].map(a => [a, hex(a)]), show: is('ads1115') },
    { k: 'adsChannel', label: 'Channel', type: 'select', num: true, opts: [[0, 'A0'], [1, 'A1'], [2, 'A2'], [3, 'A3']], show: is('ads1115') },
    { k: 'mcpAddress', label: 'MCP9600 address', type: 'select', num: true, opts: [0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67].map(a => [a, hex(a)]), show: is('mcp9600') },
    { k: 'i2cAddr', label: 'I2C address', type: 'select', num: true, opts: [0x40, 0x41, 0x44, 0x45].map(a => [a, hex(a)]), show: is('ina226') },
    { k: 'i2cAddr', label: 'I2C address', type: 'select', num: true, opts: [[0x68, '0x68'], [0x69, '0x69']], show: is('imu') },
    { k: 'bmpField', label: 'Reading', type: 'select', opts: [['temperature', 'Temperature'], ['pressure', 'Pressure'], ['altitude', 'Altitude'], ['humidity', 'Humidity (BME280)']], show: is('bmp') },
    { k: 'field', label: 'Reading', type: 'select', opts: [['current', 'Current'], ['voltage', 'Voltage'], ['power', 'Power']], show: is('ina226') },
    { k: 'field', label: 'Reading', type: 'select', opts: [['roll', 'Roll (side tilt)'], ['pitch', 'Pitch (nose up/down)']], show: is('imu') },
    { k: 'field', label: 'Reading', type: 'select', opts: [['speed', 'Speed'], ['altitude', 'Altitude'], ['heading', 'Heading'], ['sats', 'Satellites'], ['hdop', 'Accuracy (HDOP)']], show: is('gps') },
    num('shuntOhms', 'Shunt (Ω)', { show: is('ina226') }),
    num('pulsesPerRev', 'Pulses per revolution', { show: is('rpm'), help: 'Or calibrate with the engine running' }),
    { k: 'activeLow', label: 'On when the input is LOW', type: 'bool', show: is('switch') },
    { k: 'formula', label: 'Formula', type: 'select', redraw: true, show: is('computed'),
      opts: [['diff', 'A − B'], ['sum', 'A + B'], ['rate', 'Change per minute of A'], ['time_in_warn', 'Minutes A spent in warning'], ['curve_margin', 'B minus curve(A)']] },
    { k: 'srcA', label: 'Sensor A', type: 'sensor', show: is('computed') },
    { k: 'srcB', label: 'Sensor B', type: 'sensor', show: o => o.type === 'computed' && ['diff', 'sum', 'curve_margin'].includes(o.formula) },
    // conversion
    num('divider', 'Divider ratio', { show: analog, help: 'Voltage at the sensor ÷ voltage at the input. B25 module: 5' }),
    num('inMin', 'Volts at low end', { show: analog }), num('inMax', 'Volts at high end', { show: analog }),
    num('outMin', 'Value at low end', { show: analog }), num('outMax', 'Value at high end', { show: analog }),
    { k: 'curve', label: 'Curve volts:value (instead of the line)', show: o => analog(o) || o.formula === 'curve_margin', wide: true, placeholder: '0.5:0,2.5:50,4.5:100' },
    num('faultLowV', 'Fault below (V)', { show: analog, help: 'Broken wire / short: a 0.5–4.5 V sender reads < 0.2 V' }),
    num('faultHighV', 'Fault above (V)', { show: analog }),
    num('offset', 'Offset'), num('smoothing', 'Smoothing (0.05 slow – 1 none)', { step: 0.05 }),
    // display and alarms
    num('decimals', 'Decimals'), num('gaugeMin', 'Dial from'), num('gaugeMax', 'Dial to'),
    num('warnLow', 'Warn below'), num('warnHigh', 'Warn above'), num('critLow', 'Critical below'), num('critHigh', 'Critical above'),
    num('hysteresis', 'Hysteresis'),
    { k: 'engineRunningOnly', label: 'Alarm only while running', type: 'bool' },
    { k: 'faultAlarm', label: 'Alarm when the sensor fails', type: 'bool' },
    { k: 'oledStyle', label: 'OLED', type: 'select', opts: [['off', 'Off'], ['text', 'Text'], ['bar', 'Bar'], ['marker', 'Marker']] },
    { k: 'oledLabel', label: 'OLED label (≤3)' },
  ];
  // One-tap starting points for analog inputs
  const analogPresets = [
    ['0.5–4.5 V sender, 0–100 psi', { unit: 'psi', divider: 1, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 100, faultLowV: 0.2, faultHighV: 4.8, curve: '' }],
    ['0.5–4.5 V sender, 0–10 bar', { unit: 'bar', divider: 1, inMin: 0.5, inMax: 4.5, outMin: 0, outMax: 10, faultLowV: 0.2, faultHighV: 4.8, curve: '' }],
    ['12 V battery via B25 (÷5)', { unit: 'V', divider: 5, inMin: 0, inMax: 5, outMin: 0, outMax: 5, faultLowV: null, faultHighV: null, curve: '' }],
    ['0–5 V linear', { divider: 1, inMin: 0, inMax: 5, outMin: 0, outMax: 100, curve: '' }],
  ];

  const actTypes = [['relay', 'Relay (on / off)'], ['pwm', 'PWM (fan / pump speed, dimmer)'], ['hbridge', 'Motor both ways (H-bridge)'], ['servo', 'Servo (angle)']];
  const rules = [['manual', 'Manual'], ['threshold', 'Threshold (on above / below)'], ['curve', 'Curve (sensor → output)'], ['condition', 'Condition']];
  const nonRelay = o => o.type !== 'relay';
  const actuator = [
    { k: 'name', label: 'Name' },
    { k: 'type', label: 'Type', type: 'select', opts: actTypes, redraw: true },
    { k: 'enabled', label: 'Enabled', type: 'bool' },
    { k: 'pin', label: 'Pin', type: 'pin', show: o => o.type !== 'hbridge' },
    { k: 'pin', label: 'Forward pin (RPWM)', type: 'pin', show: is('hbridge') },
    { k: 'pin2', label: 'Reverse pin (LPWM)', type: 'pin', show: is('hbridge') },
    { k: 'activeLow', label: 'Active LOW', type: 'bool', show: o => o.type !== 'servo', help: 'Most relay boards switch on when the input is LOW' },
    { k: 'mode', label: 'Mode', type: 'select', opts: [['on_off', 'On / off'], ['blink', 'Flash']], show: is('relay') },
    num('blinkMs', 'Flash every (ms)', { show: o => o.type === 'relay' && o.mode === 'blink' }),
    { k: 'defaultOn', label: 'On after start', type: 'bool', show: o => o.rule === 'manual' },
    num('freqHz', 'PWM frequency (Hz)', { show: is('pwm', 'hbridge'), help: '25000 for 4-wire fans, 100–1000 for MOSFET modules' }),
    num('minDuty', 'Lowest running %', { show: is('pwm', 'hbridge') }), num('maxDuty', 'Highest %', { show: is('pwm', 'hbridge') }),
    num('kickMs', 'Kick-start (ms at full power)', { show: is('pwm', 'hbridge') }),
    num('rampPerS', 'Ramp (% or ° per second, 0 = at once)', { show: nonRelay }),
    num('servoMinDeg', 'Lowest angle', { show: is('servo') }), num('servoMaxDeg', 'Highest angle', { show: is('servo') }),
    { k: 'rule', label: 'Controlled by', type: 'select', opts: rules, redraw: true },
    { k: 'ruleSensor', label: 'Sensor', type: 'sensor', show: o => ['threshold', 'curve'].includes(o.rule) },
    num('onAt', 'On at', { show: is2('threshold'), help: 'On at or above this; or at or below it if it is lower than "off at"' }),
    num('offAt', 'Off at', { show: is2('threshold') }),
    { k: 'curve', label: 'Curve sensor:output', show: is2('curve'), wide: true, placeholder: '85:0,92:40,100:100' },
    { k: 'condition', label: 'When', type: 'select', show: is2('condition'),
      opts: () => [['running', `${CM.ui().runningWord || 'engine'} running`], ['not_running', `${CM.ui().runningWord || 'engine'} stopped`],
                   ['ignition', 'Ignition on'], ['warn', 'Any warning'], ['crit', 'Any critical alarm'], ['fault', 'A sensor failed']] },
    { k: 'safeState', label: 'If the link / its sensor is lost', type: 'select', opts: [['off', 'Off'], ['on', 'On / safe value'], ['hold', 'Keep as it is']] },
    num('safeDuty', 'Safe value (% or °)', { show: o => o.type !== 'relay' && o.safeState === 'on' }),
  ];
  function is2(r) { return o => o.rule === r; }

  const newSensor = () => ({ name: 'New sensor', type: 'ds18b20', unit: '°C', enabled: true, decimals: 1, gaugeMin: 0, gaugeMax: 120, smoothing: 0.3, oledStyle: 'off', faultAlarm: true, pin: -1, sckPin: -1, soPin: -1 });
  const newActuator = () => ({ name: 'New output', type: 'relay', enabled: true, pin: -1, pin2: -1, activeLow: true, mode: 'on_off', blinkMs: 500, rule: 'manual', safeState: 'off',
                               freqHz: 1000, minDuty: 0, maxDuty: 100, kickMs: 0, rampPerS: 0, safeDuty: 0, servoMinDeg: 0, servoMaxDeg: 180 });
  return { sensor, actuator, sensorTypes, actTypes, analogPresets, newSensor, newActuator, hex };
})();
