/**
  * @file    drsstc.h
  * @brief   Shared types/state between the TIM1/TIM2 burst state machine
  *          (main.c) and the USB console (console.c).
  */
#ifndef DRSSTC_H
#define DRSSTC_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Top-level safety state, driven by the USB console.
   DISARMED: ESC is ignored, TIM2 will never start a burst.
   ARMED:    parameters can be set, ESC starts firing.
   FIRING:   TIM2 starts a burst each period; ESC stops it. */
typedef enum
{
  SYS_DISARMED = 0,
  SYS_ARMED,
  SYS_FIRING
} SystemState_t;

/* Per-burst gate state, see main.c for the full state machine. */
typedef enum
{
  GATE_IDLE = 0,
  GATE_OPENLOOP,
  GATE_CLOSEDLOOP,
  GATE_FAULT
} GateState_t;

/* Why the last fault happened, for the async USB fault report. */
typedef enum
{
  FAULT_NONE = 0,
  FAULT_NO_EDGE,       /* a full nominal period elapsed with no ZCD edge */
  FAULT_PULSE_WIDTH    /* a ZCD edge arrived, but its half-cycle width was
                           outside the configured plausible range */
} FaultReason_t;

typedef enum
{
  TOGGLE_IGNORED_NOT_ARMED = 0,
  TOGGLE_NOW_FIRING,
  TOGGLE_NOW_STOPPED
} ToggleResult_t;

/* Result of one System_SweepLeadInTrigger() call, i.e. one bare-ENTER
   press on the console while a SWEEP is armed. */
typedef enum
{
  SWEEP_TRIGGER_FIRED = 0,   /* burst just launched at the given frequency */
  SWEEP_TRIGGER_NOT_ACTIVE,  /* no sweep armed (or it was just cancelled) */
  SWEEP_TRIGGER_BUSY,        /* previous burst still running, press ignored */
  SWEEP_TRIGGER_DONE         /* sweep already fired its last (stop_hz) step */
} SweepTriggerResult_t;

/* Live state, read-only from console.c. */
extern volatile SystemState_t system_state;
extern volatile GateState_t gate_state;
extern volatile FaultReason_t last_fault_reason;
extern volatile uint32_t feedback_fault_count;
extern volatile uint32_t last_fault_width_ticks;
extern volatile uint32_t closedloop_edge_count;

/* Tunables, read-only display + write via the setters below. */
extern volatile uint32_t burst_openloop_cycles;
extern volatile uint32_t burst_closedloop_cycles;
extern volatile uint32_t pulse_width_min_ticks;
extern volatile uint32_t pulse_width_max_ticks;
extern volatile uint32_t phase_lead_ns;

/* Set when a fault has just been latched; main.c's Console_Poll() (via
   main's superloop) consumes it to emit the async fault report. Owned by
   the ISR producer / console.c consumer pair in console.c. */
extern volatile uint8_t fault_message_pending;

/* Control API, all safe to call from USB (console) context. */
void System_Arm(void);
void System_Disarm(void);
ToggleResult_t System_ToggleFiring(void);
void ClearFeedbackFault(void);
uint8_t System_SetBurstWidth(uint32_t cycles);
uint8_t System_SetLeadInCycles(uint32_t cycles);
uint8_t System_SetBurstRateBPM(uint32_t bpm);
uint8_t System_SetPulseWidthRangeUs(uint32_t min_us, uint32_t max_us);
uint32_t System_GetBpm(void);

/* Digital phase lead on BOTH closed-loop switching instants -- the
   boundary/ARR transition and leg B's mid-point/CCR transition (see
   phase_lead_ns's declaration comment in main.c for the full mechanism: the
   real ZCD edge still forces a genuine resync every cycle, ARR is just
   deliberately shortened so TIM1 wraps a bit before it's predicted to
   arrive). A FIXED nanosecond value, 0-PHASE_LEAD_MAX_NS (main.c), not an
   angle -- meant to match a scope-measured propagation delay, not to scale
   with tank frequency. Applying it costs some loss-of-feedback detection
   latency: see lead_wrap_pending's declaration comment in main.c. The
   EFFECTIVE lead applied per cycle can be less than this: see
   PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T's declaration comment in main.c and
   System_GetEffectivePhaseLeadNs() below. */
uint8_t System_SetPhaseLeadNs(uint32_t ns);
uint32_t System_GetEffectivePhaseLeadNs(void);

/* Lead-in (open-loop) drive frequency, set independently of burst_openloop_cycles
   via TIM1's prescaler; ARR (and so the 50% duty point) is unaffected. */
uint8_t System_SetLeadInFreqHz(uint32_t freq_hz);
uint32_t System_GetLeadInFreqHz(void);

/* Manual lead-in frequency sweep: separate from the normal TIM2-timed
   FIRING burst train. System_SweepLeadInStart() arms it; each subsequent
   bare ENTER on the console (System_SweepLeadInTrigger()) fires exactly one
   burst at the current step and advances to the next, until the range is
   exhausted or System_SweepLeadInStop() cancels it. Each fired burst is
   open-loop lead-in pulses ONLY, hard-switched for burst_openloop_cycles
   half-cycles: unlike a normal FIRING burst it never hands over to
   TIM1_ETR/ZCD closed-loop tracking, so none of the ZCD feedback-quality
   protection (TIF / pulse-width checks) applies to it either. */
uint8_t System_SweepLeadInStart(uint32_t start_hz, uint32_t stop_hz, uint32_t step_hz);
void System_SweepLeadInStop(void);
uint8_t System_SweepLeadInActive(void);
uint32_t System_SweepLeadInNextHz(void);
SweepTriggerResult_t System_SweepLeadInTrigger(uint32_t *out_fired_hz);

/* CPU (DWT) clock, used to convert the pulse-width bounds to/from
   microseconds. Matches RCC.CpuClockFreq_Value in the .ioc. */
#define CPU_CLK_HZ           405000000UL
#define US_TO_TICKS(us)      ((uint32_t)((uint64_t)(us) * (CPU_CLK_HZ / 1000000UL)))
#define TICKS_TO_US(ticks)   ((uint32_t)((ticks) / (CPU_CLK_HZ / 1000000UL)))

#ifdef __cplusplus
}
#endif

#endif /* DRSSTC_H */
