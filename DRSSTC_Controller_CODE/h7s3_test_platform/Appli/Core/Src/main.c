/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "usb_device.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "drsstc.h"
#include "console.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */
/* GateState_t, SystemState_t, FaultReason_t, ToggleResult_t: see drsstc.h --
   shared with console.c, which reads state and calls the setters below. */
/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define BURST_OPENLOOP_CYCLES_DEFAULT    5U   /* free-running half-cycles before ETR handover */
#define BURST_CLOSEDLOOP_CYCLES_DEFAULT  5U   /* ZCS-locked half-cycles per burst (burst length);
                                                  also the feedback-loss timeout with no ZCD edges */

/* Budget of *repeated* natural (non-ETR) overflows tolerated during the
   whole acquisition phase (handover through the second real ZCD edge, after
   which ARR is driven by a real measurement rather than the handover's
   bootstrap estimate -- see real_period_measured's declaration comment).
   Exactly one natural overflow per cycle is always tolerated for free before
   this budget is even touched -- see lead_wrap_pending's declaration
   comment -- so this only gets charged by a SECOND natural overflow in the
   same cycle (no genuine edge in between). Once ARR is driven by a real
   measurement, tolerance for that second-in-a-row overflow drops to zero --
   any confirmed miss thereafter aborts immediately (fail-safe: a lock that
   was acquired and then lost is treated as more dangerous than one that was
   never acquired). */
#define FEEDBACK_ACQUIRE_GRACE_CYCLES    3U

/* TIM1_ETR digital input filter (SMCR.ETF), see RM0477 SMCR register: 0x3
   samples at the full 202.5 MHz timer clock and requires 8 consecutive
   agreeing samples (~40 ns window) before accepting an edge -- cheap,
   near-zero-latency rejection of narrow comparator/EMI glitches. Raise it
   (e.g. towards fDTS/8 or fDTS/32 ranges) if scope testing still shows
   false triggers; each step trades more noise immunity for more delay
   between the real zero-crossing and TIM1 seeing it. */
#define TIM1_ETR_FILTER_VALUE            3U

/* TIM1's ARR as configured at init (MX_TIM1_Init(): htim1.Init.Period) --
   just the peripheral's boot-time default before any burst has run.
   GATE_CLOSEDLOOP no longer runs at a fixed ARR: both ARR and CCR1/CCR2 are
   continuously re-derived from the measured (or, at the very first
   half-cycle, the open-loop lead-in's known) period -- see
   ComputeClosedLoopArrCcr() below. The *lead-in* phase has its own
   independent ARR: see leadin_arr / ComputeLeadInPscArr() below, which pick
   a smaller one (at PSC=0) to reach lead-in frequencies above what this one
   can reach even at PSC=0. */
#define TIM1_ARR_TICKS                2999U
#define TIM1_CCR_NOMINAL              1500U   /* TIM1_ARR_TICKS' 50% duty point */
#define TIM1_CCR_MARGIN_TICKS          100U   /* ~494 ns, comfortably > the
                                                  ~247 ns DTG=50 dead time,
                                                  so the clamp never lets a
                                                  channel's pulse collapse
                                                  into the dead-time gap */

/* TIM1's APB2 timer kernel clock -- exactly half CPU_CLK_HZ on this part
   (same 2x relationship the closed-loop CCR tracking math relies on).
   Used to turn a requested lead-in Hz into a (PSC, ARR) pair: below
   TIM1_KERNEL_CLK_HZ/(TIM1_ARR_TICKS+1) (67500 Hz), ARR stays at
   TIM1_ARR_TICKS and PSC divides the clock down, exactly as before ARR
   became adjustable; above it, PSC stays 0 and ARR itself shrinks instead,
   since PSC can only divide the clock down, never up. */
#define TIM1_KERNEL_CLK_HZ            (CPU_CLK_HZ / 2UL)

/* Upper end of the lead-in frequency range reachable via the ARR-shrinking
   path above (PSC=0, ARR = round(TIM1_KERNEL_CLK_HZ/freq_hz) - 1 = 1349 at
   exactly 150 kHz). LEADINHZ/SWEEP reject anything above it. */
#define TIM1_LEADIN_FREQ_MAX_HZ        150000UL
/* Lower end of the lead-in frequency range: driving the bridge open-loop
   any slower than this stretches each open-loop half-cycle's conduction
   time further than is sensible for this tank. LEADINHZ/SWEEP
   (both endpoints, via ComputeLeadInPscArr()) reject anything below it. */
#define TIM1_LEADIN_FREQ_MIN_HZ         50000UL

/* Plausible ZCD half-cycle width window, in microseconds. These are
   placeholder defaults spanning a generous 33 kHz-500 kHz-ish band --
   tune to your actual tank via the USB console's PULSE command
   once you know its real operating range and how far it drifts over a
   burst. Too tight and normal frequency drift will nuisance-trip; too
   wide and it stops being a useful check. */
#define PULSE_WIDTH_MIN_US_DEFAULT       2U
#define PULSE_WIDTH_MAX_US_DEFAULT      30U

/* Ceiling for phase_lead_ns (see its declaration below): 7000 ns. This is
   just the PHASELEAD input ceiling -- generous enough to be usable at
   TIM1_LEADIN_FREQ_MIN_HZ (50 kHz), where 7000 ns is a plausible fraction of
   the period. At higher lead-in/tank frequencies the ns value entered here
   can easily exceed what's sensible for a much shorter period; that's
   handled separately by deriving an actual per-cycle tick cap from the
   current lead-in period in ComputeClosedLoopArrCcr() (see
   PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T below) rather than by lowering this
   input ceiling -- so the full range stays enterable when running slow,
   without ever letting the *effective* lead become a nonsensical fraction
   of a short cycle if leadin_freq is raised afterwards. */
#define PHASE_LEAD_MAX_NS              7000UL
/* Per-cycle cap on phase_lead_ticks, as a percentage of the current lead-in
   period T = 1/leadin_freq (in PSC=0-equivalent TIM1 ticks -- see
   ComputeClosedLoopArrCcr()'s use of leadin_psc/leadin_arr). 35% of T at
   TIM1_LEADIN_FREQ_MIN_HZ (50 kHz, T = 20 us) is exactly PHASE_LEAD_MAX_NS
   (7 us) -- i.e. the full PHASELEAD range is usable, undiminished, right
   at the lead-in frequency floor, and gets progressively derated as
   leadin_freq is raised (shrinking T) above that floor. Applied to the
   *lead-in* period specifically (not a per-cycle real ZCD measurement,
   which can vary burst to burst) so the effective ceiling only moves when
   LEADINHZ is deliberately changed, not from one cycle to the next. */
#define PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T   35U
/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

TIM_HandleTypeDef htim1;
TIM_HandleTypeDef htim2;

/* USER CODE BEGIN PV */
volatile SystemState_t system_state = SYS_DISARMED;
volatile GateState_t gate_state = GATE_IDLE;
volatile uint32_t closedloop_edge_count = 0;
static volatile uint8_t feedback_locked = 0;          /* at least one real ZCD edge seen this burst */
static volatile uint32_t acquire_grace_remaining = 0;  /* natural overflows still tolerated pre-lock */
static volatile uint32_t last_edge_timestamp = 0;      /* DWT->CYCCNT at the last accepted ZCD edge */
/* True once ARR/CCR1/CCR2 are being driven by an actual two-real-edge
   measurement (from the second real edge on), as opposed to the handover's
   bootstrap estimate (from the open-loop lead-in frequency -- see the
   GATE_OPENLOOP->GATE_CLOSEDLOOP handover below) that governs the interval
   between the first and second real edges. The bootstrap estimate is only
   ever as good as how closely LEADINHZ happens to match the tank's
   real resonance, which is routinely somewhat off, so a SECOND-in-a-row
   overflow during that interval falls back to the lenient, multi-cycle
   acquire_grace_remaining budget instead of the strict immediate-fault rule
   below (which assumes ARR reflects a real, just-measured period -- true
   from the second real edge on, never true for this first, estimated
   interval). Either way, ComputeClosedLoopArrCcr() shortens ARR by
   phase_lead_ticks whether it's working from the bootstrap estimate or a
   real measurement, so the deliberate lead-wrap (see lead_wrap_pending
   below) happens regardless of real_period_measured -- only what a SECOND,
   unexplained overflow on top of it means (a plausibly-off bootstrap guess
   vs. an unambiguous loss of a confirmed lock) differs by phase. */
