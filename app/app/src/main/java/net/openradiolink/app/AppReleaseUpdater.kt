package net.openradiolink.app

import android.Manifest
import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.pm.PackageInfo
import android.content.pm.PackageInstaller
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import android.util.AtomicFile
import java.io.File
import java.io.IOException
import java.lang.ref.WeakReference
import java.security.MessageDigest
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import okhttp3.HttpUrl
import okhttp3.OkHttpClient
import okhttp3.Request
import okhttp3.Response
import okio.ByteString.Companion.toByteString

internal data class AppUpdateView(val message:String="等待检查更新",val asset:ReleaseAsset?=null,
    val ready:Boolean=false,val busy:Boolean=false,val systemConfirmation:Boolean=false)

internal class AppReleaseUpdater(private val app:OrlApp) {
    private val gate=Mutex()
    private val state=MutableStateFlow(AppUpdateView())
    val view=state.asStateFlow()
    private val dir=app.noBackupFilesDir.resolve("release-update").apply{mkdirs()}
    private val policy=AtomicFile(dir.resolve("update.json"))
    private val prefs=app.getSharedPreferences("release-update",Context.MODE_PRIVATE)
    private val notifications=app.getSystemService(NotificationManager::class.java)
    private val installer=app.packageManager.packageInstaller
    private val client=OkHttpClient.Builder().followRedirects(false).followSslRedirects(false)
        .callTimeout(100,TimeUnit.SECONDS).readTimeout(20,TimeUnit.SECONDS).build()
    private var lastCompleted=0L
    private var restored=false
    private var launched=false
    private var candidate:ReleaseAsset?=null
    private var confirmation:Intent?=null
    private var waitingForSystem=false
    private var installing=false
    private var foregroundInstall:WeakReference<Context>?=null

    private fun apk(asset:ReleaseAsset)=dir.resolve(asset.sha256+".apk")

    fun launchCheck() {
        if(launched)return
        launched=true
        app.manager.scope.launch { withContext(Dispatchers.IO){gate.withLock{restore()}} }
        schedule(DAILY)
        schedule(NOW)
    }

    fun checkNow() {
        app.manager.scope.launch { if(!check(manual=true))schedule(NOW) }
    }

    private fun restore() {
        if(restored)return
        restored=true
        candidate=runCatching {
            decode(policy.openRead().use{it.readNBytesBounded(ReleaseUpdateManifest.maxManifest+1)})
                ?.takeIf{verified(apk(it),it)}
        }.getOrNull()
        if(candidate==null) {
            abandonWaiting()
            dir.listFiles()?.filter{it.extension=="apk" || it.extension=="part"}?.forEach{it.delete()}
            prefs.edit().remove("attempt").apply()
        } else {
            waitingForSystem=prefs.getString("attempt","")==candidate?.sha256
            ready(if(waitingForSystem)"等待系统确认，可继续安装" else "新版已下载")
        }
    }

    private fun ready(message:String) {
        state.value=AppUpdateView(message,candidate,ready=candidate!=null,systemConfirmation=waitingForSystem)
    }
    private fun decode(bytes:ByteArray)=ReleaseUpdateManifest.decode(bytes,app.packageName,BuildConfig.VERSION_CODE)

    private fun request(url:HttpUrl):Response {
        var next=url
        repeat(6) {
            require(ReleaseUpdateManifest.allowed(next)){"更新下载地址无效"}
            val response=client.newCall(Request.Builder().url(next)
                .header("Cache-Control","no-cache").header("Accept-Encoding","identity").build()).execute()
            if(response.code !in listOf(301,302,303,307,308))return response
            response.use {
                next=response.header("Location")?.let(next::resolve) ?: throw IOException("下载重定向地址缺失")
            }
        }
        throw IOException("下载重定向次数过多")
    }

    private fun fetchPolicy():ByteArray? = request(ReleaseUpdateManifest.latest).use { response->
        if(response.code==404)return null
        if(!response.isSuccessful)throw IOException("GitHub 返回 "+response.code)
        checkNotNull(response.body).byteStream().readNBytesBounded(ReleaseUpdateManifest.maxManifest+1)
    }

    private fun persistPolicy(bytes:ByteArray) {
        val output=policy.startWrite()
        try { output.write(bytes);policy.finishWrite(output) }
        catch(e:IOException){policy.failWrite(output);throw e}
    }

