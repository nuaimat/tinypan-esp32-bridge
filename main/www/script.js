/* ═══════════════════════════════════════════════════════════════════════
   TinyPAN Bridge — Web UI Client Script
   Polls /api/status, drives Bluetooth scan/connect/disconnect,
   and saves WiFi settings.
   ═══════════════════════════════════════════════════════════════════════ */

(function () {
  "use strict";

  /* ── DOM refs ───────────────────────────────────────────────────── */

  /* header */
  var headerDot = document.getElementById("header-dot");
  /* status panel */
  var btStatusEl   = document.getElementById("bt-status");
  var panStatusEl  = document.getElementById("pan-status");
  var apIpEl       = document.getElementById("ap-ip");
  var clientsEl    = document.getElementById("wifi-clients");
  var uptimeEl     = document.getElementById("uptime");

  /* bt panel */
  var btConnectedInfo    = document.getElementById("bt-connected-info");
  var btDisconnectedActs = document.getElementById("bt-disconnected-actions");
  var btPeerNameEl       = document.getElementById("bt-peer-name");
  var btPeerBdaEl        = document.getElementById("bt-peer-bda");
  var btScanBtn          = document.getElementById("bt-scan-btn");
  var btScanStatus       = document.getElementById("bt-scan-status");
  var btDeviceList       = document.getElementById("bt-device-list");
  var btDisconnectBtn    = document.getElementById("bt-disconnect-btn");

  /* wifi panel */
  var wifiForm      = document.getElementById("wifi-form");
  var wifiSsid      = document.getElementById("wifi-ssid");
  var wifiPassword  = document.getElementById("wifi-password");
  var wifiChannel   = document.getElementById("wifi-channel");
  var wifiSaveStatus = document.getElementById("wifi-save-status");
  var togglePwBtn   = document.getElementById("toggle-password");

  /* ── State ──────────────────────────────────────────────────────── */

  var btConnected = false;   /* cached from last /api/status poll */
  var btPollTimer  = null;  /* interval ID for BT status poll */

  /* ── Header dot ─────────────────────────────────────────────────── */

  function setDot(state) {
    headerDot.className = "dot " + state;   /* on | off | unknown */
  }

  /* ── Status poll ────────────────────────────────────────────────── */

  function fmtUptime(sec) {
    var h = Math.floor(sec / 3600);
    var m = Math.floor((sec % 3600) / 60);
    var s = sec % 60;
    if (h > 0) return h + "h " + m + "m " + s + "s";
    return m + "m " + s + "s";
  }

  function refreshStatus() {
    fetch("/api/status")
      .then(function (r) { return r.json(); })
      .then(function (d) {
        /* header dot */
        setDot(d.bt_connected ? "on" : "off");

        /* status panel */
        btStatusEl.textContent  = d.bt_connected ? "Connected" : "Disconnected";
        btStatusEl.className    = "value " + (d.bt_connected ? "online" : "offline");
        panStatusEl.textContent = "—";
        apIpEl.textContent      = d.ip || "—";
        clientsEl.textContent   = (d.wifi_clients != null) ? d.wifi_clients : "—";
        uptimeEl.textContent    = fmtUptime(d.uptime || 0);

        /* bt connected state change */
        if (d.bt_connected !== btConnected) {
          btConnected = d.bt_connected;
          if (btConnected) {
            btConnectedInfo.classList.remove("hidden");
            btDisconnectedActs.classList.add("hidden");
            pollBTStatus();  /* immediate first fetch */
            startBTPoll();
          } else {
            btConnectedInfo.classList.add("hidden");
            btDisconnectedActs.classList.remove("hidden");
            stopBTPoll();
            btPeerNameEl.textContent = "";
            btPeerBdaEl.textContent  = "";
            panStatusEl.textContent  = "Offline";
            panStatusEl.className    = "value offline";
          }
        }
      })
      .catch(function () {
        setDot("unknown");
        /* server may be restarting; degrade silently */
      });
  }

  /* ── BT status poll ─────────────────────────────────────────────── */

  function pollBTStatus() {
    fetch("/api/bt/status")
      .then(function (r) { return r.json(); })
      .then(function (bs) {
        btPeerNameEl.textContent = bs.peer_name || bs.peer_bda || "—";
        btPeerBdaEl.textContent  = bs.peer_bda || "—";
        panStatusEl.textContent  = bs.pan_online ? "Online" : "Offline";
        panStatusEl.className    = "value " + (bs.pan_online ? "online" : "offline");
      })
      .catch(function () {
        /* silent — will retry on next tick */
      });
  }

  function startBTPoll() {
    if (btPollTimer === null) {
      btPollTimer = setInterval(pollBTStatus, 5000);
    }
  }

  function stopBTPoll() {
    if (btPollTimer !== null) {
      clearInterval(btPollTimer);
      btPollTimer = null;
    }
  }

  /* ── BT scan ────────────────────────────────────────────────────── */

  var scanPollTimer  = null;  /* interval ID for scan results poll */
  var scanPollCount  = 0;     /* iteration counter for timeout */
  var SCAN_MAX_POLLS = 60;    /* poll every 1s: 10s inquiry + name lookups */

  function showScanStatus(msg, cls) {
    btScanStatus.innerHTML = msg;
    btScanStatus.className = "scan-status " + (cls || "");
    btScanStatus.classList.remove("hidden");
  }

  function hideScanStatus() {
    btScanStatus.classList.add("hidden");
  }

  function stopScanPoll() {
    if (scanPollTimer !== null) {
      clearInterval(scanPollTimer);
      scanPollTimer = null;
    }
  }

  function renderDeviceList(devices) {
    if (!devices || devices.length === 0) {
      hideScanStatus();  /* clear spinner first */
      showScanStatus("No devices found.", "error");
      return;
    }

    hideScanStatus();
    btDeviceList.innerHTML = "";
    btDeviceList.classList.remove("hidden");

    devices.forEach(function (dev) {
      var li = document.createElement("li");
      li.className = "device-item";

      var rssiClass = "";
      if (dev.rssi >= -50) rssiClass = "online";
      else if (dev.rssi <= -80) rssiClass = "offline";

      li.innerHTML =
        '<div class="device-info">' +
          '<div class="device-name">' + escHtml(dev.name || "Unknown") + '</div>' +
          '<div class="device-meta">' + escHtml(dev.bda) + '  •  CoD: 0x' + (dev.cod || 0).toString(16) + '</div>' +
        '</div>' +
        '<span class="device-rssi ' + rssiClass + '">' + (dev.rssi || 0) + ' dBm</span>' +
        '<button class="device-connect-btn" data-bda="' + escAttr(dev.bda) + '">Connect</button>';

      btDeviceList.appendChild(li);
    });

    /* wire connect buttons */
    btDeviceList.querySelectorAll(".device-connect-btn").forEach(function (btn) {
      btn.addEventListener("click", function () {
        var bda = btn.getAttribute("data-bda");
        btn.disabled = true;
        btn.textContent = "…";

        fetch("/api/bt/connect", {
          method: "POST",
          headers: { "Content-Type": "application/json" },
          body: JSON.stringify({ bda: bda }),
        })
          .then(function (r) { return r.json(); })
          .then(function (res) {
            if (res.ok) {
              showScanStatus("Connecting… refresh in a moment");
              /* status poll will pick up the change */
            } else {
              showScanStatus("Connect failed: " + (res.error || "unknown"), "error");
              btn.disabled = false;
              btn.textContent = "Connect";
            }
          })
          .catch(function () {
            showScanStatus("Connect failed (network error)", "error");
            btn.disabled = false;
            btn.textContent = "Connect";
          });
      });
    });
  }

  btScanBtn.addEventListener("click", function () {
    btScanBtn.disabled = true;
    showScanStatus("Starting scan…");
    btDeviceList.innerHTML = "";
    btDeviceList.classList.add("hidden");
    stopScanPoll();

    /* Step 1: trigger scan (non-blocking, returns 202) */
    fetch("/api/bt/scan")
      .then(function (r) {
        /* Check for error responses before parsing JSON */
        if (!r.ok && r.status !== 202) {
          return r.json().then(function (err) {
            throw new Error(err.error || "Scan failed (HTTP " + r.status + ")");
          }).catch(function () {
            throw new Error("Scan failed (HTTP " + r.status + ")");
          });
        }
        return r.json();
      })
      .then(function (d) {
        if (!d.ok) {
          showScanStatus("Scan error: " + (d.error || "unknown"), "error");
          btScanBtn.disabled = false;
          return;
        }

        /* Step 2: scan started — poll with spinner + progress dots */
        showScanStatus('<span class="spinner"></span> Scanning', "scanning");
        scanPollCount = 0;

        scanPollTimer = setInterval(function () {
          scanPollCount++;

          /* Update progress dots */
          var dots = "";
          for (var i = 0; i < (scanPollCount % 4); i++) dots += ".";
          if (dots === "") dots = "...";
          btScanStatus.innerHTML = '<span class="spinner"></span> Scanning' + dots;

          fetch("/api/bt/scan/results")
            .then(function (r) { return r.json(); })
            .then(function (res) {
              if (!res.ok) {
                stopScanPoll();
                showScanStatus("Scan error: " + (res.error || "unknown"), "error");
                btScanBtn.disabled = false;
                return;
              }

              if (res.completed) {
                stopScanPoll();
                renderDeviceList(res.devices);
                btScanBtn.disabled = false;
                return;
              }

              /* Timeout check — must be AFTER completed check */
              if (scanPollCount >= SCAN_MAX_POLLS) {
                stopScanPoll();
                showScanStatus("Scan timed out. Try again.", "error");
                btScanBtn.disabled = false;
              }
            })
            .catch(function () {
              stopScanPoll();
              showScanStatus("Scan failed — server may be busy.", "error");
              btScanBtn.disabled = false;
            });
        }, 1000);
      })
      .catch(function (e) {
        showScanStatus(e.message || "Scan failed — server may be busy.", "error");
        btScanBtn.disabled = false;
      });
  });

  /* ── BT disconnect ──────────────────────────────────────────────── */

  btDisconnectBtn.addEventListener("click", function () {
    btDisconnectBtn.disabled = true;
    btDisconnectBtn.textContent = "Disconnecting…";

    fetch("/api/bt/disconnect", { method: "POST" })
      .then(function (r) { return r.json(); })
      .then(function (res) {
        if (res.ok) {
          stopBTPoll();
          btConnectedInfo.classList.add("hidden");
          btDisconnectedActs.classList.remove("hidden");
          btConnected = false;
          setDot("off");
          btStatusEl.textContent = "Disconnected";
          btStatusEl.className = "value offline";
          panStatusEl.textContent = "Offline";
          panStatusEl.className = "value offline";
        }
        btDisconnectBtn.disabled = false;
        btDisconnectBtn.textContent = "Disconnect";
      })
      .catch(function () {
        btDisconnectBtn.disabled = false;
        btDisconnectBtn.textContent = "Disconnect";
      });
  });

  /* ── WiFi settings ──────────────────────────────────────────────── */

  /* show / hide password toggle */
  togglePwBtn.addEventListener("click", function () {
    if (wifiPassword.type === "password") {
      wifiPassword.type = "text";
      togglePwBtn.textContent = "🙈";
    } else {
      wifiPassword.type = "password";
      togglePwBtn.textContent = "👁";
    }
  });

  function flashSaveStatus(cls, msg) {
    wifiSaveStatus.textContent = msg;
    wifiSaveStatus.className = "save-status " + cls;
    wifiSaveStatus.classList.remove("hidden");
    setTimeout(function () {
      wifiSaveStatus.classList.add("hidden");
    }, 4000);
  }

  /* load current config */
  function loadWifiConfig() {
    fetch("/api/wifi")
      .then(function (r) { return r.json(); })
      .then(function (cfg) {
        wifiSsid.value     = cfg.ssid || "";
        wifiPassword.value = cfg.password || "";
        wifiChannel.value  = cfg.channel || 1;
      })
      .catch(function () {
        /* leave defaults */
      });
  }

  wifiForm.addEventListener("submit", function (e) {
    e.preventDefault();

    var ssid    = wifiSsid.value.trim();
    var pass    = wifiPassword.value;
    var channel = parseInt(wifiChannel.value, 10);

    if (!ssid) {
      flashSaveStatus("error", "SSID is required.");
      return;
    }
    if (isNaN(channel) || channel < 1 || channel > 13) {
      flashSaveStatus("error", "Channel must be 1–13.");
      return;
    }

    var payload = { ssid: ssid, password: pass, channel: channel };
    var submitBtn = wifiForm.querySelector('button[type="submit"]');
    submitBtn.disabled = true;
    submitBtn.textContent = "Saving…";

    fetch("/api/wifi", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(payload),
    })
      .then(function (r) { return r.json(); })
      .then(function (res) {
        if (res.ok) {
          flashSaveStatus("success", "Saved. Reboot to apply new SSID/password.");
        } else {
          flashSaveStatus("error", "Save failed: " + (res.error || "unknown"));
        }
        submitBtn.disabled = false;
        submitBtn.textContent = "Save Settings";
      })
      .catch(function () {
        flashSaveStatus("error", "Save failed (network error).");
        submitBtn.disabled = false;
        submitBtn.textContent = "Save Settings";
      });
  });

  /* ── Helpers ────────────────────────────────────────────────────── */

  function escHtml(s) {
    if (!s) return "";
    return s.replace(/&/g, "&amp;")
            .replace(/</g, "&lt;")
            .replace(/>/g, "&gt;")
            .replace(/"/g, "&quot;");
  }

  function escAttr(s) {
    if (!s) return "";
    return s.replace(/&/g, "&amp;")
            .replace(/"/g, "&quot;");
  }

  /* ── Init ───────────────────────────────────────────────────────── */

  loadWifiConfig();
  refreshStatus();
  setInterval(refreshStatus, 3000);
})();
