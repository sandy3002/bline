package com.bline.nav

import android.Manifest
import android.content.Intent
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.PowerManager
import android.provider.Settings
import android.widget.Button
import android.widget.TextView
import androidx.appcompat.app.AppCompatActivity
import androidx.core.app.ActivityCompat
import androidx.core.app.NotificationManagerCompat

class MainActivity : AppCompatActivity() {
    private lateinit var statusText: TextView
    private lateinit var stateText: TextView
    private lateinit var toggleBtn: Button

    private val listener = NavStore.Listener { state ->
        runOnUiThread {
            stateText.text = state.toJson()
            refreshStatus()
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        setContentView(R.layout.activity_main)

        statusText = findViewById(R.id.statusText)
        stateText = findViewById(R.id.stateText)
        toggleBtn = findViewById(R.id.btnToggle)

        findViewById<Button>(R.id.btnNotifAccess).setOnClickListener {
            startActivity(Intent(Settings.ACTION_NOTIFICATION_LISTENER_SETTINGS))
        }
        findViewById<Button>(R.id.btnBattery).setOnClickListener {
            requestBatteryExemption()
        }
        toggleBtn.setOnClickListener { toggleService() }

        if (Build.VERSION.SDK_INT >= 33) {
            ActivityCompat.requestPermissions(
                this,
                arrayOf(Manifest.permission.POST_NOTIFICATIONS),
                1
            )
        }
    }

    override fun onResume() {
        super.onResume()
        NavStore.addListener(listener)
        refreshStatus()
    }

    override fun onPause() {
        NavStore.removeListener(listener)
        super.onPause()
    }

    private fun toggleService() {
        if (NavForegroundService.running) {
            NavForegroundService.stop(this)
            toggleBtn.postDelayed({ refreshStatus() }, 300)
        } else {
            NavForegroundService.start(this)
            toggleBtn.postDelayed({ refreshStatus() }, 300)
        }
    }

    private fun refreshStatus() {
        val notif = hasNotificationAccess()
        val batt = isIgnoringBattery()
        val svc = NavForegroundService.running
        statusText.text = buildString {
            append(if (svc) "Service running on :8080" else "Service stopped")
            append("\nNotification access: ").append(if (notif) "granted" else "missing")
            append("\nBattery optimization: ").append(if (batt) "ignored" else "active")
        }
        toggleBtn.text = if (svc) "Stop service" else "Start service"
    }

    private fun hasNotificationAccess(): Boolean {
        val enabled = NotificationManagerCompat.getEnabledListenerPackages(this)
        return enabled.contains(packageName)
    }

    private fun isIgnoringBattery(): Boolean {
        val pm = getSystemService(PowerManager::class.java)
        return pm.isIgnoringBatteryOptimizations(packageName)
    }

    private fun requestBatteryExemption() {
        val intent = Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS).apply {
            data = Uri.parse("package:$packageName")
        }
        try {
            startActivity(intent)
        } catch (_: Exception) {
            startActivity(Intent(Settings.ACTION_IGNORE_BATTERY_OPTIMIZATION_SETTINGS))
        }
    }
}