static volatile uint8_t real_period_measured = 0;
/* Natural-overflow tolerance, both pre- and post-lock: ComputeClosedLoopArrCcr()
   deliberately shortens ARR by phase_lead_ticks (for the bootstrap estimate
   just as much as for a real measurement), so every cycle now produces one
   EXPECTED natural (non-TIF) overflow -- the lead-wrap -- right before the
   genuine ZCD edge that should follow it. This flag is set on that first,
   expected overflow and cleared on the next genuine edge; a SECOND natural
   overflow while it's still set (i.e. two in a row with no genuine edge
   between them) means the expected edge never showed up. What that means
   depends on real_period_measured: once true, it's unambiguous loss of a
   confirmed lock and aborts immediately, exactly as any missing edge was
   treated before phase lead existed; before that, ARR is still only the
   handover's bootstrap estimate, which can legitimately take more than one
   retry to line up with the tank's real period, so it instead charges one
   unit of the lenient acquire_grace_remaining budget and keeps waiting. This
   does cost some fault-detection latency post-lock compared to before (a
   real loss of feedback now takes about one extra predicted period to
   catch, instead of being caught on the very next update) -- accepted as
   the direct, unavoidable price of scheduling the lead-wrap in the first
   place. */
static volatile uint8_t lead_wrap_pending = 0;

/* Diagnostics: bumped every time a burst is aborted for lack/quality of
   feedback. gate_state == GATE_FAULT latches until ClearFeedbackFault() is
   called (from the USB console's CLR command) -- bursts stay suppressed
   until then, since TIM2's ISR only (re)starts a burst from GATE_IDLE. */
volatile uint32_t feedback_fault_count = 0;
volatile FaultReason_t last_fault_reason = FAULT_NONE;
volatile uint32_t last_fault_width_ticks = 0;
volatile uint8_t fault_message_pending = 0;

/* Burst length knobs, in TIM1 half-cycles. Set via the USB console
   (LEADIN / WIDTH) to tune the open-loop lead-in and the ZCS-locked
   burst length. */
volatile uint32_t burst_openloop_cycles = BURST_OPENLOOP_CYCLES_DEFAULT;
volatile uint32_t burst_closedloop_cycles = BURST_CLOSEDLOOP_CYCLES_DEFAULT;

/* Plausible ZCD half-cycle width, in DWT cycle-counter ticks. Set via the
   USB console's PULSE (microseconds in, converted here). */
volatile uint32_t pulse_width_min_ticks = US_TO_TICKS(PULSE_WIDTH_MIN_US_DEFAULT);
volatile uint32_t pulse_width_max_ticks = US_TO_TICKS(PULSE_WIDTH_MAX_US_DEFAULT);

/* Digital phase lead, in nanoseconds (0-PHASE_LEAD_MAX_NS) -- a FIXED time
   value, not an angle, matching a comparator/gate-driver/device turn-on
   delay you'd measure once on a scope, so it does NOT itself scale with
   tank frequency. Set via the USB console's PHASELEAD; phase_lead_ticks
   (below) is the same value pre-converted to TIM1 ticks at set-time so the
   ISR does a plain subtraction, no per-cycle division. NOT related to
   burst_openloop_cycles' open-loop lead-in -- a different, unrelated use of
   the word "lead".
     The EFFECTIVE lead actually applied each cycle can still end up less
   than this, though: ComputeClosedLoopArrCcr() derates phase_lead_ticks to
   at most PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T of the current lead-in period
   (1/leadin_freq -- see that macro's declaration comment). That's a
   deliberate safety clamp, not a contradiction of "fixed, not scaled": it
   only ever pulls the applied lead *down* from what PHASELEAD asked for,
   never up, and only matters once the requested ns value would otherwise be
   an unreasonably large fraction of a short lead-in period.

   Applied to BOTH closed-loop switching instants per tank cycle -- the
   below-50%-point transition (CCR1/CCR2) AND, now, the boundary transition
   too, via ARR (see ComputeClosedLoopArrCcr() and the GATE_CLOSEDLOOP branch
   of the TIM1 ISR below). A true hardware-native lead on the boundary edge
   -- i.e. making the *real* ETR/ZCD-triggered reset itself fire early --
   isn't physically possible (you can't react before a real edge arrives),
   and the two ways to sidestep that were both dead ends on this board's
   wiring:
    - STM32's Asymmetric PWM mode would need a second CH3/CH4-driven leg
      this board's TIM1 wiring doesn't have (only CH1/CH1N + CH2/CH2N reach
      the bridge), plus center-aligned counting whose interaction with the
      existing ETR Reset-slave-mode fault detection wasn't something to
      guess at on hardware that can shoot-through.
    - True both-edge ZCD detection (TIM1 TS=TI1F_ED instead of ETRF) is also
      not available on this board: TIM1_ETR lands on PA12, whose alternate
      functions don't include TIM1_CH1 (TI1F_ED's only source, per RM0477 --
      ETRF itself has no both-edges option), and TIM1 isn't one of the
      timers this part's TISEL register can remux a TI input for.
   Instead, ARR is shortened by phase_lead_ticks each cycle so TIM1 wraps
   (and CH1/CH2 take their post-boundary state) lead_ticks *before* the real
   edge is predicted to arrive, using the same last-full-cycle measurement
   already used for the CCR prediction -- see ComputeClosedLoopArrCcr(). The
   real edge, arriving shortly after, still forces its own genuine reset
   every single cycle (SMS=Reset is untouched), so this can never drift more
   than one cycle's prediction error away from ground truth -- but it does
   mean a deliberate natural (non-TIF) overflow now happens on every locked
   cycle by design, which changes what the fault-detection logic has to
   tolerate: see lead_wrap_pending above. */
volatile uint32_t phase_lead_ns = 0U;
static volatile uint32_t phase_lead_ticks = 0U;

/* TIM1 PSC/ARR/CCR applied for the open-loop lead-in only (StartBurst()
   below writes all three fresh every burst; the GATE_OPENLOOP->
   GATE_CLOSEDLOOP handover in the TIM1 ISR always resets PSC back to 0
   before ETR takes over, and reads leadin_psc/leadin_arr one more time --
   after that reset -- to seed the first closed-loop ARR/CCR, see the
   handover code's own comment). Set together, indirectly, via
   System_SetLeadInFreqHz()/LEADINHZ: it stores the (PSC, ARR) pair
   that gets closest to the requested Hz, plus the matching 50% duty CCR for
   that ARR, since those are what's actually loaded into hardware. */
static volatile uint32_t leadin_psc = 0U;
static volatile uint32_t leadin_arr = TIM1_ARR_TICKS;
static volatile uint32_t leadin_ccr = TIM1_CCR_NOMINAL;

/* Manual lead-in frequency sweep (SWEEP / bare ENTER on the console),
   entirely separate from the normal TIM2-timed FIRING burst train: each
   bare ENTER fires exactly one burst at sweep_next_hz via StartBurst() and
   advances it, so the operator can watch each frequency on a scope one at a
   time. See System_SweepLeadInTrigger(). */
static volatile uint8_t sweep_active = 0U;
static volatile uint32_t sweep_stop_hz = 0U;
static volatile uint32_t sweep_step_hz = 0U;
static volatile uint32_t sweep_next_hz = 0U;     /* Hz ENTER will fire next; 0 == nothing pending */
static volatile uint8_t sweep_descending = 0U;

