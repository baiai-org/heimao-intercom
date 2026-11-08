@file:OptIn(androidx.compose.material3.ExperimentalMaterial3Api::class,androidx.compose.foundation.layout.ExperimentalLayoutApi::class)
package net.openradiolink.app

import android.os.SystemClock
import androidx.compose.foundation.*
import androidx.compose.foundation.gestures.detectTapGestures
import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons

import androidx.compose.material.icons.outlined.*
import androidx.compose.material3.*
import androidx.compose.runtime.*
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

@Composable internal fun RadioHome(manager:DeviceManager,p:DeviceProfile,s:SessionView,pttDown:(String)->Unit,open:(String)->Unit,channels:()->Unit,confirm:(String,()->Unit)->Unit,chooseDevice:()->Unit) {
    val activity=RadioStateRules.activity(s.connected,s.ptt,s.status,s.activity.sample)
    // Debug APK verification: log after the composition reaches a frame, not
    // merely when a network callback arrives. Contains no voice or credentials.
    LaunchedEffect(activity,s.connected) {
        if(BuildConfig.DEBUG) {
            withFrameNanos { }
            android.util.Log.d("ORL-Activity","frame=$activity board_us=${s.activity.sample?.sampleUs ?: 0}")
        }
    }
    val active=activity!=RadioActivity.IDLE
    val cardFill=when(activity){RadioActivity.TRANSMIT->Color(0xFFB3263A);RadioActivity.RECEIVE->Color(0xFF176B48);else->panel}
    val cardInk=if(active)Color.White else MaterialTheme.colorScheme.onSurface
    val cardSecondary=if(active)Color.White.copy(alpha=.88f) else secondary
    val cardAccent=if(active)Color.White else blue
    BoxWithConstraints(Modifier.fillMaxSize()) {
        val short=maxHeight<660.dp || LocalDensity.current.fontScale>1.15f
        Column(Modifier.fillMaxSize().then(if(short)Modifier.verticalScroll(rememberScrollState()) else Modifier)) {
            Column((if(short)Modifier else Modifier.weight(1f).verticalScroll(rememberScrollState())).padding(horizontal=20.dp).padding(top=8.dp,bottom=4.dp),verticalArrangement=Arrangement.spacedBy(10.dp)) {
                Surface(onClick=chooseDevice,color=panel,shape=RoundedCornerShape(20.dp)) {
                    Row(Modifier.fillMaxWidth().heightIn(min=68.dp).padding(horizontal=14.dp,vertical=10.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(12.dp)) {
                        Icon(Icons.Outlined.Sensors,null,modifier=Modifier.size(20.dp))
                        Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(2.dp)) {
                            Text(p.displayName,fontSize=18.sp,lineHeight=24.sp,fontWeight=FontWeight.Medium)
                            val url=runCatching{EndpointAddress.parse(p.endpoint().address)}.getOrNull()
                            val address=url?.let{it.host+if(it.port==80 || it.port==443)"" else ":${it.port}"} ?: p.endpoint().address
                            Text("${p.deviceId.takeLast(6).uppercase()} · ${p.endpoint().label} · $address",style=MaterialTheme.typography.bodySmall,color=secondary)
                        }
                        Icon(Icons.Outlined.UnfoldMore,"切换电台",modifier=Modifier.size(18.dp),tint=secondary)
                    }
                }
                Row(Modifier.fillMaxWidth().heightIn(min=36.dp).clickable{manager.pttUp(p.id)},verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.SpaceBetween) {
                    Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(5.dp)) {
                        Icon(if(s.connected)Icons.Outlined.CheckCircle else Icons.Outlined.Schedule,null,tint=if(s.connected)green else amber,modifier=Modifier.size(16.dp))
                        Text(s.phase,color=if(s.connected)green else amber,fontSize=13.sp)
                    }
                    Text(if(s.connected)"已连接" else "未连接",fontSize=13.sp,color=blue)
                }
                Surface(onClick={open("射频参数")},shape=RoundedCornerShape(24.dp),color=cardFill,modifier=Modifier.semantics{stateDescription=when(activity){RadioActivity.TRANSMIT->"发射中";RadioActivity.RECEIVE->"接收中";else->"待机"}}) {
                    Column(Modifier.fillMaxWidth().padding(20.dp),verticalArrangement=Arrangement.spacedBy(10.dp)) {
                        Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.SpaceBetween) {
                            Row(verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                              Surface(color=if(active)Color.White.copy(alpha=.17f) else MaterialTheme.colorScheme.primaryContainer,shape=RoundedCornerShape(9.dp)){
                                Text(s.radio.value("band").ifBlank{"—"},fontSize=12.sp,lineHeight=18.sp,color=cardAccent,modifier=Modifier.padding(horizontal=9.dp,vertical=3.dp))
                              }
                              if(active)Text(if(activity==RadioActivity.TRANSMIT)"发射中" else "接收中",fontSize=12.sp,lineHeight=18.sp,color=cardInk)
                            }
                            Text("MHz",fontSize=13.sp,lineHeight=20.sp,color=cardSecondary)
                        }
                        PreviewFrequency("接收",s.status.value("freq.rx"),cardInk,cardSecondary)
                        PreviewFrequency("发射",s.status.value("freq.tx"),cardInk,cardSecondary)
                        Column(verticalArrangement=Arrangement.spacedBy(12.dp)) {
                            HorizontalDivider(color=if(active)Color.White.copy(alpha=.25f) else MaterialTheme.colorScheme.outlineVariant)
                            FlowRow(horizontalArrangement=Arrangement.spacedBy(14.dp),verticalArrangement=Arrangement.spacedBy(4.dp)) {
                                Text(when(s.status.value("freq.bw")){"0"->"12.5 kHz";"1"->"25 kHz";else->"带宽 —"},fontSize=13.sp,lineHeight=20.sp,color=cardSecondary)
                                Text("静噪 ${s.status.value("freq.sq").ifBlank{"—"}}",fontSize=13.sp,lineHeight=20.sp,color=cardSecondary)
                                Text(when(s.status.value("freq.power")){"0"->"低功率";"1"->"高功率";else->"功率 —"},fontSize=13.sp,lineHeight=20.sp,color=cardSecondary)
                            }
                        }
                        Row(Modifier.heightIn(min=40.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(5.dp)) {
                            Text("编辑射频参数",fontSize=14.sp,color=cardAccent)
                            Icon(Icons.Outlined.NorthEast,null,tint=cardAccent,modifier=Modifier.size(16.dp))
                        }
                    }
                }
                Row(horizontalArrangement=Arrangement.spacedBy(10.dp)) {
                    PreviewShortcut("工作模式",modes.firstOrNull{it.first==s.status.value("intercom.mode")}?.second ?: "等待更新",Modifier.weight(1f)){open("网络语音")}
                    PreviewShortcut("快捷信道","${s.channels.length()} 个已保存",Modifier.weight(1f),channels)
                }
                if(s.error.isNotBlank())Text(s.error,color=red,style=MaterialTheme.typography.bodySmall)
                if(s.maintenance)Column(verticalArrangement=Arrangement.spacedBy(8.dp)){
                    Text("维护中 · 请等待完成",color=amber)
                    if(s.progress>=0)LinearProgressIndicator(progress={s.progress/100f},modifier=Modifier.fillMaxWidth())else LinearProgressIndicator(Modifier.fillMaxWidth())
                }
            }
            PreviewPttDock(manager,p,s,pttDown)
        }
    }
}

