export const state = {
  apiBase: 'http://localhost:8000',
  wsStream: null,
  wsEvents: null,
  chart: null,
  chartMode: 'line',
  chartData: [],
  selectedNode: null,
  selectedSignal: 'ax',
  windowCount: 0,
  eventCount: 0
};

export const SIG_COLORS = {
  ax: '#0099ff', ay: '#00d4aa', az: '#7c6aff',
  gx: '#f5a623', gy: '#ff6b6b', gz: '#ff9f43', ir: '#ff6bc6'
};

export const SIG_UNITS = {
  ax:'m/s²', ay:'m/s²', az:'m/s²',
  gx:'°/s',  gy:'°/s',  gz:'°/s',  ir:'ADC'
};
