# TinyPAN Bridge — Status Handoff

## Session 2026-11-18 — reliability audit of GitHub HEAD (cb52be3) + fixes

Audit of the current v0.3-dev (the Sep-1 state: tp_tx task, TX-wedge
self-recovery, adaptive probe threshold, AMPDU heap fix all present).
Respects the ADR: **no throughput surgery** — the wedge is phone-side.
These are correctness/build fixes only.

### Findings (verified, with evidence)

1. **`VFS_WRITE_TIMEOUT=500` never took effect on stock ESP-IDF.**
   `btc_l2cap.c` hard-`#define`s it to `(40 * 1000)` in v5.4, v5.5 AND
   v6.1 (checked all three). The `-D` override in CMakeLists.txt only
   produces `warning: "VFS_WRITE_TIMEOUT" redefined` (captured in a clean
   build) — `write()` still blocks up to **40 s**, not 500 ms. The
   "timeouts fire at the 500 ms cap" observation in the ADR must have
   come from the locally patched Mac IDF, not the repo config.
2. **Keepalive still froze the main loop.** `bnep_transport_process()`
   sent the 2 s BNEP keepalive via blocking `hal_bt_l2cap_send()` from the
   app loop. With finding 1, a congested VFS queue could freeze the main
   loop (hal_bt_poll, watchdog, station polling) for up to 40 s — the
   exact failure mode tp_tx was created to eliminate, still alive via the
   keepalive path.
3. **flush-vs-drain race (use-after-free / double-free).** The drain
   releases `s_bnep_tx_mutex` during `send_iovec()`. On disconnect,
   `flush_tx_queue()` (app task) frees `job->p` and advances head — in
   synchronous-TX mode `in_flight` is false during the write, so nothing
   stopped the flush from freeing the pbuf tp_tx was copying from, then
   the drain's success path double-freed it and skipped a live job.
   Window is huge with 40 s blocks, and the TX-wedge recovery *deliberately
   bounces PAN while tp_tx may be blocked in write()* — so this race was
   reachable in normal self-recovery operation.
4. **Mutex leak (deadlock landmine).** The in-flight TX-timeout path in
   `bnep_transport_drain_tx_queue()` returned without unlocking
   `s_bnep_tx_mutex` → permanent deadlock of output()/drain/flush.
   Dormant with SYNCHRONOUS_TX=1 (in_flight never set) but one refactor
   away. Same bug exists upstream in Akhil-Chaturvedi/TinyPAN.
5. **Repo did not link on a clean ESP-IDF v5.5 build:**
   `region 'iram0_0_seg' overflowed by 1144 bytes` (reproduced on HEAD
   before fixes). New builders could not produce firmware.
6. **DNS forwarder silent-drop + unbounded stale-reply wait.** `sendto`
   failure dropped the query with no response (client waits out its own
   5-10 s resolver timeout), and each discarded stale reply restarted the
   full 1.5 s `recvfrom` wait — a stream of stale replies could stall the
   serialized forwarder well past 1.5 s.

### Fixes applied (this commit)

- `tinypan_bnep_transport.c`: keepalive deferred to tp_tx (`s_keepalive_due`
  + semaphore give) — **no blocking write remains in the app main loop**;
  per-job `writing` flag (flush/GC skip it) + `s_session_gen` check after
  the blocking send reclaims the writing job and everything queued behind
  it (no double-free, no stale-session packets into the next BNEP session);
  mutex unlock on the TX-timeout return path.
- `captive_dns.c`: `dns_send_servfail()` helper; SERVFAIL on sendto failure
  (+ one bounded 20 ms retry on ENOMEM/ENOBUFS); per-query budget
  `DNS_FWD_QUERY_BUDGET_MS=2000` with per-iteration `SO_RCVTIMEO` =
  remaining time (stale replies can no longer extend the wait). Kept the
  1.5 s base timeout, queue depth 32, and ENOMEM log-spam suppression.
