import { state, getLabelColor, getLabelBgColor, SIG_COLORS, SIG_UNITS } from './state.js';
import { fetchNodeVitalsHistory, fetchNodeIMUHistory } from './api.js';

const trendCharts = new Map(); // nodeId -> echarts instance (vitals trend)
const imuCharts   = new Map(); // nodeId -> echarts instance (IMU waveform)

const IMU_BUFFER_SIZE = 256;
const imuBuffers = new Map(); // nodeId -> { ax, ay, az, gx, gy, gz }

// ── playbackQueue DIHAPUS — rendering sekarang via global setInterval seperti kode 2 ──

function formatDuration(sec) {
  if (sec < 60) return `${sec}s`;
  const m = Math.floor(sec / 60);
  const s = sec % 60;
  if (m < 60) return `${m}m ${s}s`;
  const h = Math.floor(m / 60);
  const remM = m % 60;
  return `${h}h ${remM}m`;
}

export function renderNodeList() {
  const container = document.getElementById('nodesContainer');
  if (!container) return;

  if (state.nodes.size === 0) {
    container.innerHTML = `
      <div class="no-data" style="height: 200px; display: flex; flex-direction: column; align-items: center; justify-content: center; width: 100%; grid-column: 1/-1;">
        <div class="no-data-icon" style="font-size: 48px; margin-bottom: 16px;">📡</div>
        <div style="font-size: 16px; color: var(--text2);">Menunggu koneksi node...</div>
      </div>
    `;
    return;
  }

  const noDataEl = container.querySelector('.no-data');
  if (noDataEl) {
    container.innerHTML = '';
  }
  
  Array.from(state.nodes.values()).sort((a,b) => a.node_id - b.node_id).forEach(node => {
    let shell = document.getElementById(`node-card-shell-${node.node_id}`);
    
    if (!shell) {
      shell = document.createElement('div');
      shell.className = 'node-card-shell';
      shell.id = `node-card-shell-${node.node_id}`;
      shell.onclick = () => selectNode(node.node_id);
      container.appendChild(shell);
    }
    
    if (state.selectedNode === node.node_id) {
      shell.classList.add('active');
    } else {
      shell.classList.remove('active');
    }
    
    const hr = node.last_hr > 0 ? Math.round(node.last_hr) : '--';
    const spo2 = node.last_spo2 > 0 ? `${node.last_spo2.toFixed(1)}%` : '--%';
    
    const label = node.last_activity || "OK";
    const conf = node.last_confidence ? ` (${(node.last_confidence * 100).toFixed(0)}%)` : "";
    const color = getLabelColor(label);
    const bgColor = getLabelBgColor(label);

    const now = Date.now();
    const uptimeSec = state.server_start_time_offset ? Math.floor((now - state.server_start_time_offset) / 1000) : null;
    const uptimeStr = uptimeSec !== null ? formatDuration(uptimeSec) : '--';

    shell.innerHTML = `
      <div class="node-card" id="node-card-${node.node_id}">
        <div class="card-header">
          <span class="node-name">Node ${node.node_id}</span>
          <span class="node-status-badge disconnected" id="status-${node.node_id}">
            <span class="status-dot"></span>
            <span class="status-text">Disconnected</span>
          </span>
        </div>
        <div class="card-meta">
          <div class="meta-item">
            <span class="meta-icon">
              <svg class="icon-svg mini" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <circle cx="12" cy="12" r="10"/><polyline points="12 6 12 12 16 14"/>
              </svg>
            </span>
            <span class="meta-label">Uptime:</span>
            <span class="meta-value" id="uptime-${node.node_id}">${uptimeStr}</span>
          </div>
          <div class="meta-item">
            <span class="meta-icon">
              <svg class="icon-svg mini" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M21 16V8a2 2 0 0 0-1-1.73l-7-4a2 2 0 0 0-2 0l-7 4A2 2 0 0 0 3 8v8a2 2 0 0 0 1 1.73l7 4a2 2 0 0 0 2 0l7-4A2 2 0 0 0 21 16z"/>
                <polyline points="3.27 6.96 12 12.01 20.73 6.96"/>
                <line x1="12" y1="22.08" x2="12" y2="12"/>
              </svg>
            </span>
            <span class="meta-label">Windows:</span>
            <span class="meta-value" id="win-${node.node_id}">${node.total_windows || 0}</span>
          </div>
        </div>
        <div class="card-activity" id="act-lbl-${node.node_id}" style="color: ${color}; background-color: ${bgColor};">
          ${label}${conf}
        </div>
        <div class="card-vitals">
          <div class="vital-item hr">
            <span class="vital-icon">
              <svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M19 14c1.49-1.46 3-3.21 3-5.5A5.5 5.5 0 0 0 16.5 3c-1.76 0-3 .5-4.5 2-1.5-1.5-2.74-2-4.5-2A5.5 5.5 0 0 0 2 8.5c0 2.3 1.5 4.05 3 5.5l7 7Z"/>
              </svg>
            </span>
            <div class="vital-details">
              <span class="vital-value" id="vit-hr-${node.node_id}">${hr}</span>
              <span class="vital-label">Heart Rate (bpm)</span>
            </div>
          </div>
          <div class="vital-item spo2">
            <span class="vital-icon">
              <svg class="icon-svg" viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2.5" stroke-linecap="round" stroke-linejoin="round">
                <path d="M12 22a7 7 0 0 0 7-7c0-4.3-7-11-7-11S5 10.7 5 15a7 7 0 0 0 7 7Z"/>
              </svg>
            </span>
            <div class="vital-details">
              <span class="vital-value" id="vit-spo2-${node.node_id}">${spo2}</span>
              <span class="vital-label">Oxygen Level</span>
            </div>
          </div>
        </div>
        <div class="card-trend" id="chart-${node.node_id}"></div>
        <div class="card-imu-header">IMU Signal — Historical Stream</div>
        <div class="card-imu" id="imu-chart-${node.node_id}"></div>
      </div>
    `;
    
    setTimeout(async () => {
      initTrendChart(node.node_id);
      await initIMUChart(node.node_id);
    }, 10);
  });

  updateConnectionStatuses();
}

