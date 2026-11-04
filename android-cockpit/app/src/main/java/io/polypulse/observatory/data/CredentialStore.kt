package io.polypulse.observatory.data

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import androidx.core.content.edit
import org.json.JSONObject
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/** Stores the cockpit address and login as AES-GCM ciphertext protected by Android Keystore. */
class CredentialStore(context: Context) {
    private val preferences = context.getSharedPreferences("observatory_secure", Context.MODE_PRIVATE)

    @Synchronized
    fun save(credentials: CockpitCredentials) {
        val plain = JSONObject()
            .put("server", credentials.serverUrl)
            .put("username", credentials.username)
            .put("password", credentials.password)
            .put("local_network_permission_requested", credentials.localNetworkPermissionRequested)
            .toString()
            .toByteArray(Charsets.UTF_8)
        val cipher = Cipher.getInstance(TRANSFORMATION).apply { init(Cipher.ENCRYPT_MODE, getOrCreateKey()) }
        val ciphertext = cipher.doFinal(plain)
        val combined = cipher.iv + ciphertext
        preferences.edit { putString(STORAGE_KEY, Base64.encodeToString(combined, Base64.NO_WRAP)) }
    }

    @Synchronized
    fun load(): CockpitCredentials? {
        val encoded = preferences.getString(STORAGE_KEY, null) ?: return null
        return runCatching {
            val combined = Base64.decode(encoded, Base64.NO_WRAP)
            require(combined.size > IV_LENGTH)
            val iv = combined.copyOfRange(0, IV_LENGTH)
            val ciphertext = combined.copyOfRange(IV_LENGTH, combined.size)
            val cipher = Cipher.getInstance(TRANSFORMATION).apply {
                init(Cipher.DECRYPT_MODE, getOrCreateKey(), GCMParameterSpec(128, iv))
            }
            val json = JSONObject(String(cipher.doFinal(ciphertext), Charsets.UTF_8))
            CockpitCredentials(
                serverUrl = json.getString("server"),
                username = json.getString("username"),
                password = json.getString("password"),
                localNetworkPermissionRequested = json.optBoolean("local_network_permission_requested", false),
            )
        }.getOrElse {
            clear()
            null
        }
    }

    @Synchronized
    fun clear() {
        preferences.edit { remove(STORAGE_KEY) }
    }

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(ANDROID_KEY_STORE).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }
        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, ANDROID_KEY_STORE)
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setRandomizedEncryptionRequired(true)
                .build(),
        )
        return generator.generateKey()
    }

    private companion object {
        const val STORAGE_KEY = "credentials_v1"
        const val KEY_ALIAS = "polysparq_observatory_credentials"
        const val ANDROID_KEY_STORE = "AndroidKeyStore"
        const val TRANSFORMATION = "AES/GCM/NoPadding"
        const val IV_LENGTH = 12
    }
}
