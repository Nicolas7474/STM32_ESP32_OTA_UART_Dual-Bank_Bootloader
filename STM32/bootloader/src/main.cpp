/* Bootloader with firmware update capability on dual-bank flash memory swap
 * BOOTLOADER FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 */

// ============================================================================
// SYSTEM INCLUDES & DRIVERS
// ============================================================================
#include <cstdint>
#include <cstddef>
#include <span>
#include <cstring>
#include <array>
#include <string_view>
#include <type_traits>
#include <concepts>
//#include <cstdio>
//#include "memory_map.hpp"
#include "stm32f469xx.h"
#include "uart3.hpp"
#include "myConfig.h"
#include "firmware_header.hpp"
#include "timers.h"
#include <stdio.h> // for snprintf (debug purpose)

// ============================================================================
// CONFIGURATIONS, CONSTANTS & PROTOCOL MEMORY LAYOUT
// ============================================================================
enum class State {
    IDLE_START,
    READ_HEADER,
    READ_DATA,
    READ_CRC,
};

struct PacketHeader {
    uint32_t total_size;
    uint16_t packet_id;		 // packet_id increments but we track only the first
    uint16_t payload_length;
    uint16_t version_major;
    uint16_t version_minor;
    uint32_t total_crc;
};

struct FlashSectorMap {
    uint8_t sector_number;
    uint32_t start_address;
};


// Packet constants
constexpr uint8_t PACKET_START_BYTE 	= 0x02;
constexpr uint8_t ACK_BYTE         		= 0x06;
constexpr uint8_t NAK_GENERIC           = 0x15; // Kept as general fallback
constexpr uint8_t NAK_CRC_ERROR         = 0x16; // Per-packet or final CRC failure
constexpr uint8_t NAK_VERSION_MISMATCH  = 0x17; // Firmware version rejected
constexpr uint8_t NAK_MAGIC_MISSING     = 0x18; // Metadata anchor verification failed
constexpr uint8_t ERR_FLASH_ERASE       = 0xE1; // Flash erase routine hardware fault
constexpr uint8_t ERR_FLASH_WRITE       = 0xE2; // Flash program routine hardware fault
constexpr uint8_t CMD_REQ_UPDATE_SERVER = 0x28;  // STM32 -> ESP32: "You can update the new FW"
constexpr uint8_t WAIT_BYTE             = 0x19;

// Explicit constants for our boot tracking state machine
constexpr uint8_t STATE_RUN_BANK1      = 1;
constexpr uint8_t STATE_RUN_BANK2      = 2;
constexpr uint8_t STATE_FORCE_UPDATE   = 0x0A;

// Define the function pointer type for jumping to the application entry point
using AppEntryFunction = void(*)(); // in C: typedef void (*AppEntryFunction)(void);

// ============================================================================
// DUAL-BANK FLASH BOUNDARIES & HELPERS
// ============================================================================
inline constexpr uint32_t BANK1_APP_START_ADDR = 0x08008000U; // Since we don't write the FW on the first two sectors
inline constexpr uint32_t BANK2_APP_START_ADDR = 0x08108000U; // Pushed from 12 originally to 14th sector to get symmetry with Bank 1
// Total Isolation: because the linker doesn't know these addresses exist, it will never try to compile code into them.
inline constexpr uint32_t SECTOR12_START = 0x08100000U; // Eeprom-like sector to store any user data
inline constexpr uint32_t SECTOR12_END   = 0x08104000U; // Boundary limit
inline constexpr uint32_t SECTOR13_START = 0x08104000U; // Non-volatile sector that store linearly the Bank number the application currently runs on
inline constexpr uint32_t SECTOR13_END   = 0x08108000U;
inline constexpr uint32_t SECTOR13_MAGIC_ADDR = 0x08107FFCU; // Last 32-bit word of Sector 13
inline constexpr uint32_t SECTOR13_MAGIC_VAL  = 0x1A2B3C4DU; // Your chosen initialization token

// Explicit physical layout of STM32F469 Bank 1
// Sized to 12 sectors total (Sector 0 to 11)
inline constexpr std::array<FlashSectorMap, 12> bank1_sectors{{
    {0,  0x08000000U}, // 16 KB - Bootloader Entry Point / Vector Table
    {1,  0x08004000U}, // 16 KB - Bootloader Continuation Space
    {2,  0x08008000U}, // 16 KB - Historical App Vector / Header Target
    {3,  0x0800C000U}, // 16 KB - Workspace / App Execution
    {4,  0x08010000U}, // 64 KB
    {5,  0x08020000U}, // 128 KB
    {6,  0x08040000U}, // 128 KB
    {7,  0x08060000U}, // 128 KB
    {8,  0x08080000U}, // 128 KB
    {9,  0x080A0000U}, // 128 KB
    {10, 0x080C0000U}, // 128 KB
    {11, 0x080E0000U}  // 128 KB
}}; // The Outer Braces { } initialize the std::array object itself
    //The Inner Braces { } initialize the underlying raw internal C-array hidden inside the class

