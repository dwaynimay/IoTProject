export const state = {
  apiBase: 'http://localhost:8000',
  wsStream: null,
  wsEvents: null,
  
  // UI State
  selectedNode: null,
  selectedSignal: 'smv', // Default to SMV
  isPaused: false,
  
  // Server Stats
  windowCount: 0,
  eventCount: 0,
  
  // Data Store
  nodes: new Map(), // node_id -> { stats, last_window, recent_events }
  ekgBuffer: new Map(), // node_id -> { smv: [], ir: [] } (max 300)
  vitalsBuffer: new Map(), // node_id -> [{ts, hr, spo2}] (max 60)
  
  // ML State
  mlModels: {},
  mlLabels: new Set(),
  mlLabelColors: new Map(),
  
  // Alert State
  alerts: [], // [{ id, type, message, ts }]
  alertCount: 0
};

export const SIG_COLORS = {
  ax: '#0099ff', ay: '#00d4aa', az: '#7c6aff',
  gx: '#f5a623', gy: '#ff6b6b', gz: '#ff9f43', ir: '#ff6bc6',
  smv: '#00d4aa' // Main signal color
};

export const SIG_UNITS = {
  ax:'m/s²', ay:'m/s²', az:'m/s²',
  gx:'°/s',  gy:'°/s',  gz:'°/s',  ir:'ADC', smv:'m/s²'
};

const DANGER_KEYWORDS = ['jatuh', 'fall', 'critical', 'tachycardia', 'low_spo2'];
const WARN_KEYWORDS   = ['sedang', 'medium', 'bradycardia', 'low'];

// Hash string to color hue
function hashCode(str) {
  let hash = 0;
  for (let i = 0; i < str.length; i++) {
    hash = str.charCodeAt(i) + ((hash << 5) - hash);
  }
  return Math.abs(hash);
}

export function getLabelColor(label) {
  if (!label) return '#8a8f9e';
  
  const lowerLabel = label.toLowerCase();
  
  // Danger checks
  if (DANGER_KEYWORDS.some(k => lowerLabel.includes(k))) return 'var(--danger)';
  
  // Warn checks
  if (WARN_KEYWORDS.some(k => lowerLabel.includes(k))) return 'var(--warn)';
  
  // OK defaults
  if (lowerLabel === 'ok' || lowerLabel === 'normal') return 'var(--ok)';
  
  // Generate consistent color based on string hash
  if (!state.mlLabelColors.has(label)) {
    const hue = hashCode(label) % 360;
    // Keep it bright (saturation 70-90, lightness 50-70)
    state.mlLabelColors.set(label, `hsl(${hue}, 80%, 65%)`);
  }
  
  return state.mlLabelColors.get(label);
}
