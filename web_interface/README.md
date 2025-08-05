# MouthPad^USB Web Interface

A real-time web interface for monitoring and controlling the MouthPad^USB device.

## Features

### Real-time Monitoring
- **Touch Grid Display**: Visual representation of the 8x6 capacitive touch grid
- **Pressure Chart**: Real-time pressure data visualization
- **Serial Communication**: Direct USB serial connection to the device

### Log View Modes
- **📊 Details View**: Parsed and formatted log messages with packet analysis
- **🔍 Raw View**: Raw hexadecimal packet data for debugging

### Stream Control
- **JCP Stream**: Capacitive touch data streaming
- **IMU Stream**: Inertial measurement unit data
- **Click Stream**: Button click events
- **Power Stream**: Battery and power management data

## Usage

### Connection
1. Click "Connect Serial" to establish USB connection
2. Select the MouthPad^USB device when prompted
3. The status indicator will turn green when connected

### Log View Toggle
- Click the "📊 Details" / "🔍 Raw" button to switch between log modes
- **Details Mode**: Shows parsed packet information and sensor data
- **Raw Mode**: Shows raw hexadecimal data with packet size indicators
  - 📝 Small packets (< 32 bytes)
  - 📄 Medium packets (32-64 bytes)  
  - 📦 Large packets (> 64 bytes)

### Stream Control
- Use the "▶ StartStream" buttons to control data streams
- Active streams are highlighted in blue
- Streams automatically timeout after 15 seconds of inactivity

### Custom Commands
- Enter custom commands in the text field
- Press Enter or click "Send" to execute

### Log Management
- **Clear Log**: Clear the current log display
- **Export Log**: Download log as text file

## Technical Details

### Packet Format
The device sends framed packets with the following format:
- **New Format**: `[0xAA][0x55][LEN_H][LEN_L][DATA...][CRC_H][CRC_L]`
- **Old Format**: `[0xAA][LEN_L][LEN_H][DATA...][0x55]`

### Data Interpretation
- **JCP Packets**: 138+ bytes, capacitive touch sensor data
- **Power Packets**: 9 bytes, battery and power information
- **IMU Packets**: Inertial measurement data
- **Click Packets**: Button press events

### Browser Compatibility
- Requires Web Serial API support (Chrome, Edge, Opera)
- HTTPS required for Web Serial API access
- Local development server supported

## Development

### Local Testing
```bash
cd web_interface
python3 -m http.server 8000
# Open http://localhost:8000
```

### File Structure
- `index.html`: Main interface layout
- `script.js`: Core functionality and data processing
- `styles.css`: Dark theme styling
- `README.md`: This documentation 