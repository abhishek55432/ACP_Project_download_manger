import { useState, useCallback } from 'react';
import { useDownloads, useHistory, useSettings } from './hooks/useDownloads';
import { api } from './api/client';
import './index.css';

/* ── Helper: format bytes ──────────────────────────────────── */
function fmtBytes(b) {
  if (b < 0) return '—';
  if (b === 0) return '0 B';
  const u = ['B', 'KB', 'MB', 'GB'];
  const i = Math.floor(Math.log(b) / Math.log(1024));
  return (b / Math.pow(1024, i)).toFixed(i > 0 ? 1 : 0) + ' ' + u[i];
}

function fmtSpeed(bps) {
  if (!bps || bps <= 0) return '—';
  return fmtBytes(bps) + '/s';
}

function fmtEta(sec) {
  if (!sec || sec <= 0) return '—';
  if (sec > 3600) return `${Math.floor(sec/3600)}h ${Math.floor((sec%3600)/60)}m`;
  if (sec > 60) return `${Math.floor(sec/60)}m ${Math.floor(sec%60)}s`;
  return `${Math.floor(sec)}s`;
}

function fmtDate(ts) {
  if (!ts) return '—';
  return new Date(ts * 1000).toLocaleString();
}

/* ── Toast System ──────────────────────────────────────────── */
let toastId = 0;
function useToasts() {
  const [toasts, setToasts] = useState([]);
  const addToast = useCallback((msg, type = 'info') => {
    const id = ++toastId;
    setToasts(t => [...t, { id, msg, type }]);
    setTimeout(() => setToasts(t => t.filter(x => x.id !== id)), 3000);
  }, []);
  return { toasts, addToast };
}

/* ── Sidebar ───────────────────────────────────────────────── */
function Sidebar({ page, setPage, open, setOpen }) {
  const items = [
    { id: 'dashboard', icon: '📊', label: 'Dashboard' },
    { id: 'downloads', icon: '⬇️', label: 'Downloads' },
    { id: 'history', icon: '📋', label: 'History' },
    { id: 'settings', icon: '⚙️', label: 'Settings' },
  ];
  return (
    <aside className={`sidebar ${open ? 'open' : ''}`}>
      <div className="sidebar-logo">
        <h1>DownloadMgr</h1>
        <span>C Backend • React UI</span>
      </div>
      <nav className="sidebar-nav">
        {items.map(i => (
          <button key={i.id} className={`nav-item ${page === i.id ? 'active' : ''}`}
            onClick={() => { setPage(i.id); setOpen(false); }}>
            <span className="nav-icon">{i.icon}</span>{i.label}
          </button>
        ))}
      </nav>
      <div style={{ padding: '16px 24px', borderTop: '1px solid var(--border)' }}>
        <div style={{ fontSize: 11, color: 'var(--text-muted)' }}>
          API: 127.0.0.1:8080<br/>Engine: C + WinINet
        </div>
      </div>
    </aside>
  );
}

/* ── Dashboard Cards ───────────────────────────────────────── */
function DashboardCards({ stats }) {
  const cards = [
    { key: 'total', icon: '📦', label: 'Total Downloads', val: stats.total, cls: 'total' },
    { key: 'active', icon: '🔄', label: 'Active', val: stats.active, cls: 'active' },
    { key: 'completed', icon: '✅', label: 'Completed', val: stats.completed, cls: 'completed' },
    { key: 'failed', icon: '❌', label: 'Failed', val: stats.failed, cls: 'failed' },
  ];
  return (
    <div className="stats-grid">
      {cards.map(c => (
        <div key={c.key} className={`stat-card ${c.cls}`}>
          <div className="stat-icon">{c.icon}</div>
          <div className="stat-value">{c.val}</div>
          <div className="stat-label">{c.label}</div>
        </div>
      ))}
    </div>
  );
}

/* ── Add Download Form ─────────────────────────────────────── */
function AddDownloadForm({ onAdd }) {
  const [url, setUrl] = useState('');
  const [loading, setLoading] = useState(false);

  const handleSubmit = async (e) => {
    e.preventDefault();
    if (!url.trim()) return;
    setLoading(true);
    try {
      await onAdd(url.trim());
      setUrl('');
    } finally {
      setLoading(false);
    }
  };

  return (
    <form className="add-form" onSubmit={handleSubmit}>
      <div className="form-group">
        <label htmlFor="url-input">Download URL</label>
        <input id="url-input" className="form-input" type="url" required
          placeholder="https://example.com/file.zip"
          value={url} onChange={e => setUrl(e.target.value)} disabled={loading} />
      </div>
      <button className="btn btn-primary" type="submit" disabled={loading}>
        {loading ? <span className="spinner" /> : '⬇️'} Add Download
      </button>
    </form>
  );
}

