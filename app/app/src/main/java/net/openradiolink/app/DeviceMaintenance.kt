package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import org.json.JSONObject
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.TimeZone

internal data class DeviceMaintenanceState(
    val available:Boolean=false,val ready:Boolean=false,val revision:Long=-1,val apiVersion:Int=0,
    val cleanup:Boolean=true,val restart:Boolean=false,val hours:Int=24,val remaining:Long=0,
    val waiting:Boolean=false,val pending:Boolean=false,val idleSeconds:Long=0,val busy:String="",
    val runs:Long=0,val bytes:Long=0,val decoders:Long=0,val internalFree:Long=0,val largest:Long=0,
    val psram:Long=0,val result:String="",val lastReboot:String="",
    val lastCleanupSeconds:Long=0,
    val days:Int=0,val minute:Int=240,val clock:String="",val lastSync:Long=0,val nextRestart:Long=0,
    val restartSaveFailed:Boolean=false,
) {
    companion object {
        fun parse(json:JSONObject):DeviceMaintenanceState {
            val version=json.optInt("version")
            if(version !in 1..2 || !json.optBoolean("available",true)) return DeviceMaintenanceState()
            fun whole(key:String,range:LongRange):Long? {
                val value=(json.opt(key) as? Number)?.toDouble() ?: return null
                val integer=value.toLong()
                return integer.takeIf { it in range && it.toDouble()==value }
            }
            val revision=whole("revision",0..4294967295L) ?: return DeviceMaintenanceState()
            val hours=if(version==1) whole("restart_hours",1L..168L)?.toInt() ?: return DeviceMaintenanceState() else 24
            val days=if(version==2) whole("restart_days",0L..127L)?.toInt() ?: return DeviceMaintenanceState() else 0
            val minute=if(version==2) whole("restart_minute",0L..1439L)?.toInt() ?: return DeviceMaintenanceState() else 240
            val restart=json.optBoolean("restart_enabled")
            val complete=json.opt("cleanup_enabled") is Boolean && json.opt("restart_enabled") is Boolean &&
                json.optString("device_id").isNotBlank() && json.optString("boot_id").isNotBlank() &&
                (version==1 || ((!restart || days!=0) && json.optString("timezone")=="Asia/Shanghai"))
            return DeviceMaintenanceState(available=complete,ready=complete&&json.optBoolean("ready"),revision=revision,
                apiVersion=version,cleanup=json.optBoolean("cleanup_enabled"),restart=restart,hours=hours,
                days=days,minute=minute,clock=json.optString("clock_state"),lastSync=json.optLong("last_sync_utc"),
                nextRestart=json.optLong("next_restart_utc"),restartSaveFailed=json.optBoolean("restart_save_failed"),
                remaining=json.optLong("restart_remaining_s"),waiting=json.optBoolean("restart_waiting"),pending=json.optBoolean("reboot_pending"),
                idleSeconds=json.optLong("idle_s"),busy=json.optString("busy"),runs=json.optLong("cleanup_runs"),
                bytes=json.optLong("released_buffer_bytes"),decoders=json.optLong("released_decoders"),
                internalFree=json.optLong("internal_free"),largest=json.optLong("internal_largest"),psram=json.optLong("psram_free"),
                result=json.optString("result"),lastReboot=json.optString("last_reboot"),lastCleanupSeconds=json.optLong("last_cleanup_uptime_s"))
        }
    }
}
private val weekdays=listOf("周一","周二","周三","周四","周五","周六","周日")
private fun beijingTime(utc:Long):String {
    if(utc !in 1..253402300799L) return "—"
    return SimpleDateFormat("MM-dd E HH:mm",Locale.SIMPLIFIED_CHINESE).apply {
        timeZone=TimeZone.getTimeZone("Asia/Shanghai")
    }.format(Date(utc*1000L))
}

