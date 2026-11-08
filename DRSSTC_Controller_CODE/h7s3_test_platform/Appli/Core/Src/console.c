/**
  * @file    console.c
  * @brief   Line-based command console over USB CDC (PuTTY-friendly).
  *
  * Everything this file sends to the PC goes through a small ring buffer
  * (txq) drained by Console_Poll() from the main superloop -- never from
  * interrupt context. That keeps CDC_Transmit_HS()'s "busy, try later"
  * behaviour out of the USB and TIM ISRs entirely: producers just push
  * bytes and move on.
  */
#include "console.h"
#include "drsstc.h"
#include "usbd_cdc_if.h"
#include "main.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>

/* 200 bytes comfortably fits the longest realistic CFGLOAD line (all six
   tunables plus "CFGLOAD " and " CHK=XXXX", see HandleCfgDump()) with
   headroom -- the original 64 was sized only for a single SETxxx command
   and silently truncated (not overflowed: Console_RxByte()'s bounds check
   just drops bytes past this) anything longer, which would have made every
   pasted-back CFGDUMP line fail its own checksum. */
#define LINE_BUF_SIZE   200
#define TXQ_SIZE        1024
#define TX_CHUNK_SIZE   64

static char line_buf[LINE_BUF_SIZE];
static uint16_t line_len = 0;
static uint8_t crlf_skip_next_lf = 0;  /* see Console_RxByte()'s CR/LF handling */

static uint8_t txq[TXQ_SIZE];
static volatile uint16_t txq_head = 0;
static volatile uint16_t txq_tail = 0;
static volatile uint16_t txq_count = 0;

static uint8_t tx_stage[TX_CHUNK_SIZE];
static uint16_t tx_stage_len = 0;
static uint8_t tx_inflight = 0;

static const char *const BANNER =
  "\r\nDRSSTC console. Type HELP for commands.\r\n> ";

static void TxqPush(const uint8_t *data, uint16_t len)
{
  __disable_irq();
  for (uint16_t i = 0; i < len; i++)
  {
    if (txq_count < TXQ_SIZE)
    {
      txq[txq_head] = data[i];
      txq_head = (uint16_t)((txq_head + 1U) % TXQ_SIZE);
      txq_count++;
    }
    /* else: queue full, drop the byte rather than block anything. */
  }
  __enable_irq();
}

static void Print(const char *s)
{
  TxqPush((const uint8_t *)s, (uint16_t)strlen(s));
}

static void Printf(const char *fmt, ...)
{
  /* 256 comfortably covers STATUS (the worst existing case: two %s state
     names plus nine %lu fields, ~210 bytes at full uint32/"CLOSEDLOOP"
     width) and CFGDUMP's composed line, with headroom. The original 128
     was already marginal for STATUS alone -- vsnprintf() truncates safely
     (no overflow) rather than corrupting anything, but it would have
     silently clipped the tail of a long status line. */
  char buf[256];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > 0)
  {
    TxqPush((const uint8_t *)buf, (uint16_t)((n < (int)sizeof(buf)) ? n : (int)sizeof(buf) - 1));
  }
}

static int StrEqualCI(const char *a, const char *b)
{
  while (*a && *b)
  {
    char ca = *a, cb = *b;
    if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
    if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
    if (ca != cb) return 0;
    a++;
    b++;
  }
  return (*a == '\0') && (*b == '\0');
}

static char *NextToken(char **cursor)
{
  char *s = *cursor;
  while (*s == ' ') s++;
  if (*s == '\0')
  {
    *cursor = s;
    return NULL;
  }
  char *start = s;
  while (*s != '\0' && *s != ' ') s++;
  if (*s != '\0')
  {
    *s = '\0';
    s++;
  }
  *cursor = s;
  return start;
}

/**
  * @brief  CRC-16/CCITT-FALSE (poly 0x1021, init 0xFFFF, no reflection) over
  *         a byte string. Used by CFGDUMP/CFGLOAD to catch a corrupted or
  *         truncated copy-paste round trip -- not a security checksum, just
  *         cheap, well-distributed error detection with no lookup table.
  */
