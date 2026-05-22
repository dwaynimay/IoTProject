import { state, SIG_COLORS, SIG_UNITS } from './state.js';

export function renderChart(data, signal) {
  const el = document.getElementById('mainChart');
  const noData = document.getElementById('chartNoData');

  if (!data || data.length === 0) {
    noData.style.display = 'flex';
    return;
  }
  noData.style.display = 'none';

  const color = SIG_COLORS[signal] || '#00d4aa';
  const labels = data.map((_, i) => i);

  if (state.chart) {
    state.chart.data.labels = labels;
    state.chart.data.datasets[0].data = data;
    state.chart.data.datasets[0].borderColor = color;
    state.chart.data.datasets[0].backgroundColor = color + '18';
    state.chart.data.datasets[0].label = signal + ' (' + (SIG_UNITS[signal]||'') + ')';
    state.chart.config.type = state.chartMode;
    state.chart.update('none');
    return;
  }

  // Chart is available globally from CDN
  state.chart = new Chart(el, {
    type: state.chartMode,
    data: {
      labels,
      datasets: [{
        label: signal + ' (' + (SIG_UNITS[signal]||'') + ')',
        data,
        borderColor: color,
        backgroundColor: color + '18',
        borderWidth: 1.5,
        pointRadius: 0,
        tension: 0.3,
        fill: true,
      }]
    },
    options: {
      responsive: true,
      maintainAspectRatio: false,
      animation: { duration: 200 },
      plugins: {
        legend: { display: false },
        tooltip: {
          mode: 'index',
          intersect: false,
          backgroundColor: '#111318',
          borderColor: 'rgba(255,255,255,0.1)',
          borderWidth: 1,
          titleColor: '#8a8f9e',
          bodyColor: '#e8eaf0',
          titleFont: { family: 'IBM Plex Mono', size: 10 },
          bodyFont:  { family: 'IBM Plex Mono', size: 11 },
        }
      },
      scales: {
        x: { display: false },
        y: {
          grid: { color: 'rgba(255,255,255,0.04)', drawBorder: false },
          ticks: {
            color: '#555a6a',
            font: { family: 'IBM Plex Mono', size: 10 },
            maxTicksLimit: 6,
          }
        }
      },
      interaction: { mode: 'index', intersect: false }
    }
  });
}

export function setChartMode(mode, btn) {
  state.chartMode = mode;
  document.querySelectorAll('.chart-mode').forEach(b => b.classList.remove('active'));
  btn.classList.add('active');
  if (state.chart) {
    state.chart.config.type = mode;
    state.chart.update();
  }
}