// Explicit physical layout of STM32F469 Bank 2
// Sized to 12 sectors total (Sector 12 to 23)
inline constexpr std::array<FlashSectorMap, 12> bank2_sectors{{
    {12, 0x08100000U}, // 16 KB
    {13, 0x08104000U}, // 16 KB
    {14, 0x08108000U}, // 16 KB
    {15, 0x0810C000U}, // 16 KB
    {16, 0x08110000U}, // 64 KB
    {17, 0x08120000U}, // 128 KB
    {18, 0x08140000U}, // 128 KB
    {19, 0x08160000U}, // 128 KB
    {20, 0x08180000U}, // 128 KB
    {21, 0x081A0000U}, // 128 KB
    {22, 0x081C0000U}, // 128 KB
    {23, 0x081E0000U}  // 128 KB
}};

// ============================================================================
// GLOBAL STATE VOLATILE & RUNTIME TRACKING VARIABLES
// ============================================================================
// Global or static tracking variables
volatile State current_state = State::IDLE_START;
PacketHeader header = {0, 0, 0, 0, 0, 0};
uint8_t payload_buffer[512]; // Matches our max expected packet size
uint32_t incoming_crc = 0;
uint32_t bytes_read = 0;
constexpr uint32_t INACTIVITY_TIMEOUT_MS = 4000; // 4 seconds of silence = abort
volatile uint32_t last_debounce_tick = 0;

std::array<uint8_t, 1024> buffer_rx{}; // Sized to 1024 to easily hold a full 512-byte payload packet + protocol framing

// Keep track of the last erased sector number to make sure we don't accidentally re-erase a sector while streaming multiple 512-byte packets into it
static uint32_t target_bank_start_address = 0;
static int16_t last_erased_sector         = -1;  // -1 means nothing erased yet (uint8_t would be enough but int16_t is better for CPU Word Alignment and Implicit Integer Promotion.
static uint32_t current_flash_address     = 0; // no longer hard-coded (0x08100000U)
static bool target_is_bank2               = true;
static uint32_t tot_fw_bytes_written      = 0; // keep track of the written bits
static uint32_t expected_firmware_crc = 0; // Global tracking variable

//===================================================================
// =================== Functions Flash ==============================
//===================================================================

static void flash_unlock() {
    // Check if the flash is already unlocked
    if ((FLASH->CR & FLASH_CR_LOCK) != 0) {
        // Authorize flash register access by writing the mandatory key sequence
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}

static void flash_lock() {
    FLASH->CR |= FLASH_CR_LOCK;  // Re-engage the hardware lock guard
}

static bool flash_erase_sector(uint32_t sector)
{
    if (sector == 0 || sector == 1) return false; // Bank 1 Bootloader sectors cannot be erased
    while (FLASH->SR & FLASH_SR_BSY);
    // Clear previous errors by writing 1 to them
    FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;

    FLASH->CR &= ~FLASH_CR_SNB;
    // --- Dual-Bank Adjustment ---
    uint32_t snb_value = sector;
    if (sector >= 12) {
        snb_value = 16 + (sector - 12); // Adjusts 12->16, 13->17, etc., for Bank 2 hardware
    }

    FLASH->CR |= (snb_value << FLASH_CR_SNB_Pos);
    FLASH->CR |= FLASH_CR_SER; // Sector Erase activated
    FLASH->CR |= FLASH_CR_STRT;

    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_SER;

    // Check if any error flags were pulled high during the erase process
    if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR)) != 0) {
    	// Clear flags so the hardware doesn't lock up future attempts
    	FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;
    	return false; // Erase failed
    }

    return true; // Erase succeeded cleanly
}


static uint32_t CRC32_compute(const uint8_t* data, size_t length) {
    uint32_t crc = 0xFFFFFFFF;
    for (size_t i = 0; i < length; i++) {
        crc ^= data[i];
        for (int j = 0; j < 8; j++) {
            if (crc & 1) {
                crc = (crc >> 1) ^ 0xEDB88320; // Reverse polynomial for standard CRC32
            } else {
                crc >>= 1;
            }
        }
    }
    return ~crc;
}


// Define a custom C++20 concept to lock down our allowed Flash data types
template <typename T>
concept ValidFlashType = std::same_as<T, uint8_t> || std::same_as<T, uint32_t>;

static bool flash_write(uint32_t address, ValidFlashType auto data) {

    while (FLASH->SR & FLASH_SR_BSY);

    FLASH->CR &= ~FLASH_CR_PSIZE;

    // Changed T to decltype(data)
    if constexpr (std::same_as<decltype(data), uint32_t>) {
        FLASH->CR |= FLASH_CR_PSIZE_1;
    }

    FLASH->CR |= FLASH_CR_PG;

    // Changed T to decltype(data)
    *reinterpret_cast<volatile decltype(data)*>(address) = data;

    while (FLASH->SR & FLASH_SR_BSY);

    FLASH->CR &= ~FLASH_CR_PG;

    if ((FLASH->SR & (FLASH_SR_WRPERR | FLASH_SR_PGAERR | FLASH_SR_PGPERR)) != 0) {
        FLASH->SR |= FLASH_SR_PGSERR | FLASH_SR_PGPERR | FLASH_SR_PGAERR | FLASH_SR_WRPERR;
        return false;
    }

    return true;
}