static uint16_t Crc16(const uint8_t *data, uint16_t len)
{
  uint16_t crc = 0xFFFFU;
  for (uint16_t i = 0; i < len; i++)
  {
    crc = (uint16_t)(crc ^ (uint16_t)((uint16_t)data[i] << 8));
    for (uint8_t bit = 0; bit < 8U; bit++)
    {
      if ((crc & 0x8000U) != 0U)
      {
        crc = (uint16_t)((uint16_t)(crc << 1) ^ 0x1021U);
      }
      else
      {
        crc = (uint16_t)(crc << 1);
      }
    }
  }
  return crc;
}

static const char *GateStateName(GateState_t s)
{
  switch (s)
  {
    case GATE_IDLE:       return "IDLE";
    case GATE_OPENLOOP:   return "OPENLOOP";
    case GATE_CLOSEDLOOP: return "CLOSEDLOOP";
    case GATE_FAULT:      return "FAULT";
    default:              return "?";
  }
}

static const char *SystemStateName(SystemState_t s)
{
  switch (s)
  {
    case SYS_DISARMED: return "DISARMED";
    case SYS_ARMED:    return "ARMED";
    case SYS_FIRING:   return "FIRING";
    default:           return "?";
  }
}

/* Defined further down (needed by DispatchLine()'s FIRE/STOP, which reuse
   its toggle+message logic after pre-checking direction so a stray FIRE
   while already firing can't accidentally stop the burst train, and vice
   versa). */
static void HandleEsc(void);

static void PrintHelp(void)
{
  Print(
    "\r\nCommands:\r\n"
    "\r\n"
    "  Safety / firing:\r\n"
    "    ARM                     - allow firing\r\n"
    "    DISARM                  - force stop, refuse to fire until ARMed\r\n"
    "    FIRE                    - start firing (ARMED only)\r\n"
    "    STOP                    - stop firing (FIRING only)\r\n"
    "    ESC key                 - same as FIRE when ARMED, STOP when FIRING\r\n"
    "    CLR                     - clear a latched feedback fault\r\n"
    "\r\n"
    "  Tuning (no argument prints the current value):\r\n"
    "    WIDTH <cycles>          - closed-loop (ZCS) half-cycles/burst, 1-100000\r\n"
    "    LEADIN <cycles>         - open-loop half-cycles before ETR handover,\r\n"
    "                              1-1000\r\n"
    "    LEADINHZ <hz>           - open-loop lead-in drive frequency,\r\n"
    "                              50000-150000\r\n"
    "    PULSE <lo> <hi>         - plausible ZCD half-cycle width, in us\r\n"
    "    PHASELEAD <ns>          - closed-loop phase lead, 0-7000 ns fixed\r\n"
    "                              time (not an angle); derated to at most\r\n"
    "                              35% of the current lead-in period, see\r\n"
    "                              STATUS's eff= for what's actually applied\r\n"
    "    BPM <n>                 - bursts per minute (TIM2 repetition rate)\r\n"
    "\r\n"
    "  Lead-in frequency sweep:\r\n"
    "    SWEEP <lo> <hi> <step>  - arm a sweep, in Hz; ENTER (empty line)\r\n"
    "                              fires each step -- open-loop lead-in\r\n"
    "                              pulses only, hard-switched, no ZCD\r\n"
    "                              handover or feedback protection\r\n"
    "    SWEEPSTOP               - cancel an armed/in-progress sweep\r\n"
    "\r\n"
    "  Configuration save/restore:\r\n"
    "    CFGDUMP                 - print the current WIDTH/LEADIN/LEADINHZ/\r\n"
    "                              PULSE/PHASELEAD/BPM as one CFGLOAD line\r\n"
    "                              with a checksum -- copy it into a text\r\n"
    "                              file\r\n"
    "    CFGLOAD <dumped line>   - restore a config CFGDUMP printed earlier;\r\n"
    "                              paste the whole line back verbatim. All\r\n"
    "                              six values are validated (checksum first,\r\n"
    "                              then each value's own range) and applied\r\n"
    "                              together -- any single problem rejects\r\n"
    "                              the whole line and changes nothing\r\n"
    "\r\n"
    "  STATUS                    - print current state and settings\r\n"
    "  HELP                      - this text\r\n");
}

