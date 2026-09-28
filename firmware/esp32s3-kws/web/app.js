const NODE_IDS = ["zlmdesign"];
const NODE_LABELS = {
  zlmdesign: "ESP32-S3 · 气体传感器节点"
};
const MQ4_ALERT_THRESHOLD = 1500;
const MQ7_ALERT_THRESHOLD = 1500;
const POLL_INTERVAL_MS = 5000;

const state = {
  nodes: {},          // { deviceName: { mq4, mq7, inAlert, message, severity, ts } }
  activeAlerts: [],
  alertHistory: [],
  chartMQ4: null,
  chartMQ7: null,
  chartDist: null,
  chartHazard: null,
  chartNodes: null,
  mq4History: {},     // kept for chart structure compatibility
  mq7History: {},
  leafletMap: null,
  leafletMarkers: {},
  soundEnabled: true,
  notifEnabled: false,
  selectedAlertId: null
};

NODE_IDS.forEach(id => {
  state.mq4History[id] = [];
  state.mq7History[id] = [];
});

function initNav() {
  const navItems = document.querySelectorAll(".nav-item[data-view]");
  const panels   = document.querySelectorAll(".view-panel");
  const titles   = {
    overview:  ["总览", "矿井安全实时监测数据"],
    alerts:    ["警报", "当前危险状态与历史记录"],
    map:       ["矿井地图", "设备节点位置与运行状态"],
    analytics: ["数据分析", "传感器统计与趋势分析"],
    settings:  ["设置", "系统配置与警报阈值"]
  };

  navItems.forEach(btn => {
    btn.addEventListener("click", () => {
      const view = btn.dataset.view;

      navItems.forEach(b => b.classList.remove("active"));
      btn.classList.add("active");

      panels.forEach(p => p.classList.remove("active"));
      const target = document.getElementById("view-" + view);
      if (target) target.classList.add("active");

      const [title, sub] = titles[view] || ["矿安脉搏", ""];
      document.getElementById("pageTitle").textContent    = title;
      document.getElementById("pageSubtitle").textContent = sub;

      if (view === "map" && !state.leafletMap) initMap();
      if (view === "analytics") setTimeout(redrawAnalyticsCharts, 100);
    });
  });
}

function initClock() {
  function tick() {
    const now = new Date();
    document.getElementById("clockDisplay").textContent =
      now.toLocaleTimeString("en-US", { hour12: false });
  }
  tick();
  setInterval(tick, 1000);
}

async function pollOneNet() {
  try {
    const response = await fetch("/api/sensors", { cache: "no-store" });
    if (!response.ok) throw new Error(`HTTP ${response.status}`);
    const data = await response.json();
    const nodeId = data.deviceName || NODE_IDS[0];
    const prev = state.nodes[nodeId];
    const mq4 = Number(data.mq4);
    const mq7 = Number(data.mq7);
    const mq4Alert = mq4 >= MQ4_ALERT_THRESHOLD;
    const mq7Alert = mq7 >= MQ7_ALERT_THRESHOLD;
    const inAlert = mq4Alert || mq7Alert;
    const message = inAlert
      ? [mq4Alert ? `MQ-4 数值过高：${mq4}` : "", mq7Alert ? `MQ-7 数值过高：${mq7}` : ""].filter(Boolean).join(" · ")
      : `MQ-4 ${mq4} · MQ-7 ${mq7}`;

    state.nodes[nodeId] = {
      id: nodeId, mq4, mq7, inAlert,
      title: inAlert ? "气体传感器警报" : "传感器正常",
      message,
      severity: inAlert ? "critical" : "info",
      ts: data.updatedAt || Date.now()
    };
    state.mq4History[nodeId] ??= [];
    state.mq7History[nodeId] ??= [];
    state.mq4History[nodeId].push({ value: mq4, ts: Date.now() });
    state.mq7History[nodeId].push({ value: mq7, ts: Date.now() });
    state.mq4History[nodeId] = state.mq4History[nodeId].slice(-30);
    state.mq7History[nodeId] = state.mq7History[nodeId].slice(-30);

    if (inAlert && (!prev || !prev.inAlert)) handleNewAlert(nodeId, state.nodes[nodeId]);
    if (!inAlert && prev?.inAlert) handleAlertCleared(nodeId);
    renderOverview();
    renderActiveAlerts();
    renderSensorTable();
    updateMapMarker(nodeId);
    setConnectionStatus(true);
  } catch (error) {
    console.error("传感器数据读取失败：", error);
    setConnectionStatus(false);
  }
}