static uint8_t get_active_bank_choice() {
    uint32_t address = SECTOR13_START;

    while (address < SECTOR13_END) {
        uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

        if (current_byte == 0xFF) {
            if (address == SECTOR13_START) {
                return STATE_RUN_BANK1; // Default to Bank 1 if empty
            }

            // Peek backward by 1 byte and return exactly what is there
            return *reinterpret_cast<volatile uint8_t*>(address - 1);
        }
        address++;
    }

    return *reinterpret_cast<volatile uint8_t*>(SECTOR13_END - 1);
}


static void format_sector13_fresh(uint8_t initial_state) {
	flash_unlock();
	// Attempt physical erasure and verify success
	if (!flash_erase_sector(13)) {
		// HARDWARE ERASE FAULT: Stop everything!
		flash_lock();
		uart6.UART_Transmit(std::span<const uint8_t>(&ERR_FLASH_ERASE, 1), 500);
		return; // Break out of execution early to prevent programming un-erased memory
	}
	flash_write(SECTOR13_START, initial_state);  // Write the starting bank choice at the very first byte
	flash_write(SECTOR13_MAGIC_ADDR, SECTOR13_MAGIC_VAL);  // Write the magic word at the very last 32-bit word
}

static void record_new_bank_state(uint8_t new_state) {
	// Wear-Leveling byte-by-byte linear scanning strategy inside Sector 13
	uint32_t address = SECTOR13_START;
	flash_unlock();

    while (address < SECTOR13_MAGIC_ADDR) {
        uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

        if (current_byte == 0xFF) {
            // Found the very first available empty byte slot! Write our state here.
            flash_write(address, new_state);
            return;
        }
        address++; // Advance byte by byte
    }
    // Fallback safeguard: if Sector 13 is completely packed full (all 16KB),
    // we must erase it and reset to the beginning
    format_sector13_fresh(new_state);
}


static bool program_packet_to_flash(uint32_t start_address, std::span<const uint8_t> payload) {
    // Ensure our length is a multiple of 4 bytes to avoid partial word writes
    if (payload.size() % 4 != 0) {
        return false;
    }
    uint32_t target_address = start_address;

    // Loop jumping 4 bytes at a time
    for (size_t offset = 0; offset < payload.size(); offset += 4) {
        uint32_t word = 0;

        // Perfectly safe from both alignment and strict aliasing rules
        std::memcpy(&word, &payload[offset], sizeof(uint32_t));

        // Attempt bare-metal physical write
        if (!flash_write(target_address, word)) {
        	return false; // Hardware write failure
        }
        // Immediate read-back verification
        // Directly sample the physical Flash memory address
        uint32_t written_word = *reinterpret_cast<volatile uint32_t*>(target_address);
        if (written_word != word) {
        	uart6.UART_Transmit(std::span<const uint8_t>(&ERR_FLASH_WRITE, 1), 500);
        	return false; // Flash verification mismatch! Silicon/Data corruption detected.
        }
        // Advance physical flash pointer forward by exactly 1 word (4 bytes)
        target_address += 4;
    }

    return true;
}

//===================================================================
// =================== UART RECEIVE FW ENGINE =======================
//===================================================================

