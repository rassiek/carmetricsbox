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

// --- GPIO Panel Configuration ---
let allGPIOConfigs = [];

function loadGPIOConfigurations() {
    console.log("Loading GPIO configurations...");
    const gpioListArea = document.getElementById('gpio-config-list-area');
    gpioListArea.innerHTML = '<p>Loading GPIO configurations...</p>';

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                try {
                    allGPIOConfigs = JSON.parse(this.responseText);
                    displayGPIOConfigs(allGPIOConfigs);
                } catch (e) {
                    gpioListArea.innerHTML = '<p style="color: red;">Error parsing GPIO configurations.</p>';
                    console.error("Parse error for GPIO configs:", e);
                    displayStatusMessage("Error parsing GPIO configurations from ESP32.", false);
                }
            } else {
                gpioListArea.innerHTML = '<p style="color: red;">Failed to load GPIO configurations. Status: ' + this.status + '</p>';
                displayStatusMessage("Failed to load GPIO configurations. Status: " + this.status, false);
            }
        }
    };
    xhr.open("GET", "/api/gpioconfig", true);
    xhr.send();
}

function displayGPIOConfigs(gpioConfigs) {
    const gpioListArea = document.getElementById('gpio-config-list-area');
    gpioListArea.innerHTML = '';

    if (!gpioConfigs || gpioConfigs.length === 0) {
        gpioListArea.innerHTML = '<p>No GPIO pin configurations found.</p>';
        return;
    }

    const table = document.createElement('table');
    const thead = table.createTHead();
    const tbody = table.createTBody();
    const headerRow = thead.insertRow();
    const headers = ["ID", "Name", "Pin", "Mode", "Default State/Delay", "Actions"];
    headers.forEach(text => {
        const th = document.createElement('th');
        th.textContent = text;
        headerRow.appendChild(th);
    });

    gpioConfigs.forEach(config => {
        const row = tbody.insertRow();
        row.insertCell().textContent = config.id;
        row.insertCell().textContent = config.name;
        row.insertCell().textContent = config.pinNumber;
        row.insertCell().textContent = config.mode; // Already a string like "ON_OFF" or "BLINK"

        let defaultsText = "";
        if (config.mode === "ON_OFF") {
            defaultsText = config.defaultState ? "ON" : "OFF";
        } else if (config.mode === "BLINK") {
            defaultsText = `Initial: ${config.defaultState ? "Blinking" : "Not Blinking"}, Delay: ${config.defaultBlinkDelayMs}ms`;
        }
        row.insertCell().textContent = defaultsText;

        const actionsCell = row.insertCell();
        const editButton = document.createElement('button');
        editButton.textContent = "Edit";
        editButton.onclick = function() { populateGPIOEditForm(config.id); };
        actionsCell.appendChild(editButton);

        const removeButton = document.createElement('button');
        removeButton.textContent = "Remove";
        removeButton.style.marginLeft = "5px";
        removeButton.onclick = function() { handleRemoveGPIOConfig(config.id); };
        actionsCell.appendChild(removeButton);
    });
    gpioListArea.appendChild(table);
}

function toggleGPIOFormFields(mode) {
    const onOffSettings = document.getElementById('gpio-on-off-settings');
    const blinkSettings = document.getElementById('gpio-blink-settings');
    if (mode === 'ON_OFF') {
        onOffSettings.classList.remove('hidden');
        blinkSettings.classList.add('hidden');
    } else if (mode === 'BLINK') {
        onOffSettings.classList.add('hidden'); // Or repurpose defaultState for initial blink state
        blinkSettings.classList.remove('hidden');
    } else {
        onOffSettings.classList.add('hidden');
        blinkSettings.classList.add('hidden');
    }
}

document.getElementById('gpio-mode').addEventListener('change', function() {
    toggleGPIOFormFields(this.value);
});

