@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import android.content.Intent
import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.input.pointer.pointerInput
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.unit.sp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.text.DateFormat
import java.util.Date

@Composable internal fun EndpointEditor(profile:DeviceProfile?,entry:AccessPoint?,dismiss:()->Unit,
    save:(DeviceProfile?,AccessPoint,String,Boolean)->Unit) {
    var name by remember{mutableStateOf(profile?.displayName ?: "")};var label by remember{mutableStateOf(entry?.label ?: "局域网")}
    var address by remember{mutableStateOf(entry?.address ?: "")};var mode by remember{mutableStateOf(entry?.mode ?: "LAN")}
    var user by remember{mutableStateOf(entry?.username ?: "")};var pass by remember{mutableStateOf(entry?.password ?: "")}
    var bearer by remember{mutableStateOf(entry?.bearer ?: "")};var audio by remember{mutableStateOf(entry?.audioAddress ?: "")}
    var audioAuth by remember{mutableStateOf(entry?.audioAuthorization ?: "")};var advanced by remember{mutableStateOf(false)};var error by remember{mutableStateOf("")}
    fun submit(connect:Boolean){
        try {
            val url=EndpointAddress.parse(address)
            val next=(entry ?: AccessPoint(address=url.toString())).copy(label=label.ifBlank{"访问入口"},address=url.toString(),mode=mode,
                username=user,password=pass,bearer=bearer,audioAddress=audio,audioAuthorization=audioAuth)
            save(profile,next,name.ifBlank{profile?.displayName ?: url.host},connect)
        }catch(e:Exception){error=e.message ?: "地址无效"}
    }
    ModalBottomSheet(onDismissRequest=dismiss,sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true)) {
      Column(Modifier.fillMaxWidth().fillMaxHeight(.94f).padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()).imePadding().padding(horizontal=24.dp).padding(bottom=16.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
        Text(if(profile==null)"添加设备" else "访问入口",style=MaterialTheme.typography.headlineSmall)
        Column(Modifier.weight(1f).verticalScroll(rememberScrollState()),verticalArrangement=Arrangement.spacedBy(16.dp)) {
        if(profile==null)OutlinedTextField(name,{name=it},label={Text("设备名称（可选）")},singleLine=true,modifier=Modifier.fillMaxWidth())
        Choice("接入方式",mode,listOf("LAN" to "路由器局域网","HOTSPOT" to "本机热点","PUBLIC" to "用户公网入口","SETUP" to "手台配网热点")){mode=it}
        OutlinedTextField(label,{label=it},label={Text("入口备注")},singleLine=true,modifier=Modifier.fillMaxWidth())
        OutlinedTextField(address,{address=it},label={Text("网址或网络地址")},placeholder={Text("https://域名:端口/路径/")},singleLine=true,modifier=Modifier.fillMaxWidth(),keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Uri))
        Text("支持 IPv4、[IPv6]:端口及代理路径。公网穿透由你自行配置。",color=secondary,style=MaterialTheme.typography.bodySmall)
        TextButton(onClick={advanced=!advanced}){Text(if(advanced)"收起认证与音频设置" else "认证与高级音频入口")}
        if(advanced){
            OutlinedTextField(user,{user=it},label={Text("入口认证用户名")},singleLine=true)
            OutlinedTextField(pass,{pass=it},label={Text("入口认证密码")},singleLine=true,visualTransformation=PasswordVisualTransformation())
            OutlinedTextField(bearer,{bearer=it},label={Text("Bearer 凭据（选填）")},singleLine=true,visualTransformation=PasswordVisualTransformation())
            OutlinedTextField(audio,{audio=it},label={Text("独立音频网址（可选）")},placeholder={Text("wss://域名:端口/路径/ws/sdr")},singleLine=true)
            OutlinedTextField(audioAuth,{audioAuth=it},label={Text("独立音频入口 Authorization（选填）")},visualTransformation=PasswordVisualTransformation())
            Text("留空则使用设备入口的音频通道。跨域音频不会自动携带主入口认证。",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        if(error.isNotBlank())Text(error,color=red)
        }
        Button(onClick={submit(true)},shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text("保存并连接")}
        Row(Modifier.fillMaxWidth(),horizontalArrangement=Arrangement.SpaceEvenly){TextButton(onClick={submit(false)}){Text("仅保存")};TextButton(onClick=dismiss){Text("取消")}}
      }
    }
}

@Composable internal fun Choice(label:String,value:String,options:List<Pair<String,String>>,changed:(String)->Unit) {
    var open by remember{mutableStateOf(false)}
    Box {OutlinedButton(onClick={open=true},modifier=Modifier.fillMaxWidth()) {Text("$label：${options.firstOrNull{it.first==value}?.second ?: value.ifBlank{"未设置"}}",modifier=Modifier.weight(1f));Icon(Icons.Default.ArrowDropDown,null)}
        DropdownMenu(expanded=open,onDismissRequest={open=false}){options.forEach{(key,text)->DropdownMenuItem(text={Text(text)},onClick={open=false;changed(key)})}}}
}

@Composable internal fun RadioFields(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    Section("射频参数") {
        Parameter(manager,p,s,"rf_band","模块频段",s.radio.value("band"),listOf("UHF" to "UHF","VHF" to "VHF"))
        FrequencyEditor(manager,p,s)
        Parameter(manager,p,s,"bw","带宽",s.status.value("freq.bw"),listOf("0" to "12.5 kHz 窄带","1" to "25 kHz 宽带"))
        Parameter(manager,p,s,"sq","模块静噪",s.status.value("freq.sq"),hint="0–8；0 为开放静噪")
        Parameter(manager,p,s,"vol","电台模块音量",s.status.value("freq.vol"),hint="1–8")
        Parameter(manager,p,s,"power","发射功率",s.status.value("freq.power"),listOf("0" to "低功率","1" to "高功率"))
        if(s.radio.optBoolean("power_pending"))Text("功率将在发话结束后生效",color=amber)
    }
    Section("亚音设置") {
        ToneEditor(manager,p,s,"发射亚音","tx")
        HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
        ToneEditor(manager,p,s,"接收亚音","rx")
    }
}

