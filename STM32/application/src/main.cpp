/* Bootloader with firmware update capability on dual-bank flash memory swap
 * APPLICATION FILE
 * @file main.cpp
 * @brief STM32F469 Custom Bare-Metal C++ Bootloader
 */

#include "memory_map.hpp"
#include "firmware_header.hpp"
#include <cstdint>
#include <string_view>
#include <span>
#include "stm32f469xx.h"
#include "myConfig.h"
#include "timers.h"
#include <string.h>
#include "uart3.hpp"
#include <stdio.h> // snprintf

#define CMD_CHECK_UPDATE_REQ          0x25  // STM32 -> ESP32: "Check for updates!"
#define CMD_UPDATE_INFO_REPLY         0x20 // ESP32: "This is the FW version I fetched from server"
#define CMD_REQ_UPDATE_SERVER  		  0x28  // STM32 -> ESP32: "You can update the new FW"

volatile uint32_t last_debounce_tick = 0;
std::array<uint8_t, 1024> buffer_Rx{}; // Sized to 1024 to easily hold a full 512-byte payload packet + protocol framing

//  Declare the current FW version
__attribute__((section(".fw_metadata"), used))
const FirmwareMetadata current_fw_info = {
    .magic_anchor  = 0x56455253,  // "VERS" ASCII literal
    .version_major = 1,			  // Your current running version
    .version_minor = 2,           // Change version (higher), build and upload the resulting application.bin file to the server
    .total_size    = 3428,        // Match your server JSON payload if known
    .total_crc     = 1140727448
};

void initialize_hardware() {
    // 1. Relocate the Vector Table to point to the application's starting Flash boundary.
    // This is the absolute first thing a shifted bare-metal firmware must execute.
	SCB->VTOR = Memory::APP_START_ADDR;
	__enable_irq();

     // 3. Initialize your application-specific peripherals here (GPIO, clocks, timers, etc.)
    SysClockConfig();
    SysTick_Init();
    GPIO_Config();
    InterruptGPIO_Config();
}

static void flash_unlock() {
    // Check if the flash is already unlocked
    if ((FLASH->CR & FLASH_CR_LOCK) != 0) {
        // Authorize flash register access by writing the mandatory key sequence
        FLASH->KEYR = 0x45670123U;
        FLASH->KEYR = 0xCDEF89ABU;
    }
}

static uint8_t get_active_bank_choice() {
    // Enable SYSCFG clock just in case it was turned off
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    // Check if the User Flash Bank mode bit is set
    if ((SYSCFG->MEMRMP & SYSCFG_MEMRMP_UFB_MODE) != 0) {
        return 2;
    }
    return 1;
}

[[maybe_unused]]
static void write_0xA() {

    uint32_t address = (get_active_bank_choice() == 1) ? 0x08104000U : 0x08004000U; // Sector 13 start address
    uint32_t sector_boundary = (get_active_bank_choice() == 1) ? 0x08107FFCU : 0x08007FFCU;
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

    // Fallback: If the code reaches here, Sector 13 is full!
    // We must erase the sector to reset the wear-leveling tracking block.
    flash_unlock();
    // Sector 13 Erase Sequence (Sector 13, Voltage Range x32 assumed for 3.3V)
    FLASH->CR &= ~FLASH_CR_SNB;
    FLASH->CR |= (13U << FLASH_CR_SNB_Pos) | FLASH_CR_SER;
    FLASH->CR |= FLASH_CR_STRT;
    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_SER;
    // Now write 0x0A to the very first slot of the clean sector
    FLASH->CR &= ~FLASH_CR_PSIZE;
    FLASH->CR |= FLASH_CR_PG;
    *reinterpret_cast<volatile uint8_t*>(0x08104000U) = 0x0A;
    *reinterpret_cast<volatile uint32_t*>(0x08107FFCU) = 0x1A2B3C4DU; // Write the magic word at the very last 32-bit word
    while (FLASH->SR & FLASH_SR_BSY);
    FLASH->CR &= ~FLASH_CR_PG;
    FLASH->CR |= FLASH_CR_LOCK;
}


