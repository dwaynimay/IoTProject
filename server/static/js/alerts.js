import { state } from './state.js';
import { setNodeAlert } from './nodes.js';

let audioCtx = null;

function playAlertTone() {
  if (!audioCtx) {
    try {
      audioCtx = new (window.AudioContext || window.webkitAudioContext)();
    } catch (e) {
      return;
    }
  }
  
  if (audioCtx.state === 'suspended') {
    audioCtx.resume();
  }
  
  const osc = audioCtx.createOscillator();
  const gain = audioCtx.createGain();
  
  osc.type = 'sine';
  osc.frequency.setValueAtTime(880, audioCtx.currentTime); // 880Hz (A5)
  
  gain.gain.setValueAtTime(0, audioCtx.currentTime);
  gain.gain.linearRampToValueAtTime(0.1, audioCtx.currentTime + 0.05); // low volume
  gain.gain.linearRampToValueAtTime(0, audioCtx.currentTime + 0.2);
  
  osc.connect(gain);
  gain.connect(audioCtx.destination);
  
  osc.start();
  osc.stop(audioCtx.currentTime + 0.25);
}

export function triggerAlert(nodeId, type, message) {
  state.alertCount++;
  
  const alertWrap = document.getElementById('cmd-alerts-wrap');
  const alertCountEl = document.getElementById('cmd-alerts');
  if (alertWrap && alertCountEl) {
    alertWrap.style.display = 'flex';
    alertCountEl.textContent = state.alertCount;
  }
  
  // Create banner
  const banner = document.getElementById('alertBanner');
  if (!banner) return;
  
  const item = document.createElement('div');
  item.className = 'alert-item glow-danger';
  
  const now = new Date();
  const timeStr = now.toLocaleTimeString();
  
  item.innerHTML = `
    <div class="alert-header">
      <span class="alert-title">CRITICAL ALERT - NODE ${nodeId}</span>
      <span class="alert-time">${timeStr}</span>
    </div>
    <div class="alert-body">${message}</div>
  `;
  
  // Keep max 5
  if (banner.children.length >= 5) {
    const old = banner.lastElementChild;
    old.classList.add('closing');
    setTimeout(() => { if(old.parentNode) old.parentNode.removeChild(old); }, 300);
  }
  
  banner.prepend(item);
  
  setNodeAlert(nodeId, true);
  playAlertTone();
  
  // Auto dismiss after 8s
  setTimeout(() => {
    item.classList.add('closing');
    setTimeout(() => {
      if(item.parentNode) item.parentNode.removeChild(item);
      setNodeAlert(nodeId, false);
    }, 300);
  }, 8000);
}
window.triggerAlert = triggerAlert;