@Composable private fun PreviewFrequency(label:String,value:String,tint:Color,labelTint:Color) {
    val large=LocalDensity.current.fontScale>1.35f
    if(large)Column(verticalArrangement=Arrangement.spacedBy(3.dp)){Text(label,fontSize=13.sp,color=labelTint);PreviewDigits(value,tint)}
    else Row(Modifier.fillMaxWidth(),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(10.dp)) {
        Text(label,fontSize=13.sp,lineHeight=20.sp,color=labelTint)
        Box(Modifier.weight(1f),contentAlignment=Alignment.CenterEnd){PreviewDigits(value,tint)}
    }
}

@Composable private fun PreviewDigits(value:String,tint:Color) {
    BoxWithConstraints(Modifier.fillMaxWidth(),contentAlignment=Alignment.CenterEnd) {
        val size=minOf(32f,maxWidth.value/(8f*.58f)/LocalDensity.current.fontScale)
        Text(RadioUiRules.displayFrequency(value),fontSize=size.sp,lineHeight=(size*1.18f).sp,letterSpacing=(-1).sp,fontFamily=FontFamily.SansSerif,fontWeight=FontWeight.Medium,color=tint,maxLines=1,style=MaterialTheme.typography.bodyLarge.copy(fontFeatureSettings="tnum"))
    }
}

@Composable private fun PreviewShortcut(title:String,value:String,modifier:Modifier,click:()->Unit) {
    Surface(onClick=click,shape=RoundedCornerShape(16.dp),color=panel,modifier=modifier) {
        Column(Modifier.fillMaxWidth().heightIn(min=78.dp).padding(12.dp),verticalArrangement=Arrangement.Center) {
            Text(title,fontSize=14.sp,lineHeight=21.sp,fontWeight=FontWeight.Medium)
            Text(value,fontSize=12.sp,lineHeight=18.sp,color=secondary)
        }
    }
}