function listenOneNet() {
  pollOneNet();
  setInterval(pollOneNet, POLL_INTERVAL_MS);
}

function handleNewAlert(nodeId, data) {
  const alertObj = {
    id:       `${nodeId}-${Date.now()}`,
    nodeId,
    title:    data.title,
    message:  data.message,
    severity: data.severity,
    ts:       Date.now(),
    active:   true,
    label:    NODE_LABELS[nodeId] || nodeId
  };

  state.activeAlerts.unshift(alertObj);

  state.alertHistory.unshift({ ...alertObj, timestamp: new Date() });
  state.alertHistory = state.alertHistory.slice(0, 50);
  renderAlertHistory();
  updateAnalyticsCounters();

  showToast(
    `🚨 ${data.title}`,
    `${NODE_LABELS[nodeId]}: ${data.message}`,
    data.severity === "critical" ? "danger" : "warning"
  );
  triggerBrowserNotification(nodeId, data.title, data.message);
  if (state.soundEnabled) playAlertSound();

  renderActiveAlerts();
  renderOverview();
}

function handleAlertCleared(nodeId) {
  state.activeAlerts = state.activeAlerts.filter(a => a.nodeId !== nodeId);
  showToast("✅ 警报已解除", `${NODE_LABELS[nodeId]} 已恢复安全状态。`, "info");
  renderActiveAlerts();
  renderOverview();
}

function sevClass(severity) {
  if (severity === "critical") return "critical";
  if (severity === "warning")  return "warning";
  return "info";
}

function sevIcon(severity) {
  if (severity === "critical") return "🔴";
  if (severity === "warning")  return "🟡";
  return "🔵";
}

function severityLabel(severity) {
  if (severity === "critical") return "严重";
  if (severity === "warning") return "预警";
  return "信息";
}

function renderOverview() {
  const nodes      = Object.values(state.nodes);
  const alertNodes = nodes.filter(n => n.inAlert);
  const safeNodes  = nodes.filter(n => !n.inAlert);

  setText("metricAlerts", alertNodes.length || "0");
  setText("metricSafe",   safeNodes.length  || "0");
  setText("metricOnline", nodes.length      || "0");

  const now = new Date();
  setText("metricTime", now.toLocaleTimeString("en-US", { hour12: false }));
  setText("metricDate", now.toLocaleDateString("zh-CN", { year:"numeric", month:"2-digit", day:"2-digit" }));

  // Status banner
  const banner      = document.getElementById("statusBanner");
  const bannerTitle = document.getElementById("bannerTitle");
  const bannerSub   = document.getElementById("bannerSub");

  if (alertNodes.length > 0) {
    banner.className        = "status-banner alert";
    bannerTitle.textContent = `⚠️ 当前有 ${alertNodes.length} 条警报—请立即处理`;
    bannerSub.textContent   = alertNodes.map(n =>
      `${NODE_LABELS[n.id] || n.id}: ${n.message}`
    ).join(" | ");

    let pulse = banner.querySelector(".banner-pulse");
    if (!pulse) {
      pulse = document.createElement("div");
      pulse.className = "banner-pulse";
      banner.appendChild(pulse);
    }
  } else {
    banner.className        = "status-banner safe";
    bannerTitle.textContent = "系统运行稳定";
    bannerSub.textContent   = "当前未检测到危险，请继续常规监测。";
    const pulse = banner.querySelector(".banner-pulse");
    if (pulse) pulse.remove();
  }

  // Nav badge
  const badge = document.getElementById("navAlertBadge");
  if (alertNodes.length > 0) {
    badge.style.display = "";
    badge.textContent   = alertNodes.length;
  } else {
    badge.style.display = "none";
  }

  renderNodeCards();
  renderTrendCharts();
}

