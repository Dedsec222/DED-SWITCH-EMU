// SPDX-FileCopyrightText: Copyright 2026 DED SWITCH contributors
// SPDX-License-Identifier: GPL-3.0-or-later

package org.yuzu.yuzu_emu.features.savevault

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import org.json.JSONObject
import java.io.BufferedInputStream
import java.io.BufferedOutputStream
import java.io.File
import java.io.FileInputStream
import java.io.DataInputStream
import java.io.FileOutputStream
import java.io.IOException
import java.io.FilterOutputStream
import java.security.KeyStore
import java.security.MessageDigest
import java.security.SecureRandom
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.CipherInputStream
import javax.crypto.CipherOutputStream
import javax.crypto.spec.GCMParameterSpec
import java.util.UUID
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * Local, device-bound encrypted save snapshots for DED SWITCH.
 *
 * Important: callers must stop emulation before invoking createBackup(). This first
 * implementation deliberately refuses to snapshot a live save directory because a
 * directory copy taken during an emulator write is not guaranteed to be consistent.
 * Restore and UI integration are separate follow-up steps.
 */
class SaveVault(private val vaultDirectory: File) {
    data class BackupRecord(
        val id: String,
        val titleId: String,
        val createdAtEpochMs: Long,
        val encryptedFile: File,
        val encryptedSha256: String,
        val formatVersion: Int
    )

    companion object {
        private const val KEYSTORE_PROVIDER = "AndroidKeyStore"
        private const val KEY_ALIAS = "ded.switch.savevault.master.v1"
        private const val TRANSFORMATION = "AES/GCM/NoPadding"
        private const val NONCE_SIZE_BYTES = 12
        private const val TAG_SIZE_BITS = 128
        private const val FORMAT_VERSION = 1
        private const val BUFFER_SIZE = 64 * 1024
        private const val METADATA_SUFFIX = ".json"
        private const val BACKUP_SUFFIX = ".dsvault"
    }

    /** Creates an encrypted snapshot. Never deletes or modifies [saveDirectory]. */
    @Synchronized
    @Throws(IOException::class, SecurityException::class)
    fun createBackup(
        titleId: String,
        saveDirectory: File,
        emulatorStopped: Boolean
    ): BackupRecord {
        require(emulatorStopped) { "Emulation must be stopped before backing up saves." }
        require(titleId.matches(Regex("[A-Fa-f0-9]{1,16}"))) { "Invalid title ID." }
        require(saveDirectory.isDirectory) { "Save directory does not exist." }

        if (!vaultDirectory.exists() && !vaultDirectory.mkdirs()) {
            throw IOException("Unable to create save vault directory.")
        }
        if (!vaultDirectory.isDirectory || !vaultDirectory.canWrite()) {
            throw IOException("Save vault directory is not writable.")
        }

        val backupId = UUID.randomUUID().toString()
        val createdAt = System.currentTimeMillis()
        val baseName = "${titleId.uppercase()}_${createdAt}_$backupId"
        val encryptedFinal = File(vaultDirectory, baseName + BACKUP_SUFFIX)
        val encryptedTemp = File(vaultDirectory, baseName + BACKUP_SUFFIX + ".partial")
        val metadataFinal = File(vaultDirectory, baseName + METADATA_SUFFIX)
        val metadataTemp = File(vaultDirectory, baseName + METADATA_SUFFIX + ".partial")

        try {
            writeEncryptedDirectoryZip(saveDirectory, encryptedTemp)
            if (!encryptedTemp.renameTo(encryptedFinal)) {
                throw IOException("Unable to commit encrypted backup.")
            }

            val digest = sha256(encryptedFinal)
            val metadata = JSONObject()
                .put("formatVersion", FORMAT_VERSION)
                .put("id", backupId)
                .put("titleId", titleId.uppercase())
                .put("createdAtEpochMs", createdAt)
                .put("encryptedSha256", digest)
                .put("encryption", "AES-256-GCM")
                .put("keyAliasVersion", 1)

            FileOutputStream(metadataTemp).use { output ->
                output.write(metadata.toString().toByteArray(Charsets.UTF_8))
                output.fd.sync()
            }
            if (!metadataTemp.renameTo(metadataFinal)) {
                encryptedFinal.delete()
                throw IOException("Unable to commit backup metadata.")
            }

            return BackupRecord(
                id = backupId,
                titleId = titleId.uppercase(),
                createdAtEpochMs = createdAt,
                encryptedFile = encryptedFinal,
                encryptedSha256 = digest,
                formatVersion = FORMAT_VERSION
            )
        } finally {
            encryptedTemp.delete()
            metadataTemp.delete()
        }
    }

