package net.openradiolink.app

import android.content.Context
import android.media.*
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.update
import java.io.IOException

data class MicrophoneTestView(val running:Boolean=false,val playing:Boolean=false,val complete:Boolean=false,
    val key:String="",val input:String="尚未测试麦克风",val level:MicrophoneLevel=MicrophoneLevel(),val error:String="")

/** Foreground-only local RAM recording. No BoardApi, socket, file or PTT. */
class LocalMicrophoneTest(private val context:Context,private val scope:CoroutineScope) {
    val view=MutableStateFlow(MicrophoneTestView())
    @Volatile private var input:MicrophoneInput?=null
    @Volatile private var track:AudioTrack?=null
    private var job:Job?=null
    @Volatile private var clip:ShortArray?=null
    private val epoch=java.util.concurrent.atomic.AtomicLong()
    private var clipGain=0
    fun record(gain:Int) {
        if(job?.isCompleted==false)return
        val ticket=epoch.incrementAndGet()
        clip?.fill(0);clip=null;clipGain=gain;view.value=MicrophoneTestView(running=true)
        job=scope.launch(Dispatchers.IO) {
            var capture:MicrophoneInput?=null
            val recorded=ShortArray(16000*5)
            try {
                capture=MicrophoneInput.open(context);input=capture
                capture.recorder.startRecording()
                val samples=ShortArray(320)
                var offset=0;var identity:Pair<String,String>?=null
                val process=MicrophoneGain(gain)
                while(isActive && offset<recorded.size) {
                    val n=capture.recorder.read(samples,0,minOf(samples.size,recorded.size-offset),AudioRecord.READ_BLOCKING)
                    ensureActive()
                    if(n<=0)throw IOException("麦克风测试中断")
                    val actual=capture.identity()
                    if(identity==null) { identity=actual;MicrophoneSettings(context).remember(actual.first,actual.second) }
                    else if(identity.first!=actual.first)throw IOException("输入设备已切换，请重新测试")
                    val level=process.process(samples,n,actual.second)
                    samples.copyInto(recorded,offset,0,n);offset+=n
                    if(offset%1600==0 && epoch.get()==ticket)view.update{it.copy(key=actual.first,input=actual.second,level=level)}
                }
                ensureActive()
                if(epoch.get()==ticket) { clip=recorded;view.update{it.copy(running=false,complete=true)} }
            } catch(e:CancellationException) { throw e }
            catch(e:Exception) { if(epoch.get()==ticket)view.update{it.copy(running=false,complete=false,error=e.message ?: "麦克风测试失败")} }
            finally { capture?.release();input=null;if(clip!==recorded)recorded.fill(0) }
        }
    }
    fun play() {
        if(job?.isCompleted==false)return
        val pcm=clip ?: return
        view.update{it.copy(playing=true,error="")}
        job=scope.launch(Dispatchers.IO) {
            var output:AudioTrack?=null
            try {
                output=AudioTrack.Builder().setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build()).setAudioFormat(AudioFormat.Builder()
                    .setSampleRate(16000).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).setEncoding(AudioFormat.ENCODING_PCM_16BIT).build())
                    .setBufferSizeInBytes(pcm.size*2).setTransferMode(AudioTrack.MODE_STATIC).build()
                track=output
                check(output.state==AudioTrack.STATE_INITIALIZED) { "试听播放不可用" }
                check(output.write(pcm,0,pcm.size)==pcm.size) { "试听数据写入失败" }
                output.play();delay(5100)
            } catch(e:CancellationException) { throw e }
            catch(e:Exception) { view.update{it.copy(error=e.message ?: "试听失败")} }
            finally { runCatching{output?.stop();output?.release()};track=null;view.update{it.copy(playing=false)} }
        }
    }
    fun invalidate() { if(job?.isCompleted!=false){clip?.fill(0);clip=null;view.update{it.copy(complete=false)}} }
    fun recordedGain()=clipGain
    fun stop() {
        epoch.incrementAndGet()
        input?.stop();runCatching{track?.stop()};job?.cancel()
        clip?.fill(0);clip=null
        view.update{it.copy(running=false,playing=false,complete=false)}
    }
}