function renderNodeCards() {
  const grid = document.getElementById("nodesGrid");
  if (!grid) return;

  if (Object.keys(state.nodes).length === 0) {
    grid.innerHTML = `<div class="node-card"><div class="empty-state"><div class="icon">📡</div>正在连接节点…</div></div>`;
    return;
  }

  grid.innerHTML = NODE_IDS.map(id => {
    const n = state.nodes[id];
    return n ? nodeCard(id, n) : nodeOfflineCard(id);
  }).join("");
}

function nodeCard(id, n) {
  const sev     = n.severity || "info";
  const inAlert = n.inAlert;
  const ts      = new Date(n.ts).toLocaleTimeString("en-US", { hour12: false });

  const statusPill = inAlert
    ? `<span class="pill pill-danger">⚠ 警报</span>`
    : `<span class="pill pill-success">✓ 安全</span>`;

  const sevColor = sev === "critical" ? "var(--danger)"
                 : sev === "warning"  ? "var(--warning)"
                 : "var(--accent)";

  const msgIcon = sev === "critical" ? "🔴" : sev === "warning" ? "🟡" : "🟢";

  return `
  <div class="node-card ${inAlert ? "in-alert" : ""}">
    <div class="node-header">
      <div>
        <div class="node-title">${NODE_LABELS[id]}</div>
        <div class="node-id">${id.toUpperCase()}</div>
      </div>
      <div class="node-badges">${statusPill}</div>
    </div>

    <div class="sensors">
      <div class="sensor-row">
        <div class="sensor-top">
          <div class="sensor-name">
            <div class="sensor-dot" style="background:var(--warning)"></div>
            MQ-4 · 甲烷原始值
          </div>
          <div class="sensor-val" style="color:${n.mq4 >= MQ4_ALERT_THRESHOLD ? "var(--danger)" : "var(--success)"}">
            ${n.mq4 ?? "—"}
          </div>
        </div>
        <div class="sensor-bar-bg">
          <div class="sensor-bar-fill" style="width:${Math.min(100, ((n.mq4 || 0) / 4095) * 100)}%;background:var(--warning)"></div>
        </div>
      </div>

      <div class="sensor-row">
        <div class="sensor-top">
          <div class="sensor-name">
            <div class="sensor-dot" style="background:var(--accent)"></div>
            MQ-7 · 一氧化碳原始值
          </div>
          <div class="sensor-val" style="color:${n.mq7 >= MQ7_ALERT_THRESHOLD ? "var(--danger)" : "var(--success)"}">
            ${n.mq7 ?? "—"}
          </div>
        </div>
        <div class="sensor-bar-bg">
          <div class="sensor-bar-fill" style="width:${Math.min(100, ((n.mq7 || 0) / 4095) * 100)}%;background:var(--accent)"></div>
        </div>
      </div>

      <div class="sensor-row">
        <div class="sensor-top">
          <div class="sensor-name">
            <div class="sensor-dot" style="background:${sevColor}"></div>
            状态
          </div>
          <div class="sensor-val" style="color:${sevColor}">${msgIcon} ${n.message}</div>
        </div>
      </div>

    </div>

    <div class="node-footer">
      <span class="node-ts">更新时间：${ts}</span>
      <span class="pill ${sev === "critical" ? "pill-danger" : sev === "warning" ? "pill-warning" : "pill-muted"}"
            style="font-size:10px">${severityLabel(sev)}</span>
    </div>
  </div>`;
}

