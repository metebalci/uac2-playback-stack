/* SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Mete Balci
 *
 * board.c: the NUCLEO-U5A5ZJ-Q (STM32U5A5ZJT6Q, 16 MHz HSE crystal; LD1
 * green on PC7, LD2 blue on PB7; ST-LINK VCP on USART1, PA9 TX and PA10
 * RX; the user USB-C port on the OTG_HS PHY, its VBUS on PC2) for the u2ps
 * test app. Register level, with ST's CMSIS device header; RM0456 Rev 7
 * sections.
 */
#include "board.h"

#include "stm32u5xx.h"

#define HSE_HZ  16000000u
#define SYS_HZ  160000000u
#define VCP_BAUD 115200u

/* Interrupt priorities (4 bits): the USB vector above everything else that
 * runs (set by the app), then the VCP, then the tick */
#define PRIO_VCP  3u
#define PRIO_TICK 4u

static volatile uint32_t millis;

/* Spins until (reg & mask) == value; false after about 'ms' milliseconds
 * at the reset clock (4 MHz MSIS) or more */
static bool wait_bits(volatile const uint32_t *reg, uint32_t mask, uint32_t value, uint32_t ms)
{
  for (uint32_t n = ms * 4000u; (*reg & mask) != value; n--) {
    if (n == 0u)
      return false;
  }
  return true;
}

/* 160 MHz SYSCLK from PLL1 on the HSE, in voltage range 1 with the EPOD
 * booster (RM0456 10.5.4, p. 410-411): PLL1 source and booster clock
 * first, then VOS and BOOSTEN, flash wait states, PLL1, and the switch
 * above 80 MHz through an AHB prescaler of 2 */
static bool clock_init(void)
{
  RCC->AHB3ENR |= RCC_AHB3ENR_PWREN;
  (void)RCC->AHB3ENR;

  RCC->CR |= RCC_CR_HSEON;
  if (!wait_bits(&RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY, 100u))
    return false;

  /* PLL1: HSE / 1 = 16 MHz in (8-16 MHz range); booster input 16 MHz,
   * PLL1MBOOST = /1 (4-16 MHz, RM0456 11.8.9); R output only */
  RCC->PLL1CFGR = (3u << RCC_PLL1CFGR_PLL1SRC_Pos) | (3u << RCC_PLL1CFGR_PLL1RGE_Pos) |
                  RCC_PLL1CFGR_PLL1REN;
  PWR->VOSR = (PWR->VOSR & ~PWR_VOSR_VOS) | (3u << PWR_VOSR_VOS_Pos) | PWR_VOSR_BOOSTEN;
  if (!wait_bits(&PWR->VOSR, PWR_VOSR_VOSRDY | PWR_VOSR_BOOSTRDY,
                 PWR_VOSR_VOSRDY | PWR_VOSR_BOOSTRDY, 100u))
    return false;

  /* 4 wait states up to 160 MHz in range 1 (RM0456 7.3.3, Table 54) */
  FLASH->ACR = (FLASH->ACR & ~FLASH_ACR_LATENCY) | 4u | FLASH_ACR_PRFTEN;
  if ((FLASH->ACR & FLASH_ACR_LATENCY) != 4u)
    return false;

  /* VCO 16 MHz * 20 = 320 MHz; R = 2: 160 MHz; P = 8, Q = 2 (unused) */
  RCC->PLL1DIVR = ((20u - 1u) << RCC_PLL1DIVR_PLL1N_Pos) | ((8u - 1u) << RCC_PLL1DIVR_PLL1P_Pos) |
                  ((2u - 1u) << RCC_PLL1DIVR_PLL1Q_Pos) | ((2u - 1u) << RCC_PLL1DIVR_PLL1R_Pos);
  RCC->PLL1FRACR = 0u;
  RCC->CR |= RCC_CR_PLL1ON;
  if (!wait_bits(&RCC->CR, RCC_CR_PLL1RDY, RCC_CR_PLL1RDY, 100u))
    return false;

  /* More than 80 MHz at once is not allowed in range 1 (RM0456 10.5.4,
   * p. 411): HCLK = SYSCLK / 2 for the switch, 5 us, then / 1 */
  RCC->CFGR2 = (RCC->CFGR2 & ~RCC_CFGR2_HPRE) | (8u << RCC_CFGR2_HPRE_Pos);
  RCC->CFGR1 = (RCC->CFGR1 & ~RCC_CFGR1_SW) | (3u << RCC_CFGR1_SW_Pos);
  if (!wait_bits(&RCC->CFGR1, RCC_CFGR1_SWS, 3u << RCC_CFGR1_SWS_Pos, 100u))
    return false;
  for (volatile uint32_t n = 0; n < 1000u; n++) {
  }
  RCC->CFGR2 &= ~RCC_CFGR2_HPRE;
  return true;
}