static void PrintStatus(void)
{
  Printf("\r\nsystem=%s gate=%s bpm=%lu width=%lu leadin=%lu leadinhz=%luHz "
         "pulse=%lu,%luus phaselead=%luns(eff=%luns) faults=%lu\r\n",
         SystemStateName(system_state), GateStateName(gate_state),
         (unsigned long)System_GetBpm(),
         (unsigned long)burst_closedloop_cycles,
         (unsigned long)burst_openloop_cycles,
         (unsigned long)System_GetLeadInFreqHz(),
         (unsigned long)TICKS_TO_US(pulse_width_min_ticks),
         (unsigned long)TICKS_TO_US(pulse_width_max_ticks),
         (unsigned long)phase_lead_ns,
         (unsigned long)System_GetEffectivePhaseLeadNs(),
         (unsigned long)feedback_fault_count);
  if (System_SweepLeadInActive())
  {
    Printf("sweep armed, next ENTER fires %lu Hz\r\n",
           (unsigned long)System_SweepLeadInNextHz());
  }
}

/**
  * @brief  CFGDUMP: print the six tunables as one CFGLOAD line, ready to
  *         copy-paste back verbatim (either right now, or after a restart --
  *         these are RAM-only settings with no on-chip persistence, so this
  *         is the fast path back to a known-good configuration instead of
  *         re-typing six SETxxx-turned-bare commands by hand).
  * @note   The checksum covers exactly the "KEY=VALUE KEY=VALUE ..." bytes
  *         built into cfg[] below, with no trailing space -- HandleCfgLoad()
  *         must trim the line the same way before recomputing it, or a
  *         perfectly-intact paste would appear corrupted.
  */
static void HandleCfgDump(void)
{
  char cfg[160];
  int n = snprintf(cfg, sizeof(cfg),
                    "WIDTH=%lu LEADIN=%lu LEADINHZ=%lu PULSE=%lu,%lu PHASELEAD=%lu BPM=%lu",
                    (unsigned long)burst_closedloop_cycles,
                    (unsigned long)burst_openloop_cycles,
                    (unsigned long)System_GetLeadInFreqHz(),
                    (unsigned long)TICKS_TO_US(pulse_width_min_ticks),
                    (unsigned long)TICKS_TO_US(pulse_width_max_ticks),
                    (unsigned long)phase_lead_ns,
                    (unsigned long)System_GetBpm());
  if (n < 0)
  {
    n = 0;
  }
  else if (n >= (int)sizeof(cfg))
  {
    /* Can't happen at any range any current setter accepts (worst case,
       every field at the full uint32 10-digit ceiling, is ~120 bytes) --
       guard anyway rather than let Crc16() below run past what snprintf()
       actually wrote. */
    n = (int)sizeof(cfg) - 1;
  }
  uint16_t chk = Crc16((const uint8_t *)cfg, (uint16_t)n);
  Printf("\r\nCFGLOAD %s CHK=%04X\r\n", cfg, (unsigned)chk);
}

/**
  * @brief  CFGLOAD: parse, checksum-verify, and apply a line CFGDUMP printed
  *         earlier.
  * @param  rest  Whatever followed the "CFGLOAD " token on the command line
  *         -- still raw/untokenized, and still part of the mutable
  *         line_buf, so this function is free to write '\0's into it.
  * @note   All-or-nothing: the checksum is verified before anything is
  *         touched, every key is required exactly once before anything is
  *         touched, and if any individual value is then rejected by its own
  *         setter's range check, every value already applied this call is
  *         rolled back to what it was before CFGLOAD ran. A partially-typed
  *         or partially-corrupted line can reject outright, but it can never
  *         leave a mix of old and new settings in place.
  */