    /** Verifies the encrypted-file digest and authenticates the complete GCM payload. */
    @Synchronized
    @Throws(IOException::class, SecurityException::class)
    fun verifyBackup(encryptedFile: File, expectedSha256: String): Boolean {
        if (!encryptedFile.isFile || !encryptedFile.canRead()) return false
        if (!sha256(encryptedFile).equals(expectedSha256, ignoreCase = true)) return false

        return try {
            FileInputStream(encryptedFile).use { fileInput ->
                val nonce = ByteArray(NONCE_SIZE_BYTES)
                DataInputStream(fileInput).readFully(nonce)
                val cipher = Cipher.getInstance(TRANSFORMATION)
                cipher.init(Cipher.DECRYPT_MODE, getOrCreateKey(), GCMParameterSpec(TAG_SIZE_BITS, nonce))
                CipherInputStream(BufferedInputStream(fileInput), cipher).use { decrypted ->
                    val buffer = ByteArray(BUFFER_SIZE)
                    while (decrypted.read(buffer) != -1) {
                        // Consume the complete stream so GCM authentication is checked.
                    }
                }
            }
            true
        } catch (_: Exception) {
            false
        }
    }

    /** Returns metadata-backed backups. Unpaired files are ignored, never auto-deleted. */
    @Synchronized
    fun listBackups(titleId: String? = null): List<BackupRecord> {
        if (!vaultDirectory.isDirectory) return emptyList()
        return vaultDirectory.listFiles { file -> file.isFile && file.name.endsWith(METADATA_SUFFIX) }
            .orEmpty()
            .mapNotNull { metadataFile ->
                try {
                    val json = JSONObject(metadataFile.readText(Charsets.UTF_8))
                    val backupFile = File(
                        vaultDirectory,
                        metadataFile.name.removeSuffix(METADATA_SUFFIX) + BACKUP_SUFFIX
                    )
                    val record = BackupRecord(
                        id = json.getString("id"),
                        titleId = json.getString("titleId"),
                        createdAtEpochMs = json.getLong("createdAtEpochMs"),
                        encryptedFile = backupFile,
                        encryptedSha256 = json.getString("encryptedSha256"),
                        formatVersion = json.getInt("formatVersion")
                    )
                    if (backupFile.isFile && (titleId == null ||
                            record.titleId.equals(titleId, ignoreCase = true))) record else null
                } catch (_: Exception) {
                    null
                }
            }
            .sortedByDescending { it.createdAtEpochMs }
    }

    /** Streams ZIP entries directly into AES-GCM; no plaintext archive is written to disk. */
    private fun writeEncryptedDirectoryZip(source: File, destination: File) {
        val root = source.canonicalFile
        val nonce = ByteArray(NONCE_SIZE_BYTES).also { SecureRandom().nextBytes(it) }
        val cipher = Cipher.getInstance(TRANSFORMATION)
        cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey(), GCMParameterSpec(TAG_SIZE_BITS, nonce))

        FileOutputStream(destination).use { rawOutput ->
            rawOutput.write(nonce)
            val nonClosing = object : FilterOutputStream(rawOutput) {
                override fun close() = flush()
            }
            val encryptedOutput = CipherOutputStream(BufferedOutputStream(nonClosing), cipher)
            ZipOutputStream(encryptedOutput).use { zip ->
                val files = root.walkTopDown().filter { it.isFile }.toList()
                if (files.isEmpty()) throw IOException("Save directory contains no files.")
                for (file in files) {
                    val canonical = file.canonicalFile
                    val rootPath = root.path
                    val childPath = canonical.path
                    if (childPath != rootPath && !childPath.startsWith(rootPath + File.separator)) {
                        throw IOException("Save entry escapes the selected directory.")
                    }
                    val relative = childPath.removePrefix(rootPath)
                        .trimStart(File.separatorChar)
                        .replace(File.separatorChar, '/')
                    if (relative.isBlank() || relative.startsWith("/")) {
                        throw IOException("Invalid save entry path.")
                    }
                    zip.putNextEntry(ZipEntry(relative))
                    val initialLength = file.length()
                    val initialModified = file.lastModified()
                    BufferedInputStream(FileInputStream(file)).use { input ->
                        val buffer = ByteArray(BUFFER_SIZE)
                        while (true) {
                            val count = input.read(buffer)
                            if (count < 0) break
                            zip.write(buffer, 0, count)
                        }
                    }
                    if (file.length() != initialLength || file.lastModified() != initialModified) {
                        throw IOException("Save file changed during backup; snapshot aborted.")
                    }
                    zip.closeEntry()
                }
            }
            rawOutput.fd.sync()
        }
    }

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(KEYSTORE_PROVIDER).apply { load(null) }
        (keyStore.getKey(KEY_ALIAS, null) as? SecretKey)?.let { return it }

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE_PROVIDER)
        generator.init(
            KeyGenParameterSpec.Builder(
                KEY_ALIAS,
                KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
            )
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
                .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
                .setKeySize(256)
                .setRandomizedEncryptionRequired(true)
                .build()
        )
        return generator.generateKey()
    }

    private fun sha256(file: File): String {
        val digest = MessageDigest.getInstance("SHA-256")
        FileInputStream(file).use { input ->
            val buffer = ByteArray(BUFFER_SIZE)
            while (true) {
                val count = input.read(buffer)
                if (count < 0) break
                digest.update(buffer, 0, count)
            }
        }
        return digest.digest().joinToString("") { "%02x".format(it) }
    }
}