@Composable private fun PreviewPttDock(manager:DeviceManager,p:DeviceProfile,s:SessionView,pttDown:(String)->Unit) {
    var pressed by remember(p.id){mutableStateOf(false)}
    var elapsed by remember(p.id){mutableLongStateOf(0)}
    val enabled=s.connected&&s.listening&&s.mine&&!s.maintenance
    LaunchedEffect(s.ptt){elapsed=0;if(s.ptt){val start=SystemClock.elapsedRealtime();while(true){elapsed=(SystemClock.elapsedRealtime()-start)/1000;delay(250)}}}
    DisposableEffect(p.id){onDispose{manager.pttUp(p.id)}}
    val reason=when{!s.connected->"先连接电台";s.maintenance->"维护中";!s.listening->"先开启收听";!s.mine->if(s.control.optBoolean("held"))"控制权由其他端持有" else "正在取得控制权…";else->s.pttReason.ifBlank{"松开即结束"}}
    val networkSummary=RadioStateRules.networkSummary(s.activity.live(s.live),s.status)
    val fill=if(s.ptt)red else if(pressed||s.pttPending)amber else if(enabled)blue else panel
    val ink=if(s.ptt || pressed || s.pttPending){if(LocalRadioDark.current)Color(0xFF29070B) else Color.White} else if(enabled)MaterialTheme.colorScheme.onPrimary else secondary
    Column(Modifier.fillMaxWidth().padding(horizontal=20.dp).padding(top=4.dp,bottom=10.dp),verticalArrangement=Arrangement.spacedBy(14.dp)) {
        Surface(color=panel,shape=RoundedCornerShape(20.dp),modifier=Modifier.fillMaxWidth().toggleable(s.listening,enabled=s.connected&&!s.maintenance,role=Role.Switch,onValueChange={manager.toggleListening(p.id)}).semantics{contentDescription="手机收听"}) {
            Row(Modifier.fillMaxWidth().heightIn(min=76.dp).padding(horizontal=16.dp,vertical=12.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(13.dp)) {
                Icon(Icons.Outlined.Headphones,null,tint=if(s.listening)green else secondary,modifier=Modifier.size(20.dp))
                Column(Modifier.weight(1f),verticalArrangement=Arrangement.spacedBy(1.dp)) {
                    val overload=s.ptt&&(s.boardTxClipped()||s.microphoneLevel.clipped>0)
                    val meterLabel=if(s.boardTxClipped())"手台输出过载" else if(s.microphoneLevel.clipped>0)"麦克风过载" else "发话电平 · ${s.microphoneLevel.label}"
                    Text(if(s.ptt)meterLabel else if(s.listening)"正在收听" else "收听关闭",fontSize=16.sp,lineHeight=23.sp,
                        maxLines=1,overflow=androidx.compose.ui.text.style.TextOverflow.Ellipsis,
                        color=if(overload)red else if(s.listening)green else MaterialTheme.colorScheme.onSurface)
                    // Reuse the existing subtitle height: PTT never moves under the finger.
                    if(s.ptt)Row(Modifier.fillMaxWidth().height(19.dp),verticalAlignment=Alignment.CenterVertically,horizontalArrangement=Arrangement.spacedBy(8.dp)) {
                        LinearProgressIndicator(progress={s.microphoneLevel.fraction},modifier=Modifier.weight(1f).height(3.dp),color=if(overload)red else green)
                        Text("+${s.microphoneLevel.gainDb} dB",fontSize=12.sp,color=secondary)
                    } else Text(if(s.listening)"声音在手机播放" else "轻点开始收听",fontSize=13.sp,lineHeight=19.sp,color=secondary)
                }
                Box(Modifier.size(47.dp,28.dp).background(if(s.listening)green else MaterialTheme.colorScheme.outlineVariant,RoundedCornerShape(20.dp)).padding(3.dp),contentAlignment=if(s.listening)Alignment.CenterEnd else Alignment.CenterStart){Box(Modifier.size(22.dp).background(panel,RoundedCornerShape(50)))}
            }
        }
        Surface(color=fill,shape=RoundedCornerShape(24.dp),modifier=Modifier.fillMaxWidth().heightIn(min=96.dp)
            .semantics(mergeDescendants=true){role=Role.Button;contentDescription="按住发话，松开结束";stateDescription=if(s.ptt)"发话中" else reason;if(!enabled)disabled()}
            .pointerInput(p.id,enabled){detectTapGestures(onPress={
                if(!enabled)return@detectTapGestures
                pressed=true;pttDown(p.id)
                try{tryAwaitRelease()}finally{pressed=false;manager.pttUp(p.id)}
            })}) {
            Row(Modifier.padding(16.dp),horizontalArrangement=Arrangement.Center,verticalAlignment=Alignment.CenterVertically) {
                Icon(Icons.Outlined.Mic,null,tint=ink,modifier=Modifier.size(20.dp));Spacer(Modifier.width(14.dp))
                Column {
                    Text(if(s.ptt&&!pressed)"正在结束发话" else if(s.ptt)"发话中 · 松开结束" else if(pressed||s.pttPending)"正在申请发话" else if(enabled)"按住发话" else reason,fontSize=20.sp,lineHeight=26.sp,fontWeight=FontWeight.Medium,color=ink)
                    Text(if(s.ptt)"${elapsed/60}:${(elapsed%60).toString().padStart(2,'0')}" else networkSummary ?: if(enabled)"松开即结束" else if(!s.listening)"开启后可使用按住发话" else reason,fontSize=12.sp,lineHeight=18.sp,color=ink)
                }
            }
        }
    }
}