static void HandleCfgLoad(char *rest)
{
  char *chk_pos = strstr(rest, "CHK=");
  if (chk_pos == NULL)
  {
    Print("\r\nERR CFGLOAD: no CHK= checksum found -- paste the exact line "
          "CFGDUMP printed\r\n");
    return;
  }
  /* Trim the (normally single) space between the last KEY=VALUE token and
     CHK= so this covers exactly the bytes HandleCfgDump() checksummed. */
  char *cfg_end = chk_pos;
  while ((cfg_end > rest) && (cfg_end[-1] == ' '))
  {
    cfg_end--;
  }
  *cfg_end = '\0';

  uint16_t want_chk = Crc16((const uint8_t *)rest, (uint16_t)(cfg_end - rest));
  uint16_t got_chk = (uint16_t)strtoul(chk_pos + 4, NULL, 16);
  if (got_chk != want_chk)
  {
    Printf("\r\nERR CFGLOAD: checksum mismatch (line reads %04X, content "
           "hashes to %04X) -- corrupted in transit, nothing changed\r\n",
           (unsigned)got_chk, (unsigned)want_chk);
    return;
  }

  uint8_t seen_width = 0U, seen_leadin = 0U, seen_leadinhz = 0U,
          seen_pulse = 0U, seen_phaselead = 0U, seen_bpm = 0U;
  uint32_t v_width = 0U, v_leadin = 0U, v_leadinhz = 0U,
           v_pulse_min = 0U, v_pulse_max = 0U, v_phaselead = 0U, v_bpm = 0U;

  /* Checksum already guarantees these bytes are exactly what some CFGDUMP
     produced, so parsing by key name (rather than assuming CFGDUMP's fixed
     emission order) costs nothing here and stays forward-compatible with a
     future CFGDUMP that reorders its fields. */
  char *tcursor = rest;
  char *tok;
  while ((tok = NextToken(&tcursor)) != NULL)
  {
    char *eq = strchr(tok, '=');
    if (eq == NULL)
    {
      Printf("\r\nERR CFGLOAD: malformed token '%s' (no '='), nothing "
             "changed\r\n", tok);
      return;
    }
    *eq = '\0';
    char *key = tok;
    char *val = eq + 1;

    if (StrEqualCI(key, "WIDTH"))
    {
      v_width = (uint32_t)strtoul(val, NULL, 10);
      seen_width = 1U;
    }
    else if (StrEqualCI(key, "LEADIN"))
    {
      v_leadin = (uint32_t)strtoul(val, NULL, 10);
      seen_leadin = 1U;
    }
    else if (StrEqualCI(key, "LEADINHZ"))
    {
      v_leadinhz = (uint32_t)strtoul(val, NULL, 10);
      seen_leadinhz = 1U;
    }
    else if (StrEqualCI(key, "PULSE"))
    {
      char *comma = strchr(val, ',');
      if (comma == NULL)
      {
        Print("\r\nERR CFGLOAD: PULSE needs '<min>,<max>', nothing "
              "changed\r\n");
        return;
      }
      *comma = '\0';
      v_pulse_min = (uint32_t)strtoul(val, NULL, 10);
      v_pulse_max = (uint32_t)strtoul(comma + 1, NULL, 10);
      seen_pulse = 1U;
    }
    else if (StrEqualCI(key, "PHASELEAD"))
    {
      v_phaselead = (uint32_t)strtoul(val, NULL, 10);
      seen_phaselead = 1U;
    }
    else if (StrEqualCI(key, "BPM"))
    {
      v_bpm = (uint32_t)strtoul(val, NULL, 10);
      seen_bpm = 1U;
    }
    else
    {
      Printf("\r\nERR CFGLOAD: unknown key '%s', nothing changed\r\n", key);
      return;
    }
  }

  if (!(seen_width && seen_leadin && seen_leadinhz && seen_pulse &&
        seen_phaselead && seen_bpm))
  {
    Print("\r\nERR CFGLOAD: missing one or more of WIDTH/LEADIN/LEADINHZ/"
          "PULSE/PHASELEAD/BPM, nothing changed\r\n");
    return;
  }

  /* Snapshot before touching anything: every one of these is a value some
     setter already accepted in the past (it's live right now), so
     re-applying it on rollback below can't itself fail. */
  uint32_t orig_width = burst_closedloop_cycles;
  uint32_t orig_leadin = burst_openloop_cycles;
  uint32_t orig_leadinhz = System_GetLeadInFreqHz();
  uint32_t orig_pulse_min = TICKS_TO_US(pulse_width_min_ticks);
  uint32_t orig_pulse_max = TICKS_TO_US(pulse_width_max_ticks);
  uint32_t orig_phaselead = phase_lead_ns;
  uint32_t orig_bpm = System_GetBpm();

  uint8_t ok = System_SetBurstWidth(v_width) &&
               System_SetLeadInCycles(v_leadin) &&
               System_SetLeadInFreqHz(v_leadinhz) &&
               System_SetPulseWidthRangeUs(v_pulse_min, v_pulse_max) &&
               System_SetPhaseLeadNs(v_phaselead) &&
               System_SetBurstRateBPM(v_bpm);
  if (!ok)
  {
    /* Short-circuited above on the first rejected value, so anything past
       it was never touched -- restoring all six anyway is still correct,
       just a harmless no-op for whichever ones didn't change. */
    System_SetBurstWidth(orig_width);
    System_SetLeadInCycles(orig_leadin);
    System_SetLeadInFreqHz(orig_leadinhz);
    System_SetPulseWidthRangeUs(orig_pulse_min, orig_pulse_max);
    System_SetPhaseLeadNs(orig_phaselead);
    System_SetBurstRateBPM(orig_bpm);
    Print("\r\nERR CFGLOAD: one or more values rejected by their range "
          "check -- rolled back, nothing changed\r\n");
    return;
  }

  Print("\r\nOK CFGLOAD applied\r\n");
  PrintStatus();
}

