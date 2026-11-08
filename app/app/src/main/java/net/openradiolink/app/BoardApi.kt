package net.openradiolink.app

import android.net.Network
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.*
import okhttp3.HttpUrl.Companion.toHttpUrl
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody.Companion.toRequestBody
import okio.BufferedSink
import org.json.JSONObject
import java.io.ByteArrayOutputStream
import java.io.IOException
import java.security.KeyStore
import java.security.MessageDigest
import java.security.cert.X509Certificate
import java.util.concurrent.TimeUnit
import java.util.concurrent.atomic.AtomicReference
import javax.net.ssl.SSLContext
import javax.net.ssl.TrustManagerFactory
import javax.net.ssl.X509TrustManager
import kotlin.coroutines.resume
import kotlin.coroutines.resumeWithException

class ApiFailure(val status: Int,message: String): IOException(message)
class ControlHeld(val holder: String): IOException(if (holder.isBlank()) "控制权由其他客户端持有" else "控制权由「$holder」持有，可点“接管”")
class CertificateFailure(val host: String,val fingerprint: String,val audio: Boolean): IOException("需要确认 $host 的服务端证书")
data class HttpReply(val code: Int,val body: ByteArray)

class BoardApi(val endpoint: AccessPoint,private val network: Network? = null) {
    val base=EndpointAddress.parse(endpoint.address)
    @Volatile var deviceId=""
    @Volatile var token=""
    private val requests=Mutex()
    private val activityRequests=Mutex()
    private val setupRequests=Mutex()
    private val pttRequests=Mutex()
    private val observed=AtomicReference<String?>(null)
    private val audioObserved=AtomicReference<String?>(null)
    private val client=createClient(endpoint.certificate,observed)
    private val audioUrl=if (endpoint.audioAddress.isBlank()) EndpointAddress.api(base,"ws/sdr") else
        endpoint.audioAddress.replaceFirst("wss://","https://").replaceFirst("ws://","http://").toHttpUrl()
    private val audioClient=if (endpoint.audioAddress.isBlank()) client else createClient(endpoint.audioCertificate,audioObserved)

