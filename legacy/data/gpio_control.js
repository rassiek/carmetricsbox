window.addEventListener('load', loadAndDisplayGPIOControls);

let allGPIOConfigs = [];
let currentGPIOStatuses = {}; // Store by ID: { id: {id, pin_number, current_control_state, actual_pin_level, mode, blink_delay_ms}, ... }

function displayGPIOStatusMessage(message, isSuccess) {
    const statusDiv = document.getElementById('status-messages-gpio');
    if (!statusDiv) return;
    statusDiv.innerHTML = '';
    const messageP = document.createElement('p');
    messageP.textContent = message;
    messageP.className = isSuccess ? 'status-message success' : 'status-message error';
    statusDiv.appendChild(messageP);
    setTimeout(() => { statusDiv.innerHTML = ''; }, 3000);
}

function renderGPIOControls() {
    const controlsArea = document.getElementById('gpio-controls-area');
    controlsArea.innerHTML = ''; // Clear previous controls

    if (allGPIOConfigs.length === 0) {
        controlsArea.innerHTML = '<p>No GPIO pins configured. Configure them in <a href="settings.html">Settings</a>.</p>';
        return;
    }

    allGPIOConfigs.forEach(config => {
        if (!config.enabled) return; // Skip disabled GPIOs

        const status = currentGPIOStatuses[config.id] || {
            current_control_state: config.defaultState,
            actual_pin_level: config.defaultState ? 1 : 0, // Best guess if no status yet
            mode: config.mode,
            blink_delay_ms: config.defaultBlinkDelayMs
        };

        const card = document.createElement('div');
        card.className = 'gpio-card';
        card.id = `gpiocard-${config.id}`;

        let content = `<h3>${config.name} (ID: ${config.id}, Pin: ${config.pinNumber})</h3>`;
        content += `<p>Mode: ${status.mode}</p>`;
        content += `<p>Desired State: <span id="controlstate-${config.id}">${status.current_control_state ? (status.mode === 'BLINK' ? 'Blinking Active' : 'ON') : 'OFF'}</span></p>`;
        content += `<p>Actual Pin Level: <span id="pinlevel-${config.id}">${status.actual_pin_level ? 'HIGH' : 'LOW'}</span></p>`;

        if (config.mode === "ON_OFF") {
            const btnOn = document.createElement('button');
            btnOn.textContent = "Turn ON";
            btnOn.onclick = () => sendGPIOControlCommand(config.id, "SET_STATE", true);

            const btnOff = document.createElement('button');
            btnOff.textContent = "Turn OFF";
            btnOff.className = "off";
            btnOff.onclick = () => sendGPIOControlCommand(config.id, "SET_STATE", false);

            card.innerHTML = content;
            card.appendChild(btnOn);
            card.appendChild(btnOff);
        } else if (config.mode === "BLINK") {
            content += `<p>Blink Delay: <span id="delaydisp-${config.id}">${status.blink_delay_ms}</span> ms</p>`;
            card.innerHTML = content;

            const toggleBlinkBtn = document.createElement('button');
            toggleBlinkBtn.textContent = status.current_control_state ? "Stop Blinking" : "Start Blinking";
            toggleBlinkBtn.className = "blink-toggle" + (status.current_control_state ? " active" : "");
            toggleBlinkBtn.onclick = () => sendGPIOControlCommand(config.id, "SET_STATE", !status.current_control_state);
            card.appendChild(toggleBlinkBtn);

            const delayInput = document.createElement('input');
            delayInput.type = "number";
            delayInput.value = status.blink_delay_ms;
            delayInput.min = "0";
            delayInput.step = "50";
            delayInput.id = `delayinput-${config.id}`;
            card.appendChild(delayInput);

            const setDelayBtn = document.createElement('button');
            setDelayBtn.textContent = "Set Delay";
            setDelayBtn.onclick = () => {
                const newDelay = parseInt(document.getElementById(`delayinput-${config.id}`).value);
                if (!isNaN(newDelay) && newDelay >= 0) {
                    sendGPIOControlCommand(config.id, "SET_BLINK_DELAY", newDelay);
                } else {
                    displayGPIOStatusMessage("Invalid blink delay value.", false);
                }
            };
            card.appendChild(setDelayBtn);
        }
        controlsArea.appendChild(card);
    });
}

