/* STM32F469
******* UART3 - C++ Bare-Metal Driver - Polling, Interrupts and DMA based  **
******* with COBS option
******** See usage example for IT and DMA in main.c  ****************/

#include "../inc/uart3.hpp"   // Always include its own header first

#include "timers.h"  // For GetSysTick() timing guards
// C++ Library implementations used inside the driver logic
#include <cstring>    // For std::memcpy
#include <algorithm>  // For std::min


//#define USE_COBS
#if defined(USE_COBS)
#include "cobs.h"
uint8_t encodeBuffer[2] = {0}; // COBS
#endif

// ============================================================================
// COMPILE-TIME MCU BUS CLOCK LOOKUP
// ============================================================================
// Detects standard STM32 macro targets (set by the build system or toolchain IDE) and configures peripheral clock speeds
#if defined(STM32F469xx) || defined(STM32F479xx) || defined(STM32F429xx) || defined(STM32F439xx)
    #define APB1_CLOCK_FREQ   45000000U  // 45 MHz
    #define APB2_CLOCK_FREQ   90000000U  // 90 MHz

#elif defined(STM32F407xx) || defined(STM32F417xx) || defined(STM32F405xx) || defined(STM32F415xx)
    #define APB1_CLOCK_FREQ   42000000U  // 42 MHz
    #define APB2_CLOCK_FREQ   84000000U  // 84 MHz

#elif defined(STM32F411xE)
    #define APB1_CLOCK_FREQ   50000000U  // 50 MHz
    #define APB2_CLOCK_FREQ   100000000U // 100 MHz

#elif defined(STM32F401xC) || defined(STM32F401xE)
    #define APB1_CLOCK_FREQ   42000000U  // 42 MHz
    #define APB2_CLOCK_FREQ   84000000U  // 84 MHz

#else
    // Default Fallback configuration (e.g., Standard STM32F469 180MHz tree defaults)
    #define APB1_CLOCK_FREQ   45000000U
    #define APB2_CLOCK_FREQ   90000000U
#endif

// Default weak definitions so the linker doesn't complain if they are missing. We aren't using extern "C" to write C code.
// We are using it to turn off C++ name mangling so that the linker can successfully match and override your weak fallback function.
extern "C" {
    __attribute__((weak)) void UART_RxCpltCallback_DMA(const UartDriver& instance, std::span<const uint8_t> incoming_data) {
        (void)instance;
        (void)incoming_data; // Default: do nothing
    }

    __attribute__((weak)) void UART_RxCpltCallback_IT(const UartDriver& instance, std::span<const uint8_t> incoming_data) {
        (void)instance;
        (void)incoming_data; // Default: do nothing
    }
}

// ============================================================================
// HARDWARE ISOLATION LAYER
// ============================================================================

// Handles the strict, unchangeable hardware connections for UART3, acts as a pin guard so that other instances cannot steal these pins
static void USART3_LowLevelInit(void) {
    // Enable USART3 and GPIOB Clocks
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    // 1. CLAMP BOTH LINES HIGH: Set Pull-up (0b01) on PB10 (TX) and PB11 (RX)
    // This stops ? PB10 from sagging low during mode transition
    GPIOB->PUPDR &= ~((3U << 20) | (3U << 22)); // Clear bits for Pin 10 & 11
    GPIOB->PUPDR |=  ((1U << 20) | (1U << 22)); // Set Pull-up (01)

    // 2. MAP MUX FIRST: Route Pin 10 and Pin 11 to AF7 (USART3)
    // AFR[1] handles pins 8-15. Pin 10 is index 2, Pin 11 is index 3.
    GPIOB->AFR[1] &= ~((15U << 8) | (15U << 12)); // Clear 4-bit fields
    GPIOB->AFR[1] |=  ((7U  << 8) | (7U  << 12)); // Set AF7 (0111)

    // 3. ENGAGE PINS: Safely switch to Alternate Function Mode (0b10)
    GPIOB->MODER &= ~((3U << 20) | (3U << 22));
    GPIOB->MODER |=  ((2U << 20) | (2U << 22));

    // 4. SPEED: Configure High Speed (0b10) or Very High Speed (0b11)
    // Matching your original choice of '3' (Very High Speed)
    GPIOB->OSPEEDR &= ~((3U << 20) | (3U << 22));
    GPIOB->OSPEEDR |=  ((3U << 20) | (3U << 22));
}

