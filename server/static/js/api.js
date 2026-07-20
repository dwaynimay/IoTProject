import { state } from './state.js';
import { toast } from './ui.js';

export async function api(path) {
  try {
    const r = await fetch(state.apiBase + path);
    if (!r.ok) throw new Error(r.status);
    return await r.json();
  } catch(e) {
    console.error(`API Error: ${path}`, e);
    return null;
  }
}

export async function fetchStatus() {
  const d = await api('/api/status');
  if (!d) return null;
  
  // Populate state.nodes
  const currentIds = new Set(d.nodes.map(node => node.node_id));
  d.nodes.forEach(node => {
    state.nodes.set(node.node_id, { ...(state.nodes.get(node.node_id) || {}), ...node });
  });
  for (const nodeId of state.nodes.keys()) {
    if (!currentIds.has(nodeId)) state.nodes.delete(nodeId);
  }
  
  return d;
}

export async function fetchMLStatus() {
  const d = await api('/api/ml/status');
  if (!d) return null;
  
  state.mlModels = d.models || {};
  
  // Collect all unique labels for color mapping
  Object.values(state.mlModels).forEach(model => {
    if (model.labels) {
      model.labels.forEach(l => state.mlLabels.add(l));
    }
  });
  
  return d;
}

export async function fetchNodeActivity(nodeId, hours = 24) {
  const d = await api(`/api/nodes/${nodeId}/activity?hours=${hours}`);
  if (!d) return [];
  return d.segments || [];
}

export async function fetchNodeDetail(nodeId) {
  const d = await api(`/api/nodes/${nodeId}`);
  return d;
}

export async function fetchMetrics() {
  const d = await api('/api/metrics');
  return d;
}

export async function fetchDB() {
  const d = await api('/api/db');
  return d;
}

export async function fetchEvents(nodeId) {
  const d = await api(`/api/nodes/${nodeId}/events?n=30`);
  return d;
}

export async function fetchNodeVitalsHistory(nodeId, n = 60) {
  const d = await api(`/api/nodes/${nodeId}/windows?signal=ir&n=${n}&include_values=false`);
  return d ? d.windows : [];
}

export async function fetchNodeIMUHistory(nodeId, nWindows = 4) {
  const IMU_KEYS = ['ax', 'ay', 'az', 'gx', 'gy', 'gz'];
  const result = {
    timestamps: []
  };
  await Promise.all(IMU_KEYS.map(async sig => {
    const d = await api(`/api/nodes/${nodeId}/windows?signal=${sig}&n=${nWindows}&include_values=true`);
    if (d && d.windows) {
      // Flatten semua windows menjadi satu array kontinu
      result[sig] = d.windows.flatMap(w => w.values || []);
      // Populasi timestamps dari sinyal ax (cukup sekali saja)
      if (sig === 'ax') {
        d.windows.forEach(w => {
          const startTs = w.ts_server_ms;
          const vals = w.values || [];
          for (let i = 0; i < vals.length; i++) {
            result.timestamps.push(startTs + i * 10);
          }
        });
      }
    } else {
      result[sig] = [];
    }
  }));
  return result;
}