function nodeOfflineCard(id) {
  return `
  <div class="node-card offline">
    <div class="node-header">
      <div>
        <div class="node-title">${NODE_LABELS[id]}</div>
        <div class="node-id">${id.toUpperCase()}</div>
      </div>
      <span class="pill pill-muted">离线</span>
    </div>
    <div class="empty-state" style="padding:20px 0">尚未收到数据</div>
  </div>`;
}

function renderActiveAlerts() {
  const list  = document.getElementById("activeAlertsList");
  const count = document.getElementById("activeCount");
  if (!list) return;

  const liveAlerts = Object.values(state.nodes).filter(n => n.inAlert);

  if (liveAlerts.length === 0) {
    list.innerHTML = `<div class="empty-state"><div class="icon">✅</div>当前无警报，所有节点均安全。</div>`;
    if (count) count.style.display = "none";
    return;
  }

  if (count) { count.style.display = ""; count.textContent = liveAlerts.length; }

  list.innerHTML = liveAlerts.map(n => alertRowHTML({
    nodeId:    n.id,
    title:     n.title,
    message:   n.message,
    severity:  n.severity,
    ts:        new Date(n.ts).toLocaleTimeString("en-US", { hour12: false }),
    isHistory: false
  })).join("");

  list.querySelectorAll(".alert-row").forEach(row => {
    row.addEventListener("click", () => {
      const nodeId = row.dataset.nodeid;
      showAlertDetail(nodeId, state.nodes[nodeId]);
    });
  });
}

function renderAlertHistory() {
  const list  = document.getElementById("alertHistoryList");
  const count = document.getElementById("historyCount");
  if (!list) return;

  if (state.alertHistory.length === 0) {
    list.innerHTML = `<div class="empty-state"><div class="icon">📂</div>暂无历史警报。</div>`;
    if (count) count.textContent = "0";
    return;
  }

  if (count) count.textContent = state.alertHistory.length;

  list.innerHTML = state.alertHistory.slice(0, 20).map((a, idx) => alertRowHTML({
    nodeId:    a.nodeId,
    title:     a.title    || "警报",
    message:   a.message  || "",
    severity:  a.severity || "info",
    ts:        a.timestamp?.toDate ? a.timestamp.toDate().toLocaleString("zh-CN")
               : a.timestamp ? new Date(a.timestamp).toLocaleString("zh-CN") : "—",
    isHistory: true,
    idx
  })).join("");

  list.querySelectorAll(".alert-row").forEach(row => {
    row.addEventListener("click", () => {
      const idx = parseInt(row.dataset.idx);
      showAlertDetail(null, state.alertHistory[idx], true);
    });
  });
}

function alertRowHTML({ nodeId, title, message, severity, ts, isHistory, idx }) {
  const sev   = sevClass(severity);
  const icon  = sevIcon(severity);
  const label = NODE_LABELS[nodeId] || nodeId || "未知节点";
  const extra = isHistory ? `data-idx="${idx}"` : `data-nodeid="${nodeId}"`;

  return `
  <div class="alert-row ${sev}" ${extra}>
    <div class="alert-icon">${icon}</div>
    <div>
      <div class="alert-title-text">${title}</div>
      <div class="alert-meta">${label} · ${ts}</div>
      <div class="alert-desc">${message}</div>
    </div>
    <span class="alert-sev sev-${sev}">${severityLabel(sev)}</span>
  </div>`;
}

