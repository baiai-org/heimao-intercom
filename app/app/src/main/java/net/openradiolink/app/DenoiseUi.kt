@file:OptIn(androidx.compose.foundation.layout.ExperimentalLayoutApi::class)

package net.openradiolink.app

import androidx.compose.foundation.layout.*
import androidx.compose.foundation.selection.toggleable
import androidx.compose.material3.*
import androidx.compose.runtime.*
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.semantics.Role
import androidx.compose.ui.unit.dp
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

@Composable
internal fun DenoiseEditor(manager: DeviceManager, profile: DeviceProfile, state: SessionView) {
    val scope = rememberCoroutineScope()
    val device = state.deviceId
    val connected = state.profileId == profile.id && device.isNotBlank() && state.connected && !state.maintenance
    val currentMode = RadioStateRules.mode(state.activity.live(state.live), state.status)

    var saved by remember(profile.id, device) { mutableStateOf<DenoiseSettings?>(null) }
    val drafts = remember(profile.id, device) { mutableStateMapOf<DenoisePath, DenoiseDraft>() }
    var selectionDraft by remember(profile.id, device) { mutableStateOf<DenoiseSelectionWrite?>(null) }
    var advanced by remember(profile.id, device) { mutableStateOf(false) }
    var busy by remember(profile.id, device) { mutableStateOf(false) }
    var reading by remember(profile.id, device) { mutableStateOf(false) }
    var unsupported by remember(profile.id, device) { mutableStateOf(false) }
    var epoch by remember(profile.id, device) { mutableLongStateOf(0L) }
    var loadError by remember(profile.id, device) { mutableStateOf("") }
    var feedback by remember(profile.id, device) { mutableStateOf("") }
    var saveFailed by remember(profile.id, device) { mutableStateOf(false) }

    fun read() {
        if (!connected || busy || reading || unsupported) return
        reading = true
        val started = epoch
        val job = manager.action(profile.id) {
            try {
                val result = api.readDenoise()
                withContext(Dispatchers.Main) {
                    if (started == epoch && api.deviceId == device) {
                        saved = result
                        loadError = ""
                    }
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                withContext(Dispatchers.Main) {
                    if (started == epoch) {
                        if (error is DenoiseUnsupported) {
                            saved = null
                            unsupported = true
                        }
                        loadError = error.message ?: "读取降噪设置失败"
                    }
                }
                if (error is CertificateFailure) throw error
            }
        }
        if (job == null) {
            reading = false
            loadError = "手台当前不可操作，请重新读取降噪设置"
        } else {
            job.invokeOnCompletion { scope.launch { reading = false } }
        }
    }

    fun save(write: DenoiseUpdate) {
        if (busy) return
        busy = true
        ++epoch
        feedback = ""
        saveFailed = false
        fun retainDraft() {
            when (write) {
                is DenoiseWrite -> drafts.putAll(write.changes)
                is DenoiseSelectionWrite -> selectionDraft = write
            }
        }
        val job = manager.action(profile.id) {
            try {
                check(api.deviceId == device) { "设备身份已变化，请重新连接" }
                val result = api.writeDenoise(write)
                withContext(Dispatchers.Main) {
                    saved = result
                    loadError = ""
                    val confirmed = write.confirmed(result)
                    when (write) {
                        is DenoiseWrite -> write.changes.forEach { (path, draft) ->
                            val latest = result.level(path)
                            if (latest == draft.requested) drafts.remove(path)
                            else drafts[path] = draft.copy(expected = latest)
                        }
                        is DenoiseSelectionWrite -> {
                            selectionDraft = if (confirmed) null
                                else if (result.selection != null) write.copy(expected = result) else write
                        }
                    }
                    saveFailed = !confirmed
                    feedback = if (confirmed) "已保存，下一话段生效"
                        else "保存未确认，已读取手台当前档位，请核对后重试"
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                withContext(Dispatchers.Main) {
                    retainDraft()
                    saveFailed = true
                    feedback = error.message ?: "保存未确认，请重试"
                }
                if (error is CertificateFailure) throw error
            }
        }
        if (job == null) {
            busy = false
            retainDraft()
            saveFailed = true
            feedback = "手台当前不可操作，请重试"
        } else {
            job.invokeOnCompletion { scope.launch { busy = false } }
        }
    }

    LaunchedEffect(connected, device, currentMode) {
        if (connected) {
            while (true) {
                read()
                delay(2500)
            }
        }
    }

    Text("手台语音降噪", style = MaterialTheme.typography.titleSmall)
    val current = saved
    val selection = current?.selection
    if (current != null && selection != null) {
        val editable = connected && !busy
        val shown = selectionDraft?.selection ?: selection
        val mixed = !selection.matches(current)
        fun edit(value: DenoiseSelection) {
            selectionDraft = if (value.matches(current)) null
                else DenoiseSelectionWrite(value, selectionDraft?.expected ?: current)
            feedback = ""
        }
        DenoiseLevelRow("降噪档位", shown.level, editable, DenoiseLevel.entries.drop(1)) {
            edit(shown.copy(level = it))
        }
        if (mixed) Text("手台保存了不同的线路档位。保存后，已勾选的线路会统一使用所选档位。",
            color = amber, style = MaterialTheme.typography.bodySmall)
        for ((title, group) in DenoiseSettings.selectionGroups) {
            Text(title, style = MaterialTheme.typography.labelMedium, color = secondary)
            for (path in group) {
                Row(Modifier.fillMaxWidth().heightIn(min = 48.dp)
                    .toggleable(value = shown.includes(path), enabled = editable, role = Role.Checkbox,
                        onValueChange = { edit(shown.toggle(path)) }),
                    verticalAlignment = Alignment.CenterVertically) {
                    Checkbox(checked = shown.includes(path), onCheckedChange = null, enabled = editable,
                        modifier = Modifier.padding(end = 8.dp).size(36.dp))
                    Text(path.label)
                }
            }
        }
        if (shown.paths != 0 && shown.level == DenoiseLevel.EXTREME) {
            Text("极强档可能削弱弱话音和尾音。", color = amber, style = MaterialTheme.typography.bodySmall)
        }
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (feedback.isNotBlank()) Text(feedback, color = if (saveFailed) red else secondary)
        if (selectionDraft != null && feedback.isBlank()) {
            Text("有未保存的线路设置", color = amber, style = MaterialTheme.typography.bodySmall)
        }
        FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
            Button(onClick = { save(selectionDraft ?: DenoiseSelectionWrite(selection, current)) },
                enabled = editable && (selectionDraft != null || mixed)) { Text("保存降噪设置") }
            TextButton(onClick = { selectionDraft = null; feedback = "" },
                enabled = editable && selectionDraft != null) { Text("撤销修改") }
        }
    } else if (current != null) {
        val editable = connected && !busy && (currentMode == null || current.mode == currentMode)
        val local = current.localPaths
        val localLevel = current.uniformLevel(local)
        DenoiseLevelRow("本机收发 · 各模式共用", localLevel, editable) { level ->
            if (level != localLevel) save(current.preset(local, level))
        }
        val network = current.networkPaths
        if (network.isNotEmpty()) {
            val networkLevel = current.uniformLevel(network)
            DenoiseLevelRow(RadioStateRules.modeName(current.mode), networkLevel, editable) { level ->
                if (level != networkLevel) save(current.preset(network, level))
            }
        }

        val changes = current.paths.mapNotNull { path -> drafts[path]?.let { path to it } }.toMap()
        if (busy) LinearProgressIndicator(Modifier.fillMaxWidth())
        if (feedback.isNotBlank()) Text(feedback, color = if (saveFailed) red else secondary)
        if (changes.isNotEmpty()) Text("有未保存的线路设置", color = amber, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { advanced = !advanced }) {
            Text(if (advanced) "收起线路设置" else "按线路调整")
        }
        if (advanced) {
            for (path in current.paths) {
                val stored = current.level(path)
                val draft = drafts[path]
                DenoiseLevelRow(path.label, draft?.requested ?: stored, editable) { level ->
                    if (level == stored) drafts.remove(path)
                    else drafts[path] = DenoiseDraft(draft?.expected ?: stored, level)
                    feedback = ""
                }
            }
            if (current.paths.any { (drafts[it]?.requested ?: current.level(it)) == DenoiseLevel.EXTREME }) {
                Text("极强档可能削弱弱话音和尾音。", color = amber, style = MaterialTheme.typography.bodySmall)
            }
            if (changes.isNotEmpty()) {
                FlowRow(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                    Button(onClick = { save(DenoiseWrite(current.mode, changes)) }, enabled = editable) {
                        Text("保存线路设置")
                    }
                    TextButton(onClick = {
                        changes.keys.forEach { drafts.remove(it) }
                        feedback = ""
                    }, enabled = editable) { Text("撤销修改") }
                }
            }
        }
    } else if (connected && loadError.isBlank()) {
        Text("正在读取降噪设置…", color = secondary)
    }
    if (loadError.isNotBlank()) {
        Text(loadError, color = red, style = MaterialTheme.typography.bodySmall)
        TextButton(onClick = { unsupported = false; read() }, enabled = connected && !busy && !reading) {
            Text("重新读取")
        }
    }
}

@Composable
private fun DenoiseLevelRow(
    title: String,
    level: DenoiseLevel?,
    enabled: Boolean,
    choices: List<DenoiseLevel> = DenoiseLevel.entries,
    change: (DenoiseLevel) -> Unit,
) {
    var expanded by remember(title) { mutableStateOf(false) }
    LaunchedEffect(enabled) { if (!enabled) expanded = false }
    Row(
        Modifier.fillMaxWidth(),
        horizontalArrangement = Arrangement.spacedBy(10.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(title, Modifier.weight(1f))
        Box {
            OutlinedButton(onClick = { expanded = true }, enabled = enabled) {
                Text(level?.label ?: "分线路")
            }
            DropdownMenu(expanded = expanded && enabled, onDismissRequest = { expanded = false }) {
                choices.forEach { choice ->
                    DropdownMenuItem(text = { Text(choice.label) }, onClick = { expanded = false; change(choice) })
                }
            }
        }
    }
}
