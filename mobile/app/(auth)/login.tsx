import { useState } from 'react';
import {
  View,
  Text,
  TextInput,
  TouchableOpacity,
  StyleSheet,
  Image,
  KeyboardAvoidingView,
  Platform,
  ActivityIndicator,
} from 'react-native';
import { router } from 'expo-router';
import { useAuthStore } from '../../src/state/AuthContext';
import { api } from '../../src/api/endpoints';
import { colors, spacing, fontSize } from '../../src/utils/theme';

export default function LoginScreen() {
  const { serverUrl, setServerUrl, setCredentials, setUser, persist } = useAuthStore();
  const [url, setUrl] = useState(serverUrl);
  const [username, setUsername] = useState('');
  const [password, setPassword] = useState('');
  const [loading, setLoading] = useState(false);
  const [error, setError] = useState('');

  async function handleLogin() {
    const candidate = url.trim();
    if (!candidate || !username.trim() || !password) {
      setError('Enter the cockpit address, username, and password to connect.');
      return;
    }

    let normalizedUrl = candidate;
    if (!/^https?:\/\//i.test(normalizedUrl)) normalizedUrl = `https://${normalizedUrl}`;
    try {
      const parsed = new URL(normalizedUrl);
      if (!parsed.hostname) throw new Error('missing host');
      normalizedUrl = normalizedUrl.replace(/\/+$/, '');
    } catch {
      setError('Enter a valid server address, such as https://your-cockpit.example.');
      return;
    }

    setLoading(true);
    setError('');
    try {
      setServerUrl(normalizedUrl);
      setCredentials({ username: username.trim(), password });

      const user = await api.whoami();
      setUser(user);
      await persist();
      router.replace('/(tabs)');
    } catch (e: any) {
      setCredentials(null);
      setUser(null);
      setError(e.message?.includes('Unauthorized')
        ? 'That username or password was not accepted.'
        : `Could not reach the cockpit. ${e.message}`);
    } finally {
      setLoading(false);
    }
  }

  return (
    <KeyboardAvoidingView
      style={styles.container}
      behavior={Platform.OS === 'ios' ? 'padding' : 'height'}
    >
      <View style={styles.inner}>
        <View style={styles.brandMark}>
          <View style={styles.brandOrbit}><View style={styles.brandCore} /></View>
          <Image source={require('../../assets/polysparq-wordmark.png')} resizeMode="contain" style={styles.wordmark} accessibilityLabel="PolySparQ plus plus" />
        </View>
        <Text style={styles.subtitle}>THE MOBILE TRADING COCKPIT</Text>
        <Text style={styles.hero}>See the market.<Text style={styles.heroAccent}> Move with intent.</Text></Text>
        <Text style={styles.intro}>Securely connect to your PolySparQ server to monitor bots, explore markets, and review every order.</Text>

        <View style={styles.form}>
          <Text style={styles.label}>Server URL</Text>
          <TextInput
            style={styles.input}
            value={url}
            onChangeText={setUrl}
            placeholder="https://your-cockpit.example"
            placeholderTextColor={colors.textMuted}
            autoCapitalize="none"
            autoCorrect={false}
            keyboardType="url"
            autoComplete="url"
            textContentType="URL"
            accessibilityLabel="Cockpit server URL"
          />

          <Text style={styles.label}>Username</Text>
          <TextInput
            style={styles.input}
            value={username}
            onChangeText={setUsername}
            placeholder="admin"
            placeholderTextColor={colors.textMuted}
            autoCapitalize="none"
            autoCorrect={false}
            textContentType="username"
            autoComplete="username"
            accessibilityLabel="Username"
          />

          <Text style={styles.label}>Password</Text>
          <TextInput
            style={styles.input}
            value={password}
            onChangeText={setPassword}
            placeholder="Password"
            placeholderTextColor={colors.textMuted}
            secureTextEntry
            textContentType="password"
            autoComplete="current-password"
            returnKeyType="go"
            onSubmitEditing={handleLogin}
            accessibilityLabel="Password"
          />

          {error ? <Text style={styles.error}>{error}</Text> : null}
          <TouchableOpacity
            style={[styles.button, loading && styles.buttonDisabled]}
            onPress={handleLogin}
            disabled={loading}
            accessibilityRole="button"
            accessibilityLabel="Connect to PolySparQ cockpit"
          >
            {loading ? (
              <ActivityIndicator color="#fff" />
            ) : (
              <Text style={styles.buttonText}>Connect</Text>
            )}
          </TouchableOpacity>
        </View>
      </View>
    </KeyboardAvoidingView>
  );
}

const styles = StyleSheet.create({
  container: {
    flex: 1,
    backgroundColor: colors.bg,
  },
  inner: {
    flex: 1,
    justifyContent: 'center',
    paddingHorizontal: spacing.xxl,
    paddingBottom: spacing.xxl,
  },
  brandMark: { alignItems: 'center', justifyContent: 'center', marginBottom: spacing.sm },
  brandOrbit: { width: 42, height: 42, alignItems: 'center', justifyContent: 'center', marginBottom: spacing.md, borderWidth: 1, borderColor: '#315367', borderRadius: 14, backgroundColor: '#10202b' },
  brandCore: { width: 13, height: 13, borderRadius: 5, backgroundColor: colors.accent, shadowColor: colors.accent, shadowOpacity: 0.8, shadowRadius: 12, elevation: 4 },
  wordmark: { width: 238, height: 54,
  },
  subtitle: {
    fontSize: 9,
    color: colors.accent,
    textAlign: 'center',
    fontWeight: '800',
    letterSpacing: 1.8,
    marginTop: spacing.xs,
  },
  hero: { color: colors.text, fontSize: fontSize.xl, fontWeight: '800', textAlign: 'center', marginTop: spacing.xl },
  heroAccent: { color: colors.accent },
  intro: { maxWidth: 330, alignSelf: 'center', fontSize: fontSize.sm, lineHeight: 19, textAlign: 'center', color: colors.textMuted, marginTop: spacing.sm, marginBottom: spacing.xl * 2,
  },
  form: {
    gap: spacing.sm,
    padding: spacing.lg,
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: 12,
    backgroundColor: colors.bgCard,
  },
  label: {
    fontSize: fontSize.sm,
    color: colors.textDim,
    marginTop: spacing.sm,
  },
  input: {
    backgroundColor: colors.bgInput,
    borderWidth: 1,
    borderColor: colors.border,
    borderRadius: 8,
    paddingHorizontal: spacing.lg,
    paddingVertical: spacing.md,
    fontSize: fontSize.md,
    color: colors.text,
  },
  error: { color: colors.red, fontSize: fontSize.xs, lineHeight: 16 },
  button: {
    backgroundColor: colors.accent,
    borderRadius: 8,
    paddingVertical: spacing.lg,
    alignItems: 'center',
    marginTop: spacing.xl,
  },
  buttonDisabled: {
    opacity: 0.6,
  },
  buttonText: {
    fontSize: fontSize.lg,
    fontWeight: '600',
    color: '#fff',
  },
});