export function initTrendChart(nodeId) {
  const el = document.getElementById(`chart-${nodeId}`);
  if (!el || typeof echarts === 'undefined') return;
  
  if (trendCharts.has(nodeId)) {
    trendCharts.get(nodeId).dispose();
  }
  
  const chart = echarts.init(el);
  const option = {
    grid: { top: 15, bottom: 20, left: 35, right: 35 },
    tooltip: {
      trigger: 'axis',
      backgroundColor: 'rgba(255, 255, 255, 0.98)',
      borderColor: '#e2e8f0',
      borderWidth: 1,
      textStyle: { color: '#0f172a', fontSize: 10, fontFamily: 'var(--sans)' },
      shadowColor: 'rgba(0, 0, 0, 0.04)',
      shadowBlur: 10
    },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: Array(60).fill(''),
      axisLine: { lineStyle: { color: 'rgba(0, 0, 0, 0.05)' } },
      axisLabel: { show: false }
    },
    yAxis: [
      {
        type: 'value',
        min: function(value) { return Math.max(0, Math.floor(value.min - 10)); },
        max: function(value) { return Math.min(220, Math.ceil(value.max + 10)); },
        splitLine: { lineStyle: { color: 'rgba(0,0,0,0.03)' } },
        axisLabel: { color: '#dc2626', fontSize: 9, fontFamily: 'var(--mono)' }
      },
      {
        type: 'value',
        min: 80,
        max: 100,
        position: 'right',
        splitLine: { show: false },
        axisLabel: { color: '#0284c7', fontSize: 9, fontFamily: 'var(--mono)' }
      }
    ],
    series: [
      {
        name: 'Heart Rate',
        type: 'line',
        smooth: true,
        symbol: 'none',
        lineStyle: { color: '#dc2626', width: 1.5 },
        data: Array(60).fill(null)
      },
      {
        name: 'SpO2',
        type: 'line',
        yAxisIndex: 1,
        smooth: true,
        symbol: 'none',
        lineStyle: { color: '#0284c7', width: 1.5 },
        data: Array(60).fill(null)
      }
    ],
    animation: false
  };
  
  chart.setOption(option);
  trendCharts.set(nodeId, chart);
  
  if (state.vitalsBuffer.has(nodeId)) {
    updateChartData(nodeId);
  } else {
    state.vitalsBuffer.set(nodeId, Array(60).fill(null));
    fetchNodeVitalsHistory(nodeId).then(windows => {
      const buffer = Array(60).fill(null);
      const startIdx = 60 - windows.length;
      for (let i = 0; i < windows.length; i++) {
        buffer[startIdx + i] = {
          hr: windows[i].hr > 0 ? windows[i].hr : null,
          spo2: windows[i].spo2 > 0 ? windows[i].spo2 : null
        };
      }
      state.vitalsBuffer.set(nodeId, buffer);
      updateChartData(nodeId);
    }).catch(err => {
      console.error(`Failed to fetch history for node ${nodeId}:`, err);
    });
  }
}