void execute_flash_and_respond() {
    // 1. Packet Integrity Check via software CRC32 validation
    uint32_t computed_crc = CRC32_compute(payload_buffer, header.payload_length);

    if (computed_crc != incoming_crc) {
        // Signal NAK; allows the ESP32 to retry sending this specific payload block
    	uart6.UART_Transmit(std::span<const uint8_t>(&NAK_CRC_ERROR, 1), 500);
        return;
    }

    // 2. Transaction Initialization (Executes only upon receiving the initial payload block)
    if (header.packet_id == 1) {
    	tot_fw_bytes_written = 0; // Reset counter to prevent accumulation from previous failed attempts
    	expected_firmware_crc = header.total_crc; // Cache the global firmware CRC validation token
    	uint8_t active_bank = get_active_bank_choice();

    	// If stuck in a forced update state, step backward through Sector 13 to resolve the last valid active bank
    	if (active_bank == STATE_FORCE_UPDATE) {
    		uint32_t address = SECTOR13_START;
    		while (address < SECTOR13_END) {
    			if (*reinterpret_cast<volatile uint8_t*>(address) == 0xFF) {
    				break;
    			}
    			address++;
    		}

    		// Step back past the current state token to retrieve the historical active bank identifier
    		if (address >= (SECTOR13_START + 2)) {
    			active_bank = *reinterpret_cast<volatile uint8_t*>(address - 2);
    		} else {
    			active_bank = STATE_RUN_BANK1; // Ultimate hardware fallback
    		}
    	}

    	// Assign target bank destination to the opposing inactive side (Dual-Bank execution swap topology)
    	if (active_bank == STATE_RUN_BANK2) {
    		target_bank_start_address = BANK1_APP_START_ADDR;
    		target_is_bank2 = false;
    	} else {
    		target_bank_start_address = BANK2_APP_START_ADDR;
    		target_is_bank2 = true;
    	}

    	current_flash_address = target_bank_start_address;
    	last_erased_sector    = -1;
    }

    // 3. Dynamic Sector Erase Engine
    uint32_t packet_end_address = current_flash_address + header.payload_length - 1;
    uint8_t target_sector = 0;
    bool sector_found = false;

    // Scan the designated bank sector table in reverse order to resolve the target physical boundary
    if (target_is_bank2) {
    	for (int i = static_cast<int>(bank2_sectors.size()) - 1; i >= 0; --i) {
    		if (packet_end_address >= bank2_sectors[i].start_address) {
    			target_sector = bank2_sectors[i].sector_number;
    			sector_found = true;
    			break;
    		}
    	}
    	// Hard Safeguard: Deny application updates from dropping into Sectors 12 & 13 (EEPROM non-volatile configs)
    	if (sector_found && target_sector <= 13) {
    		sector_found = false;
    	}
    } else {
    	for (int i = static_cast<int>(bank1_sectors.size()) - 1; i >= 0; --i) {
    		if (packet_end_address >= bank1_sectors[i].start_address) {
    			target_sector = bank1_sectors[i].sector_number;
    			sector_found = true;
    			break;
    		}
    	}
        // Hard Safeguard: Avoid overwriting Sector 0 & 1 containing the running bootloader code
    	if (sector_found && target_sector <= 1) {
    		sector_found = false;
    	}
    }

    flash_unlock();  // Gain peripheral access right before interacting with memory controllers

    // Execute safe on-the-fly erasure only when stepping into a fresh un-erased physical sector block
    if (sector_found) {
    	if (static_cast<int16_t>(target_sector) > last_erased_sector) {
    		// ! Delay guaranteed on 1st packet ! Minimum erase time > 300ms !
    		uart6.UART_Transmit(std::span<const uint8_t>(&WAIT_BYTE, 1), 100); // Inform the ESP32 it must adjust its listening timeout
    		if (!flash_erase_sector(target_sector)) {
    			flash_lock(); // Hardware failure: Immediately secure controller registers and abort
    			uart6.UART_Transmit(std::span<const uint8_t>(&ERR_FLASH_ERASE, 1), 500);
    			return;
    		}
    		last_erased_sector = static_cast<int16_t>(target_sector);
    	}
    }

    // 4. Commit current payload block into Flash memory
    std::span<const uint8_t> payload_span(payload_buffer, header.payload_length);
    if (program_packet_to_flash(current_flash_address, payload_span)) {
    	current_flash_address += header.payload_length;
    	tot_fw_bytes_written += header.payload_length;

    	// 5. Finalize firmware download transaction upon full data reception
    	if (tot_fw_bytes_written >= header.total_size) {

    		// Retrieve application compiled metadata layout from explicit linker script memory offset (0x200)
    		const auto* flashed_meta = reinterpret_cast<const FirmwareMetadata*>(target_bank_start_address + 0x200U);

    		// Verify the binary validation anchor token ("VERS") to prevent uncompiled raw crashes
    		if (flashed_meta->magic_anchor != 0x56455253) {
    			char err_buf[64];
    			snprintf(err_buf, sizeof(err_buf), "\r\n[ERR] Metadata Magic anchor missing! Found: %08lX\r\n", flashed_meta->magic_anchor);
    			uart3.UART_Transmit({reinterpret_cast<const uint8_t*>(err_buf), strlen(err_buf)}, 500);
    			flash_lock();
    			tot_fw_bytes_written = 0;
    			ms_since_last_packet = 0;
    			transfer_in_progress = false; // No need to reboot to retry after a 4sec delay if the magic key is not present
    			uart6.UART_Transmit(std::span<const uint8_t>(&NAK_MAGIC_MISSING, 1), 500);
    			return;
    		}
    		uint16_t expected_major = header.version_major;
    		uint16_t expected_minor = header.version_minor;

    		// Validate compile-time version metadata properties against host manifest attributes
    		if (flashed_meta->version_major != expected_major || flashed_meta->version_minor != expected_minor) {
    			constexpr std::string_view mismatch_msg = "[ERR] Manifest version mismatch with compilation version!\r\n";
    			uart3.UART_Transmit({reinterpret_cast<const uint8_t*>(mismatch_msg.data()), mismatch_msg.size()}, 500);
    			flash_lock();
    			tot_fw_bytes_written = 0;
    			ms_since_last_packet = 0;
    			transfer_in_progress = false; // No need to reboot to retry after a 4sec delay if the version is mismatched
    			uart6.UART_Transmit(std::span<const uint8_t>(&NAK_VERSION_MISMATCH, 1), 500);
    			return;
    		}

    		// Verify the entire flashed binary against the expected CRC from the header
    		uint32_t physical_flash_crc = CRC32_compute(reinterpret_cast<const uint8_t*>(target_bank_start_address), header.total_size);

    		if (physical_flash_crc == expected_firmware_crc) {
    			// After last check, image completely validated
    			uint8_t bank_choice = (target_is_bank2) ? 0x02 : 0x01;
    			// It will be safe to switch execution configuration mapping in Sector 13
    			record_new_bank_state(bank_choice);
    			flash_lock();
    			tot_fw_bytes_written = 0;
    			transfer_in_progress = false;
    			uart6.UART_Transmit(std::span<const uint8_t>(&ACK_BYTE, 1), 500); // Send final ACK byte

                constexpr std::string_view msg = "New application FW flashed successfully, STM32 will reboot now.";
    			uart3.UART_Transmit(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, 500);

    			// Wait until peripheral transmission shift registers fully clear to avoid truncated UART lines
    			while ((USART3->SR & USART_SR_TC) == 0 || (USART6->SR & USART_SR_TC) == 0);
    			__disable_irq();     // Close execution interrupts prior to reset sequence
    			NVIC_SystemReset();  // Execute system reset to boot application image
    		} else {
    			// Comprehensive CRC evaluation failed; maintain safe bootloader status loop to prevent booting corrupted images
    			flash_lock();
    			tot_fw_bytes_written = 0;
    			ms_since_last_packet = 0;
    			transfer_in_progress = false;  // No need to reboot to retry after a 4sec delay if the CRC don't match
    			uart6.UART_Transmit(std::span<const uint8_t>(&NAK_CRC_ERROR, 1), 2); // send byte CRC error
    			return;
    		}
    	} else {
    		// Continuous multi-packet data streams receive sequential block ACKs to prompt next chunk transfers
    		uart6.UART_Transmit(std::span<const uint8_t>(&ACK_BYTE, 1), 500);
    	}
    } else {
    	// Non-volatile memory flash write peripheral driver failure detected
    	uart6.UART_Transmit(std::span<const uint8_t>(&ERR_FLASH_WRITE, 1), 500);
    }
}