@Composable internal fun AudioFields(manager:DeviceManager,p:DeviceProfile,s:SessionView,file:(Boolean)->Unit,confirm:(String,()->Unit)->Unit) {
    Section("固定音频参数") {
        Parameter(manager,p,s,"monitor","本地守听",if(s.status.optString("mode").startsWith("MONITOR"))"1" else "0",yesNo)
        Parameter(manager,p,s,"spk_vol","手台喇叭音量",s.status.value("spk"),(0..7).map{it.toString() to if(it==0)"关闭" else "第 $it 档"})
        Text("发射路径增益（每条线独立）",color=blue,style=MaterialTheme.typography.titleSmall)
        Parameter(manager,p,s,"txgain","本机麦克风 → 射频",s.status.value("txgain"),hint="机身麦克风专用；16 = 1 倍")
        Parameter(manager,p,s,"ntxgain","手机/网络语音 → 射频",s.status.value("ntxgain"),hint="只影响 App/网页发话；16 = 1 倍")
        Parameter(manager,p,s,"bgtx","网络桥接 → 射频重发",s.status.value("bgtx"),hint="只影响模拟↔网络转发；16 = 1 倍，0 = 关闭")
        Parameter(manager,p,s,"gain","射频 → 喇叭",s.status.value("gain"),hint="只影响本机喇叭这条接收路；16 = 1 倍")
        Parameter(manager,p,s,"rxnetgain","射频 → 网络",s.status.value("rxnetgain"),hint="只影响推给 App/浏览器的音量；64 = +12 dB")
        Row(verticalAlignment=Alignment.CenterVertically,modifier=Modifier.fillMaxWidth().padding(vertical=6.dp)) {
            Column(Modifier.weight(1f)) {
                Text("发射预加重",style=MaterialTheme.typography.bodyLarge)
                Text("+6 dB/倍频程（1.2 kHz 起）；对端是商业手台时保持开启",color=secondary,style=MaterialTheme.typography.bodySmall)
            }
            Switch(checked=s.status.value("txpre")=="1",
                onCheckedChange={on->if(s.connected&&!s.maintenance)manager.action(p.id){set("txpre",if(on)"1" else "0")}},
                enabled=s.connected&&!s.maintenance)
        }
        Text("上次发射峰值：送入 "+s.status.value("sdr.rawpk")+" / 送出 "+s.status.value("sdr.outpk")+
             " / 削顶 "+s.status.value("sdr.clip")+"（满刻度 32767，发一次话后刷新）",
             color=secondary,style=MaterialTheme.typography.bodySmall)
        Text("板载麦克风前级",color=blue,style=MaterialTheme.typography.titleSmall)
        Parameter(manager,p,s,"micgain","板载麦克风增益档位",s.status.value("micgain"),hint="每档 6 dB，存 NVS")
        Parameter(manager,p,s,"adcvol","板载 ADC 音量",s.status.value("adcvol"))
        Text("手机收听音量由系统媒体音量键调整。",color=secondary)
    }
    Section("均衡器") {
        Equalizer(manager,p,s)
    }
    Section("尾音") {
        val tails=listOf("关闭","单音 Beep","业余无线电 K-CHOO","宝峰 ROGER","摩托罗拉 MDC-1200","Icom IDAS *","Icom IDAS #","泉盛 UV-K5 双音","自定义音频")
        Parameter(manager,p,s,"tail","尾音类型",s.status.value("tail.id"),tails.mapIndexed{i,t->i.toString() to t})
        Parameter(manager,p,s,"tail_modes","应用范围",s.status.value("tail.modes"),listOf("1" to "本地","2" to "公网直连","4" to "网络中继","3" to "本地与直连","5" to "本地与中继","6" to "直连与中继","7" to "所有模式"))
        Text("自定义音频：${s.status.value("tail.custom_s")} 秒",color=secondary)
        FlowRow {OutlinedButton(onClick={manager.action(p.id){stopListening();set("tail_preview",s.status.value("tail.id"))}},enabled=s.connected&&!s.maintenance&&s.status.optJSONObject("tail")?.optInt("id",0)!=0){Text("本机试听")}
            TextButton(onClick={manager.action(p.id){set("tail_preview","0")}}){Text("停止试听")}
            OutlinedButton(onClick={file(false)},enabled=s.connected&&!s.maintenance){Text("上传 WAV / MP3")}
            TextButton(onClick={confirm("删除手台保存的自定义尾音？",{manager.action(p.id){set("tail_custom_del","1")}})}){Text("删除自定义",color=red)}}
    }
}

