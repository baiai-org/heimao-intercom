package net.openradiolink.app

import kotlin.math.abs
import kotlin.math.log10
import kotlin.math.pow
import kotlin.math.roundToInt
import kotlin.math.sqrt

data class MicrophoneLevel(
    val rawPeak:Int=0,
    val outputPeak:Int=0,
    val rawRmsDb:Double = -96.0,
    val outputRmsDb:Double = -96.0,
    val clipped:Long=0,
    val samples:Long=0,
    val gainDb:Int=0,
    val input:String="等待麦克风",
) {
    val label:String get()=when {
        clipped>0 -> "过载，请调低发话音量"
        outputPeak>=29490 -> "接近过载"
        outputRmsDb < -36 -> "偏低"
        else -> "合适"
    }
    val fraction:Float get()=((outputRmsDb+60)/60).coerceIn(0.0,1.0).toFloat()
}

/** Fixed gain only. No AGC, gate, compression or automatic PTT. */
class MicrophoneGain(val gainDb:Int) {
    init { require(gainDb in 0..12 && gainDb%3==0) }
    private val factor=10.0.pow(gainDb/20.0)
    private var clips=0L
    private var total=0L
    fun process(samples:ShortArray,count:Int,input:String):MicrophoneLevel {
        require(count in 1..samples.size)
        var rawPeak=0;var outputPeak=0;var rawSquares=0.0;var outputSquares=0.0
        for(i in 0 until count) {
            val raw=samples[i].toInt()
            val amplified=(raw*factor).roundToInt()
            val out=amplified.coerceIn(-32768,32767)
            if(out!=amplified)clips++
            rawPeak=maxOf(rawPeak,abs(raw));outputPeak=maxOf(outputPeak,abs(out))
            rawSquares+=raw.toDouble()*raw;outputSquares+=out.toDouble()*out
            samples[i]=out.toShort()
        }
        total+=count
        fun db(squares:Double)=if(squares==0.0)-96.0 else (20*log10(sqrt(squares/count)/32768)).coerceAtLeast(-96.0)
        return MicrophoneLevel(rawPeak,outputPeak,db(rawSquares),db(outputSquares),clips,total,gainDb,input)
    }
}
