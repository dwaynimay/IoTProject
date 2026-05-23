import { state } from './state.js';
import { fetchStatus, fetchMetrics, fetchDB } from './api.js';
import { connectWS, closeWS } from './ws.js';
import { updateCommandBar, renderQualityGrid } from './ui.js';
import { renderNodeList, selectNode } from './nodes.js';
import { initEKGChart } from './charts/ekg_strip.js';
import { initVitalsChart } from './charts/vitals_trend.js';
import { initTimelineChart, loadTimeline } from './charts/activity_timeline.js';
import { initML } from './ml.js';

export async function init() {
  // Reset UI
  document.getElementById('cmd-alerts-wrap').style.display = 'none';
  state.alertCount = 0;
  
  closeWS();
  
  // Initialize UI components
  initEKGChart();
  initVitalsChart();
  initTimelineChart();
  await initML();
  
  // Initial fetch
  await fetchAll();
  
  // Auto select first node if none selected
  if (!state.selectedNode && state.nodes.size > 0) {
    const firstNode = Array.from(state.nodes.keys())[0];
    selectNode(firstNode);
  }
  
  // Connect WebSocket
  connectWS();
  
  // Start polling
  if (state.fetchInterval) clearInterval(state.fetchInterval);
  state.fetchInterval = setInterval(fetchAll, 15000);
}

export async function fetchAll() {
  const [status, metrics, db] = await Promise.all([
    fetchStatus(),
    fetchMetrics(),
    fetchDB()
  ]);
  
  if (status) {
    renderNodeList();
    document.getElementById('nodeCount').textContent = state.nodes.size;
  }
  
  if (metrics) {
    updateCommandBar(metrics);
  }
}

// Global hook for when a node is selected from nodes.js
window.onNodeSelect = async (nodeId) => {
  // Fetch initial activity timeline
  await loadTimeline(nodeId);
  
  // The EKG and Vitals will be populated via WebSocket incrementally.
  // Real implementation might fetch historical data, but for this design
  // we just wait for new WS packets.
};

// Controls
document.getElementById('btn-stream-play').onclick = (e) => {
  state.isPaused = false;
  e.target.classList.add('active');
  document.getElementById('btn-stream-pause').classList.remove('active');
};

document.getElementById('btn-stream-pause').onclick = (e) => {
  state.isPaused = true;
  e.target.classList.add('active');
  document.getElementById('btn-stream-play').classList.remove('active');
};

// Signal picker for EKG
document.querySelectorAll('#signalSelector .toggle-btn').forEach(btn => {
  btn.onclick = () => {
    document.querySelectorAll('#signalSelector .toggle-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    state.selectedSignal = btn.dataset.sig;
    
    // In full app, this would change the series mapped in the EKG chart
  };
});

// Setup globals for inline handlers if any
window.init = init;

document.addEventListener('DOMContentLoaded', () => {
  init();
});