function showAlertDetail(nodeId, data, isHistory = false) {
  const panel = document.getElementById("detailContent");
  if (!panel || !data) return;

  const label   = NODE_LABELS[nodeId || data.nodeId] || nodeId || "未知节点";
  const sev     = data.severity || "info";
  const sevCol  = sev === "critical" ? "var(--danger)" : sev === "warning" ? "var(--warning)" : "var(--accent)";
  const timeStr = isHistory
    ? (data.timestamp?.toDate ? data.timestamp.toDate().toLocaleString("zh-CN")
       : data.timestamp ? new Date(data.timestamp).toLocaleString("zh-CN") : "—")
    : new Date(data.ts || Date.now()).toLocaleString("zh-CN");

  // Recommended actions based on message content
  let action = "• 持续监测该节点，并在现场核实传感器读数。";
  const msg = (data.message || "").toLowerCase();
  if (msg.includes("co") || msg.includes("carbon")) {
    action = "• 检测到一氧化碳异常，请立即疏散区域人员并佩戴空气呼吸器。";
  } else if (msg.includes("ch4") || msg.includes("methane") || msg.includes("gas")) {
    action = "• 检测到甲烷或气体异常，请疏散巷道人员、加强通风并检查气源。";
  } else if (msg.includes("water") || msg.includes("flood")) {
    action = "• 存在积水风险，请启动水泵并将设备转移至较高位置。";
  } else if (sev === "critical") {
    action = "• 存在严重危险，请立即疏散并联系矿井安全负责人。";
  }

  panel.innerHTML = `
  <div class="detail-content">
    <div class="detail-row">
      <span class="detail-key">节点</span>
      <span class="detail-val">${label}</span>
    </div>
    <div class="detail-row">
      <span class="detail-key">标题</span>
      <span class="detail-val" style="color:${sevCol}">${data.title || "—"}</span>
    </div>
    <div class="detail-row">
      <span class="detail-key">信息</span>
      <span class="detail-val">${data.message || "—"}</span>
    </div>
    <div class="detail-row">
      <span class="detail-key">警报等级</span>
      <span class="detail-val" style="color:${sevCol}">${severityLabel(sev)}</span>
    </div>
    <div class="detail-row">
      <span class="detail-key">时间</span>
      <span class="detail-val">${timeStr}</span>
    </div>
    <div class="detail-row">
      <span class="detail-key">来源</span>
      <span class="detail-val">${isHistory ? "历史记录" : "实时数据"}</span>
    </div>
    <div style="margin-top:12px;padding:12px;
                background:var(--danger-dim);
                border:1px solid rgba(255,77,77,0.25);
                border-radius:8px;font-size:12px;
                color:var(--text-2);line-height:1.8">
      <strong style="color:var(--text)">处理建议：</strong><br>${action}
    </div>
    ${!isHistory ? `<button class="detail-action" onclick="triggerSOS()">⚠️ 触发紧急求救</button>` : ""}
  </div>`;
}

function updateAnalyticsCounters() {
  const critical = state.alertHistory.filter(a => a.severity === "critical").length;
  const warning  = state.alertHistory.filter(a => a.severity === "warning").length;
  setText("anCritical", critical);
  setText("anWarning",  warning);
  setText("anTotal",    state.alertHistory.length);

  renderSensorTable();
  redrawAnalyticsCharts();
}

function renderSensorTable() {
  const tbody = document.getElementById("sensorTableBody");
  if (!tbody) return;

  if (Object.keys(state.nodes).length === 0) {
    tbody.innerHTML = `<tr><td colspan="6" style="color:var(--text-3);text-align:center;padding:20px">正在等待数据…</td></tr>`;
    return;
  }

  tbody.innerHTML = NODE_IDS.map(id => {
    const n = state.nodes[id];
    if (!n) return `<tr><td>${id}</td><td colspan="5" style="color:var(--text-3)">离线</td></tr>`;
    const sev    = n.severity || "info";
    const sevCol = sev === "critical" ? "var(--danger)" : sev === "warning" ? "var(--warning)" : "var(--success)";
    const pill   = n.inAlert
      ? `<span class="pill pill-danger" style="font-size:10px">警报</span>`
      : `<span class="pill pill-success" style="font-size:10px">安全</span>`;
    return `
    <tr>
      <td>${NODE_LABELS[id]}</td>
      <td>${n.mq4 ?? "—"}</td>
      <td>${n.mq7 ?? "—"}</td>
      <td style="color:${sevCol}">${n.message || "—"}</td>
      <td style="color:${sevCol}">${severityLabel(sev)}</td>
      <td>${pill}</td>
    </tr>`;
  }).join("");
}

