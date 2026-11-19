package net.openradiolink.app

import org.json.JSONObject
import java.security.KeyFactory
import java.security.Signature
import java.security.spec.X509EncodedKeySpec
import okio.ByteString.Companion.decodeBase64
import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl

internal data class ReleaseAsset(val code:Int,val version:String,val tag:String,val file:String,
                                val size:Long,val sha256:String,val force:Boolean,val notes:String) {
    val url:HttpUrl get()=ReleaseUpdateManifest.root.newBuilder()
        .addPathSegments("download").addPathSegment(tag).addPathSegment(file).build()

    fun sameBinary(other:ReleaseAsset)=code==other.code && size==other.size && sha256==other.sha256
}

internal object ReleaseUpdateManifest {
    const val maxManifest=6144
    private const val maxApk=100L*1024*1024
    private const val key="MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEnToQWAjXjPIjMhvAwxbC24UnVSotFBI7SsV7ErhcA4lze2FkGLW/pxsRJmSvnDL3HkvI31BPGdqapxFlEjZ5Tg=="
    val root="https://github.com/baiai-org/heimao-intercom/releases/".toHttpUrl()
    val latest=root.newBuilder().addPathSegments("latest/download/update.json").build()
    private val name=Regex("[A-Za-z0-9][A-Za-z0-9._-]{0,95}")
    private val digest=Regex("[0-9a-f]{64}")

    fun allowed(url:HttpUrl)=url.isHttps && url.port==443 && url.username.isEmpty() && url.password.isEmpty() &&
        when(url.host) {
            "github.com"->url.encodedPath.startsWith(root.encodedPath)
            "release-assets.githubusercontent.com","objects.githubusercontent.com"->true
            else->false
        }

    fun decode(bytes:ByteArray, packageName:String, installed:Int):ReleaseAsset? {
        require(bytes.size<=maxManifest)
        val envelope=JSONObject(bytes.toString(Charsets.UTF_8))
        val payload=envelope.getString("payload").decodeBase64()?.toByteArray() ?: error("清单编码无效")
        val signature=envelope.getString("signature").decodeBase64()?.toByteArray() ?: error("签名编码无效")
        require(payload.size in 1..4096 && signature.size in 64..80)
        val pub=KeyFactory.getInstance("EC").generatePublic(X509EncodedKeySpec(checkNotNull(key.decodeBase64()).toByteArray()))
        require(Signature.getInstance("SHA256withECDSA").run {
            initVerify(pub);update(payload);verify(signature)
        }){"更新清单签名无效"}
        val data=JSONObject(payload.toString(Charsets.UTF_8))
        require(data.get("schema")==1)
        if(!data.has("app"))return null
        val item=data.getJSONObject("app")
        if(item.getString("target")!=packageName)return null
        val code=item.get("code") as? Int ?: error("版本无效")
        if(code<=installed)return null
        val version=item.getString("version")
        val tag=item.getString("tag")
        val file=item.getString("file")
        val size=(item.get("size") as? Int ?: error("文件长度无效")).toLong()
        val sha256=item.getString("sha256")
        val force=if(item.has("force"))item.get("force") as? Boolean ?: error("安装方式无效") else false
        val notes=if(data.has("notes"))data.getString("notes") else ""
        require(listOf(version,tag,file).all(name::matches) && file.endsWith(".apk")
            && size in 1..maxApk && digest.matches(sha256) && notes.toByteArray().size<=512)
        return ReleaseAsset(code,version,tag,file,size,sha256,force,notes)
    }
}
