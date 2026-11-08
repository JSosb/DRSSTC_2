/**
  * @file    console.h
  * @brief   Line-based command console over USB CDC (PuTTY-friendly).
  */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Resets internal buffers. Call once from main() before the superloop. */
void Console_Init(void);

/* Feed one received byte in. Safe to call from USB (CDC_Receive_HS)
   context: bounded work only, nothing here blocks or waits on the USB
   stack. ESC (0x1B) is handled immediately as a control byte; everything
   else is line-buffered until CR/LF. */
void Console_RxByte(uint8_t byte);

/* Call repeatedly from the main superloop. Drains the outgoing text queue
   into CDC_Transmit_HS() a chunk at a time, and turns a pending
   fault_message_pending flag into the async "FAULT: ..." report. Must run
   outside interrupt context. */
void Console_Poll(void);

#ifdef __cplusplus
}
#endif

#endif /* CONSOLE_H */