/* ── Progress Component ────────────────────────────────────── */
function ProgressBar({ dl }) {
  const pct = Math.min(100, Math.max(0, dl.progress || 0));
  const cls = dl.status === 'completed' ? 'completed'
    : dl.status === 'failed' ? 'failed'
    : dl.status === 'paused' ? 'paused' : '';
  return (
    <div className="progress-container">
      <div className="progress-bar-bg">
        <div className={`progress-bar-fill ${cls}`} style={{ width: `${pct}%` }} />
      </div>
      <div className="progress-text">
        {dl.status === 'downloading' && <>{pct.toFixed(1)}% • {fmtSpeed(dl.speed)} • ETA {fmtEta(dl.eta)}</>}
        {dl.status === 'completed' && <>100% • {fmtBytes(dl.bytes_downloaded)}</>}
        {dl.status === 'paused' && <>{pct.toFixed(1)}% • Paused</>}
        {dl.status === 'failed' && <>{dl.error || 'Failed'}</>}
        {dl.status === 'queued' && <>Waiting...</>}
        {dl.status === 'cancelled' && <>Cancelled</>}
      </div>
    </div>
  );
}

/* ── Download Table ────────────────────────────────────────── */
function DownloadTable({ downloads, onAction }) {
  const [search, setSearch] = useState('');
  const [filter, setFilter] = useState('all');
  const [sortBy, setSortBy] = useState('id');
  const [sortDir, setSortDir] = useState(-1);

  let items = [...downloads];
  if (search) items = items.filter(d =>
    d.filename.toLowerCase().includes(search.toLowerCase()) ||
    d.url.toLowerCase().includes(search.toLowerCase()));
  if (filter !== 'all') items = items.filter(d => d.status === filter);
  items.sort((a, b) => {
    if (sortBy === 'filename') return sortDir * a.filename.localeCompare(b.filename);
    if (sortBy === 'progress') return sortDir * ((a.progress || 0) - (b.progress || 0));
    if (sortBy === 'status') return sortDir * a.status.localeCompare(b.status);
    return sortDir * (a.id - b.id);
  });

  const handleSort = (col) => {
    if (sortBy === col) setSortDir(-sortDir);
    else { setSortBy(col); setSortDir(1); }
  };

  return (
    <div className="dl-table-container">
      <div className="dl-table-header">
        <h3>Downloads ({items.length})</h3>
        <div style={{ display: 'flex', gap: 8, alignItems: 'center', flexWrap: 'wrap' }}>
          <button className="btn btn-sm btn-secondary" onClick={() => onAction('open-folder')} title="Open backend\downloads in Windows Explorer" style={{ whiteSpace: 'nowrap', padding: '6px 12px', fontSize: 12, background: 'rgba(56, 189, 248, 0.15)', color: '#38bdf8', border: '1px solid rgba(56, 189, 248, 0.3)', cursor: 'pointer', borderRadius: 6 }}>
            📂 Open Downloads Folder
          </button>
          <input className="search-input" placeholder="🔍  Search downloads..."
            value={search} onChange={e => setSearch(e.target.value)} />
        </div>
      </div>
      <div className="filter-tabs" style={{ margin: '0 16px' }}>
        {['all', 'downloading', 'queued', 'paused', 'completed', 'failed', 'cancelled'].map(f => (
          <button key={f} className={`filter-tab ${filter === f ? 'active' : ''}`}
            onClick={() => setFilter(f)}>{f}</button>
        ))}
      </div>
      {items.length === 0 ? (
        <div className="empty-state">
          <div className="empty-icon">📭</div>
          <h4>No downloads {filter !== 'all' ? `with status "${filter}"` : 'yet'}</h4>
          <p>Enter a URL above to start downloading</p>
        </div>
      ) : (
        <table className="dl-table">
          <thead>
            <tr>
              <th onClick={() => handleSort('id')}>#</th>
              <th onClick={() => handleSort('filename')}>File</th>
              <th onClick={() => handleSort('progress')}>Progress</th>
              <th>Size</th>
              <th onClick={() => handleSort('status')}>Status</th>
              <th>Actions</th>
            </tr>
          </thead>
          <tbody>
            {items.map(dl => (
              <tr key={dl.id}>
                <td>{dl.id}</td>
                <td>
                  <div className="dl-filename" title={dl.filename}>{dl.filename}</div>
                  <div className="dl-url" title={dl.url}>{dl.url}</div>
                  {dl.filepath && (
                    <div style={{ fontSize: 11, color: '#38bdf8', marginTop: 2, display: 'flex', alignItems: 'center', gap: 4 }}>
                      📁 <span style={{ opacity: 0.9 }}>backend\{dl.filepath}</span>
                    </div>
                  )}
                </td>
                <td><ProgressBar dl={dl} /></td>
                <td style={{ whiteSpace: 'nowrap', fontSize: 12 }}>
                  {fmtBytes(dl.bytes_downloaded)}{dl.total_bytes > 0 ? ` / ${fmtBytes(dl.total_bytes)}` : ''}
                </td>
                <td>
                  <span className={`status-badge status-${dl.status}`}>
                    <span className="status-dot" />{dl.status}
                  </span>
                </td>
                <td>
                  <div className="dl-actions">
                    {dl.status === 'completed' && (
                      <button className="action-btn" onClick={() => onAction('open', dl.id)} title="Show in Windows File Explorer" style={{ color: '#38bdf8', borderColor: 'rgba(56, 189, 248, 0.4)' }}>📂 Show</button>
                    )}
                    {dl.status === 'downloading' && dl.supports_resume && (
                      <button className="action-btn pause" onClick={() => onAction('pause', dl.id)}>⏸ Pause</button>
                    )}
                    {dl.status === 'paused' && (
                      <button className="action-btn resume" onClick={() => onAction('resume', dl.id)}>▶ Resume</button>
                    )}
                    {(dl.status === 'downloading' || dl.status === 'queued') && (
                      <button className="action-btn cancel" onClick={() => onAction('cancel', dl.id)}>✕ Cancel</button>
                    )}
                    {(dl.status === 'failed' || dl.status === 'cancelled') && (
                      <button className="action-btn retry" onClick={() => onAction('retry', dl.id)}>↻ Retry</button>
                    )}
                    {(dl.status === 'completed' || dl.status === 'failed' || dl.status === 'cancelled') && (
                      <button className="action-btn" onClick={() => onAction('remove', dl.id)}>🗑</button>
                    )}
                  </div>
                </td>
              </tr>
            ))}
          </tbody>
        </table>
      )}
    </div>
  );
}

