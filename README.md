# Next Train Info Screen

LED Matrix Display controlled by an ESP32 to display the next train at a given station.

Python files are for testing API Calls and proof of concept.

The CPP Sketch when loaded onto an ESP32 Dev Board connected to a HUB75 32x64 led Matrix Display will show the next 2 trains leaving the selected station.

Stations can be selected by visiting the IP address shown on the screen and choosing from the dropdown.

**Note**: The ESP32 must be connected to WiFi, change the SSID and Password in the sketch as required.

Thanks to [RJM](https://rjm.ie) for Irish Rail API Wrapper.
