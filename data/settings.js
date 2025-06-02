window.addEventListener('load', loadSensorConfigurations);

let allSensorData = []; // To store sensor data locally for editing

function displayStatusMessage(message, isSuccess) {
    const statusDiv = document.getElementById('status-messages');
    statusDiv.innerHTML = ''; // Clear previous messages
    const messageP = document.createElement('p');
    messageP.textContent = message;
    messageP.className = isSuccess ? 'status-message success' : 'status-message error';
    statusDiv.appendChild(messageP);
    setTimeout(() => { statusDiv.innerHTML = ''; }, 5000); // Clear message after 5 seconds
}

function loadSensorConfigurations() {
    console.log("Loading sensor configurations...");
    const configArea = document.getElementById('sensor-config-area');
    configArea.innerHTML = '<p>Loading sensor configurations...</p>'; // Show loading message

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                try {
                    allSensorData = JSON.parse(this.responseText);
                    displaySensors(allSensorData);
                } catch (e) {
                    configArea.innerHTML = '<p style="color: red;">Error parsing sensor configurations.</p>';
                    console.error("Parse error:", e);
                    displayStatusMessage("Error parsing sensor configurations from ESP32.", false);
                }
            } else {
                configArea.innerHTML = '<p style="color: red;">Failed to load sensor configurations. Status: ' + this.status + '</p>';
                displayStatusMessage("Failed to load sensor configurations. Status: " + this.status, false);
            }
        }
    };
    xhr.open("GET", "/api/sensors", true);
    xhr.send();
}

function displaySensors(sensors) {
    const configArea = document.getElementById('sensor-config-area');
    configArea.innerHTML = ''; // Clear loading message or old table

    if (!sensors || sensors.length === 0) {
        configArea.innerHTML = '<p>No sensor configurations found.</p>';
        return;
    }

    const table = document.createElement('table');
    const thead = table.createTHead();
    const tbody = table.createTBody();
    const headerRow = thead.insertRow();
    const headers = ["ID", "Name", "Type", "Enabled", "Pins", "Addr", "Thresholds (W/C, LW/LC)", "Actions"];
    headers.forEach(text => {
        const th = document.createElement('th');
        th.textContent = text;
        headerRow.appendChild(th);
    });

    sensors.forEach(sensor => {
        const row = tbody.insertRow();
        row.insertCell().textContent = sensor.id;
        row.insertCell().textContent = sensor.name;
        row.insertCell().textContent = sensor.sensorType; // This is already a string like "DS18B20"
        row.insertCell().textContent = sensor.enabled === "1" || sensor.enabled === true ? "Yes" : "No";
        row.insertCell().textContent = `P1:${sensor.pin1}, P2:${sensor.pin2}, P3:${sensor.pin3}`;
        row.insertCell().textContent = sensor.oneWireAddress === "0000000000000000" ? "N/A" : sensor.oneWireAddress;
        row.insertCell().textContent = `${sensor.warningThreshold}/${sensor.criticalThreshold}, ${sensor.lowerWarningThreshold}/${sensor.lowerCriticalThreshold}`;
        
        const actionsCell = row.insertCell();
        const editButton = document.createElement('button');
        editButton.textContent = "Edit";
        editButton.onclick = function() { populateEditForm(sensor.id); };
        actionsCell.appendChild(editButton);
    });

    configArea.appendChild(table);
}

function populateEditForm(sensorId) {
    const form = document.getElementById('sensor-form');
    const formContainer = document.getElementById('sensor-edit-form-container');
    const formTitle = document.getElementById('form-title');
    
    if (sensorId) { // Editing existing sensor
        const sensor = allSensorData.find(s => s.id === sensorId);
        if (!sensor) {
            displayStatusMessage("Error: Sensor not found for editing.", false);
            return;
        }
        formTitle.textContent = "Edit Sensor: " + sensor.name;
        form.elements['form-mode'].value = "edit";
        form.elements['original-sensor-id'].value = sensor.id; // Store original ID in case it's changed

        form.elements['id'].value = sensor.id;
        form.elements['name'].value = sensor.name;
        form.elements['sensorType'].value = sensor.sensorType; // This should match the <option value="...">
        form.elements['pin1'].value = sensor.pin1;
        form.elements['pin2'].value = sensor.pin2;
        form.elements['pin3'].value = sensor.pin3;
        form.elements['oneWireAddress'].value = sensor.oneWireAddress;
        form.elements['enabled'].checked = sensor.enabled === "1" || sensor.enabled === true;
        form.elements['warningThreshold'].value = parseFloat(sensor.warningThreshold).toFixed(2);
        form.elements['criticalThreshold'].value = parseFloat(sensor.criticalThreshold).toFixed(2);
        form.elements['lowerWarningThreshold'].value = parseFloat(sensor.lowerWarningThreshold).toFixed(2);
        form.elements['lowerCriticalThreshold'].value = parseFloat(sensor.lowerCriticalThreshold).toFixed(2);
        form.elements['id'].readOnly = true; // Usually, ID is not editable once created
    } else { // Adding new sensor
        formTitle.textContent = "Add New Sensor";
        form.reset(); // Clear form fields
        form.elements['form-mode'].value = "add";
        form.elements['original-sensor-id'].value = "";
        form.elements['id'].readOnly = false;
    }
    formContainer.classList.remove('hidden');
}

