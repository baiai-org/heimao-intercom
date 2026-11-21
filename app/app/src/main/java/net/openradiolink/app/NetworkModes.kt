package net.openradiolink.app

import org.json.JSONObject

/**
 * The three voice-network modes the firmware supports (intercom.mode). Kept free of
 * Android types so the labelling and connectivity rules can be unit-tested.
 */
internal object NetworkModes {
    data class Card(val mode:Int,val title:String,val hint:String)

    val cards=listOf(
        Card(0,"本地电台","不经网络，本机麦克风与手台直通"),
        Card(1,"公网直连 P2P","STUN 打洞，点对点，不经过服务器"),
        Card(2,"网络中继","经 MQTT 服务器转发，适合跨网段"),
    )
    fun isCurrent(mode:Int,activeMode:Int)=mode==activeMode
    fun stateLabel(state:String)=when(state) {
        "idle"->"待机"; "tx"->"发话中"; "rx"->"接收中"; "knocking"->"握手中"
        "connecting"->"连接中"; "stopped"->"已停止"; "no-net"->"无网络"
        "no-engine"->"引擎未就绪"; "stub"->"未启用"; "off"->"已关闭"
        else->state.ifBlank{"未知状态"}
    }
    /** Status text for the local and direct-peer cards. */
    fun status(mode:Int,activeMode:Int,state:String,peers:Int):Pair<String,Boolean> {
        if(mode!=activeMode)return "未启用 · 点按切换" to false
        if(mode==0)return "本机直通" to true
        if(state=="no-net")return "无网络" to false
        if(state=="stopped")return "已停止" to false
        /* No engine / stub engine / disabled: nothing can carry audio, so the dot
         * must not read as "linked" just because the socket has no objection. */
        if(state=="no-engine"||state=="stub"||state=="off")return stateLabel(state) to false
        val label=if(peers>0)"${stateLabel(state)} · $peers 在线用户" else stateLabel(state)
        return label to (peers>0)
    }

    enum class RelayTone { NEUTRAL, CONNECTED, WAITING, DOWN }
    data class RelayStatus(val title:String,val tone:RelayTone,val reason:String="")

    fun relayStatus(status:JSONObject,live:JSONObject,online:Boolean):RelayStatus {
        if(!online)return RelayStatus("手台离线 · 中继状态未知",RelayTone.NEUTRAL)
        val unknown=RelayStatus("中继状态待确认",RelayTone.NEUTRAL)
        val intercom=status.optJSONObject("intercom") ?: return unknown
        val mode=intercom.optInt("mode",-1)
        if(mode<0)return unknown
        if(mode!=2)return RelayStatus("中继未启用",RelayTone.NEUTRAL)
        val reason=when(intercom.optString("relay_reason")){
            "authentication"->"请检查服务器用户名和密码"
            "dns"->"服务器域名解析失败"
            "saved_waiting"->"服务器地址已保存，等待话段结束后重连"
            "network"->"请检查手台联网和服务器地址"
            "subscribing"->"等待服务器确认语音订阅"
            "subscribe_failed"->"语音订阅失败，请检查服务器权限"
            "subscribe_timeout"->"服务器未确认语音订阅"
            "start_failed"->"中继服务启动失败，请检查设置"
            else->""
        }
        val linked=RelayStatus("✓ 中继已连接",RelayTone.CONNECTED)
        val waiting=RelayStatus("◷ 正在连接中继",RelayTone.WAITING,reason)
        val down=RelayStatus("! 中继未连接",RelayTone.DOWN)
        return when(intercom.optString("relay_link")){
            "connected"->linked
            "connecting"->waiting
            "reconnecting"->waiting.copy(title="◷ 正在重连中继")
            "disconnected"->down
            "error"->RelayStatus("! 中继连接失败",RelayTone.DOWN,reason)
            ""->when(intercom.optString("state")){
                "connecting"->waiting
                "stopped","no-engine","no-net","stub","off","down"->down
                "idle","tx","rx"->when {
                    intercom.optString("diag").startsWith("connected,") -> linked
                    intercom.optString("diag").startsWith("trying ") -> waiting
                    live.optInt("ic_mode",-1)==2 && live.opt("net_ready")==false -> down
                    intercom.optString("state")=="idle" && live.optInt("ic_mode",-1)==2 && live.opt("net_ready")==true -> linked
                    else -> unknown
                }
                else->unknown
            }
            else->unknown
        }
    }
}
