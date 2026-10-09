const fs = require('node:fs');
const path = require('node:path');

const staticExpo = require('./app.json').expo;

module.exports = ({ config = {} } = {}) => {
  const expo = {
    ...config,
    ...staticExpo,
    android: { ...(config.android || {}), ...(staticExpo.android || {}) },
    ios: { ...(config.ios || {}), ...(staticExpo.ios || {}) },
  };
  const androidServices = path.join(__dirname, 'google-services.json');
  const iosServices = path.join(__dirname, 'GoogleService-Info.plist');
  const firebasePushEnabled = fs.existsSync(androidServices) && fs.existsSync(iosServices);

  expo.plugins = (expo.plugins || []).filter((plugin) => {
    const name = Array.isArray(plugin) ? plugin[0] : plugin;
    return firebasePushEnabled || ![
      '@react-native-firebase/app',
      '@react-native-firebase/messaging',
    ].includes(name);
  });
  // Ship a JS bundle in debug APKs too. This keeps the installable APK
  // usable when it is launched without Metro (for example, from Downloads).
  if (!expo.plugins.some((plugin) =>
    (Array.isArray(plugin) ? plugin[0] : plugin) === './plugins/withBundledDebugJs'
  )) {
    expo.plugins.push('./plugins/withBundledDebugJs');
  }
  expo.extra = { ...(expo.extra || {}), firebasePushEnabled };

  if (firebasePushEnabled) {
    expo.android.googleServicesFile = './google-services.json';
    expo.ios.googleServicesFile = './GoogleService-Info.plist';
  } else {
    delete expo.android.googleServicesFile;
    delete expo.ios.googleServicesFile;
  }

  return expo;
};
