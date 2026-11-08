package net.openradiolink.app

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.os.PowerManager
import android.provider.Settings

/** Pure rules, unit-tested without an Android context. */
internal object KeepAliveRules {
    enum class Restart { STICKY, NOT_STICKY }
    fun restartMode(persistent:Boolean)=if(persistent)Restart.STICKY else Restart.NOT_STICKY
    fun batteryLabel(exempt:Boolean?)=when(exempt){true->"已允许";false->"未允许";null->"无法读取"}
    fun confirmLabel(confirmed:Boolean)=if(confirmed)"已手动确认" else "未确认"
    fun persistentLabel(on:Boolean)=if(on)"常驻前台已开启" else "常驻前台已关闭"
}

/**
 * Background survival helpers. Android exposes no API for "autostart" or
 * "recent-task lock", so those two are user-confirmed checkboxes plus a jump to
 * the vendor's page; only the battery-optimisation exemption is a real check.
 */
internal object KeepAlive {
    private const val PREFS="keepalive"
    private const val PERSISTENT="persistent"
    private const val AUTOSTART="autostart_confirmed"
    private const val RECENTS="recents_confirmed"
    private const val LAST_PROFILE="last_profile"

    private fun prefs(c:Context)=c.getSharedPreferences(PREFS,Context.MODE_PRIVATE)

    fun persistent(c:Context)=prefs(c).getBoolean(PERSISTENT,false)
    fun setPersistent(c:Context,on:Boolean){prefs(c).edit().putBoolean(PERSISTENT,on).apply()}
    fun autostartConfirmed(c:Context)=prefs(c).getBoolean(AUTOSTART,false)
    fun setAutostartConfirmed(c:Context,on:Boolean){prefs(c).edit().putBoolean(AUTOSTART,on).apply()}
    fun recentsConfirmed(c:Context)=prefs(c).getBoolean(RECENTS,false)
    fun setRecentsConfirmed(c:Context,on:Boolean){prefs(c).edit().putBoolean(RECENTS,on).apply()}
    fun lastProfile(c:Context)=prefs(c).getString(LAST_PROFILE,"") ?: ""
    fun setLastProfile(c:Context,id:String){prefs(c).edit().putString(LAST_PROFILE,id).apply()}

    /** null means the OEM removed the standard API; do not claim "allowed". */
    fun batteryExempt(c:Context):Boolean? = try {
        (c.getSystemService(Context.POWER_SERVICE) as PowerManager).isIgnoringBatteryOptimizations(c.packageName)
    } catch(_:Exception) { null }

    private val autostartPages=listOf(
        ComponentName("com.miui.securitycenter","com.miui.permcenter.autostart.AutoStartManagementActivity"),
        ComponentName("com.huawei.systemmanager","com.huawei.systemmanager.startupmgr.ui.StartupNormalAppListActivity"),
        ComponentName("com.huawei.systemmanager","com.huawei.systemmanager.optimize.process.ProtectActivity"),
        ComponentName("com.coloros.safecenter","com.coloros.safecenter.permission.startup.StartupAppListActivity"),
        ComponentName("com.coloros.safecenter","com.coloros.safecenter.startupapp.StartupAppListActivity"),
        ComponentName("com.oppo.safe","com.oppo.safe.permission.startup.StartupAppListActivity"),
        ComponentName("com.vivo.permissionmanager","com.vivo.permissionmanager.activity.BgStartUpManagerActivity"),
        ComponentName("com.iqoo.secure","com.iqoo.secure.ui.phoneoptimize.AddWhiteListActivity"),
        ComponentName("com.oneplus.security","com.oneplus.security.chainlaunch.view.ChainLaunchAppListActivity"),
        ComponentName("com.samsung.android.lool","com.samsung.android.sm.ui.battery.BatteryActivity"),
        ComponentName("com.meizu.safe","com.meizu.safe.permission.SmartBGActivity"),
        ComponentName("com.asus.mobilemanager","com.asus.mobilemanager.MainActivity"),
        ComponentName("com.letv.android.letvsafe","com.letv.android.letvsafe.AutobootManageActivity"),
    )

    /**
     * Tries each known vendor page, then falls back to the app's own settings page.
     * Deliberately no resolveActivity() pre-check: on API 30+ package visibility
     * hides other vendors' activities, so a query answers null for pages that do
     * exist and would skip every candidate. Starting an explicit component is
     * still allowed; a missing page throws ActivityNotFoundException.
     */
    fun openAutostart(c:Context):Boolean {
        for(page in autostartPages) {
            val intent=Intent().setComponent(page).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
            try { c.startActivity(intent);return true } catch(_:Exception) {}
        }
        return openAppDetails(c)
    }

    fun openAppDetails(c:Context):Boolean = try {
        c.startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,Uri.parse("package:${c.packageName}"))
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));true
    } catch(_:Exception) { false }
}
