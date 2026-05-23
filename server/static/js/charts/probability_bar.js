import { state, getLabelColor } from '../state.js';

const mlCharts = new Map(); // modelName -> echarts instance

export function initMLPanel() {
  const container = document.getElementById('mlPanelContainer');
  if (!container || typeof echarts === 'undefined') return;

  container.innerHTML = '';
  
  if (Object.keys(state.mlModels).length === 0) {
    container.innerHTML = '<div class="no-data">Tidak ada model aktif</div>';
    document.getElementById('mlStatusBadge').textContent = 'OFFLINE';
    document.getElementById('mlStatusBadge').className = 'badge badge-outline';
    return;
  }

  document.getElementById('mlStatusBadge').textContent = 'ACTIVE';
  document.getElementById('mlStatusBadge').className = 'badge badge-ok';

  Object.entries(state.mlModels).forEach(([modelName, info]) => {
    // Create section for model
    const section = document.createElement('div');
    section.className = 'ml-model-section';
    
    const header = document.createElement('div');
    header.className = 'ml-model-header';
    header.innerHTML = `
      <span class="ml-model-name">${modelName}</span>
      <span class="badge badge-outline">${info.labels ? info.labels.length : 0} LABELS</span>
      <span class="badge badge-outline" id="ml-badge-${modelName}" style="display:none; margin-left:auto;">SKIP</span>
    `;
    
    const barContainer = document.createElement('div');
    barContainer.className = 'ml-bar-container';
    barContainer.id = `ml-chart-${modelName}`;
    
    section.appendChild(header);
    section.appendChild(barContainer);
    container.appendChild(section);
    
    // Init chart
    const chart = echarts.init(barContainer);
    
    // Default empty data matching labels
    const labels = info.labels || [];
    const data = labels.map(l => ({ value: 0, itemStyle: { color: getLabelColor(l) } }));
    
    chart.setOption({
      grid: { top: 0, bottom: 20, left: 60, right: 20 },
      xAxis: { 
        type: 'value', 
        max: 1, 
        splitLine: { show: false },
        axisLabel: { show: false },
        axisLine: { show: false },
        axisTick: { show: false }
      },
      yAxis: { 
        type: 'category', 
        data: labels,
        inverse: true, // top to bottom
        axisLabel: { 
          color: 'var(--text2)', 
          fontFamily: 'var(--mono)', 
          fontSize: 10,
          formatter: (v) => v.length > 8 ? v.substring(0,8) + '..' : v
        },
        axisLine: { show: false },
        axisTick: { show: false }
      },
      series: [{
        type: 'bar',
        data: data,
        label: {
          show: true,
          position: 'right',
          fontFamily: 'var(--mono)',
          fontSize: 10,
          color: 'var(--text)',
          formatter: (p) => (p.value * 100).toFixed(0) + '%'
        },
        barWidth: '60%',
        itemStyle: { borderRadius: [0, 4, 4, 0] },
        animationDurationUpdate: 300,
        animationEasingUpdate: 'cubicOut'
      }]
    });
    
    mlCharts.set(modelName, { chart, labels });
  });
  
  window.addEventListener('resize', () => {
    mlCharts.forEach(c => c.chart.resize());
  });
}

export function updateMLPanel(nodeId, mlResults) {
  if (state.selectedNode !== nodeId) return;
  
  Object.entries(mlResults).forEach(([modelName, result]) => {
    if (!mlCharts.has(modelName)) return; // Might be a new model not initialized yet
    
    const chartObj = mlCharts.get(modelName);
    const badgeEl = document.getElementById(`ml-badge-${modelName}`);
    
    if (result.skipped) {
      if (badgeEl) {
        badgeEl.style.display = 'inline-block';
        badgeEl.textContent = `SKIP: ${result.skip_reason || 'Unknown'}`;
        badgeEl.className = 'badge badge-warn';
      }
      // Zero out chart
      const zeroData = chartObj.labels.map(l => ({ value: 0, itemStyle: { color: getLabelColor(l) } }));
      chartObj.chart.setOption({ series: [{ data: zeroData }] });
      return;
    }
    
    if (badgeEl) badgeEl.style.display = 'none';
    
    // Map probabilities to labels order
    const probs = result.probabilities || {};
    const topLabel = result.prediction;
    
    const data = chartObj.labels.map(l => {
      const val = probs[l] || 0;
      const isTop = l === topLabel;
      return {
        value: val,
        itemStyle: { 
          color: getLabelColor(l),
          opacity: isTop ? 1 : 0.6
        },
        label: {
          fontWeight: isTop ? 600 : 400
        }
      };
    });
    
    chartObj.chart.setOption({ series: [{ data: data }] });
  });
}
window.updateMLPanel = updateMLPanel;
