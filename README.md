# Oratio

Oratio is an off-grid, mesh-based emergency telecommunications system.
Computer engineering capstone by Adam Dia, Yadah Ngolo, Fayza Oyarekhua, Mohamed Boustta and Omon Okondo.

The system comprises of wearable nodes participating in a wireless mesh, a Raspberry Pi hosting a UDP server for forwarding and a dashboard.

## Organization

- `fw` contains all of our firmware
- `hw` contains PCB schematics and enclosure
- `sw` contains the UDP server and dashboard


## Versions

| Component | Version |
| --- | --- |
| PlatformIO Core | 6.1.19 |
| Pioarduino platform | 54.03.21-2 |
| ESP-IDF | 5.4.2 |
| ESP-Mesh-Lite | 1.0.2 |
| IoT-Bridge | 1.0.1 |

## Local firmware builds

Use PlatformIO Core 6.1.19 to match CI. From the repository root in PowerShell:

```powershell
python -m venv fw/.venv-pio
& fw/.venv-pio/Scripts/python.exe -m pip install platformio==6.1.19
& fw/.venv-pio/Scripts/Activate.ps1
pio --version
cd fw
```

If the virtual environment already exists, reuse it and install the pinned version
there. PlatformIO downloads its platform/tool packages automatically into the normal
user .platformio directory. That directory does not select the pio executable version.
Generated sdkconfig files and the virtual environment are local, ignored build state.

Build the S3 root and node firmware:

```powershell
pio run -e root -e node
```

Build and link a test suite without uploading or running it:

```powershell
pio test -e root_test -f test_155_protocol --without-uploading --without-testing
```

The same build-only command supports test_102_mesh, test_106_udp,
test_109_heartbeat, and test_113_relay. Hardware tests require the appropriate board,
serial port, network settings, and gateway setup. Successful build-only checks are
not hardware test passes.

### Board-specific flash defaults

Shared network settings live in fw/sdkconfig.defaults. ESP-IDF automatically adds
fw/sdkconfig.defaults.esp32s3 for the S3: 8 MB flash and the console on the S3's
native USB port (plug into the USB port, not the UART one, to see logs). These small defaults files
belong in version control; generated per-environment sdkconfig files do not.
Existing generated configurations retain their saved settings, so check/regenerate
local configuration when changing board assumptions; preserve any local credentials.
