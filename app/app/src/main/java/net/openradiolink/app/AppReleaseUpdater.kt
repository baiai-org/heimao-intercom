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
import androidx.core.content.FileProvider
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
    val ready:Boolean=false,val busy:Boolean=false,val systemConfirmation:Boolean=false,
    val fileInstall:Boolean=false)

internal enum class AppInstallDestination { PERMISSION, CONFIRMATION, FILE }

internal class AppInstallPrompt(val destination:AppInstallDestination,val intent:Intent,val sha256:String)

internal class AppReleaseUpdater(private val app:OrlApp) {
    private val gate=Mutex()
    private val state=MutableStateFlow(AppUpdateView())
    val view=state.asStateFlow()
    private val prompts=MutableStateFlow<AppInstallPrompt?>(null)
    val installPrompts=prompts.asStateFlow()
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
    private var autoConfirm=false
    private var fileInstall=false
    private var callbackTimeout:Job?=null
    private var permissionAttempt:Pair<String,Boolean>?=null
    private val sharedApks=app.cacheDir.resolve("verified-updates")

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
            fileInstall=waitingForSystem
            ready(if(waitingForSystem)"等待系统确认，可继续安装" else "新版已下载")
        }
    }

    private fun ready(message:String) {
        state.value=AppUpdateView(message,candidate,ready=candidate!=null,systemConfirmation=waitingForSystem,
            fileInstall=fileInstall)
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
                        if(!commit(latest,true)) {
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
            check(abandonWaiting()){"Android 正在处理安装"}
            waitingForSystem=false
            fileInstall=false
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

    private fun abandonWaiting():Boolean {
        val id=prefs.getInt("session",-1)
        if(id>=0 && runCatching{installer.abandonSession(id)}.isFailure && installer.getSessionInfo(id)!=null) {
            return false
        }
        confirmation=null
        autoConfirm=false
        prompts.value=null
        callbackTimeout?.cancel()
        callbackTimeout=null
        prefs.edit().remove("session").apply()
        return true
    }

    private fun clearCandidate() {
        check(abandonWaiting()){"Android 正在处理安装"}
        sharedApks.listFiles()?.forEach{it.delete()}
        candidate?.let{apk(it).delete()}
        candidate=null
        waitingForSystem=false
        fileInstall=false
        permissionAttempt=null
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

    private fun prompt(destination:AppInstallDestination,intent:Intent,asset:ReleaseAsset) {
        prompts.value=AppInstallPrompt(destination,intent,asset.sha256)
    }

    fun takeInstallPrompt(prompt:AppInstallPrompt)=prompts.compareAndSet(prompt,null)

    fun install(viaFile:Boolean=false) {
        app.manager.scope.launch(Dispatchers.IO) {
            gate.withLock {
                restore()
                val asset=candidate ?: return@withLock
                beginInstall(asset,viaFile)
            }
        }
    }

    fun installPermissionReturned() {
        app.manager.scope.launch(Dispatchers.IO) {
            gate.withLock {
                val attempt=permissionAttempt ?: return@withLock
                permissionAttempt=null
                val asset=candidate?.takeIf{it.sha256==attempt.first} ?: return@withLock
                if(canInstall())beginInstall(asset,attempt.second)
                else ready("请允许此来源安装应用，再点击继续")
            }
        }
    }

    fun installLaunchFailed(prompt:AppInstallPrompt,blockedByCall:Boolean=false) {
        app.manager.scope.launch(Dispatchers.IO) {
            gate.withLock {
                if(candidate?.sha256!=prompt.sha256 || installing)return@withLock
                if(prompt.destination==AppInstallDestination.PERMISSION)permissionAttempt=null
                if(!blockedByCall && prompt.destination==AppInstallDestination.CONFIRMATION)fileInstall=true
                ready(when {
                    blockedByCall -> "请先结束发话或收听，再继续安装"
                    prompt.destination==AppInstallDestination.PERMISSION -> "无法打开安装授权页，请在系统设置中允许黑猫互联安装应用"
                    prompt.destination==AppInstallDestination.FILE -> "无法打开系统安装器，可从 GitHub 下载同签名 APK 覆盖安装"
                    else -> "无法打开系统确认，可重试或使用兼容安装"
                })
            }
        }
    }

    private suspend fun beginInstall(asset:ReleaseAsset,viaFile:Boolean) {
        if(installing || prompts.value!=null || (viaFile && !fileInstall))return
        if(app.manager.updateBusy()) {
            ready("请先结束发话或收听，再继续安装")
            return
        }
        try {
            if(!canInstall()) {
                permissionAttempt=asset.sha256 to viaFile
                ready("请在 Android 允许此应用安装更新，再返回继续")
                prompt(AppInstallDestination.PERMISSION,
                    Intent(Settings.ACTION_MANAGE_UNKNOWN_APP_SOURCES,Uri.parse("package:"+app.packageName)),asset)
                return
            }
            permissionAttempt=null
            val pending=confirmation
            if(!viaFile && pending!=null && installer.getSessionInfo(prefs.getInt("session",-1))!=null) {
                prompt(AppInstallDestination.CONFIRMATION,pending,asset)
                return
            }
            if(!abandonWaiting()) {
                ready("Android 正在处理安装，请稍后继续")
                return
            }
            if(viaFile) {
                openVerifiedApk(asset)
            } else if(!commit(asset,false)) {
                ready("请先结束发话或收听，再安装")
            }
        } catch(e:CancellationException){throw e}
        catch(e:IOException){ready("无法准备安装文件，请检查存储空间后重试")}
        catch(e:Exception){ready("Android 安装未提交，可以重试")}
    }

    private fun openVerifiedApk(asset:ReleaseAsset) {
        val source=apk(asset)
        require(verified(source,asset)){"APK 校验失败"}
        val shared=sharedApks.resolve(source.name)
        sharedApks.mkdirs()
        source.copyTo(shared,overwrite=true)
        val uri=FileProvider.getUriForFile(app,app.packageName+".update-files",shared)
        ready("请在系统安装器中确认")
        prompt(AppInstallDestination.FILE,Intent(Intent.ACTION_VIEW)
            .setDataAndType(uri,"application/vnd.android.package-archive")
            .addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION),asset)
    }

    private suspend fun commit(asset:ReleaseAsset,force:Boolean):Boolean {
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
                if(!withContext(Dispatchers.Main){app.manager.reserveAppInstall()})return false
                try {
                    if(!prefs.edit().putString("attempt",asset.sha256).putInt("session",id).commit()) {
                        throw IOException("无法保存 Android 安装会话")
                    }
                    installing=true
                    autoConfirm=!force
                    state.value=AppUpdateView("正在交给 Android 安装",asset,busy=true)
                    session.commit(sender)
                    committed=true
                } finally {
                    if(!committed) {
                        installing=false
                        prefs.edit().remove("attempt").remove("session").apply()
                        withContext(NonCancellable+Dispatchers.Main){app.manager.releaseAppInstall()}
                    }
                }
            }
        } finally { if(!committed)runCatching{installer.abandonSession(id)} }
        if(committed) {
            callbackTimeout=app.manager.scope.launch(Dispatchers.IO) {
                delay(20_000)
                gate.withLock {
                    if(!installing || prefs.getInt("session",-1)!=id)return@withLock
                    installing=false
                    withContext(Dispatchers.Main){app.manager.releaseAppInstall()}
                    waitingForSystem=true
                    fileInstall=true
                    ready("Android 暂未返回安装确认，可以重试或使用兼容安装")
                }
            }
        }
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
                        callbackTimeout?.cancel()
                        callbackTimeout=null
                        installing=false
                        withContext(Dispatchers.Main){app.manager.releaseAppInstall()}
                        val asset=candidate ?: return@withLock
                        when(result.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)) {
                            PackageInstaller.STATUS_PENDING_USER_ACTION->{
                                confirmation=if(Build.VERSION.SDK_INT>=33)
                                    result.getParcelableExtra(Intent.EXTRA_INTENT,Intent::class.java)
                                else result.getParcelableExtra<Intent>(Intent.EXTRA_INTENT)
                                waitingForSystem=true
                                fileInstall=confirmation==null
                                ready(if(confirmation!=null)"等待系统确认，可继续安装" else "Android 未提供确认页面，可重试或使用兼容安装")
                                notify(asset,true)
                                if(autoConfirm)confirmation?.let{prompt(AppInstallDestination.CONFIRMATION,it,asset)}
                                autoConfirm=false
                            }
                            PackageInstaller.STATUS_SUCCESS->{
                                prefs.edit().remove("session").apply()
                                clearCandidate()
                                ready("APP 更新完成")
                            }
                            else->{
                                prefs.edit().remove("session").apply()
                                confirmation=null
                                prompts.value=null
                                autoConfirm=false
                                fileInstall=false
                                waitingForSystem=true
                                ready(when(result.getIntExtra(PackageInstaller.EXTRA_STATUS,PackageInstaller.STATUS_FAILURE)) {
                                    PackageInstaller.STATUS_FAILURE_ABORTED -> "系统安装已取消，可以继续安装"
                                    PackageInstaller.STATUS_FAILURE_BLOCKED -> "Android 安装策略已阻止此次更新"
                                    PackageInstaller.STATUS_FAILURE_STORAGE -> "存储空间不足，请清理后重试"
                                    PackageInstaller.STATUS_FAILURE_CONFLICT -> "APK 与手机上已装版本冲突，请核对安装签名"
                                    PackageInstaller.STATUS_FAILURE_INCOMPATIBLE -> "此 APK 与当前手机不兼容"
                                    PackageInstaller.STATUS_FAILURE_INVALID -> "Android 判定 APK 无效，请重新检查更新"
                                    else -> "Android 暂未完成安装，可手动重试"
                                })
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

class AppUpdateFileProvider:FileProvider()

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