function updateGPIOControlDisplay(gpioId, newStatus) {
    const controlStateSpan = document.getElementById(`controlstate-${gpioId}`);
    const pinLevelSpan = document.getElementById(`pinlevel-${gpioId}`);
    const delayDisplaySpan = document.getElementById(`delaydisp-${gpioId}`); // For blink delay
    const toggleBlinkBtn = document.querySelector(`#gpiocard-${gpioId} .blink-toggle`);

    if (newStatus) {
        if (controlStateSpan) {
             controlStateSpan.textContent = newStatus.current_control_state ? (newStatus.mode === 'BLINK' ? 'Blinking Active' : 'ON') : 'OFF';
        }
        if (pinLevelSpan) {
            pinLevelSpan.textContent = newStatus.actual_pin_level ? 'HIGH' : 'LOW';
        }
        if (newStatus.mode === 'BLINK') {
            if(delayDisplaySpan) delayDisplaySpan.textContent = newStatus.blink_delay_ms;
            if(toggleBlinkBtn) {
                toggleBlinkBtn.textContent = newStatus.current_control_state ? "Stop Blinking" : "Start Blinking";
                if (newStatus.current_control_state) toggleBlinkBtn.classList.add('active');
                else toggleBlinkBtn.classList.remove('active');
            }
            const delayInput = document.getElementById(`delayinput-${gpioId}`);
            if(delayInput) delayInput.value = newStatus.blink_delay_ms;
        }
    }
}


function refreshGPIOStatus(specificId = null) {
    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4 && this.status == 200) {
            try {
                const statuses = JSON.parse(this.responseText);
                statuses.forEach(status => {
                    currentGPIOStatuses[status.id] = status;
                    if (!specificId || specificId === status.id) {
                         // If specificId is provided, ideally update only that one for efficiency
                         // For now, if specificId caused the refresh, we might re-render all or just update one
                         // The renderGPIOControls function re-renders all if called without args.
                         // A more granular update would be:
                         updateGPIOControlDisplay(status.id, status);
                    }
                });
                if (!specificId) { // If it was a general refresh, re-render all
                   // renderGPIOControls(); //This might be too much if only one thing changed due to API call
                }
            } catch (e) {
                console.error("Error parsing GPIO status:", e);
            }
        } else if (this.readyState == 4) {
            // console.error("Failed to get GPIO status. Status: " + this.status);
        }
    };
    xhr.open("GET", "/api/gpio/status", true);
    xhr.send();
}


function loadAndDisplayGPIOControls() {
    var xhrConfig = new XMLHttpRequest();
    xhrConfig.onreadystatechange = function() {
        if (this.readyState == 4 && this.status == 200) {
            try {
                allGPIOConfigs = JSON.parse(this.responseText);
                refreshGPIOStatus(); // Get initial statuses
                renderGPIOControls(); // Initial render
                setInterval(refreshGPIOStatus, 2000); // Periodically update statuses
            } catch (e) {
                document.getElementById('gpio-controls-area').innerHTML = '<p style="color: red;">Error parsing GPIO configurations.</p>';
                console.error("Parse error for GPIO configs:", e);
            }
        } else if (this.readyState == 4) {
            document.getElementById('gpio-controls-area').innerHTML = '<p style="color: red;">Failed to load GPIO configurations. Status: ' + this.status + '</p>';
        }
    };
    xhrConfig.open("GET", "/api/gpioconfig", true);
    xhrConfig.send();
}

function sendGPIOControlCommand(id, command, value) {
    let payload = { id: id, command: command };
    if (command === "SET_STATE") {
        payload.state = value; // value is boolean
    } else if (command === "SET_BLINK_DELAY") {
        payload.delay_ms = parseInt(value); // value is number
    }

    console.log("Sending GPIO Control Command:", payload);

    var xhr = new XMLHttpRequest();
    xhr.onreadystatechange = function() {
        if (this.readyState == 4) {
            let message = "";
            let isSuccess = false;
            if (this.status == 200) {
                try {
                    const resp = JSON.parse(this.responseText);
                    message = resp.message || "Command acknowledged.";
                    isSuccess = resp.status === "success";
                } catch (e) {
                    message = "Command sent, but response parse error.";
                    isSuccess = false; // Or true if assuming success on 200 OK
                }
            } else {
                message = "Failed to send command. Status: " + this.status;
                isSuccess = false;
            }
            displayGPIOStatusMessage(message, isSuccess);
            if (isSuccess) {
                // Refresh the status of the specific GPIO or all after a short delay
                setTimeout(() => refreshGPIOStatus(id), 250);
            }
        }
    };
    xhr.open("POST", "/api/gpio/control", true);
    xhr.setRequestHeader("Content-Type", "application/json;charset=UTF-8");
    xhr.send(JSON.stringify(payload));
}
