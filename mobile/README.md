# PolySparQ++ Mobile

The mobile client is built with React Native and Expo. It connects to a PolySparQ++ cockpit server chosen by the operator; no hosted service endpoint is bundled in the public app configuration.

## Development

```bash
npm ci
npm run start
```

Enter a reachable cockpit URL on the login screen. A developer may set `EXPO_PUBLIC_POLYSPARQ_SERVER_URL` in an ignored local `.env.local` file to prefill the URL.

To build for Android, install the Android SDK and run `npm run android`. Firebase push notifications are optional and require private configuration files from your own Firebase project. Those files must remain local.

The native package IDs, URL scheme, and secure-storage names retain their established values to avoid breaking existing installations. They are compatibility identifiers and do not change the product branding.

## Licensing

The PolySparQ++ source license is in the repository root. Expo and all other dependencies retain their own licenses; see `../THIRD_PARTY_NOTICES.md` and each dependency's license.

## Contact

Stephane Jacob · [jacobstephane@outlook.com](mailto:jacobstephane@outlook.com)
