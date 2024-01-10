// Get current sensor readings when the page loads  
window.addEventListener('load', getReadings);
setInterval(getReadings, 3000); // Update every 10 seconds (adjust as needed)


// Create Cabin Temperature Gauge
var gaugeCabinTemp = new LinearGauge({
  renderTo: 'gauge-cabin-temp',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 30,
  majorTicks: ["0", "5", "10", "15", "20", "25", "30"],
  highlights: [
    { from: 0, to: 15, color: 'rgba(169, 169, 169, 0.75)' },  // Grey
    { from: 15, to: 25, color: 'rgba(50, 205, 50, 0.75)' },   // Green
    { from: 25, to: 30, color: 'rgba(255, 0, 0, 0.75)' }     // Red
  ],
  
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
    
  
  // ... other gauge properties
}).draw();

// Create Coolant Temperature Gauge
var gaugeCoolantTemp = new LinearGauge({
  renderTo: 'gauge-coolant-temp',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 110,
  majorTicks: ["0", "20", "40", "60", "80", "100", "110"],
  highlights: [
    { from: 0, to: 40, color: 'rgba(255, 255, 0, 0.75)' },   // Yellow
    { from: 40, to: 95, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 95, to: 102, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 102, to: 110, color: 'rgba(255, 0, 0, 0.75)' }   // Red
  ],
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
  // ... other gauge properties
}).draw();