/* ── History Panel ─────────────────────────────────────────── */
function HistoryPanel() {
  const { history, loading, refresh } = useHistory();
  const handleClear = async () => {
    if (window.confirm('Clear all download history?')) {
      await api.clearHistory();
      refresh();
    }
  };
  return (
    <>
      <div className="page-header">
        <h2>📋 Download History</h2>
        <p>Past downloads are recorded here</p>
      </div>
      <div className="dl-table-container">
        <div className="dl-table-header">
          <h3>History ({history.length} entries)</h3>
          {history.length > 0 && (
            <button className="btn btn-sm btn-danger" onClick={handleClear}>Clear History</button>
          )}
        </div>
        {loading ? (
          <div className="empty-state"><span className="spinner" /></div>
        ) : history.length === 0 ? (
          <div className="empty-state">
            <div className="empty-icon">📭</div>
            <h4>No history yet</h4>
            <p>Completed downloads will appear here</p>
          </div>
        ) : (
          <table className="history-table">
            <thead><tr><th>File</th><th>Size</th><th>Status</th><th>Date</th></tr></thead>
            <tbody>
              {history.map((h, i) => (
                <tr key={i}>
                  <td>
                    <div className="dl-filename">{h.filename}</div>
                    <div className="dl-url">{h.url}</div>
                    {h.filepath && <div style={{ fontSize: 11, color: '#38bdf8', marginTop: 2 }}>📁 backend\{h.filepath}</div>}
                  </td>
                  <td>{fmtBytes(h.size)}</td>
                  <td><span className={`status-badge status-${h.status}`}>
                    <span className="status-dot" />{h.status}
                  </span></td>
                  <td style={{ fontSize: 12 }}>{fmtDate(h.completed_at || h.created_at)}</td>
                </tr>
              ))}
            </tbody>
          </table>
        )}
      </div>
    </>
  );
}

