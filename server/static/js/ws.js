import { state } from './state.js';
import { updateWSStatus } from './ui.js';
// We will create these modules in Fase 5
// import { updateNodeCard } from './nodes.js';
// import { updateEKGBuffer } from './charts/ekg_strip.js';
// import { updateVitalsBuffer } from './charts/vitals_trend.js';
// import { updateMLPanel } from './ml.js';
// import { triggerAlert } from './alerts.js';

let reconnectAttempts = 0;

export function connectWS() {
  if (state.wsStream || state.wsEvents) return;
  
  const wsBase = state.apiBase.replace(/^http/, 'ws');

  // Stream
  state.wsStream = new WebSocket(wsBase + '/ws/stream');
  
  state.wsStream.onopen = () => {
    reconnectAttempts = 0;
    if (window.updateWSStatus) updateWSStatus('connected');
  };
  
  state.wsStream.onerror = () => {
    if (window.updateWSStatus) updateWSStatus('error');
  };
  
  state.wsStream.onclose = () => {
    if (window.updateWSStatus) updateWSStatus('disconnected');
    state.wsStream = null;
    scheduleReconnect();
  };
  
  state.wsStream.onmessage = (ev) => {
    if (state.isPaused) return;
    
    const d = JSON.parse(ev.data);
    if (d.type === 'window') {
      state.windowCount++;
      
      // We will call the UI handlers here once they are implemented
      if (window.updateNodeCard) window.updateNodeCard(d.node_id, d);
      if (window.updateEKGBuffer) window.updateEKGBuffer(d.node_id, d);
      if (window.updateVitalsBuffer) window.updateVitalsBuffer(d.node_id, d);
      
      // Handle ML results
      if (d.ml_results && window.updateMLPanel) {
        window.updateMLPanel(d.node_id, d.ml_results);
        
        // Alert checking logic
        Object.values(d.ml_results).forEach(result => {
           if (!result.skipped && result.label) {
               const label = (result.label || '').toLowerCase();
               // check if label is critical and confidence > 0.7
               const isCritical = ['jatuh', 'fall', 'critical', 'tachycardia'].some(k => label.includes(k));
               if (isCritical && result.confidence > 0.7) {
                   if (window.triggerAlert) {
                       window.triggerAlert(d.node_id, 'CRITICAL', `ML Detected: ${result.label} (${(result.confidence*100).toFixed(0)}%)`);
                   }
               }
           }
        });
      }
      
      // Update UI command bar
      const wEl = document.getElementById('cmd-windows');
      if (wEl) wEl.textContent = state.windowCount;
    }
  };

  // Events
  state.wsEvents = new WebSocket(wsBase + '/ws/events');
  state.wsEvents.onmessage = (ev) => {
    const d = JSON.parse(ev.data);
    if (d.type === 'event') {
      state.eventCount++;
      if (window.appendEvent) window.appendEvent(d, true);
      
      if (d.event_type === 'CRITICAL' && window.triggerAlert) {
        window.triggerAlert(d.node_id, 'CRITICAL', d.detail);
      }
    }
  };
}

function scheduleReconnect() {
  closeWS();
  
  // Exponential backoff (1s, 2s, 4s, 8s, 16s, 30s max)
  let delay = Math.pow(2, reconnectAttempts) * 1000;
  if (delay > 30000) delay = 30000;
  
  reconnectAttempts++;
  console.log(`WS disconnected. Reconnecting in ${delay/1000}s...`);
  
  setTimeout(() => {
    connectWS();
  }, delay);
}

export function closeWS() {
  if (state.wsStream) {
    state.wsStream.onclose = null;
    state.wsStream.close();
    state.wsStream = null;
  }
  if (state.wsEvents) {
    state.wsEvents.onclose = null;
    state.wsEvents.close();
    state.wsEvents = null;
  }
}
