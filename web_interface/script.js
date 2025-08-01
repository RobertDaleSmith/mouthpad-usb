class MouthPadController {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.isConnected = false;
        this.gridData = new Array(48).fill(0); // 8x6 grid
        this.dataBuffer = []; // Buffer for handling split packets
        
        // Stream state tracking
        this.streamStates = {
            jcp: false,
            imu: false,
            click: false,
            pwr: false
        };
        
        // Packet activity timers
        this.packetTimers = {
            jcp: null,
            imu: null,
            click: null,
            pwr: null
        };
        
        // Manual stop flags
        this.manualStopFlags = {
            jcp: false,
            imu: false,
            click: false,
            pwr: false
        };
        
        // Timeout duration (2 seconds)
        this.PACKET_TIMEOUT = 2000;
        // Shorter timeout for manual stops (500ms)
        this.MANUAL_STOP_TIMEOUT = 500;
        
        // Track flags pattern
        this.lastFlagsValue = null;
        this.lastFlagsBits = null;
        
        this.initializeElements();
        this.bindEvents();
        this.createGrid();
        this.updateConnectionStatus('disconnected');
    }

    initializeElements() {
        this.connectBtn = document.getElementById('connectBtn');
        this.disconnectBtn = document.getElementById('disconnectBtn');
        this.portSelect = document.getElementById('portSelect');
        this.statusIndicator = document.getElementById('statusIndicator');
        this.statusText = document.getElementById('statusText');
        this.terminal = document.getElementById('terminal');
        this.customCommand = document.getElementById('customCommand');
        this.sendCustomBtn = document.getElementById('sendCustomBtn');
        this.clearLogBtn = document.getElementById('clearLogBtn');
        this.exportLogBtn = document.getElementById('exportLogBtn');
        this.pressureGrid = document.getElementById('pressureGrid');
        this.minPressure = document.getElementById('minPressure');
        this.maxPressure = document.getElementById('maxPressure');
        this.activeCells = document.getElementById('activeCells');
    }

    bindEvents() {
        this.connectBtn.addEventListener('click', () => this.connect());
        this.disconnectBtn.addEventListener('click', () => this.disconnect());
        this.sendCustomBtn.addEventListener('click', () => this.sendCustomCommand());
        this.clearLogBtn.addEventListener('click', () => this.clearLog());
        this.exportLogBtn.addEventListener('click', () => this.exportLog());
        
        // Command buttons
        document.querySelectorAll('.btn-command').forEach(btn => {
            btn.addEventListener('click', (e) => {
                const command = e.target.dataset.command;
                this.handleStreamCommand(command);
            });
        });

        // Enter key for custom command
        this.customCommand.addEventListener('keypress', (e) => {
            if (e.key === 'Enter') {
                this.sendCustomCommand();
            }
        });
    }

    async connect() {
        try {
            this.updateConnectionStatus('connecting');
            this.log('Connecting to serial port...', 'info');

            // Request serial port access with MouthPad USB filter
            this.port = await navigator.serial.requestPort({
                filters: [{ 
                    usbVendorId: 0x1915,  // Nordic Semiconductor
                    usbProductId: 0xEEEE   // MouthPad
                }]
            });
            await this.port.open({ baudRate: 115200 });

            this.writer = this.port.writable.getWriter();
            this.isConnected = true;
            this.updateConnectionStatus('connected');
            this.log('Connected to MouthPad USB!', 'success');

            // Start reading data
            this.startReading();

        } catch (error) {
            this.log(`Connection failed: ${error.message}`, 'error');
            this.updateConnectionStatus('disconnected');
        }
    }

    async disconnect() {
        try {
            // Cancel any ongoing read operations
            if (this.reader) {
                try {
                    await this.reader.cancel();
                } catch (error) {
                    this.log(`Reader cancel error: ${error.message}`, 'warn');
                }
                this.reader = null;
            }
            
            // Close the writer
            if (this.writer) {
                try {
                    await this.writer.close();
                } catch (error) {
                    this.log(`Writer close error: ${error.message}`, 'warn');
                }
                this.writer = null;
            }
            
            // Close the serial port
            if (this.port) {
                try {
                    await this.port.close();
                    this.log('Serial port closed successfully', 'info');
                } catch (error) {
                    this.log(`Port close error: ${error.message}`, 'warn');
                }
                this.port = null;
            }
            
            // Clear any remaining data
            this.dataBuffer = [];
            
            // Reset connection state
            this.isConnected = false;
            this.updateConnectionStatus('disconnected');
            this.log('Disconnected from serial port - port should now be available for other applications', 'success');
            
        } catch (error) {
            this.log(`Disconnect error: ${error.message}`, 'error');
            // Force cleanup even if there's an error
            this.reader = null;
            this.writer = null;
            this.port = null;
            this.isConnected = false;
            this.updateConnectionStatus('disconnected');
        }
    }

    async startReading() {
        try {
            this.reader = this.port.readable.getReader();
            
            while (true) {
                const { value, done } = await this.reader.read();
                if (done) break;
                
                // Process received data
                this.processReceivedData(value);
            }
        } catch (error) {
            this.log(`Reading error: ${error.message}`, 'error');
        }
    }

    processReceivedData(data) {
        this.log(`Received ${data.length} bytes of binary data`, 'data');
        
        // Add received data to buffer
        for (let i = 0; i < data.length; i++) {
            this.dataBuffer.push(data[i]);
        }
        
        this.log(`*** BUFFER SIZE: ${this.dataBuffer.length} bytes ***`, 'debug');
        
        // Process complete packets from buffer
        this.processBuffer();
    }

    processBuffer() {
        // Look for complete packets in buffer
        while (this.dataBuffer.length >= 4) {
            // Look for packet start (0x64 = 'd')
            let packetStart = -1;
            for (let i = 0; i < this.dataBuffer.length - 3; i++) {
                if (this.dataBuffer[i] === 0x64) {
                    packetStart = i;
                    break;
                }
            }
            
            if (packetStart === -1) {
                // No packet start found, clear buffer
                this.log(`*** NO PACKET START FOUND, CLEARING BUFFER (${this.dataBuffer.length} bytes) ***`, 'warn');
                this.dataBuffer = [];
                return;
            }
            
            // Read packet length from header - let's try different approaches
            // The 3rd byte (index 3) seems to be always 0xff, so that's not the length
            // Let's try calculating the length based on the actual packet size we've seen (62 bytes)
            const expectedPacketSize = 62;
            const expectedDataLength = expectedPacketSize - 4; // 4 bytes header
            
            this.log(`*** PACKET HEADER: Type=0x64, Byte1=${this.dataBuffer[packetStart + 1]}, Byte2=${this.dataBuffer[packetStart + 2]}, Byte3=${this.dataBuffer[packetStart + 3]} ***`, 'debug');
            this.log(`*** EXPECTED: ${expectedPacketSize} bytes total, ${expectedDataLength} bytes data ***`, 'debug');
            
            // Use the expected packet size for now
            const totalPacketSize = expectedPacketSize;
            
            this.log(`*** PACKET HEADER: Type=0x64, ExpectedSize=${totalPacketSize} ***`, 'debug');
            
            // Check if we have enough data for the complete packet
            if (this.dataBuffer.length < packetStart + totalPacketSize) {
                // Not enough data for complete packet, wait for more
                this.log(`*** INCOMPLETE PACKET: Need ${totalPacketSize} bytes, have ${this.dataBuffer.length - packetStart} ***`, 'debug');
                return;
            }
            
            // Extract complete packet
            const packet = this.dataBuffer.slice(packetStart, packetStart + totalPacketSize);
            this.dataBuffer = this.dataBuffer.slice(packetStart + totalPacketSize);
            
            this.log(`*** EXTRACTED PACKET: ${packet.length} bytes, Buffer remaining: ${this.dataBuffer.length} ***`, 'debug');
            
            // Process the packet
            this.processPacket(packet);
        }
        
        // Prevent buffer from growing too large
        if (this.dataBuffer.length > 10000) {
            this.log(`*** BUFFER TOO LARGE (${this.dataBuffer.length} bytes), CLEARING ***`, 'warn');
            this.dataBuffer = [];
        }
    }

    processPacket(packet) {
        this.log(`*** PACKET SIZE: ${packet.length} bytes ***`, 'info');
        
        const packetType = packet[0]; // First byte is packet type
        const sequence = packet[1];   // Second byte is sequence
        const flags = packet[2];      // Third byte is flags
        
        // Log the packet in hex
        let hexString = '';
        for (let i = 0; i < Math.min(packet.length, 32); i++) {
            hexString += packet[i].toString(16).padStart(2, '0') + ' ';
        }
        this.log(`*** COMPLETE PACKET: ${packet.length} bytes ***`, 'info');
        this.log(`*** HEX: ${hexString} ***`, 'data');
        
        // Debug: Show first few bytes in detail
        this.log(`*** PACKET STRUCTURE: Type=0x${packet[0].toString(16).padStart(2, '0')}, Seq=0x${packet[1].toString(16).padStart(2, '0')}, Flags=0x${packet[2].toString(16).padStart(2, '0')}, Byte3=0x${packet[3].toString(16).padStart(2, '0')} ***`, 'info');
        
        // Debug: Show first 16 bytes of sensor data to see if there's a pattern
        let sensorHex = '';
        for (let i = 4; i < Math.min(20, packet.length); i++) {
            sensorHex += packet[i].toString(16).padStart(2, '0') + ' ';
        }
        this.log(`*** SENSOR DATA (first 16 bytes): ${sensorHex} ***`, 'info');
        
        // Extract sensor data dynamically based on packet size
        // 4 bytes header (packetType, sequence, flags, byte3), rest is sensor data
        const headerSize = 4;
        const sensorData = packet.slice(headerSize, packet.length);
        
        // Debug: Show how much data we have
        this.log(`*** EXTRACTED ${sensorData.length} bytes of sensor data ***`, 'info');
        this.log(`*** PACKET SIZE: ${packet.length} bytes (${headerSize} header + ${sensorData.length} sensor data) ***`, 'info');
        
        // Detect stream type and update toggle states
        this.detectStreamType(packetType, flags);
        
        // Debug: Show flags value which might contain pressure data
        this.log(`*** FLAGS VALUE: 0x${flags.toString(16).padStart(2, '0')} (${flags}) ***`, 'info');
        
        // Analyze individual bits in flags byte
        const bit0 = (flags & 0x01) ? 1 : 0;
        const bit1 = (flags & 0x02) ? 1 : 0;
        const bit2 = (flags & 0x04) ? 1 : 0;
        const bit3 = (flags & 0x08) ? 1 : 0;
        const bit4 = (flags & 0x10) ? 1 : 0;
        const bit5 = (flags & 0x20) ? 1 : 0;
        const bit6 = (flags & 0x40) ? 1 : 0;
        const bit7 = (flags & 0x80) ? 1 : 0;
        
        this.log(`*** FLAGS BIT ANALYSIS: [${bit7}${bit6}${bit5}${bit4}${bit3}${bit2}${bit1}${bit0}] = 0x${flags.toString(16).padStart(2, '0')} ***`, 'info');
        
        // Track flags pattern if it's incrementing
        if (packetType === 0x64) {
            if (!this.lastFlagsValue) {
                this.lastFlagsValue = flags;
                this.lastFlagsBits = [bit0, bit1, bit2, bit3, bit4, bit5, bit6, bit7];
            } else {
                const diff = flags - this.lastFlagsValue;
                this.log(`*** FLAGS PATTERN: Previous=0x${this.lastFlagsValue.toString(16).padStart(2, '0')}, Current=0x${flags.toString(16).padStart(2, '0')}, Diff=${diff} ***`, 'info');
                
                // Track which bits changed
                const currentBits = [bit0, bit1, bit2, bit3, bit4, bit5, bit6, bit7];
                const changedBits = [];
                for (let i = 0; i < 8; i++) {
                    if (currentBits[i] !== this.lastFlagsBits[i]) {
                        changedBits.push(`Bit${i}:${this.lastFlagsBits[i]}→${currentBits[i]}`);
                    }
                }
                if (changedBits.length > 0) {
                    this.log(`*** CHANGED BITS: ${changedBits.join(', ')} ***`, 'info');
                }
                
                this.lastFlagsValue = flags;
                this.lastFlagsBits = currentBits;
            }
        }
        
        // Debug: Show raw byte pairs and different interpretations
        let debugInfo = [];
        for (let i = 0; i < Math.min(8, sensorData.length); i++) {
            const byte = sensorData[i];
            const value8bit = byte;
            const value9bit = byte; // Could be 9-bit if using bit manipulation
            const value10bit = byte; // Could be 10-bit if using bit manipulation
            
            debugInfo.push(`${byte.toString(16).padStart(2, '0')}->8b:${value8bit} 9b:${value9bit} 10b:${value10bit}`);
        }
        this.log(`*** BYTE VALUES: ${debugInfo.join(' ')} ***`, 'data');
        
        // Only update grid for JCP packets (0x64) - same logic as toggle detection
        if (packetType === 0x64) {
            this.updateGrid(sensorData);
            // Also try interpreting flags as pressure data
            this.log(`*** TRYING FLAGS AS PRESSURE: 0x${flags.toString(16).padStart(2, '0')} = ${flags} ***`, 'info');
            
            // Try different interpretations of the data
            this.tryDifferentDataInterpretations(packet, flags);
        }
        
        // Determine packet type name for logging based on packet type
        let packetTypeName = 'Unknown';
        if (packetType === 0x64) packetTypeName = 'JCP';
        else if (packetType === 0x0f) packetTypeName = 'IMU';
        else if (packetType === 0x0e) packetTypeName = 'Click';
        else if (packetType === 0x0c) packetTypeName = 'PWR';
        
        this.logParsedPacket(packet, sequence, flags, sensorData, packetTypeName);
    }

    detectStreamType(packetType, flags) {
        // Update stream states based on packet type (first byte)
        // The first byte is always 0x64 for all streams, so we need to detect differently
        if (packetType === 0x64) { // All streams use 0x64, need to detect by other means
            // For now, assume all 0x64 packets are JCP since that's what we're testing
            this.log(`*** JCP PACKET DETECTED - Current state: ${this.streamStates.jcp} ***`, 'info');
            if (!this.manualStopFlags.jcp) {
                this.resetPacketTimer('jcp');
            }
            // Always update state and toggle when packet is detected
            if (!this.streamStates.jcp) {
                this.streamStates.jcp = true;
                this.updateStreamToggle('jcp', true);
                this.log('*** JCP STREAM DETECTED - Toggle ON ***', 'success');
            }
        }
        // Note: We need to find a different way to distinguish between stream types
        // since all packets have packetType = 0x64
    }

    resetPacketTimer(streamType) {
        // Clear existing timer
        if (this.packetTimers[streamType]) {
            clearTimeout(this.packetTimers[streamType]);
            this.log(`*** Reset ${streamType.toUpperCase()} timer (${this.PACKET_TIMEOUT}ms) ***`, 'info');
        }
        
        // Set new timer to turn off stream if no packets received
        this.packetTimers[streamType] = setTimeout(() => {
            if (this.streamStates[streamType]) {
                this.streamStates[streamType] = false;
                this.updateStreamToggle(streamType, false);
                this.log(`*** ${streamType.toUpperCase()} STREAM TIMEOUT - Toggle OFF ***`, 'warning');
            }
        }, this.PACKET_TIMEOUT);
    }

    startPacketTimer(streamType) {
        // Start timer when stream is manually stopped
        if (this.packetTimers[streamType]) {
            clearTimeout(this.packetTimers[streamType]);
        }
        
        // Set manual stop flag
        this.manualStopFlags[streamType] = true;
        this.log(`*** Starting ${streamType.toUpperCase()} stop timer (${this.MANUAL_STOP_TIMEOUT}ms) ***`, 'info');
        
        this.packetTimers[streamType] = setTimeout(() => {
            if (this.streamStates[streamType]) {
                this.streamStates[streamType] = false;
                this.updateStreamToggle(streamType, false);
                this.log(`*** ${streamType.toUpperCase()} STREAM TIMEOUT - Toggle OFF ***`, 'warning');
            }
            // Clear manual stop flag
            this.manualStopFlags[streamType] = false;
        }, this.MANUAL_STOP_TIMEOUT);
    }

    updateStreamToggle(streamType, isActive) {
        const button = document.querySelector(`[data-command*="${streamType}"]`);
        if (button) {
            this.log(`*** Updating ${streamType.toUpperCase()} toggle: ${isActive} ***`, 'info');
            if (isActive) {
                button.classList.add('active');
                button.textContent = `Stop ${streamType.toUpperCase()} Stream`;
            } else {
                button.classList.remove('active');
                button.textContent = `Start ${streamType.toUpperCase()} Stream`;
            }
        } else {
            this.log(`*** Button not found for ${streamType} ***`, 'error');
        }
    }

    parseJCPData(data) {
        // This function is now replaced by processBuffer()
        // Keeping for compatibility but not used
    }

    parseIMUData(data) {
        // This function is now replaced by processBuffer()
        // Keeping for compatibility but not used
    }

    parseClickData(data) {
        // This function is now replaced by processBuffer()
        // Keeping for compatibility but not used
    }

    logParsedPacket(packetData, sequence, flags, sensorData, type = 'Unknown') {
        // Log the complete packet in hex format
        let hexString = '';
        for (let j = 0; j < Math.min(packetData.length, 32); j++) {
            hexString += packetData[j].toString(16).padStart(2, '0') + ' ';
        }
        this.log(`*** RECEIVED ${type} BINARY DATA: ${packetData.length} bytes ***`, 'data');
        this.log(`*** FIRST 32 BYTES: ${hexString} ***`, 'data');
        
        // Parse packet structure
        this.log(`*** PARSED ${type} PACKET: Type=0x${packetData[0].toString(16).padStart(2, '0')}, Seq=${sequence}, Flags=0x${flags.toString(16).padStart(2, '0')} ***`, 'info');
        this.log(`*** ${type} DATA (${sensorData.length} bytes): ${this.arrayToHex(sensorData.slice(0, 16))}... ***`, 'data');
        
        // Parse first few sensor values as 16-bit
        const sensorValues = [];
        for (let k = 0; k < Math.min(sensorData.length, 16); k += 2) {
            if (k + 1 < sensorData.length) {
                const value = (sensorData[k + 1] << 8) | sensorData[k]; // Little-endian
                sensorValues.push(value);
            }
        }
        this.log(`*** FIRST ${type} VALUES (16-bit): ${sensorValues.join(' ')} ***`, 'data');
        
        // Calculate statistics
        let minVal = 255;
        let maxVal = 0;
        let activeCount = 0;
        let totalValue = 0;
        
        for (let val of sensorData) {
            minVal = Math.min(minVal, val);
            maxVal = Math.max(maxVal, val);
            if (val > 0) activeCount++;
            totalValue += val;
        }
        
        const avgValue = totalValue / sensorData.length;
        this.log(`*** ${type} STATISTICS: Min=${minVal}, Max=${maxVal}, Active=${activeCount}, Avg=${avgValue.toFixed(1)} ***`, 'info');
    }

    arrayToHex(array) {
        return Array.from(array).map(b => b.toString(16).padStart(2, '0')).join(' ');
    }

    updateGrid(sensorData) {
        let minVal = 255; // 8-bit max
        let maxVal = 0;
        let activeCount = 0;
        let activeCells = [];

        // Debug: Show data length and expected cells
        this.log(`*** SENSOR DATA: ${sensorData.length} bytes, expecting 48 cells ***`, 'info');
        
        // Calculate how many cells we can actually process
        const maxCells = sensorData.length; // 1 byte per cell
        this.log(`*** CAN PROCESS: ${maxCells} cells (${sensorData.length} bytes) ***`, 'info');

        // Update grid cells - 1 byte per cell
        for (let i = 0; i < Math.min(48, maxCells); i++) {
            const byte = sensorData[i];
            
            // Try different value interpretations
            let value;
            
            // Option 1: 8-bit (0-255)
            value = byte;
            
            // Option 2: 9-bit (0-511) - if using bit manipulation
            // value = byte * 2; // Example: scale up 8-bit to 9-bit range
            
            // Option 3: 10-bit (0-1023) - if using bit manipulation  
            // value = byte * 4; // Example: scale up 8-bit to 10-bit range
            
            // Try different grid mappings
            const cellIndex = this.mapGridIndex(i);
            const cell = this.pressureGrid.children[cellIndex];
            
            if (cell) {
                // Update cell value and styling
                cell.textContent = value;
                cell.className = 'grid-cell';
                
                if (value > 0) {
                    activeCount++;
                    activeCells.push(`${cellIndex}:${value}`);
                    if (value > 200) {
                        cell.classList.add('high');
                    } else if (value > 100) {
                        cell.classList.add('medium');
                    } else {
                        cell.classList.add('low');
                    }
                }
                
                minVal = Math.min(minVal, value);
                maxVal = Math.max(maxVal, value);
            }
        }

        // Log active cells for debugging
        if (activeCells.length > 0) {
            this.log(`*** ACTIVE CELLS: ${activeCells.join(' ')} ***`, 'info');
        } else {
            this.log(`*** NO ACTIVE CELLS FOUND ***`, 'warning');
        }

        // Update statistics
        this.minPressure.textContent = minVal;
        this.maxPressure.textContent = maxVal;
        this.activeCells.textContent = activeCount;
    }

    mapGridIndex(dataIndex) {
        // Try different mapping strategies
        // Strategy 1: Row-major order (left-to-right, top-to-bottom)
        // return dataIndex;
        
        // Strategy 2: Column-major order (top-to-bottom, left-to-right)
        // const row = dataIndex % 6;
        // const col = Math.floor(dataIndex / 6);
        // return row * 8 + col;
        
        // Strategy 3: Reversed row-major (bottom-to-top)
        const row = Math.floor(dataIndex / 8);
        const col = dataIndex % 8;
        return (5 - row) * 8 + col;
        
        // Strategy 4: Snake pattern
        // const row = Math.floor(dataIndex / 8);
        // const col = dataIndex % 8;
        // return row * 8 + (row % 2 === 0 ? col : 7 - col);
        
        // Strategy 5: Interleaved columns
        // const row = dataIndex % 6;
        // const col = Math.floor(dataIndex / 6);
        // return row * 8 + (col % 2 === 0 ? col/2 : 4 + col/2);
    }

    createGrid() {
        this.pressureGrid.innerHTML = '';
        
        // Create 8x6 grid (48 cells)
        // Try different orientations to match the actual touchpad layout
        for (let row = 0; row < 6; row++) {
            for (let col = 0; col < 8; col++) {
                const cell = document.createElement('div');
                cell.className = 'grid-cell';
                cell.textContent = '0';
                cell.dataset.row = row;
                cell.dataset.col = col;
                // Add data attributes for debugging
                cell.dataset.index = row * 8 + col;
                this.pressureGrid.appendChild(cell);
            }
        }
        
        // Show the grid mapping for debugging
        this.logGridMapping();
    }

    logGridMapping() {
        let mapping = [];
        for (let i = 0; i < 24; i++) { // Show first 24 cells
            const cellIndex = this.mapGridIndex(i);
            const row = Math.floor(cellIndex / 8);
            const col = cellIndex % 8;
            mapping.push(`${i}->(${row},${col})`);
        }
        this.log(`*** GRID MAPPING: ${mapping.join(' ')} ***`, 'info');
    }

    async sendCommand(command) {
        if (!this.isConnected) {
            this.log('Not connected to serial port', 'error');
            return;
        }

        try {
            const encoder = new TextEncoder();
            const data = encoder.encode(command + '\n');
            await this.writer.write(data);
            this.log(`Sent: ${command}`, 'success');
        } catch (error) {
            this.log(`Failed to send command: ${error.message}`, 'error');
        }
    }

    async sendCustomCommand() {
        const command = this.customCommand.value.trim();
        if (command) {
            await this.sendCommand(command);
            this.customCommand.value = '';
        }
    }

    log(message, type = 'info') {
        const line = document.createElement('div');
        line.className = `terminal-line ${type}`;
        line.textContent = `[${new Date().toLocaleTimeString()}] ${message}`;
        
        this.terminal.appendChild(line);
        this.terminal.scrollTop = this.terminal.scrollHeight;
        
        // Limit log lines to prevent memory issues
        while (this.terminal.children.length > 1000) {
            this.terminal.removeChild(this.terminal.firstChild);
        }
    }

    clearLog() {
        this.terminal.innerHTML = '<div class="terminal-line">Log cleared...</div>';
    }

    exportLog() {
        const logText = Array.from(this.terminal.children)
            .map(line => line.textContent)
            .join('\n');
        
        const blob = new Blob([logText], { type: 'text/plain' });
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `mouthpad-log-${new Date().toISOString().slice(0, 19).replace(/:/g, '-')}.txt`;
        a.click();
        URL.revokeObjectURL(url);
    }

    updateConnectionStatus(status) {
        this.statusIndicator.className = `status-indicator ${status}`;
        
        switch (status) {
            case 'connected':
                this.statusText.textContent = 'Connected';
                this.connectBtn.disabled = true;
                this.disconnectBtn.disabled = false;
                break;
            case 'connecting':
                this.statusText.textContent = 'Connecting...';
                this.connectBtn.disabled = true;
                this.disconnectBtn.disabled = true;
                break;
            case 'disconnected':
                this.statusText.textContent = 'Not Connected';
                this.connectBtn.disabled = false;
                this.disconnectBtn.disabled = true;
                break;
        }
    }

    handleStreamCommand(command) {
        if (!this.isConnected) {
            this.log('Not connected to serial port', 'error');
            return;
        }

        const streamType = command;
        const isActive = this.streamStates[streamType];

        if (isActive) {
            // Stop the stream
            this.streamStates[streamType] = false;
            this.updateStreamToggle(streamType, false);
            this.log(`Stopping ${streamType.toUpperCase()} stream...`, 'info');
            this.sendCommand(`StopStream ${streamType}`);
            // Start timer to wait for packets to stop
            this.log(`*** Manual stop detected - using ${this.MANUAL_STOP_TIMEOUT}ms timeout ***`, 'info');
            this.startPacketTimer(streamType);
        } else {
            // Start the stream
            this.streamStates[streamType] = true;
            this.updateStreamToggle(streamType, true);
            this.log(`Starting ${streamType.toUpperCase()} stream...`, 'info');
            this.sendCommand(`StartStream ${streamType}`);
        }
    }

    tryDifferentDataInterpretations(packet, flags) {
        this.log('*** TRYING DIFFERENT DATA INTERPRETATIONS ***', 'info');

        // Since flags appears to be incrementing (0x24 → 0x25 → 0x26), 
        // it's likely a counter, not pressure data
        this.log(`*** FLAGS APPEARS TO BE COUNTER: 0x${flags.toString(16).padStart(2, '0')} ***`, 'info');

        // Focus on sensor data for pressure values
        const sensorData = packet.slice(4, 52);
        
        // Look for non-zero values in sensor data
        let nonZeroCount = 0;
        let maxValue = 0;
        let minValue = 255;
        
        for (let i = 0; i < sensorData.length; i++) {
            const byte = sensorData[i];
            if (byte > 0) {
                nonZeroCount++;
                maxValue = Math.max(maxValue, byte);
                minValue = Math.min(minValue, byte);
            }
        }
        
        this.log(`*** SENSOR DATA ANALYSIS: Non-zero bytes=${nonZeroCount}, Max=${maxValue}, Min=${minValue} ***`, 'info');
        
        // If we have non-zero values, try using them as pressure data
        if (nonZeroCount > 0) {
            this.log(`*** USING SENSOR DATA AS PRESSURE: ${nonZeroCount} non-zero values found ***`, 'info');
            // Update grid with the sensor data (this will be handled by updateGrid)
        } else {
            this.log(`*** ALL SENSOR DATA IS ZERO - No pressure detected ***`, 'warning');
        }
    }
}

// Initialize the controller when the page loads
document.addEventListener('DOMContentLoaded', () => {
    window.mouthpadController = new MouthPadController();
});