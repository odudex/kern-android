package com.odudex.kern

import android.view.Surface

internal object KernNative {
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
