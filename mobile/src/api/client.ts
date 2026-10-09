import { useAuthStore } from '../state/AuthContext';

export class ApiError extends Error {
  status: number;
  details?: unknown;

  constructor(status: number, message: string, details?: unknown) {
    super(message);
    this.name = 'ApiError';
    this.status = status;
    this.details = details;
  }
}

function getHeaders(): Record<string, string> {
  const { credentials } = useAuthStore.getState();
  const headers: Record<string, string> = { Accept: 'application/json' };
  if (credentials) {
    const encoded = btoa(`${credentials.username}:${credentials.password}`);
    headers.Authorization = `Basic ${encoded}`;
  }
  return headers;
}

function getUrl(path: string, params?: Record<string, string>): string {
  const base = useAuthStore.getState().serverUrl.replace(/\/+$/, '') + '/';
  const url = new URL(path.replace(/^\//, ''), base);
  Object.entries(params ?? {}).forEach(([key, value]) => url.searchParams.set(key, value));
  return url.toString();
}

async function readResponse<T>(res: Response): Promise<T> {
  const text = await res.text();
  let payload: any = null;
  if (text) {
    try {
      payload = JSON.parse(text);
    } catch {
      payload = text;
    }
  }

  if (res.status === 401) {
    await useAuthStore.getState().logout();
    throw new ApiError(401, 'Session expired. Sign in again.', payload);
  }
  if (!res.ok) {
    const message = payload?.error || payload?.message || `Request failed (${res.status})`;
    throw new ApiError(res.status, String(message), payload);
  }
  if (payload?.error) throw new ApiError(res.status, String(payload.error), payload);
  // Shared wallet writes can succeed atomically while one or more running-bot
  // restarts fail. Preserve the structured partial result so Wallet can report
  // exactly which services need attention instead of reducing it to a generic
  // operation error.
  if (payload?.ok === false && Array.isArray(payload.updated_instances) &&
      Array.isArray(payload.restart_failures) && payload.modes_preserved === true) {
    return payload as T;
  }
  if (payload && typeof payload === 'object' && payload.ok === false) {
    const resultErrors = Array.isArray(payload.results)
      ? payload.results.map((item: any) => item?.error).filter(Boolean).join(', ')
      : '';
    const message = payload.error || payload.stderr || resultErrors || 'Operation failed';
    throw new ApiError(res.status, String(message), payload);
  }
  return payload as T;
}

export async function apiGet<T>(path: string, params?: Record<string, string>): Promise<T> {
  const res = await fetch(getUrl(path, params), { headers: getHeaders() });
  return readResponse<T>(res);
}

export async function apiPost<T>(path: string, body?: unknown): Promise<T> {
  const res = await fetch(getUrl(path), {
    method: 'POST',
    headers: { ...getHeaders(), 'Content-Type': 'application/json' },
    body: body === undefined ? undefined : JSON.stringify(body),
  });
  return readResponse<T>(res);
}