    private fun createClient(pin: String,seen: AtomicReference<String?>): OkHttpClient {
        val defaults=TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm()).apply { init(null as KeyStore?) }
            .trustManagers.filterIsInstance<X509TrustManager>().first()
        val trust=object:X509TrustManager {
            override fun getAcceptedIssuers()=defaults.acceptedIssuers
            override fun checkClientTrusted(chain: Array<X509Certificate>,auth: String)=defaults.checkClientTrusted(chain,auth)
            override fun checkServerTrusted(chain: Array<X509Certificate>,auth: String) {
                require(chain.isNotEmpty())
                val actual=sha256(chain[0].encoded);seen.set(actual)
                if (pin.isNotBlank()) {
                    if (!actual.equals(pin.replace(":", ""),true)) throw java.security.cert.CertificateException("绑定证书已变化")
                    chain[0].checkValidity()
                } else defaults.checkServerTrusted(chain,auth)
            }
        }
        val ssl=SSLContext.getInstance("TLS").apply { init(null,arrayOf(trust),null) }
        return OkHttpClient.Builder().connectTimeout(5,TimeUnit.SECONDS).readTimeout(10,TimeUnit.SECONDS)
            .writeTimeout(15,TimeUnit.SECONDS).callTimeout(15,TimeUnit.SECONDS)
            .retryOnConnectionFailure(false).followRedirects(false).followSslRedirects(false)
            .sslSocketFactory(ssl.socketFactory,trust).apply {
                if (pin.isNotBlank()) hostnameVerifier { _,session ->
                    runCatching { sha256(session.peerCertificates[0].encoded).equals(pin.replace(":",""),true) }.getOrDefault(false)
                }
                if (network!=null) {
                    socketFactory(network.socketFactory)
                    dns { host -> network.getAllByName(host).toList() }
                }
            }.build()
    }
    private fun authorization(audio: Boolean): String {
        if (audio && endpoint.audioAddress.isNotBlank() && (audioUrl.scheme!=base.scheme || audioUrl.host!=base.host || audioUrl.port!=base.port)) return endpoint.audioAuthorization
        return when {
            endpoint.bearer.isNotBlank() -> "Bearer ${endpoint.bearer}"
            endpoint.username.isNotBlank() -> Credentials.basic(endpoint.username,endpoint.password,Charsets.UTF_8)
            else -> ""
        }
    }
    private fun builder(url: HttpUrl,audio: Boolean=false)=Request.Builder().url(url).apply {
        header("Cache-Control","no-store")
        if (deviceId.isNotBlank()) header("X-ORL-Device",deviceId)
        if (token.isNotBlank()) header("X-ORL-Control",token)
        authorization(audio).takeIf { it.isNotBlank() }?.let { header("Authorization",it) }
    }
    private suspend fun execute(request: Request,limit: Int,upload: Boolean=false): HttpReply {
        // Keep settings/PCM serialized, while PTT confirmations are not queued
        // behind an unrelated slow configuration download.
        val lane=if(request.url.pathSegments.last() in listOf("sdr_ptt","sdr_state"))pttRequests else requests
        return lane.withLock { executeOnce(request,limit,upload) }
    }
    private suspend fun executeOnce(request: Request,limit: Int,upload: Boolean): HttpReply = suspendCancellableCoroutine { continuation ->
        val transport=if (upload) client.newBuilder().callTimeout(240,TimeUnit.SECONDS).readTimeout(45,TimeUnit.SECONDS).build() else client
        val call=transport.newCall(request)
        continuation.invokeOnCancellation { call.cancel() }
        call.enqueue(object:Callback {
            override fun onFailure(call: Call,e: IOException) {
                val error=if (e is javax.net.ssl.SSLException && observed.get()!=null) CertificateFailure(base.host,observed.get()!!,false) else e
                if (continuation.isActive) continuation.resumeWithException(error)
            }
            override fun onResponse(call: Call,response: Response) {
                response.use {
                    try {
                        val body=it.body
                        if (body.contentLength()>limit) throw IOException("设备响应超过限制")
                        val out=ByteArrayOutputStream();val buffer=ByteArray(8192)
                        body.byteStream().use { stream ->
                            while (true) {
                                val n=stream.read(buffer);if (n<0) break
                                if (out.size()+n>limit) throw IOException("设备响应超过限制")
                                out.write(buffer,0,n)
                            }
                        }
                        if (continuation.isActive) continuation.resume(HttpReply(it.code,out.toByteArray()))
                    } catch (e: Exception) { if (continuation.isActive) continuation.resumeWithException(e) }
                }
            }
        })
    }
    suspend fun bytes(path: String,limit: Int=4*1024*1024): ByteArray {
        val result=execute(builder(EndpointAddress.api(base,path)).build(),limit,path.startsWith("api/firmware.bin"))
        if (result.code !in 200..299) throw failure(result)
        return result.body
    }
    suspend fun json(path: String,body: JSONObject?=null): JSONObject = jsonReply(path,body,false)
    /** Takes the control lease for this phone; a free or expired lease is simply claimed. */
    /** hold:true marks a user-initiated acquisition; the board then refuses to let
     * automatic clients displace it, while an explicit takeover always wins. */
    suspend fun claim(client: String,takeover: Boolean,hold: Boolean=true):JSONObject = json("api/control",JSONObject()
        .put("op",if(takeover)"takeover" else "claim").put("client",client).put("label",Brand.APP)
        .put("device_id",deviceId).put("hold",hold))
    // PTT rejection bodies carry sid/tx/id and reason; keep them for correlated validation.
    suspend fun sdrPtt(sid:Long,tx:Long,on:Boolean,id:Long,activity:Boolean=false):JSONObject {
        require(sid>0 && tx>0 && id>0)
        return jsonReply("api/sdr_ptt?sid=$sid&tx=$tx&on=${if(on)1 else 0}&id=$id"+(if(activity)"&activity=1" else ""),null,true)
    }
    suspend fun activity():JSONObject=activityRequests.withLock { jsonReply("api/activity",null,false,true) }
    suspend fun setupSession(owner:String,release:Boolean=false):JSONObject=setupRequests.withLock {
        jsonReply("api/wifi/setup_session",JSONObject().put("op",if(release)"release" else "renew").put("session",owner),false,true)
    }
    private suspend fun jsonReply(path:String,body:JSONObject?,allowRejected:Boolean,independent:Boolean=false):JSONObject {
        val request=builder(EndpointAddress.api(base,path)).apply {
            if (body!=null) post(body.toString().toRequestBody("application/json; charset=utf-8".toMediaType()))
        }.build()
        val reply=if(independent)executeOnce(request,4096,false) else execute(request,256*1024)
        if (reply.code !in 200..299) throw failure(reply)
        val result=JSONObject(String(reply.body,Charsets.UTF_8))
        if (!allowRejected && result.has("ok") && !result.optBoolean("ok")) throw ApiFailure(reply.code,result.optString("err","手台拒绝了操作"))
        return result
    }
    suspend fun upload(path: String,data: ByteArray,onProgress: (Int)->Unit): JSONObject {
        val body=object:RequestBody() {
            override fun contentType()="application/octet-stream".toMediaType()
            override fun contentLength()=data.size.toLong()
            override fun writeTo(sink: BufferedSink) {
                var written=0
                while (written<data.size) {
                    val n=minOf(8192,data.size-written);sink.write(data,written,n);written+=n
                    onProgress((written.toLong()*100/data.size).toInt())
                }
            }
        }
        val reply=execute(builder(EndpointAddress.api(base,path)).post(body).build(),8192,true)
        if (reply.code !in 200..299) throw failure(reply)
        val result=JSONObject(String(reply.body,Charsets.UTF_8))
        if (!result.optBoolean("ok")) throw ApiFailure(reply.code,result.optString("err","上传失败"))
        return result
    }
    suspend fun sendPcm(sid: Long,tx: Long,data: ByteArray) {
        val request=builder(EndpointAddress.api(base,"api/sdr_tx?sid=$sid&tx=$tx"))
            .post(data.toRequestBody("application/octet-stream".toMediaType())).build()
        val reply=execute(request,1024)
        if (reply.code !in 200..299) throw failure(reply)
    }
    fun audioSocket(sid: Long,listener: WebSocketListener,activity:Boolean=false): WebSocket {
        val url=audioUrl.newBuilder().addQueryParameter("sid",sid.toString()).apply{if(activity)addQueryParameter("activity","1")}.build()
        val request=builder(url,true).build()
        return audioClient.newWebSocket(request,object:WebSocketListener() {
            override fun onOpen(webSocket: WebSocket,response: Response)=listener.onOpen(webSocket,response)
            override fun onMessage(webSocket: WebSocket,text: String)=listener.onMessage(webSocket,text)
            override fun onMessage(webSocket: WebSocket,bytes: okio.ByteString)=listener.onMessage(webSocket,bytes)
            override fun onClosing(webSocket: WebSocket,code: Int,reason: String)=listener.onClosing(webSocket,code,reason)
            override fun onClosed(webSocket: WebSocket,code: Int,reason: String)=listener.onClosed(webSocket,code,reason)
            override fun onFailure(webSocket: WebSocket,t: Throwable,response: Response?) {
                val seen=if (endpoint.audioAddress.isBlank()) observed.get() else audioObserved.get()
                val error=if (t is javax.net.ssl.SSLException && seen!=null) CertificateFailure(audioUrl.host,seen,endpoint.audioAddress.isNotBlank()) else t
                listener.onFailure(webSocket,error,response)
            }
        })
    }
    fun close() {
        client.dispatcher.cancelAll();client.connectionPool.evictAll()
        if (audioClient!==client) { audioClient.dispatcher.cancelAll();audioClient.connectionPool.evictAll() }
    }
    private fun failure(reply: HttpReply): ApiFailure {
        val message=runCatching { JSONObject(String(reply.body,Charsets.UTF_8)).optString("err") }.getOrNull()
        return ApiFailure(reply.code,message?.takeIf { it.isNotBlank() } ?: "连接返回 HTTP ${reply.code}")
    }
    companion object {
        fun sha256(bytes: ByteArray)=MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it.toInt() and 255) }
    }
}
