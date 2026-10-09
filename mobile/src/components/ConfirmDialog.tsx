import { Alert } from 'react-native';

export function confirmAction(
  title: string,
  message: string,
  onConfirm: () => void | Promise<void>,
) {
  Alert.alert(title, message, [
    { text: 'Cancel', style: 'cancel' },
    { text: 'Confirm', style: 'destructive', onPress: onConfirm },
  ]);
}