static void ParseIncomingStream(std::span<const uint8_t> incoming_chunk) {
	if (incoming_chunk.empty()) return;

	// Process incoming byte stream sequentially through a state-machine parser
	for (uint8_t byte : incoming_chunk) {

		switch (current_state) {
		case State::IDLE_START: {
			// Wait for a valid packet sync byte to begin parsing
			if (byte == PACKET_START_BYTE) {
				bytes_read = 0;
				transfer_in_progress = true; // Mark that the update is alive
				ms_since_last_packet = 0;    // Reset inactivity countdown
				current_state = State::READ_HEADER;
			}
			break;
		}

		case State::READ_HEADER: {
			static uint8_t header_raw[16];
			header_raw[bytes_read++] = byte;

			// Once the fixed 16-byte header is buffered, decode fields from Big-Endian
			if (bytes_read == 16) {
				header.total_size	 =  ((uint32_t)header_raw[0] << 24) | 	 // Decode total_size (Big-Endian)
										((uint32_t)header_raw[1] << 16) |
										((uint32_t)header_raw[2] << 8)  |
										header_raw[3];
        		header.packet_id	  = (header_raw[4] << 8)  | header_raw[5]; // uart sends in a big endian format (MSB first) then
        		header.payload_length = (header_raw[6] << 8)  | header_raw[7]; // we shift them back into a single 16-bit integer
        		header.version_major  = (header_raw[8] << 8)  | header_raw[9];  // Decode Major
        		header.version_minor  = (header_raw[10] << 8) | header_raw[11]; // Decode Minor
        		header.total_crc	  = ((uint32_t)header_raw[12]  << 24) |
										((uint32_t)header_raw[13]  << 16) |
										((uint32_t)header_raw[14] << 8)  |
										  header_raw[15];

        		// DEFENSIVE SAFEGUARD: Prevent malicious or malformed packets from crashing RAM
        		if (header.payload_length > 512) {
        			header.payload_length = 512;
        		}

        		bytes_read = 0;
        		current_state = State::READ_DATA;
        	}
        	break;
        }

        case State::READ_DATA: {
        	payload_buffer[bytes_read++] = byte;

        	// Advance once the variable-length payload dictates we have captured the full chunk
        	if (bytes_read == header.payload_length) {
        		bytes_read = 0;
        		current_state = State::READ_CRC;
        	}
        	break;
        }

        case State::READ_CRC: {
        	static uint8_t crc_raw[4];
        	crc_raw[bytes_read++] = byte;

        	// Decode packet-level CRC and finalize block transaction
        	if (bytes_read == 4) {
        		incoming_crc = ((uint32_t)crc_raw[0] << 24) |
        					   ((uint32_t)crc_raw[1] << 16) |
							   ((uint32_t)crc_raw[2] << 8)  |
							   crc_raw[3];

        		execute_flash_and_respond();  // Process the flash operations immediatly
        		bytes_read = 0;
        		current_state = State::IDLE_START; // Reset variables for the next incoming packet
        	}
        	break;
        }
        }
    }
}

// ============================================================================
// CPU HANDOVER & ARCHITECTURAL JUMP SEQUENCER
// ============================================================================

