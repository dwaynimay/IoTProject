import { state } from './state.js';
import { fetchNodeWindows, fetchEvents } from './api.js';

// ═══════════════════════════════════════════════════════
// NODE LIST
// ═══════════════════════════════════════════════════════
export function renderNodes(nodes) {
  const list = document.getElementById('nodeList');
  list.innerHTML = '';
  nodes.forEach(n => {
    const lqRate = n.total_windows > 0 ? n.low_quality_count / n.total_windows : 0;
    const dotCls = lqRate > 0.3 ? 'q-danger' : lqRate > 0.1 ? 'q-warn' : 'q-ok';
    const ago = n.last_seen_ago_s != null ? n.last_seen_ago_s.toFixed(0) + 's ago' : 'never';
    const div = document.createElement('div');
    div.className = 'node-card' + (state.selectedNode == n.node_id ? ' active' : '');
    div.onclick = () => window.selectNode(n.node_id);
    div.innerHTML = `
      <div class="node-id"><span class="q-dot ${dotCls}"></span> Node ${n.node_id}</div>
      <div class="node-meta">
        <span>${n.total_windows} win</span>
        <span>HR ${n.last_hr > 0 ? n.last_hr : '—'}</span>
        <span>SpO₂ ${n.last_spo2 > 0 ? n.last_spo2.toFixed(1)+'%' : '—'}</span>
      </div>
      <div style="font-size:10px;color:var(--text3);font-family:var(--mono);margin-top:3px;">${ago}</div>`;
    list.appendChild(div);

    if (!state.selectedNode && nodes.length > 0) window.selectNode(nodes[0].node_id);
  });
}

export async function selectNode(nodeId) {
  state.selectedNode = nodeId;
  document.querySelectorAll('.node-card').forEach(c => c.classList.remove('active'));
  document.querySelectorAll('.node-card').forEach(c => {
    if (c.querySelector('.node-id').textContent.includes('Node ' + nodeId))
      c.classList.add('active');
  });
  document.getElementById('chartNodeBadge').textContent = nodeId;
  await fetchNodeWindows(nodeId, state.selectedSignal);
  await fetchEvents(nodeId);
}

// ═══════════════════════════════════════════════════════
// SIGNAL PICKER
// ═══════════════════════════════════════════════════════
export function selectSignal(sig, btn) {
  state.selectedSignal = sig;
  document.querySelectorAll('.sig-btn').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  document.getElementById('chartSigBadge').textContent = sig;
  if (state.selectedNode) fetchNodeWindows(state.selectedNode, sig);
}

// ═══════════════════════════════════════════════════════
// QUALITY GRID
// ═══════════════════════════════════════════════════════
export function renderQuality(signals) {
  const grid = document.getElementById('qualityGrid');
  grid.innerHTML = '';
  const sigs = ['ax','ay','az','gx','gy','gz','ir'];
  sigs.forEach(sig => {
    const m = signals[sig];
    const err = m ? m.rel_error : null;
    const flag = m ? m.flag : null;
    const pct = err != null ? Math.max(0, Math.min(100, (1 - err) * 100)) : 0;
    const barColor = flag === 'CRITICAL' ? 'var(--danger)' :
                     flag === 'LOW_QUALITY' ? 'var(--warn)' :
                     err != null ? 'var(--ok)' : 'var(--border2)';
    const div = document.createElement('div');
    div.className = 'sig-quality';
    div.innerHTML = `
      <div class="sig-quality-name">${sig}</div>
      <div class="sig-quality-bar">
        <div class="sig-quality-fill" style="width:${pct.toFixed(1)}%;background:${barColor}"></div>
      </div>
      <div class="sig-quality-val">${err != null ? err.toFixed(3) : '—'}</div>`;
    grid.appendChild(div);
  });
}

export function setWSStatus(status) {
  const el = document.getElementById('wsStatus');
  el.className = 'ws-badge ' + status;
  el.querySelector('span').textContent = status;
}