/* Set by StartBurst()'s caller: when true, this burst is a manual
   SWEEP step -- open-loop lead-in pulses only, hard-switched at the
   fixed leadin_arr/leadin_ccr for the full burst_openloop_cycles count, with
   no ETR/ZCD handover and none of the closed-loop feedback-quality
   protection (TIF/pulse-width fault checks). The normal TIM2-timed FIRING
   burst train always passes false, keeping its existing closed-loop
   behaviour. */
static volatile uint8_t leadin_only_burst = 0U;
/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
static void MX_GPIO_Init(void);
static void MX_TIM1_Init(void);
static void MX_TIM2_Init(void);
/* USER CODE BEGIN PFP */

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */

/**
  * @brief  Launch one open-loop lead-in burst on TIM1 right now.
  *
  * Shared by the normal TIM2-timed FIRING burst train (called from
  * HAL_TIM_PeriodElapsedCallback() below, already in ISR context) and the
  * manual SWEEP trigger (System_SweepLeadInTrigger(), called from
  * console RX context with IRQs disabled by its caller) -- both call sites
  * must have already confirmed gate_state == GATE_IDLE (and, for the sweep,
  * system_state == SYS_ARMED) before calling this.
  * @param  leadin_only  See leadin_only_burst's declaration comment: true
  *         for a manual SWEEP step (open-loop pulses only, no ZCD
  *         handover/protection), false for the normal closed-loop FIRING
  *         burst train.
  */
static void StartBurst(uint8_t leadin_only)
{
  leadin_only_burst = leadin_only;
  TIM1->CNT = 0;
  TIM1->PSC = leadin_psc;
  TIM1->ARR = leadin_arr;
  TIM1->RCR = (burst_openloop_cycles > 0U) ? (burst_openloop_cycles - 1U) : 0U;
  /* A prior burst's closed-loop tracking (see below) may have left
     CCR1/CCR2 drifted away from 50% of ARR. The fresh open-loop lead-in
     runs at a fixed ARR (leadin_arr, until this same burst hands over), so
     start it from that ARR's correct symmetric point (leadin_ccr) every
     time rather than inheriting the last burst's last value. */
  TIM1->CCR1 = leadin_ccr;
  TIM1->CCR2 = leadin_ccr;
  TIM1->SMCR &= ~TIM_SMCR_SMS;
  /* Push the RCR/CCR1/CCR2/PSC/ARR values just written above into their active
     registers right now (EGR.UG), rather than waiting up to one whole ARR
     period for the first natural update to load them. The catch: on
     advanced timers EGR.UG unconditionally generates a *real* update event
     -- interrupt included -- regardless of RCR, unlike a natural overflow
     (which only interrupts once RCR reaches 0). Without URS, that forced
     interrupt fires almost immediately with gate_state already
     GATE_OPENLOOP (set just below), and HAL_TIM_PeriodElapsedCallback reads
     it as "the lead-in already elapsed" -- handing over to ETR/closed-loop
     after zero real half-cycles, independent of burst_openloop_cycles.
     Setting URS first restricts this one forced update to a register-only
     reload: no interrupt fires until URS is cleared, on the next genuine
     trigger (RCR overflow or, once closed-loop, an ETR reset). */
  TIM1->CR1 |= TIM_CR1_URS;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->CR1 &= ~TIM_CR1_URS;
  closedloop_edge_count = 0;
  gate_state = GATE_OPENLOOP;
  TIM1->CR1 |= TIM_CR1_CEN;
  TIM1->BDTR |= TIM_BDTR_MOE;
}

/**
  * @brief  Compute the lead-compensated (ARR, CCR1=CCR2) pair for one
  *         closed-loop half [a full 360-degree tank period], given that
  *         period in TIM1 ticks (PSC=0, the fixed rate closed loop always
  *         runs at).
  * @param  period_ticks  The full period this half should track: either a
  *         just-measured real one (per-edge tracking), or the open-loop
  *         lead-in's known period, used as the best available estimate
  *         before any real closed-loop edge exists (handover bootstrap).
  * @note   ARR is computed and floor-clamped first; CCR is then clamped
  *         relative to THAT ARR, not a fixed constant -- ARR is no longer
  *         fixed during closed loop (see TIM1_ARR_TICKS' declaration
  *         comment), so clamping CCR against a stale fixed value could let
  *         it end up outside whatever period is actually in use this cycle.
  */
/**
  * @brief  The phase_lead_ticks actually applied to a closed-loop cycle,
  *         after derating to at most PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T of
  *         the current lead-in period (see that macro's declaration
  *         comment) -- computed from leadin_psc/leadin_arr exactly like the
  *         handover bootstrap period is, so it tracks whatever
  *         LEADINHZ is currently set to rather than a fixed constant.
  *         This keeps a large PHASELEAD ns value (now up to 7000) from
  *         becoming a nonsensically large fraction of a much shorter cycle
  *         if leadin_freq is raised, without capping what's usable at low
  *         leadin_freq. Shared by ComputeClosedLoopArrCcr() (which uses the
  *         tick value directly) and System_GetEffectivePhaseLeadNs() (which
  *         converts it back to ns for STATUS).
  * @note   leadin_period_ticks * PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T is
  *         computed in 64 bits: leadin_psc maxes out at 65535 and
  *         leadin_arr at TIM1_ARR_TICKS (2999), so leadin_period_ticks alone
  *         can't exceed ~197M (comfortably inside uint32_t, as
  *         ComputeLeadInPscArr()'s bootstrap use of the same product notes)
  *         -- but that times a 0-100 percentage can exceed 32 bits, and
  *         TIM1_LEADIN_FREQ_MIN_HZ (the only reason leadin_period_ticks
  *         stays far smaller than that in practice) is a tunable, not a
  *         guarantee this arithmetic should depend on.
  */
static uint32_t EffectivePhaseLeadTicks(void)
{
  uint32_t leadin_period_ticks = (leadin_psc + 1U) * (leadin_arr + 1U);
  uint32_t lead_cap_ticks = (uint32_t)(((uint64_t)leadin_period_ticks *
                                         PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T) / 100ULL);
  return (phase_lead_ticks < lead_cap_ticks) ? phase_lead_ticks : lead_cap_ticks;
}

static void ComputeClosedLoopArrCcr(uint32_t period_ticks, uint32_t *out_arr, uint32_t *out_ccr)
{
  uint32_t lead_ticks = EffectivePhaseLeadTicks();

  uint32_t arr = (period_ticks > lead_ticks) ? (period_ticks - lead_ticks) : period_ticks;
  if (arr < (2U * TIM1_CCR_MARGIN_TICKS))
  {
    arr = 2U * TIM1_CCR_MARGIN_TICKS;
  }

  uint32_t ccr = period_ticks / 2U;
  ccr = (ccr > lead_ticks) ? (ccr - lead_ticks) : 0U;
  if (ccr < TIM1_CCR_MARGIN_TICKS)
  {
    ccr = TIM1_CCR_MARGIN_TICKS;
  }
  else if (ccr > (arr - TIM1_CCR_MARGIN_TICKS))
  {
    ccr = arr - TIM1_CCR_MARGIN_TICKS;
  }

  *out_arr = arr;
  *out_ccr = ccr;
}