@Composable internal fun WifiFields(manager:DeviceManager,p:DeviceProfile,s:SessionView,wirelessSettings:()->Unit,confirm:(String,()->Unit)->Unit) {
    var editing by remember(p.id){mutableStateOf<Long?>(null)}
    var ssid by remember(p.id){mutableStateOf("")};var password by remember(p.id){mutableStateOf("")};var priority by remember(p.id){mutableStateOf("0")}
    var autoJoin by remember(p.id){mutableStateOf(true)};var passwordChanged by remember(p.id){mutableStateOf(false)}
    var scanned by remember(p.id){mutableStateOf(JSONArray())}
    val context=LocalContext.current
    val currentHost=s.wifi.optString("host").trim().lowercase()
    /* Always start empty: the operator must type the name, so a factory "heimao"
     * or an old value can never be carried through with zero input. */
    var domain by remember(p.id){mutableStateOf("")}
    var pendingSave by remember(p.id){mutableStateOf<(() -> Unit)?>(null)}
    var domainSaving by remember(p.id){mutableStateOf(false)}
    val domainName=PanelDomain.normalize(domain)
    val domainOk=PanelDomain.valid(domainName)
    fun payload():JSONObject {
        val rank=priority.toIntOrNull();require(ssid.isNotBlank() && rank!=null && rank in -100..100){"请填写名称和 −100..100 的优先级"}
        require(domainOk){"请先填写合法的管理面板访问域名"}
        val body=JSONObject().put("op","save").put("ssid",ssid).put("auto_join",autoJoin).put("priority",rank)
            .put("host",domainName)
        editing?.let{body.put("id",it)}
        if(editing==null || passwordChanged)body.put("password",password)
        return body
    }
    fun saveDomain() {
        val profiles=s.wifi.optJSONArray("profiles");val activeId=s.wifi.optLong("active_id",0)
        var target:JSONObject?=null
        if(profiles!=null&&profiles.length()>0){
            for(i in 0 until profiles.length()){val n=profiles.optJSONObject(i) ?: continue;if(activeId>0&&n.optLong("id")==activeId)target=n}
            if(target==null)target=profiles.optJSONObject(0)
        }
        val chosen=target
        if(chosen==null){manager.message("请先保存一个 Wi-Fi 档案，再设置域名");return}
        domainSaving=true
        manager.action(p.id){
            try { wifi(JSONObject().put("op","save").put("id",chosen.optLong("id")).put("host",domainName)
                .put("auto_join",chosen.optBoolean("auto_join",true)).put("priority",chosen.optInt("priority",0)))
            } finally { withContext(Dispatchers.Main){domainSaving=false} }
        }
    }
    Section("管理面板访问域名") {
        Text(if(currentHost.isBlank())"尚未设置。设好后，同一网络里的手机或电脑用 https://<域名>.local 打开管理面板。"
            else if(currentHost=="heimao")"当前为出厂默认 ${PanelDomain.address(currentHost)}，建议改成容易记的名字。"
            else "当前地址：${PanelDomain.address(currentHost)}",color=secondary,style=MaterialTheme.typography.bodySmall)
        OutlinedTextField(domain,{domain=it},label={Text("域名（必填）")},
            placeholder={Text(if(currentHost.isBlank())"例如 heimao-u13" else "当前 ${currentHost}，改名请重新输入")},
            singleLine=true,isError=!domainOk,modifier=Modifier.fillMaxWidth(),keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Uri))
        Text(if(domainOk)"管理面板地址 ${PanelDomain.address(domainName)}" else PanelDomain.error(domainName),
            color=if(domainOk)green else red,style=MaterialTheme.typography.bodySmall)
        Text("只用字母、数字和连字符，首尾须为字母或数字，最长 63 个；后缀 .local 由手台自动补上。",color=secondary,style=MaterialTheme.typography.bodySmall)
        FlowRow {
            Button(onClick={pendingSave={saveDomain()}},enabled=domainOk&&!domainSaving&&s.connected&&!s.maintenance){Text("保存域名")}
            if(currentHost.isNotBlank())OutlinedButton(onClick={
                runCatching{context.startActivity(Intent(Intent.ACTION_VIEW,Uri.parse(PanelDomain.address(currentHost))))}
                    .onFailure{manager.message("找不到可打开该地址的浏览器")}
            }){Text("打开管理面板")}
        }
    }
    pendingSave?.let{ action ->
        AlertDialog(onDismissRequest={pendingSave=null},
            title={Text("请确认局域网内没有重复域名")},
            text={Text("同一局域网里，每台设备的管理面板域名必须唯一。请先自行检查现有设备是否已在使用 ${domainName}.local。\n\n如果重名：两台会互相抢占，地址会时好时坏，手台可能被自动改成 ${domainName}-2.local。")},
            confirmButton={TextButton(enabled=!domainSaving,onClick={pendingSave=null;action()}){Text(if(domainSaving)"正在保存…" else "我已确认，继续")}},
            dismissButton={TextButton(onClick={pendingSave=null}){Text("取消")}})
    }
    Section("手台端 Wi-Fi") {
        Text("${s.wifi.optString("state")} · ${s.wifi.optString("ssid")}")
        if(p.endpoint().mode=="SETUP")Text(
            (if(s.wifi.optLong("setup_hold_ms")>0)"配网连接保护已开启。" else "")+
                "此热点用于连接手台；系统提示无互联网时，请选择保持连接。",color=secondary)
        Text("Wi-Fi 断开后搜索所有允许自动连接的档案；连续 60 秒无有效 IP 则恢复开放配网热点。",color=secondary)
        val list=s.wifi.optJSONArray("profiles") ?: JSONArray()
        repeat(list.length()){i->val n=list.getJSONObject(i);val id=n.getLong("id")
            HorizontalDivider();Text(n.getString("ssid"),style=MaterialTheme.typography.titleSmall)
            Text("优先级 ${n.optInt("priority")} · ${if(n.optBoolean("auto_join"))"自动连接开启" else "仅手动连接"}"+(if(n.optLong("retry_in_ms")>0)" · 冷却 ${n.optLong("retry_in_ms")/1000} 秒" else ""),color=secondary)
            FlowRow {TextButton(onClick={manager.connectSavedNetwork(p.id,id)}){Text("连接")}
                TextButton(onClick={editing=id;ssid=n.getString("ssid");password="";passwordChanged=false;priority=n.optInt("priority").toString();autoJoin=n.optBoolean("auto_join")}){Text("编辑")}
                TextButton(onClick={manager.action(p.id){wifi(JSONObject().put("op","save").put("id",id).put("auto_join",!n.optBoolean("auto_join")).put("priority",n.optInt("priority")))}}){Text(if(n.optBoolean("auto_join"))"关闭自动连接" else "允许自动连接")}
                TextButton(onClick={confirm("只忘记 Wi-Fi“${n.getString("ssid")}”？",{manager.action(p.id){wifi(JSONObject().put("op","forget").put("id",id))}})}){Text("忘记",color=red)}}
        }
        FlowRow {OutlinedButton(onClick={manager.action(p.id){
            claimIfNeeded();api.bytes("api/scan?force=1",32768)
            var scans=0
            do{kotlinx.coroutines.delay(350)}while(api.json("api/wifi").optBoolean("scan_running") && ++scans<20)
            val found=JSONArray(String(api.bytes("api/scan",32768),Charsets.UTF_8))
            manager.scope.launch{scanned=found}
        }},enabled=s.connected&&!s.maintenance){Text("扫描手台附近 Wi-Fi")}
            if(editing!=null)TextButton(onClick={editing=null;ssid="";password="";passwordChanged=false;priority="0";autoJoin=true}){Text("取消编辑 / 新增")}}
        if(scanned.length()>0)Choice("扫描结果","",(0 until scanned.length()).map{val n=scanned.getJSONObject(it);n.getString("ssid") to "${n.getString("ssid")} · ${n.optInt("rssi")} dBm"}){ssid=it;editing=null;password="";passwordChanged=false}
        OutlinedTextField(ssid,{ssid=it},label={Text("新增 / 修改 Wi-Fi 名称")},modifier=Modifier.fillMaxWidth())
        OutlinedTextField(password,{password=it;passwordChanged=true},label={Text(if(editing==null)"Wi-Fi 密码，开放网络留空" else "新密码，留空保持原密码")},modifier=Modifier.fillMaxWidth(),visualTransformation=PasswordVisualTransformation())
        if(editing!=null)TextButton(onClick={password="";passwordChanged=true}){Text(if(passwordChanged&&password.isEmpty())"将保存为开放网络" else "清除原密码，设为开放网络")}
        OutlinedTextField(priority,{priority=it},label={Text("优先级 −100..100")},modifier=Modifier.fillMaxWidth(),keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Number))
        Row(verticalAlignment=Alignment.CenterVertically){Switch(autoJoin,{autoJoin=it});Text("允许自动连接")}
        /* Saving a profile writes the domain too, so it must pass the same
         * duplicate-name confirmation as the domain button. */
        FlowRow {OutlinedButton(onClick={pendingSave={
                runCatching{payload()}.onSuccess{body->manager.action(p.id){val saved=wifi(body);manager.scope.launch{editing=saved.optLong("id").takeIf{it>0};password="";passwordChanged=false}}}.onFailure{manager.message(it.message ?: "参数无效")}
            }},enabled=domainOk&&s.connected&&!s.maintenance){Text("仅保存")}
            Button(onClick={pendingSave={
                runCatching{payload()}.onSuccess{body->manager.submitNetworkProfile(p.id,body){password="";passwordChanged=false}}.onFailure{manager.message(it.message ?: "参数无效")}
            }},enabled=domainOk&&s.connected&&!s.maintenance){Text("保存并连接")}}
        Text("同一手机开热点：收到已保存应答后，退出手台热点，开启填写的 2.4 GHz 共享热点。",color=secondary)
        TextButton(onClick=wirelessSettings){Text("打开手机网络与热点设置")}
    }
}

