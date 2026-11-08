package net.openradiolink.app

import org.json.JSONObject
import java.io.IOException

internal enum class RadioActivity { IDLE, RECEIVE, TRANSMIT }
internal class PttRejected(val reason:String):IOException(reason)
internal class PttDenied(message:String,cause:Throwable?=null):IOException(message,cause)

internal object RadioStateRules {
    fun mode(live:JSONObject,status:JSONObject):Int? =
        if(live.has("ic_mode"))live.optInt("ic_mode",-1).takeIf{it in 0..2}
        else status.optJSONObject("intercom")?.optInt("mode",-1)?.takeIf{it in 0..2}

    fun modeName(mode:Int?):String=when(mode){0->"本地电台";1->"公网直连（P2P）";2->"网络中继（MQTT）";else->"网络转发"}

    fun networkReason(mode:Int?):String=when(mode){
        1->"当前处于公网直连（P2P）模式，尚未连接到其他手台，因此禁止发射。请先连通对端手台，或手动切换到本地电台模式。"
        2->"当前处于网络中继（MQTT）模式，中继服务尚未连接，未建立可用的手台间转发通道，因此禁止发射。请先连接中继服务，或手动切换到本地电台模式。"
        else->"当前处于网络转发模式，转发链路尚未就绪，未连接到可用对端，因此禁止发射。请先恢复手台间连接，或手动切换到本地电台模式。"
    }

    fun networkBlock(live:JSONObject,status:JSONObject):String? {
        val mode=mode(live,status)
        return if(mode in 1..2 && live.has("net_ready") && !live.optBoolean("net_ready"))networkReason(mode) else null
    }

    fun networkSummary(live:JSONObject,status:JSONObject):String? = if(networkBlock(live,status)==null)null
        else when(mode(live,status)){1->"公网直连 · 未连接其他手台";2->"网络中继 · 中继服务未连接";else->"网络转发 · 尚未连通对端"}

    // A rejection reason belongs to this press only after all correlation IDs match.
    fun validatePtt(reply:JSONObject,sid:Long,tx:Long,id:Long):JSONObject {
        if(reply.optLong("sid")!=sid || reply.optLong("tx")!=tx || reply.optLong("id")!=id)
            throw IOException("手台的 PTT 确认与本次请求不匹配，已取消发话，请重试")
        if(!reply.optBoolean("ok"))throw PttRejected(reply.optString("reason").ifBlank{"unknown"})
        return reply
    }

    fun rejection(reason:String,mode:Int?,networkReady:Boolean?=null):String=when(reason){
        "network_not_ready"->if(networkReady==true)"当前处于${modeName(mode)}模式，手台报告网络转发或本机电台暂未就绪，本次发话已取消。请检查对端连接和本机射频参数后松开重试。" else networkReason(mode)
        "radio_not_ready"->"当前处于${modeName(mode)}模式，电台模块或发射频率尚未就绪，因此禁止发射。请检查模块频段与收发参数。"
        "busy"->"上一轮发射还在收尾（尾音/释放中），本次按下已取消；请稍等再按，APP 会自动尝试释放。"
        "owner_lost"->"手台的发射占用已失效，本次发话已取消；APP 会自动尝试释放，请稍后重按。"
        "hold_limit"->"已达到手台的单次发话时限，发话已结束。"
        "expired","session_expired"->"手机通联会话已过期，请重新开启收听后发话。"
        else->"当前处于${modeName(mode)}模式，手台拒绝了本次发话（${reason.ifBlank{"未提供原因"}}）。请检查连接与手台端状态后重试。"
    }

    private fun JSONObject.flag(path:String)=value(path) in listOf("1","true")
    fun activity(connected:Boolean,phonePtt:Boolean,status:JSONObject,realtime:RadioActivitySample?=null):RadioActivity {
        if(!connected)return RadioActivity.IDLE
        if(phonePtt)return RadioActivity.TRANSMIT
        // Once a realtime snapshot exists, slow configuration replies cannot
        // resurrect their stale TX/RX flags after a confirmed release.
        if(realtime!=null)return when {realtime.transmit->RadioActivity.TRANSMIT;realtime.receive->RadioActivity.RECEIVE;else->RadioActivity.IDLE}
        if(phonePtt || status.flag("fp.rf_tx") || status.flag("fp.net_tx") || status.optString("ptt") in listOf("TX","TX(BRIDGE)"))return RadioActivity.TRANSMIT
        if(status.flag("fp.rf_rx") || status.flag("fp.net_rx") || status.flag("ptt_net.rx_ptt") || status.flag("ptt_net.rx_carrier"))return RadioActivity.RECEIVE
        return RadioActivity.IDLE
    }
}