/**
  * @brief  Burst state machine, driven from the TIM1/TIM2 update interrupts.
  *
  * TIM2's update event starts a burst via StartBurst(): TIM1's slave mode is
  * left disabled, RCR is loaded with (burst_openloop_cycles - 1) and TIM1
  * free-runs at ARR (at the lead-in prescaler set via LEADINHZ).
  * Because no ETR reset happens yet, RCR decrements normally on every real
  * counter overflow, so TIM1's own update ISR only fires once the open-loop
  * lead-in has elapsed.
  *
  * At that point TIM1_ETR (ZCD) slave-mode reset is armed. From RM0477: a
  * slave-mode reset always forces a real update event (and the interrupt
  * that comes with it) regardless of RCR, and does not decrement RCR itself
  * -- RCR only decrements on a genuine counter overflow. So once locked, this
  * callback runs once per ZCD edge, and the closed-loop burst length is
  * counted in software (closedloop_edge_count) rather than via RCR.
  *
  * __HAL_TIM_MOE_DISABLE()/__HAL_TIM_DISABLE() are not used for teardown:
  * both are no-ops unless every CCx/CCxN channel is already disabled, and
  * CH1/CH1N/CH2/CH2N are kept permanently enabled here so OSSI holds the
  * gates at their idle (low) level whenever MOE = 0.
  *
  * Feedback-quality protection has two independent legs, both landing on the
  * same GATE_FAULT abort:
  *  - SR.TIF distinguishes a genuine ETR-driven reset from a natural ARR
  *    overflow (see the GATE_CLOSEDLOOP branch below). ComputeClosedLoopArrCcr()'s
  *    deliberate phase_lead_ticks shortening makes exactly ONE natural
  *    overflow (the lead-wrap) expected every cycle, from the very first
  *    cycle after handover on -- whether ARR is still the handover's
  *    bootstrap estimate or, from the second real edge on, an actual
  *    measurement -- and that first one is always tolerated for free (see
  *    lead_wrap_pending's declaration comment above). A SECOND natural
  *    overflow in the same cycle, with no genuine edge in between, means the
  *    expected edge never showed up: post-lock (real_period_measured) that's
  *    unambiguous loss of feedback and aborts immediately; pre-lock it
  *    instead charges one unit of the FEEDBACK_ACQUIRE_GRACE_CYCLES budget
  *    and keeps waiting, since the bootstrap estimate can plausibly overflow
  *    more than once if it doesn't closely match the tank's real period.
  *  - For edges that DO arrive, their spacing (DWT->CYCCNT delta between
  *    consecutive accepted edges) is checked against
  *    [pulse_width_min_ticks, pulse_width_max_ticks]. This catches a
  *    *present but wrong* edge -- e.g. comparator noise firing early, mid
  *    conduction, which TIF alone cannot see since a spurious edge still
  *    sets TIF.
  * Either failure aborts the burst into GATE_FAULT (latched, LED on, MOE
  * forced off, system_state dropped to SYS_ARMED) rather than let TIM1 keep
  * switching a resonant tank with no confirmed-good zero-crossing.
  *
  * Note the very first accepted edge after handover only establishes the
  * width-check baseline -- it is never itself judged, since the interval
  * from the handover instant (not a real edge) to it isn't a genuine
  * half-cycle. ARR/CCR1/CCR2 are still correct for that whole interval
  * though: the GATE_OPENLOOP->GATE_CLOSEDLOOP handover below seeds them from
  * the open-loop lead-in's own known period (via ComputeClosedLoopArrCcr())
  * rather than a generic fixed guess, precisely so this un-judged interval
  * still runs close to the tank's real frequency, lead included, instead of
  * an arbitrary one. From the second accepted edge on, each measured width
  * re-derives a fresh ARR/CCR pair from that real, just-measured period
  * (same helper) -- necessary since, once ETR is setting the real period,
  * holding either at a stale value lets one leg's conduction (or its whole
  * pulse, if the period runs shorter than 2x the old CCR) drift or
  * disappear as the tracked frequency moves.
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  if (htim->Instance == TIM2)
  {
    if ((system_state == SYS_FIRING) && (gate_state == GATE_IDLE))
    {
      StartBurst(0U);
    }
    /* else: not firing, or TIM2 fired again before the previous burst
       finished (burst_closedloop_cycles too long for the current burst
       repetition rate) -- drop this trigger rather than corrupt a running
       burst or start one nobody asked for. */
  }
  else if (htim->Instance == TIM1)
  {
    if (gate_state == GATE_OPENLOOP)
    {
      if (leadin_only_burst)
      {
        /* Manual SWEEP step: the open-loop lead-in pulses are the
           whole point of this burst, so stop right here -- no ETR/ZCD
           handover, and therefore none of the closed-loop feedback-quality
           protection (TIF / pulse-width checks) either. Just hard-switch
           for burst_openloop_cycles half-cycles and idle, same teardown as
           ForceStopBurst() / a completed closed-loop burst. */
        TIM1->BDTR &= ~TIM_BDTR_MOE;
        TIM1->SMCR &= ~TIM_SMCR_SMS;
        TIM1->CR1 &= ~TIM_CR1_CEN;
        gate_state = GATE_IDLE;
        return;
      }
      /* Open-loop lead-in elapsed: hand over to TIM1_ETR (ZCD) lock. TS is
         already ETRF from MX_TIM1_Init(); only SMS needs to change.
         RCR must be dropped to 0 here too: it is shadow-buffered and still
         holds (burst_openloop_cycles - 1) from burst start, so if it were
         left alone a *natural* overflow (ETR silent / no comparator yet)
         would only produce a real update event -- and only increment
         closedloop_edge_count -- once every (old RCR + 1) physical cycles
         instead of every cycle, silently stretching the closed-loop phase
         by that same factor. With RCR = 0, every physical cycle produces a
         real update event whether it comes from a genuine ETR edge (which
         always forces one regardless of RCR) or from free-running overflow
         when no ZCD edge arrives, so the software count is accurate either
         way. */
      TIM1->RCR = 0;
      /* Likewise restore PSC to 0 (full speed): the closed-loop ARR/CCR
         tracking below converts DWT->CYCCNT deltas to TIM1 ticks with a
         fixed divide-by-2, only valid at the full 202.5 MHz kernel clock --
         leadin_psc may have set it away from that for the lead-in which
         just ended. Same shadow-buffered timing as RCR above: takes effect
         at the next update, which is what arms it. */
      TIM1->PSC = 0;
      /* Bootstrap ARR/CCR1/CCR2 for this very first closed-loop half-cycle,
         before any real ZCD edge has been measured yet: rather than jumping
         to a generic fixed guess, seed them from the open-loop lead-in
         period that was JUST driving this exact tank (leadin_psc/leadin_arr,
         converted to its PSC=0-equivalent tick count) via the same
         ComputeClosedLoopArrCcr() the real per-edge measurements use below --
         LEADINHZ is meant to be tuned close to the tank's real
         resonance, so this keeps the open-loop->closed-loop handover a
         smooth continuation instead of an abrupt jump to an unrelated
         period, and applies the phase lead from this very first half-cycle
         too rather than waiting for real measurements to catch up.
         This bootstrap value stays in effect through the first REAL edge as
         well -- see that branch below, which establishes the timing
         baseline without touching ARR/CCR -- and is only superseded once a
         genuine measured period exists, from the second real edge on.
         leadin_psc maxes out at 65535 and leadin_arr at TIM1_ARR_TICKS
         (2999), so this product can never exceed ~197M, comfortably inside
         uint32_t. */
      {
        uint32_t bootstrap_period_ticks = (leadin_psc + 1U) * (leadin_arr + 1U);
        uint32_t bootstrap_arr, bootstrap_ccr;
        ComputeClosedLoopArrCcr(bootstrap_period_ticks, &bootstrap_arr, &bootstrap_ccr);
        TIM1->ARR = bootstrap_arr;
        TIM1->CCR1 = bootstrap_ccr;
        TIM1->CCR2 = bootstrap_ccr;
      }
      TIM1->SMCR = (TIM1->SMCR & ~TIM_SMCR_SMS) | TIM_SLAVEMODE_RESET;
      /* Clear any stale TIF latched while SMS was still disabled, so the
         first GATE_CLOSEDLOOP check can't mistake it for a post-handover
         edge and falsely confirm lock. */
      __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_TRIGGER);
      feedback_locked = 0;
      real_period_measured = 0;
      acquire_grace_remaining = FEEDBACK_ACQUIRE_GRACE_CYCLES;
      lead_wrap_pending = 0;
      last_edge_timestamp = DWT->CYCCNT;
      gate_state = GATE_CLOSEDLOOP;
    }
    else if (gate_state == GATE_CLOSEDLOOP)
    {
      /* SR.TIF is set by hardware only when this update event was actually
         caused by a valid TRGI (ETR/ZCD) edge -- a natural ARR overflow
         with no edge leaves it clear. This is the per-cycle feedback-quality
         signal: it tells apart "ZCS-locked, running normally" from
         "free-running because no ZCD edge arrived", at zero extra latency.
         TIF is read (and, when consumed, cleared) entirely by this callback
         -- TIM_IT_TRIGGER is never enabled (only TIM_IT_UPDATE is, in
         main()), so HAL_TIM_IRQHandler never touches it.
           Clearing it is deliberately NOT unconditional here. With
         phase_lead_ns > 0 the genuine ETR edge is expected only
         phase_lead_ticks after the deliberate lead-wrap's own update event
         -- as little as a handful of CPU cycles at small settings, close
         enough to ordinary interrupt-entry jitter that the genuine edge's
         hardware TIF-set can land in the few-instruction gap between a read
         of SR here and an unconditional clear-flag write right after it.
         Clearing unconditionally there would wipe out a TIF that hardware
         just set for a real edge this callback hasn't even recognized yet
         (read correctly as 0 at the read above, an instant before the real
         edge landed) -- clobbered before the *next* callback invocation
         (triggered by that same edge's own UIF) gets a chance to see it, so
         a perfectly genuine edge gets misread as a second natural overflow
         and, post-lock, faults immediately -- exactly the "ZCD faults with
         clean feedback, only with phase lead set" symptom this was written
         to fix. Only clearing TIF once it's actually been read as set
         (below) closes the window: on a true natural overflow TIF is
         already 0, so leaving it untouched here is a no-op, and if the race
         above did leave it set, the very next read (right here, on
         whichever invocation actually observes it) still finds it and
         handles the edge correctly instead of losing it. */
      uint8_t trig = (TIM1->SR & TIM_SR_TIF) != 0U;

      if (trig)
      {
        __HAL_TIM_CLEAR_FLAG(&htim1, TIM_FLAG_TRIGGER);
        uint32_t now = DWT->CYCCNT;
        /* A genuine edge arrived (whether or not it's the first since
           handover) -- clear any pending "expected lead-wrap" streak so the
           natural-overflow branch below starts counting fresh for the next
           cycle. */
        lead_wrap_pending = 0;

        if (!feedback_locked)
        {
          /* First real edge since handover: last_edge_timestamp is only the
             handover instant (an artifact of when the open-loop RCR expired),
             not a prior real edge, so the interval to here is not a genuine
             half-cycle width -- it's essentially the ZCD's arbitrary phase
             offset from that instant. Judging it against the plausibility
             window was the bug behind "fault after just one cycle" with a
             signal applied: establish the baseline here instead, and start
             judging widths from the *next* edge, where both ends are real. */
          last_edge_timestamp = now;
          feedback_locked = 1;
          closedloop_edge_count++;
          if (closedloop_edge_count >= burst_closedloop_cycles)
          {
            TIM1->BDTR &= ~TIM_BDTR_MOE;
            TIM1->SMCR &= ~TIM_SMCR_SMS;
            TIM1->CR1 &= ~TIM_CR1_CEN;
            gate_state = GATE_IDLE;
          }
        }
        else
        {
          uint32_t width_ticks = now - last_edge_timestamp;
          last_edge_timestamp = now;
          /* This edge is the first with a real, just-measured width_ticks
             behind it (the first real edge, above, only established a
             baseline) -- from here on, every following ARR/CCR1/CCR2 update
             is derived from an actual measurement, so the natural-overflow
             branch below can switch over to its strict one-overflow
             tolerance starting with the interval this edge is about to
             open. See real_period_measured's declaration comment. */
          real_period_measured = 1;

          if ((width_ticks < pulse_width_min_ticks) || (width_ticks > pulse_width_max_ticks))
          {
            /* Edge arrived, but not where a real zero-crossing could be --
               treated as a fault unconditionally, with no acquisition-grace
               tolerance: an implausible edge is itself evidence of a problem,
               not a transient still settling. */
            TIM1->BDTR &= ~TIM_BDTR_MOE;
            TIM1->SMCR &= ~TIM_SMCR_SMS;
            TIM1->CR1 &= ~TIM_CR1_CEN;
            gate_state = GATE_FAULT;
            system_state = SYS_ARMED;
            last_fault_reason = FAULT_PULSE_WIDTH;
            last_fault_width_ticks = width_ticks;
            feedback_fault_count++;
            fault_message_pending = 1U;
            HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
          }
          else
          {
            /* Track the tank: re-derive ARR and CCR1/CCR2 from the
               just-measured real period (period_tim1_ticks, a full
               360-degree cycle) so neither the boundary transition (ARR) nor
               leg B's mid-point transition (CCR) drifts off the true
               zero-crossing as the tracked frequency moves. CPU clock is
               exactly 2x TIM1's kernel clock (405 MHz / 202.5 MHz), so DWT
               ticks -> TIM1 ticks is an exact divide-by-2, no rounding drift
               accumulates. ARR/CCR1/CCR2 are all preload-buffered (ARPE /
               OCxPE), so this write is glitch-free: it only takes effect at
               the next update event, never the cycle just measured.
               ComputeClosedLoopArrCcr() applies phase_lead_ticks to both. */
            uint32_t period_tim1_ticks = width_ticks / 2U;
            uint32_t new_arr, new_ccr;
            ComputeClosedLoopArrCcr(period_tim1_ticks, &new_arr, &new_ccr);
            TIM1->ARR = new_arr;
            TIM1->CCR1 = new_ccr;
            TIM1->CCR2 = new_ccr;

            closedloop_edge_count++;
            if (closedloop_edge_count >= burst_closedloop_cycles)
            {
              TIM1->BDTR &= ~TIM_BDTR_MOE;
              TIM1->SMCR &= ~TIM_SMCR_SMS;
              TIM1->CR1 &= ~TIM_CR1_CEN;
              gate_state = GATE_IDLE;
            }
          }
        }
      }
      else if (lead_wrap_pending)
      {
        /* Second natural overflow in a row with no genuine edge in between.
           ComputeClosedLoopArrCcr() shortens ARR by phase_lead_ticks
           regardless of whether it's working from the handover's bootstrap
           estimate or a real measurement, so the FIRST natural overflow
           every cycle is always the expected lead-wrap and was already
           tolerated for free below -- this is specifically a SECOND one,
           meaning the genuine edge itself never showed up (see
           lead_wrap_pending's declaration comment).
             Once ARR is driven by a real measurement (real_period_measured)
           that is unambiguous loss of a confirmed lock -- abort immediately
           rather than keep switching a resonant tank on a stale period
           estimate. Before that, ARR is still only the handover's bootstrap
           estimate, which can legitimately take more than one retry to line
           up with the tank's real period (see real_period_measured's
           declaration comment), so charge one unit of the lenient
           acquire_grace_remaining budget instead and keep waiting -- only
           aborting once that budget itself is exhausted. */
        if (real_period_measured || (acquire_grace_remaining == 0U))
        {
          TIM1->BDTR &= ~TIM_BDTR_MOE;
          TIM1->SMCR &= ~TIM_SMCR_SMS;
          TIM1->CR1 &= ~TIM_CR1_CEN;
          gate_state = GATE_FAULT;
          system_state = SYS_ARMED;
          last_fault_reason = FAULT_NO_EDGE;
          feedback_fault_count++;
          fault_message_pending = 1U;
          HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_SET);
        }
        else
        {
          /* Pre-lock retry: charge the budget and keep lead_wrap_pending
             set -- a genuine edge is still needed to clear it before another
             overflow is tolerated for free again. */
          acquire_grace_remaining--;
        }
      }
      else
      {
        /* First natural overflow this cycle: the expected lead-wrap (see
           lead_wrap_pending's declaration comment), tolerated for free
           regardless of lock state. Wait for the genuine edge (or, if the
           period prediction was a little off, one more natural overflow)
           next. */
        lead_wrap_pending = 1;
      }
    }
  }
}