@Composable internal fun VoiceNetworkFields(manager:DeviceManager,p:DeviceProfile,s:SessionView,confirm:(String,()->Unit)->Unit) {
    val active=s.status.value("intercom.mode").toIntOrNull() ?: 0
    val state=s.status.value("intercom.state")
    val peers=s.status.optJSONObject("intercom")?.optInt("peers",0) ?: 0
    var switching by remember(p.id){mutableStateOf(false)}
    fun switchTo(mode:Int) {
        if(switching || mode==active || !s.connected || s.maintenance) return
        switching=true
        manager.action(p.id){
            try { set("ix_mode",mode.toString()) }
            finally { withContext(Dispatchers.Main){switching=false} }
        }
    }
    val accents=listOf(green,violet,amber)
    Section("工作模式") {
        NetworkModes.cards.forEachIndexed{i,card->
            if(i>0)Spacer(Modifier.height(8.dp))
            val (label,linked)=NetworkModes.status(card.mode,active,state,peers)
            ModeCard(card.title,card.hint,label,linked,NetworkModes.isCurrent(card.mode,active),
                enabled=s.connected&&!s.maintenance&&!switching,accent=accents[i]){switchTo(card.mode)}
        }
        Spacer(Modifier.height(4.dp))
        Text(if(switching)"正在切换工作模式…" else "切换模式会重建网络语音通道；正在发话时请先松开 PTT。",
            color=secondary,style=MaterialTheme.typography.bodySmall)
    }
    when(active) {
        0 -> Section("本地电台") {
            Text("本机麦克风直接由手台发射，收到的音频直接进手台喇叭，不经过网络。",color=secondary)
            Text("房间号、桥接与中继设置只在网络模式下出现。",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
        1 -> {
            /* Same information, same order as the web panel's 公网直连 module:
             * own public address first, room, online users, then the rest. */
            /* The parser reads ip6 from the wifi object inside /api/status;
             * SessionView.wifi is the profile list from /api/wifi and has no ip6. */
            val np=Np2pState.parse(s.status)
            if(!np.available) {
                Section("公网直连 P2P") {
                    Text("手台本轮没有上报 NET-P2P 数据（状态体过大被整体丢弃），刷新后重试。",color=secondary)
                }
            } else {
                Np2pAddressCard(manager,p,s,np)
                Section("互通房间") {
                    Parameter(manager,p,s,"ix_room","房间号码",s.status.value("intercom.room"),hint="1000–9999，同一房间才能互通")
                }
                Np2pPeerSection(manager,p,s,np,confirm)
                Np2pAdvanced(manager,p,s)
            }
        }
        else -> Section("网络中继") {
            Parameter(manager,p,s,"ix_broker","服务器 / 域名与端口",s.status.value("intercom.broker"))
            Parameter(manager,p,s,"ix_user","用户名",s.status.value("intercom.user"))
            Parameter(manager,p,s,"ix_pass","密码",if(s.status.value("intercom.pass")=="1")"已设置" else "",secret=true)
            TextButton(onClick={confirm("清除手台保存的中继密码？",{manager.action(p.id){set("ix_pass","")}})}){Text("清除已保存密码")}
            Text("服务器地址与账号由你自行维护；连接状态见上方卡片。",color=secondary,style=MaterialTheme.typography.bodySmall)
        }
    }
    if(active!=0) {
        Section("网络通用") {
            /* netrelay.c has no room: the EMQX server is the scope.  The P2P room
             * now has its own section above the peer list, like the web panel. */
            Parameter(manager,p,s,"ix_rf","网络发话同时发射本机电台",s.live.value("rf_tx"),yesNo)
        }
        Section("模拟桥接") {
            Parameter(manager,p,s,"bridge_en","模拟电台与网络桥接",s.status.value("bridge.en"),yesNo)
            val channels=listOf("0" to "不选择信道")+(0 until s.channels.length()).map{val c=s.channels.getJSONObject(it);(c.optInt("i",it)+1).toString() to c.optString("name")}
            Parameter(manager,p,s,"bridge_chan","桥接使用信道",s.status.value("bridge.chan"),channels)
        }
    }
}

/** Writes one plain-text run to the system clipboard; the caller shows the feedback. */
private fun copyToClipboard(context:android.content.Context,text:String) {
    val cm=context.getSystemService(android.content.Context.CLIPBOARD_SERVICE) as android.content.ClipboardManager
    cm.setPrimaryClip(android.content.ClipData.newPlainText("公网直连地址",text))
}

@Composable private fun Np2pAddressCard(manager:DeviceManager,p:DeviceProfile,s:SessionView,np:Np2pState) {
    val enabled=s.connected&&!s.maintenance
    val context=LocalContext.current
    fun copy(text:String){copyToClipboard(context,text);manager.message("已复制 $text")}
    val hint=when{
        !np.ready->"等待 STUN 结果"
        np.stale->"地址可能过期（${np.stunAgeSeconds} s）· 点击重新探测"
        else->"STUN ${np.stunAgeSeconds} s 前刷新"
    }
    Section("本机公网地址") {
        Row(verticalAlignment=Alignment.CenterVertically) {
            Text("发给对方，填到对方控制面板的地址栏",color=secondary,style=MaterialTheme.typography.bodySmall,modifier=Modifier.weight(1f))
            IconButton(onClick={copy(np.ipv4Text)},enabled=enabled&&np.ready){
                Icon(Icons.Default.ContentCopy,"复制公网地址",tint=if(np.ready)violet else secondary)
            }
        }
        /* One monospace run, like the panel's #npPub: it selects and copies
         * normally, and the 1 s refresh must not wipe a selection in progress.
         * key() keeps the node alive while the value is unchanged; a tap on the
         * address copies it, a long press still selects. */
        key(np.ipv4Text) {
            SelectionContainer {
                Text(np.ipv4Text.ifEmpty{"—"},fontFamily=FontFamily.Monospace,fontSize=24.sp,fontWeight=FontWeight.ExtraBold,
                    color=if(!np.ready)secondary else if(np.stale)amber else violet,
                    modifier=Modifier.clickable(enabled=enabled&&np.ready){copy(np.ipv4Text)})
            }
        }
        Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(4.dp)) {
            Box(Modifier.weight(1f)) {
                key(np.ipv6Text) {
                    SelectionContainer {
                        Text(if(np.ipv6Text.isNotEmpty())np.ipv6Text else "无 IPv6（路由器未下发 RA）",
                            fontFamily=FontFamily.Monospace,fontSize=15.sp,
                            color=if(np.ipv6Text.isNotEmpty())violet else secondary,
                            modifier=Modifier.clickable(enabled=enabled&&np.ipv6Text.isNotEmpty()){copy(np.ipv6Text)})
                    }
                }
            }
            if(np.ipv6Text.isNotEmpty())IconButton(onClick={copy(np.ipv6Text)},enabled=enabled){
                Icon(Icons.Default.ContentCopy,"复制 IPv6 地址",tint=violet,modifier=Modifier.size(18.dp))
            }
        }
        Text(np.natText+" · "+hint,
            color=if(np.stale)amber else secondary,style=MaterialTheme.typography.bodySmall,
            modifier=if(np.stale&&enabled)Modifier.clickable{
                /* Sending the STUN value back is the board's "force a probe" path. */
                manager.action(p.id){set("np2p_stun",s.status.optJSONObject("np2p")?.optString("stun","") ?: "")}
            } else Modifier)
    }
}

@Composable private fun Np2pPeerSection(manager:DeviceManager,p:DeviceProfile,s:SessionView,np:Np2pState,confirm:(String,()->Unit)->Unit) {
    val enabled=s.connected&&!s.maintenance
    var adding by remember(p.id){mutableStateOf(false)}
    Section("在线用户 ${np.onlineCount()}") {
        Row(verticalAlignment=Alignment.CenterVertically) {
            Text("hub 星型群聊",style=MaterialTheme.typography.bodyLarge,modifier=Modifier.weight(1f))
            if(np.hubOffline)Surface(color=MaterialTheme.colorScheme.errorContainer,shape=RoundedCornerShape(8.dp)) {
                Text("枢纽离线 ${np.hubQuietSeconds} s",color=red,fontSize=12.sp,modifier=Modifier.padding(horizontal=8.dp,vertical=2.dp))
            }
            Switch(checked=np.hub,onCheckedChange={on->manager.action(p.id){set("np2p_hub",if(on)"1" else "0")}},enabled=enabled)
        }
        if(np.rttMs>=0)Text("连通 ${np.linkedCount}/${np.peerCount} · RTT ${np.rttMs} ms",color=secondary,style=MaterialTheme.typography.bodySmall)
        if(np.peers.isEmpty())Text("还没有对端。把上方地址发给对方填写，或点下面按钮添加对方的公网地址。",color=secondary,style=MaterialTheme.typography.bodySmall)
        repeat(np.peers.size){i->
            val peer=np.peers[i]
            if(i>0)HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
            Np2pPeerRow(manager,p,s,peer,i,enabled,confirm)
        }
        OutlinedButton(onClick={adding=true},enabled=enabled){Text("添加对端")}
        Text("这是手台的语音业务对端，与 App 保存的设备档案分别管理。",color=secondary,style=MaterialTheme.typography.bodySmall)
        if(adding)AddPeerSheet(manager,p,s,{adding=false})
    }
}

@Composable private fun Np2pPeerRow(manager:DeviceManager,p:DeviceProfile,s:SessionView,peer:PeerInfo,index:Int,enabled:Boolean,confirm:(String,()->Unit)->Unit) {
    val dotColor=when(peer.state){"linked"->green;"knocking"->amber;else->MaterialTheme.colorScheme.outline}
    val stateColor=when(peer.state){"linked"->green;"knocking"->amber;else->secondary}
    Column(verticalArrangement=Arrangement.spacedBy(2.dp)) {
        Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(6.dp)) {
            Box(Modifier.size(8.dp).background(dotColor,CircleShape))
            Text(peer.name,style=MaterialTheme.typography.bodyLarge)
            Spacer(Modifier.weight(1f))
            Text(peer.stateText,color=stateColor,fontWeight=FontWeight.SemiBold,style=MaterialTheme.typography.bodySmall)
        }
        Text(peer.address,fontFamily=FontFamily.Monospace,fontSize=13.sp,color=secondary)
        Row(verticalAlignment=Alignment.CenterVertically) {
            Text("rtt ${if(peer.rttMs>=0)"${peer.rttMs} ms" else "—"} · rx ${peer.rx} / tx ${peer.tx} / 丢 ${peer.drop}",
                color=secondary,style=MaterialTheme.typography.bodySmall,modifier=Modifier.weight(1f))
            TextButton(onClick={manager.action(p.id){set("np2p_knock",index.toString())}},enabled=enabled){Text("重敲")}
            TextButton(onClick={confirm("移除对端「${peer.name}」？",{manager.action(p.id){set("np2p_peer_del",index.toString())}})},enabled=enabled){Text("删除",color=red)}
        }
    }
}