- `CMakeLists.txt`: removed the ineffective `VFS_WRITE_TIMEOUT=500`
  define; comment documents why the firmware tolerates 40 s blocks
  (tp_tx confinement + EPIPE wake on close + 15 s wedge detector).
  quota=2 / GAP_DATA_BUF_SIZE=2048 / link-policy overrides unchanged.
- `sdkconfig.defaults`: `CONFIG_ESP_WIFI_IRAM_OPT=n`,
  `CONFIG_ESP_WIFI_RX_IRAM_OPT=n`,
  `CONFIG_FREERTOS_PLACE_FUNCTIONS_INTO_FLASH=y` → IRAM ~71% (was
  overflowing). Trade-off: WiFi code runs from flash; brief WiFi pause
  during NVS/SPIFFS writes (config saves only). BT is the bottleneck,
  not WiFi.
- `README.md`: architecture bullet for tp_tx.

### Deliberately NOT changed

- No TX-pipeline/throughput surgery (ADR: phone-side ceiling ~35%).
- tp_tx priority, quota=2, QoS 0x80, PREFER_WIFI, DNS queue 32,
  resilience STALLED/adaptive thresholds, wedge-recovery timings — all
  hardware-verified tuning, left as-is.
- Candidate for a future pass (not applied): `CONFIG_LWIP_IPV6=y` costs
  heap (ip_addr_t 20+ B → DNS queue ~17 KB) while the bridge is IPv4-NAPT
  only; disabling IPv6 would free heap and stop blackholing client IPv6
  traffic. Needs a hardware re-test before changing.

### Verification

- Clean `idf.py build` (esp32, stock ESP-IDF v5.5 at ~/esp-idf,
  `IDF_PYTHON_ENV_PATH=~/.espressif/python_env/idf5.5_py3.11_env`):
  links, no VFS_WRITE_TIMEOUT redefinition warning, IRAM ~71%,
  app partition ~36% free.
- **No hardware load test** — no ESP32 attached to this Linux host.
  When hardware is available: sustained ping/browsing from a WiFi client;
  expect main-loop heap/txstat logs to keep ticking under load, wedge
  recovery bounces to be clean (no heap corruption / crashes after
  `TX wedge:` bounces), dnsstat timeouts ≤ ~2 s, SERVFAILs instead of
  client-side resolver stalls on sendto errors.
- The Mac's local IDF patches (packet_fragmenter.c assert→drop,
  btc_l2cap.c timeout if patched) are no longer required for correctness
  of the TX path; the fragmenter patch is still recommended for
  iPhone-burst OOM resilience (upstream IDF still asserts).

---

**Date:** 2026-09-01 (supersedes 2026-08-27 session)
**Repo:** `/Users/nuaimat/code/esp_others/tinypan`
**Branch:** `v0.3-dev`

## Session 2026-09-01 — Mac-client heap exhaustion + log-spam cleanup