/**
  * @brief  Unconditionally drive TIM1 to the safe idle state, regardless of
  *         which gate_state it was in. Caller must already hold IRQs
  *         disabled (System_Disarm() / System_ToggleFiring() do).
  */
static void ForceStopBurst(void)
{
  TIM1->BDTR &= ~TIM_BDTR_MOE;
  TIM1->SMCR &= ~TIM_SMCR_SMS;
  TIM1->CR1 &= ~TIM_CR1_CEN;
  if (gate_state != GATE_FAULT)
  {
    gate_state = GATE_IDLE;
  }
}

void System_Arm(void)
{
  __disable_irq();
  if (system_state == SYS_DISARMED)
  {
    system_state = SYS_ARMED;
  }
  __enable_irq();
}

void System_Disarm(void)
{
  __disable_irq();
  system_state = SYS_DISARMED;
  ForceStopBurst();
  __enable_irq();
}

ToggleResult_t System_ToggleFiring(void)
{
  ToggleResult_t result = TOGGLE_IGNORED_NOT_ARMED;
  __disable_irq();
  if (system_state == SYS_ARMED)
  {
    system_state = SYS_FIRING;
    result = TOGGLE_NOW_FIRING;
  }
  else if (system_state == SYS_FIRING)
  {
    system_state = SYS_ARMED;
    ForceStopBurst();
    result = TOGGLE_NOW_STOPPED;
  }
  __enable_irq();
  return result;
}

