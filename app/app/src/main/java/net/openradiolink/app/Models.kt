package net.openradiolink.app

import okhttp3.HttpUrl
import okhttp3.HttpUrl.Companion.toHttpUrl
import org.json.JSONArray
import org.json.JSONObject
import java.util.UUID

data class AccessPoint(
    val id: String = UUID.randomUUID().toString(),
    val label: String = "局域网",
    val address: String,
    val username: String = "",
    val password: String = "",
    val bearer: String = "",
    val certificate: String = "",
    val audioAddress: String = "",
    val audioAuthorization: String = "",
    val audioCertificate: String = "",
    val mode: String = "LAN",
) {
    fun json() = JSONObject().put("id",id).put("label",label).put("address",address)
        .put("username",username).put("password",password).put("bearer",bearer).put("certificate",certificate)
        .put("audioAddress",audioAddress).put("audioAuthorization",audioAuthorization).put("audioCertificate",audioCertificate)
        .put("mode",mode)
    companion object {
        fun from(o: JSONObject) = AccessPoint(o.getString("id"),o.optString("label","局域网"),o.getString("address"),
            o.optString("username"),o.optString("password"),o.optString("bearer"),o.optString("certificate"),
            o.optString("audioAddress"),o.optString("audioAuthorization"),o.optString("audioCertificate"),o.optString("mode","LAN"))
    }
}

data class DeviceProfile(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val deviceId: String = "",
    val endpoints: List<AccessPoint>,
    val selectedEndpoint: String = endpoints.first().id,
    val favorite: Boolean = false,
    val notes: String = "",
    val drafts: Map<String,String> = emptyMap(),
    val lastSeen: Long = 0,
    val lastStatus: String = "",
    val customName: String = "",
    val nameRevision: Long = -1,
) {
    val displayName: String get() = customName.ifBlank { name.ifBlank { Brand.discoveryName(deviceId) } }
    fun endpoint() = endpoints.firstOrNull { it.id == selectedEndpoint } ?: endpoints.first()
    fun json() = JSONObject().put("id",id).put("name",name).put("deviceId",deviceId)
        .put("endpoints",JSONArray(endpoints.map { it.json() })).put("selectedEndpoint",selectedEndpoint)
        .put("favorite",favorite).put("notes",notes).put("drafts",JSONObject(drafts)).put("lastSeen",lastSeen).put("lastStatus",lastStatus)
        .put("customName",customName).put("nameRevision",nameRevision)
    companion object {
        fun from(o: JSONObject): DeviceProfile {
            val endpoints=o.getJSONArray("endpoints").let { a -> (0 until a.length()).map { AccessPoint.from(a.getJSONObject(it)) } }
            require(endpoints.isNotEmpty()) { "设备缺少访问入口" }
            val d=o.optJSONObject("drafts") ?: JSONObject()
            return DeviceProfile(o.getString("id"),o.getString("name"),o.optString("deviceId"),endpoints,
                o.optString("selectedEndpoint",endpoints.first().id),o.optBoolean("favorite"),o.optString("notes"),
                d.keys().asSequence().associateWith { d.optString(it) },o.optLong("lastSeen"),o.optString("lastStatus"),
                o.optString("customName"),o.optLong("nameRevision",-1))
        }
    }
}

object EndpointAddress {
    fun parse(input: String): HttpUrl {
        val text=input.trim()
        require(text.isNotEmpty()) { "请输入网址或网络地址" }
        val host=if (!text.contains("://") && text.count { it==':' }>1 && !text.startsWith("[") && !text.contains('/')) "[$text]" else text
        val url=(if (host.contains("://")) host else "http://$host").toHttpUrl()
        require(url.username.isEmpty() && url.password.isEmpty()) { "账号与密码请填写在认证栏" }
        require(url.query==null && url.fragment==null) { "入口地址不能包含查询参数或片段" }
        return if (url.encodedPath.endsWith('/')) url else url.newBuilder().addPathSegment("").build()
    }
    fun api(base: HttpUrl, path: String): HttpUrl {
        require(!path.startsWith('/') && !path.contains("://"))
        return requireNotNull(base.resolve(path))
    }
}

data class SessionView(
    val profileId: String = "",
    val deviceId: String = "",
    val phase: String = "未连接",
    val connected: Boolean = false,
    val status: JSONObject = JSONObject(),
    val live: JSONObject = JSONObject(),
    val activity: RadioActivityState = RadioActivityState(),
    val radio: JSONObject = JSONObject(),
    val channels: JSONArray = JSONArray(),
    val wifi: JSONObject = JSONObject(),
    val control: JSONObject = JSONObject(),
    val updated: Long = 0,
    val listening: Boolean = false,
    val ptt: Boolean = false,
    val pttPending: Boolean = false,
    val pttReason: String = "",
    val pttNotice: String = "",
    val pttNoticeId: Long = 0,
    val mine: Boolean = false,
    val maintenance: Boolean = false,
    val progress: Int = -1,
    val error: String = "",
    val taskResult: String = "",
    val microphoneLevel: MicrophoneLevel = MicrophoneLevel(),
    val audioSid:Long=0,
    val audioTx:Long=0,
)

fun SessionView.boardTxClipped():Boolean = status.optJSONObject("sdr")?.let {
    audioSid>0 && audioTx>0 && it.optLong("sid")==audioSid && it.optLong("txid")==audioTx && it.optLong("clip")>0
} ?: false

data class CertificateQuestion(val profileId: String,val endpointId: String,val host: String,val fingerprint: String,val audio: Boolean)
data class DiscoveredBoard(val id: String,val address: String,val name: String,val source: String)

fun JSONObject.value(path: String): String {
    var result: Any? = this
    for (part in path.split('.')) result=when(val current=result) {
        is JSONObject -> current.opt(part)
        is JSONArray -> part.toIntOrNull()?.let { current.opt(it) }
        else -> null
    }
    return when(result) {
        null,JSONObject.NULL -> ""
        is Double -> if (result % 1.0==0.0) result.toLong().toString() else result.toString()
        is Boolean -> if (result) "1" else "0"
        else -> result.toString()
    }
}

fun mergeProfiles(all: List<DeviceProfile>,requested: DeviceProfile,deviceId: String): Pair<List<DeviceProfile>,DeviceProfile> {
    require(deviceId.isNotBlank() && (requested.deviceId.isBlank() || requested.deviceId==deviceId))
    val existing=all.firstOrNull{it.deviceId==deviceId && it.id!=requested.id} ?: all.firstOrNull{it.id==requested.id} ?: requested
    val canonical=existing.copy(deviceId=deviceId,endpoints=(existing.endpoints+requested.endpoints).distinctBy{it.id},selectedEndpoint=requested.endpoint().id,
        favorite=existing.favorite||requested.favorite,notes=if(existing.notes.isBlank())requested.notes else existing.notes,drafts=requested.drafts+existing.drafts)
    val next=all.filterNot{it.id==requested.id && it.id!=canonical.id}.map{if(it.id==canonical.id)canonical else it}
    return (if(next.none{it.id==canonical.id})next+canonical else next) to canonical
}
