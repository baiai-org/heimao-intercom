package net.openradiolink.app

import org.json.JSONObject
import java.net.InetAddress
/**
 * Rules for the management-panel domain: the <name> in https://<name>.local.
 * Android-free so it can be unit-tested; mirrors the firmware's valid_host().
 */
internal object PanelDomain {
    const val MAX=63
    private val RE=Regex("^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$")

    fun normalize(raw:String)=raw.trim().lowercase()
    fun valid(name:String)=name.isNotEmpty() && name.length<=MAX && RE.matches(name)
    fun address(name:String)=if(valid(name)) "https://$name.local" else ""
    fun ipAddress(value:String):InetAddress? {
        val bytes=value.split('.').map { it.toIntOrNull()?.takeIf { number -> number in 0..255 } ?: return null }
        if(bytes.size!=4 || bytes[0] !in 1..223 || bytes[0]==127)return null
        return InetAddress.getByAddress(bytes.map(Int::toByte).toByteArray())
    }

    fun cachedIp(profile:DeviceProfile):InetAddress? {
        val endpoint=profile.endpoint()
        if(profile.deviceId.isBlank() || endpoint.mode!="LAN")return null
        val status=runCatching{JSONObject(profile.lastStatus)}.getOrNull() ?: return null
        if(status.optString("device_id")!=profile.deviceId)return null
        val wifi=status.optJSONObject("wifi") ?: return null
        if(!wifi.optBoolean("connected"))return null
        val url=EndpointAddress.parse(endpoint.address)
        if(url.scheme!="https" && (endpoint.username.isNotBlank() || endpoint.bearer.isNotBlank()))return null
        val names=listOf(wifi.optString("host"),wifi.optString("mdns_host"))
        if(names.none{valid(it) && url.host=="$it.local"})return null
        return ipAddress(wifi.optString("ip"))
    }
    fun error(name:String):String=when {
        name.isEmpty()->"请填写管理面板访问域名"
        name.length>MAX->"域名最长 $MAX 个字符"
        !name.first().isAsciiDigitOrLetter()->"首字符必须是字母或数字"
        !name.last().isAsciiDigitOrLetter()->"末字符必须是字母或数字"
        else->"只能用字母、数字和连字符"
    }
    private fun Char.isAsciiDigitOrLetter()=this in 'a'..'z' || this in '0'..'9'
}
