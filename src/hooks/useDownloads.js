import { useState, useEffect, useCallback } from 'react';
import { api } from '../api/client';

export function useDownloads(interval = 1000) {
  const [downloads, setDownloads] = useState([]);
  const [stats, setStats] = useState({ total: 0, active: 0, completed: 0, failed: 0 });
  const [error, setError] = useState(null);

  const refresh = useCallback(async () => {
    try {
      const [dl, st] = await Promise.all([api.getDownloads(), api.getStats()]);
      setDownloads(dl);
      setStats(st);
      setError(null);
    } catch (err) {
      setError('Cannot connect to backend. Is download_manager.exe running?');
    }
  }, []);

  useEffect(() => {
    refresh();
    const id = setInterval(refresh, interval);
    return () => clearInterval(id);
  }, [refresh, interval]);

  return { downloads, stats, error, refresh };
}

export function useHistory() {
  const [history, setHistory] = useState([]);
  const [loading, setLoading] = useState(true);

  const refresh = useCallback(async () => {
    try {
      setLoading(true);
      const data = await api.getHistory();
      setHistory(data);
    } catch { /* ignore */ }
    finally { setLoading(false); }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);
  return { history, loading, refresh };
}

export function useSettings() {
  const [settings, setSettings] = useState({ download_dir: '', max_concurrent: 3 });
  const [loading, setLoading] = useState(true);

  const refresh = useCallback(async () => {
    try {
      setLoading(true);
      const data = await api.getSettings();
      setSettings(data);
    } catch { /* ignore */ }
    finally { setLoading(false); }
  }, []);

  useEffect(() => { refresh(); }, [refresh]);
  return { settings, loading, refresh, setSettings };
}
