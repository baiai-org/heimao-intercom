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

internal data class DenoiseSettings(val mode:Int,val levels:Int) {
    val paths get()=paths(mode)
    fun level(path:DenoisePath)=DenoiseLevel.entries[(levels shr path.shift) and 7]

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
            return DenoiseSettings(mode,levels)
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

internal class DenoiseWrite(val mode:Int,val changes:Map<DenoisePath,DenoiseDraft>) {
    init {
        require(changes.isNotEmpty() && DenoiseSettings.paths(mode).containsAll(changes.keys))
    }

    fun body():JSONObject {
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

    fun confirmed(settings:DenoiseSettings)=changes.all{(path,draft)->settings.level(path)==draft.requested}
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

internal suspend fun BoardApi.writeDenoise(write:DenoiseWrite):DenoiseSettings {
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
