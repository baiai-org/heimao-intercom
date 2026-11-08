@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import android.net.Uri
import androidx.activity.compose.BackHandler
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.*
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.selection.SelectionContainer
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material.icons.automirrored.filled.FormatListBulleted
import androidx.compose.material.icons.filled.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.unit.dp
import androidx.lifecycle.compose.collectAsStateWithLifecycle
import kotlinx.coroutines.launch

@Composable fun OrlScreen(manager:DeviceManager,connect:(String,String?)->Unit,discover:(Boolean)->Unit,provision:()->Unit,
    pttDown:(String)->Unit,wirelessSettings:()->Unit,appSettings:()->Unit,backgroundAllowed:Boolean,requestBackground:()->Unit) {
    val profiles by manager.store.profiles.collectAsStateWithLifecycle()
    val loaded by manager.store.loaded.collectAsStateWithLifecycle()
    val storeError by manager.store.error.collectAsStateWithLifecycle()
    val state by manager.view.collectAsStateWithLifecycle()
    val discovered by manager.discovered.collectAsStateWithLifecycle()
    val certificate by manager.certificates.collectAsStateWithLifecycle()
    val profile=profiles.firstOrNull{it.id==state.profileId}
    val preferences=LocalContext.current.getSharedPreferences("appearance",0)
    var appearance by remember{mutableStateOf(preferences.getString("theme","dark") ?: "dark")}
    var tab by rememberSaveable{mutableIntStateOf(0)}
    var detail by rememberSaveable{mutableStateOf<String?>(null)}
    var devicePicker by remember{mutableStateOf(false)}
    var editor by remember{mutableStateOf<Pair<DeviceProfile?,AccessPoint?>?>(null)}
    var newEditor by remember{mutableStateOf(false)}
    var endpoints by remember{mutableStateOf<DeviceProfile?>(null)}
    var rename by remember{mutableStateOf<DeviceProfile?>(null)}
    var confirm by remember{mutableStateOf<Pair<String,()->Unit>?>(null)}
    var discoveryEnabled by rememberSaveable{mutableStateOf(false)}
    var fileDevice by rememberSaveable{mutableStateOf("")}
    var firmwareFile by rememberSaveable{mutableStateOf(false)}
    var selectedFile by remember{mutableStateOf<Triple<String,Uri,Boolean>?>(null)}
    var pttNotice by remember(state.profileId){mutableStateOf<String?>(null)}
    var seenPttNotice by rememberSaveable(state.profileId){mutableLongStateOf(0L)}
    val snackbar=remember{SnackbarHostState()}
    val picker=rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()){uri->
        if(uri!=null && fileDevice.isNotBlank())selectedFile=Triple(fileDevice,uri,firmwareFile)
    }
    val exporter=rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/octet-stream")){uri->
        if(uri!=null && fileDevice.isNotBlank())manager.exportFirmware(fileDevice,uri)
    }
    val reportExporter=rememberLauncherForActivityResult(ActivityResultContracts.CreateDocument("application/json")){uri->
        if(uri!=null && fileDevice.isNotBlank())manager.exportDiagnostics(fileDevice,uri)
    }
    fun open(route:String){manager.pttUp(state.profileId);detail=route}
    fun select(p:DeviceProfile){devicePicker=false;detail=null;tab=0;if(state.profileId!=p.id || !state.connected)connect(p.id,null)}
    val ask:(String,()->Unit)->Unit={message,action->confirm=message to action}
    val file:(Boolean)->Unit={firmware->profile?.let{fileDevice=it.id;firmwareFile=firmware;picker.launch(arrayOf("*/*"))}}
    LaunchedEffect(manager){manager.messages.collect{snackbar.showSnackbar(it)}}
    LaunchedEffect(tab,detail){manager.pttUp(state.profileId);if((tab!=2 || detail!=null)&&discoveryEnabled){discoveryEnabled=false;discover(false)}}
    LaunchedEffect(state.profileId){detail=null}
    LaunchedEffect(state.pttNoticeId) {
        if(state.pttNoticeId!=0L && state.pttNoticeId!=seenPttNotice) {
            seenPttNotice=state.pttNoticeId;pttNotice=state.pttNotice
            manager.pttUp(state.profileId)
        }
    }
    LaunchedEffect(confirm,editor,newEditor,endpoints,rename,selectedFile,certificate,devicePicker){
        if(confirm!=null || editor!=null || newEditor || endpoints!=null || rename!=null || selectedFile!=null || certificate!=null || devicePicker)manager.pttUp(state.profileId)
    }
    BackHandler(detail!=null){detail=null}
    RadioTheme(appearance) {
        Scaffold(containerColor=background,snackbarHost={SnackbarHost(snackbar)},topBar={
            if(detail!=null)TopAppBar(title={Text(detail!!)},navigationIcon={IconButton(onClick={detail=null}){Icon(Icons.AutoMirrored.Filled.ArrowBack,"返回")}},colors=TopAppBarDefaults.topAppBarColors(containerColor=background))
            else PreviewHeader(choose={devicePicker=true},info={if(profile!=null)open("诊断与维护") else {tab=2;detail=null}})
        },bottomBar={Column {
            if(state.listening && (tab!=0 || detail!=null) && profile!=null)Surface(color=MaterialTheme.colorScheme.primaryContainer){
                Row(Modifier.fillMaxWidth().padding(start=20.dp,end=8.dp),verticalAlignment=Alignment.CenterVertically) {
                    Icon(Icons.Default.Headphones,null,tint=green);Spacer(Modifier.width(10.dp))
                    Text("${profile.displayName} · 收听中",modifier=Modifier.weight(1f),style=MaterialTheme.typography.bodyMedium)
                    TextButton(onClick={manager.toggleListening(profile.id)}){Text("停止")}
                }
            }
            PreviewNavigation(tab){detail=null;tab=it}
        }}){padding->Column(Modifier.fillMaxSize().padding(padding)) {
            if(storeError!=null)Text(storeError!!,color=red,modifier=Modifier.padding(16.dp))
            if(!loaded)Box(Modifier.fillMaxSize(),contentAlignment=Alignment.Center){CircularProgressIndicator()}
            else if(detail=="外观")Column(Modifier.verticalScroll(rememberScrollState()).padding(20.dp),verticalArrangement=Arrangement.spacedBy(16.dp)) {
                Section("显示偏好") {
                    listOf("dark" to "深色 · 专业电台","light" to "浅色","system" to "跟随系统").forEach{(key,label)->
                        Row(Modifier.fillMaxWidth().clickable{appearance=key;preferences.edit().putString("theme",key).apply()}.heightIn(min=56.dp),verticalAlignment=Alignment.CenterVertically){RadioButton(appearance==key,{appearance=key;preferences.edit().putString("theme",key).apply()});Text(label)}
                    }
                }
                Text("使用实色表面保持文字清楚，字号跟随 Android 系统设置。",color=secondary)
            }
            else if(detail=="APP 发话音量")MicrophonePage(manager,state)
            else if(detail=="连接与权限")SettingsPage(profile,state,manager,appSettings,wirelessSettings,backgroundAllowed,requestBackground)
            else if(detail!=null && profile==null)EmptySession(state){detail=null;tab=2}
            else if(detail=="诊断与维护" && profile!=null)Diagnostics(manager,profile,state,
                upload={file(true)},download={fileDevice=profile.id;exporter.launch("${Brand.PROJECT}-${profile.deviceId}.bin")},
                exportReport={fileDevice=profile.id;reportExporter.launch("${Brand.PROJECT}-${profile.deviceId}-诊断报告.json")},confirm=ask)
            else if(detail!=null && profile!=null)Column(Modifier.fillMaxSize().verticalScroll(rememberScrollState()).padding(20.dp),verticalArrangement=Arrangement.spacedBy(20.dp)) {
                when(detail){
                    "手台名称"->HandsetNameFields(manager,profile,state)
                    "射频参数"->RadioFields(manager,profile,state)
                    "音频与尾音"->AudioFields(manager,profile,state,file,ask)
                    "手台端 Wi-Fi"->WifiFields(manager,profile,state,wirelessSettings,ask)
                    "网络语音"->VoiceNetworkFields(manager,profile,state,ask)
                    "板载按键"->FrontKeys(manager,profile,state)
                }
            }
            else when(tab){
                0->if(profile==null)EmptySession(state){tab=2} else RadioHome(manager,profile,state,pttDown,::open,{tab=1},ask,{devicePicker=true})
                1->if(profile==null)EmptySession(state){tab=2} else ChannelPage(manager,profile,state,ask)
                2->DevicePage(profiles,state,discovered,discoveryEnabled,::select,{newEditor=true},{rename=it},{endpoints=it},
                    {p->ask("只移除手机中的“${p.displayName}”档案？手台自身配置保留。",{manager.remove(p.id)})},
                    {p->manager.scope.launch{manager.store.edit(p.id){it.copy(favorite=!it.favorite)}}},
                    {p->manager.scope.launch{manager.store.update{all->val next=all.toMutableList();val i=next.indexOfFirst{it.id==p.id};val prev=(i-1 downTo 0).firstOrNull{next[it].favorite==p.favorite};if(prev!=null){val old=next[prev];next[prev]=p;next[i]=old};next}}},
                    {discoveryEnabled=!discoveryEnabled;discover(discoveryEnabled)},provision,{manager.addDiscovered(it);tab=0},manager::disconnect)
                3->MorePage(profile,state,::open)
            }
        }}
        pttNotice?.let{message->AlertDialog(onDismissRequest={pttNotice=null},title={Text("发话被阻止")},text={Text(message)},
            confirmButton={TextButton(onClick={pttNotice=null}){Text("知道了")}},
            dismissButton={TextButton(onClick={pttNotice=null;open("网络语音")}){Text("查看工作模式")}})}
        if(devicePicker)ModalBottomSheet(sheetState=rememberModalBottomSheetState(skipPartiallyExpanded=true),onDismissRequest={devicePicker=false}) {
            Column(Modifier.fillMaxWidth().padding(bottom=WindowInsets.navigationBars.asPaddingValues().calculateBottomPadding()).verticalScroll(rememberScrollState()).padding(horizontal=24.dp).padding(bottom=32.dp),verticalArrangement=Arrangement.spacedBy(12.dp)) {
                Text("切换电台",style=MaterialTheme.typography.headlineSmall)
                Text("只连接当前使用的一台，切换会结束原手台通联。",color=secondary)
                profiles.sortedByDescending{it.favorite}.forEach{p->SettingRow(p.displayName,if(state.profileId==p.id)state.phase else "未连接",p.endpoint().address,!state.maintenance){select(p)}}
                OutlinedButton(onClick={devicePicker=false;tab=2;detail=null},modifier=Modifier.fillMaxWidth()){Text("管理设备")}
            }
        }
        if(newEditor || editor!=null)EndpointEditor(editor?.first,editor?.second,
            dismiss={newEditor=false;editor=null},save={old,entry,name,doConnect ->
                manager.scope.launch {
                    runCatching {
                        val id=old?.id ?: DeviceProfile(name=name,endpoints=listOf(entry)).id
                        if(old==null)manager.store.save(DeviceProfile(id=id,name=name,endpoints=listOf(entry)))
                        else manager.store.edit(id){p -> p.copy(endpoints=if(p.endpoints.any{it.id==entry.id})p.endpoints.map{if(it.id==entry.id)entry else it}else p.endpoints+entry)}
                        newEditor=false;editor=null
                        if(doConnect){connect(id,entry.id);tab=0}
                    }.onFailure{manager.message(it.message ?: "保存失败")}
                }
            })
        endpoints?.let { original ->
            val current=profiles.firstOrNull{it.id==original.id} ?: original
            AlertDialog(onDismissRequest={endpoints=null},title={Text("${current.displayName} · 访问入口")},text={Column(Modifier.verticalScroll(rememberScrollState())) {
                current.endpoints.forEach { entry ->
                    Text(entry.label,style=MaterialTheme.typography.titleSmall);Text(entry.address,color=secondary)
                    FlowRow {TextButton(onClick={endpoints=null;connect(current.id,entry.id);tab=0}){Text("切换到此入口")}
                        TextButton(onClick={endpoints=null;editor=current to entry}){Text("编辑")}
                        TextButton(onClick={
                            if(current.endpoints.size==1)manager.message("至少保留一个入口，或移除整个设备档案")
                            else if(state.profileId==current.id && current.selectedEndpoint==entry.id)manager.message("请先断开或切换入口")
                            else manager.scope.launch {manager.store.edit(current.id){it.copy(endpoints=it.endpoints.filterNot{e->e.id==entry.id})}}
                        }){Text("移除",color=red)}}
                }
            }},confirmButton={TextButton(onClick={endpoints=null;editor=current to null}){Text("新增入口")}},dismissButton={TextButton(onClick={endpoints=null}){Text("关闭")}})
        }
        rename?.let { p ->
            var name by remember(p.id){mutableStateOf(p.name)};var notes by remember(p.id){mutableStateOf(p.notes)}
            AlertDialog(onDismissRequest={rename=null},title={Text("本机备注")},text={Column {
                OutlinedTextField(name,{name=it},label={Text("本机备注名（仅此手机）")},modifier=Modifier.fillMaxWidth())
                OutlinedTextField(notes,{notes=it},label={Text("备注")},modifier=Modifier.fillMaxWidth(),minLines=3)
                Text("共享名称请在“更多 → 手台名称”中修改。",color=secondary)
                Text("设备身份：${p.deviceId.ifBlank{"连接后核对"}}",color=secondary)
            }},confirmButton={TextButton(onClick={manager.scope.launch{manager.store.edit(p.id){it.copy(name=name.ifBlank{it.name},notes=notes)}};rename=null}){Text("保存")}},dismissButton={TextButton(onClick={rename=null}){Text("取消")}})
        }
        confirm?.let { (text,action) -> AlertDialog(onDismissRequest={confirm=null},title={Text("确认操作")},text={Text(text)},confirmButton={TextButton(onClick={confirm=null;action()}){Text("确认")}},dismissButton={TextButton(onClick={confirm=null}){Text("取消")}}) }
        selectedFile?.let { selection ->
            val target=profiles.firstOrNull{it.id==selection.first}
            AlertDialog(onDismissRequest={selectedFile=null},title={Text(if(selection.third)"上传固件" else "上传尾音")},text={Text(
                "目标：${target?.displayName ?: "原选定设备"}\n${target?.endpoint()?.address ?: ""}\n"+
                    if(selection.third)"升级会停止通联并重启；手台端现有 OTA 流程会切到本地模式，网络模式需之后手动恢复。上传后读取完整运行镜像核对；完成前暂不能切换手台。" else "通联将停止，手台会转换并保存所选 WAV 或 MP3。文件最多 512 KB。"
            )},confirmButton={TextButton(onClick={selectedFile=null;manager.uploadFile(selection.first,selection.second,selection.third)}){Text("开始上传")}},dismissButton={TextButton(onClick={selectedFile=null}){Text("取消")}})
        }
        certificate?.let { question -> AlertDialog(onDismissRequest={manager.certificates.value=null},title={Text("确认设备证书")},text={SelectionContainer {Text(
            "入口：${question.host}\n请核对这是你要连接的手台或代理。证书指纹：\n${question.fingerprint.chunked(2).joinToString(":")}\n确认后，仅信任这张证书；证书变化会重新提示。"
        )}},confirmButton={TextButton(onClick={manager.acceptCertificate(question)}){Text("信任并重新连接")}},dismissButton={TextButton(onClick={manager.certificates.value=null;manager.disconnect(question.profileId)}){Text("取消连接")}}) }
    }
}
