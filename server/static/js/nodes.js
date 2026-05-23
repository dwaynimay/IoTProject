import { state, getLabelColor } from './state.js';

const sparklines = new Map(); // nodeId -> echarts instance

export function renderNodeList() {
  const container = document.getElementById('nodesContainer');
  if (!container) return;

  if (state.nodes.size === 0) {
    container.innerHTML = `<div class="no-data"><div class="no-data-icon">📡</div><div>Menunggu node...</div></div>`;
    return;
  }

  container.innerHTML = '';
  
  Array.from(state.nodes.values()).sort((a,b) => a.node_id - b.node_id).forEach(node => {
    const el = document.createElement('div');
    el.className = 'node-card';
    if (state.selectedNode === node.node_id) el.classList.add('active');
    el.id = `node-card-${node.node_id}`;
    el.onclick = () => selectNode(node.node_id);
    
    // Get last data
    const hr = node.last_hr > 0 ? node.last_hr : '--';
    const spo2 = node.last_spo2 > 0 ? node.last_spo2 : '--';
    
    // Fallback label to OK if not found
    const label = "OK";
    const conf = "";
    const color = getLabelColor(label);

    el.innerHTML = `
      <div class="card-header">
        <div style="display:flex; align-items:center;">
          <span class="node-indicator online" id="ind-${node.node_id}"></span>
          <span class="node-name">Node ${node.node_id}</span>
        </div>
        <span class="node-ago" id="ago-${node.node_id}">just now</span>
      </div>
      <div class="card-activity">
        <span class="activity-label" id="act-lbl-${node.node_id}" style="color: ${color}">${label}</span>
        <span class="activity-confidence" id="act-conf-${node.node_id}">${conf}</span>
      </div>
      <div class="card-vitals">
        <span class="vital-hr" id="vit-hr-${node.node_id}">${hr}</span>
        <span class="vital-spo2" id="vit-spo2-${node.node_id}">${spo2}</span>
      </div>
      <div class="card-sparkline" id="spark-${node.node_id}"></div>
      <div class="card-footer" id="win-num-${node.node_id}">Win #${node.total_windows || 0}</div>
    `;
    
    container.appendChild(el);
    
    // Init sparkline
    setTimeout(() => initSparkline(node.node_id), 10);
  });
}

function initSparkline(nodeId) {
  const el = document.getElementById(`spark-${nodeId}`);
  if (!el || typeof echarts === 'undefined') return;
  
  if (sparklines.has(nodeId)) {
    sparklines.get(nodeId).dispose();
  }
  
  const chart = echarts.init(el);
  chart.setOption({
    grid: { top: 2, bottom: 2, left: 0, right: 0 },
    xAxis: { type: 'category', show: false, boundaryGap: false },
    yAxis: { type: 'value', show: false, min: 'dataMin', max: 'dataMax' },
    series: [{
      type: 'line',
      data: Array(30).fill(0),
      showSymbol: false,
      smooth: true,
      lineStyle: { width: 1.5, color: 'rgba(0, 212, 170, 0.8)' },
      areaStyle: {
        color: new echarts.graphic.LinearGradient(0, 0, 0, 1, [
          { offset: 0, color: 'rgba(0, 212, 170, 0.3)' },
          { offset: 1, color: 'rgba(0, 212, 170, 0)' }
        ])
      }
    }],
    animation: false
  });
  
  sparklines.set(nodeId, {
    chart,
    data: Array(30).fill(0)
  });
}

export function updateNodeCard(nodeId, data) {
  // If not rendered yet, add it
  if (!document.getElementById(`node-card-${nodeId}`)) {
    if (!state.nodes.has(nodeId)) state.nodes.set(nodeId, { node_id: nodeId });
    renderNodeList();
    return;
  }
  
  // Update texts
  const hrEl = document.getElementById(`vit-hr-${nodeId}`);
  if (hrEl) hrEl.textContent = data.hr > 0 ? data.hr : '--';
  
  const spo2El = document.getElementById(`vit-spo2-${nodeId}`);
  if (spo2El) spo2El.textContent = data.spo2 > 0 ? data.spo2.toFixed(1) : '--';
  
  const winEl = document.getElementById(`win-num-${nodeId}`);
  if (winEl) winEl.textContent = `Win #${data.window_num}`;
  
  // Update ML label if available
  if (data.ml_results) {
    // Find highest confidence prediction across all models
    let topLabel = null;
    let topConf = 0;
    
    Object.values(data.ml_results).forEach(res => {
      if (!res.skipped && res.confidence > topConf) {
        topConf = res.confidence;
        topLabel = res.prediction;
      }
    });
    
    if (topLabel) {
      const actLbl = document.getElementById(`act-lbl-${nodeId}`);
      if (actLbl) {
        actLbl.textContent = topLabel;
        actLbl.style.color = getLabelColor(topLabel);
      }
      
      const actConf = document.getElementById(`act-conf-${nodeId}`);
      if (actConf) actConf.textContent = `${(topConf*100).toFixed(1)}%`;
    }
  }
  
  // Update sparkline with SMV or IR
  if (sparklines.has(nodeId) && data.quality && data.quality.signals) {
    const sp = sparklines.get(nodeId);
    // Use rel_error as a proxy for sparkline variance if we don't have direct signal here
    // or just use hr if available.
    const val = data.hr > 0 ? data.hr : 60; 
    
    sp.data.shift();
    sp.data.push(val);
    sp.chart.setOption({ series: [{ data: sp.data }] });
  }
}

window.updateNodeCard = updateNodeCard;

export function selectNode(nodeId) {
  state.selectedNode = nodeId;
  
  // Update UI active state
  document.querySelectorAll('.node-card').forEach(el => el.classList.remove('active'));
  const card = document.getElementById(`node-card-${nodeId}`);
  if (card) card.classList.add('active');
  
  // Trigger redraws
  if (window.onNodeSelect) window.onNodeSelect(nodeId);
}

export function setNodeAlert(nodeId, isAlert) {
  const card = document.getElementById(`node-card-${nodeId}`);
  if (!card) return;
  
  if (isAlert) {
    card.classList.add('alert');
  } else {
    card.classList.remove('alert');
  }
}
window.setNodeAlert = setNodeAlert;