@OptIn(ExperimentalLayoutApi::class)
@Composable internal fun DeviceMaintenanceSection(manager:DeviceManager,p:DeviceProfile,s:SessionView,confirm:(String,()->Unit)->Unit) {
    val state=DeviceMaintenanceState.parse(s.deviceMaintenance)
    var dirty by remember(p.id){mutableStateOf(false)}
    var clean by remember(p.id){mutableStateOf(true)};var restart by remember(p.id){mutableStateOf(false)}
    var hours by remember(p.id){mutableStateOf("24")}
    var days by remember(p.id){mutableIntStateOf(0)}
    var hour by remember(p.id){mutableStateOf("04")}
    var minute by remember(p.id){mutableStateOf("00")}
    var revision by remember(p.id){mutableLongStateOf(-1)}
    var saving by remember(p.id){mutableStateOf(false)};var message by remember(p.id){mutableStateOf("")}
    LaunchedEffect(state.revision,state.apiVersion,state.available,dirty) {
        if(state.available&&!dirty) {
            clean=state.cleanup
            restart=state.restart
            hours=state.hours.toString()
            days=state.days
            hour=(state.minute/60).toString().padStart(2,'0')
            minute=(state.minute%60).toString().padStart(2,'0')
            revision=state.revision
        }
    }
    val weekly=state.apiVersion==2
    val enabled=s.connected&&!s.maintenance&&state.ready&&!saving
    Section("设备自动维护与重启") {
        if(!state.available) {
            Text(if(s.deviceMaintenance.has("err"))s.deviceMaintenance.optString("err") else "当前手台未提供完整维护状态，请刷新或使用配套新固件。",color=secondary)
        } else {
            if(!state.ready)Text("维护配置读取失败，自动维护已停止，请检查诊断。",color=red)
            Row(verticalAlignment=Alignment.CenterVertically) {Text("自动资源维护",Modifier.weight(1f));Switch(clean,{clean=it;dirty=true},enabled=enabled)}
            Text("每5分钟检查；连续空闲60秒后回收过期资源，保留配置和有效音频缓存。",color=secondary,style=MaterialTheme.typography.bodySmall)
            Row(verticalAlignment=Alignment.CenterVertically) {Text("定时重启",Modifier.weight(1f));Switch(restart,{restart=it;dirty=true},enabled=enabled)}
            if(weekly) {
                FlowRow(horizontalArrangement=Arrangement.spacedBy(6.dp)) {
                    weekdays.forEachIndexed { day,label ->
                        val bit=1 shl day
                        FilterChip(selected=(days and bit)!=0,onClick={days=days xor bit;dirty=true},
                            label={Text(label)},enabled=enabled)
                    }
                }
                Row(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                    OutlinedTextField(hour,{ if(it.length<=2 && it.all { c -> c in '0'..'9' }) {hour=it;dirty=true} },
                        label={Text("小时（0～23）")},singleLine=true,modifier=Modifier.weight(1f),
                        keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number),enabled=enabled)
                    OutlinedTextField(minute,{ if(it.length<=2 && it.all { c -> c in '0'..'9' }) {minute=it;dirty=true} },
                        label={Text("分钟（0～59）")},singleLine=true,modifier=Modifier.weight(1f),
                        keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number),enabled=enabled)
                }
                Text("按北京时间到点重启，当前通联会结束。正在升级或写入文件时顺延。",
                    color=secondary,style=MaterialTheme.typography.bodySmall)
                val clock=when(state.clock) {
                    "synced"->"已校时"
                    "holdover"->"暂用上次校准的时钟"
                    "stale"->"校时已过期"
                    else->"等待首次校时"
                }
                Text("$clock · 最近校时 ${beijingTime(state.lastSync)}（北京时间）",color=secondary)
            } else {
                OutlinedTextField(hours,{hours=it;dirty=true},label={Text("间隔小时（1～168）")},singleLine=true,
                    keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number),enabled=enabled,modifier=Modifier.fillMaxWidth())
                Text("由手台独立计时，到期后等待连续空闲60秒。",color=secondary,style=MaterialTheme.typography.bodySmall)
            }
            val busy=when(state.busy){"idle"->"空闲";"writing"->"正在写入";"transmitting"->"正在发射或测试";"receiving"->"正在接收";else->"音频尚未结束"}
            val schedule=when {
                state.pending -> "重启请求已接受"
                !state.restart -> "定时重启已关闭"
                state.restartSaveFailed -> "执行记录写入失败，正在重试"
                state.waiting -> if(weekly) "已到点，等待写入结束或 BOOT 松开" else "已到期，等待空闲"
                !weekly -> "距下次重启 ${state.remaining/3600}小时${state.remaining%3600/60}分钟"
                state.nextRestart>0 -> "下次重启 ${beijingTime(state.nextRestart)}"
                else -> "等待校时后安排重启"
            }
            Text("$schedule · $busy · 已空闲${state.idleSeconds}秒",color=secondary)
            Button(onClick={
                val value=if(weekly) {
                    val h=hour.toIntOrNull()
                    val m=minute.toIntOrNull()
                    if(h==null || h !in 0..23 || m==null || m !in 0..59 || (restart && days==0)) {
                        message="请填写有效的时间，并为已启用的计划至少选择一天。"
                        return@Button
                    }
                    h*60+m
                } else {
                    val interval=hours.toIntOrNull()
                    if(interval==null || interval !in 1..168) {
                        message="间隔须为1至168的整数小时"
                        return@Button
                    }
                    interval
                }
                saving=true;manager.action(p.id){try{saveDeviceMaintenance(clean,restart,revision,value,days.takeIf { weekly });dirty=false;message="维护设置已确认并保存"}
                    catch(e:CancellationException){throw e}catch(e:Exception){message="保存失败，草稿已保留：${e.message}"}finally{saving=false}}
            },enabled=enabled){Text("保存维护设置")}
            OutlinedButton(onClick={saving=true;manager.action(p.id){try{cleanDeviceResources();message="检查完成"}
                catch(e:CancellationException){throw e}catch(e:Exception){message="未执行回收：${e.message}"}finally{saving=false}}},enabled=enabled&&!state.pending){Text("立即检查并回收")}
            TextButton(onClick={dirty=false;manager.action(p.id){refresh()}},enabled=s.connected&&!saving&&!s.maintenance){Text("重新读取设置")}
            OutlinedButton(onClick={confirm("重启当前手台「${p.customName.ifBlank{p.name}}」？将中断通联，保留已保存配置。",{
                manager.action(p.id){rebootDevice()}})},enabled=s.connected&&!s.maintenance&&state.available&&!saving&&!state.pending){Text("重启本机",color=red)}
            val bootReason=when(state.lastReboot) {
                "manual"->"手动重启"
                "scheduled"->"定时重启"
                "factory"->"恢复出厂"
                "update"->"在线更新"
                else->"上电或其他原因"
            }
            Text("最近启动 $bootReason",color=secondary,style=MaterialTheme.typography.bodySmall)
            val result=when(state.result){"reclaimed"->"已回收过期资源";"nothing_to_reclaim"->"无需清理";else->"尚未执行"}
            Text("$result · 检查${state.runs}次 · 释放缓冲${state.bytes}字节/解码器${state.decoders}个",color=secondary,style=MaterialTheme.typography.bodySmall)
            Text(if(state.lastCleanupSeconds>0)"上次检查：启动后${state.lastCleanupSeconds}秒" else "尚无检查记录",color=secondary,style=MaterialTheme.typography.bodySmall)
            Text("内部可用${state.internalFree}字节 · 最大连续${state.largest}字节 · PSRAM可用${state.psram}字节",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        if(dirty)Text("有未保存设置",color=amber)
        if(message.isNotBlank())Text(message,color=secondary)
    }
}
