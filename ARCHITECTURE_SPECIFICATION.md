```mermaid
flowchart LR

subgraph STM32F469[STM32F469 (Host)]
direction TB
A0[Bootloader wakes / App running]
A1[Compare firmware version (.fw_metadata)]
A2[Write update flag (0x0A) to Sect 13]
A3[Reboot + Bootloader selects new bank]

A4[Request firmware byte (0x28)]
A5[Parse stream FSM]
A6[Flash erase + programming]
A7[Validate CRC + magic + version]
A8[Finalize + reset system]
end

subgraph ESP32[ESP32 (Gateway)]
direction TB
B0[Wi-Fi + Manifest server]
B1[Serve version (v1.2)]
B2[Wait for reboot signal]
B3[Serve firmware binary via HTTPS]
B4[Send chunks + headers + CRC]
B5[Send remaining tail bytes]
B6[Send final ACK]
end

%% Phase 1 (version check)
A1 -->|1. Request Version| B1
B1 -->|2. Server Version| A2

%% Phase 2 (update scheduling)
A2 -.->|3. Update pending| B2

%% Phase 3 (bootloader request)
A3 --> A4
A4 -->|4. Request FW byte| B3

%% Phase 4 (streaming)
B3 --> B4
B4 -->|5. Frame TX| A5
A5 --> A6
A6 -->|6. WAIT_BYTE| B4
A6 -->|7. ACK| B4

%% Phase 5 (finalization)
B4 --> B5
B5 -->|8. Tail bytes| A7
A7 --> B6
B6 -->|9. Final ACK| A8
```