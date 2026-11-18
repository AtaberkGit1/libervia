package com.libervia.app

import android.Manifest
import android.annotation.SuppressLint
import android.app.Activity
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.PackageManager
import android.graphics.Color
import android.net.Uri
import android.net.VpnService
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.view.HapticFeedbackConstants
import android.view.View
import android.view.WindowManager
import android.widget.Button
import android.widget.TextView

class MainActivity : Activity() {

    companion object {
        const val EXTRA_AUTO_ENGAGE = "auto_engage"
        private const val PREFS_NAME = "libervia_ui"
        private const val KEY_BATTERY_PROMPTED = "battery_prompted"
        private const val REQ_VPN_CONSENT = 100
        private const val REQ_NOTIF_PERM = 101
        private const val REQ_BATTERY_EXEMPT = 102
    }

    private lateinit var rootContainer: View
    private lateinit var tvStatus: TextView
    private lateinit var btnToggleVpn: Button
    private var isReceiverRegistered = false

    private val stateReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context?, intent: Intent?) {
            syncUiState()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        configureSystemBars()
        setContentView(R.layout.activity_main)

        rootContainer = findViewById(R.id.root_container)
        tvStatus = findViewById(R.id.tv_status)
        btnToggleVpn = findViewById(R.id.btn_toggle_vpn)

        applyWindowInsets()

        btnToggleVpn.setOnClickListener { view ->
            view.performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY)
            if (LiberViaVpnService.isRunning) {
                LiberViaVpnService.stopServiceIntent(this)
            } else {
                engageVpn()
            }
        }

        syncUiState()

        if (intent?.getBooleanExtra(EXTRA_AUTO_ENGAGE, false) == true && !LiberViaVpnService.isRunning) {
            engageVpn()
        }
    }

    override fun onStart() {
        super.onStart()
        if (!isReceiverRegistered) {
            val filter = IntentFilter(LiberViaVpnService.ACTION_STATE_CHANGED)
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
                registerReceiver(stateReceiver, filter, RECEIVER_NOT_EXPORTED)
            } else {
                @Suppress("DEPRECATION")
                registerReceiver(stateReceiver, filter)
            }
            isReceiverRegistered = true
        }
        syncUiState()
    }

    override fun onStop() {
        if (isReceiverRegistered) {
            try {
                unregisterReceiver(stateReceiver)
            } catch (_: Exception) {
            }
            isReceiverRegistered = false
        }
        super.onStop()
    }

    @Suppress("DEPRECATION")
    private fun configureSystemBars() {
        window.addFlags(WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS)
        window.statusBarColor = Color.BLACK
        window.navigationBarColor = Color.BLACK
    }

    private fun applyWindowInsets() {
        val baseLeft = rootContainer.paddingLeft
        val baseTop = rootContainer.paddingTop
        val baseRight = rootContainer.paddingRight
        val baseBottom = rootContainer.paddingBottom

        rootContainer.setOnApplyWindowInsetsListener { view, insets ->
            val leftInset: Int
            val topInset: Int
            val rightInset: Int
            val bottomInset: Int
            if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.R) {
                val sys = insets.getInsets(
                    android.view.WindowInsets.Type.systemBars() or
                        android.view.WindowInsets.Type.displayCutout()
                )
                leftInset = sys.left
                topInset = sys.top
                rightInset = sys.right
                bottomInset = sys.bottom
            } else {
                @Suppress("DEPRECATION")
                leftInset = insets.systemWindowInsetLeft
                @Suppress("DEPRECATION")
                topInset = insets.systemWindowInsetTop
                @Suppress("DEPRECATION")
                rightInset = insets.systemWindowInsetRight
                @Suppress("DEPRECATION")
                bottomInset = insets.systemWindowInsetBottom
            }
            view.setPadding(
                baseLeft + leftInset,
                baseTop + topInset,
                baseRight + rightInset,
                baseBottom + bottomInset
            )
            insets
        }
    }

    @SuppressLint("BatteryLife")
    private fun engageVpn() {
        requestNotificationPermissionIfNeeded()

        val pm = getSystemService(PowerManager::class.java)
        val ignoring = pm?.isIgnoringBatteryOptimizations(packageName) ?: true
        val prefs = getSharedPreferences(PREFS_NAME, MODE_PRIVATE)
        val alreadyPrompted = prefs.getBoolean(KEY_BATTERY_PROMPTED, false)

        if (!ignoring && !alreadyPrompted) {
            prefs.edit().putBoolean(KEY_BATTERY_PROMPTED, true).apply()
            try {
                val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
                    data = Uri.parse("package:$packageName")
                }
                @Suppress("DEPRECATION")
                startActivityForResult(intent, REQ_BATTERY_EXEMPT)
                return
            } catch (_: Exception) {
            }
        }

        proceedWithVpnConsentAndStart()
    }

    private fun proceedWithVpnConsentAndStart() {
        val prepareIntent = VpnService.prepare(this)
        if (prepareIntent != null) {
            @Suppress("DEPRECATION")
            startActivityForResult(prepareIntent, REQ_VPN_CONSENT)
        } else {
            LiberViaVpnService.startServiceIntent(this)
        }
    }

    private fun requestNotificationPermissionIfNeeded() {
        if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.TIRAMISU) {
            if (checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED) {
                requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), REQ_NOTIF_PERM)
            }
        }
    }

    @Deprecated("Pure Activity framework callback")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        when (requestCode) {
            REQ_BATTERY_EXEMPT -> proceedWithVpnConsentAndStart()
            REQ_VPN_CONSENT -> if (resultCode == RESULT_OK) {
                LiberViaVpnService.startServiceIntent(this)
            }
        }
    }

    private fun syncUiState() {
        val active = LiberViaVpnService.isRunning
        if (active) {
            tvStatus.text = getString(R.string.status_active)
            tvStatus.setTextColor(getColor(R.color.electric_cyan))
            tvStatus.setBackgroundResource(R.drawable.bg_btn_active)

            btnToggleVpn.text = getString(R.string.btn_disengage)
            btnToggleVpn.setTextColor(getColor(R.color.electric_cyan))
            btnToggleVpn.setBackgroundResource(R.drawable.bg_btn_disengage)
        } else {
            tvStatus.text = getString(R.string.status_idle)
            tvStatus.setTextColor(getColor(R.color.muted_gray))
            tvStatus.setBackgroundResource(R.drawable.bg_btn_idle)

            btnToggleVpn.text = getString(R.string.btn_engage)
            btnToggleVpn.setTextColor(getColor(R.color.pure_white))
            btnToggleVpn.setBackgroundResource(R.drawable.bg_btn_engage)
        }
    }
}