// Force this function to run entirely out of RAM, prevent a CPU HardFault crash when flipping UFB_MODE,
// as modifying active Flash banks mid-execution would shift code addresses directly beneath the active instruction pipeline
__attribute__((section(".RamFunc"), noinline))
static void jump_to_application(uint32_t target_app_addr) {

    // PHASE 1: CORE HARDWARE DE-INITIALIZATION
    // Forcefully disable the global interrupts at the core level first
    __disable_irq();

    // Disable SysTick completely and clear its pending exception state
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;
    SCB->ICSR |= SCB_ICSR_PENDSTCLR_Msk;

    // Stop and clear UART3 + UART6 configuration registers
    USART3->CR1 &= ~USART_CR1_UE;  // Disable USART3 globally
    USART3->CR3 &= ~(USART_CR3_DMAR | USART_CR3_DMAT); // Sever DMA linkages
    USART6->CR1 &= ~USART_CR1_UE;  // Disable USART6 globally
    USART6->CR3 &= ~(USART_CR3_DMAR | USART_CR3_DMAT); // Sever DMA linkages from firmware stream

    // Force clear the specific NVIC Enable and Pending registers used
    NVIC_DisableIRQ(RTC_WKUP_IRQn);
    NVIC_ClearPendingIRQ(RTC_WKUP_IRQn);

    NVIC_DisableIRQ(USART3_IRQn);
    NVIC_ClearPendingIRQ(USART3_IRQn);
    NVIC_DisableIRQ(USART6_IRQn);
    NVIC_ClearPendingIRQ(USART6_IRQn);

    // Reset peripheral clocks completely to force hardware modules back to factory states
    RCC->APB1RSTR |= RCC_APB1RSTR_USART3RST;
    RCC->APB1RSTR &= ~RCC_APB1RSTR_USART3RST;
    RCC->APB2RSTR |= RCC_APB2RSTR_USART6RST;
    RCC->APB2RSTR &= ~RCC_APB2RSTR_USART6RST;

    // PHASE 2: HARDWARE SWAP AND EXECUTION HANDOFF
    uint8_t active_bank = get_active_bank_choice();

    // Swap physical mapping of Flash Bank 1 and Bank 2.
    if (active_bank == 2) {
        SYSCFG->MEMRMP |= SYSCFG_MEMRMP_UFB_MODE;
    } else {
        SYSCFG->MEMRMP &= ~SYSCFG_MEMRMP_UFB_MODE;
    }
    __DSB();
    __ISB();

    // Read the first 32-bit word from the application's binary vector table. This contains the initial safe stack
    // pointer (RAM ceiling) of the bank we will switch onto (because of target_app_addr) calculated by the linker
    uint32_t app_stack_pointer = *reinterpret_cast<volatile uint32_t*>(target_app_addr);

    // jump_address value is the absolute address where the application's first instruction of the target bank is physically located in Flash.
    // Ex: Once the linker places all the compiled functions into the Flash space and determines that Reset_Handler is located exactly at 0x0800814C,
    // it goes back to the vector table at 0x08008004 (Start + 4) and writes 0x0800814C (plus 1 for Thumb mode, so 0x0800814D) into those 4 bytes.
    uint32_t jump_address      = *reinterpret_cast<volatile uint32_t*>(target_app_addr + 4);

    // Enforce Thumb-mode state execution alignment constraint
    jump_address |= 0x01U; // ARM Cortex-M processors only support the Thumb instruction set

    // Before leaping to the new application, the CPU needs to forget the bootloader's local RAM variables and setup its execution boundary
    __set_MSP(app_stack_pointer); // Changes the hardware's MSP register to point directly to the application’s fresh, clean RAM ceiling

    // We cast jump_address (raw number) as a callable function pointer type (AppEntryFunction).
    // When app_reset_handler() is called, the CPU updates its internal Program Counter (PC) register to your new application's address.
    AppEntryFunction app_reset_handler = reinterpret_cast<AppEntryFunction>(jump_address);
    app_reset_handler(); // with: typedef void (*AppEntryFunction)(void);
}

// Helper function just in case we need to force an update byte in sector 13
[[maybe_unused]] static void write_0xA() {
    uint32_t address = 0x08104000U; // Sector 13 start address
    constexpr uint32_t sector_boundary = 0x08107FFCU;

    while (address < sector_boundary) {
    	uint8_t current_byte = *reinterpret_cast<volatile uint8_t*>(address);

    	if (current_byte == 0xFF) {
    		// Found an empty slot! Unlock flash right here where needed
    		flash_unlock();
    		// Clear any outstanding flash status flags before writing
    		FLASH->SR = FLASH_SR_EOP | FLASH_SR_OPERR | FLASH_SR_WRPERR |FLASH_SR_PGAERR | FLASH_SR_PGPERR | FLASH_SR_PGSERR;
    		// Set register to byte write size (PSIZE = 0)
    		FLASH->CR &= ~FLASH_CR_PSIZE;
    		FLASH->CR |= FLASH_CR_PG;
    		// Write the 0x0A force update flag
    		*reinterpret_cast<volatile uint8_t*>(address) = 0x0A;
    		// Wait for operation to complete safely
    		while (FLASH->SR & FLASH_SR_BSY);
    		// Clean up programming configuration and lock flash immediately
    		FLASH->CR &= ~FLASH_CR_PG;
    		FLASH->CR |= FLASH_CR_LOCK;
    		return;
    	}
    	address++;
    }
}

