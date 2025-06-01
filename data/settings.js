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

document.getElementById('btn-save-all-to-eeprom').addEventListener('click', function() {
    if (!confirm("Are you sure you want to save all current configurations to Arduino's EEPROM? This will overwrite previous EEPROM settings.")) return;

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("Successfully commanded Arduino to save configurations to EEPROM.", true);
            } else {
                 try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error saving to EEPROM: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error saving to EEPROM. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/saveconfig", true);
    xhr.send();
});

document.getElementById('btn-load-defaults').addEventListener('click', function() {
    if (!confirm("Are you sure you want to load default configurations on the Arduino? Current unsaved changes on Arduino will be overwritten. You may need to 'Save All' afterwards to persist them to EEPROM.")) return;

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            if (this.status == 200) {
                displayStatusMessage("Arduino commanded to load default configurations. Refreshing list...", true);
                loadSensorConfigurations(); // Refresh list to show defaults loaded in Arduino RAM
            } else {
                 try {
                    const errResp = JSON.parse(this.responseText);
                    displayStatusMessage("Error loading defaults: " + (errResp.message || this.statusText), false);
                } catch(e) {
                    displayStatusMessage("Error loading defaults. Status: " + this.status + " " + this.statusText, false);
                }
            }
        }
    };
    xhr.open("POST", "/api/loaddefaults", true);
    xhr.send();
});