// Handles the strict, unchangeable hardware connections for UART6, acts as a pin guard so that other instances cannot steal these pins
static void USART6_LowLevelInit(void) {
    // Enable peripheral clocks
    RCC->APB2ENR |= RCC_APB2ENR_USART6EN;
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOCEN;

    // Pulling up PC6 doesn't solve the issue of parasitic/ultra-sensitive behavior
    // (spurious bytes at startup): PC6 (Tx) must be pulled high with an external resistor (22K -> 100K)
    GPIOC->PUPDR &= ~((3U << 12) | (3U << 14));
    GPIOC->PUPDR |=  ((1U << 12) | (1U << 14)); //  Pull-up on PC6 (Tx) and PC7 (Rx)

    // 2. MAP MUX FIRST: Route Pin 6 and Pin 7 to AF8 (USART6)
    GPIOC->AFR[0] &= ~((15U << 24) | (15U << 28));
    GPIOC->AFR[0] |=  ((8U  << 24) | (8U  << 28));

    // 3. ENGAGE PINS: Now switch to Alternate Function Mode safely
    GPIOC->MODER &= ~((3U << 12) | (3U << 14));
    GPIOC->MODER |=  ((2U << 12) | (2U << 14));

    // 4. SPEED: Configure High/Medium Speed
    GPIOC->OSPEEDR &= ~((3U << 12) | (3U << 14));
    GPIOC->OSPEEDR |=  ((1U << 12) | (1U << 14));
}


// Pass the hardware attributes and function pointer directly into the compile-time configuration instance
inline constexpr UartHardwareConfig Uart3Config {
	USART3, 			// .usart
	DMA1_Stream1, 		// .rxStream
	DMA1_Stream3, 		// .txStream
	4, 					// .dmaChannel
	DMA1, 				// .dmaBase
	USART3_IRQn, 		// .usartIrq
	DMA1_Stream3_IRQn,  // .txDmaIrq
	USART3_LowLevelInit // .lowLevelInit - hard-bound by construction
};

inline constexpr UartHardwareConfig Uart6Config {
	USART6, 			// .usart
	DMA2_Stream1, 		// .rxStream
	DMA2_Stream6, 		// .txStream
	5, 					// .dmaChannel
	DMA2, 				// .dmaBase
	USART6_IRQn, 		// .usartIrq
	DMA2_Stream6_IRQn,  // .txDmaIrq
	USART6_LowLevelInit // .lowLevelInit - hard-bound by construction
};


// ============================================================================
// DRIVER CORE IMPLEMENTATION
// ============================================================================

BareM_StatusTypeDef UartDriver::init(uint32_t baudrate) {
    // 1. Execute the hard-bound low-level clock/pin configuration
    if (config.lowLevelInit != nullptr) {
        config.lowLevelInit();
    }

    // 2. CRUCIAL: Clear configuration flags first, but keep UE (Peripheral Enable) DISABLED for now
    config.usart->CR1 &= ~(USART_CR1_UE | USART_CR1_M | USART_CR1_RE | USART_CR1_TE);

    // 3. Calculate and Set Baud Rate safely while the peripheral hardware engine is quiet
    // Decoupled dynamic bus configuration based on target MCU compilation definitions
    uint32_t pclk = APB1_CLOCK_FREQ;
    if (config.usart == USART1 || config.usart == USART6) {
        pclk = APB2_CLOCK_FREQ;
    }
    uint32_t usartdiv = (pclk + (baudrate / 2)) / baudrate;
    config.usart->BRR = usartdiv;

    // 4. Configure NVIC Interrupt Vectors
    NVIC_SetPriority(config.usartIrq, 4);
    NVIC_EnableIRQ(config.usartIrq);

    // 5. Explicitly initialize state links
    this->tx_link = LinkState::Idle;
    this->rx_link = LinkState::Idle;

    // 6. CLEAR GLITCHES: Force-clear any dirty status flags locked up during the step 1 pin-switch
    [[maybe_unused]] volatile uint32_t tmpreg = config.usart->SR;
    [[maybe_unused]] volatile uint32_t tmpdr  = config.usart->DR;

    // 7. FINALIZE: Enable the engine and the receiver/transmitter simultaneously
    config.usart->CR1 |= (USART_CR1_UE | USART_CR1_RE | USART_CR1_TE);

    return Bare_OK;
}

