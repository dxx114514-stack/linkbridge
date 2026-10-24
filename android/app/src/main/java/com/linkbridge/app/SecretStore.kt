package com.linkbridge.app

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

/**
 * Android 平台密钥保护：主密钥由 Android Keystore 托管，按需再派生 AES-256-GCM。
 * 落盘格式与 C++ 侧一致：nonce(12) || 密文 || tag(16)。
 */
object SecretStore {
    private const val KEY_ALIAS = "linkbridge.master"
    private const val TRANSFORM = "AES/GCM/NoPadding"
    private const val TAG_BITS = 128
    private const val NONCE_BYTES = 12

    private fun key(): SecretKey {
        val store = KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getEntry(KEY_ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, "AndroidKeyStore")
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .build(),
        )
        return generator.generateKey()
    }

    @JvmStatic
    fun encrypt(plain: ByteArray): ByteArray {
        val cipher = Cipher.getInstance(TRANSFORM)
        cipher.init(Cipher.ENCRYPT_MODE, key())
        val sealed = cipher.doFinal(plain)
        return cipher.iv + sealed
    }

    @JvmStatic
    fun decrypt(sealed: ByteArray): ByteArray? {
        if (sealed.size <= NONCE_BYTES + TAG_BITS / 8) return null
        return try {
            val cipher = Cipher.getInstance(TRANSFORM)
            cipher.init(
                Cipher.DECRYPT_MODE,
                key(),
                GCMParameterSpec(TAG_BITS, sealed.copyOfRange(0, NONCE_BYTES)),
            )
            cipher.doFinal(sealed.copyOfRange(NONCE_BYTES, sealed.size))
        } catch (e: Exception) {
            null
        }
    }
}
