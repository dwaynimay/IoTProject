import { state, getLabelColor } from '../state.js';
import { fetchNodeActivity } from '../api.js';

let chart = null;
let currentHours = 24;
let interval = null;

export function initTimelineChart() {
  const container = document.getElementById('timelineChartContainer');
  if (!container || typeof echarts === 'undefined') return;

  container.innerHTML = '';
  chart = echarts.init(container);
  
  window.addEventListener('resize', () => chart && chart.resize());
  
  // Setup toggles
  document.querySelectorAll('#timelineSection .toggle-btn').forEach(btn => {
    btn.onclick = () => {
      document.querySelectorAll('#timelineSection .toggle-btn').forEach(b => b.classList.remove('active'));
      btn.classList.add('active');
      currentHours = parseInt(btn.dataset.hours);
      if (state.selectedNode) loadTimeline(state.selectedNode);
    };
  });
}

function renderItems(params, api) {
  const categoryIndex = api.value(0);
  const start = api.coord([api.value(1), categoryIndex]);
  const end = api.coord([api.value(2), categoryIndex]);
  const height = api.size([0, 1])[1] * 0.6;

  const rectShape = echarts.graphic.clipRectByRect({
    x: start[0],
    y: start[1] - height / 2,
    width: end[0] - start[0],
    height: height
  }, {
    x: params.coordSys.x,
    y: params.coordSys.y,
    width: params.coordSys.width,
    height: params.coordSys.height
  });

  return rectShape && {
    type: 'rect',
    transition: ['shape'],
    shape: rectShape,
    style: api.style()
  };
}

export async function loadTimeline(nodeId) {
  if (!chart) return;
  
  chart.showLoading({ color: 'var(--accent)', maskColor: 'rgba(17, 19, 24, 0.8)' });
  
  const segments = await fetchNodeActivity(nodeId, currentHours);
  
  chart.hideLoading();
  
  if (!segments || segments.length === 0) {
    chart.clear();
    const c = document.getElementById('timelineChartContainer');
    c.innerHTML = '<div class="no-data">Tidak ada aktivitas terdeteksi</div>';
    chart = echarts.init(c); // reinit
    return;
  }
  
  const data = segments.map(seg => {
    return {
      value: [
        0, // Category index (only 1 row)
        seg.start_ms,
        seg.end_ms,
        seg.label,
        seg.duration_s
      ],
      itemStyle: {
        color: getLabelColor(seg.label)
      }
    };
  });
  
  const now = Date.now();
  const startTime = now - (currentHours * 3600 * 1000);
  
  const option = {
    grid: { top: 20, bottom: 20, left: 10, right: 10 },
    tooltip: {
      formatter: function (params) {
        const val = params.value;
        return `<div class="echarts-tooltip">
          <div class="echarts-tooltip-title">${val[3]}</div>
          <div>Durasi: ${val[4]}s</div>
          <div style="color:var(--text3);font-size:10px;margin-top:4px;">
            ${new Date(val[1]).toLocaleTimeString()} - ${new Date(val[2]).toLocaleTimeString()}
          </div>
        </div>`;
      }
    },
    xAxis: {
      type: 'time',
      min: startTime,
      max: now,
      splitLine: { show: true, lineStyle: { color: 'var(--border)', type: 'dashed' } },
      axisLabel: { color: 'var(--text2)', fontFamily: 'var(--mono)', fontSize: 10 },
      axisLine: { lineStyle: { color: 'var(--border2)' } }
    },
    yAxis: {
      type: 'category',
      data: ['Activity'],
      show: false
    },
    series: [{
      type: 'custom',
      renderItem: renderItems,
      itemStyle: { opacity: 0.8 },
      encode: {
        x: [1, 2],
        y: 0
      },
      data: data
    }]
  };
  
  chart.setOption(option);
  
  if (interval) clearInterval(interval);
  interval = setInterval(() => loadTimeline(nodeId), 60000);
}