void UartDriver::ConfigureDma() {
    if (this->isDmaInitialized) return; // Exit immediately if already run once

    // Enable DMA Clock dynamically based on instance binding
    if (config.dmaBase == DMA1) {
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA1EN;
    }
    else if (config.dmaBase == DMA2) {
        RCC->AHB1ENR |= RCC_AHB1ENR_DMA2EN;
    }

    // Read back RCC to create the necessary timing propagation delay
    [[maybe_unused]] volatile uint32_t delay_read = RCC->AHB1ENR;

    // 1. Configure DMA RX Stream (Circular buffer routing)
    config.rxStream->CR &= ~DMA_SxCR_EN;
    while (config.rxStream->CR & DMA_SxCR_EN);

    config.rxStream->CR |= (config.dmaChannel << DMA_SxCR_CHSEL_Pos)
                        | DMA_SxCR_PL_0   // Priority: Medium
                        | DMA_SxCR_MINC   // Memory Increment Mode enabled
                        | DMA_SxCR_CIRC   // Circular Mode enabled
                        | DMA_SxCR_TCIE;   // Interrupt at byte 1024
                        //| DMA_SxCR_HTIE;  // Interrupt at byte 512

    // 2. Configure DMA TX Stream (Normal transmission mode)
    config.txStream->CR &= ~DMA_SxCR_EN;
    while (config.txStream->CR & DMA_SxCR_EN);

    config.txStream->CR |= (config.dmaChannel << DMA_SxCR_CHSEL_Pos)
                        | DMA_SxCR_PL_0   // Priority: Medium
                        | DMA_SxCR_MINC   // Memory Increment Mode enabled
                        | DMA_SxCR_DIR_0  // Direction: Memory-to-peripheral
                        | DMA_SxCR_TCIE;  // Transfer Complete Interrupt Enable

    NVIC_SetPriority(config.txDmaIrq, 5);
    NVIC_EnableIRQ(config.txDmaIrq);

    // 3. Mark as initialized so this blocks never executes again
    this->isDmaInitialized = true;
}

UartDriver uart3(Uart3Config); // Global Instance allocation in the memory section (uart3 is extern)
UartDriver uart6(Uart6Config); // Global Instance allocation in the memory section (uart6 is extern)

// ============================================================================
// Functions DMA MODE
// ============================================================================

BareM_StatusTypeDef UartDriver::UART_Transmit_DMA(std::span<const uint8_t> message) {
	// std::span cleanly captures data layout pointers at zero runtime cost
	ConfigureDma();

	// Wait until the TX DMA stream is disabled and UART hardware shift register is empty (crucial)
	while ((config.txStream->CR & DMA_SxCR_EN) || !(config.usart->SR & USART_SR_TC));
	// Check ONLY if the TX wire is busy. RX can be doing whatever it wants!
	uint32_t timeout_counter = GetSysTick();
	while (this->tx_link != LinkState::Idle) {
		if (GetSysTick() - timeout_counter > 5) return Bare_TIMEOUT;
	}
	    // Mask 0x3DUL (0b111101) sets all 5 active flags to 1 to clear any pending stream interrupts (TCIF, HTIF, TEIF, DMEIF,FEIF)
	    // PORTING WARNING: If you change USART instances or DMA streams, you MUST adjust these register destinations and bit-shift values
	if (config.usart == USART3) {
		config.dmaBase->LIFCR = (0x3DUL << 22); // The DMA status/clear flags are split across LIFCR (Streams 0-3)
	}
	else if (config.usart == USART6) {
		config.dmaBase->HIFCR = (0x3DUL << 16); // and HIFCR (Streams 4-7).
	}
	config.usart->SR &= ~USART_SR_TC; // Clear UART transmission complete flag

	this->tx_link = LinkState::DMA; // Claim ONLY the TX highway
	uint16_t final_tx_length = 0;

#if defined(USE_COBS)
    // Calculate the maximum safe raw input allowed based on your BufferSize limits
    // For a BufferSize of 1024, max raw input is 1021 bytes to guarantee the encoded result fits.
    constexpr size_t maxRawInputSize = BufferSize - (BufferSize / 1021 + 1) - 1;
    size_t length_to_encode = std::min(message.size(), maxRawInputSize);

    // Encode directly from the span data into your permanent asynchronous class buffer
    cobs_encode_result encode_result = cobs_encode(
        txBuffer_DMA,
        BufferSize,
        message.data(),
        length_to_encode
    );

    if (encode_result.status != COBS_ENCODE_OK) {
    	this->tx_link = LinkState::Idle; // Reset state machine on error
        return Bare_ERROR;
    }
    // Append the mandatory framing delimiter zero marker at the end of the packet frame
    if (encode_result.out_len < BufferSize) {
        txBuffer_DMA[encode_result.out_len] = 0x00;
        final_tx_length = static_cast<uint16_t>(encode_result.out_len + 1);
    } else {
    	this->tx_link = LinkState::Idle;
        return Bare_ERROR; // Out of bounds safety check
    }

#else
    // Standard Transmission Layer (No COBS modification)
    size_t length = std::min(message.size(), BufferSize);
    // Because DMA transmissions happen asynchronously in the background while the CPU moves on,
    // you cannot point the DMA directly to message.data(). If you do, the DMA might transmit corrupted stack memory
    std::memcpy(txBuffer_DMA, message.data(), length); // Decouples the memory by copying the span into a dedicated, permanent internal class buffer
    final_tx_length = static_cast<uint16_t>(length);
#endif

    // The internal config structural elements replace the local pointers entirely
    config.txStream->PAR  = reinterpret_cast<uint32_t>(&config.usart->DR);
    config.txStream->M0AR = reinterpret_cast<uint32_t>(txBuffer_DMA);
    config.txStream->NDTR = final_tx_length;

    // Mask 0x3DUL (0b111101) sets all 5 active flags to 1 to clear any pending stream interrupts (TCIF, HTIF, TEIF, DMEIF,FEIF)
    // PORTING WARNING: If you change USART instances or DMA streams, you MUST adjust these register destinations and bit-shift values
    if (config.usart == USART3) {
    	DMA1->LIFCR = (0x3DUL << 16);  // Clear DMA1 Stream 3 flags
    }
    else if (config.usart == USART6) {
    	DMA2->HIFCR = (0x3DUL << 16);   // Clear DMA2 Stream 6 flags (Lives in HISR!)
    }
    config.usart->CR3 	|= USART_CR3_DMAT;
    config.usart->CR1 	|= USART_CR1_TE;
    config.txStream->CR |= DMA_SxCR_EN;

    return Bare_OK;
}