// ═══════════════════════════════════════════════════════
// HANDLE WINDOW DATA
// ═══════════════════════════════════════════════════════
export function handleWindow(d) {
  if (state.selectedNode && d.node_id !== state.selectedNode) return;

  state.windowCount++;
  document.getElementById('winCount').textContent = state.windowCount + ' win';

  // Stat cards
  if (d.hr > 0) {
    setText('val-hr', d.hr + '<span class="stat-unit">bpm</span>');
    flash('card-hr');
  }
  if (d.spo2 > 0) {
    setText('val-spo2', d.spo2.toFixed(1) + '<span class="stat-unit">%</span>');
    flash('card-spo2');
  }
  setText('val-finger', d.finger ? '✓ terdeteksi' : '✗ tidak ada');
  document.getElementById('val-finger').style.color = d.finger ? 'var(--ok)' : 'var(--text3)';
  document.getElementById('sub-finger').textContent = 'Win #' + d.window_num;
  flash('card-finger');

  const q = d.quality;
  if (q && q.avg_rel_error != null) {
    const errPct = (q.avg_rel_error * 100).toFixed(2);
    const col = q.any_critical ? 'var(--danger)' : q.any_low_quality ? 'var(--warn)' : 'var(--ok)';
    document.getElementById('val-quality').innerHTML = `<span style="color:${col}">${q.avg_rel_error.toFixed(4)}</span>`;
    document.getElementById('sub-quality').textContent = q.any_critical ? '⚠ CRITICAL' : q.any_low_quality ? '⚠ LOW_QUALITY' : '✓ OK';
    document.getElementById('sub-quality').style.color = col;
    flash('card-quality');
    renderQuality(q.signals || {});
  }

  // Append to window stream
  appendWindow(d);

  // Refresh chart dari REST jika sinyal sesuai
  if (d.window_num % 3 === 0 && state.selectedNode) {
    fetchNodeWindows(state.selectedNode, state.selectedSignal);
  }
}

export function handleSnapshot(d) {
  if (!d.nodes || !d.nodes.length) return;
  renderNodes(d.nodes);
  document.getElementById('nodeCount').textContent = d.nodes.length;
}

export function appendWindow(d) {
  const wrap = document.getElementById('windowStream');
  const first = wrap.querySelector('div[style]');
  if (first && first.textContent.includes('menunggu')) first.remove();

  const q = d.quality || {};
  const err = q.avg_rel_error != null ? q.avg_rel_error.toFixed(4) : '—';
  const col = q.any_critical ? 'var(--danger)' : q.any_low_quality ? 'var(--warn)' : 'var(--ok)';

  const div = document.createElement('div');
  div.className = 'window-item';
  div.innerHTML = `
    <div>
      <div class="window-num">#${d.window_num} · node ${d.node_id}</div>
      <div class="window-meta-row">
        <span>HR ${d.hr > 0 ? d.hr : '—'}</span>
        <span>${d.elapsed_ms}ms</span>
        <span>${d.finger ? 'finger ✓' : 'no finger'}</span>
      </div>
    </div>
    <div class="window-err" style="color:${col}">${err}</div>`;
  wrap.insertBefore(div, wrap.firstChild);

  while (wrap.children.length > 40) wrap.removeChild(wrap.lastChild);
}

// ═══════════════════════════════════════════════════════
// EVENT LOG
// ═══════════════════════════════════════════════════════
export function appendEvent(e, prepend=true) {
  const log = document.getElementById('eventLog');
  const first = log.querySelector('div[style]');
  if (first && (first.textContent.includes('menunggu') || first.textContent.includes('tidak ada')))
    first.remove();

  state.eventCount++;
  document.getElementById('evCount').textContent = state.eventCount;

  const ts = new Date(e.ts_ms || Date.now()).toLocaleTimeString('id-ID');
  const div = document.createElement('div');
  div.className = 'event-item';
  div.innerHTML = `
    <span class="event-type ${e.event_type}">${e.event_type}</span>
    <div class="event-detail" title="${e.detail || ''}">${e.detail || '—'}</div>
    <div class="event-meta">
      <span>node ${e.node_id}</span>
      <span>${ts}</span>
    </div>`;

  if (prepend) log.insertBefore(div, log.firstChild);
  else         log.appendChild(div);

  while (log.children.length > 80) log.removeChild(log.lastChild);
}

// ═══════════════════════════════════════════════════════
// UTILS
// ═══════════════════════════════════════════════════════
export function setText(id, html) {
  const el = document.getElementById(id);
  if (el) el.innerHTML = html;
}

export function flash(id) {
  const el = document.getElementById(id);
  if (!el) return;
  el.classList.remove('updated');
  void el.offsetWidth;
  el.classList.add('updated');
  setTimeout(() => el.classList.remove('updated'), 1100);
}

export function toast(msg, type='') {
  const wrap = document.getElementById('toastWrap');
  const div = document.createElement('div');
  div.className = 'toast ' + type;
  div.textContent = msg;
  wrap.appendChild(div);
  setTimeout(() => div.remove(), 3100);
}
