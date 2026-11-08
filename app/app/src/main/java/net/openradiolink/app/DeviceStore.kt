package net.openradiolink.app

import android.content.Context
import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.AtomicFile
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import org.json.JSONArray
import java.io.File
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec

class DeviceStore(context: Context) {
    private val file=AtomicFile(File(context.filesDir,"devices-v1.enc"))
    private val lock=Mutex()
    private val mutable=MutableStateFlow<List<DeviceProfile>>(emptyList())
    val profiles=mutable.asStateFlow()
    val error=MutableStateFlow<String?>(null)
    val loaded=MutableStateFlow(false)
    private val alias="openradiolink.profiles.v1"
    private fun key(): SecretKey {
        val store=KeyStore.getInstance("AndroidKeyStore").apply { load(null) }
        (store.getKey(alias,null) as? SecretKey)?.let { return it }
        return KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES,"AndroidKeyStore").apply {
            init(KeyGenParameterSpec.Builder(alias,KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT)
                .setBlockModes(KeyProperties.BLOCK_MODE_GCM).setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE).build())
        }.generateKey()
    }
    suspend fun load()=withContext(Dispatchers.IO) {
        if (!file.baseFile.exists()) { loaded.value=true;return@withContext }
        try {
            val raw=file.openRead().use { it.readBytes() }
            require(raw.size>28 && raw[0]==1.toByte())
            val size=raw[1].toInt() and 255
            require(size in 12..32 && raw.size>size+2)
            val cipher=Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.DECRYPT_MODE,key(),GCMParameterSpec(128,raw.copyOfRange(2,2+size)))
            val json=JSONArray(String(cipher.doFinal(raw.copyOfRange(2+size,raw.size)),Charsets.UTF_8))
            mutable.value=(0 until json.length()).map { DeviceProfile.from(json.getJSONObject(it)) }
        } catch (_: Exception) {
            error.value="设备档案无法解密或读取；原文件已保留，请勿卸载 App。"
        } finally { loaded.value=true }
    }
    suspend fun update(change: (List<DeviceProfile>)->List<DeviceProfile>)=lock.withLock {
        check(error.value==null) { "档案读取异常，已停止覆盖保存" }
        check(loaded.value) { "设备档案正在加载" }
        val next=change(mutable.value)
        require(next.map { it.id }.distinct().size==next.size)
        withContext(Dispatchers.IO+NonCancellable) {
            val plain=JSONArray(next.map { it.json() }).toString().toByteArray(Charsets.UTF_8)
            val cipher=Cipher.getInstance("AES/GCM/NoPadding")
            cipher.init(Cipher.ENCRYPT_MODE,key())
            val encrypted=byteArrayOf(1,cipher.iv.size.toByte())+cipher.iv+cipher.doFinal(plain)
            val stream=file.startWrite()
            try { stream.write(encrypted); file.finishWrite(stream) }
            catch (e: Exception) { file.failWrite(stream); throw e }
            mutable.value=next
        }
    }
    suspend fun save(profile: DeviceProfile)=update { all ->
        if (all.any { it.id==profile.id }) all.map { if (it.id==profile.id) profile else it } else all+profile
    }
    suspend fun edit(id: String, change: (DeviceProfile)->DeviceProfile)=update { all -> all.map { if (it.id==id) change(it) else it } }
    suspend fun draft(id: String,key: String,value: String)=edit(id) { it.copy(drafts=it.drafts+(key to value)) }
    suspend fun clearDraft(id: String,vararg keys: String)=edit(id) { it.copy(drafts=it.drafts-keys.toSet()) }
}
