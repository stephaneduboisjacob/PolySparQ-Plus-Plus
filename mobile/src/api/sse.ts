import { useEffect, useRef } from 'react';
import { AppState as RNAppState } from 'react-native';
import EventSource from 'react-native-sse';
import { useAuthStore } from '../state/AuthContext';
import { useAppStore } from '../state/store';
import type { SSEFrame } from './types';

const RECONNECT_BASE_MS = 2000;
const RECONNECT_MAX_MS = 30000;

export function useSSE() {
  const credentials = useAuthStore((s) => s.credentials);
  const serverUrl = useAuthStore((s) => s.serverUrl);
  const applySseFrame = useAppStore((s) => s.applySseFrame);
  const setConnectionStatus = useAppStore((s) => s.setConnectionStatus);
  const esRef = useRef<EventSource | null>(null);
  const retriesRef = useRef(0);
  const timerRef = useRef<ReturnType<typeof setTimeout> | null>(null);

  useEffect(() => {
    if (!credentials || !serverUrl) return;

    const encoded = btoa(`${credentials.username}:${credentials.password}`);

    function connect() {
      esRef.current?.close();

      const url = `${serverUrl.replace(/\/+$/, '')}/api/stream`;
      const es = new EventSource(url, {
        headers: { Authorization: `Basic ${encoded}` },
      });
      esRef.current = es;

      es.addEventListener('open', () => {
        retriesRef.current = 0;
        setConnectionStatus('connected');
      });

      es.addEventListener('message', (event: any) => {
        try {
          const data: SSEFrame = JSON.parse(event.data);
          applySseFrame(data);
        } catch {
          // ignore parse errors
        }
      });

      es.addEventListener('error', () => {
        if (timerRef.current) clearTimeout(timerRef.current);
        setConnectionStatus('reconnecting');
        es.close();
        scheduleReconnect();
      });
    }

    function scheduleReconnect() {
      const delay = Math.min(
        RECONNECT_BASE_MS * Math.pow(2, retriesRef.current),
        RECONNECT_MAX_MS,
      );
      retriesRef.current++;
      timerRef.current = setTimeout(connect, delay);
    }

    connect();

    const subscription = RNAppState.addEventListener('change', (state) => {
      if (state === 'active') {
        if (!esRef.current || (esRef.current as any).readyState === 2) {
          retriesRef.current = 0;
          connect();
        }
      } else if (state === 'background') {
        esRef.current?.close();
        esRef.current = null;
        if (timerRef.current) clearTimeout(timerRef.current);
        setConnectionStatus('offline');
      }
    });

    return () => {
      esRef.current?.close();
      if (timerRef.current) clearTimeout(timerRef.current);
      subscription.remove();
      setConnectionStatus('offline');
    };
  }, [credentials, serverUrl, applySseFrame, setConnectionStatus]);
}