BareM_StatusTypeDef UartDriver::startReceiveToIdle_DMA(std::span<uint8_t> user_buffer) {
	if (user_buffer.empty()) return Bare_ERROR;
	ConfigureDma();

    uint32_t timeout_counter = GetSysTick();
    config.rxStream->CR &= ~DMA_SxCR_EN; // Safely disable the RX DMA stream to reconfigure it

    // Wait for hardware to fully flush and disable the stream (Max 5ms)
    while (config.rxStream->CR & DMA_SxCR_EN || !(config.usart->SR & USART_SR_TC)) {
        if (GetSysTick() - timeout_counter > 5) {
            return Bare_TIMEOUT;
        }
    }
    // Bind the runtime buffer destinations
    this->pRxUserBuffer_DMA = user_buffer.data();
    this->rxMaxLen_DMA = static_cast<uint16_t>(user_buffer.size());

    this->rx_link = LinkState::DMA;
    this->last_rx_read_index = 0;

    // Configure memory addresses and buffer lengths natively via span
    config.rxStream->PAR  = reinterpret_cast<uint32_t>(&config.usart->DR);
    config.rxStream->M0AR = reinterpret_cast<uint32_t>(this->pRxUserBuffer_DMA);
    config.rxStream->NDTR = rxMaxLen_DMA; // size of the user-provided buffer

    // Clear hardware flags for Stream 1 (CTCIF1, CHTIF1, CTEIF1, CDMEIF1, CFEIF1)
    config.dmaBase->LIFCR = (0x3DUL << 6); // Works perfectly for DMA1 & DMA2 ! PORTING WARNING: check for your MCU !
    config.usart->CR3 |= USART_CR3_DMAR; // DMA mode is enabled for reception
    config.usart->CR1 |= USART_CR1_RE | USART_CR1_IDLEIE; // Enable receiver and IDLE line interrupt

    // Fire up the RX engine
    config.rxStream->CR |= DMA_SxCR_EN;

    return Bare_OK;
}

//  Asynchronous Deferred Processor Executed inside ISR Context
void UartDriver::invalidateAndFlushRx() {
    if (rxCallback_DMA == nullptr) return;

    uint16_t current_dma_index = getRxBufferIndex(); // Snapshot the current hardware position (NDTR) first

    if (current_dma_index > last_rx_read_index) {
        // Direct linear data segment invocation
    	rxCallback_DMA(*this, std::span<const uint8_t>(&pRxUserBuffer_DMA[last_rx_read_index], current_dma_index - last_rx_read_index));
    }
    else if (current_dma_index < last_rx_read_index) {
        // Hardware buffer wrap-around handling
    	rxCallback_DMA(*this, std::span<const uint8_t>(&pRxUserBuffer_DMA[last_rx_read_index], rxMaxLen_DMA - last_rx_read_index));
        if (current_dma_index > 0) {
            rxCallback_DMA(*this, std::span<const uint8_t>(&pRxUserBuffer_DMA[0], current_dma_index));
        }
    }
    last_rx_read_index = current_dma_index; // Update the pointer strictly to the snapshot above
    // If they are equal, nothing happens, indices stay exactly where they are!
}