const CHART_COLORS = {
  zlmdesign: "#2e7dff"
};

function chartDefaults() {
  return {
    responsive: true, maintainAspectRatio: false,
    animation: { duration: 300 },
    plugins: { legend: { display: false } },
    scales: {
      x: { display: false },
      y: {
        grid: { color: "rgba(255,255,255,0.05)" },
        ticks: { color: "#4f6280", font: { size: 10 } }
      }
    }
  };
}

function initTrendCharts() {
  const ctxMQ4 = document.getElementById("chartMQ4")?.getContext("2d");
  const ctxMQ7 = document.getElementById("chartMQ7")?.getContext("2d");
  const ctxDist = document.getElementById("chartDist")?.getContext("2d");

  if (ctxMQ4) {
    state.chartMQ4 = new Chart(ctxMQ4, {
      type: "line",
      data: { labels: [], datasets: [{ data: [], borderColor: "#ffb020", backgroundColor: "rgba(255,176,32,.12)", fill: true, tension: .3, pointRadius: 1 }] },
      options: chartDefaults()
    });
  }
  if (ctxMQ7) {
    state.chartMQ7 = new Chart(ctxMQ7, {
      type: "line",
      data: { labels: [], datasets: [{ data: [], borderColor: "#2e7dff", backgroundColor: "rgba(46,125,255,.12)", fill: true, tension: .3, pointRadius: 1 }] },
      options: chartDefaults()
    });
  }

  if (ctxDist) {
    state.chartDist = new Chart(ctxDist, {
      type: "doughnut",
      data: {
        labels: ["严重", "预警", "信息", "安全"],
        datasets: [{
          data: [0, 0, 0, 1],
          backgroundColor: ["#ff4d4d", "#ffb020", "#29b6f6", "#22c55e"],
          borderWidth: 2, borderColor: "#111827"
        }]
      },
      options: {
        responsive: true, maintainAspectRatio: false,
        plugins: { legend: { position: "bottom", labels: { color: "#8fa3bf", font: { size: 10 }, padding: 10 } } }
      }
    });
  }
}

function renderTrendCharts() {
  const nodeId = NODE_IDS[0];
  const mq4 = state.mq4History[nodeId] || [];
  const mq7 = state.mq7History[nodeId] || [];
  if (state.chartMQ4) {
    state.chartMQ4.data.labels = mq4.map(x => new Date(x.ts).toLocaleTimeString([], { minute: "2-digit", second: "2-digit" }));
    state.chartMQ4.data.datasets[0].data = mq4.map(x => x.value);
    state.chartMQ4.update("none");
  }
  if (state.chartMQ7) {
    state.chartMQ7.data.labels = mq7.map(x => new Date(x.ts).toLocaleTimeString([], { minute: "2-digit", second: "2-digit" }));
    state.chartMQ7.data.datasets[0].data = mq7.map(x => x.value);
    state.chartMQ7.update("none");
  }
  if (state.chartDist) {
    const critical = state.alertHistory.filter(a => a.severity === "critical").length;
    const warning  = state.alertHistory.filter(a => a.severity === "warning").length;
    const info     = state.alertHistory.filter(a => a.severity === "info").length;
    const safe     = Math.max(0, Object.values(state.nodes).filter(n => !n.inAlert).length);
    state.chartDist.data.datasets[0].data = [critical, warning, info, safe];
    state.chartDist.update("none");
  }
}

