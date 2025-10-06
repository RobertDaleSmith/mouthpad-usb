class MouthPadController {
    constructor() {
        this.port = null;
        this.reader = null;
        this.writer = null;
        this.isConnected = false;
        this.gridData = new Array(48).fill(0); // 8x6 grid
        this.dataBuffer = []; // Buffer for handling USB CDC fragmentation
        this.lastPacketTime = null; // Track when we last received data
        this.packetFragmentationCount = 0; // Track packet fragmentation
        this.lastFragmentationTime = null; // Track when fragmentation occurred
        
        // Log view mode (raw vs details)
        this.logViewMode = 'details'; // 'details' or 'raw'
        
        // Pressure data storage for charting (like Mac app)
        this.pressureHistory = [];
        this.maxPressureHistoryLength = 100; // Keep last 100 readings
        
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
        
        // Timeout duration (much longer for high data rate)
        this.PACKET_TIMEOUT = 15000; // Increased significantly for high data rate and fragmentation
        // Shorter timeout for manual stops (500ms)
        this.MANUAL_STOP_TIMEOUT = 500;
        
        // Track flags pattern
        this.lastFlagsValue = null;
        this.lastFlagsBits = null;
        
        this.initializeElements();
        this.bindEvents();
        this.createGrid();
        this.initializePressureChart();
        this.updateConnectionStatus('disconnected');
    }

    initializeElements() {
        this.connectBtn = document.getElementById('connectBtn');
        this.disconnectBtn = document.getElementById('disconnectBtn');
        this.statusIndicator = document.getElementById('statusIndicator');
        this.statusText = document.getElementById('statusText');
        this.terminal = document.getElementById('terminal');
        this.customCommand = document.getElementById('customCommand');
        this.sendCustomBtn = document.getElementById('sendCustomBtn');
        this.clearLogBtn = document.getElementById('clearLogBtn');
        this.exportLogBtn = document.getElementById('exportLogBtn');
        this.logViewToggleBtn = document.getElementById('logViewToggleBtn');
        this.touchpadGrid = document.getElementById('touchpadGrid');
        this.minPressure = document.getElementById('minPressure');
        this.maxPressure = document.getElementById('maxPressure');
        this.activeCells = document.getElementById('activeCells');
        
        // Pressure chart elements
        this.pressureChart = document.getElementById('pressureChart');
        this.currentPressure = document.getElementById('currentPressure');
        this.avgPressure = document.getElementById('avgPressure');
        this.peakPressure = document.getElementById('peakPressure');
    }

    bindEvents() {
        this.connectBtn.addEventListener('click', () => this.connect());
        this.disconnectBtn.addEventListener('click', () => this.disconnect());
        this.sendCustomBtn.addEventListener('click', () => this.sendCustomCommand());
        this.clearLogBtn.addEventListener('click', () => this.clearLog());
        this.exportLogBtn.addEventListener('click', () => this.exportLog());
        this.logViewToggleBtn.addEventListener('click', () => this.toggleLogView());
        
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
        // Log raw hex data if in raw mode
        if (this.logViewMode === 'raw') {
            this.logRaw(data);
        } else {
            this.log(`Received ${data.length} bytes of binary data`, 'data');
        }
        
        // Update last packet time
        this.lastPacketTime = Date.now();
        
        // Add received data to buffer (USB CDC fragments data)
        for (let i = 0; i < data.length; i++) {
            this.dataBuffer.push(data[i]);
        }
        
        // this.log(`*** BUFFER SIZE: ${this.dataBuffer.length} bytes ***`, 'debug');
        
        // Debug: Show first few bytes of new data
        if (data.length > 0) {
            const hexData = data.slice(0, Math.min(16, data.length)).map(b => b.toString(16).padStart(2, '0')).join(' ');
            // this.log(`*** NEW DATA HEX: ${hexData} ***`, 'debug');
        }
        
        // Process complete packets from buffer
        this.processBuffer();
    }

    processBuffer() {
        // Look for framed packets - support both old and new formats
        // Old format: [0xAA][LEN_L][LEN_H][DATA...][0x55]
        // New format: [0xAA][0x55][LEN_H][LEN_L][DATA...][CRC_H][CRC_L]
        while (this.dataBuffer.length >= 5) { // Need at least 5 bytes for minimal frame
            // First try new format: dual start markers (0xAA 0x55)
            let packetStart = -1;
            let useNewFormat = false;
            
            for (let i = 0; i < this.dataBuffer.length - 4; i++) {
                if (this.dataBuffer[i] === 0xAA) {
                    if (i + 1 < this.dataBuffer.length && this.dataBuffer[i + 1] === 0x55) {
                        // New format detected
                        packetStart = i;
                        useNewFormat = true;
                        break;
                    } else if (i + 2 < this.dataBuffer.length) {
                        // Old format detected (0xAA followed by length bytes)
                        packetStart = i;
                        useNewFormat = false;
                        break;
                    }
                }
            }
            
            // Debug: If buffer is getting large, show what we're looking for
            if (this.dataBuffer.length > 1000 && packetStart === -1) {
                const searchArea = this.dataBuffer.slice(0, Math.min(100, this.dataBuffer.length));
                const searchHex = searchArea.map(b => b.toString(16).padStart(2, '0')).join(' ');
                // this.log(`*** LARGE BUFFER SEARCH: Looking for 0xAA in: ${searchHex} ***`, 'debug');
            }
            
            if (packetStart === -1) {
                // No packet start found, remove one byte and continue
                this.dataBuffer.shift();
                return;
            }
            
            // Process packet based on detected format
            if (useNewFormat) {
                // New format: [0xAA][0x55][LEN_H][LEN_L][DATA...][CRC_H][CRC_L]
                if (this.dataBuffer.length < packetStart + 6) {
                    this.log(`*** INCOMPLETE NEW FORMAT LENGTH FIELD ***`, 'debug');
                    return;
                }
                
                // Parse packet length (2 bytes, big-endian)
                const packetLength = (this.dataBuffer[packetStart + 2] << 8) | this.dataBuffer[packetStart + 3];
                
                // Sanity check: packet length should be reasonable (max 1000 bytes)
                if (packetLength > 1000 || packetLength < 0) {
                    this.log(`*** INVALID NEW FORMAT PACKET LENGTH: ${packetLength} bytes, removing start marker ***`, 'warn');
                    this.dataBuffer = this.dataBuffer.slice(packetStart + 1);
                    continue;
                }
                
                // this.log(`*** NEW FORMAT FRAMED PACKET: Length=${packetLength} bytes ***`, 'debug');
                
                // Check if we have the complete packet (start + len + payload + crc)
                const totalFramedSize = 4 + packetLength + 2; // start(2) + len(2) + payload + crc(2)
                if (this.dataBuffer.length < packetStart + totalFramedSize) {
                    // Not enough data for complete framed packet, wait for more
                    this.packetFragmentationCount++;
                    this.lastFragmentationTime = Date.now();
                    this.log(`*** INCOMPLETE NEW FORMAT PACKET: Need ${totalFramedSize} bytes, have ${this.dataBuffer.length - packetStart} (Fragmentation #${this.packetFragmentationCount}) ***`, 'debug');
                    
                    if (this.packetFragmentationCount > 5) {
                        this.log(`*** HIGH FRAGMENTATION WARNING: ${this.packetFragmentationCount} fragmented packets detected ***`, 'warn');
                    }
                    return;
                }
                
                // Extract payload and CRC
                const payloadStart = packetStart + 4;
                const payloadEnd = payloadStart + packetLength;
                const crcStart = payloadEnd;
                const crcEnd = crcStart + 2;
                
                const payload = this.dataBuffer.slice(payloadStart, payloadEnd);
                const receivedCrc = (this.dataBuffer[crcStart] << 8) | this.dataBuffer[crcStart + 1];
                
                // Calculate CRC for payload
                const calculatedCrc = this.calculateCRC16(payload);
                
                if (receivedCrc === calculatedCrc) {
                    // CRC matches, process the packet
                    this.dataBuffer = this.dataBuffer.slice(packetStart + totalFramedSize);
                    
                    if (this.packetFragmentationCount > 0) {
                        this.log(`*** PACKET RECOVERY: Successfully processed new format packet after ${this.packetFragmentationCount} fragmentations ***`, 'info');
                        this.packetFragmentationCount = 0;
                    }
                    
                    // this.log(`*** EXTRACTED NEW FORMAT PACKET: ${payload.length} bytes, CRC=0x${receivedCrc.toString(16).padStart(4, '0')}, Buffer remaining: ${this.dataBuffer.length} ***`, 'debug');
                    this.processPacket(payload);
                } else {
                    // CRC mismatch, remove start marker and continue
                    this.log(`*** NEW FORMAT CRC MISMATCH: Expected 0x${calculatedCrc.toString(16).padStart(4, '0')}, got 0x${receivedCrc.toString(16).padStart(4, '0')} ***`, 'warn');
                    this.dataBuffer = this.dataBuffer.slice(packetStart + 1);
                    continue;
                }
            } else {
                // Old format: [0xAA][LEN_L][LEN_H][DATA...][0x55]
                if (this.dataBuffer.length < packetStart + 3) {
                    this.log(`*** INCOMPLETE OLD FORMAT LENGTH FIELD ***`, 'debug');
                    return;
                }
                
                // Parse packet length (2 bytes, little-endian)
                const packetLength = this.dataBuffer[packetStart + 1] | (this.dataBuffer[packetStart + 2] << 8);
                
                // Sanity check: packet length should be reasonable (max 1000 bytes)
                if (packetLength > 1000 || packetLength < 0) {
                    this.log(`*** INVALID OLD FORMAT PACKET LENGTH: ${packetLength} bytes, removing start marker ***`, 'warn');
                    this.dataBuffer = this.dataBuffer.slice(packetStart + 1);
                    continue;
                }
                
                // this.log(`*** OLD FORMAT FRAMED PACKET: Length=${packetLength} bytes ***`, 'debug');
                
                // Check if we have the complete packet (start + len + data + end)
                const totalFramedSize = 1 + 2 + packetLength + 1; // start + length + data + end
                if (this.dataBuffer.length < packetStart + totalFramedSize) {
                    // Not enough data for complete framed packet, wait for more
                    this.packetFragmentationCount++;
                    this.lastFragmentationTime = Date.now();
                    this.log(`*** INCOMPLETE OLD FORMAT PACKET: Need ${totalFramedSize} bytes, have ${this.dataBuffer.length - packetStart} (Fragmentation #${this.packetFragmentationCount}) ***`, 'debug');
                    
                    if (this.packetFragmentationCount > 5) {
                        this.log(`*** HIGH FRAGMENTATION WARNING: ${this.packetFragmentationCount} fragmented packets detected ***`, 'warn');
                    }
                    return;
                }
                
                // Check for end marker
                const endMarkerPos = packetStart + totalFramedSize - 1;
                if (this.dataBuffer[endMarkerPos] !== 0x55) {
                    this.log(`*** INVALID OLD FORMAT END MARKER: Expected 0x55, got 0x${this.dataBuffer[endMarkerPos].toString(16)} ***`, 'warn');
                    this.dataBuffer = this.dataBuffer.slice(packetStart + 1);
                    continue;
                }
                
                // Extract the actual packet data (without framing)
                const payload = this.dataBuffer.slice(packetStart + 3, endMarkerPos);
                this.dataBuffer = this.dataBuffer.slice(packetStart + totalFramedSize);
                
                if (this.packetFragmentationCount > 0) {
                    this.log(`*** PACKET RECOVERY: Successfully processed old format packet after ${this.packetFragmentationCount} fragmentations ***`, 'info');
                    this.packetFragmentationCount = 0;
                }
                
                // this.log(`*** EXTRACTED OLD FORMAT PACKET: ${payload.length} bytes, Buffer remaining: ${this.dataBuffer.length} ***`, 'debug');
                this.processPacket(payload);
            }
        }
        
        // Prevent buffer from growing too large
        if (this.dataBuffer.length > 10000) {
            this.log(`*** BUFFER TOO LARGE (${this.dataBuffer.length} bytes), CLEARING ***`, 'warn');
            this.dataBuffer = [];
        }
        
        // If we have a partial packet for too long, clear it (increased timeout for high data rate)
        if (this.dataBuffer.length > 0 && this.lastPacketTime && (Date.now() - this.lastPacketTime) > 10000) {
            this.log(`*** PARTIAL PACKET TIMEOUT (${this.dataBuffer.length} bytes), CLEARING ***`, 'warn');
            this.dataBuffer = [];
            this.lastPacketTime = null;
        }
    }
    
    calculateCRC16(data) {
        // Calculate CRC-16 (CCITT) for data integrity checking
        let crc = 0xFFFF; // Initial value
        
        for (let i = 0; i < data.length; i++) {
            crc ^= data[i] << 8;
            for (let j = 0; j < 8; j++) {
                if (crc & 0x8000) {
                    crc = (crc << 1) ^ 0x1021; // CCITT polynomial
                } else {
                    crc = crc << 1;
                }
            }
        }
        
                return crc & 0xFFFF;
    }
    
    analyzeCapacitiveData(capTouchData, capacitance) {
        // Analyze the capacitive data to find potential issues
        const validValues = capacitance.filter(v => v > 0 && v < 10000);
        const invalidValues = capacitance.filter(v => v >= 10000 || isNaN(v) || !isFinite(v));
        const zeroValues = capacitance.filter(v => v === 0);
        
        this.log(`*** CAPACITIVE ANALYSIS: Valid=${validValues.length}, Invalid=${invalidValues.length}, Zero=${zeroValues.length} ***`, 'info');
        
        if (invalidValues.length > 0) {
            this.log(`*** INVALID VALUES FOUND: ${invalidValues.slice(0, 5).join(', ')}... ***`, 'warn');
        }
        
        // Check if we have a reasonable number of valid values
        if (validValues.length < 20) {
            this.log(`*** LOW VALID VALUES: Only ${validValues.length} valid cells - possible alignment issue ***`, 'warn');
            
            // Try different alignments
            this.tryCapacitiveAlignments(capTouchData);
        }
        
        // Log the data structure analysis
        this.log(`*** DATA STRUCTURE: ${capTouchData.length} bytes = ${Math.floor(capTouchData.length / 2)} potential cells ***`, 'info');
        if (capTouchData.length > 84) {
            this.log(`*** EXTRA DATA: ${capTouchData.length - 84} bytes beyond 84-byte capacitive data ***`, 'info');
            const extraData = capTouchData.slice(84);
            const extraHex = extraData.map(b => b.toString(16).padStart(2, '0')).join(' ');
            this.log(`*** EXTRA DATA HEX: ${extraHex} ***`, 'debug');
        }
        
        // Analyze the sensor layout to find correct zero positions
        this.analyzeSensorLayout(capacitance);
    }
    
    tryCapacitiveAlignments(capTouchData) {
        // Try different byte alignments to find better data
        const alignments = [0, 1, 2, 4];
        
        for (let offset of alignments) {
            if (offset + 80 <= capTouchData.length) {
                const testData = capTouchData.slice(offset, offset + 80);
                const testCapacitance = [];
                
                // Try to parse as capacitive data
                for (let idx = 0; idx < 40; idx++) {
                    const lowByte = testData[2 * idx];
                    const highByte = testData[2 * idx + 1];
                    const value = (highByte << 8) | lowByte;
                    testCapacitance[idx] = value;
                }
                
                // Check if this looks like valid capacitive data
                const validValues = testCapacitance.filter(v => v > 0 && v < 10000);
                const avgValue = validValues.length > 0 ? validValues.reduce((a, b) => a + b, 0) / validValues.length : 0;
                
                if (validValues.length > 15 && avgValue > 100 && avgValue < 2000) {
                    this.log(`*** POTENTIAL BETTER ALIGNMENT: offset=${offset}, valid=${validValues.length}, avg=${avgValue.toFixed(0)} ***`, 'info');
                }
            }
        }
    }
    
    analyzeSensorLayout(capacitance) {
        // Analyze the sensor layout to identify where zeros should be inserted
        this.log(`*** SENSOR LAYOUT ANALYSIS ***`, 'info');
        
        // Show the first 48 values in a 6x8 grid format
        let gridDisplay = '';
        for (let row = 0; row < 6; row++) {
            let rowStr = '';
            for (let col = 0; col < 8; col++) {
                const idx = row * 8 + col;
                const value = capacitance[idx] || 0;
                rowStr += `${value.toString().padStart(4)} `;
            }
            gridDisplay += `Row ${row}: ${rowStr}\n`;
        }
        this.log(`*** CURRENT GRID LAYOUT:\n${gridDisplay} ***`, 'debug');
        
        // Look for patterns that might indicate where zeros should be
        const zeroCandidates = [];
        for (let i = 0; i < capacitance.length; i++) {
            if (capacitance[i] === 0) {
                zeroCandidates.push(i);
            }
        }
        
        // if (zeroCandidates.length > 0) {
        //     this.log(`*** ZERO CANDIDATES FOUND: ${zeroCandidates.join(', ')} ***`, 'info');
        // }
        
        // Check if the current layout looks like a valid sensor pattern
        const nonZeroValues = capacitance.filter(v => v > 0);
        this.log(`*** NON-ZERO VALUES: ${nonZeroValues.length} cells with values > 0 ***`, 'info');
        
        // Suggest potential zero positions based on common sensor layouts
        // this.suggestZeroPositions(capacitance);
    }
    
    suggestZeroPositions(capacitance) {
        // Common sensor layouts and their zero positions
        const layouts = [
            { name: "Corner zeros", positions: [0, 7, 40, 47] },
            { name: "Edge zeros", positions: [0, 7, 8, 15, 40, 47] },
            { name: "Center zeros", positions: [3, 4, 11, 12, 35, 36, 43, 44] },
            { name: "No zeros", positions: [] }
        ];
        
        this.log(`*** SUGGESTED LAYOUTS: ***`, 'info');
        for (let layout of layouts) {
            const testCapacitance = [...capacitance];
            let insertOffset = 0;
            
            for (let pos of layout.positions) {
                const insertIndex = pos + insertOffset;
                if (insertIndex < testCapacitance.length) {
                    testCapacitance.splice(insertIndex, 0, 0);
                    insertOffset++;
                }
            }
            
            const validValues = testCapacitance.filter(v => v > 0 && v < 10000);
            this.log(`*** ${layout.name}: ${validValues.length} valid values, positions: [${layout.positions.join(', ')}] ***`, 'info');
        }
    }
    
    processPacket(packet) {
        // Log complete packet in raw mode
        if (this.logViewMode === 'raw') {
            this.logRaw(packet);
            // Don't return early - continue to process for grid updates
        }
        
        this.log(`*** PACKET SIZE: ${packet.length} bytes ***`, 'info');
        
        // Parse header for variable-size BLE NUS packets
        if (packet.length < 4) {
            this.log(`*** PACKET TOO SMALL: Need at least 4 bytes header, got ${packet.length} ***`, 'warn');
            return;
        }
        
        // Extract header components
        const packetType = packet[0];     // Byte 0: Type (0x64)
        const sequence = packet[1];       // Byte 1: Sequence
        const flags = packet[2];          // Byte 2: Flags
        const byte3 = packet[3];          // Byte 3: Variable
        
        // Determine packet type and process accordingly
        if (packet.length >= 138) {
            // Large packets (138+ bytes) are always sensor data (JCP, IMU, Click streams)
            const sensorData = packet.slice(4, 138);
            // this.log(`*** SENSOR DATA: ${sensorData.length} bytes ***`, 'info');
            
            // Debug: Show the first few bytes to help identify header structure
            const firstBytes = sensorData.slice(0, 16).map(b => b.toString(16).padStart(2, '0')).join(' ');
            // this.log(`*** FIRST 16 BYTES: ${firstBytes} ***`, 'debug');
            
            // Process the sensor data for BLE NUS format
            this.processBLESensorData(sensorData, packetType, flags);
            
        } else if (packet.length >= 9 && byte3 < 0x10) {
            // Small packets (9 bytes) with low byte3 are power data
            const powerData = packet.slice(4, 9);
            this.log(`*** POWER DATA: ${powerData.length} bytes ***`, 'info');
            
            // Process the power data
            this.processPowerData(powerData, packetType, flags);
        } else {
            this.log(`*** UNKNOWN PACKET FORMAT: ${packet.length} bytes, Byte3=0x${byte3.toString(16)} ***`, 'warn');
        }
        
        // this.log(`*** PACKET STRUCTURE: Type=0x${packetType.toString(16)}, Seq=${sequence}, Flags=0x${flags.toString(16)}, Byte3=0x${byte3.toString(16)} ***`, 'info');
        
        // Update stream detection
        this.detectStreamType(packetType, flags);
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
        const adaptiveTimeout = this.getAdaptiveTimeout(streamType);
        this.packetTimers[streamType] = setTimeout(() => {
            if (this.streamStates[streamType]) {
                this.streamStates[streamType] = false;
                this.updateStreamToggle(streamType, false);
                this.log(`*** ${streamType.toUpperCase()} STREAM TIMEOUT - Toggle OFF ***`, 'warning');
            }
        }, adaptiveTimeout);
    }
    
    getAdaptiveTimeout(streamType) {
        // Use longer timeout when we detect high data rate (touch activity)
        if (streamType === 'jcp') {
            // If we have recent touch activity, use much longer timeout
            const now = Date.now();
            if (this.lastTouchTime && (now - this.lastTouchTime) < 10000) {
                return 30000; // 30 seconds when touch is active
            }
        }
        return this.PACKET_TIMEOUT;
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
            this.log(`*** Updating ${streamType.toLowerCase()} toggle: ${isActive} ***`, 'info');
            if (isActive) {
                button.classList.add('active');
                button.textContent = `⏹ StopStream ${streamType.toLowerCase()}`;
            } else {
                button.classList.remove('active');
                button.textContent = `▶ StartStream ${streamType.toLowerCase()}`;
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
        this.touchpadGrid.innerHTML = '';
        
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
                this.touchpadGrid.appendChild(cell);
            }
        }
        
        // Show the grid mapping for debugging
        // this.logGridMapping();
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
        // Don't log anything in raw mode except raw hex data
        if (this.logViewMode === 'raw') {
            return;
        }
        
        const timestamp = new Date().toLocaleTimeString();
        const logEntry = document.createElement('div');
        logEntry.className = `log-entry log-${type}`;
        logEntry.innerHTML = `<span class="timestamp">[${timestamp}]</span> ${message}`;
        
        this.terminal.appendChild(logEntry);
        
        // Auto-scroll to bottom
        this.terminal.scrollTop = this.terminal.scrollHeight;
        
        // Auto-clear old entries to prevent browser lockup
        const maxLogEntries = 1000; // Keep last 1000 entries
        const logEntries = this.terminal.querySelectorAll('.log-entry');
        if (logEntries.length > maxLogEntries) {
            // Remove oldest entries (keep the last maxLogEntries)
            const entriesToRemove = logEntries.length - maxLogEntries;
            for (let i = 0; i < entriesToRemove; i++) {
                logEntries[i].remove();
            }
        }
        
        // Also clear console to prevent memory issues
        if (this.terminal.children.length > 2000) {
            this.terminal.innerHTML = '';
            const clearMessage = document.createElement('div');
            clearMessage.className = 'log-entry log-info';
            clearMessage.innerHTML = `<span class="timestamp">[${timestamp}]</span> *** LOGS AUTO-CLEARED TO PREVENT BROWSER LOCKUP ***`;
            this.terminal.appendChild(clearMessage);
        }
    }

    logRaw(data) {
        if (this.logViewMode !== 'raw') {
            return; // Only log raw data in raw mode
        }
        
        const timestamp = new Date().toLocaleTimeString();
        const logEntry = document.createElement('div');
        logEntry.className = 'log-entry raw';
        
        // Convert data to hex string with spaces
        const hexString = Array.from(data).map(byte => byte.toString(16).padStart(2, '0')).join(' ');
        
        // Add packet size indicator
        const packetSize = data.length;
        const sizeIndicator = packetSize > 64 ? '📦' : packetSize > 32 ? '📄' : '📝';
        
        logEntry.innerHTML = `<span class="timestamp">[${timestamp}]</span> <span class="hex-data">${sizeIndicator} ${hexString}</span>`;
        
        this.terminal.appendChild(logEntry);
        
        // Auto-scroll to bottom
        this.terminal.scrollTop = this.terminal.scrollHeight;
        
        // Auto-clear old entries to prevent browser lockup
        const maxLogEntries = 1000;
        const logEntries = this.terminal.querySelectorAll('.log-entry');
        if (logEntries.length > maxLogEntries) {
            const entriesToRemove = logEntries.length - maxLogEntries;
            for (let i = 0; i < entriesToRemove; i++) {
                logEntries[i].remove();
            }
        }
        
        // Also clear console to prevent memory issues
        if (this.terminal.children.length > 2000) {
            this.terminal.innerHTML = '';
            const clearMessage = document.createElement('div');
            clearMessage.className = 'log-entry log-info';
            clearMessage.innerHTML = `<span class="timestamp">[${timestamp}]</span> *** LOGS AUTO-CLEARED TO PREVENT BROWSER LOCKUP ***`;
            this.terminal.appendChild(clearMessage);
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
        // Add null checks to prevent errors if DOM elements aren't loaded yet
        if (this.statusIndicator) {
            this.statusIndicator.className = `status-indicator ${status}`;
        }
        
        switch (status) {
            case 'connected':
                if (this.statusText) this.statusText.textContent = 'Connected';
                if (this.connectBtn) this.connectBtn.disabled = true;
                if (this.disconnectBtn) this.disconnectBtn.disabled = false;
                break;
            case 'connecting':
                if (this.statusText) this.statusText.textContent = 'Connecting...';
                if (this.connectBtn) this.connectBtn.disabled = true;
                if (this.disconnectBtn) this.disconnectBtn.disabled = true;
                break;
            case 'disconnected':
                if (this.statusText) this.statusText.textContent = 'Not Connected';
                if (this.connectBtn) this.connectBtn.disabled = false;
                if (this.disconnectBtn) this.disconnectBtn.disabled = true;
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

    processCapacitiveData(capTouchData) {
        // Parse exactly like Mac app capacitanceData function
        this.log(`*** RECEIVED CAPACITIVE DATA (in processCapacitiveData): ${capTouchData.length} bytes ***`, 'debug');
        const capDataHex = capTouchData.map(b => b.toString(16).padStart(2, '0')).join(' ');
        this.log(`*** CAP DATA HEX (in processCapacitiveData): ${capDataHex} ***`, 'debug');
        const capacitance = [];
        
        // Calculate how many cells we can extract (2 bytes per cell)
        // But limit to 42 cells (84 bytes) to avoid invalid data at the end
        const maxValidCells = 42; // 84 bytes of valid capacitive data
        const numCells = Math.min(Math.floor(capTouchData.length / 2), maxValidCells);
        this.log(`*** EXTRACTING ${numCells} CELLS FROM ${capTouchData.length} BYTES (max ${maxValidCells}) ***`, 'info');
        
        // Extract sensor values based on data length (like Mac app)
        const sensorValues = [];
        let zeroPositions = [];
        
        if (capTouchData.length === 80) {
            // 80 bytes = 40 cells (old format)
            for (let idx = 0; idx < 40; idx++) {
                const lowByte = capTouchData[2 * idx];
                const highByte = capTouchData[2 * idx + 1];
                const value = (highByte << 8) | lowByte; // Little-endian 16-bit
                sensorValues[idx] = value;
            }
            zeroPositions = [0, 1, 6, 7, 8, 15, 40, 47]; // 8 positions for old format
            this.log(`*** OLD FORMAT: 40 sensor values, 8 zero positions ***`, 'info');
        } else {
            // 88+ bytes = 44+ cells (new format)
            for (let idx = 0; idx < 44; idx++) {
                const lowByte = capTouchData[2 * idx];
                const highByte = capTouchData[2 * idx + 1];
                const value = (highByte << 8) | lowByte; // Little-endian 16-bit
                sensorValues[idx] = value;
            }
            zeroPositions = [0, 7, 40, 47]; // 4 positions for new format
            this.log(`*** NEW FORMAT: 44 sensor values, 4 zero positions ***`, 'info');
        }
        
        this.log(`*** EXTRACTED ${sensorValues.length} SENSOR VALUES FROM ${capTouchData.length} BYTES ***`, 'info');
        this.log(`*** SENSOR VALUES (first 10): ${sensorValues.slice(0, 10).join(', ')} ***`, 'debug');
        
        // Don't reverse here - we'll reverse just before grid update
        
        // Now insert zeros at the specified positions to get 48 total values
        let sensorIndex = 0;
        
        for (let i = 0; i < 48; i++) {
            if (zeroPositions.includes(i)) {
                // This is a zero position, insert 0
                capacitance[i] = 0;
                // this.log(`*** ZERO AT INDEX ${i} ***`, 'debug');
            } else {
                // This is a sensor position, use the next sensor value
                if (sensorIndex < sensorValues.length) {
                    capacitance[i] = sensorValues[sensorIndex];
                    sensorIndex++;
                } else {
                    capacitance[i] = 0; // Fallback
                }
            }
        }
        
        this.log(`*** FINAL CAPACITANCE ARRAY: ${capacitance.length} values ***`, 'info');
        
        // Analyze which side is being touched
        const leftSide = capacitance.slice(0, 24); // First 24 cells (left side)
        const rightSide = capacitance.slice(24, 48); // Last 24 cells (right side)
        
        // Filter out invalid values (too high = reading beyond data)
        const validLeftSide = leftSide.filter(v => !isNaN(v) && isFinite(v) && v < 10000);
        const validRightSide = rightSide.filter(v => !isNaN(v) && isFinite(v) && v < 10000);
        
        const leftMax = validLeftSide.length > 0 ? Math.max(...validLeftSide) : 0;
        const rightMax = validRightSide.length > 0 ? Math.max(...validRightSide) : 0;
        const leftActive = leftSide.filter(v => v > 100 && !isNaN(v) && v < 10000).length;
        const rightActive = rightSide.filter(v => v > 100 && !isNaN(v) && v < 10000).length;
        
        this.log(`*** TOUCH ANALYSIS: Left max=${leftMax} (${leftActive} active), Right max=${rightMax} (${rightActive} active) ***`, 'info');
        
        // Detect potential hardware issues
        if (leftMax > 800 && rightMax < 100) {
            this.log(`*** LEFT SIDE TOUCH DETECTED - Monitoring for packet issues ***`, 'warn');
        } else if (rightMax > 800 && leftMax < 100) {
            this.log(`*** RIGHT SIDE TOUCH DETECTED - Monitoring for packet issues ***`, 'warn');
        }
        
        // this.log(`*** CAPACITIVE: ${capacitance.length} cells, values: ${capacitance.slice(0, 10).join(', ')}... ***`, 'info');
        
        // Analyze the data to find potential alignment issues
        this.analyzeCapacitiveData(capTouchData, capacitance);
        
        // Update the grid with complete data
        this.updateGridFromCompleteCapacitive44(capacitance);
    }
    
    processPressureData(pressureData, packetIndex) {
        // this.log(`*** PROCESSING PRESSURE DATA: ${pressureData.length} bytes ***`, 'info');
        
        // Parse pressure data according to Mac app format:
        // data[0...3] = pressure reading (Float32)
        // data[4...5] = temperature (2 bytes)
        // data[6] = mouse state (1 byte)
        // data[7...8] = duration (2 bytes)
        // data[9...12] = mean pressure (Float32)
        // data[17...20] = upper threshold (Float32)
        // data[21...24] = lower threshold (Float32)
        
        let pressureReading = 0;
        let upperThreshold = 0;
        let lowerThreshold = 0;
        
        // Parse pressure reading (bytes 0-3)
        if (pressureData.length >= 4) {
            try {
                const pressureBytes = pressureData.slice(0, 4);
                const pressureHex = pressureBytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
                this.log(`*** PRESSURE READING BYTES (0-3): ${pressureHex} ***`, 'debug');
                
                const bytes = new Uint8Array(pressureBytes);
                const view = new DataView(bytes.buffer);
                pressureReading = view.getFloat32(0, false); // Big-endian first
                this.log(`*** PARSED PRESSURE READING: ${pressureReading} ***`, 'debug');
                
            } catch (e) {
                this.log(`*** PRESSURE READING PARSE FAILED: ${e.message} ***`, 'warn');
            }
        }
        
        // Parse upper threshold (bytes 17-20)
        if (pressureData.length >= 21) {
            try {
                const upperBytes = pressureData.slice(17, 21);
                const upperHex = upperBytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
                this.log(`*** UPPER THRESHOLD BYTES (17-20): ${upperHex} ***`, 'debug');
                
                const bytes = new Uint8Array(upperBytes);
                const view = new DataView(bytes.buffer);
                upperThreshold = view.getFloat32(0, false); // Big-endian first
                this.log(`*** PARSED UPPER THRESHOLD: ${upperThreshold} ***`, 'debug');
                
            } catch (e) {
                this.log(`*** UPPER THRESHOLD PARSE FAILED: ${e.message} ***`, 'warn');
            }
        }
        
        // Parse lower threshold (bytes 21-24)
        if (pressureData.length >= 25) {
            try {
                const lowerBytes = pressureData.slice(21, 25);
                const lowerHex = lowerBytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
                this.log(`*** LOWER THRESHOLD BYTES (21-24): ${lowerHex} ***`, 'debug');
                
                const bytes = new Uint8Array(lowerBytes);
                const view = new DataView(bytes.buffer);
                lowerThreshold = view.getFloat32(0, false); // Big-endian first
                this.log(`*** PARSED LOWER THRESHOLD: ${lowerThreshold} ***`, 'debug');
                
            } catch (e) {
                this.log(`*** LOWER THRESHOLD PARSE FAILED: ${e.message} ***`, 'warn');
            }
        }
        
        // Parse additional data for debugging
        if (pressureData.length >= 6) {
            const temperature = this.parseUInt16(pressureData.slice(4, 6));
            // this.log(`*** TEMPERATURE: ${temperature} ***`, 'debug');
        }
        
        if (pressureData.length >= 7) {
            const mouseState = pressureData[6];
            // this.log(`*** MOUSE STATE: ${mouseState} ***`, 'debug');
        }
        
        if (pressureData.length >= 9) {
            const duration = this.parseUInt16(pressureData.slice(7, 9));
            // this.log(`*** DURATION: ${duration} ***`, 'debug');
        }
        
        if (pressureData.length >= 13) {
            try {
                const meanPressureBytes = pressureData.slice(9, 13);
                const meanPressureHex = meanPressureBytes.map(b => b.toString(16).padStart(2, '0')).join(' ');
                // this.log(`*** MEAN PRESSURE BYTES (9-12): ${meanPressureHex} ***`, 'debug');
                
                const bytes = new Uint8Array(meanPressureBytes);
                const view = new DataView(bytes.buffer);
                const meanPressure = view.getFloat32(0, false); // Big-endian first
                // this.log(`*** PARSED MEAN PRESSURE: ${meanPressure} ***`, 'debug');
                
            } catch (e) {
                this.log(`*** MEAN PRESSURE PARSE FAILED: ${e.message} ***`, 'warn');
            }
        }
        
        // Check if values are reasonable (not corrupted)
        const isReasonable = (value) => {
            return !isNaN(value) && isFinite(value) && value >= 0 && value < 1000;
        };
        
        // If big-endian values are corrupted, try little-endian
        if (!isReasonable(pressureReading) || !isReasonable(upperThreshold) || !isReasonable(lowerThreshold)) {
            // this.log(`*** TRYING LITTLE-ENDIAN FLOAT PARSING ***`, 'debug');
            
            // Try little-endian for pressure reading
            if (pressureData.length >= 4) {
                const bytes = new Uint8Array(pressureData.slice(0, 4));
                const view = new DataView(bytes.buffer);
                pressureReading = view.getFloat32(0, true);
                // this.log(`*** LITTLE-ENDIAN PRESSURE READING: ${pressureReading} ***`, 'debug');
            }
            
            // Try little-endian for upper threshold
            if (pressureData.length >= 21) {
                const bytes = new Uint8Array(pressureData.slice(17, 21));
                const view = new DataView(bytes.buffer);
                upperThreshold = view.getFloat32(0, true);
                // this.log(`*** LITTLE-ENDIAN UPPER THRESHOLD: ${upperThreshold} ***`, 'debug');
            }
            
            // Try little-endian for lower threshold
            if (pressureData.length >= 25) {
                const bytes = new Uint8Array(pressureData.slice(21, 25));
                const view = new DataView(bytes.buffer);
                lowerThreshold = view.getFloat32(0, true);
                // this.log(`*** LITTLE-ENDIAN LOWER THRESHOLD: ${lowerThreshold} ***`, 'debug');
            }
        }
        
        if (isReasonable(pressureReading) && isReasonable(upperThreshold) && isReasonable(lowerThreshold)) {
            // this.log(`*** PRESSURE DATA VALID: Reading=${pressureReading}, Upper=${upperThreshold}, Lower=${lowerThreshold} ***`, 'info');
            
            // Update pressure display
            this.updatePressureDisplay(pressureReading, upperThreshold, lowerThreshold);
            
            // Add to pressure history for charting
            // Temporarily disabled for performance testing
            /*
            if (this.pressureHistory.length >= this.maxPressureHistoryLength) {
                this.pressureHistory.shift(); // Remove oldest entry
            }
            this.pressureHistory.push({
                time: Date.now(),
                pressure: pressureReading,
                upper: upperThreshold,
                lower: lowerThreshold
            });
            */
            
            // Update pressure chart
            // this.updatePressureChart(); // Temporarily disabled for performance testing
            
        } else {
            // this.log(`*** PRESSURE DATA CORRUPTED: Reading=${pressureReading}, Upper=${upperThreshold}, Lower=${lowerThreshold} ***`, 'warn');
        }
    }
    
    initializePressureChart() {
        if (!this.pressureChart) return;
        
        this.chartCtx = this.pressureChart.getContext('2d');
        this.chartCtx.fillStyle = '#f8f9fa';
        this.chartCtx.fillRect(0, 0, this.pressureChart.width, this.pressureChart.height);
        
        // Draw initial grid
        this.drawChartGrid();
    }
    
    drawChartGrid() {
        if (!this.chartCtx) return;
        
        const ctx = this.chartCtx;
        const width = this.pressureChart.width;
        const height = this.pressureChart.height;
        
        // Clear canvas
        ctx.fillStyle = '#f8f9fa';
        ctx.fillRect(0, 0, width, height);
        
        // Draw grid lines
        ctx.strokeStyle = '#e9ecef';
        ctx.lineWidth = 1;
        
        // Vertical lines (time)
        for (let i = 0; i <= 10; i++) {
            const x = (width / 10) * i;
            ctx.beginPath();
            ctx.moveTo(x, 0);
            ctx.lineTo(x, height);
            ctx.stroke();
        }
        
        // Horizontal lines (pressure)
        for (let i = 0; i <= 5; i++) {
            const y = (height / 5) * i;
            ctx.beginPath();
            ctx.moveTo(0, y);
            ctx.lineTo(width, y);
            ctx.stroke();
        }
    }
    
    updatePressureChart() {
        if (!this.chartCtx || this.pressureHistory.length === 0) return;
        
        const ctx = this.chartCtx;
        const width = this.pressureChart.width;
        const height = this.pressureChart.height;
        
        // Redraw grid
        this.drawChartGrid();
        
        // Get pressure data for charting
        const pressures = this.pressureHistory.map(p => p.pressure);
        const maxPressure = Math.max(...pressures);
        const minPressure = Math.min(...pressures);
        const pressureRange = maxPressure - minPressure || 1000; // Default range if all values are same
        
        // Draw pressure line
        ctx.strokeStyle = '#007bff';
        ctx.lineWidth = 2;
        ctx.beginPath();
        
        this.pressureHistory.forEach((entry, index) => {
            const x = (index / (this.pressureHistory.length - 1)) * width;
            const normalizedPressure = (entry.pressure - minPressure) / pressureRange;
            const y = height - (normalizedPressure * height);
            
            if (index === 0) {
                ctx.moveTo(x, y);
            } else {
                ctx.lineTo(x, y);
            }
        });
        
        ctx.stroke();
        
        // Draw current point
        if (this.pressureHistory.length > 0) {
            const latest = this.pressureHistory[this.pressureHistory.length - 1];
            const x = width - 10; // Right side
            const normalizedPressure = (latest.pressure - minPressure) / pressureRange;
            const y = height - (normalizedPressure * height);
            
            ctx.fillStyle = '#dc3545';
            ctx.beginPath();
            ctx.arc(x, y, 4, 0, 2 * Math.PI);
            ctx.fill();
        }
    }
    
    updatePressureDisplay(pressureReading, upperThreshold, lowerThreshold) {
        // Update pressure display elements if they exist
        if (this.minPressure && this.maxPressure) {
            this.minPressure.textContent = `Min: ${Math.min(pressureReading, upperThreshold, lowerThreshold).toFixed(2)} hPa`;
            this.maxPressure.textContent = `Max: ${Math.max(pressureReading, upperThreshold, lowerThreshold).toFixed(2)} hPa`;
        }
        
        // Update chart display elements
        if (this.currentPressure) {
            this.currentPressure.textContent = pressureReading.toFixed(2);
        }
        
        if (this.pressureHistory.length > 0) {
            const pressures = this.pressureHistory.map(p => p.pressure);
            const avgPressure = pressures.reduce((a, b) => a + b, 0) / pressures.length;
            const peakPressure = Math.max(...pressures);
            
            if (this.avgPressure) {
                this.avgPressure.textContent = avgPressure.toFixed(2);
            }
            if (this.peakPressure) {
                this.peakPressure.textContent = peakPressure.toFixed(2);
            }
        }
        
        // Update pressure chart
        this.updatePressureChart();
        
        // Log pressure statistics
        if (this.pressureHistory.length > 0) {
            const pressures = this.pressureHistory.map(p => p.pressure);
            const avgPressure = pressures.reduce((a, b) => a + b, 0) / pressures.length;
            const minPressure = Math.min(...pressures);
            const maxPressure = Math.max(...pressures);
            
            this.log(`*** PRESSURE STATS: Avg=${avgPressure.toFixed(2)}, Min=${minPressure.toFixed(2)}, Max=${maxPressure.toFixed(2)} hPa ***`, 'debug');
        }
    }
    
    processPositionData(positionData) {
        // Position data (4 bytes): x and y coordinates (16-bit each)
        let x = 0, y = 0;
        
        try {
            if (positionData.length >= 2) {
                x = (positionData[1] << 8) | positionData[0]; // Little-endian
            }
            if (positionData.length >= 4) {
                y = (positionData[3] << 8) | positionData[2]; // Little-endian
            }
            
            this.log(`*** POSITION: X=${x}, Y=${y} (${positionData.length} bytes) ***`, 'info');
        } catch (error) {
            this.log(`*** POSITION PARSE ERROR: ${error.message} ***`, 'error');
        }
    }
    
    parseUInt16(bytes) {
        // Parse 2 bytes as little-endian uint16 (like Mac app formatUInt16)
        if (bytes.length !== 2) {
            throw new Error(`Expected 2 bytes, got ${bytes.length}`);
        }
        return (bytes[1] << 8) | bytes[0]; // Little-endian
    }
    
    parseFloat32(bytes) {
        // Parse 4 bytes as little-endian float32
        if (bytes.length !== 4) {
            throw new Error(`Expected 4 bytes, got ${bytes.length}`);
        }
        
        const buffer = new ArrayBuffer(4);
        const view = new DataView(buffer);
        for (let i = 0; i < 4; i++) {
            view.setUint8(i, bytes[i]);
        }
        const result = view.getFloat32(0, true); // true = little-endian
        
        // Sanity check for corrupted float values
        if (isNaN(result) || !isFinite(result)) {
            throw new Error(`Invalid float value: ${result}`);
        }
        
        return result;
    }
    
    updateGridFromCapacitive(capacitance) {
        // Map 29 capacitive values to 6x8 grid using Mac app pattern
        // The Mac app uses 48 cells, but we have 29, so we'll map what we have
        const grid = [
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
        ];
        
        // Map the 29 values to the grid using a pattern similar to Mac app
        // Start from the bottom row (like Mac app) and work up
        let cellIndex = 0;
        
        // Bottom row (row 5): map cells 0-5 to positions 1-6
        for (let col = 1; col <= 6; col++) {
            if (cellIndex < capacitance.length) {
                grid[5][col] = capacitance[cellIndex++];
            }
        }
        
        // Middle rows (rows 1-4): map 6 cells each
        for (let row = 1; row <= 4; row++) {
            for (let col = 0; col < 8; col++) {
                if (cellIndex < capacitance.length) {
                    grid[row][col] = capacitance[cellIndex++];
                }
            }
        }
        
        // Top row (row 0): map remaining cells to positions 1-6
        for (let col = 1; col <= 6; col++) {
            if (cellIndex < capacitance.length) {
                grid[0][col] = capacitance[cellIndex++];
            }
        }
        
        // Find the maximum value for scaling
        const maxValue = Math.max(...capacitance);
        
        // Update the visual grid with better scaling
        this.updateGridVisualWithScaling(grid, maxValue);
    }
    
    updateGridFromCompleteCapacitive(capacitance) {
        // Map 48 capacitive values to 6x8 grid using Mac app pattern
        // This is the complete dataset with all 48 cells
        const grid = [
            [0, capacitance[38], capacitance[39], capacitance[40], capacitance[41], capacitance[42], capacitance[43], 0],
            [capacitance[30], capacitance[31], capacitance[32], capacitance[33], capacitance[34], capacitance[35], capacitance[36], capacitance[37]],
            [capacitance[22], capacitance[23], capacitance[24], capacitance[25], capacitance[26], capacitance[27], capacitance[28], capacitance[29]],
            [capacitance[14], capacitance[15], capacitance[16], capacitance[17], capacitance[18], capacitance[19], capacitance[20], capacitance[21]],
            [capacitance[6], capacitance[7], capacitance[8], capacitance[9], capacitance[10], capacitance[11], capacitance[12], capacitance[13]],
            [0, capacitance[0], capacitance[1], capacitance[2], capacitance[3], capacitance[4], capacitance[5], 0],
        ];
        
        // Find the maximum value for scaling
        const maxValue = Math.max(...capacitance);
        
        // Update the visual grid with better scaling
        this.updateGridVisualWithScaling(grid, maxValue);
    }
    
    updateGridFromCompleteCapacitive44(capacitance) {
        // Reverse the array to match Mac app orientation
        const reversedCapacitance = [...capacitance].reverse();
        
        // Simple: just map the first 48 values to the 6x8 grid
        let activeCells = 0;
        let invalidCells = 0;
        
        for (let i = 0; i < Math.min(reversedCapacitance.length, 48); i++) {
            const row = Math.floor(i / 8);
            const col = i % 8;
            const cell = document.querySelector(`[data-row="${row}"][data-col="${col}"]`);
            if (cell) {
                let value = reversedCapacitance[i] || 0;
                
                // Validate the value - if it's too high, it's probably invalid data
                if (value > 10000 || isNaN(value) || !isFinite(value)) {
                    this.log(`*** INVALID CELL VALUE: cell ${i} = ${reversedCapacitance[i]}, setting to 0 ***`, 'warn');
                    value = 0;
                    invalidCells++;
                }
                
                // Use golden gradient visualization
                const intensity = Math.min(1, Math.max(0, value / 1000)); // Normalize to 0-1 range
                
                // Interpolate from transparent to golden (#dca32f)
                const goldenR = 220; // 0xdc
                const goldenG = 163; // 0xa3
                const goldenB = 47;  // 0x2f
                
                const r = Math.round(goldenR * intensity);
                const g = Math.round(goldenG * intensity);
                const b = Math.round(goldenB * intensity);
                const a = intensity;
                
                cell.style.backgroundColor = `rgba(${r}, ${g}, ${b}, ${a})`;
                cell.style.opacity = '1';
                cell.textContent = value.toString(); // Update the text content
                
                if (value > 0) {
                    activeCells++;
                }
            }
        }
        
        this.log(`*** GRID UPDATED: ${reversedCapacitance.length} values, ${activeCells} active cells, ${invalidCells} invalid cells ***`, 'info');
    }
    
    updateGridFromBLECapacitive(capacitance) {
        // Map 29 cells to a 6x8 grid (48 cells total)
        // We'll need to interpolate or map the 29 values to 48 grid positions
        const grid = [
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
            [0, 0, 0, 0, 0, 0, 0, 0],
        ];
        
        // Find the maximum value for scaling
        const maxValue = Math.max(...capacitance);
        this.log(`*** GRID UPDATE: Max value = ${maxValue} ***`, 'info');
        
        // Simple mapping: distribute the 29 values across the grid
        for (let i = 0; i < Math.min(capacitance.length, 48); i++) {
            const row = Math.floor(i / 8);
            const col = i % 8;
            if (row < 6 && col < 8) {
                grid[row][col] = capacitance[i] || 0;
            }
        }
        
        // Update the visual grid with better scaling
        this.updateGridVisualWithScaling(grid, maxValue);
    }
    
    updateGridVisual(grid) {
        // Update the visual grid with capacitive touch data using golden gradient
        for (let row = 0; row < 6; row++) {
            for (let col = 0; col < 8; col++) {
                const cell = document.querySelector(`[data-row="${row}"][data-col="${col}"]`);
                if (cell) {
                    const value = grid[row][col];
                    const intensity = Math.min(1, Math.max(0, value / 1000)); // Normalize to 0-1 range
                    
                    // Interpolate from transparent to golden (#dca32f)
                    const goldenR = 220; // 0xdc
                    const goldenG = 163; // 0xa3
                    const goldenB = 47;  // 0x2f
                    
                    const r = Math.round(goldenR * intensity);
                    const g = Math.round(goldenG * intensity);
                    const b = Math.round(goldenB * intensity);
                    const a = intensity;
                    
                    cell.style.backgroundColor = `rgba(${r}, ${g}, ${b}, ${a})`;
                    cell.style.opacity = '1';
                    cell.textContent = value.toString(); // Update the text content
                }
            }
        }
    }
    
    updateGridVisualWithScaling(grid, maxValue) {
        // Update the visual grid with improved contrast and gradient calculation
        let activeCells = 0;
        let maxCellValue = 0;
        
        // Corner positions that should be excluded from gradient calculation
        const cornerPositions = [
            {row: 0, col: 0}, {row: 0, col: 7}, 
            {row: 5, col: 0}, {row: 5, col: 7}
        ];
        
        // Calculate max value excluding corner zeros for better gradient
        let maxValueForGradient = 0;
        for (let row = 0; row < 6; row++) {
            for (let col = 0; col < 8; col++) {
                const value = grid[row][col];
                const isCorner = cornerPositions.some(pos => pos.row === row && pos.col === col);
                
                // Only include non-corner values in gradient calculation
                if (value > 0 && !isCorner) {
                    maxValueForGradient = Math.max(maxValueForGradient, value);
                }
            }
        }
        
        this.log(`*** GRID UPDATE: maxValue=${maxValue}, maxValueForGradient=${maxValueForGradient} ***`, 'debug');
        
        for (let row = 0; row < 6; row++) {
            for (let col = 0; col < 8; col++) {
                const cell = document.querySelector(`[data-row="${row}"][data-col="${col}"]`);
                if (cell) {
                    const value = grid[row][col];
                    const isCorner = cornerPositions.some(pos => pos.row === row && pos.col === col);
                    
                    // Gradient-based visualization from transparent to golden
                    if (value === 0 || isCorner) {
                        // Corner cells or zero values - transparent
                        cell.style.backgroundColor = 'transparent';
                        cell.style.opacity = '0.3';
                    } else if (value > 0) {
                        // Active sensor cells - use gradient from transparent to golden
                        const normalizedValue = maxValueForGradient > 0 ? value / maxValueForGradient : 0;
                        const intensity = Math.min(1, Math.max(0, normalizedValue));
                        
                        // Interpolate from transparent (0,0,0,0) to golden (#dca32f)
                        const goldenR = 220; // 0xdc
                        const goldenG = 163; // 0xa3
                        const goldenB = 47;  // 0x2f
                        
                        const r = Math.round(goldenR * intensity);
                        const g = Math.round(goldenG * intensity);
                        const b = Math.round(goldenB * intensity);
                        const a = intensity;
                        
                        cell.style.backgroundColor = `rgba(${r}, ${g}, ${b}, ${a})`;
                        cell.style.opacity = '1'; // Keep opacity at 1 since we're using rgba alpha
                        activeCells++;
                        if (value > maxCellValue) maxCellValue = value;
                    } else {
                        // Inactive cells - very transparent
                        cell.style.backgroundColor = 'transparent';
                        cell.style.opacity = '0.1';
                    }
                    cell.textContent = value.toString();
                    
                    // Remove any test borders
                    cell.style.border = '';
                }
            }
        }
        
        // Log summary
        // Log summary - commented out for performance
        /*
        if (activeCells > 0) {
            this.log(`*** GRID UPDATE: ${activeCells} active cells, max cell value=${maxCellValue} ***`, 'info');
        }
        */
        
        // Update touch detection for adaptive timeout
        if (activeCells > 0) {
            this.lastTouchTime = Date.now();
        }
    }

    processBLESensorData(sensorData, packetType, flags) {
        // Parse exactly like Mac app: processSensorData function
        this.log(`*** BLE SENSOR DATA: ${sensorData.length} bytes ***`, 'info');
        
        // Mac app expects minimum 99 bytes
        if (sensorData.length >= 99) {
            this.log(`*** PARSING AS MOUTHPAD SENSOR DATA (Mac app format) ***`, 'info');
            
            // Try different header alignments to find the correct one
            this.tryDifferentAlignments(sensorData);
            
            // Parse packet index (first 2 bytes, little endian) - like Mac app
            const packetIndex = this.parseUInt16(sensorData.slice(0, 2));
            this.log(`*** PACKET INDEX: ${packetIndex} ***`, 'info');
            
            // Determine capacitive data range based on packet size (like Mac app)
            let capTouchStartIdx = 20;
            let capTouchEndIdx = 107;
            if (sensorData.length < 131) {
                capTouchStartIdx = 20;
                capTouchEndIdx = 99;
            }
            
            // Parse capacitive data (like Mac app capacitanceData function)
            // Note: slice(end) is exclusive, so we don't need +1
            const capacitiveData = sensorData.slice(capTouchStartIdx, capTouchEndIdx);
            this.log(`*** CAPACITIVE DATA: ${capacitiveData.length} bytes (indices ${capTouchStartIdx}-${capTouchEndIdx}) ***`, 'info');
            
            // Debug: Show the exact extraction details
            this.log(`*** EXTRACTION DETAILS: sensorData=${sensorData.length} bytes, extracting ${capTouchEndIdx - capTouchStartIdx} bytes ***`, 'debug');
            
            // Debug: Show the data around the capacitive area
            const debugStart = Math.max(0, capTouchStartIdx - 4);
            const debugEnd = Math.min(sensorData.length, capTouchEndIdx + 8);
            const debugData = sensorData.slice(debugStart, debugEnd);
            const debugHex = debugData.map(b => b.toString(16).padStart(2, '0')).join(' ');
            this.log(`*** DEBUG AREA (${debugStart}-${debugEnd}): ${debugHex} ***`, 'debug');
            
            // Try to find the best alignment for capacitive data
            const bestAlignment = this.findBestCapacitiveAlignment(sensorData);
            if (bestAlignment) {
                this.log(`*** USING BEST ALIGNMENT: start=${bestAlignment.start}, length=${bestAlignment.length} ***`, 'info');
                const alignedCapacitiveData = sensorData.slice(bestAlignment.start, bestAlignment.start + bestAlignment.length);
                this.processCapacitiveData(alignedCapacitiveData);
            } else if (capacitiveData.length >= 80) {
                this.processCapacitiveData(capacitiveData);
            } else {
                this.log(`*** INCOMPLETE CAPACITIVE DATA: ${capacitiveData.length} bytes ***`, 'warn');
            }
            
            // Parse pressure data (24 bytes after capacitive data) - like Mac app pressureData function
            const pressureStartIdx = capTouchEndIdx + 1;
            const pressureEndIdx = pressureStartIdx + 24;
            const pressureData = sensorData.slice(pressureStartIdx, pressureEndIdx);
            
            // Debug: Show pressure data extraction
            this.log(`*** PRESSURE EXTRACTION: sensorData=${sensorData.length} bytes, capTouchEndIdx=${capTouchEndIdx}, pressureStartIdx=${pressureStartIdx}, pressureEndIdx=${pressureEndIdx} ***`, 'debug');
            
            if (pressureData.length === 24) {
                this.log(`*** PRESSURE DATA: ${pressureData.length} bytes (indices ${pressureStartIdx}-${pressureEndIdx}) ***`, 'info');
                
                // Debug: Show pressure data hex
                const pressureHex = pressureData.map(b => b.toString(16).padStart(2, '0')).join(' ');
                this.log(`*** PRESSURE DATA HEX: ${pressureHex} ***`, 'debug');
                
                this.processPressureData(pressureData, packetIndex);
            } else {
                this.log(`*** INCOMPLETE PRESSURE DATA: ${pressureData.length} bytes (expected 24) ***`, 'warn');
                // Try to process partial pressure data if we have at least 12 bytes
                if (pressureData.length >= 12) {
                    this.log(`*** ATTEMPTING PARTIAL PRESSURE PROCESSING ***`, 'info');
                    this.processPressureData(pressureData, packetIndex);
                }
            }
            
            // Parse position data (4 bytes after pressure data) - like Mac app
            const positionIdx = pressureEndIdx;
            const positionData = sensorData.slice(positionIdx, positionIdx + 4);
            
            if (positionData.length === 4) {
                this.log(`*** POSITION DATA: ${positionData.length} bytes (indices ${positionIdx}-${positionIdx + 3}) ***`, 'info');
                this.processPositionData(positionData);
            } else {
                this.log(`*** INCOMPLETE POSITION DATA: ${positionData.length} bytes (expected 4) ***`, 'warn');
                // Try to process partial position data if we have at least 2 bytes
                if (positionData.length >= 2) {
                    this.log(`*** ATTEMPTING PARTIAL POSITION PROCESSING ***`, 'info');
                    this.processPositionData(positionData);
                }
            }
            
        } else {
            this.log(`*** SENSOR DATA TOO SMALL: ${sensorData.length} bytes (need >= 99) ***`, 'warn');
        }
    }

    findBestCapacitiveAlignment(sensorData) {
        // Try different alignments and lengths to find the best capacitive data
        const alignments = [0, 1, 2, 4, 6, 8, 10, 12, 16, 20];
        const lengths = [80, 82, 84, 86, 88, 90, 92, 94, 96];
        
        let bestScore = 0;
        let bestAlignment = null;
        
        for (let start of alignments) {
            for (let length of lengths) {
                if (start + length <= sensorData.length) {
                    const testData = sensorData.slice(start, start + length);
                    const testCapacitance = [];
                    
                    // Try to parse as capacitive data
                    for (let idx = 0; idx < length / 2; idx++) {
                        const lowByte = testData[2 * idx];
                        const highByte = testData[2 * idx + 1];
                        const value = (highByte << 8) | lowByte;
                        testCapacitance[idx] = value;
                    }
                    
                    // Score this alignment based on data quality
                    const validValues = testCapacitance.filter(v => v > 0 && v < 10000);
                    const invalidValues = testCapacitance.filter(v => v >= 10000 || isNaN(v) || !isFinite(v));
                    
                    if (validValues.length > 0) {
                        const avgValue = validValues.reduce((a, b) => a + b, 0) / validValues.length;
                        const score = validValues.length - (invalidValues.length * 2) - Math.abs(avgValue - 800) / 100;
                        
                        if (score > bestScore && validValues.length > 10 && avgValue > 100 && avgValue < 2000) {
                            bestScore = score;
                            bestAlignment = { start, length, score, avgValue, validCount: validValues.length };
                        }
                    }
                }
            }
        }
        
        if (bestAlignment) {
            this.log(`*** BEST ALIGNMENT FOUND: start=${bestAlignment.start}, length=${bestAlignment.length}, score=${bestAlignment.score.toFixed(1)}, avg=${bestAlignment.avgValue.toFixed(0)}, valid=${bestAlignment.validCount} ***`, 'info');
        }
        
        return bestAlignment;
    }
    
    tryDifferentAlignments(sensorData) {
        // Try different header alignments to find the correct capacitive data start
        const alignments = [0, 1, 2, 4, 6, 8, 10, 12, 16, 20];
        
        for (let offset of alignments) {
            if (offset + 88 <= sensorData.length) {
                const testData = sensorData.slice(offset, offset + 88);
                const testCapacitance = [];
                
                // Try to parse as capacitive data
                for (let idx = 0; idx < 44; idx++) {
                    const lowByte = testData[2 * idx];
                    const highByte = testData[2 * idx + 1];
                    const value = (highByte << 8) | lowByte;
                    testCapacitance[idx] = value;
                }
                
                // Check if this looks like valid capacitive data
                const validValues = testCapacitance.filter(v => v > 0 && v < 10000);
                const avgValue = validValues.length > 0 ? validValues.reduce((a, b) => a + b, 0) / validValues.length : 0;
                
                if (validValues.length > 10 && avgValue > 100 && avgValue < 2000) {
                    this.log(`*** POTENTIAL ALIGNMENT: offset=${offset}, valid=${validValues.length}, avg=${avgValue.toFixed(0)} ***`, 'info');
                }
            }
        }
        
        // Also try different data lengths to see if we're missing bytes
        const testLengths = [80, 82, 84, 86, 88, 90, 92, 94, 96];
        for (let length of testLengths) {
            if (20 + length <= sensorData.length) {
                const testData = sensorData.slice(20, 20 + length);
                const testCapacitance = [];
                
                // Try to parse as capacitive data
                for (let idx = 0; idx < length / 2; idx++) {
                    const lowByte = testData[2 * idx];
                    const highByte = testData[2 * idx + 1];
                    const value = (highByte << 8) | lowByte;
                    testCapacitance[idx] = value;
                }
                
                // Check if this looks like valid capacitive data
                const validValues = testCapacitance.filter(v => v > 0 && v < 10000);
                const avgValue = validValues.length > 0 ? validValues.reduce((a, b) => a + b, 0) / validValues.length : 0;
                
                if (validValues.length > 10 && avgValue > 100 && avgValue < 2000) {
                    this.log(`*** POTENTIAL LENGTH: length=${length}, cells=${length/2}, valid=${validValues.length}, avg=${avgValue.toFixed(0)} ***`, 'info');
                }
            }
        }
    }

    processPowerData(powerData, packetType, flags) {
        // Process 5 bytes of power data from PWR stream
        if (powerData.length < 5) {
            this.log(`*** POWER DATA TOO SMALL: Expected 5 bytes, got ${powerData.length} ***`, 'warn');
            return;
        }
        
        // Parse power data (5 bytes)
        // Format might be: battery level, voltage, etc.
        const batteryLevel = powerData[0];  // 0-100%
        const voltage = (powerData[2] << 8) | powerData[1];  // 16-bit voltage
        const temperature = powerData[3];   // Temperature
        const status = powerData[4];        // Status byte
        
        this.log(`*** POWER: Battery=${batteryLevel}%, Voltage=${voltage}mV, Temp=${temperature}°C, Status=0x${status.toString(16)} ***`, 'info');
    }

    toggleLogView() {
        this.logViewMode = this.logViewMode === 'details' ? 'raw' : 'details';
        
        // Update button text and styling
        if (this.logViewMode === 'raw') {
            this.logViewToggleBtn.textContent = '🔍 Raw';
            this.logViewToggleBtn.classList.add('raw-mode');
            // Clear terminal for clean raw view
            this.terminal.innerHTML = '<div class="terminal-line">Raw HEX view - waiting for data...</div>';
        } else {
            this.logViewToggleBtn.textContent = '📊 Details';
            this.logViewToggleBtn.classList.remove('raw-mode');
            this.log('Switched to DETAILED view - showing parsed data', 'info');
        }
    }
}

// Initialize the controller when the page loads
document.addEventListener('DOMContentLoaded', () => {
    window.mouthpadController = new MouthPadController();
});