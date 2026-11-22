package net.openradiolink.app

import kotlinx.coroutines.CancellationException
import org.json.JSONObject
import java.io.IOException

internal enum class DenoiseLevel(val label:String) {
    OFF("关闭"), LIGHT("轻度"), STANDARD("标准"), STRONG("强力"), EXTREME("极强")
}

internal data class DenoisePath(val setting:Int,val label:String) {
    val shift get()=(setting-1)*3
}

internal data class DenoiseSettings(
    val mode:Int,val levels:Int,val selectionLevel:DenoiseLevel?=null,
    val selectionPaths:Int?=null,val configRevision:Long?=null,
) {
    val paths get()=paths(mode)
    val localPaths get()=local
    val networkPaths get()=paths.filterNot(local::contains)
    fun level(path:DenoisePath)=DenoiseLevel.entries[(levels shr path.shift) and 7]
    val selection get()=selectionLevel?.let { strength ->
        DenoiseSelection(strength,selectionPaths ?: allPaths.fold(0){mask,path->
            if(level(path)!=DenoiseLevel.OFF)mask or (1 shl path.setting) else mask
        })
    }

    fun uniformLevel(group: List<DenoisePath>): DenoiseLevel? {
        val first = group.firstOrNull()?.let(::level) ?: return null
        return first.takeIf { group.all { level(it) == first } }
    }

    fun preset(group: List<DenoisePath>, level: DenoiseLevel): DenoiseWrite =
        DenoiseWrite(mode, group.associateWith { DenoiseDraft(this.level(it), level) })

    companion object {
        private val handset=DenoisePath(4,"手台发话 → 本机射频")
        private val speaker=DenoisePath(3,"射频来话 → 本机喇叭")
        private val local=listOf(handset,speaker)
        private fun network(mode:Int,first:Int)=listOf(
            handset,DenoisePath(first,"手台发话 → 网络"),speaker,
            DenoisePath(mode,"射频来话 → 网络转发"),
            DenoisePath(first+1,"网络来话 → 本机喇叭"),
            DenoisePath(first+2,"网络来话 → 模拟桥接"),
        )
        private val byMode=listOf(local,network(1,5),network(2,8))
        val selectionGroups=listOf(
            "本机收发 · 各模式共用" to local,
            "公网直连" to byMode[1].filterNot(local::contains),
            "MQTT 中继" to byMode[2].filterNot(local::contains),
        )
        val allPaths=selectionGroups.flatMap{it.second}
        fun paths(mode:Int)=byMode[mode]

        fun parse(body:JSONObject,expectedDevice:String):DenoiseSettings {
            if(expectedDevice.isBlank() || body.optString("device_id")!=expectedDevice) {
                throw IOException("降噪响应的设备身份不匹配，请重新连接")
            }
            if(!body.optBoolean("ok")) throw IOException("手台未返回可用的降噪设置")
            if(!body.has("levels")) throw DenoiseUnsupported()
            val mode=exactInt(body,"mode",2)
            val levels=exactInt(body,"levels",0x3fffffff)
            if((0..9).any{((levels shr (it*3)) and 7)>DenoiseLevel.EXTREME.ordinal}) {
                throw IOException("手台降噪档位无效")
            }
            if(body.has("config_revision") || body.has("selected_level") || body.has("selected_paths")) {
                val selected=DenoiseLevel.entries[exactInt(body,"selected_level",4)]
                val paths=exactInt(body,"selected_paths",0x7fe)
                val revision=(body.opt("config_revision") as? Number)?.toDouble()
                if(paths and 1!=0 || revision==null || revision !in 0.0..4294967295.0 ||
                    revision.toLong().toDouble()!=revision ||
                    (selected==DenoiseLevel.OFF && levels!=0)) {
                    throw IOException("手台降噪选择无效")
                }
                return DenoiseSettings(mode,levels,selected,paths,revision.toLong())
            }
            val selection=if(body.has("selection_level")) {
                DenoiseLevel.entries[exactInt(body,"selection_level",4)].also {
                    if(it==DenoiseLevel.OFF)throw IOException("手台降噪档位无效")
                }
            } else null
            return DenoiseSettings(mode,levels,selection)
        }

        private fun exactInt(body:JSONObject,key:String,max:Int):Int {
            val number=(body.opt(key) as? Number)?.toDouble()
            val value=number?.toInt()
            if(value==null || value !in 0..max || value.toDouble()!=number) {
                throw IOException("手台降噪设置格式无效")
            }
            return value
        }
    }
}

