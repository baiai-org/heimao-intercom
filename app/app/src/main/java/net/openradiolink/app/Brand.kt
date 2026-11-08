package net.openradiolink.app

/** Public names only. Protocol IDs, package name and encrypted storage stay stable. */
object Brand {
    const val PROJECT="黑猫互联"
    const val DEVICE="黑猫智能手台"
    const val PANEL="黑猫控制面板"
    const val APP="黑猫对讲"
    const val SETUP_PREFIX="$DEVICE-"
    const val LEGACY_SETUP_PREFIX="OpenRadioLink-"
    fun discoveryName(id:String)="$DEVICE · ${id.takeLast(6).uppercase()}"
}