document.getElementById('btn-add-new-sensor').addEventListener('click', function() {
    populateEditForm(null); // Pass null to indicate new sensor
});

document.getElementById('btn-cancel-edit').addEventListener('click', function() {
    document.getElementById('sensor-edit-form-container').classList.add('hidden');
    document.getElementById('sensor-form').reset();
});

document.getElementById('sensor-form').addEventListener('submit', function(event) {
    event.preventDefault(); // Prevent default form submission

    const form = event.target;
    const originalId = form.elements['original-sensor-id'].value;
    const mode = form.elements['form-mode'].value;
    
    // The ID sent to SET_SENSOR_CONF should be the one the Arduino knows if editing,
    // or the new one if adding. If ID can be changed, this needs more complex handling.
    // For now, if ID is editable in "add" mode, use that. If "edit" mode, use original ID.
    // The current form makes ID readonly in edit mode, so form.elements['id'].value is fine.
    const sensorData = {
        id: form.elements['id'].value,
        name: form.elements['name'].value,
        sensorType: form.elements['sensorType'].value, // String value from select
        pin1: parseInt(form.elements['pin1'].value),
        pin2: parseInt(form.elements['pin2'].value),
        pin3: parseInt(form.elements['pin3'].value),
        oneWireAddress: form.elements['oneWireAddress'].value,
        enabled: form.elements['enabled'].checked,
        warningThreshold: parseFloat(form.elements['warningThreshold'].value),
        criticalThreshold: parseFloat(form.elements['criticalThreshold'].value),
        lowerWarningThreshold: parseFloat(form.elements['lowerWarningThreshold'].value),
        lowerCriticalThreshold: parseFloat(form.elements['lowerCriticalThreshold'].value)
    };

    console.log("Saving sensor data:", sensorData);

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("Sensor configuration saved successfully.", true);
                loadSensorConfigurations(); // Refresh list
                document.getElementById('sensor-edit-form-container').classList.add('hidden');
            } else {
                try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error saving sensor: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error saving sensor. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/setconfig", true);
    xhr.setRequestHeader("Content-Type", "application/json;charset=UTF-TUF-8");
    xhr.send(JSON.stringify(sensorData));
});

document.getElementById('btn-save-all-to-esp32-flash').addEventListener('click', function() {
    if (!confirm("Are you sure you want to save all current configurations to the ESP32's flash memory? This will overwrite current settings in flash.")) return;
    
    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("Successfully commanded ESP32 to save configurations to flash.", true);
            } else {
                 try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error saving to ESP32 flash: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error saving to ESP32 flash. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/saveconfig", true); // This endpoint on ESP32 now saves to SPIFFS
    xhr.send();
});

document.getElementById('btn-load-defaults-esp32').addEventListener('click', function() {
    if (!confirm("Are you sure you want to load default configurations on the ESP32? This will overwrite current settings in RAM and save these defaults to its flash memory.")) return;

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("ESP32 commanded to load and save default configurations. Refreshing list...", true);
                loadSensorConfigurations(); // Refresh list to show defaults
            } else {
                 try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error loading defaults on ESP32: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error loading defaults on ESP32. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/loaddefaults", true); // This endpoint on ESP32 loads defaults and saves to SPIFFS
    xhr.send();
});