static void DispatchLine(char *line)
{
  char *cursor = line;
  char *cmd = NextToken(&cursor);
  if (cmd == NULL) return;

  if (StrEqualCI(cmd, "ARM"))
  {
    System_Arm();
    Print("\r\nOK armed\r\n");
  }
  else if (StrEqualCI(cmd, "DISARM"))
  {
    System_Disarm();
    Print("\r\nOK disarmed\r\n");
  }
  else if (StrEqualCI(cmd, "FIRE"))
  {
    if (system_state == SYS_FIRING)
    {
      Print("\r\nERR already firing\r\n");
    }
    else
    {
      HandleEsc();  /* prints [FIRING], or the not-armed error */
    }
  }
  else if (StrEqualCI(cmd, "STOP"))
  {
    if (system_state != SYS_FIRING)
    {
      Print("\r\nERR not firing\r\n");
    }
    else
    {
      HandleEsc();  /* prints [STOPPED] */
    }
  }
  else if (StrEqualCI(cmd, "WIDTH"))
  {
    char *arg = NextToken(&cursor);
    if (arg == NULL)
    {
      Printf("\r\nwidth=%lu\r\n", (unsigned long)burst_closedloop_cycles);
    }
    else
    {
      uint32_t v = (uint32_t)strtoul(arg, NULL, 10);
      if (System_SetBurstWidth(v))
      {
        Printf("\r\nOK width=%lu\r\n", (unsigned long)v);
      }
      else
      {
        Print("\r\nERR usage: WIDTH [cycles 1-100000]\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "LEADIN"))
  {
    char *arg = NextToken(&cursor);
    if (arg == NULL)
    {
      Printf("\r\nleadin=%lu\r\n", (unsigned long)burst_openloop_cycles);
    }
    else
    {
      uint32_t v = (uint32_t)strtoul(arg, NULL, 10);
      if (System_SetLeadInCycles(v))
      {
        Printf("\r\nOK leadin=%lu\r\n", (unsigned long)v);
      }
      else
      {
        Print("\r\nERR usage: LEADIN [cycles 1-1000]\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "LEADINHZ"))
  {
    char *arg = NextToken(&cursor);
    if (arg == NULL)
    {
      Printf("\r\nleadinhz=%luHz\r\n", (unsigned long)System_GetLeadInFreqHz());
    }
    else
    {
      uint32_t v = (uint32_t)strtoul(arg, NULL, 10);
      if (System_SetLeadInFreqHz(v))
      {
        Printf("\r\nOK leadinhz=%luHz\r\n", (unsigned long)System_GetLeadInFreqHz());
      }
      else
      {
        Print("\r\nERR usage: LEADINHZ [hz 50000-150000]\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "SWEEP"))
  {
    char *arg1 = NextToken(&cursor);
    char *arg2 = NextToken(&cursor);
    char *arg3 = NextToken(&cursor);
    uint32_t lo = (arg1 != NULL) ? (uint32_t)strtoul(arg1, NULL, 10) : 0U;
    uint32_t hi = (arg2 != NULL) ? (uint32_t)strtoul(arg2, NULL, 10) : 0U;
    uint32_t step = (arg3 != NULL) ? (uint32_t)strtoul(arg3, NULL, 10) : 0U;
    if (arg1 != NULL && arg2 != NULL && arg3 != NULL && System_SweepLeadInStart(lo, hi, step))
    {
      Printf("\r\nOK sweep armed %lu..%lu step %luHz -- ENTER fires %lu Hz, "
             "SWEEPSTOP to cancel\r\n",
             (unsigned long)lo, (unsigned long)hi, (unsigned long)step,
             (unsigned long)System_SweepLeadInNextHz());
    }
    else
    {
      Print("\r\nERR usage: SWEEP <lo_hz> <hi_hz> <step_hz>, both ends "
            "50000-150000, system must be ARMED (not FIRING) and idle\r\n");
    }
  }
  else if (StrEqualCI(cmd, "SWEEPSTOP"))
  {
    System_SweepLeadInStop();
    Print("\r\nOK sweep stopped\r\n");
  }
  else if (StrEqualCI(cmd, "BPM"))
  {
    char *arg = NextToken(&cursor);
    if (arg == NULL)
    {
      Printf("\r\nbpm=%lu\r\n", (unsigned long)System_GetBpm());
    }
    else
    {
      uint32_t v = (uint32_t)strtoul(arg, NULL, 10);
      if (System_SetBurstRateBPM(v))
      {
        Printf("\r\nOK bpm=%lu\r\n", (unsigned long)System_GetBpm());
      }
      else
      {
        Print("\r\nERR usage: BPM [bursts per minute]\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "PULSE"))
  {
    char *arg1 = NextToken(&cursor);
    if (arg1 == NULL)
    {
      Printf("\r\npulse=%lu,%luus\r\n",
             (unsigned long)TICKS_TO_US(pulse_width_min_ticks),
             (unsigned long)TICKS_TO_US(pulse_width_max_ticks));
    }
    else
    {
      char *arg2 = NextToken(&cursor);
      uint32_t lo = (uint32_t)strtoul(arg1, NULL, 10);
      uint32_t hi = (arg2 != NULL) ? (uint32_t)strtoul(arg2, NULL, 10) : 0U;
      if (arg2 != NULL && System_SetPulseWidthRangeUs(lo, hi))
      {
        Printf("\r\nOK pulse=%lu,%luus\r\n", (unsigned long)lo, (unsigned long)hi);
      }
      else
      {
        Print("\r\nERR usage: PULSE [<min_us> <max_us>], min>0 and max>min\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "PHASELEAD"))
  {
    char *arg = NextToken(&cursor);
    if (arg == NULL)
    {
      Printf("\r\nphaselead=%luns(eff=%luns)\r\n",
             (unsigned long)phase_lead_ns,
             (unsigned long)System_GetEffectivePhaseLeadNs());
    }
    else
    {
      uint32_t v = (uint32_t)strtoul(arg, NULL, 10);
      if (System_SetPhaseLeadNs(v))
      {
        Printf("\r\nOK phaselead=%luns(eff=%luns)\r\n",
               (unsigned long)v, (unsigned long)System_GetEffectivePhaseLeadNs());
      }
      else
      {
        Print("\r\nERR usage: PHASELEAD [ns 0-7000]\r\n");
      }
    }
  }
  else if (StrEqualCI(cmd, "CLR") || StrEqualCI(cmd, "CLEAR"))
  {
    ClearFeedbackFault();
    Print("\r\nOK fault cleared (send ESC to resume firing)\r\n");
  }
  else if (StrEqualCI(cmd, "CFGDUMP"))
  {
    HandleCfgDump();
  }
  else if (StrEqualCI(cmd, "CFGLOAD"))
  {
    HandleCfgLoad(cursor);
  }
  else if (StrEqualCI(cmd, "STATUS"))
  {
    PrintStatus();
  }
  else if (StrEqualCI(cmd, "HELP") || StrEqualCI(cmd, "?"))
  {
    PrintHelp();
  }
  else
  {
    Printf("\r\nERR unknown command '%s', try HELP\r\n", cmd);
  }
}

/* A bare ENTER (empty line) while a sweep is armed fires the next step
   instead of being a no-op -- see SWEEP in DispatchLine() and
   System_SweepLeadInTrigger()'s doc comment. */
static void HandleSweepTrigger(void)
{
  uint32_t fired_hz = 0U;
  SweepTriggerResult_t r = System_SweepLeadInTrigger(&fired_hz);
  switch (r)
  {
    case SWEEP_TRIGGER_FIRED:
    {
      uint32_t next_hz = System_SweepLeadInNextHz();
      if (next_hz != 0U)
      {
        Printf("\r\n[SWEEP] fired %lu Hz -- ENTER for next (%lu Hz), "
               "SWEEPSTOP to cancel\r\n> ",
               (unsigned long)fired_hz, (unsigned long)next_hz);
      }
      else
      {
        Printf("\r\n[SWEEP] fired %lu Hz -- sweep complete\r\n> ",
               (unsigned long)fired_hz);
      }
      break;
    }
    case SWEEP_TRIGGER_BUSY:
      Print("\r\n[SWEEP] previous burst still running, try again\r\n> ");
      break;
    case SWEEP_TRIGGER_DONE:
      Print("\r\n[SWEEP] already complete, SWEEP to start another\r\n> ");
      break;
    case SWEEP_TRIGGER_NOT_ACTIVE:
    default:
      Print("\r\n[SWEEP] no sweep active\r\n> ");
      break;
  }
}

static void HandleEsc(void)
{
  ToggleResult_t r = System_ToggleFiring();
  switch (r)
  {
    case TOGGLE_NOW_FIRING:
      Print("\r\n[FIRING]\r\n");
      break;
    case TOGGLE_NOW_STOPPED:
      Print("\r\n[STOPPED]\r\n");
      break;
    case TOGGLE_IGNORED_NOT_ARMED:
    default:
      Print("\r\nERR not armed, send ARM first\r\n");
      break;
  }
}

void Console_Init(void)
{
  line_len = 0;
  crlf_skip_next_lf = 0;
  txq_head = 0;
  txq_tail = 0;
  txq_count = 0;
  tx_stage_len = 0;
  tx_inflight = 0;
  Print(BANNER);
}

void Console_RxByte(uint8_t b)
{
  if (b == 0x1BU) /* ESC */
  {
    crlf_skip_next_lf = 0;
    HandleEsc();
    return;
  }

  if (b == (uint8_t)'\n' && crlf_skip_next_lf)
  {
    /* second half of a CRLF pair whose '\r' already triggered this same
       ENTER below -- swallow it so terminals sending CRLF don't fire
       (or dispatch) twice per keypress. */
    crlf_skip_next_lf = 0;
    return;
  }
  crlf_skip_next_lf = 0;

  if (b == (uint8_t)'\r' || b == (uint8_t)'\n')
  {
    if (b == (uint8_t)'\r') crlf_skip_next_lf = 1;
    if (line_len > 0U)
    {
      line_buf[line_len] = '\0';
      Print("\r\n");
      DispatchLine(line_buf);
      Print("> ");
      line_len = 0U;
    }
    else if (System_SweepLeadInActive())
    {
      HandleSweepTrigger();
    }
    return;
  }

  if (b == 0x08U || b == 0x7FU) /* backspace / DEL */
  {
    if (line_len > 0U)
    {
      line_len--;
      Print("\b \b");
    }
    return;
  }

  if ((line_len < (LINE_BUF_SIZE - 1U)) && (b >= 0x20U) && (b < 0x7FU))
  {
    line_buf[line_len++] = (char)b;
    uint8_t echo = b;
    TxqPush(&echo, 1U);
  }
}

void Console_Poll(void)
{
  if (fault_message_pending)
  {
    fault_message_pending = 0U;
    if (last_fault_reason == FAULT_PULSE_WIDTH)
    {
      Printf("\r\nFAULT: ZCD half-cycle width %lu us outside configured "
             "range [%lu,%lu] us -- burst aborted, CLR to reset\r\n> ",
             (unsigned long)TICKS_TO_US(last_fault_width_ticks),
             (unsigned long)TICKS_TO_US(pulse_width_min_ticks),
             (unsigned long)TICKS_TO_US(pulse_width_max_ticks));
    }
    else
    {
      Printf("\r\nFAULT: no ZCD edge detected (feedback lost or never "
             "acquired) -- burst aborted, CLR to reset\r\n> ");
    }
  }

  if (!tx_inflight)
  {
    if (txq_count == 0U) return;
    __disable_irq();
    uint16_t n = txq_count;
    if (n > TX_CHUNK_SIZE) n = TX_CHUNK_SIZE;
    for (uint16_t i = 0; i < n; i++)
    {
      tx_stage[i] = txq[txq_tail];
      txq_tail = (uint16_t)((txq_tail + 1U) % TXQ_SIZE);
    }
    txq_count = (uint16_t)(txq_count - n);
    __enable_irq();
    tx_stage_len = n;
    tx_inflight = 1U;
  }

  if (tx_inflight)
  {
    if (CDC_Transmit_HS(tx_stage, tx_stage_len) == USBD_OK)
    {
      tx_inflight = 0U;
    }
    /* else USBD_BUSY: retry the same staged chunk next poll. */
  }
}
