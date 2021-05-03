/* ACSI2STM Atari hard drive emulator
 * Copyright (C) 2019-2021 by Jean-Matthieu Coulon
 *
 * This Library is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with the program.  If not, see <http://www.gnu.org/licenses/>.
 */

#define A1 PB6 // Must be on port B
#define CS PB7 // Must be on port B
#define IRQ PA8
#define DRQ PA11 // Must be on Timer1 channel output
#define ACK PA12 // Must be on Timer1 external clock
// Data pins are on PC8-PB15

// Pin masks for direct port access
#define A1_MASK  0b0000000001000000
#define CS_MASK  0b0000000010000000
#define IRQ_MASK 0b0000000100000000
#define DRQ_MASK 0b0000100000000000
#define ACK_MASK 0b0001000000000000

// Timer
#define ACSI_TIMER TIMER1_BASE

/* How ACSI DMA is handled (DRQ/ACK pulses and data sampling):

Expected behavior
-----------------

DRQ is generated by the STM32, it triggers a DMA transfer.
ACK is generated by the ST, it tells the STM32 when the data bus is sampled.
DRQ must go high at most 180ns after ACK goes low. Failing to do that will
abort the DMA transfer by the ST.

DMA reads (STM32 -> ST)

          ___              _________
     DRQ     |____________|
          ______________         ___
     ACK                |_______|

    DATA                        S

Data seems to be sampled when ACK goes up (marked "S").
The STM32 keeps the data up for the whole transfer (DRQ+ACK), which avoids
risks of reading invalid data.


DMA writes (ST -> STM32)

          ___              _________
     DRQ     |____________|
          ______________         ___
     ACK                |_______|

    DATA               [========]

Data is guaranteed to be available during the whole ACK pulse (marked "[==]").


STM32 implementation
--------------------

DRQ and ACK pulses are too fast to use bit banging, even with direct port
access.

The current implementation uses STM32 timers and its DMA engine to process
these signals. Data flow:
             __________
         CLK|          |CH4
  ACK ----->|  Timer1  |-----> PA11 (DRQ)
            |          |
            |          |CH3
            |          |------------
            |__________|            |
                                    |Trigger (DMA1 CH6)
                      _______     __V___     __________
                     |       |   |      |   |          |
                     | GPIOB |-->| DMA1 |-->|  Timer1  |
                     |       |   |      |   |  CH1 CC  |
                     |_______|   |______|   |__________|

 * ACK is used as Timer1 clock.
 * PA11 (DRQ) is used as a PWM output that goes up whenever Timer1 receives a clock tick.
 * Timer1 triggers a STM32 DMA transfer whenever Timer1 receives a clock tick.
 * The STM32 DMA engine copies GPIOB to Timer1 CH1 compare value.
 * Timer1 CH1 is used as a simple buffer because GPIOB is considered as memory
   by the STM32 DMA engine and memory to memory copies cannot be triggered by
   a timer, so it has to be a memory to peripheral copy. Any unused peripheral
   register can be used for this task.
 * If multiple ACK signals are received, this can be detected by having an incorrect
   counter value. This avoids silent data corruption in case of problems. This check is
   only done if ACSI_CAREFUL_DMA is enabled.


DMA read process
----------------

DMA block transfer initialization process:

 * Set Timer1 counter to a high value so DRQ will be high when enabled
 * Enable DRQ in PWM mode (high if Timer1 > 0, low if Timer1 = 0)
 * Enable Timer1

DMA byte read process:

 * Data is put on the data bus.
 * Set Timer1 counter to 0, this will pull DRQ low.
 * When ACK goes low, Timer1 counts to 1.
 * Timer1 counting will set DRQ high.
 * Wait until ACK goes high.

DMA block transfer stop process:

 * Set DRQ pin as input
 * Disable Timer1


DMA write process
-----------------

DMA block transfer initialization process:

 * Set Timer1 counter to a high value so DRQ will be high when enabled
 * Enable DRQ in PWM mode (high if Timer1 > 0, low if Timer1 = 0)
 * Enable Timer1

DMA byte write process:

 * Set Timer1 counter to 0, this will pull DRQ low.
 * When ACK goes low, Timer1 counts to 1.
 * Timer1 counting will set DRQ high.
 * Timer1 counting will trigger the STM32 DMA CH6.
 * The STM32 DMA will copy GPIOB to Timer1 CH1 compare value.
 * Wait until ACK goes high.
 * Read Timer1 CH1 compare value to get the data byte.

DMA block transfer stop process:

 * Set DRQ pin as input
 * Disable Timer1

*/

#include "acsi2stm.h"
#include "Debug.h"
#include "Acsi.h"
#include <libmaple/dma.h>

