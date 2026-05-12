package com.odudex.kern

import android.app.Activity
import android.os.Bundle
import android.view.MotionEvent
import android.view.Surface
import android.view.SurfaceHolder
import android.view.SurfaceView
import android.view.View
import android.view.Window
import android.view.WindowInsets
import android.view.WindowInsetsController

class MainActivity : Activity() {
    private lateinit var kernView: KernSurfaceView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        requestWindowFeature(Window.FEATURE_NO_TITLE)
        hideSystemUi()
        kernView = KernSurfaceView(this)
        setContentView(kernView)
    }

    override fun onResume() {
        super.onResume()
        hideSystemUi()
        KernNative.resume()
    }

    override fun onPause() {
        KernNative.pause()
        super.onPause()
    }

    override fun onDestroy() {
        kernView.shutdown()
        super.onDestroy()
    }

    private fun hideSystemUi() {
        window.decorView.systemUiVisibility =
            View.SYSTEM_UI_FLAG_FULLSCREEN or
                View.SYSTEM_UI_FLAG_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_IMMERSIVE_STICKY or
                View.SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN or
                View.SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION or
                View.SYSTEM_UI_FLAG_LAYOUT_STABLE
        if (android.os.Build.VERSION.SDK_INT >= 30) {
            window.insetsController?.let {
                it.hide(WindowInsets.Type.statusBars() or WindowInsets.Type.navigationBars())
                it.systemBarsBehavior =
                    WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE
            }
        }
    }
}

private class KernSurfaceView(context: Activity) : SurfaceView(context), SurfaceHolder.Callback {
    private companion object {
        const val BoardProfile = "wave_5"
    }

    private val filesPath = context.filesDir.absolutePath
    private var created = false

    init {
        holder.addCallback(this)
        isFocusable = true
        isFocusableInTouchMode = true
        keepScreenOn = true
    }

    override fun surfaceCreated(holder: SurfaceHolder) {
        startOrResize(holder.surface, width, height)
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        startOrResize(holder.surface, width, height)
    }

    override fun surfaceDestroyed(holder: SurfaceHolder) {
        KernNative.pause()
    }

    override fun onTouchEvent(event: MotionEvent): Boolean {
        val pointerIndex = event.actionIndex
        val action = when (event.actionMasked) {
            MotionEvent.ACTION_DOWN, MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_MOVE -> 0
            MotionEvent.ACTION_UP, MotionEvent.ACTION_POINTER_UP, MotionEvent.ACTION_CANCEL -> 1
            else -> return true
        }
        val x = event.getX(pointerIndex)
        val y = event.getY(pointerIndex)
        KernNative.touch(action, x, y)
        return true
    }

    fun shutdown() {
        KernNative.destroy()
        created = false
    }

    private fun startOrResize(surface: Surface, width: Int, height: Int) {
        if (width <= 0 || height <= 0) return
        if (!created) {
            KernNative.create(surface, width, height, filesPath, BoardProfile)
            created = true
        } else {
            KernNative.resize(surface, width, height)
        }
    }
}

private object KernNative {
    init {
        System.loadLibrary("kern_android")
    }

    external fun create(surface: Surface, width: Int, height: Int, filesDir: String, board: String)
    external fun resize(surface: Surface, width: Int, height: Int)
    external fun touch(action: Int, x: Float, y: Float)
    external fun pause()
    external fun resume()
    external fun destroy()
}