function updateChartData(nodeId) {
  const chart = trendCharts.get(nodeId);
  if (!chart) return;
  const buffer = state.vitalsBuffer.get(nodeId) || [];
  chart.setOption({
    series: [
      { data: buffer.map(b => b ? b.hr : null) },
      { data: buffer.map(b => b ? b.spo2 : null) }
    ]
  });
}

// ── IMU Chart ─────────────────────────────────────────────────────────────────
// Menerapkan improvement dari kode 2:
// 1. Y-axis soft padding 10% — mencegah zoom artifact antara history & live data
// 2. Rendering dipisah ke global setInterval — tidak perlu playbackQueue
// 3. Buffer push/shift langsung per sample — lebih sederhana & konsisten

export async function initIMUChart(nodeId) {
  const el = document.getElementById(`imu-chart-${nodeId}`);
  if (!el || typeof echarts === 'undefined') return;

  if (imuCharts.has(nodeId)) {
    imuCharts.get(nodeId).dispose();
  }

  const chart = echarts.init(el);
  const xData = Array.from({ length: IMU_BUFFER_SIZE }, (_, i) => i);

  chart.setOption({
    backgroundColor: 'rgba(0,0,0,0.015)',
    graphic: [{
      type: 'text',
      id: 'nodata',
      left: 'center', top: 'middle',
      style: { text: 'Menunggu data IMU...', font: '11px var(--mono)', fill: '#cbd5e1' },
      z: 100
    }],
    grid: { top: 14, bottom: 20, left: 44, right: 44 },
    tooltip: {
      trigger: 'axis',
      backgroundColor: 'rgba(255, 255, 255, 0.98)',
      borderColor: '#e2e8f0',
      borderWidth: 1,
      textStyle: { color: '#0f172a', fontSize: 10, fontFamily: 'var(--mono)' },
      shadowBlur: 10,
      formatter(params) {
        let s = `<div style="font-size:10px;color:#64748b;margin-bottom:4px">Sample ${params[0].axisValue}</div>`;
        params.forEach(p => {
          if (p.value != null) {
            const unit = SIG_UNITS[p.seriesName] || '';
            s += `<div style="display:flex;justify-content:space-between;gap:12px">`
              + `<span style="color:${p.color}">${p.seriesName}</span>`
              + `<span style="font-family:var(--mono)">${Number(p.value).toFixed(3)} <span style="color:#94a3b8;font-size:9px">${unit}</span></span></div>`;
          }
        });
        return s;
      }
    },
    legend: {
      top: 0, right: 0,
      textStyle: { fontSize: 9, fontFamily: 'var(--mono)', color: '#64748b' },
      itemWidth: 10, itemHeight: 3,
      data: [
        { name: 'ax', icon: 'rect' }, { name: 'ay', icon: 'rect' }, { name: 'az', icon: 'rect' },
        { name: 'gx', icon: 'rect' }, { name: 'gy', icon: 'rect' }, { name: 'gz', icon: 'rect' }
      ],
      formatter: name => `${name} (${SIG_UNITS[name] || ''})`
    },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: xData,
      axisLine: { lineStyle: { color: 'rgba(0,0,0,0.05)' } },
      axisLabel: { show: false },
      splitLine: { show: true, lineStyle: { color: 'rgba(0,0,0,0.03)', type: 'dashed' } }
    },
    yAxis: [
      {
        type: 'value', name: 'm/s²', nameTextStyle: { fontSize: 8, color: '#94a3b8' },
        position: 'left',
        // IMPROVEMENT: Soft 10% padding — mencegah Y-axis snap ke extreme saat
        // history range berbeda dengan live data range (zoom-out/zoom-in artifact)
        min: value => { const r = value.max - value.min || 1; return value.min - r * 0.1; },
        max: value => { const r = value.max - value.min || 1; return value.max + r * 0.1; },
        splitLine: { lineStyle: { color: 'rgba(0,0,0,0.04)' } },
        axisLabel: { fontSize: 8, fontFamily: 'var(--mono)', color: '#94a3b8',
          formatter: v => Math.abs(v) >= 1000 ? (v/1000).toFixed(1)+'k' : v.toFixed(1) }
      },
      {
        type: 'value', name: '°/s', nameTextStyle: { fontSize: 8, color: '#94a3b8' },
        position: 'right',
        min: value => { const r = value.max - value.min || 1; return value.min - r * 0.1; },
        max: value => { const r = value.max - value.min || 1; return value.max + r * 0.1; },
        splitLine: { show: false },
        axisLabel: { fontSize: 8, fontFamily: 'var(--mono)', color: '#94a3b8',
          formatter: v => Math.abs(v) >= 1000 ? (v/1000).toFixed(1)+'k' : v.toFixed(1) }
      }
    ],
    series: [
      { name: 'ax', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 0,
        lineStyle: { color: SIG_COLORS.ax, width: 1.2 }, data: Array(IMU_BUFFER_SIZE).fill(null) },
      { name: 'ay', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 0,
        lineStyle: { color: SIG_COLORS.ay, width: 1.2 }, data: Array(IMU_BUFFER_SIZE).fill(null) },
      { name: 'az', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 0,
        lineStyle: { color: SIG_COLORS.az, width: 1.2 }, data: Array(IMU_BUFFER_SIZE).fill(null) },
      { name: 'gx', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 1,
        lineStyle: { color: SIG_COLORS.gx, width: 1.2, type: 'dashed' }, data: Array(IMU_BUFFER_SIZE).fill(null) },
      { name: 'gy', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 1,
        lineStyle: { color: SIG_COLORS.gy, width: 1.2, type: 'dashed' }, data: Array(IMU_BUFFER_SIZE).fill(null) },
      { name: 'gz', type: 'line', smooth: false, symbol: 'none', yAxisIndex: 1,
        lineStyle: { color: SIG_COLORS.gz, width: 1.2, type: 'dashed' }, data: Array(IMU_BUFFER_SIZE).fill(null) },
    ],
    animation: false
  });

  imuCharts.set(nodeId, chart);
  window.addEventListener('resize', () => chart && chart.resize());

  // Pre-fill imuBuffers dari API history jika belum ada
  // IMPROVEMENT: Skip fetch jika buffer sudah ada (node re-render) agar live data tidak hilang
  if (!imuBuffers.has(nodeId)) {
    const buffers = {
      ax: Array(IMU_BUFFER_SIZE).fill(null),
      ay: Array(IMU_BUFFER_SIZE).fill(null),
      az: Array(IMU_BUFFER_SIZE).fill(null),
      gx: Array(IMU_BUFFER_SIZE).fill(null),
      gy: Array(IMU_BUFFER_SIZE).fill(null),
      gz: Array(IMU_BUFFER_SIZE).fill(null)
    };
    imuBuffers.set(nodeId, buffers);

    fetchNodeIMUHistory(nodeId, 4).then(history => {
      const keys = ['ax', 'ay', 'az', 'gx', 'gy', 'gz'];
      keys.forEach(k => {
        const vals = history[k] || [];
        if (vals.length > 0) {
          const len = Math.min(vals.length, IMU_BUFFER_SIZE);
          for (let i = 0; i < len; i++) {
            buffers[k][IMU_BUFFER_SIZE - len + i] = vals[vals.length - len + i];
          }
        }
      });
      // Render chart dengan history (nodata label hilang)
      const c = imuCharts.get(nodeId);
      if (c) {
        c.setOption({
          graphic: [{ id: 'nodata', style: { text: '' } }],
          series: [
            { name: 'ax', data: buffers.ax.slice() },
            { name: 'ay', data: buffers.ay.slice() },
            { name: 'az', data: buffers.az.slice() },
            { name: 'gx', data: buffers.gx.slice() },
            { name: 'gy', data: buffers.gy.slice() },
            { name: 'gz', data: buffers.gz.slice() },
          ]
        });
      }
    }).catch(err => {
      console.warn(`IMU history fetch failed for node ${nodeId}:`, err);
    });
  }
}

