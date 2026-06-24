### Project Architectural Overview: Dual-Bank Secure OTA Firmware Update System


This project implements a custom, bare-metal **Dual-Bank Over-The-Air (OTA) Firmware Update System** distributed across two microcontrollers: an **ESP32** (acting as the network gateway) and an **STM32F469** (acting as the main host/motherboard running the core application).

The philosophy of this architecture is **robustness and anti-brick safety**. By leveraging the hardware dual-bank memory topology of the STM32F469, the system guarantees that a valid, running application always exists on the chip. Even if power cuts out halfway through an OTA download, or if a user accidentally uploads a completely broken or corrupted binary, the system automatically falls back to the previous working version.


### 1. The Global Hardware Architecture

The system is divided into two clear processing layers connected via a physical asynchronous serial connection (UART):

`+-----------------------------------+             +-----------------------------------+`

`|              ESP32                |             |             STM32F469             |`

`|         (Network Gateway)         |   UART6     |        (Main Motherboard)         |`

`|  - Connects to Wi-Fi Secure AP    |============\>|  - Runs Custom Bare-Metal Boot    |`

`|  - Queries Version JSON Manifest  |  (Custom    |  - Manages Hardware Dual-Banks    |`

`|  - Downloads App Binary via HTTPS |  Protocol)  |  - Dynamically Erases & Flashes  |`

`|  - Segments Binary into 512B Chunks|             |  - Validates MD Anchors & CRCs    |`

`+-----------------------------------+             +-----------------------------------+`

1. **The ESP32 :** It does not know or care about the underlying physical hardware layout of the STM32 (such as flash sectors or memory addresses). Its job is entirely administrative: it connects to a secure Wi-Fi access point, securely pulls a JSON manifest and a firmware binary via HTTPS from a remote server, ensures packet delivery through a custom chunk-based packetizing protocol, and reports system health status.

2. **The STM32F469 :** It runs a highly optimized, custom bare-metal bootloader written in type-safe modern C++. It completely controls the non-volatile memory controller, manages safe page/sector erasures on the fly, enforces structural safeguards (such as protecting its own bootloader space and EEPROM config sector), and physically switches memory execution lanes via hardware bank swapping.  
Hardware recommendation for UART:  Pull-up mode for PC6 (Tx) in GPIO register doesn't solve the issue of parasitic/ultra-sensitive behavior (spurious bytes at startup). PC6 must be pulled high with an external resistor (22K -\> 100K).


### 2. Detailed Technical Breakthroughs

#### A. The Stream Parser & Chunk-Based Communication Protocol

To communicate safely over UART without dropping bytes or requiring massive, RAM-heavy local buffers, the system implements a strict **Finite State Machine (FSM) Stream Parser** (`ParseIncomingStream`) using a custom 16-byte fixed framing header coupled with a variable payload block up to 512 bytes.

`+-----------------------------------------------------------------------------------------------------+                                 						16-BYTE FIXED HEADER  
+----------------------+--------------------+-------------------+-------------------------------------+`

`|  Total Size (4B)     |   Packet ID (2B)   | Payload Length(2B)| Version (M/m)(4B)| CRC32 Total(4B)  | `

`+----------------------+--------------------+-------------------+-------------------------------------+                              					  	VARIABLE PAYLOAD +-----------------------------------------------------------------------------------------------------+`

`|                                 DATA BYTES (Max 512B)                              			  |`

`+-----------------------------------------------------------------------------------------------------+`

`|                                 4-BYTE TRAILING CRC32                          				  |`

`+-----------------------------------------------------------------------------------------------------+`

The parser processes data byte-by-byte through sequential states:

- `IDLE\_START`: Listens exclusively for a unique synchronization constant (`PACKET\_START\_BYTE`).

- `READ\_HEADER`: Accumulates exactly 16 bytes and decodes fields from Big-Endian (MSB first) networks into the processor's native architecture. It includes a *defensive hardware safeguard* that caps any inbound payload length strictly to 512 bytes to completely neutralize buffer overflow security attacks.

- `READ\_DATA`: Seamlessly routes incoming data directly into a local buffer until the exact `payload\_length` condition is satisfied.

- `READ\_CRC`: Gathers a 4-byte frame CRC, immediately invoking flash execution routines (`execute\_flash\_and\_respond`) if the packet passes evaluation.





#### B. The Dynamic Sector Erase Engine

Instead of blindly erasing the entire target application bank at the start of a download—which introduces an extended window of vulnerability if a network crash happens immediately—the STM32 bootloader features a specialized **Dynamic Sector Erase Engine**.

As each payload chunk arrives, the bootloader dynamically calculates the absolute physical boundary address in Flash memory where that specific block will terminate (`packet\_end\_address`). It then references internal, compiled lookup tables (`bank1\_sectors` and `bank2\_sectors`) in reverse order to deduce exactly which hardware sector corresponds to that end-boundary.

Physical flash erasure is executed *only* when the streaming address crosses a boundary into a new, un-erased physical sector block. This significantly shortens flash erase windows and keeps as much historical code intact as possible during live transitions.

To inform the ESP32 there will be a delay (minimum 350ms) due to the Flash erase time and to avoid unexpected behavior, the STM32 sends a WAIT\_BYTE allowing the ESP32 to enter a condition where an extended timeout on the uart\_read\_bytes function is set.


#### C. Comprehensive Multi-Layer Security & Structural Safeguards

