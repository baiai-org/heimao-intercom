package net.openradiolink.app

import android.content.Context
import android.net.*
import android.net.wifi.WifiNetworkSpecifier
import android.os.Build
import android.os.PatternMatcher
import java.util.concurrent.atomic.AtomicLong

class ProvisionNetwork(context: Context) {
    private val connectivity=context.getSystemService(ConnectivityManager::class.java)
    private var callback: ConnectivityManager.NetworkCallback?=null
    private val generation=AtomicLong()
    @Volatile var network: Network?=null;private set
    fun availableWifi(): Network? = connectivity.allNetworks.firstOrNull {
        connectivity.getNetworkCapabilities(it)?.hasTransport(NetworkCapabilities.TRANSPORT_WIFI)==true
    }
    fun request(ready: (Network)->Unit,lost: (Network)->Unit,failed: (String)->Unit) {
        if(Build.VERSION.SDK_INT<29)throw IllegalStateException("此系统请在 Wi-Fi 设置中手动连接手台热点")
        close();val epoch=generation.incrementAndGet()
        val specification=WifiNetworkSpecifier.Builder().setSsidPattern(PatternMatcher(Brand.SETUP_PREFIX,PatternMatcher.PATTERN_PREFIX)).build()
        val request=NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).setNetworkSpecifier(specification).build()
        val callbacks=object:ConnectivityManager.NetworkCallback() {
            override fun onAvailable(value: Network) { if(epoch==generation.get()){network=value;ready(value)} }
            override fun onLost(value: Network) {
                if(epoch==generation.get() && network==value){network=null;lost(value)}
            }
            override fun onUnavailable() { if(epoch==generation.get())failed("未连接手台配网热点，可重新选择或在系统 Wi-Fi 设置中连接") }
        }
        callback=callbacks
        try { connectivity.requestNetwork(request,callbacks) }
        catch (_: SecurityException) { close();failed("连接配网热点需要附近 Wi-Fi 或位置信息权限") }
        catch (_: RuntimeException) { close();failed("系统暂时无法请求配网连接，请检查 Wi-Fi 后重试") }
    }
    // Android 7–9: the operator selects Wi-Fi in Settings. Observe that exact
    // network so its sockets are discarded on loss, without adopting another Wi-Fi.
    fun observe(value: Network,ready: (Network)->Unit,lost: (Network)->Unit) {
        close();val epoch=generation.incrementAndGet()
        val callbacks=object:ConnectivityManager.NetworkCallback() {
            override fun onAvailable(candidate: Network) {
                if(epoch==generation.get() && candidate==value){network=candidate;ready(candidate)}
            }
            override fun onLost(candidate: Network) {
                if(epoch==generation.get() && network==candidate){network=null;lost(candidate)}
            }
        }
        callback=callbacks
        connectivity.registerNetworkCallback(NetworkRequest.Builder().addTransportType(NetworkCapabilities.TRANSPORT_WIFI)
            .removeCapability(NetworkCapabilities.NET_CAPABILITY_INTERNET).build(),callbacks)
    }
    fun close() {
        generation.incrementAndGet();callback?.let { runCatching { connectivity.unregisterNetworkCallback(it) } }
        callback=null;network=null
    }
}
