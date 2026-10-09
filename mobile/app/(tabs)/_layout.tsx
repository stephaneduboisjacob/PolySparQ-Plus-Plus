import { useEffect } from 'react';
import { Redirect, Tabs } from 'expo-router';
import { Ionicons } from '@expo/vector-icons';
import { useSafeAreaInsets } from 'react-native-safe-area-context';
import { useSSE } from '../../src/api/sse';
import { colors, fontSize } from '../../src/utils/theme';
import { registerForPushNotifications, setupForegroundNotifications } from '../../src/notifications/fcm';
import { useAuthStore } from '../../src/state/AuthContext';

export default function TabLayout() {
  useSSE();
  const insets = useSafeAreaInsets();
  const username = useAuthStore((state) => state.user?.username);
  const credentials = useAuthStore((state) => state.credentials);

  useEffect(() => {
    if (!username) return;
    registerForPushNotifications();
    return setupForegroundNotifications();
  }, [username]);

  if (!credentials) return <Redirect href="/(auth)/login" />;

  return (
    <Tabs
      screenOptions={{
        headerShown: false,
        tabBarStyle: {
          backgroundColor: '#0c121a',
          borderTopColor: colors.border,
          borderTopWidth: 1,
          height: 62 + insets.bottom,
          paddingBottom: insets.bottom + 5,
          paddingTop: 5,
          elevation: 18,
        },
        tabBarActiveTintColor: colors.accent,
        tabBarInactiveTintColor: colors.textMuted,
        tabBarLabelStyle: {
          fontSize: fontSize.xs,
          fontWeight: '600',
          letterSpacing: .1,
        },
      }}
    >
      <Tabs.Screen
        name="index"
        options={{
          title: 'Home',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="pulse-outline" size={size} color={color} />
          ),
        }}
      />
      <Tabs.Screen
        name="markets"
        options={{
          title: 'Markets',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="pulse-outline" size={size} color={color} />
          ),
        }}
      />
      <Tabs.Screen
        name="trade"
        options={{
          title: 'Trade',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="flash-outline" size={size + 1} color={color} />
          ),
        }}
      />
      <Tabs.Screen
        name="positions"
        options={{
          title: 'Portfolio',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="layers-outline" size={size} color={color} />
          ),
        }}
      />
      <Tabs.Screen
        name="more"
        options={{
          title: 'More',
          tabBarIcon: ({ color, size }) => (
            <Ionicons name="grid-outline" size={size} color={color} />
          ),
        }}
      />
      <Tabs.Screen name="fleet" options={{ href: null }} />
      <Tabs.Screen name="strategy" options={{ href: null }} />
      <Tabs.Screen name="wallet" options={{ href: null }} />
    </Tabs>
  );
}
