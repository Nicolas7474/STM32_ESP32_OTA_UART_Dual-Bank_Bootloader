//#include "stm32f469xx.h"
#include "stm32f4xx.h"
#include <myConfig.h>


void activateFPU(void) {

#if (__FPU_PRESENT == 1) && (__FPU_USED == 1)
	SCB->CPACR |= ((3UL << 20U)|(3UL << 22U));  /* set CP10 and CP11 Full Access */

	// Enable Lazy Stacking for better ISR performance : When an interrupt (like your TIM7 or TIM8 ISRs) occurs, the processor normally has to save all the FPU registers
	// to the stack. This is slow. Lazy Stacking tells the hardware only to save FPU registers if the ISR actually performs a floating-point operation.
	FPU->FPCCR |= FPU_FPCCR_LSPEN_Msk;
#endif
	// Enabling the hardware bits isn't enough; you must also tell your compiler (GCC, Clang, or Keil) to actually generate FPU instructions instead of using slow software libraries.
	// If you are using GCC (arm-none-eabi-gcc), add these flags to your build command:
	// -mfloat-abi=hard: Uses the hardware FPU for calculations and passing arguments.
	// -mfpu=fpv4-sp-d16: Specifies the specific FPU version on the STM32F4.
}


void SysClockConfig (void)
{
	#define PLL_M 	4
	#define PLL_N 	180
	#define PLL_P 	0  // PLLP = 2

	// 1. ENABLE HSE and wait for the HSE to become Ready
	RCC->CR |= RCC_CR_HSEON;  // RCC->CR |= 1<<16;
	while (!(RCC->CR & RCC_CR_HSERDY));  // while (!(RCC->CR & (1<<17)));

	// 2. Set the POWER ENABLE CLOCK and VOLTAGE REGULATOR
	RCC->APB1ENR |= RCC_APB1ENR_PWREN;  // RCC->APB1ENR |= 1<<28; RCC APB1 peripheral clock enable register (RCC_APB1ENR);
	PWR->CR |= PWR_CR_VOS;  // PWR->CR |= 3<<14; PWR power control register (PWR_CR);

	// 3. Configure the FLASH PREFETCH and the LATENCY Related Settings
	FLASH->ACR = FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN | FLASH_ACR_LATENCY_5WS;  // FLASH->ACR = (1<<8) | (1<<9)| (1<<10)| (5<<0);

	// 4. Configure the PRESCALARS HCLK, PCLK1, PCLK2
	RCC->CFGR &= ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2);
	// AHB PR
	RCC->CFGR |= RCC_CFGR_HPRE_DIV1;  // RCC->CFGR &= ~(1<<4); RCC clock configuration register (RCC_CFGR);
	// APB1 PR
	RCC->CFGR |= RCC_CFGR_PPRE1_DIV4;  // RCC->CFGR |= (5<<10);
	// APB2 PR
	RCC->CFGR |= RCC_CFGR_PPRE2_DIV2;  // RCC->CFGR |= (4<<13);

	// 5. Configure the MAIN PLL
	RCC->PLLCFGR = (PLL_M <<0) | (PLL_N << 6) | (PLL_P <<16) | (RCC_PLLCFGR_PLLSRC_HSE);  // (1<<22);

	// 6. Enable the PLL and wait for it to become ready
	RCC->CR |= RCC_CR_PLLON;  // RCC->CR |= (1<<24);
	while (!(RCC->CR & RCC_CR_PLLRDY));  // while (!(RCC->CR & (1<<25)));

	// 7. Select the Clock Source and wait for it to be set
	RCC->CFGR |= RCC_CFGR_SW_PLL;  // RCC->CFGR |= (2<<0);
	while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL);  // while (!(RCC->CFGR & (2<<2)));


	// ------------- set RTC Wakeup Timer 1Hz ----------------------- //

	PWR->CR |= PWR_CR_DBP; // (1U<<8); Disable backup domain write protection
		while((PWR->CR & PWR_CR_DBP) == 0);
	RCC->BDCR |= RCC_BDCR_BDRST; // (1U<<16); BDRST: Backup domain software reset (1: Resets the entire Backup domain)
	RCC->BDCR &= ~RCC_BDCR_BDRST; // 0: Reset not activated
	RCC->BDCR |= RCC_BDCR_LSEON; //(1U<<0); Enable LSE Clock source and wait until LSERDY bit to set
		while ((RCC->BDCR & RCC_BDCR_LSERDY) == 0); // (1U<<1)
	RCC->BDCR |= RCC_BDCR_RTCSEL; //  (1U<<8) Select LSE as RTC Clock
	RCC->BDCR &= ~(1U<<9); // Select LSE as RTC Clock (2nd bit)
	RCC->BDCR |= (1U<<15); // Enable RTC Clock

	RTC->WPR = 0xCA; // Disable the write protection for RTC registers. After backup domain reset, all the RTC registers
	RTC->WPR = 0x53; // are write-protected. Writing to the RTC registers is enabled by writing a key into the Write Protection register.
	RTC->CR &= ~RTC_CR_WUTE; //(1U<<10); Clear WUTE in RTC_CR to disable the wakeup timer before configuring it
	RTC->ISR = RTC_ISR_WUTWF; // (1U<<2); The wakeup timer values can be changed when WUTE bit is cleared and WUTWF is set. (1: Wakeup timer configuration update allowed)

	// Poll WUTWF until it is set in RTC_ICSR to make sure the access to wakeup autoreload counter and to WUCKSEL[2:0] bits is allowed
	if((RTC->ISR & (1U<<6))==0)
		while((RTC->ISR & RTC_ISR_WUTWF)==0);

	// Configure the Wakeup Timer counter and auto clear value
	RTC->WUTR = 0; // When the wakeup timer is enabled (WUTE set to 1), the WUTF flag is set every (WUT[15:0] + 1) ck_wut cycles
	RTC->PRER = 0xFF;         // Configure the RTC PRER ; Synchronus value set as 255
	RTC->PRER |= (0x7F<<16);    // Asynchronus value set as 127
	RTC->CR |= RTC_CR_WUCKSEL;  // WUCKSEL[2:0]: Configure the clock source; 10x: ck_spre (usually 1 Hz) clock is selected

	EXTI->IMR |= (1U<<22); // Configure and enable the EXTI Line 22 in interrupt mode
	EXTI->RTSR |= (1U<<22); // Rising edge trigger enabled (for Event and Interrupt) for input line 10

	RTC->CR |= RTC_CR_WUTIE; // Wakeup timer interrupt enabled (1U<<14)
	RTC->CR |= RTC_CR_WUTE; // Enable the Wakeup Timer (1U<<10)
	RTC->WPR = RTC_WPR_KEY; // Enable the write protection for RTC registers (0xFF)

	NVIC_SetPriority(RTC_WKUP_IRQn, 10);
	NVIC_EnableIRQ(RTC_WKUP_IRQn);
}