**The bug (this session's whole point):** when a real Mac joined the AP, it
disconnected after ~30-40 s of use with a wall of fast-scrolling log noise.
Two passes, two distinct defects:

### Pass 1 — over-engineering cleanup (committed `f57e2aa`)

`ponytail-review` of `v0.3-dev` vs `main`, then applied. Net -43 lines, no
behavior change:

- Merged `bnep_transport_log_stats()` + `bnep_transport_get_tx_stats()` into
  one struct accessor declared in `tinypan_internal.h` (was two `extern`-wired
  ad-hoc APIs). Dropped 4 log-only counters (`enq_ok`, `tx_complete`,
  `drain_calls`, `drain_inflight`).
- Deleted the `hal_bt_debug_tx_complete_max_lat_us()` TX-latency diagnostic
  (getter + 2 statics + timestamp reads) — only consumer was a log line.
- `resilience.h`: new tunables to plain `#define` (no override mechanism).
- `dhcp_manager.c`: NVS read straight into the table under lock (dropped the
  lock-free stack-buffer copy — boot-time-only path).
- `captive_dns.c`: dropped the unlogged `queue_full` counter.
- `tinypan.c`: `tinypan_internal_clear_ip()` early-returns when no IP was ever
  assigned — kills the false `[WARN] IP lost` + `IP_LOST` event on every
  initial link-up.
- `main.c`: bonded-device logs fall back to BDA when the NVS name is empty.

### Pass 2 — Mac disconnect (root cause = heap exhaustion)

First log (`/tmp/logs.txt`) showed `DNS fwd queue full` ×422 + `errno=12` ×184
+ `Unknown BNEP packet type` (frame desync) + heap min **5.4 KB**. Initial
diagnosis over-weighted the DNS queue, so `DNS_FWD_QUEUE_LEN` 8→32 and 5
per-frame `[DEBUG]` logs were deleted.

Second log (`/tmp/log2`, after flash) proved the real root cause:

```
10.0s  E BT_OSI: malloc failed (GAP_ConnWriteData size=2048)
10.7s  heap: free=2948  min=964      ← heap exhausted
       E l2cap_vfs_write exit for time out  (43×)
       W DNS sendto failed: errno=12        (199×)
       → Mac's DNS dies → SERVFAIL → Mac gives up
```

The Mac's AMPDU-aggregated traffic (`winSize:64`, `ba-add` frames) drove the
WiFi→BT forwarding path hard enough to exhaust heap. The BT stack couldn't get
a single GAP TX buffer, L2CAP write starved, and the DNS forwarder (which needs
pbufs) collapsed into the ENOMEM storm.

**Fix:** halved the WiFi dynamic buffer counts in `sdkconfig.defaults`
(`CONFIG_ESP_WIFI_DYNAMIC_RX_BUFFER_NUM` 16→8, `..._TX_BUFFER_NUM` 16→8,
`CONFIG_ESP_WIFI_MGMT_SBUF_NUM` 16→8) — frees ~25 KB, the exact margin between
"964 bytes free" and "works". Also silenced the `errno=12` log (kept the
counter) and deleted the `L2CAP can send now` debug line.

**STILL TO VERIFY:** re-flash and re-test with the Mac. Watch `heap: free=..
min=..` during the traffic burst — `min` must now stay ≥~20 KB. If it still
bottoms out, the next lever is `CONFIG_ESP_WIFI_STATIC_RX_BUFFER_NUM` 10→8,
then the L2CAP TX quota.

**Also note:** the `DNS_FWD_QUEUE_LEN` 8→32 change from pass 2 was kept (it is
genuinely useful for burst absorption after the heap fix), but it costs ~16 KB
— re-evaluate if the heap fix alone clears the disconnect.

---

<details><summary><b>Legacy handoff (2026-08-27 and earlier) — kept for history</b></summary>

**Date:** 2026-08-27 (supersedes 2026-08-22/23 sessions)
**Repo:** `/Users/nuaimat/code/esp_others/tinypan` (HEAD `6a539a9`; the TX-task + tuning changes below are uncommitted local changes)
**Hardware:** ESP32-D0WD-V3 rev v3.1 on `/dev/cu.usbserial-0001` (a.k.a. `/dev/cu.SLAB_USBtoUART`), 115200 baud
**ESP-IDF:** v5.4 at `~/esp/esp-idf`
**Build/flash:** `export PATH=$HOME/.espressif/python_env/idf5.4_py3.14_env/bin:$PATH && source ~/esp/esp-idf/export.sh && idf.py build && idf.py -p /dev/cu.usbserial-0001 flash` (must force the py3.14 env — see "Python env gotcha" below)
**WiFi AP:** SSID `TinyPAN-Bridge`, password `bridge1234`, AP IP `192.168.99.1`
**BT phone (NAP):** Pixel 7 Pro `d4:3a:2c:99:f8:18`, bonded in NVS (do NOT erase flash/NVS)

## Status: wedge confirmed PHONE-SIDE (Android BT-PAN itself) + self-recovery added

**2026-08-27 session — final verdict and new findings:**

- **Wedge is Android's BT-PAN forwarding, full stop.** New evidence on top of
  the ADR: wedge reproduces with the phone's uplink on **home WiFi** (not
  cellular — rules out carrier cap / Data Saver), and on the **initial
  release** `b67f6bd` (bisect test: wedged *faster* than HEAD — no firmware
  regression; the "worked for 20 minutes" memory predates the phone ever
  entering its throttled state). The throttle is **sticky**: once triggered it
  can persist across reconnects for minutes.
