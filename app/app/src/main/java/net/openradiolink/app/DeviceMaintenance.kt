package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.text.input.KeyboardType
import kotlinx.coroutines.CancellationException
import org.json.JSONObject

internal data class DeviceMaintenanceState(
    val available:Boolean=false,val ready:Boolean=false,val revision:Long=-1,
    val cleanup:Boolean=true,val restart:Boolean=false,val hours:Int=24,val remaining:Long=0,
    val waiting:Boolean=false,val pending:Boolean=false,val idleSeconds:Long=0,val busy:String="",
    val runs:Long=0,val bytes:Long=0,val decoders:Long=0,val internalFree:Long=0,val largest:Long=0,
    val psram:Long=0,val result:String="",val lastReboot:String="",
    val lastCleanupSeconds:Long=0,
) {
    companion object {
        fun parse(json:JSONObject):DeviceMaintenanceState {
            if(json.optInt("version")!=1 || !json.optBoolean("available",true))return DeviceMaintenanceState()
            val revision=json.optLong("revision",-1);val hours=json.optInt("restart_hours",0)
            val complete=revision in 0..4294967295L && hours in 1..168 &&
                json.opt("revision") is Number && json.getDouble("revision")==revision.toDouble() &&
                json.opt("restart_hours") is Number && json.getDouble("restart_hours")==hours.toDouble() &&
                json.opt("cleanup_enabled") is Boolean && json.opt("restart_enabled") is Boolean &&
                json.optString("device_id").isNotBlank() && json.optString("boot_id").isNotBlank()
            return DeviceMaintenanceState(available=complete,ready=complete&&json.optBoolean("ready"),revision=revision,
                cleanup=json.optBoolean("cleanup_enabled"),restart=json.optBoolean("restart_enabled"),hours=hours,
                remaining=json.optLong("restart_remaining_s"),waiting=json.optBoolean("restart_waiting"),pending=json.optBoolean("reboot_pending"),
                idleSeconds=json.optLong("idle_s"),busy=json.optString("busy"),runs=json.optLong("cleanup_runs"),
                bytes=json.optLong("released_buffer_bytes"),decoders=json.optLong("released_decoders"),
                internalFree=json.optLong("internal_free"),largest=json.optLong("internal_largest"),psram=json.optLong("psram_free"),
                result=json.optString("result"),lastReboot=json.optString("last_reboot"),lastCleanupSeconds=json.optLong("last_cleanup_uptime_s"))
        }
    }
}
@Composable internal fun DeviceMaintenanceSection(manager:DeviceManager,p:DeviceProfile,s:SessionView,confirm:(String,()->Unit)->Unit) {
    val state=DeviceMaintenanceState.parse(s.deviceMaintenance)
    var dirty by remember(p.id){mutableStateOf(false)}
    var clean by remember(p.id){mutableStateOf(true)};var restart by remember(p.id){mutableStateOf(false)}
    var hours by remember(p.id){mutableStateOf("24")};var revision by remember(p.id){mutableLongStateOf(-1)}
    var saving by remember(p.id){mutableStateOf(false)};var message by remember(p.id){mutableStateOf("")}
    LaunchedEffect(state.revision,state.available,dirty) {
        if(state.available&&!dirty){clean=state.cleanup;restart=state.restart;hours=state.hours.toString();revision=state.revision}
    }
    val enabled=s.connected&&!s.maintenance&&state.ready&&!saving
    Section("设备自动维护与重启") {
        if(!state.available) {
            Text(if(s.deviceMaintenance.has("err"))s.deviceMaintenance.optString("err") else "当前手台未提供完整维护状态，请刷新或使用配套新固件。",color=secondary)
        } else {
            if(!state.ready)Text("维护配置读取失败，自动维护已停止，请检查诊断。",color=red)
            Row(verticalAlignment=Alignment.CenterVertically) {Text("自动资源维护",Modifier.weight(1f));Switch(clean,{clean=it;dirty=true},enabled=enabled)}
            Text("每5分钟检查；连续空闲60秒后回收过期资源，保留配置和有效音频缓存。",color=secondary,style=MaterialTheme.typography.bodySmall)
            Row(verticalAlignment=Alignment.CenterVertically) {Text("定时重启",Modifier.weight(1f));Switch(restart,{restart=it;dirty=true},enabled=enabled)}
            OutlinedTextField(hours,{hours=it;dirty=true},label={Text("间隔小时：1～168，常用6/12/24/48/72")},singleLine=true,
                keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number),enabled=enabled,modifier=Modifier.fillMaxWidth())
            Text("由手台独立计时；保存或重启后重新计时。到期等待连续空闲60秒。",color=secondary,style=MaterialTheme.typography.bodySmall)
            val busy=when(state.busy){"idle"->"空闲";"writing"->"正在写入";"transmitting"->"正在发射或测试";"receiving"->"正在接收";else->"音频尚未结束"}
            Text((if(state.pending)"重启请求已接受" else if(!state.restart)"定时重启已关闭" else if(state.waiting)"已到期，等待空闲" else
                "距下次重启 ${state.remaining/3600}小时${state.remaining%3600/60}分钟")+" · $busy · 已空闲${state.idleSeconds}秒",color=secondary)
            Button(onClick={
                val value=hours.toIntOrNull()
                if(value==null||value !in 1..168){message="间隔须为1至168的整数小时";return@Button}
                saving=true;manager.action(p.id){try{saveDeviceMaintenance(clean,restart,value,revision);dirty=false;message="维护设置已确认并保存"}
                    catch(e:CancellationException){throw e}catch(e:Exception){message="保存失败，草稿已保留：${e.message}"}finally{saving=false}}
            },enabled=enabled){Text("保存维护设置")}
            OutlinedButton(onClick={saving=true;manager.action(p.id){try{cleanDeviceResources();message="检查完成"}
                catch(e:CancellationException){throw e}catch(e:Exception){message="未执行回收：${e.message}"}finally{saving=false}}},enabled=enabled&&!state.pending){Text("立即检查并回收")}
            TextButton(onClick={dirty=false;manager.action(p.id){refresh()}},enabled=s.connected&&!saving&&!s.maintenance){Text("重新读取设置")}
            OutlinedButton(onClick={confirm("重启当前手台「${p.customName.ifBlank{p.name}}」？将中断通联，保留已保存配置。",{
                manager.action(p.id){rebootDevice()}})},enabled=s.connected&&!s.maintenance&&state.available&&!saving&&!state.pending){Text("重启本机",color=red)}
            val result=when(state.result){"reclaimed"->"已回收过期资源";"nothing_to_reclaim"->"无需清理";else->"尚未执行"}
            Text("$result · 检查${state.runs}次 · 释放缓冲${state.bytes}字节/解码器${state.decoders}个",color=secondary,style=MaterialTheme.typography.bodySmall)
            Text(if(state.lastCleanupSeconds>0)"上次检查：启动后${state.lastCleanupSeconds}秒" else "尚无检查记录",color=secondary,style=MaterialTheme.typography.bodySmall)
            Text("内部可用${state.internalFree}字节 · 最大连续${state.largest}字节 · PSRAM可用${state.psram}字节",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        if(dirty)Text("有未保存设置",color=amber)
        if(message.isNotBlank())Text(message,color=secondary)
    }
}
