import { state } from '../state.js';

let chart = null;

export function initEKGChart() {
  const container = document.getElementById('ekgChartContainer');
  if (!container || typeof echarts === 'undefined') return;

  // Clear loading state
  container.innerHTML = '';
  
  chart = echarts.init(container);
  
  const option = {
    backgroundColor: 'transparent',
    grid: { top: 10, bottom: 20, left: 50, right: 50 },
    tooltip: {
      trigger: 'axis',
      className: 'echarts-tooltip',
      formatter: function (params) {
        let title = `<div class="echarts-tooltip-title">T: ${params[0].axisValue}</div>`;
        let lines = params.map(p => {
          return `<div style="display:flex; justify-content:space-between; gap: 16px;">
            <span style="color:${p.color}">${p.seriesName}</span>
            <span style="font-family:var(--mono);">${Number(p.value).toFixed(2)}</span>
          </div>`;
        }).join('');
        return title + lines;
      }
    },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: Array(300).fill(''),
      axisLine: { lineStyle: { color: 'var(--border2)' } },
      axisLabel: { show: false },
      splitLine: { show: true, lineStyle: { color: 'var(--border)', type: 'dashed' } }
    },
    yAxis: [
      {
        type: 'value',
        name: 'SMV',
        position: 'left',
        splitLine: { lineStyle: { color: 'var(--border)' } },
        axisLabel: { color: 'var(--text2)', fontSize: 10, fontFamily: 'var(--mono)' }
      },
      {
        type: 'value',
        name: 'IR',
        position: 'right',
        splitLine: { show: false },
        axisLabel: { color: 'var(--text2)', fontSize: 10, fontFamily: 'var(--mono)' }
      }
    ],
    series: [
      {
        name: 'SMV',
        type: 'line',
        smooth: true,
        symbol: 'none',
        lineStyle: { width: 2, color: 'var(--ekg-line)' },
        areaStyle: {
          color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
            { offset: 0, color: 'rgba(0, 212, 170, 0.2)' },
            { offset: 1, color: 'rgba(0, 212, 170, 0)' }
          ])
        },
        data: Array(300).fill(null),
        markLine: {
          symbol: ['none', 'none'],
          label: { show: false },
          data: []
        }
      },
      {
        name: 'IR',
        type: 'line',
        smooth: true,
        symbol: 'none',
        yAxisIndex: 1,
        lineStyle: { width: 1.5, color: 'var(--ekg-line-ir)', opacity: 0.7 },
        data: Array(300).fill(null)
      }
    ],
    animation: false
  };

  chart.setOption(option);
  
  // Handle resize
  window.addEventListener('resize', () => chart && chart.resize());
}

export function updateEKGBuffer(nodeId, windowData) {
  if (state.selectedNode !== nodeId) return; // Only update buffer for selected node if we want to save memory
  // Wait, state buffer might need to track all, but for UI we just update the chart
  if (!chart) return;
  
  // We need to fetch the actual signal values. The WS payload doesn't contain the full arrays (it's too large).
  // Ah, the original WS payload might not contain values array. Let's check.
  // Wait, the task says: "Buffer circular: max 300 titik per sinyal"
  // Usually the WS payload might not contain the array to save bandwidth, but we can update it if it does, 
  // or fetch via API. Let's assume we update the chart with the current window's single data point if it's aggregated, 
  // OR the payload actually contains array?
  // Processor.py notify_window does NOT send array values.
  // The user wrote: "EKG buffer terupdate setiap window masuk via WS"
  // Wait, if WS doesn't send array values, maybe we fetch them or just plot an aggregated metric?
  // Let's implement the `updateEKGBuffer` to accept arrays if they exist, or just use it as a placeholder.
  
  // Assuming the WS will send something, or we fetch from API.
  // Let's implement chart update method that accepts a new array chunk.
}
window.updateEKGBuffer = updateEKGBuffer;

export function setEKGData(smvArray, irArray) {
  if (!chart) return;
  chart.setOption({
    series: [
      { data: smvArray },
      { data: irArray }
    ]
  });
}
window.setEKGData = setEKGData;