@Composable private fun AddPeerSheet(manager:DeviceManager,p:DeviceProfile,s:SessionView,dismiss:()->Unit) {
    var peer by remember{mutableStateOf("")};var address by remember{mutableStateOf("")}
    var error by remember{mutableStateOf("")}
    val nameBytes=peer.toByteArray(Charsets.UTF_8).size
    ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest=dismiss) {
        Column(Modifier.fillMaxWidth().fillMaxHeight(.7f)
            .padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding())
            .imePadding().padding(horizontal=22.dp).padding(bottom=14.dp),
            verticalArrangement=Arrangement.spacedBy(12.dp)) {
            Text("添加直连对端",style=MaterialTheme.typography.headlineSmall)
            OutlinedTextField(peer,{peer=it;error=""},label={Text("对端名称，最多 7 字节")},singleLine=true,modifier=Modifier.fillMaxWidth())
            if(nameBytes>7)Text("名称最多 7 字节，中文一个算 2 字节",color=red)
            OutlinedTextField(address,{address=it;error=""},label={Text("对端 IP:端口 / [IPv6]:端口")},singleLine=true,modifier=Modifier.fillMaxWidth(),
                keyboardOptions=KeyboardOptions(keyboardType=KeyboardType.Uri))
            Text("填对方控制面板顶部显示的公网地址；端口 54321 可省略。",color=secondary,style=MaterialTheme.typography.bodySmall)
            if(error.isNotBlank())Text(error,color=red)
            Button(onClick={
                if(nameBytes==0 || nameBytes>7){error="名称要 1–7 字节";return@Button}
                if(address.isBlank()){error="请填写对端地址";return@Button}
                manager.action(p.id){set("np2p_peer_add","$peer,$address")}
                dismiss()
            },shape=RoundedCornerShape(17.dp),modifier=Modifier.fillMaxWidth().heightIn(min=56.dp)){Text("添加")}
        }
    }
}