    suspend fun check(manual:Boolean=false):Boolean=withContext(Dispatchers.IO) {
        gate.withLock {
            restore()
            if(installing)return@withLock true
            val started=android.os.SystemClock.elapsedRealtime()
            if(!manual && lastCompleted!=0L && started-lastCompleted<60_000)return@withLock true
            state.value=AppUpdateView("正在检查 GitHub 更新",busy=true)
            try {
                val bytes=fetchPolicy()
                val latest=bytes?.let(::decode)
                if(bytes==null || latest==null) {
                    clearCandidate()
                    ready(if(bytes==null)"当前正式发布未提供更新清单" else "当前 APP 已是最新可用版本")
                    lastCompleted=started
                    return@withLock true
                }
                prepare(bytes,latest)
                if(latest.force && prefs.getString("attempt","")!=latest.sha256) {
                    if(!quietInstallAvailable()) {
                        waitingForSystem=true
                        ready("等待系统确认，可进入安装")
                        notify(latest,true)
                    } else {
                        waitingForSystem=false
                        if(!commit(latest,true,null)) {
                            ready("新版已下载，等通联结束后自动安装")
                            return@withLock false
                        }
                    }
                } else {
                    ready(if(waitingForSystem)"等待系统确认，可继续安装" else "新版已下载，可立即安装或稍后处理")
                    notify(latest,waitingForSystem)
                }
                lastCompleted=started
                true
            } catch(e:CancellationException) {
                ready("检查已中断，联网后继续")
                throw e
            } catch(e:Exception) {
                ready(if(candidate!=null)"检查未完成，已下载的版本仍可查看" else "更新检查或下载失败，可重试")
                e !is IOException
            }
        }
    }

    private suspend fun prepare(bytes:ByteArray,asset:ReleaseAsset) {
        val previous=candidate
        if(previous!=null && !asset.sameBinary(previous)) {
            clearCandidate()
        } else if(previous!=null && previous.force!=asset.force) {
            abandonWaiting()
            waitingForSystem=false
            prefs.edit().remove("attempt").apply()
        }
        val file=apk(asset)
        if(!verified(file,asset)) {
            candidate=null
            state.value=AppUpdateView("正在下载 "+asset.version,asset,busy=true)
            download(asset,file)
        }
        persistPolicy(bytes)
        candidate=asset
        dir.listFiles()?.filter{it.extension=="apk" && it!=file}?.forEach{it.delete()}
    }

    private fun abandonWaiting() {
        confirmation=null
        val id=prefs.getInt("session",-1)
        if(id>=0)runCatching{installer.abandonSession(id)}
        prefs.edit().remove("session").apply()
    }

    private fun clearCandidate() {
        abandonWaiting()
        candidate?.let{apk(it).delete()}
        candidate=null
        waitingForSystem=false
        policy.delete()
        prefs.edit().remove("attempt").remove("notified").apply()
        notifications.cancel(NOTIFICATION)
    }

    private suspend fun download(asset:ReleaseAsset,file:File) {
        val part=file.resolveSibling(file.nameWithoutExtension+".part")
        try {
            request(asset.url).use { response->
                if(!response.isSuccessful)throw IOException("GitHub 返回 "+response.code)
                val body=checkNotNull(response.body)
                require(body.contentLength()<0 || body.contentLength()==asset.size)
                body.byteStream().use { source->
                    part.outputStream().use { output->
                        val buffer=ByteArray(64*1024)
                        var remaining=asset.size
                        while(remaining>0) {
                            currentCoroutineContext().ensureActive()
                            val n=source.read(buffer,0,minOf(remaining,buffer.size.toLong()).toInt())
                            if(n<0)throw IOException("APK 下载中断")
                            output.write(buffer,0,n)
                            remaining-=n
                        }
                        require(source.read()==-1){"APK 长度无效"}
                        output.fd.sync()
                    }
                }
            }
            // PackageManager reads a path ending in .apk.
            if(!part.renameTo(file))throw IOException("无法保存 APK")
            if(!verified(file,asset)) {
                file.delete()
                error("APK 校验失败")
            }
        } finally { part.delete() }
    }

    @Suppress("DEPRECATION")
    private fun signers(info:PackageInfo)=if(Build.VERSION.SDK_INT>=28)info.signingInfo?.apkContentsSigners
        else info.signatures

