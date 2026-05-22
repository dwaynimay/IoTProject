import { state } from './state.js';
import { renderNodes, appendEvent, toast } from './ui.js';
import { renderChart } from './chart.js';

export async function api(path) {
  try {
    const r = await fetch(state.apiBase + path);
    if (!r.ok) throw new Error(r.status);
    return await r.json();
  } catch(e) {
    toast('API error: ' + path, 'danger');
    return null;
  }
}

export async function fetchStatus() {
  const d = await api('/api/status');
  if (!d) return;
  renderNodes(d.nodes);
  document.getElementById('nodeCount').textContent = d.nodes.length;
}

export async function fetchMetrics() {
  const d = await api('/api/metrics');
  if (!d) return;
  document.getElementById('s-uptime').textContent  = Math.round(d.uptime_s) + 's';
  document.getElementById('s-windows').textContent = d.total_windows;
  document.getElementById('s-avg').textContent     = d.avg_rekon_ms.toFixed(1) + 'ms';
  document.getElementById('s-lq').textContent      = d.total_low_quality;
  document.getElementById('s-crit').textContent    = d.total_critical;
}

export async function fetchDB() {
  const d = await api('/api/db');
  if (!d) return;
  document.getElementById('s-dbsize').textContent = d.size_kb.toFixed(1) + ' KB';
  document.getElementById('s-dbrows').textContent = d.rows_windows;
  document.getElementById('s-ret').textContent    = d.retention_hours;
}

export async function fetchNodeWindows(nodeId, signal, n=64) {
  const d = await api(`/api/nodes/${nodeId}/windows?signal=${signal}&n=${n}&include_values=true`);
  if (!d || !d.windows.length) return;

  const vals = [];
  d.windows.forEach(w => {
    if (w.values) vals.push(...w.values);
  });
  state.chartData = vals.slice(-256);
  renderChart(state.chartData, signal);
}

export async function fetchEvents(nodeId) {
  const d = await api(`/api/nodes/${nodeId}/events?n=30`);
  if (!d) return;
  const log = document.getElementById('eventLog');
  if (!d.events.length) { 
    log.innerHTML = '<div style="padding:16px;font-size:11px;color:var(--text3);font-family:var(--mono);text-align:center">tidak ada event</div>'; 
    return; 
  }
  log.innerHTML = '';
  d.events.forEach(e => appendEvent(e, false));
  document.getElementById('evCount').textContent = d.events.length;
}