function redrawAnalyticsCharts() {
  const ctxH = document.getElementById("chartHazard")?.getContext("2d");
  const ctxN = document.getElementById("chartNodes")?.getContext("2d");

  if (ctxH && !state.chartHazard) {
    state.chartHazard = new Chart(ctxH, {
      type: "bar",
      data: {
        labels: ["严重", "预警", "信息"],
        datasets: [{
          label: "警报数量",
          data: [0, 0, 0],
          backgroundColor: ["#ff4d4d", "#ffb020", "#29b6f6"],
          borderRadius: 6
        }]
      },
      options: { ...chartDefaults(), plugins: { legend: { display: false } } }
    });
  }

  if (ctxN && !state.chartNodes) {
    state.chartNodes = new Chart(ctxN, {
      type: "bar",
      data: {
        labels: NODE_IDS.map(id => NODE_LABELS[id]),
        datasets: [
          { label: "警报", data: [], backgroundColor: "#ff4d4d", borderRadius: 4 },
          { label: "安全", data: [], backgroundColor: "#22c55e", borderRadius: 4 }
        ]
      },
      options: {
        ...chartDefaults(),
        plugins: { legend: { display: true, labels: { color: "#8fa3bf", font: { size: 10 } } } },
        scales: {
          x: { ticks: { color: "#4f6280", font: { size: 10 } }, grid: { display: false } },
          y: { grid: { color: "rgba(255,255,255,0.05)" }, ticks: { color: "#4f6280", font: { size: 10 } } }
        }
      }
    });
  }

  if (state.chartHazard) {
    const critical = state.alertHistory.filter(a => a.severity === "critical").length;
    const warning  = state.alertHistory.filter(a => a.severity === "warning").length;
    const info     = state.alertHistory.filter(a => a.severity === "info").length;
    state.chartHazard.data.datasets[0].data = [critical, warning, info];
    state.chartHazard.update("none");
  }

  if (state.chartNodes) {
    state.chartNodes.data.datasets[0].data = NODE_IDS.map(id => state.nodes[id]?.inAlert ? 1 : 0);
    state.chartNodes.data.datasets[1].data = NODE_IDS.map(id => state.nodes[id] && !state.nodes[id].inAlert ? 1 : 0);
    state.chartNodes.update("none");
  }
}

const NODE_COORDS = {
  zlmdesign: [6.7850, 80.3640]
};

function initMap() {
  const mapEl = document.getElementById("leafletMap");
  if (!mapEl || state.leafletMap) return;

  state.leafletMap = L.map("leafletMap", {
    center: [6.786, 80.364],
    zoom: 16,
    zoomControl: true
  });

  L.tileLayer("https://{s}.tile.openstreetmap.org/{z}/{x}/{y}.png", {
    attribution: "© OpenStreetMap contributors",
    maxZoom: 19
  }).addTo(state.leafletMap);

  L.circleMarker([6.7855, 80.3650], {
    radius: 12, fillColor: "#2e7dff", color: "#fff",
    weight: 2, fillOpacity: 0.9
  }).bindPopup("<b>矿安脉搏网关</b><br>主设备 MP-001")
    .addTo(state.leafletMap);

  NODE_IDS.forEach(id => {
    const coords = NODE_COORDS[id];
    const marker = L.circleMarker(coords, {
      radius: 10, fillColor: "#8fa3bf", color: "#fff",
      weight: 2, fillOpacity: 0.85
    }).bindPopup(`<b>${NODE_LABELS[id]}</b><br>正在加载…`)
      .addTo(state.leafletMap);
    state.leafletMarkers[id] = marker;
  });

  setTimeout(() => state.leafletMap.invalidateSize(), 200);
}

