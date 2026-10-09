import { create } from 'zustand';
import * as SecureStore from 'expo-secure-store';
import { DEFAULT_SERVER_URL } from '../utils/constants';
import type { WhoAmI } from '../api/types';

interface Credentials {
  username: string;
  password: string;
}

interface AuthState {
  credentials: Credentials | null;
  user: WhoAmI | null;
  serverUrl: string;
  isLoading: boolean;
  setCredentials: (creds: Credentials | null) => void;
  setUser: (user: WhoAmI | null) => void;
  setServerUrl: (url: string) => void;
  logout: () => void;
  loadStored: () => Promise<void>;
  persist: () => Promise<void>;
}

export const useAuthStore = create<AuthState>((set, get) => ({
  credentials: null,
  user: null,
  serverUrl: DEFAULT_SERVER_URL,
  isLoading: true,

  setCredentials: (creds) => set({ credentials: creds }),
  setUser: (user) => set({ user }),
  setServerUrl: (url) => set({ serverUrl: url }),

  logout: async () => {
    set({ credentials: null, user: null });
    await SecureStore.deleteItemAsync('polypulse_credentials');
    await SecureStore.deleteItemAsync('polypulse_server_url');
  },

  loadStored: async () => {
    try {
      const raw = await SecureStore.getItemAsync('polypulse_credentials');
      const url = await SecureStore.getItemAsync('polypulse_server_url');
      if (raw) {
        const creds = JSON.parse(raw) as Credentials;
        set({
          credentials: creds,
          serverUrl: url || DEFAULT_SERVER_URL,
          isLoading: false,
        });
      } else {
        set({ isLoading: false });
      }
    } catch {
      set({ isLoading: false });
    }
  },

  persist: async () => {
    const { credentials, serverUrl } = get();
    if (credentials) {
      await SecureStore.setItemAsync('polypulse_credentials', JSON.stringify(credentials));
      await SecureStore.setItemAsync('polypulse_server_url', serverUrl);
    }
  },
}));
