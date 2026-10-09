import { Alert, PermissionsAndroid, Platform } from 'react-native';
import Constants from 'expo-constants';
import * as SecureStore from 'expo-secure-store';
import { api } from '../api/endpoints';

const DEVICE_ID_KEY = 'polypulse_push_device_id';
const pushConfigured = Constants.expoConfig?.extra?.firebasePushEnabled === true;
let messagingFactory: (() => any) | null | undefined;
let unsubscribeTokenRefresh: (() => void) | null = null;

function getMessaging(): (() => any) | null {
  if (!pushConfigured) return null;
  if (messagingFactory !== undefined) return messagingFactory;
  try {
    const mod = require('@react-native-firebase/messaging');
    messagingFactory = mod.default || mod;
  } catch {
    messagingFactory = null;
  }
  return messagingFactory ?? null;
}

async function getDeviceId(): Promise<string> {
  const stored = await SecureStore.getItemAsync(DEVICE_ID_KEY);
  if (stored) return stored;
  const id = `${Platform.OS}-${Date.now()}-${Math.random().toString(36).slice(2, 12)}`;
  await SecureStore.setItemAsync(DEVICE_ID_KEY, id);
  return id;
}

export async function requestNotificationPermission(): Promise<boolean> {
  if (!pushConfigured) return false;
  const factory = getMessaging();
  if (!factory) return false;
  try {
    if (Platform.OS === 'android' && Number(Platform.Version) >= 33) {
      const result = await PermissionsAndroid.request(
        PermissionsAndroid.PERMISSIONS.POST_NOTIFICATIONS,
      );
      if (result !== PermissionsAndroid.RESULTS.GRANTED) return false;
    }
    if (Platform.OS === 'ios') {
      const status = await factory().requestPermission();
      return status === 1 || status === 2;
    }
    return true;
  } catch {
    return false;
  }
}

export async function registerForPushNotifications(): Promise<boolean> {
  if (!pushConfigured) return false;
  const factory = getMessaging();
  if (!factory || !(await requestNotificationPermission())) return false;
  try {
    await factory().registerDeviceForRemoteMessages();
    const [token, deviceId] = await Promise.all([factory().getToken(), getDeviceId()]);
    await api.pushRegister(token, deviceId);
    unsubscribeTokenRefresh?.();
    unsubscribeTokenRefresh = factory().onTokenRefresh((newToken: string) => {
      api.pushRegister(newToken, deviceId).catch(() => undefined);
    });
    return true;
  } catch {
    return false;
  }
}

export function setupForegroundNotifications(): () => void {
  if (!pushConfigured) return () => undefined;
  const factory = getMessaging();
  if (!factory) return () => undefined;
  return factory().onMessage(async (message: any) => {
    const title = message?.notification?.title || 'PolySparQ++';
    const body = message?.notification?.body || 'New trading activity';
    Alert.alert(title, body);
  });
}