static void GPIO__Interrupt() {
    // 1. Enable GPIOA Clock (and SYSCFG clock for interrupt routing)
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOAEN;
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;

    // 2. Set PA0 as Input Mode (00 in MODER)
    //GPIOA->MODER &= ~GPIO_MODER_MODER0;

    // 3. Clear and map EXTI0 line to Port A (0000 in EXTICR[0])
    SYSCFG->EXTICR[0] &= ~(0xF<<0);

    // 4. Configure EXTI Line 0 (Unmask + Set Rising Edge Trigger)
    EXTI->IMR  |= EXTI_IMR_IM0;     // Unmask interrupt line 0
    EXTI->RTSR |= EXTI_RTSR_TR0;    // Trigger on rising edge (button press)
    EXTI->FTSR &= ~EXTI_FTSR_TR0;   // Disable falling edge trigger

    // 5. Enable the Interrupt in the NVIC
    NVIC_SetPriority(EXTI0_IRQn, 2); // Set lower priority than critical UART/DMA
    NVIC_EnableIRQ(EXTI0_IRQn);
}


// =================== MAIN () ======================================

int main() {

    // PHASE 1: Core Hardware & LED Initialization
    SysClockConfig(); //
    SysTick_Init();   //
    GPIO_Config();    //

    GPIOG->ODR ^= GPIO_ODR_OD6; // disable green led
    GPIOD->ODR ^= GPIO_ODR_OD5; // disable rouge

    //write_0xA(); // reset the bootloader in "waiting for update file..." state
    //record_new_bank_state(0x01); // manually set the application bank
    //format_sector13_fresh(1);

    // PHASE 2: Non-Volatile Memory Integrity Validation
    // Fast autonomous magic check: read the absolute end of the sector. If it's not our token, reset the sector
    if (*reinterpret_cast<volatile uint32_t*>(SECTOR13_MAGIC_ADDR) != SECTOR13_MAGIC_VAL) {
        // The sector 13 must be initialized at 0xFF at the first use after programming
        format_sector13_fresh(STATE_RUN_BANK1); // Force a clean format (bank x [...0xFF...0xFF...] magic_nb)
    }

    uint8_t boot_state = get_active_bank_choice();

    // PHASE 3: Execution Routing Decision
    // 3. Condition Check: Only jump if we are NOT forcing an update!
    if (boot_state == STATE_RUN_BANK1 || boot_state == STATE_RUN_BANK2) {
        jump_to_application(BANK1_APP_START_ADDR);
    }

    // PHASE 4: Forced Firmware Update Mode
    else if (boot_state == STATE_FORCE_UPDATE) {
        GPIO__Interrupt();

        // 4.  Wait for firmware update
        BareM_StatusTypeDef res6 = uart6.init(1500000); // 1.5 Mbps
        while (res6 != Bare_OK);
        BareM_StatusTypeDef res3 = uart3.init(115200);
        while (res3 != Bare_OK);

        NBdelay_ms(5); // Settle time

        uart6.startReceiveToIdle_DMA(buffer_rx); // Starting listening to the uart byes of FW update file
        uart6.UART_Transmit(std::span<const uint8_t>(&CMD_REQ_UPDATE_SERVER, 1), 500); // Tells the ESP32: upload and forward to me the FW file

        constexpr std::string_view msg = "Firmware uploading mode...";
        uart3.UART_Transmit(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, 500);

        // PHASE 5: Active Streaming Loop & Watchdog Processing
        while (true) {
            // Active watchdog check for mid-transfer abandonment
            if (transfer_in_progress && (ms_since_last_packet > INACTIVITY_TIMEOUT_MS)) {
                while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0); // Crucial: wait for DMA Stream to turn off
                while ((USART3->SR & USART_SR_TC) == 0);       // Wait for USART Transmission Complete (TC) flag

                __disable_irq();
                NVIC_SystemReset(); // Purge state and reboot cleanly
            }

            GPIOD->ODR ^= GPIO_ODR_OD4;
            NBdelay_ms(150); // blink wait to receive the uart bytes
        }
        return 0;
    }
}

// ============================================================================
// 		ISR
// ============================================================================

// Overrides the weak symbol automatically
extern "C" void UART_RxCpltCallback_DMA(const UartDriver& instance, std::span<const uint8_t> incoming_data) {
    if (&instance == &uart6) {
        ParseIncomingStream(incoming_data); // Will process data from the ESP32
    }
}