// --- OneWire Scan Functionality ---
document.getElementById('btn-scan-onewire').addEventListener('click', function() {
    const resultsDiv = document.getElementById('onewire-scan-results');
    resultsDiv.innerHTML = '<p>Scanning for OneWire devices...</p>';

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                try {
                    const addresses = JSON.parse(this.responseText);
                    if (addresses && addresses.length > 0) {
                        let html = '<h4>Unconfigured DS18B20 Sensors Found:</h4><ul>';
                        addresses.forEach(addr => {
                            html += `<li>${addr} <button class="btn-use-address" data-address="${addr}">Use this address</button></li>`;
                        });
                        html += '</ul>';
                        resultsDiv.innerHTML = html;

                        // Add event listeners to new buttons
                        document.querySelectorAll('.btn-use-address').forEach(button => {
                            button.addEventListener('click', function() {
                                handleUseThisAddressClick(this.dataset.address);
                            });
                        });
                    } else {
                        resultsDiv.innerHTML = '<p>No unconfigured DS18B20 sensors found.</p>';
                    }
                } catch (e) {
                    resultsDiv.innerHTML = '<p style="color: red;">Error parsing scan results.</p>';
                    console.error("Parse error for OneWire scan:", e);
                    displayStatusMessage("Error parsing OneWire scan results.", false);
                }
            } else {
                resultsDiv.innerHTML = '<p style="color: red;">Failed to scan for OneWire devices. Status: ' + this.status + '</p>';
                displayStatusMessage("Failed to scan OneWire devices. Status: " + this.status, false);
            }
        }
    };
    xhr.open("GET", "/api/onewire/unconfigured", true);
    xhr.send();
});

function handleUseThisAddressClick(address) {
    // Show the form if it's hidden
    document.getElementById('sensor-edit-form-container').classList.remove('hidden');
    // If the form is in "add" mode, or if user wants to overwrite existing oneWire for a new sensor.
    if (document.getElementById('form-mode').value === 'add') {
         document.getElementById('sensor-id').value = 'ds18b20_' + address.substring(address.length - 4).toLowerCase(); // Suggest an ID
         document.getElementById('sensor-name').value = 'DS18B20 Temp';
    }
    document.getElementById('sensor-type').value = 'DS18B20'; // Set type to DS18B20
    document.getElementById('sensor-onewire').value = address;
    
    // Trigger change event for sensor type to show pin info if any
    document.getElementById('sensor-type').dispatchEvent(new Event('change'));
    
    document.getElementById('sensor-id').focus(); // Focus on ID or name for user to complete
    displayStatusMessage(`OneWire address ${address} populated into form. Please complete other details.`, true);
}

// Add event listener to sensor type dropdown to show pin information
const sensorTypeDropdown = document.getElementById('sensor-type');
const pinInfoSpan = document.createElement('span'); // Create a span for messages
pinInfoSpan.id = 'ds18b20-pin-info';
pinInfoSpan.style.fontSize = '0.8em';
pinInfoSpan.style.marginLeft = '10px';
// Insert it after pin3 or an appropriate place
const pin3Label = document.querySelector('label[for="sensor-pin3"]');
if(pin3Label && pin3Label.parentNode) {
    pin3Label.parentNode.insertBefore(pinInfoSpan, pin3Label.nextSibling.nextSibling); // after input
}


sensorTypeDropdown.addEventListener('change', function() {
    const selectedType = this.value;
    const pin1Input = document.getElementById('sensor-pin1');
    const pin2Input = document.getElementById('sensor-pin2');
    const pin3Input = document.getElementById('sensor-pin3');
    const oneWireInput = document.getElementById('sensor-onewire');
    
    pinInfoSpan.textContent = ''; // Clear previous message
    oneWireInput.readOnly = true; // Default to readonly

    if (selectedType === 'DS18B20') {
        pinInfoSpan.textContent = 'Pins for DS18B20 are typically set by a global OneWire bus pin on ESP32. Pin1 may be used to denote bus if multiple exist (not current fw). P2/P3 unused.';
        oneWireInput.readOnly = false;
    } else if (selectedType === 'MAX6675') {
        pinInfoSpan.textContent = 'P1:SCLK, P2:CS, P3:SO/DO';
    } else if (selectedType === 'PRES_A' || selectedType === 'VOLT_A') {
         pinInfoSpan.textContent = 'P1: Analog Input Pin. P2/P3 unused.';
    } else if (selectedType === 'BMP085' || selectedType === 'BMP280') {
         pinInfoSpan.textContent = 'Uses I2C pins (global). P1/P2/P3 unused in this form.';
    } else {
        // Clear info or set default
    }
});
// Initial check in case form is pre-filled on load (e.g. by browser)
sensorTypeDropdown.dispatchEvent(new Event('change'));