BareM_StatusTypeDef UartDriver::stopReceiveToIdle_DMA() {
    uint32_t timeout_counter = GetSysTick();

    config.usart->CR1 &= ~USART_CR1_IDLEIE; // 1. Disable the IDLE Line Interrupt first so it doesn't fire during shutdown
    config.rxStream->CR &= ~DMA_SxCR_EN; // 2. Disable the RX DMA Stream
    // 3. Wait for hardware to fully flush and acknowledge the disabled state (Max 5ms)
    while (config.rxStream->CR & DMA_SxCR_EN) {
        if (GetSysTick() - timeout_counter > 5) {
            return Bare_TIMEOUT; // Hardware stream is jammed
        }
    }
    config.usart->CR3 &= ~USART_CR3_DMAR;  // Disconnect the USART receiver from the DMA engine

    // Works perfectly for both! No if-conditions needed.
    config.dmaBase->LIFCR = (0x3DUL << 6);   // Clear DMA1 & DMA2 Stream 1 flags PORTING WARNING: check for your MCU !
    volatile uint32_t dummy_sr = config.usart->SR;  // Clear the UART internal IDLE flag line by reading SR then DR
    volatile uint32_t dummy_dr = config.usart->DR;
    (void)dummy_sr; (void)dummy_dr;

    this->rx_link = LinkState::Idle;
    return Bare_OK;
}

// ============================================================================
// Functions INTERRUPT MODE
// ============================================================================

BareM_StatusTypeDef UartDriver::UART_Transmit_IT(std::span<const uint8_t> message) {
	uint32_t timeout_counter = GetSysTick();
	// Check ONLY if the TX wire is busy. RX can be doing whatever it wants!
	while (this->tx_link != LinkState::Idle) {
		if (GetSysTick() - timeout_counter > 5) return Bare_TIMEOUT;
	}

	this->tx_link = LinkState::Interrupt; // Claim ONLY the TX highway
	this->tx_index_IT = 0;

	#if defined(USE_COBS)
	    // Cobs overheads, calculate the maximum safe raw input allowed based on your txInterruptBuffer capacity.
	    // For a buffer size of 1024, max raw input is 1021 bytes to guarantee the encoded result fits.
		constexpr size_t maxRawInputSize = BufferSize - (BufferSize / 1021 + 1) - 1; // BufferSize is fully known at compile time (this-> is not)
	    size_t length_to_encode = std::min(message.size(), maxRawInputSize);

	    // Encode directly from the span data into the persistent interrupt buffer
	    cobs_encode_result encode_result = cobs_encode(
	        txInterruptBuffer.data(),
	        txInterruptBuffer.size(),
	        message.data(),
	        length_to_encode
	    );

	    if (encode_result.status != COBS_ENCODE_OK) {
	    	this->tx_link = LinkState::Idle; // Reset state machine on failure
	        return Bare_ERROR;
	    }

	    // Append the mandatory framing delimiter zero marker at the end of the packet frame
	    if (encode_result.out_len < txInterruptBuffer.size()) {
	        txInterruptBuffer[encode_result.out_len] = 0x00;
	        this->tx_len_IT = static_cast<uint16_t>(encode_result.out_len + 1);
	    } else {
	    	this->tx_link = LinkState::Idle;
	        return Bare_ERROR; // Out of bounds safety check
	    }
	#else
	    // Standard Transmission Layer (No COBS modification)
	    size_t length = std::min(message.size(), txInterruptBuffer.size()); // Clip length safely to prevent buffer overflows
	    this->tx_len_IT = static_cast<uint16_t>(length);
	    // Modern C++ copy mechanism
	    std::copy_n(message.data(), length, txInterruptBuffer.begin());
	#endif

	config.usart->SR &= ~USART_SR_TC;  // TC flag is cleared before enabling interrupts. This step is important, as TC might still be set from a previous transmission.
	config.usart->CR1 |= USART_CR1_TXEIE; // TXE interrupt enable: An USART interrupt is generated whenever TXE=1 in the USART_SR register. It is cleared by a write to the USART_DR register.
	/* and TXE=1 when idle ! (do not enable TXE interrupt until you have smthg to send. Disable it BEFORE writing the last char to be sent)
	TXE: Transmit data register empty: This bit is set by hardware when the content of the TDR register has been transferred into the shift register. */
	return Bare_OK;
}

BareM_StatusTypeDef UartDriver::UART_Receive_IT(std::span<uint8_t> user_buffer, bool waitIfBusy) {
	if (user_buffer.empty()) return Bare_ERROR;

	uint32_t timeout_counter = GetSysTick();
	while (this->rx_link != LinkState::Idle) {
		if (waitIfBusy) {
			if (GetSysTick() - timeout_counter > 10) {
				return Bare_TIMEOUT; // Unjams the CPU and reports the issue!
			}
		} else {
			return Bare_BUSY; // Act like the ST HAL: Return immediately if busy
		}
	}
	// Bind the runtime buffer destinations
	this->pRxUserBuffer_IT = user_buffer.data();
    this->rxMaxLen_IT = static_cast<uint16_t>(user_buffer.size());

    this->rx_link = LinkState::Interrupt; // Claim ONLY the RX highway

    // Unmask the hardware interrupts to allow reception now
    config.usart->CR1 |= (USART_CR1_RXNEIE | USART_CR1_IDLEIE);

    return Bare_OK;
}

