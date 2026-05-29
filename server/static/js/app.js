import { state } from './state.js';
import { fetchStatus, fetchMetrics, fetchDB, fetchMLStatus } from './api.js';
import { connectWS, closeWS } from './ws.js';
import { renderNodeList, selectNode } from './nodes.js';

export async function init() {
  // Reset UI
  state.alertCount = 0;
  
  closeWS();
  
  // Initial fetch for ML labels/metadata
  try {
    await fetchMLStatus();
  } catch (err) {
    console.error('Failed to fetch ML status:', err);
  }
  
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
    if (status.server_uptime_s !== undefined && !state.server_start_time_offset) {
      state.server_start_time_offset = Date.now() - (status.server_uptime_s * 1000);
    }
    renderNodeList();
  }
}

// Global hook for when a node is selected from nodes.js
window.onNodeSelect = async (nodeId) => {
  // Details are now displayed inline inside each card.
};

// Setup globals for inline handlers if any
window.init = init;

document.addEventListener('DOMContentLoaded', () => {
  init();
});