void Acsi::begin(uint8_t mask) {
  deviceMask = mask;
  init();
}

void Acsi::init() {
  setupDrqTimer();
  setupAckDmaTransfer();
  setupGpio();
}

bool Acsi::idle() {
  return (GPIOA->regs->IDR & (IRQ_MASK | DRQ_MASK | ACK_MASK)) == IRQ_MASK | DRQ_MASK | ACK_MASK;
}

void Acsi::waitBusReady() {
  pinMode(CS, INPUT_PULLDOWN);
  pinMode(A1, INPUT_PULLDOWN);

  while((((GPIOB->regs->IDR) | ~(A1_MASK | CS_MASK)) != ~0) || !idle());

  pinMode(CS, INPUT);
  pinMode(A1, INPUT);
}

uint8_t Acsi::waitCommand(uint8_t mask) {
  uint16_t port;

  acsiVerbose("[+");

  // Disable systick that introduces jitter.
  systick_disable();

  do {
    // Read the command on the data pins along with the
    // A1 command start marker and the CS clock signal
    // This is done in a single operation because the
    // CS pulse is fast (250ns)
    while((port = GPIOB->regs->IDR) & (A1_MASK | CS_MASK));
  } while(!((1 << (port >> (8+5))) & mask) || !idle()); // Check the device ID and Ack line

  // If CS never goes up before the watchdog triggers, the cable is probably disconnected.
  while(!readCs());

  // Restore systick
  systick_enable();

  uint8_t byte = (uint8_t)(port >> 8);

  acsiVerbose(byte, HEX);
  acsiVerbose(']');

  return byte;
}

void Acsi::readIrq(uint8_t *bytes, int count) {
  // Disable systick that introduces jitter.
  systick_disable();

  while(count > 0) {
    *bytes = readIrq();
    ++bytes;
    --count;
  }

  // If CS never goes up before the watchdog triggers, the cable is probably disconnected.
  while(!readCs());

  // Restore systick
  systick_enable();
}

uint8_t Acsi::readIrq() {
  uint16_t b;

  acsiVerbose("[<");

  // Disable systick that introduces jitter.
  systick_disable();

  pullIrq();
  while((b = GPIOB->regs->IDR) & (CS_MASK)); // Read data and clock at the same time
  releaseRq();

  // Restore systick
  systick_enable();

  uint8_t byte = (uint8_t)(b >> 8);

  acsiVerbose(byte,HEX);
  acsiVerbose(']');

  return byte;
}

void Acsi::sendIrq(const uint8_t *bytes, int count) {
  while(count > 0) {
    sendIrq(*bytes);
    ++bytes;
    --count;
  }
}

void Acsi::sendIrq(uint8_t byte) {
  acsiVerbose("[>");
  acsiVerbose(byte,HEX);

  // Disable systick that introduces jitter.
  systick_disable();

  acquireDataBus();
  writeData(byte);
  pullIrq();
  while(readCs());
  while(!readCs());
  releaseBus();

  // Restore systick
  systick_enable();

  acsiVerbose(']');
}

void Acsi::readDma(uint8_t *bytes, int count) {
  // Disable systick that introduces jitter.
  systick_disable();

  acsiVerbose("DMA read ");

  acquireDrq();

  // Unroll for speed
  int i;
  for(i = 0; i <= count - 16; i += 16) {
#define ACSI_READ_BYTE(b) do { \
      ACSI_TIMER->CNT = 0; \
      while(ACSI_TIMER->CNT == 0); \
      bytes[b] = (uint8_t)(ACSI_TIMER->CCR1 >> 8); \
    } while(0)
    ACSI_READ_BYTE(0);
    ACSI_READ_BYTE(1);
    ACSI_READ_BYTE(2);
    ACSI_READ_BYTE(3);
    ACSI_READ_BYTE(4);
    ACSI_READ_BYTE(5);
    ACSI_READ_BYTE(6);
    ACSI_READ_BYTE(7);
    ACSI_READ_BYTE(8);
    ACSI_READ_BYTE(9);
    ACSI_READ_BYTE(10);
    ACSI_READ_BYTE(11);
    ACSI_READ_BYTE(12);
    ACSI_READ_BYTE(13);
    ACSI_READ_BYTE(14);
    ACSI_READ_BYTE(15);
    bytes += 16;
  }

  while(i < count) {
    ACSI_READ_BYTE(0);
    ++i;
    ++bytes;
  }

#undef ACSI_READ_BYTE

  releaseBus();

  // Restore systick
  systick_enable();

  acsiVerboseDump(&bytes[-i], i);
  acsiVerboseln(" OK");
}

