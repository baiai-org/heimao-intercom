package net.openradiolink.app

import android.annotation.SuppressLint
import android.app.*
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.*
import androidx.core.app.NotificationCompat
import androidx.core.app.ServiceCompat

class SessionService:Service() {
    private val manager get()=(application as OrlApp).manager
    private var microphone=false;private var playback=false
    private var wake: PowerManager.WakeLock?=null
    private var lastDisplay=0L
    override fun onBind(intent: Intent?)=null
    @SuppressLint("WakelockTimeout")
    override fun onCreate() {
        super.onCreate();manager.service=this
        if(Build.VERSION.SDK_INT>=26)getSystemService(NotificationManager::class.java)
            .createNotificationChannel(NotificationChannel("session","当前设备连接与收听",NotificationManager.IMPORTANCE_LOW))
        wake=getSystemService(PowerManager::class.java).newWakeLock(PowerManager.PARTIAL_WAKE_LOCK,"openradiolink:session").apply { acquire() }
        show(true)
        // A START_STICKY restart arrives with a null intent and no session in memory.
        manager.restoreAfterRestart()
    }
    override fun onStartCommand(intent: Intent?,flags: Int,startId: Int): Int {
        if(intent?.action=="STOP")manager.disconnect(intent.getStringExtra("profile") ?: "")
        return when(KeepAliveRules.restartMode(KeepAlive.persistent(this))) {
            KeepAliveRules.Restart.STICKY->START_STICKY
            KeepAliveRules.Restart.NOT_STICKY->START_NOT_STICKY
        }
    }
    /** Swiping the task away is where MIUI/ColorOS usually kill the process. */
    override fun onTaskRemoved(rootIntent: Intent?) {
        if(KeepAlive.persistent(this))show(true)
        else if(manager.view.value.profileId.isBlank())stopSelf()
        super.onTaskRemoved(rootIntent)
    }
    @Synchronized fun microphone(value: Boolean) { microphone=value;show(true) }
    @Synchronized fun playback(value: Boolean) { playback=value;show(true) }
    fun display() { show(false) }
    @Synchronized private fun show(force: Boolean) {
        val now=SystemClock.elapsedRealtime()
        if(!force && now-lastDisplay<500)return
        lastDisplay=now
        val state=manager.view.value
        val profile=manager.store.profiles.value.firstOrNull{it.id==state.profileId}
        val standalone=profile==null && KeepAlive.persistent(this)
        val open=PendingIntent.getActivity(this,0,Intent(this,MainActivity::class.java),PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val stop=PendingIntent.getService(this,1,Intent(this,SessionService::class.java).setAction("STOP").putExtra("profile",state.profileId),PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        val notification=NotificationCompat.Builder(this,"session").setSmallIcon(R.drawable.ic_notification)
            .setContentTitle(if(standalone)"${Brand.APP} · 常驻前台运行中" else "${Brand.APP} · ${profile?.displayName ?: "正在连接"}")
            .setContentText(if(standalone)"等待连接手台；锁屏和后台不会被立即冻结"
                else (if(state.ptt)"正在发话" else if(state.listening)"正在收听" else state.phase)+" · "+(profile?.endpoint()?.address ?: ""))
            .setContentIntent(open).setOngoing(true).setOnlyAlertOnce(true).setCategory(NotificationCompat.CATEGORY_SERVICE)
            .addAction(0,"断开当前手台",stop).build()
        val types=if(Build.VERSION.SDK_INT>=29)ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE or
            (if(playback)ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PLAYBACK else 0) or
            (if(microphone && Build.VERSION.SDK_INT>=30)ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else 0) else 0
        ServiceCompat.startForeground(this,42,notification,types)
    }
    override fun onDestroy() {
        manager.service=null
        manager.serviceStopped()
        wake?.let{if(it.isHeld)it.release()};wake=null
        super.onDestroy()
    }
}
