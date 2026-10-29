const API_BASE = '/api';

async function request(endpoint, options = {}) {
  const url = `${API_BASE}${endpoint}`;
  const config = {
    headers: { 'Content-Type': 'application/json' },
    ...options,
  };
  if (options.body && typeof options.body === 'object') {
    config.body = JSON.stringify(options.body);
  }
  const res = await fetch(url, config);
  const data = await res.json();
  if (!res.ok) throw new Error(data.error || `HTTP ${res.status}`);
  return data;
}

export const api = {
  getDownloads: () => request('/downloads'),
  addDownload: (url, filename) => request('/downloads', { method: 'POST', body: { url, filename } }),
  pauseDownload: (id) => request(`/downloads/${id}/pause`, { method: 'POST' }),
  resumeDownload: (id) => request(`/downloads/${id}/resume`, { method: 'POST' }),
  cancelDownload: (id) => request(`/downloads/${id}/cancel`, { method: 'POST' }),
  retryDownload: (id) => request(`/downloads/${id}/retry`, { method: 'POST' }),
  removeDownload: (id) => request(`/downloads/${id}`, { method: 'DELETE' }),
  getStats: () => request('/stats'),
  getHistory: () => request('/history'),
  clearHistory: () => request('/history', { method: 'DELETE' }),
  getSettings: () => request('/settings'),
  updateSettings: (settings) => request('/settings', { method: 'PUT', body: settings }),
  openFile: (id) => request(`/downloads/${id}/open`, { method: 'POST' }),
  openDownloadsFolder: () => request('/open-downloads', { method: 'POST' }),
};
