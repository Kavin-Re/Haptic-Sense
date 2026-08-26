# M-3 Edit Spec — MPU6050 Option B EXTI: `tk_wup_tsk` → `tk_sig_sem`

**Target file:** `docs/design/mpu6050_port_design_v1.md` (on-disk; not present in this session — Claude Code applies below verbatim-in-spirit, adapting only to the doc's existing section numbering/formatting).
**Decision closed:** M-3, locked 2026-07-11. Rationale: count-carrying semantics (wakes issued while P3 is mid-frame are not lost) + one universal ISR-wake mechanism across all three sensors, matching the VL53L1X production EXTI path and the paired-semaphore architecture. Scope of this edit is **narrow**: swap the wake mechanism only; the handler stays otherwise identical (kernel-legal level, no I2C, no printf).

> **Recovery note (2026-08-26):** produced 2026-07-19, never saved to disk. Restored verbatim from the chat transcript of that session.

---

## 1. Replacement design content (Option B EXTI section)

### 1a. Semaphore object (add to the doc's kernel-objects table)

| Object | Type | Init | Max | Producer | Consumer |
|---|---|---|---|---|---|
| `imu_drdy_sem` | `tk_cre_sem` | isemcnt = 0 | maxsem = 2 | EXTI ISR (PE9, task-independent portion) | Priority 3 Sensor Acquisition task |

**maxsem = 2 rationale [decision, reversible]:** MPU6050 data registers overwrite in place, so a pile-up beyond ~2 pending wakes carries no additional data — P3 would only re-read the same latest sample. A small cap bounds the drain loop after any P3 stall; overflow (`E_QOVR`) is expected under overload and is **silently discarded** in the ISR (optionally counted under `#ifdef DEBUG_TIMING`). Losing the *count* above 2 is harmless; losing the *wake* entirely (the `tk_wup_tsk`-without-queuing hazard M-3 guarded against) is not, and cannot happen here.

### 1b. ISR (replaces the `tk_wup_tsk` handler block)

```c
/* EXTI callback for IMU_INT — PE9 (ARD_D3), rising edge.
   TASK-INDEPENDENT PORTION (interrupt context). NO I2C. NO printf.
   Producer: this ISR. Consumer: Priority 3 sensor task via imu_drdy_sem. */
void HAL_GPIO_EXTI_Rising_Callback(uint16_t GPIO_Pin)   /* or the BSP2 ISR shim
                                                           the doc already names */
{
    if (GPIO_Pin == IMU_INT_PIN) {
        (void)tk_sig_sem(imu_drdy_sem, 1);
        /* E_QOVR possible at maxsem=2 under P3 overload — intentionally ignored;
           data regs overwrite, extra wakes add nothing. See §1a. */
    }
    /* VL53L1X GPIO1 (PD0) branch unchanged — already tk_sig_sem. */
}
```

### 1c. Consumer side (replaces the `tk_slp_tsk` wait in the sensor-task row)

```c
/* ONLY IN PRIORITY 3 SENSOR TASK.
   Timeout retained: expiry = data-ready starvation → staleness path
   (12-feature fallback), same semantics the Option A design gave to "INT low". */
ER er = tk_wai_sem(imu_drdy_sem, 1, IMU_DRDY_TMO);   /* TMO ≈ 2× sample period,
                                                        e.g. 40 ms @ 50 Hz */
if (er == E_TMOUT) {
    /* mark IMU frame stale — transport per M-4 constraint:
       validity field INSIDE the semaphore-protected P3→P2 frame buffer,
       never a bare cross-task flag. */
} else if (er == E_OK) {
    /* i2c_rd burst of accel/gyro regs via DMA — unchanged from existing doc */
}
```

### 1d. Init-ordering rule (add one row/note to the init sequence)

`tk_cre_sem(imu_drdy_sem)` **must complete before** the EXTI line is enabled in NVIC (enable interrupt last in init). A fired EXTI signalling a nonexistent semaphore ID is an `E_ID` in interrupt context — a silent lost wake at best. Same ordering the VL53L1X doc imposes; state it explicitly here too.

## 2. CLAUDE.md edit (§3, ABSOLUTE RULES block — add one line)

> - All sensor data-ready ISR→P3 wakes use `tk_sig_sem` (count-carrying), NEVER `tk_wup_tsk` — decided 2026-07-11, audit M-3. Producer/consumer must be named at every signal/wait site.

Also remove/annotate `tk_wup_tsk` in §3's approved-API list if it appears with no remaining legitimate use (it stays legal for non-ISR task-to-task nudges if any exist; if none, mark "reserved — not used in this project").

## 3. Claude Code handoff

1. Check `git log`/doc contents first: the 2026-07-11 session drafted a combined M-2/M-3/M-4 prompt — **verify whether M-2 (bounded DEVICE_RESET poll, ≤5 attempts + `tk_dly_tsk(1)`) and M-4 (staleness-in-frame-buffer constraint note) already landed**. Apply whichever of the three are still missing; this spec supersedes the M-3 portion of that prompt with concrete text.
2. Apply §1 into `mpu6050_port_design_v1.md`, §2 into `CLAUDE.md`. Show diff before committing.
3. Commit (adapt if M-2/M-4 already closed):
   `docs: close audit M-3 (Option B EXTI tk_wup_tsk → tk_sig_sem, maxsem=2)`
4. Standing rule applies even to doc commits touching CLAUDE.md: rebuild is not needed (no firmware change), but note in CLAUDE.md changelog that Phase 5 IMU bring-up code must be written against this version of the doc.

## 4. New ledger item

| ID | Test | Pass criterion |
|---|---|---|
| IMU-M3-V | Bench: hold P3 busy (debug spin) across ≥3 IMU DRDY edges, then release | P3 drains exactly min(pending, 2) wakes; no lost wake; no E_ID; staleness path NOT triggered |