// button PA0 pushed triggers an interrupt
extern "C" void EXTI0_IRQHandler(void) {
    // Check if the interrupt came from line 0
    if (EXTI->PR & (1 << 0)) {
        EXTI->PR = (1 << 0);  // Clear the interrupt flag by writing a 1

        uint32_t current_tick = GetSysTick(); // for the debouncing

        // Only register the press if 300ms have passed since the last valid press (Debounce Guard)
        if ((current_tick - last_debounce_tick) > 300) {

            // Safety check: Only escape if we are actively waiting for an update file
            // (This prevents accidental button presses from wiping states at wrong times)
            uint8_t current_boot_state = get_active_bank_choice();

            if (current_boot_state == STATE_FORCE_UPDATE) {

                // STEP 1: Find the upper bound of written data in Sector 13
                uint8_t fallback_bank = STATE_RUN_BANK1; // Default fallback
                uint32_t address = SECTOR13_START;

                // Find the first unwritten byte (0xFF)
                while (address < SECTOR13_MAGIC_ADDR) {
                    if (*reinterpret_cast<volatile uint8_t*>(address) == 0xFF) {
                        break;
                    }
                    address++;
                }

                // STEP 2: Start scanning backwards from the last written byte
                if (address > SECTOR13_START) {
                    address--; // Step back from 0xFF onto the last written byte (which is likely 0x0A)

                    // Scan backwards byte-by-byte down to the very first byte of the sector
                    while (address >= SECTOR13_START) {
                        uint8_t checked_byte = *reinterpret_cast<volatile uint8_t*>(address);

                        if (checked_byte == STATE_RUN_BANK1 || checked_byte == STATE_RUN_BANK2) {
                            fallback_bank = checked_byte;
                            break; // Found the last valid active bank! Stop scanning.
                        }

                        // If it's a 0x0A, just keep walking backwards
                        if (address == SECTOR13_START) {
                            break; // Prevent underflowing below our sector start
                        }
                        address--;
                    }
                }

                // STEP 3: Commit structural state recovery & trigger hardware reset
                record_new_bank_state(fallback_bank); // Write Sector 13 to store the fallback bank byte

                // Clean down and reboot
                __disable_irq();         // Prevent other interrupts from breaking the restart
                EXTI->PR = EXTI_PR_PR0;  // Clear pending flag just in case

                // while ((DMA1_Stream3->CR & DMA_SxCR_EN) != 0); // Crucial ?? if DMA is used, wait for Stream to turn off
                while ((USART3->SR & USART_SR_TC) == 0);          // Wait for USART Transmission Complete (TC) flag

                NVIC_SystemReset();      // Reboot directly out of the loop!
            }
            last_debounce_tick = current_tick; // Update the tracking timestamp
        }
    }
}


/*
 Every time the board powers up or undergoes a hard reset, your code executes this exact sequence:
    The Magic Check: The CPU checks a single address (0x08107FFC).
    The Self-Heal (Day 1): If the board just came off the factory assembly line and is filled with noise/zeros,
    the magic number isn't there. The bootloader immediately calls format_sector13_fresh(),
    erasing the entire sector to a pristine physical 0xFF state, stamps the first byte with 0x01 (Bank 1), and seals the end with 0x1A2B3C4D.
    The Instant Jump: If the magic number is there (Normal Day 2+ operation), it skips the formatting entirely,
    reads the current active bank choice from your sequential list, sets the SYSCFG->MEMRMP hardware mapping steering wheel,
    resets the stack pointer, and leaps into your main application in less than a microsecond.

UFB_MODE is volatile. It resets to 0 every single time the chip loses power or undergoes a power-on reset.
If we rely only on the volatile UFB_MODE bit inside jump_to_application(), then a simple power cycle would make the chip wake up,
default back to Bank 1 mapping, and boot right back into the old Bank 1 application. Your new Bank 2 application would be stranded!

-> How Sector 13 Saves the Day After a Power Cycle
Because Sector 13 is non-volatile physical flash, its contents survive power cuts, battery drains, and hard resets perfectly.
When you turn on the power, the STM32F469 wakes up, UFB_MODE is 0, and the CPU starts executing your bootloader from the beginning of physical Bank 1.
Here is how main() uses Sector 13 to handle a cold power-on start:

    The Power Turns On: The MCU boots natively into Bank 1. main() starts executing.
    The Non-Volatile Check: main() immediately calls get_active_bank_choice(), which reads physical Sector 13.
    The Discovery: Even though the chip just lost power, Sector 13 stubbornly remembers its last state. Let's say it reads 0x02 (meaning Bank 2 contains the active, updated firmware).
    The Volatile Restoration: Inside main(), because active_bank == 2, the bootloader immediately calls jump_to_application().
    Flipping the Bit: Inside jump_to_application(), the code detects it needs to run Bank 2, so it explicitly sets SYSCFG->MEMRMP |= SYSCFG_MEMRMP_UFB_MODE;.

By setting the bit right there, the bootloader re-engages the hardware swap on every single boot before handing control to the application. From the user's perspective, it feels like a permanent hardware change, even though the bootloader is secretly running for a microsecond at power-up to configure the steering wheel.

This means:
    If Bank 1 is the active software, the bootloader runs for a microsecond, leaves UFB_MODE at 0, and jumps to Bank 1.
    If Bank 2 is the active software, the bootloader runs for a microsecond, forces UFB_MODE to 1, and jumps to Bank 2.

The volatile nature of the register is no longer a glitch—it becomes a feature!
It guarantees your bootloader in Bank 1 always gets a chance to wake up first, read the flash memory state, and safely route the processor exactly where it belongs.
*/
