package net.openradiolink.app

import java.math.BigDecimal
import java.math.RoundingMode
import org.json.JSONObject

internal object RadioUiRules {
    fun frequencyGridHz(radio:JSONObject):Int = if(radio.optInt("freq_grid_hz")==500)500 else 12500
    fun frequency(text:String,min:String,max:String,gridHz:Int=12500):String {
        require(Regex("\\d+(?:\\.\\d*)?").matches(text.trim())){"请输入有效的 MHz 频率"}
        val value=text.trim().toBigDecimalOrNull() ?: error("请输入有效的 MHz 频率")
        val lower=min.toBigDecimalOrNull() ?: error("尚未读取模块频率范围")
        val upper=max.toBigDecimalOrNull() ?: error("尚未读取模块频率范围")
        require(value>=lower && value<=upper){"频率应为 $min–$max MHz"}
        val grid=BigDecimal(if(gridHz==500)"0.0005" else "0.0125")
        require(value.remainder(grid).compareTo(BigDecimal.ZERO)==0){"频率需符合 ${if(gridHz==500)"0.5" else "12.5"} kHz 设置分辨率"}
        return exactFrequency(value)
    }
    fun displayFrequency(value:String):String = value.toBigDecimalOrNull()?.takeIf{it>BigDecimal.ZERO}
        ?.let{exactFrequency(it)} ?: "—.————"
    private fun exactFrequency(value:BigDecimal):String = value.setScale(4,RoundingMode.HALF_UP).toPlainString()
    fun stepFrequency(text:String,deltaHz:Int,min:String,max:String,gridHz:Int):String {
        val current=frequency(text,min,max,gridHz).toBigDecimal()
        return frequency(current.add(BigDecimal(deltaHz).movePointLeft(6)).toPlainString(),min,max,gridHz)
    }
    fun nameError(name:String):String? = if(name.isBlank())"请输入信道名称" else if(name.toByteArray(Charsets.UTF_8).size>23)"名称最多 23 个 UTF-8 字节" else null
    // Indices shift when a channel is removed. Favorites follow its contents instead.
    fun channelKey(c:JSONObject):String= listOf("name","band","rx","tx","bw","sq","vol","pwr","txt","rxt")
        .joinToString("|"){key->"${c.value(key).length}:${c.value(key)}"}
    fun parameterError(key:String,value:String):String? {
        val range=when(key){"sq"->0..8;"vol"->1..8;"spk_vol"->0..7;"gain","txgain","ntxgain"->1..255;"bgtx"->0..64;"micgain"->0..7;"adcvol"->0..255;"ix_room"->1000..9999;else->if(key.startsWith("eq_"))-12..6 else null}
        return if(range!=null && value.toIntOrNull()?.let{it in range}!=true)"请输入 ${range.first}–${range.last} 的整数" else null
    }
}
