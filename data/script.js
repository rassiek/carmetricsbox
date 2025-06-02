// Global object to store active gauge instances
window.activeGauges = {};
window.sensorIdToReadingKeyMap = {}; // Map sensor.id to the key used in /readings JSON
let savedWidgetOrder = []; // To store the loaded order of widget IDs

// --- Drag and Drop Functions ---
function onDragStart(event) {
    event.dataTransfer.setData('text/plain', event.target.id);
    event.target.classList.add('dragging');
}

function onDragEnd(event) {
    event.target.classList.remove('dragging');
}

function onDragOver(event) {
    event.preventDefault(); // Necessary to allow dropping
    const cardGrid = document.getElementById('card-grid');
    const draggingCard = document.querySelector('.dragging');
    if (!draggingCard) return;

    const afterElement = getDragAfterElement(cardGrid, event.clientY);
    if (afterElement == null) {
        cardGrid.appendChild(draggingCard);
    } else {
        cardGrid.insertBefore(draggingCard, afterElement);
    }
}

function onDrop(event) {
    event.preventDefault();
    // The actual re-ordering is handled by onDragOver and appending in onDragEnd (or here if preferred)
    // The main task here is to save the new order.
    saveWidgetOrder();
}

function getDragAfterElement(container, y) {
    const draggableElements = [...container.querySelectorAll('.card:not(.dragging)')];

    return draggableElements.reduce((closest, child) => {
        const box = child.getBoundingClientRect();
        const offset = y - box.top - box.height / 2;
        if (offset < 0 && offset > closest.offset) {
            return { offset: offset, element: child };
        } else {
            return closest;
        }
    }, { offset: Number.NEGATIVE_INFINITY }).element;
}

function saveWidgetOrder() {
    const cardGrid = document.getElementById('card-grid');
    const orderedSensorIds = [];
    cardGrid.childNodes.forEach(card => {
        if (card.nodeType === 1 && card.id && card.id.startsWith('card-')) {
            orderedSensorIds.push(card.id.replace('card-', ''));
        }
    });
    localStorage.setItem('widgetOrder', JSON.stringify(orderedSensorIds));
    console.log("Saved widget order:", orderedSensorIds);
}

function loadWidgetOrder() {
    const order = localStorage.getItem('widgetOrder');
    if (order) {
        savedWidgetOrder = JSON.parse(order);
        console.log("Loaded widget order:", savedWidgetOrder);
    } else {
        savedWidgetOrder = [];
        console.log("No saved widget order found.");
    }
}

// --- End Drag and Drop Functions ---

