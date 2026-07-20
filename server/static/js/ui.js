import { state } from './state.js';

/**
 * Update WS status badge in the header.
 */
export function updateWSStatus(status) {
  const el = document.getElementById('wsStatus');
  if (!el) return;
  
  el.textContent = status.toUpperCase();
  
  el.className = 'badge';
  if (status === 'connected') el.classList.add('badge-ok');
  else if (status === 'error' || status === 'disconnected') el.classList.add('badge-danger');
  else el.classList.add('badge-info');
}

window.updateWSStatus = updateWSStatus;

/**
 * Display a short-lived toast notification.
 */
export function toast(msg, type = '') {
  const wrap = document.getElementById('toastWrap');
  if (!wrap) return;
  const div = document.createElement('div');
  div.className = 'toast ' + (type ? `text-${type}` : '');
  div.textContent = msg;
  wrap.appendChild(div);
  setTimeout(() => div.remove(), 3300);
}

/**
 * Flash animation for elements.
 */
export function flash(id) {
  const el = document.getElementById(id);
  if (!el) return;
  el.classList.remove('updated');
  void el.offsetWidth;
  el.classList.add('updated');
  setTimeout(() => el.classList.remove('updated'), 1100);
}

/**
 * Append event to the event log container.
 */
export function appendEvent(e, prepend = true) {
  const log = document.getElementById('eventLog');
  if (!log) return;
  
  const first = log.querySelector('.no-data');
  if (first) first.remove();

  const ts = new Date(e.ts_ms || Date.now()).toLocaleTimeString('id-ID');
  const div = document.createElement('div');
  div.className = 'event-item';
  div.style.padding = '8px 16px';
  div.style.borderBottom = '1px solid var(--border)';
  div.style.animation = 'slide-in-right 0.3s ease';
  
  let badgeClass = 'badge-outline';
  if (e.event_type === 'CRITICAL' || e.event_type === 'VALIDATION_ERROR') badgeClass = 'badge-danger';
  else if (e.event_type === 'LOW_QUALITY') badgeClass = 'badge-warn';
  else if (e.event_type === 'NODE_REGISTERED') badgeClass = 'badge-info';
  
  const header = document.createElement('div');
  header.style.cssText = 'display:flex;justify-content:space-between;margin-bottom:4px';

  const badge = document.createElement('span');
  badge.className = `badge ${badgeClass}`;
  badge.textContent = e.event_type || 'UNKNOWN';

  const timestamp = document.createElement('span');
  timestamp.style.cssText = 'font-size:10px;color:var(--text3);font-family:var(--mono)';
  timestamp.textContent = ts;

  const detail = document.createElement('div');
  detail.style.cssText = 'font-size:11px;color:var(--text);font-family:var(--mono);word-break:break-all';
  detail.textContent = `Node ${e.node_id} | ${e.detail || '-'}`;

  header.append(badge, timestamp);
  div.append(header, detail);

  if (prepend) log.insertBefore(div, log.firstChild);
  else         log.appendChild(div);

  while (log.children.length > 50) log.removeChild(log.lastChild);
  
  const countEl = document.getElementById('evCount');
  if (countEl) countEl.textContent = state.eventCount;
}
window.appendEvent = appendEvent;

/**
 * Update the command bar statistics.
 */
export function updateCommandBar(stats) {
  if (!stats) return;
  
  const upEl = document.getElementById('cmd-uptime');
  if (upEl) upEl.textContent = Math.round(stats.uptime_s || 0) + 's';
  
  const winEl = document.getElementById('cmd-windows');
  if (winEl) winEl.textContent = stats.total_windows || 0;
}

/**
 * Render the quality grid.
 */
export function renderQualityGrid(signals) {
  const grid = document.getElementById('qualityGrid');
  if (!grid) return;
  
  grid.innerHTML = '';
  
  const sigs = ['ax', 'ay', 'az', 'gx', 'gy', 'gz', 'ir'];
  sigs.forEach(sig => {
    const m = signals[sig];
    const err = m ? m.rel_error : null;
    const flag = m ? m.flag : null;
    
    // Convert error to percentage 0-100 (where lower error is better)
    const pct = err != null ? Math.max(0, Math.min(100, (1 - err) * 100)) : 0;
    
    let barColor = 'var(--border2)';
    if (flag === 'CRITICAL') barColor = 'var(--danger)';
    else if (flag === 'LOW_QUALITY') barColor = 'var(--warn)';
    else if (err != null) barColor = 'var(--ok)';
    
    const div = document.createElement('div');
    div.style.display = 'flex';
    div.style.flexDirection = 'column';
    div.style.alignItems = 'center';
    div.style.gap = '4px';
    
    div.innerHTML = `
      <div style="font-family:var(--mono); font-size:10px; color:var(--text2); text-transform:uppercase;">${sig}</div>
      <div style="width:100%; height:4px; border-radius:2px; background:var(--surface2); overflow:hidden;">
        <div style="height:100%; border-radius:2px; width:${pct}%; background:${barColor}; transition:width 0.3s ease;"></div>
      </div>
      <div style="font-family:var(--mono); font-size:9px; color:var(--text3);">${err != null ? err.toFixed(3) : '—'}</div>
    `;
    
    grid.appendChild(div);
  });
}
window.renderQualityGrid = renderQualityGrid;
