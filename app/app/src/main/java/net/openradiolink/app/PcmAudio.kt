package net.openradiolink.app

import android.annotation.SuppressLint
import android.content.Context
import android.media.*
import android.os.Build
import android.os.Handler
import android.os.Looper
import kotlinx.coroutines.*
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.channels.Channel
import okhttp3.Response
import okhttp3.WebSocket
import okhttp3.WebSocketListener
import okio.ByteString
import okio.ByteString.Companion.toByteString
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.nio.ByteBuffer
import java.nio.ByteOrder
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
import java.util.concurrent.atomic.AtomicLong

/** Raw mono PCM. Microphone amplitude never controls PTT. */
class PcmAudio(
    private val context: Context,
    private val api: BoardApi,
    private val sid: Long,
    private val realtime: Boolean=false,
    private val onActivity: (JSONObject)->Unit={},
    private val onLevel: (MicrophoneLevel)->Unit={},
    private val fault: (Throwable)->Unit,
) {
    private val scope=CoroutineScope(SupervisorJob()+Dispatchers.IO)
    private val playbackDispatcher=Executors.newSingleThreadExecutor { task ->
        Thread({android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);task.run()},"orl-playback")
    }.asCoroutineDispatcher()
    private val recordDispatcher=Executors.newSingleThreadExecutor { task ->
        Thread({android.os.Process.setThreadPriority(android.os.Process.THREAD_PRIORITY_AUDIO);task.run()},"orl-microphone")
    }.asCoroutineDispatcher()
    private val incoming=Channel<ByteArray>(4,BufferOverflow.DROP_OLDEST)
    private val acknowledgments=ConcurrentHashMap<Long,CompletableDeferred<JSONObject>>()
    private val sequence=AtomicLong(0)
    @Volatile private var closed=false
    @Volatile private var recording=false
    @Volatile private var transportReady=false
    @Volatile private var socket: WebSocket?=null
    @Volatile private var recorder: AudioRecord?=null
    private var microphoneInput:MicrophoneInput?=null
    private var recordJob: Job?=null
    private var senderJob: Job?=null
    private var outgoing: Channel<ByteArray>?=null
    private var track: AudioTrack?=null
    private val audioManager=context.getSystemService(AudioManager::class.java)
    private var focusRequest: AudioFocusRequest?=null
    private val focusListener=AudioManager.OnAudioFocusChangeListener { change ->
        if (change<0 && !closed) { stopMicrophone();fault(IOException("音频被其他应用占用，请稍后恢复收听")) }
    }
    var transport="连接中";private set
    @Volatile var lastActivityAt=0L;private set

    suspend fun open() {
        val focus=if (Build.VERSION.SDK_INT>=26) {
            val request=AudioFocusRequest.Builder(AudioManager.AUDIOFOCUS_GAIN)
                .setAudioAttributes(AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_MEDIA)
                    .setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
                .setOnAudioFocusChangeListener(focusListener,Handler(Looper.getMainLooper())).build()
            focusRequest=request;audioManager.requestAudioFocus(request)
        } else {
            @Suppress("DEPRECATION")
            audioManager.requestAudioFocus(focusListener,AudioManager.STREAM_MUSIC,AudioManager.AUDIOFOCUS_GAIN)
        }
        if (focus!=AudioManager.AUDIOFOCUS_REQUEST_GRANTED) throw IOException("暂时无法获得音频播放权限")
        val format=AudioFormat.Builder().setEncoding(AudioFormat.ENCODING_PCM_16BIT)
            .setSampleRate(16000).setChannelMask(AudioFormat.CHANNEL_OUT_MONO).build()
        val minimum=AudioTrack.getMinBufferSize(16000,AudioFormat.CHANNEL_OUT_MONO,AudioFormat.ENCODING_PCM_16BIT)
        if (minimum<=0) throw IOException("手机不支持所需播放格式")
        track=AudioTrack.Builder().setAudioAttributes(AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_MEDIA).setContentType(AudioAttributes.CONTENT_TYPE_SPEECH).build())
            .setAudioFormat(format).setBufferSizeInBytes(maxOf(minimum*2,8000)).setTransferMode(AudioTrack.MODE_STREAM).build()
        check(track?.state==AudioTrack.STATE_INITIALIZED) { "无法初始化手机播放" }
        track?.play()
        scope.launch(playbackDispatcher) {
            try {
                for (bytes in incoming) {
                    var offset=0
                    while (offset<bytes.size && !closed) {
                        val n=track?.write(bytes,offset,bytes.size-offset,AudioTrack.WRITE_BLOCKING) ?: break
                        if (n<=0) { if (!closed) throw IOException("手机播放输出中断");break }
                        offset+=n
                    }
                }
            } catch (e: Exception) { if (!closed && e !is CancellationException) fault(e) }
        }
        val opened=CompletableDeferred<Unit>()
        val candidate=api.audioSocket(sid,object:WebSocketListener() {
            override fun onOpen(webSocket: WebSocket,response: Response) {
                if (closed || opened.isCancelled) { webSocket.cancel();return }
                opened.complete(Unit)
            }
            override fun onMessage(webSocket: WebSocket,text: String) {
                if (closed) return
                runCatching { JSONObject(text) }.getOrNull()?.let { state ->
                    if(realtime && state.optString("type")=="activity") {
                        if(RadioActivitySample.parse(state,api.deviceId)!=null) {
                            lastActivityAt=android.os.SystemClock.elapsedRealtime();onActivity(state)
                        }
                    } else if (state.optLong("sid")==sid) acknowledgments.remove(state.optLong("id"))?.complete(state)
                }
            }
            override fun onMessage(webSocket: WebSocket,bytes: ByteString) {
                if (!closed && bytes.size%2==0) incoming.trySend(bytes.toByteArray())
            }
            override fun onClosing(webSocket: WebSocket,code: Int,reason: String) { webSocket.close(code,reason) }
            override fun onClosed(webSocket: WebSocket,code: Int,reason: String) {
                if (!closed && transportReady && socket===webSocket) fault(IOException("音频连接已关闭"))
            }
            override fun onFailure(webSocket: WebSocket,t: Throwable,response: Response?) {
                acknowledgments.values.forEach { it.completeExceptionally(t) };acknowledgments.clear()
                if (!opened.isCompleted) opened.completeExceptionally(t)
                else if (!closed && transportReady && socket===webSocket) fault(t)
            }
        },realtime)
        try {
            withTimeout(5000) { opened.await() }
            socket=candidate;transport="WebSocket";transportReady=true
            scope.launch {
                while (isActive && !closed) {
                    if (!recording && socket?.send(byteArrayOf(0).toByteString())!=true) {
                        fault(IOException("音频心跳发送失败"));break
                    }
                    delay(50)
                }
            }
        } catch (e: Exception) {
            opened.cancel();candidate.cancel()
            if (e is CertificateFailure || (e is CancellationException && e !is TimeoutCancellationException)) throw e
            transport="HTTP 兼容通道";transportReady=true
            scope.launch {
                try {
                    while (isActive && !closed) {
                        val bytes=api.bytes("api/sdr_rx?sid=$sid",16384)
                        if (bytes.size%2!=0) throw IOException("收听数据格式不正确")
                        if (bytes.isNotEmpty()) incoming.trySend(bytes)
                        delay(if (bytes.isEmpty()) 40 else if (bytes.size>=8192) 1 else 20)
                    }
                } catch (problem: Exception) { if (!closed && problem !is CancellationException) fault(problem) }
            }
        }
    }

    suspend fun ptt(on: Boolean,tx: Long): JSONObject {
        val id=sequence.incrementAndGet()
        val ws=socket
        val reply=if (ws!=null && transportReady) {
            val pending=CompletableDeferred<JSONObject>();acknowledgments[id]=pending
            try {
                if (!ws.send("ptt${if(on)1 else 0}:$sid:$tx:$id")) throw IOException("PTT 指令发送失败")
                withTimeout(3000) { pending.await() }
            } finally { acknowledgments.remove(id) }
        } else api.sdrPtt(sid,tx,on,id,realtime)
        RadioStateRules.validatePtt(reply,sid,tx,id)
        if(realtime)reply.optJSONObject("activity")?.let(onActivity)
        return reply
    }

    /**
     * Send PTT ON/OFF over the WebSocket without waiting for the ACK.
     * Returns the correlation id, or 0 when there is no live WebSocket.
     * Call [awaitPtt] later; this lets the caller start the microphone (and
     * release the PTT lock) without a 3 s ACK hold.
     */
    fun sendPtt(on: Boolean,tx: Long): Long {
        val ws=socket ?: return 0L
        if(!transportReady) return 0L
        val id=sequence.incrementAndGet()
        val pending=CompletableDeferred<JSONObject>()
        acknowledgments[id]=pending
        return try {
            if (!ws.send("ptt${if(on)1 else 0}:$sid:$tx:$id")) throw IOException("PTT 指令发送失败")
            id
        } catch (_: Exception) {
            acknowledgments.remove(id);0L
        }
    }

    suspend fun awaitPtt(id: Long, sid: Long, tx: Long): JSONObject {
        try {
            val reply=withTimeout(3000) {
                acknowledgments[id]?.await() ?: throw IOException("PTT 应答丢失")
            }
            RadioStateRules.validatePtt(reply,sid,tx,id)
            return reply
        } finally { acknowledgments.remove(id) }
    }

    @SuppressLint("MissingPermission")
    fun startMicrophone(tx: Long) {
        check(!closed && !recording)
        val input=MicrophoneInput.open(context);microphoneInput=input
        val record=input.recorder
        recorder=record;recording=true
        val queue=Channel<ByteArray>(16);outgoing=queue
        if (socket==null) senderJob=scope.launch {
            try {
                for (first in queue) {
                    val out=ByteArrayOutputStream(8192);out.write(first)
                    while (out.size()+640<=8192) out.write(queue.tryReceive().getOrNull() ?: break)
                    api.sendPcm(sid,tx,out.toByteArray())
                }
            } catch (e: Exception) { if (!closed && recording && e !is CancellationException) fault(e) }
        }
        record.startRecording()
        recordJob=scope.launch(recordDispatcher) {
            val samples=ShortArray(320)
            val settings=MicrophoneSettings(context)
            var frozen:MicrophoneGain?=null
            var identity:Pair<String,String>?=null
            var lastLevel=0L
            try {
                while (isActive && recording && !closed) {
                    val count=record.read(samples,0,samples.size,AudioRecord.READ_BLOCKING)
                    if (count<=0) { if (recording) throw IOException("麦克风输入中断");break }
                    val actual=input.identity()
                    if(identity==null) {
                        identity=actual
                        settings.remember(actual.first,actual.second)
                        frozen=MicrophoneGain(settings.gain(actual.first))
                        onLevel(MicrophoneLevel(gainDb=frozen.gainDb,input=actual.second))
                    } else if(identity.first!=actual.first)throw IOException("麦克风输入设备已切换，请核对音量后重新发话")
                    val level=frozen!!.process(samples,count,actual.second)
                    val now=android.os.SystemClock.elapsedRealtime()
                    if(now-lastLevel>=100){lastLevel=now;onLevel(level)}
                    val ws=socket
                    val data=if(ws!=null)microphonePacket(samples,count,sid,tx) else microphonePacket(samples,count)
                    if (ws!=null) {
                        if (ws.queueSize()>32768 || !ws.send(data.toByteString())) throw IOException("发话网络积压，已停止麦克风")
                    } else if (queue.trySend(data).isFailure) throw IOException("发话网络过慢，已停止麦克风")
                }
            } catch (e: Exception) { if (!closed && recording && e !is CancellationException) fault(e) }
        }
    }
    fun stopMicrophone() {
        recording=false
        runCatching { recorder?.stop() }
        recordJob?.cancel();outgoing?.close();senderJob?.cancel()
    }

    suspend fun finishMicrophone() {
        /* The board owns the courtesy tone: it plays its configured tail after
         * the last PCM and mirrors it into the network stream.  The phone only
         * stops capturing and lets the sender drain. */
        stopMicrophone();recordJob?.join();senderJob?.join()
        runCatching { microphoneInput?.release() };microphoneInput=null;recorder=null
        recordJob=null;senderJob=null;outgoing=null
    }
    suspend fun close() {
        if (closed) return
        closed=true;transportReady=false
        finishMicrophone()
        socket?.let { it.close(1000,"session closed");it.cancel() };socket=null
        acknowledgments.values.forEach { it.cancel() };acknowledgments.clear()
        incoming.close();scope.cancel()
        runCatching { track?.pause();track?.flush();track?.release() };track=null
        if (Build.VERSION.SDK_INT>=26) focusRequest?.let { audioManager.abandonAudioFocusRequest(it) }
        else { @Suppress("DEPRECATION") audioManager.abandonAudioFocus(focusListener) }
        playbackDispatcher.close();recordDispatcher.close()
    }
}
