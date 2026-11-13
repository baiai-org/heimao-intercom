package net.openradiolink.app

import java.math.BigDecimal
import java.math.RoundingMode
import org.json.JSONObject

internal object RxGuardRules {
    data class Config(val enabled:Boolean, val milliseconds:Int, val sources:Int, val outputs:Int)
    fun read(status:JSONObject,mode:Int):Config? {
        if(mode !in 1..2)return null
        val root=status.optJSONObject("rx_guard") ?: return null
        if(root.optInt("version",0)!=1)return null
        val c=root.optJSONObject(if(mode==1)"p2p" else "relay") ?: return null
        val en=c.optInt("en",-1);val ms=c.optInt("ms",0)
        val sources=c.optInt("sources",0);val outputs=c.optInt("outputs",0)
        if(en !in 0..1 || ms !in 1..60000 || sources !in 1..7 || outputs !in 1..7)return null
        return Config(en==1,ms,sources,outputs)
    }
    fun delay(text:String,milliseconds:Boolean):Int {
        val s=text.trim()
        require((if(milliseconds)Regex("[0-9]+") else Regex("[0-9]+(?:\\.[0-9]{1,3})?")).matches(s)) {
            if(milliseconds)"请输入整数毫秒" else "请输入秒数，最多三位小数"
        }
        val value=BigDecimal(s).multiply(if(milliseconds)BigDecimal.ONE else BigDecimal(1000))
        require(value>=BigDecimal.ONE && value<=BigDecimal(60000)){"保护时间须为 1–60000 毫秒（0.001–60 秒）"}
        return value.intValueExact()
    }
    fun format(ms:Int,milliseconds:Boolean):String=if(milliseconds)ms.toString() else
        BigDecimal(ms).movePointLeft(3).setScale(3,RoundingMode.UNNECESSARY).toPlainString()
    fun encode(c:Config):String {
        require(c.milliseconds in 1..60000){"保护时间须为 1–60000 毫秒"}
        require(c.sources in 1..7){"请至少选择一个触发来源"}
        require(c.outputs in 1..7){"请至少选择一个屏蔽出口"}
        return "${if(c.enabled)1 else 0},${c.milliseconds},${c.sources},${c.outputs}"
    }
    fun key(mode:Int):String {
        require(mode in 1..2)
        return if(mode==1)"rx_guard_p2p" else "rx_guard_relay"
    }
}
