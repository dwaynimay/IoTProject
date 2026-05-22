import { state } from './state.js';
import { setWSStatus, handleWindow, handleSnapshot, appendEvent } from './ui.js';

export function connectWS() {
  const wsBase = state.apiBase.replace(/^http/, 'ws');

  // Stream
  state.wsStream = new WebSocket(wsBase + '/ws/stream');
  state.wsStream.onopen  = () => setWSStatus('connected');
  state.wsStream.onerror = () => setWSStatus('error');
  state.wsStream.onclose = () => { setWSStatus('disconnected'); setTimeout(connectWS, 4000); };
  state.wsStream.onmessage = (ev) => {
    const d = JSON.parse(ev.data);
    if (d.type === 'window') handleWindow(d);
    if (d.type === 'snapshot') handleSnapshot(d);
  };

  // Events
  state.wsEvents = new WebSocket(wsBase + '/ws/events');
  state.wsEvents.onmessage = (ev) => {
    const d = JSON.parse(ev.data);
    if (d.type === 'event')    appendEvent(d, true);
    if (d.type === 'snapshot') d.events.forEach(e => appendEvent(e, false));
  };
}

export function closeWS() {
  if (state.wsStream) { state.wsStream.onclose = null; state.wsStream.close(); state.wsStream = null; }
  if (state.wsEvents) { state.wsEvents.onclose = null; state.wsEvents.close(); state.wsEvents = null; }
}