void GPIO_Config (void)
{
    // 1. ENABLE THE GPIO PERIPHERAL CLOCKS (Safe Bitwise OR)
    RCC->AHB1ENR |= (1 << 0);  // GPIO-A
    // Note: GPIO-B clock is already enabled in USART3_LowLevelInit
    RCC->AHB1ENR |= (1 << 3);  // GPIO-D
    RCC->AHB1ENR |= (1 << 6);  // GPIO-G
    RCC->AHB1ENR |= (1 << 10); // GPIO-K

    // 2. CONFIGURE PIN DIRECTIONS (MODER: 00=Input, 01=Output, 10=Alt, 11=Analog)
    // PA0 -> Input Mode (00)
    GPIOA->MODER &= ~(3U << 0);
    // PB12 -> Input Mode (00)
    GPIOB->MODER &= ~(3U << 24);
    // PA8 -> Input Mode (00)
    GPIOA->MODER &= ~(3U << 16);

    // PG6 -> Output Mode (01) [Green LED]
    GPIOG->MODER &= ~(3U << 12); // Clear bits 13:12
    GPIOG->MODER |=  (1U << 12); // Set bit 12 to 1 (01)
    // PG13 -> Output Mode (01)
    GPIOG->MODER &= ~(3U << 26); // Clear bits 27:26
    GPIOG->MODER |=  (1U << 26); // Set bit 26 to 1 (01)
    // PK3 -> Output Mode (01)
    GPIOK->MODER &= ~(3U << 6);  // Clear bits 7:6
    GPIOK->MODER |=  (1U << 6);  // Set bit 6 to 1 (01)
    // PD4 -> Output Mode (01) [Orange LED]
    GPIOD->MODER &= ~(3U << 8);  // Clear bits 9:8
    GPIOD->MODER |=  (1U << 8);  // Set bit 8 to 1 (01)
    // PD5 -> Output Mode (01) [Red LED]
    GPIOD->MODER &= ~(3U << 10); // Clear bits 11:10
    GPIOD->MODER |=  (1U << 10); // Set bit 10 to 1 (01)


    // 3. CONFIGURE PIN CHARACTERISTICS (PUPDR, OTYPER, OSPEEDR)
    // PA0 -> No Pull-Up, No Pull-Down (00)
    GPIOA->PUPDR &= ~(3U << 0);
    // PB12 -> No Pull-Up, No Pull-Down (00)
    GPIOB->PUPDR &= ~(3U << 24);
    // PA8 -> Pull-Up Resistor Enabled (01)
    GPIOA->PUPDR &= ~(3U << 16); // Clear bits 17:16
    GPIOA->PUPDR |=  (1U << 16); // Set to 01 (Pull-up)
    // PG6 & PG13 (Port G Outputs) -> Push-Pull, Low Speed (Safe masking)
    GPIOG->OTYPER  &= ~((1U << 6) | (1U << 13));  // 0 = Push-Pull
    GPIOG->OSPEEDR &= ~((3U << 12) | (3U << 26)); // 00 = Low Speed
    // PK3 (Port K Output) -> Push-Pull, Low Speed (Safe masking)
    GPIOK->OTYPER  &= ~(1U << 3);
    GPIOK->OSPEEDR &= ~(3U << 6);
    // PD4 & PD5 (Port D LEDs) -> Push-Pull
    GPIOD->OTYPER  &= ~((1U << 4) | (1U << 5));

    // 4. INITIAL BIT STATE
    // Drive PG13 high immediately using the Atomic Bit Set/Reset Register
    GPIOG->BSRR = (1U << 13);
}


void InterruptGPIO_Config (void)
{
	RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;  // Enable SYSCFGEN: System configuration controller clock enable
	SYSCFG->EXTICR[0] &= ~(0xF<<0); // Select Port A for EXTI0
	EXTI->PR = (1 << 0); // clear pending flag
	EXTI->IMR |= (1<<0);  // Unmask EXTI line 0
	EXTI->RTSR |= (1<<0);  // Enable Rising Edge Trigger for PA0 (Rising trigger selection register (EXTI_RTSR))
	EXTI->FTSR &= ~(1<<0);  // Disable Falling Edge Trigger for PA0 (Falling trigger selection register (EXTI_FTSR))
	NVIC_SetPriority(EXTI0_IRQn, 10);
	NVIC_EnableIRQ(EXTI0_IRQn);
}