// Function to initialize gauges based on sensor configuration from ESP32
function initGauges() {
    loadWidgetOrder(); // Load saved order first
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var sensorConfigs = JSON.parse(this.responseText);
      var cardGrid = document.getElementById('card-grid');
      cardGrid.innerHTML = ''; // Clear any existing static content
      
      // Add D&D listeners to the grid
      cardGrid.addEventListener('dragover', onDragOver);
      cardGrid.addEventListener('drop', onDrop);

      let createdCards = {}; // Store created card elements before ordering

      sensorConfigs.forEach(function(sensor) {
        if (sensor.enabled === "1" || sensor.enabled === true || sensor.enabled === "true") {
          var cardDiv = document.createElement('div');
          cardDiv.className = 'card';
          cardDiv.id = 'card-' + sensor.id; // Assign ID to the card itself
          cardDiv.draggable = true;
          cardDiv.addEventListener('dragstart', onDragStart);
          cardDiv.addEventListener('dragend', onDragEnd);
          
          var titleP = document.createElement('p');
          titleP.className = 'card-title';
          titleP.textContent = sensor.name;
          
          var canvas = document.createElement('canvas');
          var canvasId = 'gauge-canvas-' + sensor.id;
          canvas.id = canvasId;
          
          cardDiv.appendChild(titleP);
          cardDiv.appendChild(canvas);
          // DON'T append to cardGrid yet, store it
          createdCards[sensor.id] = cardDiv;
          
          // Determine gauge type and options (simplified)
          var gaugeOptions = {
            renderTo: canvasId,
            width: 120, // Default width
            height: 120, // Default height for radial, adjust for linear
            units: "N/A",
            minValue: parseFloat(sensor.lowerCriticalThreshold) < parseFloat(sensor.lowerWarningThreshold) ? parseFloat(sensor.lowerCriticalThreshold) : 0, // A guess
            maxValue: parseFloat(sensor.criticalThreshold) > parseFloat(sensor.warningThreshold) ? parseFloat(sensor.criticalThreshold) : 100, // A guess
            majorTicks: [], // Will be auto-generated or can be set
            highlights: [],
            value: 0, // Initial value
            // Common animation and visual properties
            colorValueBoxRect: "#049faa",
            colorValueBoxRectEnd: "#049faa",
            colorValueBoxBackground: "#f1fbfc",
            colorPlate: "#fff",
            borderShadowWidth: 0,
            borders: false,
            needleType: "arrow", // Default for linear
            needleWidth: 4,
            needleCircleSize: 7,
            needleCircleOuter: true,
            needleCircleInner: false,
            animationDuration: 1500,
            animationRule: "linear",
            barWidth: 8
          };

          // Basic mapping for /readings keys (Arduino sends short keys)
          // This needs to be robust and match Arduino's output from ESP32's getSensorReadings
          if (sensor.id === "oil_pressure") { window.sensorIdToReadingKeyMap[sensor.id] = "Oil"; gaugeOptions.units = "psi"; gaugeOptions.maxValue = 100; gaugeOptions.highlights = [{from:0,to:10,color:'rgba(255,0,0,0.75)'},{from:10,to:20,color:'rgba(255,255,0,0.75)'},{from:20,to:60,color:'rgba(50,205,50,0.75)'},{from:60,to:100,color:'rgba(255,0,0,0.75)'}]; }
          else if (sensor.id === "boost_pressure") { window.sensorIdToReadingKeyMap[sensor.id] = "BST"; gaugeOptions.units = "psi"; gaugeOptions.maxValue = 20; gaugeOptions.highlights = [{from:0,to:8,color:'rgba(255,0,0,0.75)'},{from:8,to:16,color:'rgba(50,205,50,0.75)'},{from:16,to:20,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "coolant_temp") { window.sensorIdToReadingKeyMap[sensor.id] = "CT"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 110; gaugeOptions.highlights = [{from:0,to:40,color:'rgba(255,255,0,0.75)'},{from:40,to:95,color:'rgba(50,205,50,0.75)'},{from:95,to:102,color:'rgba(255,255,0,0.75)'},{from:102,to:110,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "cabin_temp") { window.sensorIdToReadingKeyMap[sensor.id] = "CTMP"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 30; gaugeOptions.highlights = [{from:0,to:15,color:'rgba(169,169,169,0.75)'},{from:15,to:25,color:'rgba(50,205,50,0.75)'},{from:25,to:30,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "intake_temp") { window.sensorIdToReadingKeyMap[sensor.id] = "IMT"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 70; gaugeOptions.highlights = [{from:0,to:50,color:'rgba(50,205,50,0.75)'},{from:50,to:60,color:'rgba(255,255,0,0.75)'},{from:60,to:70,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "egt") { window.sensorIdToReadingKeyMap[sensor.id] = "EGT"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 700; gaugeOptions.highlights = [{from:0,to:400,color:'rgba(50,205,50,0.75)'},{from:400,to:600,color:'rgba(255,255,0,0.75)'},{from:600,to:700,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "gearbox_temp") { window.sensorIdToReadingKeyMap[sensor.id] = "GBT"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 90; gaugeOptions.highlights = [{from:0,to:50,color:'rgba(50,205,50,0.75)'},{from:50,to:70,color:'rgba(255,255,0,0.75)'},{from:70,to:90,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "transfer_case_temp") { window.sensorIdToReadingKeyMap[sensor.id] = "TCT"; gaugeOptions.units = "Temp C"; gaugeOptions.maxValue = 90; gaugeOptions.highlights = [{from:0,to:50,color:'rgba(50,205,50,0.75)'},{from:50,to:70,color:'rgba(255,255,0,0.75)'},{from:70,to:90,color:'rgba(255,0,0,0.75)'}];}
          else if (sensor.id === "bat1_voltage") { window.sensorIdToReadingKeyMap[sensor.id] = "BAT1"; gaugeOptions.units = "V"; gaugeOptions.maxValue = 20; gaugeOptions.highlights = [{from:0,to:11.5,color:'rgba(255,0,0,0.75)'},{from:11.5,to:12.2,color:'rgba(255,255,0,0.75)'},{from:12.2,to:14.2,color:'rgba(50,205,50,0.75)'},{from:14.2,to:20,color:'rgba(255,0,0,0.75)'}];}
          // Add BAT2 if it's sent in readings later
          else { window.sensorIdToReadingKeyMap[sensor.id] = sensor.id; } // Fallback

          var newGauge;
          if (sensor.id.includes("pressure") || sensor.id.includes("boost") || sensor.id.includes("voltage")) {
            gaugeOptions.width = 120; gaugeOptions.height = 120;
            gaugeOptions.needleType = "line"; 
            gaugeOptions.colorNeedle = "#007F80";
            gaugeOptions.colorNeedleEnd = "#007F80";
            gaugeOptions.needleWidth = 4;
            gaugeOptions.needleCircleSize = 3;
            gaugeOptions.colorNeedleCircleOuter = "#007F80";
            newGauge = new RadialGauge(gaugeOptions).draw();
          } else { // Default to Linear for temperatures
            gaugeOptions.width = 80; gaugeOptions.height = 120;
            gaugeOptions.startAngle = 90; gaugeOptions.ticksAngle = 180;
            newGauge = new LinearGauge(gaugeOptions).draw();
          }
          window.activeGauges[sensor.id] = newGauge;
        }
      });

      // Append cards to grid in the saved order, or default order
      if (savedWidgetOrder.length > 0) {
        savedWidgetOrder.forEach(sensorId => {
          if (createdCards[sensorId]) {
            cardGrid.appendChild(createdCards[sensorId]);
            delete createdCards[sensorId]; // Remove from temp storage
          }
        });
      }
      // Append any remaining cards (new sensors not in saved order)
      for (const sensorId in createdCards) {
        if (createdCards.hasOwnProperty(sensorId)) {
          cardGrid.appendChild(createdCards[sensorId]);
        }
      }

      // After initializing gauges, call getReadings for the first time
      getReadings(); 
    } else if (this.readyState == 4) {
      console.error("Failed to load sensor configuration. Status: " + this.status);
      var cardGrid = document.getElementById('card-grid');
      cardGrid.innerHTML = '<p style="color: red; text-align: center;">Failed to load sensor configurations. Check ESP32 connection.</p>';
    }
  };
  xhr.open("GET", "/api/sensors", true);
  xhr.send();
}

// Function to get current readings on the webpage
function getReadings() {
  var xhr = new XMLHttpRequest();
  xhr.onreadystatechange = function() {
    if (this.readyState == 4 && this.status == 200) {
      var myObj = JSON.parse(this.responseText);
      console.log("Readings received:", myObj);

      for (var sensorIdInActiveGauges in window.activeGauges) {
        if (window.activeGauges.hasOwnProperty(sensorIdInActiveGauges)) {
          var readingKey = window.sensorIdToReadingKeyMap[sensorIdInActiveGauges];
          if (myObj.hasOwnProperty(readingKey)) {
            var newValue = parseFloat(myObj[readingKey]);
            if (!isNaN(newValue)) {
              window.activeGauges[sensorIdInActiveGauges].value = newValue;
            } else {
              console.warn("Received NaN for sensor reading: " + readingKey);
            }
          } else {
            console.warn("No reading received for configured gauge (ID: " + sensorIdInActiveGauges + ", ReadingKey: " + readingKey + ")");
          }
        }
      }
    } else if (this.readyState == 4) {
      // Silently ignore failed reading updates for now, or add minimal logging
      // console.error("Failed to get readings. Status: " + this.status);
    }
  }; 
  xhr.open("GET", "/readings", true);
  xhr.send();
}

// Initial setup
window.addEventListener('load', initGauges);
setInterval(getReadings, 3000); // Update every 3 seconds