    @Suppress("DEPRECATION")
    private fun verified(file:File,asset:ReleaseAsset):Boolean {
        if(!file.isFile || file.length()!=asset.size)return false
        val sum=MessageDigest.getInstance("SHA-256")
        file.inputStream().use { stream->
            val buffer=ByteArray(64*1024)
            while(true) {
                val n=stream.read(buffer)
                if(n<0)break
                sum.update(buffer,0,n)
            }
        }
        if(sum.digest().toByteString().hex()!=asset.sha256)return false
        val flags=if(Build.VERSION.SDK_INT>=28)PackageManager.GET_SIGNING_CERTIFICATES else PackageManager.GET_SIGNATURES
        val pm=app.packageManager
        val archive=pm.getPackageArchiveInfo(file.path,flags) ?: return false
        val code=if(Build.VERSION.SDK_INT>=28)archive.longVersionCode else archive.versionCode.toLong()
        val current=signers(pm.getPackageInfo(app.packageName,flags))?.toSet().orEmpty()
        return archive.packageName==app.packageName && code==asset.code.toLong() && current.isNotEmpty() &&
            signers(archive)?.toSet()==current
    }

    private fun canInstall()=Build.VERSION.SDK_INT<26 || app.packageManager.canRequestPackageInstalls()
    private fun quietInstallAvailable()=Build.VERSION.SDK_INT>=31 && canInstall()

    fun install(context:Context) {
        app.manager.scope.launch {
            withContext(Dispatchers.IO) {
                gate.withLock {
                    restore()
                    val asset=candidate ?: return@withLock
                    if(installing)return@withLock
                    try {
                        if(!canInstall()) {
                            withContext(Dispatchers.Main) {
                                context.startActivity(Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:"+app.packageName)))
                            }
                            ready("请在 Android 允许此应用安装更新，再返回继续")
                            return@withLock
                        }
                        if(app.manager.updateBusy()) {
                            ready("请先结束发话或收听，再继续安装")
                            return@withLock
                        }
                        val pending=confirmation
                        if(pending!=null && withContext(Dispatchers.Main){runCatching{context.startActivity(pending)}.isSuccess})
                            return@withLock
                        abandonWaiting()
                        if(!commit(asset,false,context))ready("请先结束发话或收听，再安装")
                    } catch(e:CancellationException){throw e}
                    catch(e:Exception){ready("安装未提交，可以重试")}
                }
            }
        }
    }

    private suspend fun commit(asset:ReleaseAsset,force:Boolean,activity:Context?):Boolean {
        if(app.manager.updateBusy())return false
        val file=apk(asset)
        require(verified(file,asset)){"APK 校验失败"}
        val params=PackageInstaller.SessionParams(PackageInstaller.SessionParams.MODE_FULL_INSTALL).apply {
            setAppPackageName(app.packageName)
            setSize(asset.size)
            if(Build.VERSION.SDK_INT>=31)setRequireUserAction(if(force)
                PackageInstaller.SessionParams.USER_ACTION_NOT_REQUIRED else PackageInstaller.SessionParams.USER_ACTION_REQUIRED)
        }
        val id=installer.createSession(params)
        var committed=false
        try {
            installer.openSession(id).use { session->
                session.openWrite("base.apk",0,asset.size).use { out->
                    file.inputStream().use{it.copyTo(out)}
                    session.fsync(out)
                }
                if(force && !forceStillActive(asset))return true
                val result=Intent(app,AppUpdateInstallResult::class.java).putExtra("ownerSession",id)
                val sender=PendingIntent.getBroadcast(app,id,result,PendingIntent.FLAG_UPDATE_CURRENT or
                    (if(Build.VERSION.SDK_INT>=31)PendingIntent.FLAG_MUTABLE else 0)).intentSender
                withContext(Dispatchers.Main) {
                    if(!app.manager.reserveAppInstall())return@withContext
                    try {
                        check(prefs.edit().putString("attempt",asset.sha256).putInt("session",id).commit())
                        installing=true
                        state.value=AppUpdateView("正在交给 Android 安装",asset,busy=true)
                        foregroundInstall=activity?.let{WeakReference(it)}
                        session.commit(sender)
                        committed=true
                    } finally {
                        if(!committed) {
                            installing=false
                            prefs.edit().remove("attempt").remove("session").apply()
                            app.manager.releaseAppInstall()
                        }
                    }
                }
            }
        } finally { if(!committed)runCatching{installer.abandonSession(id)} }
        return committed
    }

    private fun forceStillActive(asset:ReleaseAsset):Boolean {
        val bytes=fetchPolicy()
        val current=bytes?.let(::decode)
        if(current==null || bytes==null) {
            clearCandidate();ready("该更新已撤回");return false
        }
        if(!current.sameBinary(asset))throw IOException("GitHub 已发布另一版本")
        if(current.force)return true
        persistPolicy(bytes)
        candidate=current
        ready("新版已下载，可立即安装或稍后处理")
        notify(current,false)
        return false
    }

    @Suppress("DEPRECATION")
    fun result(result:Intent,done:()->Unit) {
        app.manager.scope.launch {
            try {
                withContext(Dispatchers.IO) {
                    gate.withLock {
                        val id=result.getIntExtra(PackageInstaller.EXTRA_SESSION_ID,-1)
                        if(id<0 || id!=result.getIntExtra("ownerSession",-1) || id!=prefs.getInt("session",-1))return@withLock
                        restore()
                        val activity=foregroundInstall?.get()
                        foregroundInstall=null
                        installing=false
                        withContext(Dispatchers.Main){app.manager.releaseAppInstall()}
                        val asset=candidate ?: return@withLock
                        when(result.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)) {
                            PackageInstaller.STATUS_PENDING_USER_ACTION->{
                                confirmation=result.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                                waitingForSystem=true
                                ready("等待系统确认，可继续安装")
                                notify(asset,true)
                                if(activity!=null)withContext(Dispatchers.Main) {
                                    confirmation?.let{runCatching{activity.startActivity(it)}}
                                }
                            }
                            PackageInstaller.STATUS_SUCCESS->{clearCandidate();ready("APP 更新完成")}
                            else->{
                                prefs.edit().remove("session").apply()
                                confirmation=null
                                waitingForSystem=true
                                ready("Android 暂未完成安装，可手动重试")
                            }
                        }
                    }
                }
            } finally {done()}
        }
    }