static void gpio_mode(GPIO_TypeDef *port, uint32_t pin, uint32_t mode, uint32_t af)
{
  port->MODER = (port->MODER & ~(3u << (2u * pin))) | (mode << (2u * pin));
  port->OSPEEDR = (port->OSPEEDR & ~(3u << (2u * pin))) | (1u << (2u * pin));
  if (mode == 2u) {
    volatile uint32_t *afr = &port->AFR[pin / 8u];
    *afr = (*afr & ~(0xFu << (4u * (pin % 8u)))) | (af << (4u * (pin % 8u)));
  }
}

/* ---- VCP -------------------------------------------------------------------- */

#define TX_SIZE 4096u
#define RX_SIZE 128u

static char tx_buf[TX_SIZE];
static volatile uint32_t tx_head, tx_tail; /* head: thread writes; tail: interrupt reads */
static uint32_t tx_dropped;
static char rx_line[RX_SIZE];
static char rx_done[RX_SIZE];
static volatile uint32_t rx_len;
static volatile bool rx_ready;

void board_vcp_write(const char *s)
{
  uint32_t head = tx_head;
  for (; *s != '\0'; s++) {
    uint32_t next = (head + 1u) % TX_SIZE;
    if (next == tx_tail) {
      tx_dropped++;
      continue;
    }
    tx_buf[head] = *s;
    head = next;
  }
  tx_head = head;
  USART1->CR1 |= USART_CR1_TXEIE_TXFNFIE;
}

uint32_t board_vcp_dropped(void)
{
  return tx_dropped;
}

const char *board_vcp_line(void)
{
  static char line[RX_SIZE];
  if (!rx_ready)
    return 0;
  for (uint32_t i = 0; i < RX_SIZE; i++)
    line[i] = rx_done[i];
  rx_ready = false;
  return line;
}

void USART1_IRQHandler(void);
void USART1_IRQHandler(void)
{
  uint32_t isr = USART1->ISR;
  if ((isr & USART_ISR_RXNE_RXFNE) != 0u) {
    char c = (char)USART1->RDR;
    if (c == '\r' || c == '\n') {
      if (rx_len != 0u && !rx_ready) {
        for (uint32_t i = 0; i < rx_len; i++)
          rx_done[i] = rx_line[i];
        rx_done[rx_len] = '\0';
        rx_ready = true;
      }
      rx_len = 0;
    } else if (rx_len + 1u < RX_SIZE) {
      rx_line[rx_len++] = c;
    }
  }
  if ((isr & (USART_ISR_ORE | USART_ISR_FE | USART_ISR_NE)) != 0u)
    USART1->ICR = USART_ICR_ORECF | USART_ICR_FECF | USART_ICR_NECF;
  if ((USART1->CR1 & USART_CR1_TXEIE_TXFNFIE) != 0u && (isr & USART_ISR_TXE_TXFNF) != 0u) {
    uint32_t tail = tx_tail;
    if (tail != tx_head) {
      USART1->TDR = (uint8_t)tx_buf[tail];
      tx_tail = (tail + 1u) % TX_SIZE;
    } else {
      USART1->CR1 &= ~USART_CR1_TXEIE_TXFNFIE;
    }
  }
}

/* ---- VBUS ------------------------------------------------------------------- */

/* VBUS of the user USB-C port (CN15) reaches PC2, VBUS_SENSE (UM2861 Rev 5,
 * Table 13), divided by 7.61 (49.9 kOhm of 379.9 kOhm, as ST's NUCLEO
 * board support states it), so 0.66 V at 5 V, below a GPIO's input high
 * level: ADC1 channel 3 converts it continuously, at 14 bits (RES 00,
 * RM0456 33.6.4) against VDDA = 3.3 V. Present at or above 3.5 V. */
