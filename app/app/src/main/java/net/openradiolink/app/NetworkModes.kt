package net.openradiolink.app

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
    /**
     * One line for a mode card and whether the channel is actually usable.
     * The firmware reports "idle" both for a linked P2P peer and for a relay
     * socket that is connected, so peers decide P2P and "connecting" decides relay.
     * Both network modes count as 在线用户, the panel's unified wording; the P2P
     * peer rows themselves still say 对端, matching the web panel.
     */
    fun status(mode:Int,activeMode:Int,state:String,peers:Int):Pair<String,Boolean> {
        if(mode!=activeMode)return "未启用 · 点按切换" to false
        if(mode==0)return "本机直通" to true
        if(state=="no-net")return "无网络" to false
        if(state=="stopped")return "已停止" to false
        /* No engine / stub engine / disabled: nothing can carry audio, so the dot
         * must not read as "linked" just because the socket has no objection. */
        if(state=="no-engine"||state=="stub"||state=="off")return stateLabel(state) to false
        if(mode==1) {
            val label=if(peers>0)"${stateLabel(state)} · $peers 在线用户" else stateLabel(state)
            return label to (peers>0)
        }
        if(state=="connecting")return "连接中 · 尚未连上服务器" to false
        val label=if(peers>0)"${stateLabel(state)} · $peers 个在线用户" else stateLabel(state)
        return label to true
    }
}