// IMPROVEMENT: updateIMUChart sekarang hanya push/shift ke buffer,
// rendering diserahkan ke global setInterval di bawah — sama seperti kode 2.
// playbackQueue dihilangkan karena buffer push/shift sudah cukup smooth pada 66ms interval.
export function updateIMUChart(nodeId, imuSignals) {
  if (!imuSignals) return;

  if (!imuBuffers.has(nodeId)) {
    imuBuffers.set(nodeId, {
      ax: Array(IMU_BUFFER_SIZE).fill(null),
      ay: Array(IMU_BUFFER_SIZE).fill(null),
      az: Array(IMU_BUFFER_SIZE).fill(null),
      gx: Array(IMU_BUFFER_SIZE).fill(null),
      gy: Array(IMU_BUFFER_SIZE).fill(null),
      gz: Array(IMU_BUFFER_SIZE).fill(null)
    });
  }

  const buffers = imuBuffers.get(nodeId);
  const keys = ['ax', 'ay', 'az', 'gx', 'gy', 'gz'];

  // Jika data berupa array (burst per window), push tiap sample satu per satu
  // Jika data berupa scalar, push langsung
  keys.forEach(k => {
    if (imuSignals[k] === undefined) return;
    if (Array.isArray(imuSignals[k])) {
      imuSignals[k].forEach(val => {
        buffers[k].push(val);
        buffers[k].shift();
      });
    } else {
      buffers[k].push(imuSignals[k]);
      buffers[k].shift();
    }
  });
}
window.updateIMUChart = updateIMUChart;