// Create Intake Air Temperature Gauge
var gaugeIntakeTemp = new LinearGauge({
  renderTo: 'gauge-intake-temp',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 70,
  majorTicks: ["0", "10", "20", "30", "40", "50", "60", "70"],
  highlights: [
    { from: 0, to: 50, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 50, to: 60, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 60, to: 70, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ],
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
  // ... other gauge properties
}).draw();

// Create EGT Gauge
var gaugeEGT = new LinearGauge({
  renderTo: 'gauge-egt',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 700,
  majorTicks: ["0", "100", "200", "300", "400", "500", "600", "700"],
  highlights: [
    { from: 0, to: 400, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 400, to: 600, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 600, to: 700, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ],
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
  // ... other gauge properties
}).draw();

// Create Gear Box Temperature Gauge
var gaugeGearBoxTemp = new LinearGauge({
  renderTo: 'gauge-gearbox-temp',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 90,
  majorTicks: ["0", "20", "40", "60", "80", "90"],
  highlights: [
    { from: 0, to: 50, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 50, to: 70, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 70, to: 90, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ],
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
  // ... other gauge properties
}).draw();

// Create Transfer Case Temperature Gauge
var gaugeTransferCaseTemp = new LinearGauge({
  renderTo: 'gauge-transfercase-temp',
  width: 80,
  height: 120,
  minValue: 0,
  maxValue: 90,
  majorTicks: ["0", "20", "40", "60", "80", "90"],
  highlights: [
    { from: 0, to: 50, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 50, to: 70, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 70, to: 90, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ],
  units: "Temp C",
  minValue: 0,
  startAngle: 90,
  ticksAngle: 180,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  strokeTicks: true,
  colorPlate: "#fff",
  colorBarProgress: "#CC2936",
  colorBarProgressEnd: "#049faa",
  borderShadowWidth: 0,
  borders: false,
  needleType: "arrow",
  needleWidth: 4,
  needleCircleSize: 7,
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear",
  barWidth: 8,
  // ... other gauge properties
}).draw();

var gaugeBatteryVoltage = new RadialGauge({
  renderTo: 'gauge-battery-voltage',
  width: 120,
  height: 120,
  units: "V",
  minValue: 0,
  maxValue: 20,
  value: 15,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  valueInt: 2,
  majorTicks: ["0", "5", "10", "15", "20"],
  minorTicks: 4,
  strokeTicks: true,
  highlights: [
    { from: 0, to: 11.5, color: 'rgba(255, 0, 0, 0.75)' },  // Red
	{ from: 11.5, to: 12.2, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 12.2, to: 14.2, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 14.2, to: 20, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ], 
  colorPlate: "#fff",
  borderShadowWidth: 0,
  borders: false,
  needleType: "line",
  colorNeedle: "#007F80",
  colorNeedleEnd: "#007F80",
  needleWidth: 4,
  needleCircleSize: 3,
  colorNeedleCircleOuter: "#007F80",
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear"
}).draw();

// Create Boost Gauge
var gaugeBoost = new RadialGauge({
  renderTo: 'gauge-boost',
  width: 120,
  height: 120,
  units: "psi",
  minValue: 0,
  maxValue: 20,
  value: 10,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  valueInt: 2,
  majorTicks: ["0", "5", "10", "15", "20"],
  minorTicks: 4,
  strokeTicks: true,
  highlights: [
    { from: 0, to: 8, color: 'rgba(255, 0, 0, 0.75)' },  // Red
    { from: 8, to: 16, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 16, to: 20, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ], 
  colorPlate: "#fff",
  borderShadowWidth: 0,
  borders: false,
  needleType: "line",
  colorNeedle: "#007F80",
  colorNeedleEnd: "#007F80",
  needleWidth: 4,
  needleCircleSize: 3,
  colorNeedleCircleOuter: "#007F80",
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear"
}).draw();

// Create Oil Pressure Gauge
var gaugeOilPressure = new RadialGauge({
  renderTo: 'gauge-oil-pressure',
  width: 120,
  height: 120,
  units: "psi",
  minValue: 0,
  maxValue: 100,
  value: 30,
  colorValueBoxRect: "#049faa",
  colorValueBoxRectEnd: "#049faa",
  colorValueBoxBackground: "#f1fbfc",
  valueInt: 2,
  majorTicks: ["0", "20", "40", "60", "80", "100"],
  minorTicks: 4,
  strokeTicks: true,
  highlights: [
    { from: 0, to: 10, color: 'rgba(255, 0, 0, 0.75)' },  // Red
	{ from: 10, to: 20, color: 'rgba(255, 255, 0, 0.75)' }, // Yellow
    { from: 20, to: 60, color: 'rgba(50, 205, 50, 0.75)' },  // Green
    { from: 60, to: 100, color: 'rgba(255, 0, 0, 0.75)' }    // Red
  ],  
  colorPlate: "#fff",
  borderShadowWidth: 0,
  borders: false,
  needleType: "line",
  colorNeedle: "#007F80",
  colorNeedleEnd: "#007F80",
  needleWidth: 4,
  needleCircleSize: 3,
  colorNeedleCircleOuter: "#007F80",
  needleCircleOuter: true,
  needleCircleInner: false,
  animationDuration: 1500,
  animationRule: "linear"
}).draw();


// Function to get current readings on the webpage when it loads for the first time
function getReadings() {
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var myObj = JSON.parse(this.responseText);
      console.log(myObj);

      // Set values for each gauge based on received metrics
      gaugeCabinTemp.value = myObj.cabinTemperature;
      gaugeCoolantTemp.value = myObj.coolantTemperature;
      gaugeIntakeTemp.value = myObj.intakeTemperature;
      gaugeEGT.value = myObj.egt;
      gaugeGearBoxTemp.value = myObj.gearBoxTemperature;
      gaugeTransferCaseTemp.value = myObj.transferCaseTemperature;
      gaugeBatteryVoltage.value = myObj.batteryVoltage;
      gaugeBoost.value = myObj.boost;
      gaugeOilPressure.value = myObj.oilPressure;
	  
	//  gaugeCabinTemp.value = Number(parseFloat(myObj.cabinTemperature).toFixed(2));
      //gaugeCoolantTemp.value = Number(parseFloat(myObj.coolantTemperature).toFixed(2));
      //gaugeIntakeTemp.value = Number(parseFloat(myObj.intakeTemperature).toFixed(2));
      //gaugeEGT.value = Number(parseFloat(myObj.egt).toFixed(2));
      //gaugeGearBoxTemp.value = Number(parseFloat(myObj.gearBoxTemperature).toFixed(2));
      //gaugeTransferCaseTemp.value = Number(parseFloat(myObj.transferCaseTemperature).toFixed(2));
      //gaugeBatteryVoltage.value = Number(parseFloat(myObj.batteryVoltage).toFixed(2));
      //gaugeBoost.value = Number(parseFloat(myObj.boost).toFixed(2));
      //gaugeOilPressure.value = Number(parseFloat(myObj.oilPressure).toFixed(2));
	  
	  //gaugeCabinTemp.value = parseFloat(myObj.cabinTemperature).toFixed(2).replace(/^0+/, ''); // Format to 2 decimal places, remove leading zeros

	  
	  //gaugeCabinTemp.value = Number(parseFloat(myObj.cabinTemperature).toFixed(2));

	  
	  
    }
  }; 
  xhr.open("GET", "/readings", true);
  xhr.send();
}

// ... (EventSource code remains unchanged)
