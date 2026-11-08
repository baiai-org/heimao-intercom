package net.openradiolink.app

import android.Manifest
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.activity.SystemBarStyle
import androidx.activity.result.contract.ActivityResultContracts
import androidx.core.content.ContextCompat
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableStateOf

class MainActivity:ComponentActivity() {
    private val manager get()=(application as OrlApp).manager
    private var afterPermission: (()->Unit)?=null
    private var backgroundAllowed by mutableStateOf(false)
    private val backgroundPermission=registerForActivityResult(ActivityResultContracts.StartActivityForResult()) {
        refreshBackgroundPermission()
    }
    private fun refreshBackgroundPermission() {
        backgroundAllowed=getSystemService(PowerManager::class.java).isIgnoringBatteryOptimizations(packageName)
    }
    private fun requestBackgroundPermission() {
        refreshBackgroundPermission()
        val request=if(backgroundAllowed)Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)
            else Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS,Uri.parse("package:$packageName"))
        try { backgroundPermission.launch(request) }
        catch(_:RuntimeException) {
            try { backgroundPermission.launch(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS)) }
            catch(_:RuntimeException) {
                try{startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,Uri.parse("package:$packageName")))}
                catch(_:RuntimeException){manager.message("请在系统应用设置中允许${Brand.APP}后台运行")}
            }
        }
    }
    private val networkPermissions=registerForActivityResult(ActivityResultContracts.RequestMultiplePermissions()) { result ->
        val action=afterPermission;afterPermission=null
        val denied=result.filter { !it.value }.keys.filter { it!="android.permission.POST_NOTIFICATIONS" }
        if(denied.isEmpty())action?.invoke() else manager.message("网络权限尚未允许，请在系统权限设置中开启后重试")
    }
    private val microphonePermission=registerForActivityResult(ActivityResultContracts.RequestPermission()) { granted ->
        manager.message(if(granted)"麦克风已允许，请再次按住发话" else "发话需要麦克风权限")
    }
    private fun permissions(setup: Boolean=false,local: Boolean=true,action: ()->Unit) {
        val required=mutableListOf<String>()
        if(local && Build.VERSION.SDK_INT>=37)required+="android.permission.ACCESS_LOCAL_NETWORK"
        if(Build.VERSION.SDK_INT>=33)required+=Manifest.permission.POST_NOTIFICATIONS
        if(setup) {
            if(Build.VERSION.SDK_INT>=33)required+=Manifest.permission.NEARBY_WIFI_DEVICES
            else if(Build.VERSION.SDK_INT>=29){required+=Manifest.permission.ACCESS_FINE_LOCATION;required+=Manifest.permission.ACCESS_COARSE_LOCATION}
        }
        val missing=required.filter { ContextCompat.checkSelfPermission(this,it)!=PackageManager.PERMISSION_GRANTED }
        if(missing.isEmpty())action() else {afterPermission=action;networkPermissions.launch(missing.toTypedArray())}
    }
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(statusBarStyle=SystemBarStyle.dark(android.graphics.Color.TRANSPARENT),
            navigationBarStyle=SystemBarStyle.dark(android.graphics.Color.TRANSPARENT))
        setContent {
            OrlScreen(manager,
                connect={id,endpoint ->
                    val p=manager.store.profiles.value.firstOrNull{it.id==id}
                    val entry=p?.endpoints?.firstOrNull{it.id==(endpoint ?: p.selectedEndpoint)} ?: p?.endpoint()
                    permissions(local=entry?.mode!="PUBLIC") {manager.connect(id,endpoint)}
                },
                discover={ enabled -> if(enabled)permissions {manager.discover(true)} else manager.discover(false)},
                provision={permissions(true){
                    if(Build.VERSION.SDK_INT>=29)manager.provisionBoard()
                    else {startActivity(Intent(Settings.ACTION_WIFI_SETTINGS));manager.message("请连接手台的开放配网热点，再添加 https://192.168.4.1/ 入口")}
                }},
                pttDown={id ->
                    if(ContextCompat.checkSelfPermission(this,Manifest.permission.RECORD_AUDIO)==PackageManager.PERMISSION_GRANTED)manager.pttDown(id)
                    else microphonePermission.launch(Manifest.permission.RECORD_AUDIO)
                },
                wirelessSettings={startActivity(Intent(Settings.ACTION_WIRELESS_SETTINGS))},
                appSettings={startActivity(Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS,Uri.parse("package:$packageName")))},
                backgroundAllowed=backgroundAllowed,requestBackground=::requestBackgroundPermission
            )
        }
    }
    override fun onResume() {super.onResume();refreshBackgroundPermission();manager.setVisible(true)}
    override fun onPostResume() {
        super.onPostResume()
        val preferences=getSharedPreferences("background-permission",MODE_PRIVATE)
        if(!backgroundAllowed && !preferences.getBoolean("requested",false)) {
            // One startup request; a denial can be revisited explicitly in Connection & permissions.
            preferences.edit().putBoolean("requested",true).apply()
            window.decorView.post { if(!isFinishing && !isDestroyed)requestBackgroundPermission() }
        }
    }
    override fun onPause() {manager.setVisible(false);super.onPause()}
}