// Global setInterval untuk render IMU chart — ~15fps, konsisten untuk semua node
// Sama dengan pola kode 2: render dipisah dari update data
setInterval(() => {
  for (const [nodeId, chart] of imuCharts.entries()) {
    const buffers = imuBuffers.get(nodeId);
    if (!buffers) continue;

    chart.setOption({
      graphic: [{ id: 'nodata', style: { text: '' } }],
      series: [
        { name: 'ax', data: buffers.ax.slice() },
        { name: 'ay', data: buffers.ay.slice() },
        { name: 'az', data: buffers.az.slice() },
        { name: 'gx', data: buffers.gx.slice() },
        { name: 'gy', data: buffers.gy.slice() },
        { name: 'gz', data: buffers.gz.slice() },
      ]
    });
  }
}, 66);

export function updateNodeCard(nodeId, data) {
  if (!document.getElementById(`node-card-shell-${nodeId}`)) {
    if (!state.nodes.has(nodeId)) {
      state.nodes.set(nodeId, {
        node_id: nodeId,
        last_seen_ms: Date.now(),
        last_seen_ago_s: 0
      });
    }
    renderNodeList();
    return;
  }
  
  const node = state.nodes.get(nodeId);
  if (node) {
    node.last_seen_ms = Date.now();
    node.last_seen_ago_s = 0;
    if (data.hr > 0) node.last_hr = data.hr;
    if (data.spo2 > 0) node.last_spo2 = data.spo2;
    node.total_windows = data.window_num;
  }

  const hrEl = document.getElementById(`vit-hr-${nodeId}`);
  if (hrEl) hrEl.textContent = data.hr > 0 ? Math.round(data.hr) : '--';
  
  const spo2El = document.getElementById(`vit-spo2-${nodeId}`);
  if (spo2El) spo2El.textContent = data.spo2 > 0 ? `${data.spo2.toFixed(1)}%` : '--%';
  
  const winEl = document.getElementById(`win-${nodeId}`);
  if (winEl) winEl.textContent = data.window_num;
  
  const badge = document.getElementById(`status-${nodeId}`);
  if (badge) {
    badge.className = 'node-status-badge connected';
    badge.querySelector('.status-text').textContent = 'Connected';
  }
  
  if (data.ml_results) {
    let topLabel = null;
    let topConf = 0;
    
    Object.values(data.ml_results).forEach(res => {
      if (!res.skipped && res.confidence > topConf) {
        topConf = res.confidence;
        topLabel = res.label;
      }
    });
    
    if (topLabel) {
      if (node) {
        node.last_activity = topLabel;
        node.last_confidence = topConf;
      }
      const actLbl = document.getElementById(`act-lbl-${nodeId}`);
      if (actLbl) {
        actLbl.textContent = `${topLabel} (${(topConf*100).toFixed(0)}%)`;
        actLbl.style.color = getLabelColor(topLabel);
        actLbl.style.backgroundColor = getLabelBgColor(topLabel);
      }
    }
  }
  
  if (!state.vitalsBuffer.has(nodeId)) {
    state.vitalsBuffer.set(nodeId, Array(60).fill(null));
  }
  const buffer = state.vitalsBuffer.get(nodeId);
  buffer.shift();
  buffer.push({
    hr: data.hr > 0 ? data.hr : null,
    spo2: data.spo2 > 0 ? data.spo2 : null
  });
  updateChartData(nodeId);

  // Update IMU waveform — hanya push ke buffer, render via setInterval
  if (data.imu_signals && Object.keys(data.imu_signals).length > 0) {
    if (!imuCharts.has(nodeId)) initIMUChart(nodeId);
    updateIMUChart(nodeId, data.imu_signals);
  }
}