function populateGPIOEditForm(gpioId) {
    const form = document.getElementById('gpio-form');
    const formContainer = document.getElementById('gpio-edit-form-container');
    const formTitle = document.getElementById('gpio-form-title');

    if (gpioId) { // Editing
        const config = allGPIOConfigs.find(g => g.id === gpioId);
        if (!config) {
            displayStatusMessage("Error: GPIO config not found for editing.", false);
            return;
        }
        formTitle.textContent = "Edit GPIO Pin: " + config.name;
        form.elements['gpio-form-mode'].value = "edit";
        form.elements['original-gpio-id'].value = config.id;

        form.elements['gpio-id'].value = config.id;
        form.elements['gpio-name'].value = config.name;
        form.elements['gpio-pin-number'].value = config.pinNumber;
        form.elements['gpio-mode'].value = config.mode; // e.g. "ON_OFF"
        if (config.mode === "ON_OFF") {
            form.elements['gpio-default-state'].checked = config.defaultState;
        } else if (config.mode === "BLINK") {
            form.elements['gpio-default-blink-active'].checked = config.defaultState; // defaultState used for initial blink active state
            form.elements['gpio-default-blink-delay'].value = config.defaultBlinkDelayMs;
        }
        form.elements['gpio-id'].readOnly = true;
    } else { // Adding
        formTitle.textContent = "Add New GPIO Pin";
        form.reset();
        form.elements['gpio-form-mode'].value = "add";
        form.elements['original-gpio-id'].value = "";
        form.elements['gpio-id'].readOnly = false;
        form.elements['gpio-default-state'].checked = false; // Explicitly uncheck
        form.elements['gpio-default-blink-active'].checked = false; // Explicitly uncheck
        form.elements['gpio-default-blink-delay'].value = "500";

    }
    toggleGPIOFormFields(form.elements['gpio-mode'].value); // Show/hide fields based on current mode
    formContainer.classList.remove('hidden');
}

document.getElementById('btn-add-new-gpio').addEventListener('click', function() {
    populateGPIOEditForm(null);
});

document.getElementById('btn-cancel-gpio-edit').addEventListener('click', function() {
    document.getElementById('gpio-edit-form-container').classList.add('hidden');
    document.getElementById('gpio-form').reset();
});

document.getElementById('gpio-form').addEventListener('submit', function(event) {
    event.preventDefault();
    const form = event.target;
    const mode = form.elements['gpio-form-mode'].value;
    const originalId = form.elements['original-gpio-id'].value;
    const id = form.elements['gpio-id'].value;

    if (!id || !form.elements['gpio-name'].value || !form.elements['gpio-pin-number'].value) {
        displayStatusMessage("Error: ID, Name, and Pin Number are required.", false);
        return;
    }

    const gpioData = {
        id: id,
        name: form.elements['gpio-name'].value,
        pinNumber: parseInt(form.elements['gpio-pin-number'].value),
        mode: form.elements['gpio-mode'].value, // "ON_OFF" or "BLINK"
        defaultState: form.elements['gpio-mode'].value === 'ON_OFF' ? form.elements['gpio-default-state'].checked : form.elements['gpio-default-blink-active'].checked,
        defaultBlinkDelayMs: form.elements['gpio-mode'].value === 'BLINK' ? parseInt(form.elements['gpio-default-blink-delay'].value) : 0
    };

    let updatedConfigs;
    if (mode === 'edit') {
        updatedConfigs = allGPIOConfigs.map(g => g.id === originalId ? gpioData : g);
    } else { // add
        if (allGPIOConfigs.find(g => g.id === id)) {
            displayStatusMessage(`Error: GPIO Pin with ID '${id}' already exists.`, false);
            return;
        }
        updatedConfigs = [...allGPIOConfigs, gpioData];
    }

    console.log("Saving GPIO configurations:", updatedConfigs);

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("GPIO configurations saved successfully.", true);
                loadGPIOConfigurations(); // Refresh list
                document.getElementById('gpio-edit-form-container').classList.add('hidden');
            } else {
                try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error saving GPIO config: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error saving GPIO config. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/gpioconfig", true);
    xhr.setRequestHeader("Content-Type", "application/json;charset=UTF-8");
    xhr.send(JSON.stringify(updatedConfigs)); // Send the entire array
});

function handleRemoveGPIOConfig(gpioIdToRemove) {
    if (!confirm(`Are you sure you want to remove GPIO pin configuration '${gpioIdToRemove}'?`)) return;

    allGPIOConfigs = allGPIOConfigs.filter(g => g.id !== gpioIdToRemove);

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage(`GPIO configuration '${gpioIdToRemove}' removed successfully.`, true);
                loadGPIOConfigurations(); // Refresh the list
            } else {
                try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error removing GPIO config: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error removing GPIO config. Status: " + this.status + " " + this.statusText, false);
                }
                // If removal failed, we might want to re-fetch to get the true state
                loadGPIOConfigurations();
            }
        }
    };
    xhr.open("POST", "/api/gpioconfig", true);
    xhr.setRequestHeader("Content-Type", "application/json;charset=UTF-8");
    xhr.send(JSON.stringify(allGPIOConfigs)); // Send the modified full list
}

// Update main page load listener
window.removeEventListener('load', loadSensorConfigurations); // Remove old one if present
window.addEventListener('load', function() {
    loadSensorConfigurations();
    loadGPIOConfigurations();
});