/* ── Settings Panel ────────────────────────────────────────── */
function SettingsPanel({ addToast }) {
  const { settings, loading, refresh } = useSettings();
  const [dir, setDir] = useState('');
  const [maxC, setMaxC] = useState(3);
  const [dirty, setDirty] = useState(false);

  // Sync from server
  if (!dirty && !loading && dir === '' && settings.download_dir) {
    setDir(settings.download_dir);
    setMaxC(settings.max_concurrent);
  }

  const handleSave = async () => {
    try {
      await api.updateSettings({ download_dir: dir, max_concurrent: maxC });
      addToast('Settings saved!', 'success');
      setDirty(false);
      refresh();
    } catch (e) {
      addToast(e.message, 'error');
    }
  };

  return (
    <>
      <div className="page-header">
        <h2>⚙️ Settings</h2>
        <p>Configure download behavior</p>
      </div>
      <div className="settings-panel">
        <div className="settings-row">
          <label>Download Directory</label>
          <input className="form-input" value={dir}
            onChange={e => { setDir(e.target.value); setDirty(true); }}
            placeholder="downloads" />
          <div className="hint">Relative or absolute path where files are saved</div>
        </div>
        <div className="settings-row">
          <label>Max Concurrent Downloads</label>
          <input className="form-input" type="number" min="1" max="10" value={maxC}
            onChange={e => { setMaxC(parseInt(e.target.value) || 1); setDirty(true); }}
            style={{ width: 100 }} />
          <div className="hint">Number of downloads that run simultaneously (1–10)</div>
        </div>
        <button className="btn btn-primary" onClick={handleSave} disabled={!dirty}>
          Save Settings
        </button>
      </div>
    </>
  );
}

/* ── App (Root) ────────────────────────────────────────────── */
export default function App() {
  const [page, setPage] = useState('dashboard');
  const [sidebarOpen, setSidebarOpen] = useState(false);
  const { downloads, stats, error, refresh } = useDownloads(1000);
  const { toasts, addToast } = useToasts();

  const handleAdd = async (url) => {
    try {
      const res = await api.addDownload(url);
      addToast(`Download #${res.id} added!`, 'success');
      refresh();
    } catch (e) {
      addToast(e.message, 'error');
    }
  };

  const handleAction = async (action, id) => {
    try {
      if (action === 'open-folder') {
        await api.openDownloadsFolder();
        addToast('Opened downloads folder in Windows Explorer', 'info');
        return;
      }
      if (action === 'open') {
        await api.openFile(id);
        addToast('Revealed file in Windows Explorer', 'info');
        return;
      }
      if (action === 'pause') await api.pauseDownload(id);
      else if (action === 'resume') await api.resumeDownload(id);
      else if (action === 'cancel') await api.cancelDownload(id);
      else if (action === 'retry') await api.retryDownload(id);
      else if (action === 'remove') await api.removeDownload(id);
      addToast(`${action} #${id} — OK`, 'success');
      refresh();
    } catch (e) {
      addToast(e.message, 'error');
    }
  };

  return (
    <>
      <button className="mobile-toggle" onClick={() => setSidebarOpen(!sidebarOpen)}>☰</button>
      <Sidebar page={page} setPage={setPage} open={sidebarOpen} setOpen={setSidebarOpen} />
      <main className="main-content">
        {error && <div className="error-banner">⚠️ {error}</div>}

        {page === 'dashboard' && (
          <>
            <div className="page-header">
              <h2>📊 Dashboard</h2>
              <p>Monitor your downloads in real-time</p>
            </div>
            <DashboardCards stats={stats} />
            <AddDownloadForm onAdd={handleAdd} />
            <DownloadTable downloads={downloads} onAction={handleAction} />
          </>
        )}

        {page === 'downloads' && (
          <>
            <div className="page-header">
              <h2>⬇️ Downloads</h2>
              <p>Manage active and queued downloads</p>
            </div>
            <AddDownloadForm onAdd={handleAdd} />
            <DownloadTable downloads={downloads} onAction={handleAction} />
          </>
        )}

        {page === 'history' && <HistoryPanel />}
        {page === 'settings' && <SettingsPanel addToast={addToast} />}
      </main>

      <div className="toast-container">
        {toasts.map(t => (
          <div key={t.id} className={`toast ${t.type}`}>
            {t.type === 'success' ? '✅' : t.type === 'error' ? '❌' : 'ℹ️'} {t.msg}
          </div>
        ))}
      </div>
    </>
  );
}
