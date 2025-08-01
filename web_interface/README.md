# MouthPad Web Controller

A dark-themed web interface for controlling and monitoring the MouthPad device through a USB serial connection.

## Features

- **Dark Theme**: Modern dark interface with blue accents
- **Serial Communication**: Direct connection to USB CDC serial port
- **Command Buttons**: Quick access to common MouthPad commands
- **Custom Commands**: Send any custom command via text input
- **Real-time Grid Display**: Visual 8x6 pressure grid for JCP data
- **Terminal Log**: Real-time data logging with export functionality
- **Responsive Design**: Works on desktop and mobile devices

## Commands

### Predefined Commands
- **StartStream jcp**: Start Joint Capsule Pressure streaming (8x6 grid data)
- **StartStream imu**: Start IMU sensor streaming
- **StartStream click**: Start click detection streaming
- **StopStream**: Stop all streaming

### Custom Commands
- Type any command in the custom command input
- Press Enter or click Send to execute

## Usage

1. **Connect to Serial Port**:
   - Click "Connect to Serial Port"
   - Select your MouthPad USB CDC device
   - The status indicator will turn green when connected

2. **Send Commands**:
   - Use the predefined command buttons for common operations
   - Or type custom commands in the input field

3. **View Data**:
   - **JCP Data**: Visual 8x6 grid shows pressure values
   - **Terminal Log**: Real-time data logging
   - **Statistics**: Min/Max pressure and active cell count

4. **Export Data**:
   - Click "Export Log" to download the terminal log as a text file

## Data Format

### JCP (Joint Capsule Pressure) Data
- **Packet Header**: 0x64 (4 bytes)
- **Sensor Data**: 48 bytes (8x6 grid)
- **Grid Layout**: 8 columns × 6 rows
- **Pressure Values**: 0-255 range

### Grid Display
- **Low Pressure** (1-100): Green
- **Medium Pressure** (101-200): Orange
- **High Pressure** (201-255): Red
- **Inactive** (0): Dark gray

## Browser Requirements

- **Chrome/Edge**: Full Web Serial API support
- **Firefox**: Limited support (may need flags)
- **Safari**: No Web Serial API support

## Troubleshooting

1. **Connection Issues**:
   - Ensure MouthPad is connected via USB
   - Check that the device appears as a serial port
   - Try refreshing the page

2. **No Data Display**:
   - Verify you've sent "StartStream jcp" command
   - Check the terminal log for received data
   - Ensure the MouthPad is streaming data

3. **Grid Not Updating**:
   - Check that JCP packets are being received
   - Verify the data format matches expected structure
   - Look for error messages in the terminal log

## Development

The web interface uses:
- **HTML5**: Structure and layout
- **CSS3**: Dark theme and responsive design
- **JavaScript**: Serial communication and data parsing
- **Web Serial API**: Browser-based serial port access

## File Structure

```
web_interface/
├── index.html      # Main HTML file
├── styles.css      # Dark theme CSS
├── script.js       # JavaScript controller
└── README.md       # This file
```

## License

This project is part of the MouthPad USB bridge development. 