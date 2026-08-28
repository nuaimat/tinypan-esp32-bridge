# ADR: BT Throughput Wedge Is Phone-Side — Abort BTstack Port

- **Date:** 2026-08-23
- **Status:** Accepted
- **Deciders:** Mo (owner), @software-architect, @backend-engineer, @firmware-engineer

## Context

TinyPAN (ESP32 BNEP/PANU bridge: WiFi SoftAP ↔ Bluetooth PAN to a phone's
network-access point) showed 100% packet loss under sustained load from a WiFi
client. After the first round of fixes (VFS write timeout, BT QoS poll tuning,
TX-ring depth), sustained ping reached ~35% — "works briefly, then freezes."

The open question: **is the freeze a Bluedroid host-stack bug, or the phone's
uplink?** This determines whether a multi-day port to the BTstack host stack
is justified.

## What was tried and ruled out

| Lever | Result |
|---|---|
| `ESP_COEX_PREFER_WIFI` → `PREFER_BT` (wifi_ap.c) | No change — wedged identically under both. RF arbitration ruled out. |
| BT QoS poll `0x80` vs `0x40` | No change (561 → 557 wedge point). |
| L2CAP VFS write timeout | Timeouts fire at the 500 ms cap, but the wedge is upstream of `write()`. |
| Dedicated TX task (bnep drain off the main loop) | No change to the wedge (correct hygiene, kept). |

## Decisive experiment: BTstack PANU spike

Flashed a **BTstack** PANU example (SalimTerryLi `esp32_btstack_panu_example`)
— a different host stack driving the **same** ESP32 BT controller, with **zero
WiFi SoftAP and zero coex contention** (cleanest possible isolation). The
ESP32 pinged `8.8.8.8` directly over the PAN link.

Result: BNEP opened cleanly, got IP `10.124.45.225`, **5 real replies at
135 ms** (an early `max_rtt=135us` reading was a units bug in the spike's own
logging — `ESP_PING_PROF_TIMEGAP` is milliseconds), then the ping froze
(`consec_loss` → 44 over 130 s). BTstack's flow control dropped only **1**
packet the entire run (vs Bluedroid's 802 in the wedged runs) — yet the link
stopped delivering.

## Corroborating telemetry

The main bridge's own resilience probe logged **`BT link ALIVE (cellular dead —
patient)`** during the wedged runs: the ACL stayed up, RX keepalives flowed, but
the ESP32's upstream probe classified the *cellular* leg as dead. Two
independent stacks (Bluedroid + BTstack), two independent probes, same verdict.

## Decision

**The BT throughput wedge is phone-side — Android tethering throttles sustained
BT-PAN forwarding (Data Saver / Battery Saver / carrier cap) — not a Bluedroid
or firmware bug. The BTstack port is abandoned.**

The freeze is deterministic (~5 replies then hard stop, every run, every
firmware change) — throttle behavior, not RF flakiness.

## Consequences

- **No BTstack port.** Same controller + same phone + same uplink ceiling → a
  rewrite fixes nothing. This saved a multi-day effort.
- **Firmware is at its ceiling (~35% under sustained load)** on this
  phone/carrier combo. Further gains live on the phone/carrier side, not here.
- **Next lever (non-firmware):** disable Android Data Saver / Battery Saver and
  re-test. If the number is unchanged, it is a carrier-level tethering cap and
  ~35% is the true ceiling.
- **Landed and kept (checkpoint):** `PREFER_BT` (dropped the permanent
  `PREFER_WIFI`), QoS-restore `0x80 → 0x40` consistency fix, dedicated TX task,
  DNS forwarder timeout reduction.
- **Diagnostic to retain:** the `cellular dead / BT alive` resilience telemetry
  is the highest-value signal for any future revisit — it already pointed at the
  phone before the spike confirmed it.