// ============================================================================
// INTERRUPT SERVICE ROUTINE
// ============================================================================

extern "C" {

void DMA1_Stream1_IRQHandler(void) {		// Rx IRQHandler
    if (DMA1->LISR & DMA_LISR_TCIF1) {
    	DMA1->LIFCR = DMA_LIFCR_CTCIF1; 	// Clear Transfer Complete flag
    	uart3.invalidateAndFlushRx();
    }
    if (DMA1->LISR & DMA_LISR_HTIF1) {
    	DMA1->LIFCR = DMA_LIFCR_CHTIF1;
    	uart3.invalidateAndFlushRx();
    }
}

void DMA1_Stream3_IRQHandler(void) {		// Tx IRQHandler
	if (DMA1->LISR & DMA_LISR_TCIF3) {
		DMA1->LIFCR = DMA_LIFCR_CTCIF3; 	// Clear TX Transfer Complete
	/*	Because LinkState is defined inside the private/protected scope of the UartDriver class, global functions
		cannot see it as a standalone type. They must access it using class scope resolution: UartDriver::LinkState::Idle. */
		uart3.tx_link = UartDriver::LinkState::Idle; // Release TX link status
	}
}

void DMA2_Stream1_IRQHandler(void) {        // Uart6 Rx DMA Handler
	if (DMA2->LISR & DMA_LISR_TCIF1) {
		DMA2->LIFCR = DMA_LIFCR_CTCIF1;     // Clear flags on DMA2 Base!
		uart6.invalidateAndFlushRx();
	}
	if (DMA2->LISR & DMA_LISR_HTIF1) {
		DMA2->LIFCR = DMA_LIFCR_CHTIF1;
		uart6.invalidateAndFlushRx();
	}
}

void DMA2_Stream6_IRQHandler(void) {        // Uart6 Tx DMA Handler
	if (DMA2->HISR & DMA_HISR_TCIF6) {       // Note: Stream 6 flags live in HISR!
		DMA2->HIFCR = DMA_HIFCR_CTCIF6;
		uart6.tx_link = UartDriver::LinkState::Idle;
	}
}

void USART3_IRQHandler() {

	// HANDLE RX EVENTS
	if (uart3.rx_link == UartDriver::LinkState::Interrupt) {
		// receive UART bytes
		if (uart3.config.usart->SR & USART_SR_RXNE) { // 'Receive register not empty' interrupt; RXNE is cleared by a read to the USART_DR register
			uart3.rx_link = UartDriver::LinkState::Interrupt;
			uart3.pRxUserBuffer_IT[uart3.count_rx_IT] = uart3.config.usart->DR;    // Copy new data into the buffer
			uart3.count_rx_IT = uart3.count_rx_IT + 1;
			if (uart3.count_rx_IT >= uart3.rxMaxLen_IT) uart3.count_rx_IT = 0;  // Prevent overflowing the 1024-byte bufferRx array
		}
		// detect idle line, indicates the last character is received
		else if(uart3.config.usart->SR & USART_SR_IDLE) {
			volatile uint32_t dummy = uart3.config.usart->SR;
			dummy = uart3.config.usart->DR; (void)dummy; 	// Clear IDLE flag (Read SR then DR)
			// Turn off interrupts to close the reception window (Matches HAL style)
			uart3.config.usart->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
			uart3.rx_link = UartDriver::LinkState::Idle; // Clear RX link state separately!
			// Execute callback if there is data collected and the pointer is valid
			if (uart3.count_rx_IT > 0 && uart3.rxCallback_IT != nullptr) {
				// Pass exactly the slice of bytes that arrived
				uart3.rxCallback_IT(uart3, std::span<const uint8_t>(uart3.pRxUserBuffer_IT, uart3.count_rx_IT));
			}
			uart3.count_rx_IT = 0;
		}
	}
	else if (uart3.rx_link == UartDriver::LinkState::DMA) {
		if (uart3.config.usart->SR & USART_SR_IDLE) {
			// Safely clear IDLE flag and flush data
			volatile uint32_t dummy = uart3.config.usart->SR | uart3.config.usart->DR; (void)dummy;
			uart3.invalidateAndFlushRx();
			// Keep rx_link as LinkState::DMA because circular background listening continues!
		}
	}

	// 2. HANDLE TX EVENTS
	if (uart3.tx_link == UartDriver::LinkState::Interrupt) {
		// send UART bytes
		if ((uart3.config.usart->CR1 & USART_CR1_TXEIE) && (uart3.config.usart->SR & USART_SR_TXE)) {
			if (uart3.tx_index_IT < uart3.tx_len_IT) {
				uart3.tx_link = UartDriver::LinkState::Interrupt;
				uart3.config.usart->DR = uart3.txInterruptBuffer[uart3.tx_index_IT]; // Read and assign, then modify and write back safely
				uart3.tx_index_IT = uart3.tx_index_IT + 1;  // Avoid the warning: '++' expression of 'volatile'-qualified type is deprecated
			}
			if (uart3.tx_index_IT == uart3.tx_len_IT)	{
				uart3.config.usart->CR1 |= USART_CR1_TCIE; // TCIE: Transmission complete interrupt enable
				uart3.config.usart->CR1 &= ~USART_CR1_TXEIE;
				// No need to memset/clear the buffer here: tx_len_IT and tx_index_IT guard the index access,
				// leaving old data in memory has zero performance impact or side effects.
			}
		}
		// This bit is set by hw if the transmit of a frame is complete and if TXE is set.
		else if ((uart3.config.usart->SR & USART_SR_TC) && (uart3.config.usart->CR1 & USART_CR1_TCIE)) {
			uart3.config.usart->SR &= ~USART_SR_TC; // clear USART_SR_TC;
			uart3.config.usart->CR1 &= ~USART_CR1_TCIE;
			uart3.tx_link = UartDriver::LinkState::Idle;
		}
	}

	// Aggressive error clearing
	if (uart3.config.usart->SR & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
		volatile uint32_t dummy_sr = uart3.config.usart->SR;
		volatile uint32_t dummy_dr = uart3.config.usart->DR;
		(void)dummy_sr;
		(void)dummy_dr; // Reading SR followed by DR clears all core errors
	}
}


void USART6_IRQHandler() {

	// HANDLE RX EVENTS
	if (uart6.rx_link == UartDriver::LinkState::Interrupt) {
		// receive UART bytes
		if (uart6.config.usart->SR & USART_SR_RXNE) { // 'Receive register not empty' interrupt; RXNE is cleared by a read to the USART_DR register
			uart6.rx_link = UartDriver::LinkState::Interrupt;
			uart6.pRxUserBuffer_IT[uart6.count_rx_IT] = uart6.config.usart->DR;    // Copy new data into the buffer
			uart6.count_rx_IT = uart6.count_rx_IT + 1;
			if (uart6.count_rx_IT >= uart6.rxMaxLen_IT) uart6.count_rx_IT = 0;  // Prevent overflowing the 1024-byte bufferRx array
		}
		// detect idle line, indicates the last character is received
		else if(uart6.config.usart->SR & USART_SR_IDLE) {
			volatile uint32_t dummy = uart6.config.usart->SR;
			dummy = uart6.config.usart->DR; (void)dummy; 	// Clear IDLE flag (Read SR then DR)
			// Turn off interrupts to close the reception window (Matches HAL style)
			uart6.config.usart->CR1 &= ~(USART_CR1_RXNEIE | USART_CR1_IDLEIE);
			uart6.rx_link = UartDriver::LinkState::Idle; // Clear RX link state separately!
			// Execute callback if there is data collected and the pointer is valid
			if (uart6.count_rx_IT > 0 && uart6.rxCallback_IT != nullptr) {
				// Pass exactly the slice of bytes that arrived
				uart6.rxCallback_IT(uart6, std::span<const uint8_t>(uart6.pRxUserBuffer_IT, uart6.count_rx_IT));
			}
			uart6.count_rx_IT = 0;
		}
	}
	else if (uart6.rx_link == UartDriver::LinkState::DMA) {
		if (uart6.config.usart->SR & USART_SR_IDLE) {
			// Safely clear IDLE flag and flush data
			volatile uint32_t dummy = uart6.config.usart->SR | uart6.config.usart->DR; (void)dummy;
			uart6.invalidateAndFlushRx();
			// Keep rx_link as LinkState::DMA because circular background listening continues!
		}
	}

	// 2. HANDLE TX EVENTS
	if (uart6.tx_link == UartDriver::LinkState::Interrupt) {
		// send UART bytes
		if ((uart6.config.usart->CR1 & USART_CR1_TXEIE) && (uart6.config.usart->SR & USART_SR_TXE)) {
			if (uart6.tx_index_IT < uart6.tx_len_IT) {
				uart6.tx_link = UartDriver::LinkState::Interrupt;
				uart6.config.usart->DR = uart6.txInterruptBuffer[uart6.tx_index_IT]; // Read and assign, then modify and write back safely
				uart6.tx_index_IT = uart6.tx_index_IT + 1;  // Avoid the warning: '++' expression of 'volatile'-qualified type is deprecated
			}
			if (uart6.tx_index_IT == uart6.tx_len_IT)	{
				uart6.config.usart->CR1 |= USART_CR1_TCIE; // TCIE: Transmission complete interrupt enable
				uart6.config.usart->CR1 &= ~USART_CR1_TXEIE;
				// No need to memset/clear the buffer here: tx_len_IT and tx_index_IT guard the index access,
				// leaving old data in memory has zero performance impact or side effects.
			}
		}
		// This bit is set by hw if the transmit of a frame is complete and if TXE is set.
		else if ((uart6.config.usart->SR & USART_SR_TC) && (uart6.config.usart->CR1 & USART_CR1_TCIE)) {
			uart6.config.usart->SR &= ~USART_SR_TC; // clear USART_SR_TC;
			uart6.config.usart->CR1 &= ~USART_CR1_TCIE;
			uart6.tx_link = UartDriver::LinkState::Idle;
		}
	}

	// Aggressive error clearing
	if (uart6.config.usart->SR & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
		volatile uint32_t dummy_sr = uart6.config.usart->SR;
		volatile uint32_t dummy_dr = uart6.config.usart->DR;
		(void)dummy_sr;
		(void)dummy_dr; // Reading SR followed by DR clears all core errors
	}
}

} // extern "C"


