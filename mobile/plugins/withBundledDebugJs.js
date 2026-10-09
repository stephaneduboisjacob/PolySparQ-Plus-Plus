const { withAppBuildGradle } = require('@expo/config-plugins');

const PLUGIN_MARKER = 'debuggableVariants = [] // PolySparQ standalone debug APK';

module.exports = function withBundledDebugJs(config) {
  return withAppBuildGradle(config, (config) => {
    const contents = config.modResults.contents;
    if (contents.includes(PLUGIN_MARKER)) return config;

    const reactBlock = /react\s*\{\s*\n/;
    if (!reactBlock.test(contents)) {
      throw new Error('Could not find the React Native Gradle react {} block in android/app/build.gradle');
    }

    config.modResults.contents = contents.replace(
      reactBlock,
      (block) => `${block}    ${PLUGIN_MARKER}\n`,
    );
    return config;
  });
};