uint8_t System_SetBurstWidth(uint32_t cycles)
{
  if ((cycles == 0U) || (cycles > 100000U)) return 0U;
  burst_closedloop_cycles = cycles;
  return 1U;
}

uint8_t System_SetLeadInCycles(uint32_t cycles)
{
  if ((cycles == 0U) || (cycles > 1000U)) return 0U;
  burst_openloop_cycles = cycles;
  return 1U;
}

/**
  * @brief  Convert a requested lead-in Hz to a (PSC, ARR) pair and validate it.
  * @note   Internal helper: does not check state preconditions, only
  *         arithmetic validity (freq_hz in
  *         [TIM1_LEADIN_FREQ_MIN_HZ, TIM1_LEADIN_FREQ_MAX_HZ]).
  *         Shared by System_SetLeadInFreqHz() and the sweep start/step code.
  *         Below TIM1_KERNEL_CLK_HZ/(TIM1_ARR_TICKS+1) (67500 Hz), ARR stays
  *         at TIM1_ARR_TICKS and PSC divides the clock down -- the original
  *         mapping, for backwards-compatible resolution/behaviour at any Hz
  *         that was already reachable. Above it, up to
  *         TIM1_LEADIN_FREQ_MAX_HZ, PSC stays 0 and ARR itself shrinks
  *         instead, since PSC can only divide the clock down, never up.
  */
static uint8_t ComputeLeadInPscArr(uint32_t freq_hz, uint32_t *out_psc, uint32_t *out_arr)
{
  if ((freq_hz < TIM1_LEADIN_FREQ_MIN_HZ) || (freq_hz > TIM1_LEADIN_FREQ_MAX_HZ)) return 0U;

  if (freq_hz <= (TIM1_KERNEL_CLK_HZ / (TIM1_ARR_TICKS + 1U)))
  {
    uint64_t denom = (uint64_t)freq_hz * (TIM1_ARR_TICKS + 1U);
    uint64_t psc_plus1 = (TIM1_KERNEL_CLK_HZ + (denom / 2ULL)) / denom;
    if (psc_plus1 < 1ULL) psc_plus1 = 1ULL;
    if (psc_plus1 > 65536ULL) return 0U;
    *out_psc = (uint32_t)(psc_plus1 - 1ULL);
    *out_arr = TIM1_ARR_TICKS;
  }
  else
  {
    uint64_t arr_plus1 = (TIM1_KERNEL_CLK_HZ + ((uint64_t)freq_hz / 2ULL)) / (uint64_t)freq_hz;
    if (arr_plus1 < 1ULL) arr_plus1 = 1ULL;
    if (arr_plus1 > (uint64_t)(TIM1_ARR_TICKS + 1U)) return 0U;  /* defensive: shouldn't happen */
    *out_psc = 0U;
    *out_arr = (uint32_t)(arr_plus1 - 1ULL);
  }
  return 1U;
}

/**
  * @brief  50%-duty CCR for a given ARR, margin-clamped the same way the
  *         closed-loop tracking clamps its own CCR (see the TIM1 ISR).
  *         Purely defensive here -- every ARR ComputeLeadInPscArr() can
  *         produce (down to 1349 at TIM1_LEADIN_FREQ_MAX_HZ) leaves ARR/2
  *         far above TIM1_CCR_MARGIN_TICKS -- but cheap enough to not skip.
  */
static uint32_t LeadInNominalCcr(uint32_t arr)
{
  uint32_t ccr = (arr + 1U) / 2U;
  if (ccr < TIM1_CCR_MARGIN_TICKS)
  {
    ccr = TIM1_CCR_MARGIN_TICKS;
  }
  else if (ccr > (arr - TIM1_CCR_MARGIN_TICKS))
  {
    ccr = arr - TIM1_CCR_MARGIN_TICKS;
  }
  return ccr;
}

/**
  * @brief  Set the open-loop lead-in drive frequency via TIM1's PSC/ARR.
  * @note   See ComputeLeadInPscArr() for the (PSC, ARR) mapping and
  *         TIM1_LEADIN_FREQ_MIN_HZ/TIM1_LEADIN_FREQ_MAX_HZ for the range;
  *         requesting outside it fails. Also changes the current lead-in
  *         period T used to derate phase_lead_ticks (see
  *         PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T's declaration comment) for any
  *         *closed-loop* cycle computed from here on -- unlike the PSC/ARR/
  *         CCR values below, that effect isn't deferred to the next burst.
  *         Takes effect on the *next* burst StartBurst() launches, not
  *         retroactively on one already running.
  */
uint8_t System_SetLeadInFreqHz(uint32_t freq_hz)
{
  uint32_t psc, arr;
  if (!ComputeLeadInPscArr(freq_hz, &psc, &arr)) return 0U;
  leadin_psc = psc;
  leadin_arr = arr;
  leadin_ccr = LeadInNominalCcr(arr);
  return 1U;
}

uint32_t System_GetLeadInFreqHz(void)
{
  uint64_t denom = (uint64_t)(leadin_psc + 1U) * (leadin_arr + 1U);
  return (uint32_t)((TIM1_KERNEL_CLK_HZ + (denom / 2ULL)) / denom);
}

/**
  * @brief  Arm a manual lead-in frequency sweep from start_hz to stop_hz in
  *         steps of step_hz (either direction; stop_hz < start_hz sweeps
  *         down). Does not fire anything itself -- the first burst fires on
  *         the first subsequent bare ENTER, same as every step after it.
  * @note   Requires SYS_ARMED (not SYS_FIRING, so TIM2's own burst trigger
  *         can't race the manual one) and GATE_IDLE. Both endpoints are
  *         range-checked up front so a sweep can't die mid-way on an
  *         out-of-range step; every Hz between two in-range endpoints is
  *         itself in range, since ComputeLeadInPscArr()'s two segments meet
  *         at 67500 Hz with no gap, covering
  *         [TIM1_LEADIN_FREQ_MIN_HZ, TIM1_LEADIN_FREQ_MAX_HZ] as one
  *         contiguous valid interval.
  */
uint8_t System_SweepLeadInStart(uint32_t start_hz, uint32_t stop_hz, uint32_t step_hz)
{
  uint32_t psc_unused, arr_unused;
  if ((step_hz == 0U) ||
      !ComputeLeadInPscArr(start_hz, &psc_unused, &arr_unused) ||
      !ComputeLeadInPscArr(stop_hz, &psc_unused, &arr_unused))
  {
    return 0U;
  }

  uint8_t ok = 0U;
  __disable_irq();
  if ((system_state == SYS_ARMED) && (gate_state == GATE_IDLE))
  {
    sweep_descending = (stop_hz < start_hz) ? 1U : 0U;
    sweep_stop_hz = stop_hz;
    sweep_step_hz = step_hz;
    sweep_next_hz = start_hz;
    sweep_active = 1U;
    ok = 1U;
  }
  __enable_irq();
  return ok;
}

