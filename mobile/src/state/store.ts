import { create } from 'zustand';
import type { PositionMark, PriceUpdate, SSEFrame } from '../api/types';

interface AppState {
  selectedInstance: string | null;
  setSelectedInstance: (inst: string | null) => void;
  connectionStatus: 'connected' | 'reconnecting' | 'offline';
  setConnectionStatus: (status: AppState['connectionStatus']) => void;
  lastSseTimestamp: number;
  prices: Record<string, PriceUpdate>;
  recentFills: SSEFrame['recent_fills'];
  positionMarks: Record<string, PositionMark>;
  applySseFrame: (frame: SSEFrame) => void;
  resetLiveData: () => void;
}

export const positionMarkKey = (cid: string, side: string, instance?: string) =>
  `${cid}_${side}_${instance ?? ''}`;

export const useAppStore = create<AppState>((set) => ({
  selectedInstance: null,
  setSelectedInstance: (selectedInstance) => set({ selectedInstance }),
  connectionStatus: 'offline',
  setConnectionStatus: (connectionStatus) => set({ connectionStatus }),
  lastSseTimestamp: 0,
  prices: {},
  recentFills: [],
  positionMarks: {},
  applySseFrame: (frame) =>
    set((state) => {
      const marks = frame.positions_marks
        ? Object.fromEntries(frame.positions_marks.map((mark) => [
            positionMarkKey(mark.cid, mark.side, mark.instance),
            mark,
          ]))
        : state.positionMarks;
      return {
        prices: frame.prices ?? state.prices,
        recentFills: frame.recent_fills ?? state.recentFills,
        positionMarks: marks,
        lastSseTimestamp: frame.ts ?? Date.now(),
      };
    }),
  resetLiveData: () => set({ prices: {}, recentFills: [], positionMarks: {}, lastSseTimestamp: 0 }),
}));
