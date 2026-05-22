import { state } from './state.js';
import { fetchStatus, fetchMetrics, fetchDB } from './api.js';
import { connectWS, closeWS } from './ws.js';
import { selectSignal, selectNode } from './ui.js';
import { setChartMode } from './chart.js';

export function init() {
  state.apiBase = document.getElementById('apiBase').value.replace(/\/$/, '');
  closeWS();
  fetchAll();
  connectWS();
  
  // Clear previous interval if any (for re-init)
  if (state.fetchInterval) clearInterval(state.fetchInterval);
  state.fetchInterval = setInterval(fetchAll, 15000);
}

export async function fetchAll() {
  await Promise.all([fetchStatus(), fetchMetrics(), fetchDB()]);
}

// Expose necessary functions to the global window object
// so that inline HTML handlers (onclick="...") continue to work.
window.init = init;
window.selectSignal = selectSignal;
window.setChartMode = setChartMode;
window.selectNode = selectNode;

document.addEventListener('DOMContentLoaded', () => {
  init();
});