    private fun notify(asset:ReleaseAsset,system:Boolean) {
        if(Build.VERSION.SDK_INT>=33 && app.checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS)!=PackageManager.PERMISSION_GRANTED)return
        if(!notifications.areNotificationsEnabled())return
        val key=asset.sha256+":"+system
        if(prefs.getString("notified","")==key)return
        if(Build.VERSION.SDK_INT>=26)notifications.createNotificationChannel(
            NotificationChannel(CHANNEL,"软件更新",NotificationManager.IMPORTANCE_DEFAULT))
        val open=PendingIntent.getActivity(app,0,Intent(app,AppUpdateActivity::class.java),
            PendingIntent.FLAG_UPDATE_CURRENT or PendingIntent.FLAG_IMMUTABLE)
        @Suppress("DEPRECATION")
        val builder=if(Build.VERSION.SDK_INT>=26)Notification.Builder(app,CHANNEL) else Notification.Builder(app)
        notifications.notify(NOTIFICATION,builder.setSmallIcon(android.R.drawable.stat_sys_download_done)
            .setContentTitle("黑猫互联 "+asset.version+" 已下载")
            .setContentText(if(system)"点此处理 Android 安装确认" else "点此查看新版并选择安装时间")
            .setContentIntent(open).setAutoCancel(true).build())
        prefs.edit().putString("notified",key).apply()
    }

    fun later() { notifications.cancel(NOTIFICATION) }

    private fun schedule(id:Int) {
        val jobs=app.getSystemService(JobScheduler::class.java)
        if(jobs.getPendingJob(id)!=null)return
        val job=JobInfo.Builder(id,ComponentName(app,AppUpdateCheckJob::class.java))
            .setRequiredNetworkType(JobInfo.NETWORK_TYPE_ANY).setPersisted(true)
        if(id==DAILY)job.setPeriodic(TimeUnit.HOURS.toMillis(24))
        else job.setBackoffCriteria(TimeUnit.MINUTES.toMillis(15),JobInfo.BACKOFF_POLICY_EXPONENTIAL)
        jobs.schedule(job.build())
    }

    companion object {
        private const val CHANNEL="app-release-updates"
        private const val NOTIFICATION=710
        private const val DAILY=711
        private const val NOW=712
    }
}

private fun java.io.InputStream.readNBytesBounded(limit:Int):ByteArray {
    val result=java.io.ByteArrayOutputStream()
    val buffer=ByteArray(1024)
    while(result.size()<limit) {
        val count=read(buffer,0,minOf(buffer.size,limit-result.size()))
        if(count<0)break
        result.write(buffer,0,count)
    }
    return result.toByteArray()
}

class AppUpdateCheckJob:JobService() {
    private val running=mutableMapOf<Int,Job>()
    override fun onStartJob(params:JobParameters):Boolean {
        val app=application as OrlApp
        running[params.jobId]=app.manager.scope.launch {
            val completed=app.updater.check()
            running.remove(params.jobId)
            jobFinished(params,!completed)
        }
        return true
    }
    override fun onStopJob(params:JobParameters):Boolean {
        running.remove(params.jobId)?.cancel()
        return true
    }
}

class AppUpdateInstallResult:BroadcastReceiver() {
    override fun onReceive(context:Context,intent:Intent) {
        val pending=goAsync()
        (context.applicationContext as OrlApp).updater.result(intent){pending.finish()}
    }
}