// ============================================================================
// Functions POLLING MODE
// ============================================================================

BareM_StatusTypeDef UartDriver::UART_Transmit(std::span<const uint8_t> message, uint32_t timeout_ms) {
    if (message.empty()) return Bare_OK;

    // 1. Synchronize: Wait for any asynchronous TX operations (DMA/IT) to complete
    uint32_t start_tick = GetSysTick();
    while (this->tx_link != UartDriver::LinkState::Idle) {
        if (GetSysTick() - start_tick > timeout_ms) return Bare_TIMEOUT;
    }

    // 2. Claim the TX highway for Polling
    this->tx_link = UartDriver::LinkState::Polling;

    // 3. Clear the hardware Transmission Complete flag before beginning
    config.usart->SR &= ~USART_SR_TC;

    // 4. Loop through each character in the span
    for (uint8_t b : message) {
        // Wait for Transmit Data Register to be Empty (TXE)
        while (!(config.usart->SR & USART_SR_TXE)) {
            if (GetSysTick() - start_tick > timeout_ms) {
                this->tx_link = UartDriver::LinkState::Idle; // Release line on error
                return Bare_TIMEOUT;
            }
        }
        // Write byte directly to the Data Register
        config.usart->DR = b;
    }

    // 5. Wait for the final character to shift completely off the wire (TC)
    while (!(config.usart->SR & USART_SR_TC)) {
        if (GetSysTick() - start_tick > timeout_ms) {
            this->tx_link = UartDriver::LinkState::Idle;
            return Bare_TIMEOUT;
        }
    }

    // 6. Release the highway
    this->tx_link = UartDriver::LinkState::Idle;
    return Bare_OK;
}