@Composable private fun Np2pAdvanced(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    var open by remember(p.id){mutableStateOf(false)}
    /* Relay engine diagnostics, kept out of the peer list like the panel. */
    val diag=s.status.optJSONObject("intercom")?.optString("diag","") ?: ""
    Column(verticalArrangement=Arrangement.spacedBy(8.dp)) {
        Row(Modifier.fillMaxWidth().clickable{open=!open}.padding(start=6.dp),verticalAlignment=Alignment.CenterVertically) {
            Text("高级：STUN 服务器覆盖",style=MaterialTheme.typography.titleSmall,color=secondary,modifier=Modifier.weight(1f))
            Icon(if(open)Icons.Default.ExpandLess else Icons.Default.ExpandMore,null,tint=secondary,modifier=Modifier.size(18.dp))
        }
        if(open)Surface(shape=RoundedCornerShape(20.dp),color=panel) {
            Column(Modifier.fillMaxWidth().padding(horizontal=16.dp,vertical=4.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
                Parameter(manager,p,s,"np2p_stun","STUN 服务器",s.status.value("np2p.stun"),hint="host:port；留空使用手台默认列表")
                if(diag.isNotBlank())Text("诊断: $diag",color=amber,style=MaterialTheme.typography.bodySmall)
            }
        }
    }
}

@Composable private fun ModeCard(title:String,hint:String,status:String,linked:Boolean,current:Boolean,enabled:Boolean,accent:Color,click:()->Unit) {
    /* accent is the mode color (local green / P2P purple / relay amber) the web
     * panel paints its mode buttons with; it borders and badges the active card. */
    Surface(color=if(current)MaterialTheme.colorScheme.primaryContainer else panel,shape=RoundedCornerShape(18.dp),
        border=if(current)BorderStroke(1.5.dp,accent) else BorderStroke(1.dp,MaterialTheme.colorScheme.outlineVariant),
        modifier=Modifier.fillMaxWidth().clickable(enabled=enabled && !current,onClick=click)) {
        Column(Modifier.padding(horizontal=14.dp,vertical=12.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                Text(title,style=MaterialTheme.typography.titleMedium,color=if(current)accent else MaterialTheme.colorScheme.onSurface,modifier=Modifier.weight(1f))
                if(current)Surface(color=accent,shape=RoundedCornerShape(8.dp)) {
                    /* Dark accents in the light theme, light accents in the dark
                     * one: the chip text is always the opposite of the accent. */
                    Text("当前",color=if(LocalRadioDark.current)background else Color.White,fontSize=11.sp,modifier=Modifier.padding(horizontal=8.dp,vertical=2.dp))
                }
            }
            Text(hint,color=secondary,style=MaterialTheme.typography.bodySmall)
            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(6.dp)) {
                Box(Modifier.size(8.dp).background(if(linked)green else MaterialTheme.colorScheme.outline,CircleShape))
                Text(status,color=if(linked)green else secondary,style=MaterialTheme.typography.bodySmall)
            }
        }
    }
}