function updateMapMarker(nodeId) {
  if (!state.leafletMap) return;
  const marker = state.leafletMarkers[nodeId];
  const n      = state.nodes[nodeId];
  if (!marker || !n) return;

  const color = n.inAlert ? "#ff4d4d" : "#22c55e";
  marker.setStyle({ fillColor: color });
  marker.setPopupContent(`
    <b>${NODE_LABELS[nodeId]}</b><br>
    <b>${n.title}</b><br>
    ${n.message}<br>
    警报等级：<b style="color:${color}">${severityLabel(n.severity)}</b><br>
    状态：<b style="color:${color}">${n.inAlert ? "警报" : "安全"}</b>
  `);
}

window.requestNotificationPermission = async function () {
  if (!("Notification" in window)) return;
  const perm = await Notification.requestPermission();
  state.notifEnabled = perm === "granted";
  const label = document.getElementById("notifLabel");
  if (label) label.textContent = state.notifEnabled ? "通知已开启" : "启用通知";
  const status = document.getElementById("notifStatus");
  if (status) status.textContent = state.notifEnabled ? "已授权" : "已拒绝";
};

function triggerBrowserNotification(nodeId, title, message) {
  if (!state.notifEnabled || Notification.permission !== "granted") return;
  new Notification(`⚠️ ${title}`, {
    body: `${NODE_LABELS[nodeId]}: ${message}`,
    icon: "/favicon.ico"
  });
}

function playAlertSound() {
  try {
    const ctx = new (window.AudioContext || window.webkitAudioContext)();
    [880, 660, 880].forEach((freq, i) => {
      const osc  = ctx.createOscillator();
      const gain = ctx.createGain();
      osc.connect(gain); gain.connect(ctx.destination);
      osc.frequency.value = freq;
      osc.type = "square";
      gain.gain.setValueAtTime(0.12, ctx.currentTime + i * 0.25);
      gain.gain.exponentialRampToValueAtTime(0.001, ctx.currentTime + i * 0.25 + 0.2);
      osc.start(ctx.currentTime + i * 0.25);
      osc.stop(ctx.currentTime + i * 0.25 + 0.25);
    });
  } catch (e) {}
}

window.triggerSOS = function () {
  showToast("🆘 已触发紧急求救", "请立即疏散矿井人员并联系应急救援部门！", "danger");
  playAlertSound();
  if (state.notifEnabled) {
    new Notification("🆘 矿安脉搏紧急求救", {
      body: "监控平台已触发紧急求救，请立即拨打 119。"
    });
  }
};

window.toggleSetting = function (el) {
  el.classList.toggle("on");
  const id = el.id;
  if (id === "toggleSound") state.soundEnabled = el.classList.contains("on");
  if (id === "toggleNotif") {
    if (el.classList.contains("on")) requestNotificationPermission();
    else state.notifEnabled = false;
  }
};

function showToast(title, body, type = "danger") {
  const container = document.getElementById("toastContainer");
  if (!container) return;

  const t = document.createElement("div");
  t.className = `toast ${type}`;
  t.innerHTML = `
    <button class="toast-close" onclick="this.parentElement.remove()">×</button>
    <div class="toast-title">${title}</div>
    <div class="toast-body">${body}</div>`;
  container.appendChild(t);
  setTimeout(() => t.remove(), 6000);
}

function setConnectionStatus(connected) {
  const dot   = document.getElementById("connDot");
  const label = document.getElementById("connLabel");
  if (!dot || !label) return;
  if (connected) {
    dot.className     = "conn-dot connected";
    label.textContent = "实时 · OneNET";
  } else {
    dot.className     = "conn-dot error";
    label.textContent = "连接已断开";
  }
}

function setText(id, val) {
  const el = document.getElementById(id);
  if (el) el.textContent = val;
}

document.addEventListener("DOMContentLoaded", () => {
  initNav();
  initClock();
  initTrendCharts();
  listenOneNet();

  renderActiveAlerts();
  renderAlertHistory();

  const notifStatus = document.getElementById("notifStatus");
  if (notifStatus) {
    notifStatus.textContent = Notification.permission === "granted" ? "已授权" : "未授权";
  }
});