- **Crash-loop fixed (two causes):**
  1. Bluedroid `packet_fragmenter.c:177 assert(0)` on reassembly OOM under an
     iPhone's join-traffic burst (heap min hit 576 B). **Patched the shared
     IDF** (`~/esp/esp-idf/components/bt/host/bluedroid/hci/packet_fragmenter.c`)
     to drop the packet instead of rebooting. NOTE: local IDF patch — an IDF
     upgrade/reinstall wipes it; reapply if so.
  2. Heap relief: `DNS_FWD_QUEUE_LEN` back to 8 (the 1.5 s timeout already
     fixed the stall; 16-deep cost ~4 KB heap).
- **iPhone "unable to join network" regression fixed:** removing the permanent
  `ESP_COEX_PREFER_WIFI` (done in the 2026-08-23 session) starved EAPOL once
  BT was active — reason=2 deauth bursts. Restored `PREFER_WIFI`; the ADR
  experiment showed it costs nothing for BT throughput.
- **TX wedge self-recovery added** (`main/resilience.c`): if the TX ring
  holds data (`depth>0`) but `sent` stops advancing for
  `RESILIENCE_TX_STALL_MS` (15 s), force a PAN reconnect (non-blocking deferred
  stop/start), rate-limited to one bounce per
  `RESILIENCE_TX_STALL_COOLDOWN_MS` (120 s). Converts "wedged until manual
  phone toggle" into a ~20 s hiccup. Uses new
  `bnep_transport_get_tx_stats()` accessor.

Everything below from the 2026-08-23 ADR still stands; what changed is only
that "next lever is non-firmware" was tested and closed — the lever is
Android itself, which we cannot change. The self-recovery above is the
firmware-side mitigation.

**`docs/2026-08-23_bt-throughput-wedge-is-phone-side.md`** (ADR)

Key points:
- A BTstack PANU spike (different host stack, same controller, zero WiFi/coex)
  wedged identically: ~5 replies at 135 ms, then a hard stop. BTstack dropped
  only 1 packet all run vs Bluedroid's 802 — yet the link still stopped.
- The bridge's own resilience probe logged `BT link ALIVE (cellular dead —
  patient)` during wedged runs — ACL up, keepalives flowing, uplink dead.
- Decision: **no BTstack port** (a rewrite fixes nothing — same controller,
  same phone, same uplink ceiling).
- **Landed and kept:** PREFER_BT experiment (later reverted — see above),
  QoS 0x40, dedicated TX task, DNS forwarder timeout reduction.

## What this device does

ESP32 bridges WiFi SoftAP clients to the internet via Bluetooth Classic PAN
(BNEP) tethering to a phone. Custom lwIP NAPT router. Main components:
- `components/tinypan/` — BNEP/L2CAP PAN client library (lwIP netif `tp2`)
- `components/dhcpserver/` — custom DHCP server for AP clients
- `main/` — bt_manager, wifi_ap, nat_router, captive_dns, resilience, main loop

## Serial capture without a TTY

`idf.py monitor` fails in non-TTY shells. Use pyserial instead:

```bash
python3 -c "
import serial, time
ser = serial.Serial('/dev/cu.usbserial-0001', 115200, timeout=1)
start = time.time()
while time.time() - start < 120:
    data = ser.read(4096)
    if data: print(data.decode('utf-8', errors='replace'), end='', flush=True)
