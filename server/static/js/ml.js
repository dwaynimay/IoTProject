import { fetchMLStatus } from './api.js';
import { initMLPanel } from './charts/probability_bar.js';

export async function initML() {
  await fetchMLStatus();
  initMLPanel();
}
