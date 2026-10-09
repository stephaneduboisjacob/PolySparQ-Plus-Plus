import { useEffect, useState } from 'react';
import { ActivityIndicator, View } from 'react-native';
import { Redirect } from 'expo-router';
import { useAuthStore } from '../src/state/AuthContext';
import { colors } from '../src/utils/theme';
import { api } from '../src/api/endpoints';

export default function Index() {
  const isLoading = useAuthStore((s) => s.isLoading);
  const credentials = useAuthStore((s) => s.credentials);
  const user = useAuthStore((s) => s.user);
  const setUser = useAuthStore((s) => s.setUser);
  const logout = useAuthStore((s) => s.logout);
  const [checking, setChecking] = useState(false);

  useEffect(() => {
    if (isLoading || !credentials || user || checking) return;
    setChecking(true);
    api.whoami()
      .then(setUser)
      .catch(() => logout())
      .finally(() => setChecking(false));
  }, [checking, credentials, isLoading, logout, setUser, user]);

  if (isLoading || (credentials && !user)) {
    return (
      <View style={{ flex: 1, justifyContent: 'center', alignItems: 'center', backgroundColor: colors.bg }}>
        <ActivityIndicator size="large" color={colors.accent} />
      </View>
    );
  }

  if (!credentials || !user) {
    return <Redirect href="/(auth)/login" />;
  }

  return <Redirect href="/(tabs)" />;
}