#define VBUS_DIVIDER_X100 761u
#define VBUS_MIN_MV       3500u
#define VDDA_MV           3300u
#define ADC_FULL          16384u
#define VBUS_MIN_CODE \
  ((uint32_t)((uint64_t)VBUS_MIN_MV * ADC_FULL * 100u / ((uint64_t)VDDA_MV * VBUS_DIVIDER_X100)))
#define ADC_START_MS 2u

/* ADC_CR's bits that a write of 1 starts something ("rs", RM0456 33.6.3):
 * written as 0 when another bit is set */
#define ADC_CR_RS                                                                         \
  (ADC_CR_ADCAL | ADC_CR_JADSTP | ADC_CR_ADSTP | ADC_CR_JADSTART | ADC_CR_ADSTART | \
   ADC_CR_ADDIS | ADC_CR_ADEN)

static bool vbus_adc; /* ADC1 converts VBUS_SENSE */

/* Waits for an ADC_ISR flag, at most ADC_START_MS plus one tick */
static bool adc_wait(uint32_t flag)
{
  uint32_t t0 = millis;
  while ((ADC1->ISR & flag) == 0u) {
    if (millis - t0 > ADC_START_MS)
      return false;
  }
  return true;
}

/* ADC1 on the HSE (16 MHz): VDDA valid (PWR_SVMCR.ASV, RM0456 10.10.5);
 * the kernel clock (ADCDACSEL 011, 11.8.48); out of Deep-power-down, the
 * regulator on and LDORDY (33.4.6, p. 1277); channel 3 preselected and
 * first in the sequence, sampled for 814 cycles, continuous, the data
 * overwritten on overrun; then ADEN and ADRDY (33.4.9, p. 1282), and the
 * conversions start. Without the HSE or a flag the ADC stays off, and
 * VBUS reads absent. */
static void vbus_init(void)
{
  gpio_mode(GPIOC, 2u, 3u, 0u);
  PWR->SVMCR |= PWR_SVMCR_ASV;
  RCC->CCIPR3 = (RCC->CCIPR3 & ~RCC_CCIPR3_ADCDACSEL) | (3u << RCC_CCIPR3_ADCDACSEL_Pos);
  RCC->AHB2ENR1 |= RCC_AHB2ENR1_ADC12EN;
  (void)RCC->AHB2ENR1;

  ADC1->CR &= ~(ADC_CR_DEEPPWD | ADC_CR_RS);
  ADC1->CR = (ADC1->CR & ~ADC_CR_RS) | ADC_CR_ADVREGEN;
  if (!adc_wait(ADC_ISR_LDORDY))
    return;
  ADC1->CFGR1 |= ADC_CFGR1_CONT | ADC_CFGR1_OVRMOD;
  ADC1->PCSEL |= 1u << 3;
  ADC1->SQR1 = (ADC1->SQR1 & ~(ADC_SQR1_L | ADC_SQR1_SQ1)) | (3u << ADC_SQR1_SQ1_Pos);
  ADC1->SMPR1 = (ADC1->SMPR1 & ~ADC_SMPR1_SMP3) | (7u << ADC_SMPR1_SMP3_Pos);
  ADC1->ISR = ADC_ISR_ADRDY;
  ADC1->CR = (ADC1->CR & ~ADC_CR_RS) | ADC_CR_ADEN;
  if (!adc_wait(ADC_ISR_ADRDY))
    return;
  ADC1->CR = (ADC1->CR & ~ADC_CR_RS) | ADC_CR_ADSTART;
  vbus_adc = true;
}

bool board_vbus_adc(void)
{
  return vbus_adc;
}

uint32_t board_vbus_mv(void)
{
  if (!vbus_adc)
    return 0u;
  uint32_t code = ADC1->DR & ADC_DR_RDATA;
  return (uint32_t)((uint64_t)code * VDDA_MV * VBUS_DIVIDER_X100 / (100u * ADC_FULL));
}