The project implements four defensive perimeter gates before any newly downloaded code is permitted to run:

1. **Per-Packet CRC Check:** Every incoming 512-byte block is subjected to a local software CRC32 check. If a bit flips during the UART transmission, a `NAK\_CRC\_ERROR` is shot back to the ESP32, which immediately executes a seamless retry loop.

2. **Peripheral Register Locking:** The internal Flash controller registers are securely locked down at all times. They are explicitly opened (`flash\_unlock`) micro-moments before an erase/write sequence and immediately clamped shut (`flash\_lock`) the instant a operation completes.

3. **Hard Sector Exclusion Zones:** The erase engine contains strict programmatic boundaries. For Bank 1, it refuses to erase anything at or below Sector 1, completely shielding the bootloader itself. For Bank 2, it blocks modifications to Sectors 12 and 13, ensuring that the critical system EEPROM configuration sector can never be corrupted by an inbound application payload.

4. **Compile-Time Metadata Verification:** Even if 100% of the bytes arrive correctly, the bootloader will refuse to boot the code unless it discovers a precise `magic\_anchor` token (`"VERS"` / `0x56455253`) compiled into a fixed offset (`0x200`) explicitly set aside by the application's linker script. It then matches the compiled version numbers against the JSON cloud manifest attributes to stop corrupt or incompatible binary flashes.


#### D. The Volatile Restoration & Dual-Bank Boot Swap

The STM32F469 operates via a hardware steering map managed by the `SYSCFG-\>MEMRMP` register. When the `UFB\_MODE` configuration bit is cleared (`0`), Bank 1 maps to the boot address space `0x08000000`. When `UFB\_MODE` is set (`1`), the internal memory controller completely crosses the internal lanes, mapping Bank 2 to `0x08000000`.

Because `SYSCFG-\>MEMRMP` is entirely volatile, its state disappears the moment the MCU loses power or resets. The system brilliantly treats this as a feature:

- **The Boot Handoff Strategy:** On every single power cycle, the STM32 natively wakes up in its true physical Bank 1 configuration. The bootloader boots for a fraction of a microsecond and reads the non-volatile state stored safely in physical Sector 13.

- If Sector 13 states that Bank 1 contains the active app, the bootloader leaves `UFB\_MODE` at `0` and immediately jumps to the execution vector.

- If Sector 13 states that Bank 2 contains the active app, the bootloader engages `SYSCFG-\>MEMRMP |= SYSCFG\_MEMRMP\_UFB\_MODE`, safely mirroring Bank 2 over to the primary execution address space before executing the application jump.

This architecture ensures that if a catastrophic application error occurs, a hard hardware reset will always safely route control back through the un-brickable bootloader first.



### 3. Step-by-Step Update Execution Flow

The full lifecycle of an update follows a strict, highly coordinated dance between the two microcontrollers:


ESP32 (Gateway)                                                    STM32F469 (Host)
================                                                   ==================

(Connects Wi-Fi)                                                  (Bootloader jumps to Application)

       | <--- [1. Request Version] -------------------------------- | (Running Application layer)

(Queries Manifest JSON)

       | ---- [2. Returns Server Version (e.g., v1.2)] -----------> | (Application layer compares it against current
       |                                                            | version stored in Flash at section .fw_metada
       |                                                            | (e.g., v1.0) and detects version minor is higher)

       | ---- [3. Wait for next reboot to perform the update] ----- | Writes 0x0A (new FW available on server) to Sect 13
       |                                                            | [System Reboots manually]
       |                                                            | [Bootloader wakes up]

       | <--- [4. Sends Request Firmware Byte (0x28)] ------------- |

[Downloads App Binary via HTTPS]                                    | [Enters Parse Stream Loop]
[Buffers first 512B Block]

  +----+
  | Loop for each chunk
  v    |

       | ---- [5. Transmits Frame: Header + Data Payload + CRC] --> | [Parses Stream FSM]
       |                                                            | [Validates Packet CRC]
       |                                                            | [Evaluates Target Sector]

       | <--- [6. Transmits Packet WAIT_BYTE (0x19)] -------------- | [Erase Flash if crossing a new Sector]
(increased timeout for Erase time)                                  | [On-the-fly Flash Erase]
       |                                                            | [Programs Block to Flash]

       | <--- [7. Transmits Packet ACK (0x06)] -------------------- | [Advances Flash Pointers]

  +----+
  | End Loop
  v    |

https transaction ends, all full-packets (512B) are stored,
checks for remaining bytes and append to buffer

       | ---- [8. Transmits Remaining Tail Bytes] ----------------> | [Total Bytes Received Match]
       |                                                            | [Checks Magic Anchor "VERS"]
       |                                                            | [Verifies Major/Minor Match]
       |                                                            | [Runs Comprehensive Global CRC]
       |                                                            | [Writes New Target Bank to Sec 13]

       | <--- [9. Transmits Final Success ACK] -------------------- | [Clears Local States]
       |                                                            | [Flushes UART Pipelines]
       |                                                            | [Invokes NVIC_SystemReset()]

                                                                    | [BOOTLOADER REBOOTS]
                                                                    | [Reads New Bank Choice]
                                                                    | [Sets UFB_MODE for Swap]
                                                                    | [Fetches App Linker MSP]
                                                                    | [Launches Reset_Handler]
                                                                    | [New Application Boots Successfully!]