int main() {
	initialize_hardware();
	GPIOD->ODR ^= GPIO_ODR_OD4;

	BareM_StatusTypeDef res3 = uart3.init(115200);
	while(res3 != Bare_OK);
	BareM_StatusTypeDef res6 = uart6.init(1500000); // 1.5 Mbps
	while(res6 != Bare_OK);

	uart6.startReceiveToIdle_DMA(buffer_Rx);

	uint8_t bank_nb = get_active_bank_choice();
	std::array<uint8_t, 1> tx_buffer = { static_cast<uint8_t>(bank_nb + '0') }; 	// Create a small local buffer to back up the data safely
	uart3.UART_Transmit(tx_buffer, 200); 	// Pass the span cleanly. Note: Ensure tx_buffer remains in scope until the Tx completes!

	char buf[16];
	snprintf(buf, sizeof(buf), " Version: %d.%d ", (int)current_fw_info.version_major, (int)current_fw_info.version_minor);
	uart3.UART_Transmit({reinterpret_cast<const uint8_t*>(buf), strlen(buf)}, 500);

    while (true) {
    	NBdelay_ms(3000);
    	uart3.UART_Transmit(tx_buffer, 200);
    }
    return 0;
}

// ============================================================================
// 			ISR
// ============================================================================

// Overrides the weak symbol automatically
extern "C" void UART_RxCpltCallback_DMA(const UartDriver& instance, std::span<const uint8_t> incoming_data) {
    if (&instance == &uart6) {
        if (incoming_data.empty()) return;

        // Always mirror whatever arrived over to uart3 for debugging
        uart3.UART_Transmit(incoming_data, 50);

        // Persistent parsing states across sequential DMA call intervals
        static enum class ParserState : uint8_t {
            WAIT_CMD,
            WAIT_MAJOR,
            WAIT_MINOR
        } state = ParserState::WAIT_CMD;

        static uint8_t major = 0;

        // Byte-by-byte linear stream evaluation, since DMA and UART streams
        // do not guarantee that the entire 3-byte payload will arrive in a single chunk
        for (uint8_t byte : incoming_data) {
            switch (state) {
                case ParserState::WAIT_CMD:
                    if (byte == static_cast<uint8_t>(CMD_UPDATE_INFO_REPLY)) {
                        state = ParserState::WAIT_MAJOR; // if byte = 0x10
                    }
                    break;
                case ParserState::WAIT_MAJOR:
                	major = byte;
                	state = ParserState::WAIT_MINOR;
                	break;
                case ParserState::WAIT_MINOR: {
                	uint8_t minor = byte;
                	// If version on the server is found newer, write 0x0A on the dedicated Flash storage sector
                	if (major > static_cast<uint8_t>(current_fw_info.version_major) ||
                			(major == static_cast<uint8_t>(current_fw_info.version_major) && minor > static_cast<uint8_t>(current_fw_info.version_minor))) {
                		write_0xA();
                		// Pass the array directly
                		constexpr std::string_view msg = "New FW update has been found, will be installed at next start-up";
                		uart3.UART_Transmit(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg.data()), msg.size()}, 500);
                	}
                	else {
                		constexpr std::string_view msg2 = "Firmware file has been found, but version is older. Won't be installed.";
                		uart3.UART_Transmit(std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(msg2.data()), msg2.size()}, 500);
                	}
                	state = ParserState::WAIT_CMD; // Reset to hunt for the next command sequence
                	break;
                }
            }
        }
    }
}


// PAO button input interrupt handler
extern "C"  // Prevent C++ name mangling so the Assembly vector table can find this exact symbol
void EXTI0_IRQHandler() {
	if (EXTI->PR & (1<<0)) {  // button pushed : if the PA0 triggered the interrupt
		EXTI->PR = (1<<0);  // Clear the interrupt flag by writing a 1

		uint32_t current_tick = GetSysTick();
		// Only register the press if 300ms have passed since the last valid press
		if ((current_tick - last_debounce_tick) > 300) {
			GPIOD->ODR ^= GPIO_ODR_OD4; //toggle orange
			uart6.UART_Transmit(std::array<uint8_t, 1>{CMD_CHECK_UPDATE_REQ}, 200); // Modern C++ array literal (clean single-liner)
			last_debounce_tick = current_tick; // Update the tracking timestamp
		}
	}
}