/**
  * @brief  Cancel a pending/in-progress sweep so ENTER stops firing bursts.
  * @note   Does not touch a burst already running -- it will finish (or
  *         fault) on its own. DISARM is the hard, immediate stop.
  */
void System_SweepLeadInStop(void)
{
  __disable_irq();
  sweep_active = 0U;
  sweep_next_hz = 0U;
  __enable_irq();
}

uint8_t System_SweepLeadInActive(void)
{
  return sweep_active;
}

uint32_t System_SweepLeadInNextHz(void)
{
  return sweep_active ? sweep_next_hz : 0U;
}

/**
  * @brief  Fire one sweep step (called on a bare console ENTER).
  * @param  out_fired_hz  On SWEEP_TRIGGER_FIRED, set to the Hz just launched.
  */
SweepTriggerResult_t System_SweepLeadInTrigger(uint32_t *out_fired_hz)
{
  SweepTriggerResult_t result;
  __disable_irq();
  if (!sweep_active)
  {
    result = SWEEP_TRIGGER_NOT_ACTIVE;
  }
  else if (system_state != SYS_ARMED)
  {
    /* Armed state moved out from under the sweep (DISARM, or FIRING toggled
       via ESC/console) -- cancel rather than fire into an unexpected state. */
    sweep_active = 0U;
    sweep_next_hz = 0U;
    result = SWEEP_TRIGGER_NOT_ACTIVE;
  }
  else if (gate_state != GATE_IDLE)
  {
    result = SWEEP_TRIGGER_BUSY;
  }
  else if (sweep_next_hz == 0U)
  {
    sweep_active = 0U;
    result = SWEEP_TRIGGER_DONE;
  }
  else
  {
    uint32_t fired_hz = sweep_next_hz;
    uint32_t psc, arr;
    if (!ComputeLeadInPscArr(fired_hz, &psc, &arr))
    {
      /* Can't happen: both endpoints (and so every Hz between them) were
         validated in System_SweepLeadInStart(). Bail out safely if it ever
         does rather than fire at a stale/wrong frequency. */
      sweep_active = 0U;
      sweep_next_hz = 0U;
      result = SWEEP_TRIGGER_NOT_ACTIVE;
    }
    else
    {
      leadin_psc = psc;
      leadin_arr = arr;
      leadin_ccr = LeadInNominalCcr(arr);
      StartBurst(1U);
      if (out_fired_hz != NULL) *out_fired_hz = fired_hz;

      if (sweep_descending)
      {
        if (fired_hz <= sweep_stop_hz)
        {
          sweep_next_hz = 0U;
        }
        else
        {
          uint32_t candidate = (fired_hz > sweep_step_hz) ? (fired_hz - sweep_step_hz) : 0U;
          sweep_next_hz = (candidate <= sweep_stop_hz) ? sweep_stop_hz : candidate;
        }
      }
      else
      {
        if (fired_hz >= sweep_stop_hz)
        {
          sweep_next_hz = 0U;
        }
        else
        {
          uint32_t candidate = fired_hz + sweep_step_hz;
          sweep_next_hz = (candidate >= sweep_stop_hz) ? sweep_stop_hz : candidate;
        }
      }
      result = SWEEP_TRIGGER_FIRED;
    }
  }
  __enable_irq();
  return result;
}

uint8_t System_SetBurstRateBPM(uint32_t bpm)
{
  if (bpm == 0U) return 0U;
  /* TIM2 tick = (PSC+1)/202.5MHz = 300/202.5MHz; period_s = 60/bpm;
     ARR = round(period_s / tick_s) - 1 = round(40500000/bpm) - 1. */
  uint64_t arr64 = (40500000ULL + (bpm / 2U)) / bpm;
  if ((arr64 < 2ULL) || (arr64 > 0xFFFFFFFFULL)) return 0U;
  __HAL_TIM_SET_AUTORELOAD(&htim2, (uint32_t)(arr64 - 1ULL));
  return 1U;
}

uint8_t System_SetPulseWidthRangeUs(uint32_t min_us, uint32_t max_us)
{
  if ((min_us == 0U) || (max_us <= min_us)) return 0U;
  pulse_width_min_ticks = US_TO_TICKS(min_us);
  pulse_width_max_ticks = US_TO_TICKS(max_us);
  return 1U;
}

/**
  * @brief  Set the closed-loop phase lead (see phase_lead_ns's declaration
  *         comment for scope/rationale).
  * @param  ns  0-PHASE_LEAD_MAX_NS. Pre-converted to TIM1 ticks here (at
  *         TIM1_KERNEL_CLK_HZ, PSC=0 -- the fixed rate the closed-loop phase
  *         always runs at) so the ISR just does a plain subtraction. Takes
  *         effect on the next closed-loop CCR1/CCR2 update, not
  *         retroactively.
  */
uint8_t System_SetPhaseLeadNs(uint32_t ns)
{
  if (ns > PHASE_LEAD_MAX_NS) return 0U;
  phase_lead_ns = ns;
  phase_lead_ticks = (uint32_t)(((uint64_t)ns * TIM1_KERNEL_CLK_HZ + 500000000ULL) / 1000000000ULL);
  return 1U;
}

/**
  * @brief  What PHASELEAD's configured ns value actually works out to
  *         right now, after EffectivePhaseLeadTicks()'s derating against the
  *         current lead-in period -- for STATUS, so the derating (see
  *         PHASE_LEAD_MAX_PERCENT_OF_LEADIN_T's declaration comment) isn't
  *         invisible: phase_lead_ns alone only shows what was requested, not
  *         what's actually being applied to a closed-loop cycle right now.
  */
uint32_t System_GetEffectivePhaseLeadNs(void)
{
  uint64_t ticks = EffectivePhaseLeadTicks();
  return (uint32_t)((ticks * 1000000000ULL + (TIM1_KERNEL_CLK_HZ / 2U)) / TIM1_KERNEL_CLK_HZ);
}

uint32_t System_GetBpm(void)
{
  uint32_t arr = __HAL_TIM_GET_AUTORELOAD(&htim2);
  return (uint32_t)(40500000ULL / ((uint64_t)arr + 1ULL));
}

/**
  * @brief  Manually clear a latched feedback fault so bursts can resume.
  * @note   Called from the USB console's CLR/CLEAR command. Deliberately
  *         not auto-cleared or auto-retried: a burst that lost its ZCD lock
  *         should stay off until someone confirms the feedback path is
  *         actually fixed, and firing only resumes on an explicit ESC.
  */
void ClearFeedbackFault(void)
{
  __disable_irq();
  if (gate_state == GATE_FAULT)
  {
    gate_state = GATE_IDLE;
    HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);
  }
  __enable_irq();
}

/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* Enable the CPU Cache */

  /* Enable I-Cache---------------------------------------------------------*/
  SCB_EnableICache();

  /* Enable D-Cache---------------------------------------------------------*/
  SCB_EnableDCache();

  /* MCU Configuration--------------------------------------------------------*/

  /* Update SystemCoreClock variable according to RCC registers values. */
  SystemCoreClockUpdate();

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_USB_DEVICE_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  /* USER CODE BEGIN 2 */
  /* DWT cycle counter: used to timestamp ZCD edges (pulse-width plausibility
     check) at CPU-clock (405 MHz) resolution. */
  CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
  DWT->CYCCNT = 0;
  DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;

  Console_Init();

  /* MX_TIM1_Init() arms slave-mode reset (SMS = Reset, TS = ETRF) from init.
     PA12/TIM1_ETR floats with no comparator attached yet, so leave slave mode
     disabled until the state machine hands over mid-burst. */
  TIM1->SMCR &= ~TIM_SMCR_SMS;

  /* Hardware noise rejection on TIM1_ETR: reject glitches narrower than the
     filter window before they can ever reach the trigger/reset logic. See
     TIM1_ETR_FILTER_VALUE above for the tradeoff. */
  TIM1->SMCR = (TIM1->SMCR & ~TIM_SMCR_ETF) | (TIM1_ETR_FILTER_VALUE << TIM_SMCR_ETF_Pos);

  /* Enable all four channels once. With MOE = 0 this drives no output --
     OSSI holds GATE_x_x low -- the burst state machine below toggles MOE. */
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_1);
  HAL_TIM_PWM_Start(&htim1, TIM_CHANNEL_2);
  HAL_TIMEx_PWMN_Start(&htim1, TIM_CHANNEL_2);
  TIM1->BDTR &= ~TIM_BDTR_MOE;
  TIM1->CR1 &= ~TIM_CR1_CEN;
  __HAL_TIM_ENABLE_IT(&htim1, TIM_IT_UPDATE);

  /* TIM2 free-runs continuously and triggers one burst per period. */
  HAL_TIM_Base_Start_IT(&htim2);
  /* USER CODE END 2 */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    Console_Poll();
  }
  /* USER CODE END 3 */
}