@Composable internal fun FrontKeys(manager:DeviceManager,p:DeviceProfile,s:SessionView) {
    val actions=listOf("off" to "无功能","ch+" to "信道 +","ch-" to "信道 −","vol+" to "音量 +","vol-" to "音量 −","sql+" to "静噪 +","sql-" to "静噪 −","power" to "高低功率切换","monitor" to "守听开关","mode" to "模式切换","bridge" to "桥接开关")
    Section("板载功能键") {repeat(4){i ->Parameter(manager,p,s,"fp_key$i","功能键 ${i+1}",s.status.value("fp.act.$i"),actions)}}
}

@Composable internal fun Diagnostics(manager:DeviceManager,p:DeviceProfile,s:SessionView,upload:()->Unit,download:()->Unit,exportReport:()->Unit,confirm:(String,()->Unit)->Unit) {
    var report by remember(p.id){mutableStateOf("")}
    var showLogs by remember(p.id){mutableStateOf(false)}
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Section("连接与固件") {Text(p.displayName);Text(p.deviceId);Text(s.status.value("build.version"));Text("入口：${p.endpoint().address}")
            Text("内部可用内存 ${s.status.value("heapInt")} 字节；最大连续块 ${s.status.value("heapIntMax")} 字节",color=secondary)
            FlowRow {TextButton(onClick={manager.action(p.id){refresh(true)}}){Text("刷新")}
                TextButton(onClick={manager.diagnosticText(p.id,"api/adc"){report=it}}){Text("ADC")}
                TextButton(onClick={manager.diagnosticText(p.id,"api/speaker_diag"){report=it}}){Text("声音诊断状态")}
                TextButton(onClick={manager.diagnosticText(p.id,"api/speaker_hw"){report=it}}){Text("芯片配置")}}
        }
        Section("APP 发话电平") {
            val level=s.microphoneLevel
            MicrophoneMeter(level)
            Text("采集峰值 ${level.rawPeak} → 上传峰值 ${level.outputPeak}；削波 ${level.clipped}/${level.samples}",color=secondary)
            Text("手台处理峰值 ${s.status.value("sdr.rawpk")} → ${s.status.value("sdr.outpk")}；手台削波 ${s.status.value("sdr.clip")}",color=secondary)
            Text("板端计数为最近一次发话累计值；电平提示仅作调节参考。",color=secondary)
        }
        Section("本机喇叭诊断") {Text("只在手台喇叭播放，不主动键控电台。录音来自下一次实际收听。",color=secondary)
            FlowRow {listOf("arm" to "记录下一次收听","replay" to "重放录音","silence" to "数字静音","dacmute" to "DAC 静音","tone" to "低电平测试音","stop" to "停止").forEach{(op,label)->TextButton(onClick={manager.action(p.id){diagnostic(op)}}){Text(label)}}}}
        Section("文件与升级") {Text("维护前结束通联；文件和升级始终面向当前手台。",color=secondary)
            FlowRow {OutlinedButton(onClick=download,enabled=s.connected&&!s.maintenance){Text("导出运行固件")};Button(onClick=upload,enabled=s.connected&&!s.maintenance){Text("上传应用固件")}}
            if(s.taskResult.isNotBlank())SelectionContainer{Text(s.taskResult,color=green)}}
        Section("运行日志") {OutlinedButton(onClick=exportReport,enabled=s.connected&&!s.maintenance){Text("导出诊断与运行日志")}
            TextButton(onClick={showLogs=!showLogs}){Text(if(showLogs)"收起运行日志" else "查看运行日志")}
            if(showLogs){SelectionContainer{Text(s.status.optString("log"),fontFamily=FontFamily.Monospace,style=MaterialTheme.typography.bodySmall)}
            SelectionContainer{Text(s.status.optString("logx"),fontFamily=FontFamily.Monospace,style=MaterialTheme.typography.bodySmall)}}}
        if(report.isNotBlank())Section("诊断详情"){SelectionContainer{Text(report,fontFamily=FontFamily.Monospace,style=MaterialTheme.typography.bodySmall)}}
    }
}
@Composable internal fun SettingsPage(p:DeviceProfile?,s:SessionView,manager:DeviceManager,permissions:()->Unit,wireless:()->Unit,backgroundAllowed:Boolean,requestBackground:()->Unit) {
    Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(16.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
        Section("当前会话") {Text("退到后台或锁屏后，保留当前手台连接和已开启的收听。离开前台会结束手机发话。",color=secondary)
            Text("切换设备会先结束原手台通联并释放控制权；其他板不维持连接。",color=secondary)
            if(p!=null)OutlinedButton(onClick={manager.disconnect(p.id)},enabled=!s.maintenance){Text("断开 ${p.displayName}")}}
        KeepAliveSection(manager,backgroundAllowed,requestBackground,permissions)
        Section("手机设置") {TextButton(onClick=permissions){Text("权限与通知设置")};TextButton(onClick=wireless){Text("Wi-Fi 与热点设置")}}
        Section("本机档案") {Text("设备备注、入口和入口凭据加密保存在本机。卸载或清除 App 数据会删除档案。",color=secondary)
            Text("公网入口的穿透、代理及认证由你自行维护。",color=secondary)}
        Section(Brand.APP) {Text("版本 ${BuildConfig.VERSION_NAME}");Text("${Brand.PROJECT} · ${Brand.DEVICE}配套应用",color=secondary)}
    }
}

