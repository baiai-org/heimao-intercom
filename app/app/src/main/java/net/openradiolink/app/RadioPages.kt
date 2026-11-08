@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import android.os.SystemClock
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material.icons.outlined.Sensors
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.semantics.*
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import kotlinx.coroutines.delay
import java.text.DateFormat
import java.util.Date

@Composable internal fun EmptySession(state:SessionView,devices:()->Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(28.dp),verticalArrangement=Arrangement.Center,horizontalAlignment=Alignment.CenterHorizontally) {
        Surface(shape=RoundedCornerShape(28.dp),color=MaterialTheme.colorScheme.primaryContainer){Icon(Icons.Default.Radio,null,tint=blue,modifier=Modifier.padding(24.dp).size(48.dp))}
        Spacer(Modifier.height(24.dp));Text("你的电台，随时通联",style=MaterialTheme.typography.headlineSmall)
        Text("添加或选择一台${Brand.DEVICE}，即可查看频率、切换信道和开始收听。",color=secondary,modifier=Modifier.padding(vertical=16.dp))
        if(state.error.isNotBlank())Text(state.error,color=red,modifier=Modifier.padding(bottom=16.dp))
        Button(onClick=devices,modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text("打开设备列表")}
    }
}

@Composable internal fun MorePage(p:DeviceProfile?,s:SessionView,open:(String)->Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),verticalArrangement=Arrangement.spacedBy(18.dp)) {
        Column(verticalArrangement=Arrangement.spacedBy(4.dp)){
            Text("更多",fontSize=29.sp,fontWeight=FontWeight.Medium)
            Text(p?.let{"设置将应用于 ${it.displayName}"} ?: "选择电台后可调整参数",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        Section("当前电台",dense=true) {
            val entries=listOf(
                Triple("手台名称",p?.displayName ?: "未连接","手台名称"),
                Triple("射频与模块",s.radio.value("band"),"射频参数"),
                Triple("音频与尾音","音量 / EQ","音频与尾音"),
                Triple("手台端 Wi-Fi",s.wifi.optString("ssid").ifBlank{"已保存网络"},"手台端 Wi-Fi"),
                Triple("模式切换",modes.firstOrNull{it.first==s.status.value("intercom.mode")}?.second ?: "工作模式","网络语音"))
            entries.forEachIndexed{i,(title,value,route)->
                if(i>0)HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
                SettingRow(title,value,enabled=p!=null){open(route)}
            }
        }
        Section("维护与应用",dense=true) {
            SettingRow("APP 发话音量","麦克风 / 本机试听"){open("APP 发话音量")}
            HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
            SettingRow("诊断与维护","日志 / 固件",enabled=p!=null){open("诊断与维护")}
            HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
            SettingRow("板载按键","4 个功能键",enabled=p!=null){open("板载按键")}
            HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
            SettingRow("外观","深色 / 浅色"){open("外观")}
            HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
            SettingRow("连接与权限","后台与手机设置"){open("连接与权限")}
        }
    }
}

@Composable internal fun DevicePage(profiles:List<DeviceProfile>,state:SessionView,found:List<DiscoveredBoard>,discovering:Boolean,
    connect:(DeviceProfile)->Unit,add:()->Unit,edit:(DeviceProfile)->Unit,entries:(DeviceProfile)->Unit,
    remove:(DeviceProfile)->Unit,favorite:(DeviceProfile)->Unit,up:(DeviceProfile)->Unit,discovery:()->Unit,provision:()->Unit,selectFound:(DiscoveredBoard)->Unit,disconnect:(String)->Unit) {
    LazyColumn(Modifier.fillMaxSize(),contentPadding=PaddingValues(20.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
        item {Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically){Text("我的手台",fontSize=29.sp,fontWeight=FontWeight.Medium,modifier=Modifier.weight(1f));IconButton(onClick=add){Icon(Icons.Default.Add,"添加设备")}}}
        item {Column(verticalArrangement=Arrangement.spacedBy(12.dp)) {
            Text("只连接当前使用的这一台",color=secondary,style=MaterialTheme.typography.bodySmall)
            FlowRow(horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                OutlinedButton(onClick=discovery,shape=RoundedCornerShape(14.dp)){Text(if(discovering)"停止发现" else "发现附近手台",fontSize=13.sp)}
                OutlinedButton(onClick=provision,shape=RoundedCornerShape(14.dp)){Text("连接配网热点",fontSize=13.sp)}
            }
        }}
        if(profiles.isEmpty())item{Text("可添加局域网、手机热点或自行穿透的公网网址。每台电台可保存多个访问入口。",color=secondary)}
        items(profiles.sortedByDescending{it.favorite},key={it.id}){p->
            var menu by remember(p.id){mutableStateOf(false)}
            val current=state.profileId==p.id
            Surface(color=panel,shape=RoundedCornerShape(20.dp),border=if(current)BorderStroke(1.5.dp,blue) else null) {
                Column(Modifier.fillMaxWidth().padding(16.dp),verticalArrangement=Arrangement.spacedBy(6.dp)) {
                    Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)){
                        Icon(Icons.Outlined.Sensors,null,modifier=Modifier.size(20.dp),tint=secondary)
                        Text(p.displayName,fontSize=16.sp,fontWeight=FontWeight.Medium,modifier=Modifier.weight(1f))
                        if(p.favorite)Icon(Icons.Default.Star,"已收藏",tint=amber,modifier=Modifier.size(16.dp))
                        Box{IconButton(onClick={menu=true}){Icon(Icons.Default.MoreHoriz,"管理 ${p.displayName}")};DropdownMenu(menu,{menu=false}){
                            listOf("访问入口" to {entries(p)},"本机备注" to {edit(p)},(if(p.favorite)"取消收藏" else "收藏设备") to {favorite(p)},"向上排序" to {up(p)},"移除档案" to {remove(p)}).forEach{(label,action)->DropdownMenuItem(text={Text(label,color=if(label=="移除档案")red else MaterialTheme.colorScheme.onSurface)},onClick={menu=false;action()})}
                        }}
                    }
                    Text("${p.deviceId.takeLast(6).uppercase()} · ${p.endpoint().address}",color=secondary,style=MaterialTheme.typography.bodySmall)
                    if(p.notes.isNotBlank())Text(p.notes,color=secondary,style=MaterialTheme.typography.bodySmall)
                    Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(12.dp)){
                        Column(Modifier.weight(1f)) {
                            Text(if(current)state.phase else "未连接",color=if(current&&state.connected)green else secondary,style=MaterialTheme.typography.bodySmall)
                            if(!current&&p.lastSeen>0)Text(DateFormat.getDateTimeInstance(DateFormat.SHORT,DateFormat.SHORT).format(Date(p.lastSeen)),color=secondary,fontSize=12.sp,lineHeight=18.sp)
                        }
                        FilledTonalButton(onClick={connect(p)},enabled=!state.maintenance,shape=RoundedCornerShape(15.dp),contentPadding=PaddingValues(horizontal=15.dp,vertical=10.dp),modifier=Modifier.heightIn(min=48.dp)){Text(if(current&&state.connected)"返回通联" else "连接电台",fontSize=14.sp)}
                        if(current&&state.connected)Button(onClick={disconnect(p.id)},shape=RoundedCornerShape(15.dp),
                            colors=ButtonDefaults.buttonColors(containerColor=red,contentColor=Color.White),
                            contentPadding=PaddingValues(horizontal=15.dp,vertical=10.dp),modifier=Modifier.heightIn(min=48.dp)){Text("断开连接",fontSize=14.sp)}
                    }
                }
            }
        }
        if(discovering){item{Text("发现的电台",style=MaterialTheme.typography.titleMedium)};items(found,key={it.id+it.address}){board->Section(board.name){Text("${board.id.takeLast(6).uppercase()} · ${board.address}",color=secondary);Text(board.source,color=secondary,style=MaterialTheme.typography.bodySmall);TextButton(onClick={selectFound(board)},enabled=!state.maintenance){Text("选中并连接")}}}}
    }
}