window.updateNodeCard = updateNodeCard;

export function selectNode(nodeId) {
  state.selectedNode = nodeId;
  document.querySelectorAll('.node-card-shell').forEach(el => el.classList.remove('active'));
  const shell = document.getElementById(`node-card-shell-${nodeId}`);
  if (shell) shell.classList.add('active');
}

export function setNodeAlert(nodeId, isAlert) {
  const shell = document.getElementById(`node-card-shell-${nodeId}`);
  if (!shell) return;
  if (isAlert) {
    shell.classList.add('alert');
  } else {
    shell.classList.remove('alert');
  }
}
window.setNodeAlert = setNodeAlert;

export function updateConnectionStatuses() {
  const now = Date.now();
  
  const uptimeSec = state.server_start_time_offset ? Math.floor((now - state.server_start_time_offset) / 1000) : null;
  const uptimeStr = uptimeSec !== null ? formatDuration(uptimeSec) : '--';

  Array.from(state.nodes.values()).forEach(node => {
    const lastSeen = node.last_seen_ms || (now - ((node.last_seen_ago_s || 0) * 1000));
    const diffSec = Math.floor((now - lastSeen) / 1000);
    const isConnected = diffSec < 15;
    
    const badge = document.getElementById(`status-${node.node_id}`);
    if (badge) {
      if (isConnected) {
        badge.className = 'node-status-badge connected';
        badge.querySelector('.status-text').textContent = 'Connected';
      } else {
        badge.className = 'node-status-badge disconnected';
        badge.querySelector('.status-text').textContent = 'Disconnected';
      }
    }

    const uptimeEl = document.getElementById(`uptime-${node.node_id}`);
    if (uptimeEl) {
      uptimeEl.textContent = uptimeStr;
    }
  });
}

setInterval(updateConnectionStatuses, 3000);