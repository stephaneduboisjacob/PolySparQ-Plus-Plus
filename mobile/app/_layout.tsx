import { useEffect } from 'react';
import { Stack } from 'expo-router';
import { StatusBar } from 'expo-status-bar';
import { QueryClient, QueryClientProvider } from '@tanstack/react-query';
import { useAuthStore } from '../src/state/AuthContext';
import { colors } from '../src/utils/theme';

const queryClient = new QueryClient({
  defaultOptions: {
    queries: {
      retry: 2,
      staleTime: 2000,
    },
  },
});

export default function RootLayout() {
  const loadStored = useAuthStore((s) => s.loadStored);

  useEffect(() => {
    loadStored();
  }, [loadStored]);

  return (
    <QueryClientProvider client={queryClient}>
      <StatusBar style="light" />
      <Stack
        screenOptions={{
          headerShown: false,
          contentStyle: { backgroundColor: colors.bg },
          animation: 'fade',
        }}
      />
    </QueryClientProvider>
  );
}