/* Absent if the ADC did not start, so that a failed reading never keeps
 * the pull-up on */
bool board_vbus(void)
{
  return vbus_adc && (ADC1->DR & ADC_DR_RDATA) >= VBUS_MIN_CODE;
}

/* ---- Tick, LEDs, start-up --------------------------------------------------- */

void SysTick_Handler(void);
void SysTick_Handler(void)
{
  millis++;
}

uint32_t board_millis(void)
{
  return millis;
}

void board_led_green(bool on)
{
  GPIOC->BSRR = on ? (1u << 7) : (1u << (7 + 16));
}

void board_led_blue(bool on)
{
  GPIOB->BSRR = on ? (1u << 7) : (1u << (7 + 16));
}

uint32_t board_idcode(void)
{
  return DBGMCU->IDCODE;
}

uint32_t board_cycles(void)
{
  return DWT->CYCCNT;
}

uint32_t board_cycles_per_us(void)
{
  return SYS_HZ / 1000000u;
}

uint32_t board_usb_frame(bool *hs)
{
  const USB_OTG_DeviceTypeDef *dev =
    (const USB_OTG_DeviceTypeDef *)(USB_OTG_HS_BASE + USB_OTG_DEVICE_BASE);
  uint32_t dsts = dev->DSTS;
  *hs = (dsts & USB_OTG_DSTS_ENUMSPD) == 0u;
  return (dsts & USB_OTG_DSTS_FNSOF) >> USB_OTG_DSTS_FNSOF_Pos;
}

extern uint32_t _sstack[]; /* the lowest address of the stack (linker script) */

bool board_init(void)
{
  /* A stack that grows past its room faults instead of overwriting .bss */
  __set_MSPLIM((uint32_t)_sstack);

  bool clocks = clock_init();
  SystemCoreClockUpdate();
  if (clocks) {
    /* The instruction cache (RM0456 8.7.1) */
    if (wait_bits(&ICACHE->SR, ICACHE_SR_BUSYF, 0u, 10u))
      ICACHE->CR |= ICACHE_CR_EN;
  }
  SysTick_Config(SystemCoreClock / 1000u);
  /* The cycle counter (Armv8-M DWT) */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0u;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
  NVIC_SetPriority(SysTick_IRQn, PRIO_TICK);

  RCC->AHB2ENR1 |= RCC_AHB2ENR1_GPIOAEN | RCC_AHB2ENR1_GPIOBEN | RCC_AHB2ENR1_GPIOCEN;
  (void)RCC->AHB2ENR1;
  vbus_init();
  gpio_mode(GPIOC, 7u, 1u, 0u);
  gpio_mode(GPIOB, 7u, 1u, 0u);
  board_led_green(false);
  board_led_blue(false);
  /* PA11 and PA12 stay analog, the reset state: "When PA11 and PA12 are
   * used as OTG_HS_DM and OTG_HS_DP additional functions, GPIOs must be
   * configured in analog mode" (RM0456 10.7.12, p. 438) */

  /* VCP: USART1 on PA9 and PA10, AF7, kernel clock PCLK2 (the reset
   * selection of USART1SEL) */
  gpio_mode(GPIOA, 9u, 2u, 7u);
  gpio_mode(GPIOA, 10u, 2u, 7u);
  RCC->APB2ENR |= RCC_APB2ENR_USART1EN;
  (void)RCC->APB2ENR;
  USART1->CR1 = 0u;
  USART1->BRR = (SystemCoreClock + VCP_BAUD / 2u) / VCP_BAUD;
  USART1->CR1 = USART_CR1_TE | USART_CR1_RE | USART_CR1_RXNEIE_RXFNEIE | USART_CR1_UE;
  NVIC_SetPriority(USART1_IRQn, PRIO_VCP);
  NVIC_EnableIRQ(USART1_IRQn);
  return clocks;
}

/* __libc_init_array calls _init; the C runtime's start files are not
 * linked */
void _init(void);
void _init(void)
{
}

void board_uid(uint32_t uid[3])
{
  const volatile uint32_t *p = (const volatile uint32_t *)UID_BASE;
  for (unsigned i = 0; i < 3u; i++)
    uid[i] = p[i];
}