internal class DenoiseUnsupported:IOException("五档降噪需要 r11-106 或更新的配套固件")
internal data class DenoiseDraft(val expected:DenoiseLevel,val requested:DenoiseLevel)

internal sealed interface DenoiseUpdate {
    fun body():JSONObject
    fun confirmed(settings:DenoiseSettings):Boolean
}

internal data class DenoiseSelection(val level:DenoiseLevel,val paths:Int) {
    fun includes(path:DenoisePath)=paths and (1 shl path.setting)!=0

    fun toggle(path:DenoisePath)=copy(paths=paths xor (1 shl path.setting))

    fun matches(settings:DenoiseSettings):Boolean =
        settings.selection==this && DenoiseSettings.allPaths.all { path ->
            settings.level(path)==if(includes(path))level else DenoiseLevel.OFF
        }
}

internal data class DenoiseSelectionWrite(
    val selection:DenoiseSelection,
    val expected:DenoiseSettings,
):DenoiseUpdate {
    override fun body():JSONObject =
        expected.configRevision?.let { revision ->
            JSONObject().put("selected_level",selection.level.ordinal)
                .put("selected_paths",selection.paths).put("config_revision",revision)
        } ?: JSONObject().put("selection_level",selection.level.ordinal)
            .put("paths",selection.paths).put("expected_levels",expected.levels)
            .put("expected_selection_level",checkNotNull(expected.selectionLevel).ordinal)

    override fun confirmed(settings:DenoiseSettings)=selection.matches(settings)
}

internal class DenoiseWrite(val mode:Int,val changes:Map<DenoisePath,DenoiseDraft>):DenoiseUpdate {
    init {
        require(changes.isNotEmpty() && DenoiseSettings.paths(mode).containsAll(changes.keys))
    }

    override fun body():JSONObject {
        var mask=0
        var expected=0
        var levels=0
        changes.forEach { (path,draft) ->
            mask=mask or (1 shl path.setting)
            expected=expected or (draft.expected.ordinal shl path.shift)
            levels=levels or (draft.requested.ordinal shl path.shift)
        }
        return JSONObject().put("mode",mode).put("mask",mask)
            .put("expected_levels",expected).put("levels",levels)
    }

    override fun confirmed(settings:DenoiseSettings)=changes.all{(path,draft)->settings.level(path)==draft.requested}
}

internal suspend fun BoardApi.readDenoise():DenoiseSettings {
    val boundDevice=deviceId
    check(boundDevice.isNotBlank()){ "当前手台尚未核对身份" }
    val body=try {
        json("api/rf_denoise")
    } catch(e:ApiFailure) {
        if(e.status==404)throw DenoiseUnsupported()
        throw e
    }
    return DenoiseSettings.parse(body,boundDevice)
}

internal suspend fun BoardApi.writeDenoise(write:DenoiseUpdate):DenoiseSettings {
    val boundDevice=deviceId
    check(boundDevice.isNotBlank()){ "当前手台尚未核对身份" }
    val saved=try {
        DenoiseSettings.parse(json("api/rf_denoise",write.body()),boundDevice)
    } catch(e:CancellationException) {
        throw e
    } catch(e:CertificateFailure) {
        throw e
    } catch(e:IOException) {
        null
    } catch(e:org.json.JSONException) {
        null
    }
    if(saved!=null && write.confirmed(saved)) return saved
    // A lost response can follow a successful write. Read back once and
    // never replay the mutation. Keep the same device binding for both calls.
    check(deviceId==boundDevice){ "设备身份已变化，请重新连接" }
    return readDenoise()
}