void Acsi::sendDma(const uint8_t *bytes, int count) {
  acsiVerbose("DMA send ");
  acsiVerboseDump(&bytes[0], count);

  // Disable systick that introduces jitter.
  systick_disable();

  acquireDataBus();
  acquireDrq();

  // Unroll for speed
  int i;
  for(i = 0; i <= count - 16; i += 16) {
#define ACSI_SEND_BYTE(b) do { \
      writeData(bytes[b]); \
      ACSI_TIMER->CNT = 0; \
      while(ACSI_TIMER->CNT == 0); \
    } while(0)
    ACSI_SEND_BYTE(0);
    ACSI_SEND_BYTE(1);
    ACSI_SEND_BYTE(2);
    ACSI_SEND_BYTE(3);
    ACSI_SEND_BYTE(4);
    ACSI_SEND_BYTE(5);
    ACSI_SEND_BYTE(6);
    ACSI_SEND_BYTE(7);
    ACSI_SEND_BYTE(8);
    ACSI_SEND_BYTE(9);
    ACSI_SEND_BYTE(10);
    ACSI_SEND_BYTE(11);
    ACSI_SEND_BYTE(12);
    ACSI_SEND_BYTE(13);
    ACSI_SEND_BYTE(14);
    ACSI_SEND_BYTE(15);
    bytes += 16;
  }

  while(i < count) {
    ACSI_SEND_BYTE(0);
    ++i;
    ++bytes;
  }

#undef ACSI_SEND_BYTE
  releaseBus();

  // Restore systick
  systick_enable();

  acsiVerboseln(" OK");
}

void Acsi::releaseRq() {
  GPIOA->regs->CRH = 0x44444BB4; // Set ACK, IRQ and DRQ as inputs
}

void Acsi::releaseDataBus() {
  GPIOB->regs->CRH = 0x44444444; // Set PORTB[8:15] to input
}

void Acsi::releaseBus() {
  releaseDataBus();
  releaseRq();
}

void Acsi::acquireDrq() {
  // Set DRQ to high using timer PWM
  TIMER1_BASE->CNT = 2;

  // Transition through input pullup to avoid a hardware glitch
  GPIOA->regs->CRH = 0x44448BB4;

  // Enable timer PWM output to DRQ
  GPIOA->regs->CRH = 0x4444BBB4;
}

void Acsi::acquireDataBus() {
  GPIOB->regs->CRH = 0x33333333; // Set PORTB[8:15] to 50MHz push-pull output
}

bool Acsi::readCs() {
  return GPIOB->regs->IDR & CS_MASK;
}

bool Acsi::readAck() {
  return GPIOA->regs->IDR & ACK_MASK;
}

void Acsi::pullIrq() {
  GPIOA->regs->CRH = 0x44444BB3;
}

void Acsi::writeData(uint8_t byte) {
  GPIOB->regs->ODR = ((int)byte) << 8;
}

void Acsi::setupDrqTimer() {
  ACSI_TIMER->CR1 = TIMER_CR1_OPM;
  ACSI_TIMER->CR2 = 0;
  ACSI_TIMER->SMCR = TIMER_SMCR_ETP | TIMER_SMCR_TS_ETRF | TIMER_SMCR_SMS_EXTERNAL;
  ACSI_TIMER->PSC = 0; // Prescaler
  ACSI_TIMER->ARR = 65535; // Overflow (0 = counter stopped)
  ACSI_TIMER->DIER = TIMER_DIER_CC3DE;
  ACSI_TIMER->CCMR1 = 0;
  ACSI_TIMER->CCMR2 = TIMER_CCMR2_OC4M;
  ACSI_TIMER->CCER = TIMER_CCER_CC4E; // Enable output
  ACSI_TIMER->EGR = TIMER_EGR_UG;
  ACSI_TIMER->CCR2 = 65535; // Disable unused CC channel
  ACSI_TIMER->CCR3 = 1; // Compare value
  ACSI_TIMER->CCR4 = 1; // Compare value
  ACSI_TIMER->CNT = 2;
  ACSI_TIMER->CR1 |= TIMER_CR1_CEN;
}

void Acsi::setupAckDmaTransfer() {
  // Setup the DMA engine to copy GPIOB to timer 1 CH1 compare value
  dma_init(DMA1);
  dma_setup_transfer(DMA1,
                     DMA_CH6,
                     &(ACSI_TIMER->CCR1),
                     DMA_SIZE_16BITS,
                     &(GPIOB->regs->IDR),
                     DMA_SIZE_16BITS,
                     DMA_FROM_MEM | DMA_CIRC_MODE);
  dma_set_num_transfers(DMA1, DMA_CH6, 1);
  dma_enable(DMA1, DMA_CH6);
}

void Acsi::setupGpio() {
  GPIOA->regs->ODR |= DRQ_MASK;
  releaseBus();
}

// vim: ts=2 sw=2 sts=2 et
