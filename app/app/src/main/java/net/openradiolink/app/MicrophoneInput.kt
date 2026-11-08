package net.openradiolink.app

import android.annotation.SuppressLint
import android.content.Context
import android.media.*
import android.media.audiofx.*
import java.io.IOException

class MicrophoneSettings(context:Context) {
    private val prefs=context.getSharedPreferences("microphone-gain",Context.MODE_PRIVATE)
    fun gain(key:String):Int=prefs.getInt("gain:$key",0).takeIf{it in 0..12 && it%3==0} ?: 0
    fun save(key:String,gain:Int) {
        require(gain in 0..12 && gain%3==0)
        check(prefs.edit().putInt("gain:$key",gain).commit()) { "发话音量保存失败" }
    }
    fun lastInput():Pair<String,String> = prefs.getString("last-key","").orEmpty() to prefs.getString("last-label","尚未测试麦克风").orEmpty()
    fun remember(key:String,label:String) { prefs.edit().putString("last-key",key).putString("last-label",label).apply() }
}

class MicrophoneInput private constructor(val recorder:AudioRecord,val source:Int,private val effects:List<AudioEffect>) {
    fun identity():Pair<String,String> {
        val device=recorder.routedDevice ?: return "unresolved:$source" to "系统默认输入（路由未核实）"
        // Address is stable across sessions on API 28+; never use volatile device ID.
        val address=if(android.os.Build.VERSION.SDK_INT>=28)device.address else ""
        val key="${device.type}:$address:${device.productName}:$source"
        val label=when(device.type) {
            AudioDeviceInfo.TYPE_BUILTIN_MIC -> "内置麦克风"
            AudioDeviceInfo.TYPE_WIRED_HEADSET -> "有线耳麦"
            AudioDeviceInfo.TYPE_USB_DEVICE,AudioDeviceInfo.TYPE_USB_HEADSET,AudioDeviceInfo.TYPE_USB_ACCESSORY -> "USB 麦克风 · ${device.productName}"
            AudioDeviceInfo.TYPE_BLUETOOTH_SCO,AudioDeviceInfo.TYPE_BLE_HEADSET -> "蓝牙麦克风 · ${device.productName}"
            else -> "音频输入 · ${device.productName}"
        }
        return key to label
    }
    fun stop() { runCatching{recorder.stop()} }
    fun release() { stop();effects.forEach{runCatching{it.release()}};recorder.release() }
    companion object {
        @SuppressLint("MissingPermission")
        fun open(context:Context):MicrophoneInput {
            val manager=context.getSystemService(AudioManager::class.java)
            val unprocessed=manager.getProperty(AudioManager.PROPERTY_SUPPORT_AUDIO_SOURCE_UNPROCESSED)=="true"
            val source=if(unprocessed)MediaRecorder.AudioSource.UNPROCESSED else MediaRecorder.AudioSource.VOICE_RECOGNITION
            val minimum=AudioRecord.getMinBufferSize(16000,AudioFormat.CHANNEL_IN_MONO,AudioFormat.ENCODING_PCM_16BIT)
            if(minimum<=0)throw IOException("手机不支持 16 kHz 单声道录音")
            val record=AudioRecord.Builder().setAudioSource(source).setAudioFormat(AudioFormat.Builder()
                .setSampleRate(16000).setChannelMask(AudioFormat.CHANNEL_IN_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                .setBufferSizeInBytes(maxOf(minimum*2,6400)).build()
            if(record.state!=AudioRecord.STATE_INITIALIZED) { record.release();throw IOException("无法初始化麦克风") }
            val effects=listOfNotNull(
                runCatching{AutomaticGainControl.create(record.audioSessionId)?.apply{enabled=false}}.getOrNull(),
                runCatching{NoiseSuppressor.create(record.audioSessionId)?.apply{enabled=false}}.getOrNull(),
                runCatching{AcousticEchoCanceler.create(record.audioSessionId)?.apply{enabled=false}}.getOrNull())
            return MicrophoneInput(record,source,effects)
        }
    }
}