ser.close()"
```

There is also a console REPL on UART0 (commands: `help status ping curl
reconnect dns wifi clients bt keepalive qos`) — you can write commands to the
same serial port.

**Python env gotcha:** `source export.sh` in a fresh shell resolves to the
`idf5.4_py3.10_env`, but this project's `build/` CMake cache was configured
against `idf5.4_py3.14_env`. `idf.py` then hard-fails with *"currently active
… py3.10 … configured with … py3.14 — run fullclean"*. Force py3.14 first
(see Build/flash line above). `fullclean` is NOT needed.

## Load-test protocol (unchanged)

Requires a real WiFi client running `ping 8.8.8.8` for ~60 s. Idle counters
can't trip the TX path, and the REPL `ping`/`curl` only exercise the ESP32's
own stack (tp2 → BT), NOT the WiFi→NAPT→BT forward path. The agent must not
connect its own Mac to the AP (loses internet → loses LLM), so the human runs
the client while the agent captures serial. Watch:
- `main/main.c` main loop: `heap: free=.. min=.. largest=.. txc_lat_max=..us`
- `tinypan_bnep_transport.c` `txstat:` — `enq full sent busy err done drains
  infl_wait wmax depth infl`
- `main/captive_dns.c` `dnsstat:` — `ok/timeout/senderr/qfull/rtt_max`
- `main/resilience.c` — `BT link ALIVE (cellular dead — patient)` is the
  highest-value signal; it points at the phone, not the firmware.

## Issues FIXED so far (verified working)

1. **DHCP double-copy bug** — `components/dhcpserver/dhcpserver.c`
   `handle_dhcp()` copied the first pbuf twice. Fixed with a single
   `pbuf_copy_partial()`. Verified: clients get `192.168.99.2` leases.

2. **WiFi EAPOL 4-way handshake timeout (station `leave, reason=15`)** —
   BT controller stayed enabled while "paused", keeping coex active, starving
   EAPOL. Fix: `bt_manager_suspend()/resume()` + skip
   `wifi_ap_boost_wifi_for_handshake()` when BT is suspended. Verified.

3. **`dns_fwd` task stack overflow crash** — `main/captive_dns.c`
   stack 3072 → 6144. Verified.

4. **Heap exhaustion under WiFi→BT forwarding load** — every L2CAP VFS write
   copied into a 4112-byte heap buffer and bluedroid's congestion quota was 40
   buffers (~164 KB) vs ~35 KB free heap. Fixes: `GAP_DATA_BUF_SIZE=2048`,
   quota reduced, `TINYPAN_TX_QUEUE_LEN=8`, WiFi buffers trimmed in sdkconfig.
   Verified: no more malloc failures, heap ≥28 KB free under load.

5. **Main-loop freeze from blocking `write()`** — `l2cap_vfs_write()` blocks
   up to 40 s when bluedroid's 10-slot VFS TX queue is full, and the drain ran
   in the app main loop. Fix (uncommitted): **dedicated `tp_tx` FreeRTOS
   task** owns `bnep_transport_drain_tx_queue()`, woken via a new
   `hal_semaphore_*` HAL abstraction from `bnep_transport_output()` and the
   CAN_SEND_NOW/TX_COMPLETE paths. The synchronous-TX immediate-free behavior
   is kept; the old `TINYPAN_TX_DRAIN_BATCH_MAX` cap is removed (blocking is
   fine in the task). Task priority is below BTU so it can never preempt the
   task that unblocks `write()`. Load test: no main-loop freeze (the remaining
   loss is phone-side, per the ADR).

6. **OCR code-review sweep** (commit `6a539a9`) — all findings fixed: DHCP
   underflow/truncation, torn 64-bit counters, `wifi_ap`/`resilience` races,
   NAPT endianness, NVS blob length, probe-task watchdog race, and follow-up
   self-review + re-review rounds (H1–H6, M7–M21, L1–L7, F1–F7, R1–R11).
   (The `ocr_*.md` task files were deleted after completion — 2026-08-27.)

## Current uncommitted tuning (on top of `6a539a9`)

All in the working tree, matching the ADR's "landed and kept" list:

- `CMakeLists.txt` — `L2CAP_CBB_DEFAULT_DATA_RATE_BUFF_QUOTA=1 → 2`: quota 1
  wedged the channel into permanent congestion under load (sent froze at 57,
  busy→273, ~0.4 pkt/s); quota 2 ≈ 8 × 2048 B ≈ 16 KB, still heap-safe.
  Also `VFS_WRITE_TIMEOUT=500`: caps the VFS `write()` block at 500 ms
  (default 40 s) so the EBUSY backoff path engages instead of long stalls.
- `components/tinypan/` — dedicated TX task + semaphore HAL (item 5 above);
  QoS poll `TINYPAN_BT_QOS_POLL 0x80 → 0x40` (more TX slots for the slave
  under coex).
- `main/wifi_ap.c` — dropped the permanent `ESP_COEX_PREFER_WIFI` (default
  PREFER_BT kept); QoS-restore `0x80 → 0x40` to match connect-time value.
  NOTE: coex preference was tested both ways — no effect on the wedge.
- `main/captive_dns.c` — fwd `SO_RCVTIMEO` 5 s → 1.5 s (serialized forwarder;
  bounds worst-case queue stall), `DNS_FWD_QUEUE_LEN` 8 → 16.

## Known limitations / non-goals

- Sustained-throughput ceiling (~35% under load) is phone/carrier-side — do
  NOT attempt host-stack ports or further TX-pipeline surgery unless a phone
  re-test (Data Saver / Battery Saver off) changes the number.
- The DNS forwarder (`captive_dns.c` `fwd_task`) is still fully serialized;
  acceptable now that the wedge is understood to be phone-side.
- `resilience.c` probes upstream every 20 s; at 3 failures it aggressively
  reconnects BT. Under a phone-side throttle this can cause reconnect thrash —
  logs showing `BT link DEAD (reconnecting aggressively)` during load tests
  are expected and not a firmware bug. (The `cellular dead — patient`
  classification already mitigates this.)
- BT on-demand mode: BT disconnects 10 s after last WiFi station leaves,
  reconnects when a station joins (3 s delayed). BT controller is fully
  suspended (coex=0) while no stations — this is what makes EAPOL reliable.
- ESP-IDF L2CAP VFS internals (from `btc_l2cap.c`): `write()` memcpy's into a
  fresh buffer (pbuf freeable on success), queues into a 10-slot fixed_queue,
  wakes early below low-watermark 4, and does NOT honour `O_NONBLOCK`.
  With `VFS_WRITE_TIMEOUT=500` the worst block is now 500 ms, not 40 s.

## Verify compile-time overrides took effect

`grep -c "GAP_DATA_BUF_SIZE=2048" build/compile_commands.json` (expect 304);
same for `L2CAP_CBB_DEFAULT_DATA_RATE_BUFF_QUOTA=2` and `VFS_WRITE_TIMEOUT=500`.

## Next steps

1. **Field-test the wedge self-recovery:** sustained iPhone browsing; expect
   `TX wedge: sent frozen ...` → PAN reconnect → browsing resumes after ~20 s.
   If the phone's throttle is sticky across bounces, expect a hiccup every
   ~2-3 min (cooldown-limited) instead of permanent death.
2. **Commit** everything (wedge recovery + this file).
3. Only re-open throughput work if the phone/OS changes (different phone,
   Android update, different tethering app).

</details>