@Composable internal fun KeepAliveSection(manager:DeviceManager,backgroundAllowed:Boolean,requestBackground:()->Unit,appSettings:()->Unit) {
    val context=LocalContext.current
    var persistent by remember{mutableStateOf(KeepAlive.persistent(context))}
    var autostart by remember{mutableStateOf(KeepAlive.autostartConfirmed(context))}
    var recents by remember{mutableStateOf(KeepAlive.recentsConfirmed(context))}
    val exempt=remember(backgroundAllowed){KeepAlive.batteryExempt(context) ?: backgroundAllowed}
    Section("后台保活") {
        Text("状态检查",color=blue,style=MaterialTheme.typography.titleSmall)
        KeepAliveRow("电池优化豁免",KeepAliveRules.batteryLabel(exempt),exempt,"去允许"){requestBackground()}
        Text("系统允许本应用忽略电池优化后，锁屏和后台才不容易被冻结。",color=secondary,style=MaterialTheme.typography.bodySmall)
        HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
        KeepAliveRow("自启动 / 后台活动",KeepAliveRules.confirmLabel(autostart),autostart,"去设置"){KeepAlive.openAutostart(context)}
        Text("Android 没有读取自启动开关的接口；打开厂商页面允许后，回来点下面的确认。",color=secondary,style=MaterialTheme.typography.bodySmall)
        if(!autostart)TextButton(onClick={KeepAlive.setAutostartConfirmed(context,true);autostart=true}){Text("标记自启动已允许")}
        HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
        KeepAliveRow("最近任务锁定",KeepAliveRules.confirmLabel(recents),recents,"打开应用信息"){KeepAlive.openAppDetails(context)}
        Text("在最近任务里把本应用卡片下拉或长按加锁，系统清理时更可能保留。",color=secondary,style=MaterialTheme.typography.bodySmall)
        if(!recents)TextButton(onClick={KeepAlive.setRecentsConfirmed(context,true);recents=true}){Text("标记最近任务已锁定")}
        HorizontalDivider(color=MaterialTheme.colorScheme.outlineVariant)
        Row(Modifier.fillMaxWidth().heightIn(min=56.dp),verticalAlignment=Alignment.CenterVertically) {
            Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(2.dp)) {
                Text("常驻前台服务",style=MaterialTheme.typography.bodyLarge)
                Text("锁屏或划掉任务后保持连接与收听；被系统回收时由 START_STICKY 重启服务并重连上一台手台。",
                    color=secondary,style=MaterialTheme.typography.bodySmall)
            }
            Switch(persistent,{on->persistent=on;manager.setPersistentForeground(on)})
        }
        TextButton(onClick=appSettings){Text("打开应用信息与权限")}
    }
}

@Composable private fun KeepAliveRow(title:String,status:String,ok:Boolean,action:String,click:()->Unit) {
    Row(Modifier.fillMaxWidth().heightIn(min=52.dp),verticalAlignment=Alignment.CenterVertically) {
        Box(Modifier.size(8.dp).background(if(ok)green else amber,CircleShape))
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(2.dp)) {
            Text(title,style=MaterialTheme.typography.bodyLarge)
            Text(status,color=if(ok)green else amber,style=MaterialTheme.typography.bodySmall)
        }
        TextButton(onClick=click){Text(action)}
    }
}