/**
  * @brief TIM1 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM1_Init(void)
{

  /* USER CODE BEGIN TIM1_Init 0 */

  /* USER CODE END TIM1_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_SlaveConfigTypeDef sSlaveConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};
  TIMEx_BreakInputConfigTypeDef sBreakInputConfig = {0};
  TIM_OC_InitTypeDef sConfigOC = {0};
  TIM_BreakDeadTimeConfigTypeDef sBreakDeadTimeConfig = {0};

  /* USER CODE BEGIN TIM1_Init 1 */

  /* USER CODE END TIM1_Init 1 */
  htim1.Instance = TIM1;
  htim1.Init.Prescaler = 0;
  htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim1.Init.Period = 2999;
  htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim1.Init.RepetitionCounter = 0;
  htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim1, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  if (HAL_TIM_PWM_Init(&htim1) != HAL_OK)
  {
    Error_Handler();
  }
  sSlaveConfig.SlaveMode = TIM_SLAVEMODE_RESET;
  sSlaveConfig.InputTrigger = TIM_TS_ETRF;
  sSlaveConfig.TriggerPolarity = TIM_TRIGGERPOLARITY_NONINVERTED;
  sSlaveConfig.TriggerPrescaler = TIM_TRIGGERPRESCALER_DIV1;
  sSlaveConfig.TriggerFilter = 0;
  if (HAL_TIM_SlaveConfigSynchro(&htim1, &sSlaveConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterOutputTrigger2 = TIM_TRGO2_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim1, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakInputConfig.Source = TIM_BREAKINPUTSOURCE_BKIN;
  sBreakInputConfig.Enable = TIM_BREAKINPUTSOURCE_ENABLE;
  sBreakInputConfig.Polarity = TIM_BREAKINPUTSOURCE_POLARITY_HIGH;
  if (HAL_TIMEx_ConfigBreakInput(&htim1, TIM_BREAKINPUT_BRK, &sBreakInputConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM1;
  sConfigOC.Pulse = 1500;
  sConfigOC.OCPolarity = TIM_OCPOLARITY_HIGH;
  sConfigOC.OCNPolarity = TIM_OCNPOLARITY_HIGH;
  sConfigOC.OCFastMode = TIM_OCFAST_DISABLE;
  sConfigOC.OCIdleState = TIM_OCIDLESTATE_RESET;
  sConfigOC.OCNIdleState = TIM_OCNIDLESTATE_RESET;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_1) != HAL_OK)
  {
    Error_Handler();
  }
  sConfigOC.OCMode = TIM_OCMODE_PWM2;
  if (HAL_TIM_PWM_ConfigChannel(&htim1, &sConfigOC, TIM_CHANNEL_2) != HAL_OK)
  {
    Error_Handler();
  }
  sBreakDeadTimeConfig.OffStateRunMode = TIM_OSSR_ENABLE;
  sBreakDeadTimeConfig.OffStateIDLEMode = TIM_OSSI_ENABLE;
  sBreakDeadTimeConfig.LockLevel = TIM_LOCKLEVEL_OFF;
  sBreakDeadTimeConfig.DeadTime = 50;
  sBreakDeadTimeConfig.BreakState = TIM_BREAK_DISABLE;
  sBreakDeadTimeConfig.BreakPolarity = TIM_BREAKPOLARITY_HIGH;
  sBreakDeadTimeConfig.BreakFilter = 0;
  sBreakDeadTimeConfig.BreakAFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.Break2State = TIM_BREAK2_DISABLE;
  sBreakDeadTimeConfig.Break2Polarity = TIM_BREAK2POLARITY_HIGH;
  sBreakDeadTimeConfig.Break2Filter = 0;
  sBreakDeadTimeConfig.Break2AFMode = TIM_BREAK_AFMODE_INPUT;
  sBreakDeadTimeConfig.AutomaticOutput = TIM_AUTOMATICOUTPUT_DISABLE;
  if (HAL_TIMEx_ConfigBreakDeadTime(&htim1, &sBreakDeadTimeConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM1_Init 2 */

  /* USER CODE END TIM1_Init 2 */
  HAL_TIM_MspPostInit(&htim1);

}

/**
  * @brief TIM2 Initialization Function
  * @param None
  * @retval None
  */
static void MX_TIM2_Init(void)
{

  /* USER CODE BEGIN TIM2_Init 0 */

  /* USER CODE END TIM2_Init 0 */

  TIM_ClockConfigTypeDef sClockSourceConfig = {0};
  TIM_MasterConfigTypeDef sMasterConfig = {0};

  /* USER CODE BEGIN TIM2_Init 1 */

  /* USER CODE END TIM2_Init 1 */
  htim2.Instance = TIM2;
  htim2.Init.Prescaler = 299;
  htim2.Init.CounterMode = TIM_COUNTERMODE_UP;
  htim2.Init.Period = 33749;
  htim2.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
  htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
  if (HAL_TIM_Base_Init(&htim2) != HAL_OK)
  {
    Error_Handler();
  }
  sClockSourceConfig.ClockSource = TIM_CLOCKSOURCE_INTERNAL;
  if (HAL_TIM_ConfigClockSource(&htim2, &sClockSourceConfig) != HAL_OK)
  {
    Error_Handler();
  }
  sMasterConfig.MasterOutputTrigger = TIM_TRGO_RESET;
  sMasterConfig.MasterSlaveMode = TIM_MASTERSLAVEMODE_DISABLE;
  if (HAL_TIMEx_MasterConfigSynchronization(&htim2, &sMasterConfig) != HAL_OK)
  {
    Error_Handler();
  }
  /* USER CODE BEGIN TIM2_Init 2 */

  /* USER CODE END TIM2_Init 2 */

}

/**
  * @brief GPIO Initialization Function
  * @param None
  * @retval None
  */
static void MX_GPIO_Init(void)
{
  GPIO_InitTypeDef GPIO_InitStruct = {0};
  /* USER CODE BEGIN MX_GPIO_Init_1 */

  /* USER CODE END MX_GPIO_Init_1 */

  /* GPIO Ports Clock Enable */
  __HAL_RCC_GPIOC_CLK_ENABLE();
  __HAL_RCC_GPIOA_CLK_ENABLE();
  __HAL_RCC_GPIOB_CLK_ENABLE();
  __HAL_RCC_GPIOD_CLK_ENABLE();

  /*Configure GPIO pin Output Level */
  HAL_GPIO_WritePin(LED_GPIO_Port, LED_Pin, GPIO_PIN_RESET);

  /*Configure GPIO pin : LED_Pin */
  GPIO_InitStruct.Pin = LED_Pin;
  GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
  GPIO_InitStruct.Pull = GPIO_NOPULL;
  GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
  HAL_GPIO_Init(LED_GPIO_Port, &GPIO_InitStruct);

  /* USER CODE BEGIN MX_GPIO_Init_2 */

  /* USER CODE END MX_GPIO_Init_2 */
}

/* USER CODE BEGIN 4 */

/* USER CODE END 4 */

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */
