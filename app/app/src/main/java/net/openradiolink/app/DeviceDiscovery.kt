package net.openradiolink.app

import android.content.Context
import android.net.nsd.NsdManager
import android.net.nsd.NsdServiceInfo
import android.net.wifi.WifiManager
import kotlinx.coroutines.*
import org.json.JSONObject
import java.net.DatagramPacket
import java.net.DatagramSocket
import java.net.Inet4Address
import java.net.InetSocketAddress
import java.net.SocketTimeoutException
import java.util.concurrent.atomic.AtomicLong

class DeviceDiscovery(context: Context,private val found: (DiscoveredBoard)->Unit,private val problem: (String)->Unit) {
    private val nsd=context.getSystemService(NsdManager::class.java)
    private val wifi=context.applicationContext.getSystemService(WifiManager::class.java)
    private var lock: WifiManager.MulticastLock?=null
    private var listener: NsdManager.DiscoveryListener?=null
    @Volatile private var socket: DatagramSocket?=null
    private var job: Job?=null
    private val generation=AtomicLong()
    @Suppress("DEPRECATION")
    fun start() {
        if (listener!=null) return
        val epoch=generation.incrementAndGet()
        try {
            lock=wifi.createMulticastLock("orl-discovery").apply { setReferenceCounted(false);acquire() }
            val callbacks=object:NsdManager.DiscoveryListener {
                override fun onDiscoveryStarted(type: String) {}
                override fun onDiscoveryStopped(type: String) {}
                override fun onStartDiscoveryFailed(type: String,code: Int) { problem("局域网发现暂不可用（$code），仍可手动输入地址") }
                override fun onStopDiscoveryFailed(type: String,code: Int) {}
                override fun onServiceLost(service: NsdServiceInfo) {}
                override fun onServiceFound(service: NsdServiceInfo) {
                    if (epoch!=generation.get()) return
                    nsd.resolveService(service,object:NsdManager.ResolveListener {
                        override fun onResolveFailed(info: NsdServiceInfo,code: Int) {}
                        override fun onServiceResolved(info: NsdServiceInfo) {
                            if (epoch!=generation.get()) return
                            val id=info.attributes["device_id"]?.toString(Charsets.UTF_8) ?: return
                            val host=info.host ?: return
                            val address=host.hostAddress ?: return
                            if (host !is Inet4Address && (host.isLinkLocalAddress || address.contains('%'))) return
                            if (id.length>64 || info.port !in 1..65535) return
                            val literal=if (address.contains(':')) "[$address]" else address
                            val name=info.attributes["custom_name"]?.toString(Charsets.UTF_8) ?: ""
                            found(DiscoveredBoard(id,"http://$literal:${info.port}/",HandsetName.discovery(name,id),"局域网发现"))
                        }
                    })
                }
            }
            listener=callbacks
            nsd.discoverServices("_openradiolink._tcp.",NsdManager.PROTOCOL_DNS_SD,callbacks)
            job=CoroutineScope(SupervisorJob()+Dispatchers.IO).launch {
                var receiver: DatagramSocket?=null
                try {
                    receiver=DatagramSocket(null).apply { reuseAddress=true;bind(InetSocketAddress(54322));soTimeout=1000 }
                    socket=receiver
                    val bytes=ByteArray(2048)
                    while (isActive && epoch==generation.get()) {
                        val packet=DatagramPacket(bytes,bytes.size)
                        try { receiver.receive(packet) } catch (_: SocketTimeoutException) { continue }
                        val data=runCatching { JSONObject(String(packet.data,packet.offset,packet.length,Charsets.UTF_8)) }.getOrNull() ?: continue
                        if (data.optString("service")!="OpenRadioLink" || data.optInt("api")!=2) continue
                        val id=data.optString("device_id");val host=packet.address.hostAddress ?: continue
                        if (id.isBlank() || id.length>64 || packet.address !is Inet4Address) continue
                        val port=data.optInt("http",80);if(port !in 1..65535)continue
                        found(DiscoveredBoard(id,"http://$host:$port/",HandsetName.discovery(data.optString("custom_name"),id),"手机热点通告"))
                    }
                } catch (e: Exception) { if (epoch==generation.get() && e !is CancellationException) problem("热点发现不可用，可手动输入手台地址") }
                finally { receiver?.close();if(socket===receiver)socket=null }
            }
        } catch (_: SecurityException) { stop();problem("请先授予局域网或附近 Wi-Fi 权限") }
    }
    fun stop() {
        generation.incrementAndGet();job?.cancel();job=null;socket?.close();socket=null
        listener?.let { runCatching { nsd.stopServiceDiscovery(it) } };listener=null
        lock?.let { if(it.isHeld)it.release() };lock=null
    }
}
