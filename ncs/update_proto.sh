#!/bin/bash
# Script to update MouthpadUsb → MouthpadRelay in main.c

cd /Users/robert/git/augmental/mouthpad-usb/ncs/app/src

# Backup the file
cp main.c main.c.bak

# Update message types
sed -i '' 's/mouthware_message_MouthpadAppToUsbMessage/mouthware_message_AppToRelayMessage/g' main.c
sed -i '' 's/mouthware_message_UsbDongleToMouthpadAppMessage/mouthware_message_RelayToAppMessage/g' main.c

# Update destination enum
sed -i '' 's/mouthware_message_MouthpadAppToUsbMessageDestination_MOUTHPAD_USB_MESSAGE_DESTINATION_DONGLE/mouthware_message_AppToRelayMessageDestination_APP_RELAY_MESSAGE_DESTINATION_RELAY/g' main.c
sed -i '' 's/mouthware_message_MouthpadAppToUsbMessageDestination_MOUTHPAD_USB_MESSAGE_DESTINATION_MOUTHPAD/mouthware_message_AppToRelayMessageDestination_APP_RELAY_MESSAGE_DESTINATION_MOUTHPAD/g' main.c

# Update connection status enum
sed -i '' 's/mouthware_message_UsbDongleConnectionStatus_USB_DONGLE_CONNECTION_STATUS_/mouthware_message_RelayBleConnectionStatus_RELAY_CONNECTION_STATUS_/g' main.c

# Update message tags
sed -i '' 's/_connection_status_request_tag/_ble_connection_status_read_tag/g' main.c
sed -i '' 's/_connection_status_response_tag/_ble_connection_status_response_tag/g' main.c
sed -i '' 's/_pass_through_to_mouthpad_request_tag/_pass_through_to_mouthpad_tag/g' main.c
sed -i '' 's/_rssi_request_tag/_rssi_request_tag/g' main.c
sed -i '' 's/_rssi_status_response_tag/_rssi_status_response_tag/g' main.c

# Update struct field names
sed -i '' 's/\.connection_status_request/\.ble_connection_status_read/g' main.c
sed -i '' 's/\.connection_status_response/\.ble_connection_status_response/g' main.c
sed -i '' 's/\.pass_through_to_mouthpad_request/\.pass_through_to_mouthpad/g' main.c
sed -i '' 's/\.rssi_status_response/\.rssi_status_response/g' main.c

echo "Updated main.c with MouthpadRelay types"