BareM_StatusTypeDef UartDriver::UART_Receive(std::span<uint8_t> user_buffer, uint32_t timeout_ms) {
    if (user_buffer.empty()) return Bare_ERROR;

    // 1. Synchronize: Wait for any active asynchronous RX operations to yield
    uint32_t start_tick = GetSysTick();
    while (this->rx_link != UartDriver::LinkState::Idle) {
        if (GetSysTick() - start_tick > timeout_ms) return Bare_TIMEOUT;
    }

    // 2. Claim the RX highway for Polling
    this->rx_link = UartDriver::LinkState::Polling;

    // 3. Step through user buffer and fill it byte-by-byte
    for (size_t i = 0; i < user_buffer.size(); ++i) {
        // Wait for Read Data Register Not Empty (RXNE) flag
        while (!(config.usart->SR & USART_SR_RXNE)) {

            // Check for hardware overrun/noise/framing errors while waiting and clear them
            if (config.usart->SR & (USART_SR_ORE | USART_SR_NE | USART_SR_FE | USART_SR_PE)) {
                volatile uint32_t dummy_sr = config.usart->SR;
                volatile uint32_t dummy_dr = config.usart->DR;
                (void)dummy_sr; (void)dummy_dr;
            }

            if (GetSysTick() - start_tick > timeout_ms) {
                this->rx_link = UartDriver::LinkState::Idle; // Release line on error
                return Bare_TIMEOUT;
            }
        }

        // Read byte directly from Data Register (clears RXNE automatically)
        user_buffer[i] = static_cast<uint8_t>(config.usart->DR);
    }

    // 4. Release the highway
    this->rx_link = UartDriver::LinkState::Idle;
    return Bare_OK;
}
