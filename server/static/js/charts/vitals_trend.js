import { state } from '../state.js';

let chart = null;

export function initVitalsChart() {
  const container = document.getElementById('vitalsChartContainer');
  if (!container || typeof echarts === 'undefined') return;

  container.innerHTML = '';
  chart = echarts.init(container);
  
  const option = {
    grid: { top: 10, bottom: 20, left: 30, right: 30 },
    tooltip: { trigger: 'axis', className: 'echarts-tooltip' },
    xAxis: {
      type: 'category',
      boundaryGap: false,
      data: Array(60).fill(''),
      axisLine: { lineStyle: { color: 'var(--border2)' } },
      axisLabel: { show: false }
    },
    yAxis: [
      {
        type: 'value',
        min: 0, max: 200,
        splitLine: { lineStyle: { color: 'var(--border)', type: 'dashed' } },
        axisLabel: { color: 'var(--danger)', fontSize: 10, fontFamily: 'var(--mono)' }
      },
      {
        type: 'value',
        min: 80, max: 100,
        position: 'right',
        splitLine: { show: false },
        axisLabel: { color: 'var(--accent2)', fontSize: 10, fontFamily: 'var(--mono)' }
      }
    ],
    series: [
      {
        name: 'HR',
        type: 'line',
        smooth: true,
        symbol: 'none',
        lineStyle: { color: 'var(--danger)', width: 2 },
        data: Array(60).fill(null),
        markLine: {
          symbol: ['none', 'none'],
          label: { show: false },
          lineStyle: { type: 'solid', color: 'rgba(255, 77, 77, 0.3)' },
          data: [{ yAxis: 60 }, { yAxis: 100 }]
        },
        markArea: {
          itemStyle: { color: 'rgba(255, 77, 77, 0.05)' },
          data: [
            [{ yAxis: 0 }, { yAxis: 60 }],
            [{ yAxis: 100 }, { yAxis: 200 }]
          ]
        }
      },
      {
        name: 'SpO2',
        type: 'line',
        yAxisIndex: 1,
        smooth: true,
        symbol: 'none',
        lineStyle: { color: 'var(--accent2)', width: 2 },
        data: Array(60).fill(null),
        markLine: {
          symbol: ['none', 'none'],
          label: { show: false },
          lineStyle: { type: 'solid', color: 'rgba(0, 153, 255, 0.3)' },
          data: [{ yAxis: 95 }]
        },
        markArea: {
          itemStyle: { color: 'rgba(0, 153, 255, 0.05)' },
          data: [
            [{ yAxis: 80 }, { yAxis: 95 }]
          ]
        }
      }
    ],
    animation: false
  };

  chart.setOption(option);
  window.addEventListener('resize', () => chart && chart.resize());
}

export function updateVitalsBuffer(nodeId, data) {
  if (state.selectedNode !== nodeId || !chart) return;
  
  if (!state.vitalsBuffer.has(nodeId)) {
    state.vitalsBuffer.set(nodeId, Array(60).fill(null));
  }
  
  const buffer = state.vitalsBuffer.get(nodeId);
  buffer.shift();
  
  const hrVal = data.hr > 0 ? data.hr : null;
  const spo2Val = data.spo2 > 0 ? data.spo2 : null;
  
  buffer.push({ hr: hrVal, spo2: spo2Val });
  
  chart.setOption({
    series: [
      { data: buffer.map(b => b ? b.hr : null) },
      { data: buffer.map(b => b ? b.spo2 : null) }
    ]
  });
  
  // Update header text
  const currHr = document.getElementById('curr-hr');
  if (currHr) currHr.textContent = hrVal ? `${Math.round(hrVal)} bpm` : '-- bpm';
  
  const currSpo2 = document.getElementById('curr-spo2');
  if (currSpo2) currSpo2.textContent = spo2Val ? `${spo2Val.toFixed(1)} %` : '-- %';
}
window.updateVitalsBuffer = updateVitalsBuffer;
