package com.odudex.kern

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.pm.PackageManager
import android.graphics.ImageFormat
import android.hardware.camera2.CameraCaptureSession
import android.hardware.camera2.CameraCharacteristics
import android.hardware.camera2.CameraDevice
import android.hardware.camera2.CameraMetadata
import android.hardware.camera2.CaptureRequest
import android.hardware.camera2.params.StreamConfigurationMap
import android.media.Image
import android.media.ImageReader
import android.os.Handler
import android.os.HandlerThread
import android.os.Looper
import android.util.Log
import android.util.Size
import java.nio.ByteBuffer
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import android.hardware.camera2.CameraManager as Camera2Manager

object CameraManager {
    private const val TAG = "KernCamera"
    const val PERMISSION_REQUEST_CODE = 0xCA51

    private const val TARGET_W = 1280
    private const val TARGET_H = 960
    private const val CAMERA_TIMEOUT_SECONDS = 10L
    private const val PERMISSION_TIMEOUT_SECONDS = 60L
    private const val IMAGE_READER_IMAGES = 3

    @Volatile private var activity: Activity? = null

    @Volatile private var thread: HandlerThread? = null
    @Volatile private var handler: Handler? = null

    @Volatile private var camera: CameraDevice? = null
    @Volatile private var session: CameraCaptureSession? = null
    @Volatile private var reader: ImageReader? = null
    @Volatile private var requestBuilder: CaptureRequest.Builder? = null
    @Volatile private var streaming: Boolean = false

    @Volatile private var permissionLatch: CountDownLatch? = null
    private var permissionGranted: Boolean = false

    @Volatile private var closedLatch: CountDownLatch? = null

    @JvmStatic external fun deliverCameraFrame(
        yBuf: ByteBuffer, yRowStride: Int,
        uBuf: ByteBuffer, uRowStride: Int, uPixelStride: Int,
        vBuf: ByteBuffer, vRowStride: Int, vPixelStride: Int,
        w: Int, h: Int,
    )

    @JvmStatic external fun setNegotiatedResolution(w: Int, h: Int)

    @Synchronized
    fun attach(act: Activity) {
        activity = act
        if (thread == null) {
            val t = HandlerThread("KernCamera").apply { start() }
            thread = t
            handler = Handler(t.looper)
        }
    }

    @Synchronized
    fun detach() {
        activity = null
        val t = thread ?: return
        handler = null
        thread = null
        t.quitSafely()
    }

    fun onPermissionResult(granted: Boolean) {
        permissionGranted = granted
        permissionLatch?.countDown()
    }

    fun onActivityPause() {
        if (streaming) stopRepeating("pause")
    }

    fun onActivityResume() {
        if (streaming) startStreamingInternal()
    }

    @JvmStatic
    fun openCamera() {
        val act = activity ?: run {
            failOpen("openCamera: no Activity attached")
            return
        }

        if (!ensurePermission(act)) {
            failOpen("camera permission denied")
            return
        }

        val h = handler ?: run {
            failOpen("camera handler unavailable")
            return
        }

        val mgr = act.getSystemService(Context.CAMERA_SERVICE) as Camera2Manager

        val cameraId = pickBackCameraId(mgr) ?: run {
            failOpen("no back-facing camera")
            return
        }

        val size = pickOutputSize(mgr, cameraId) ?: run {
            failOpen("no usable YUV output size")
            return
        }

        val device = openDevice(mgr, cameraId, h) ?: run {
            failOpen("openCamera failed")
            return
        }
        camera = device

        val ir = ImageReader.newInstance(size.width, size.height, ImageFormat.YUV_420_888, IMAGE_READER_IMAGES)
        ir.setOnImageAvailableListener(imageListener, h)
        reader = ir

        session = createSession(device, ir, h) ?: run {
            failOpen("session configure failed", closeCamera = true)
            return
        }

        val rb = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW)
        rb.addTarget(ir.surface)
        rb.set(CaptureRequest.CONTROL_AF_MODE, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
        rb.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON)
        requestBuilder = rb

        // Negotiated buffer is sensor frame rotated 90° CW (see android_video.c).
        Log.i(TAG, "Camera opened: sensor ${size.width}x${size.height} → ${size.height}x${size.width}")
        setNegotiatedResolution(size.height, size.width)
    }

    @JvmStatic
    fun startStreaming() {
        streaming = true
        startStreamingInternal()
    }

    private fun startStreamingInternal() {
        val s = session ?: return
        val rb = requestBuilder ?: return
        val h = handler ?: return
        try {
            s.setRepeatingRequest(rb.build(), null, h)
        } catch (e: Exception) {
            Log.e(TAG, "setRepeatingRequest failed: $e")
        }
    }

    @JvmStatic
    fun stopStreaming() {
        streaming = false
        stopRepeating("stop")
    }

    @JvmStatic
    fun closeCamera() {
        streaming = false
        try { session?.close() } catch (_: Exception) {}
        session = null
        try { reader?.close() } catch (_: Exception) {}
        reader = null

        val d = camera
        camera = null
        if (d != null) {
            val latch = CountDownLatch(1)
            closedLatch = latch
            try {
                d.close()
                // Block until onClosed fires so the next openCamera doesn't
                // race the close and trip CAMERA_IN_USE.
                if (!awaitLatch(latch, 10)) {
                    Log.w(TAG, "closeCamera: timed out waiting for onClosed")
                }
            } catch (e: Exception) {
                Log.w(TAG, "closeCamera: $e")
            } finally {
                closedLatch = null
            }
        }
        requestBuilder = null
    }

    private fun pickBackCameraId(mgr: Camera2Manager): String? = try {
        val ids = mgr.cameraIdList
        ids.firstOrNull { id ->
            mgr.getCameraCharacteristics(id).get(CameraCharacteristics.LENS_FACING) ==
                CameraMetadata.LENS_FACING_BACK
        } ?: ids.firstOrNull()
    } catch (e: Exception) {
        Log.e(TAG, "pickBackCameraId: $e")
        null
    }

    private fun pickOutputSize(mgr: Camera2Manager, cameraId: String): Size? = try {
        val map = mgr.getCameraCharacteristics(cameraId)
            .get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP) as StreamConfigurationMap?
        val sizes = map?.getOutputSizes(ImageFormat.YUV_420_888)
        sizes?.maxWithOrNull(compareBy<Size>({ sizePreference(it) }, { it.area }))
    } catch (e: Exception) {
        Log.e(TAG, "pickOutputSize: $e")
        null
    }

    private fun ensurePermission(act: Activity): Boolean {
        if (act.checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED) {
            return true
        }
        val latch = CountDownLatch(1)
        permissionLatch = latch
        permissionGranted = false
        Handler(Looper.getMainLooper()).post {
            try {
                act.requestPermissions(arrayOf(Manifest.permission.CAMERA), PERMISSION_REQUEST_CODE)
            } catch (e: Exception) {
                Log.e(TAG, "requestPermissions threw: $e")
                latch.countDown()
            }
        }
        return try {
            val ok = latch.await(PERMISSION_TIMEOUT_SECONDS, TimeUnit.SECONDS)
            permissionLatch = null
            ok && permissionGranted
        } catch (_: InterruptedException) {
            permissionLatch = null
            false
        }
    }

    private fun openDevice(mgr: Camera2Manager, cameraId: String, h: Handler): CameraDevice? {
        val latch = CountDownLatch(1)
        var openedDevice: CameraDevice? = null
        var openError = -1
        val stateCb = object : CameraDevice.StateCallback() {
            override fun onOpened(d: CameraDevice) {
                openedDevice = d
                latch.countDown()
            }

            override fun onDisconnected(d: CameraDevice) {
                Log.w(TAG, "camera onDisconnected")
                d.close()
                if (camera === d) camera = null
            }

            override fun onError(d: CameraDevice, err: Int) {
                openError = err
                d.close()
                latch.countDown()
            }

            override fun onClosed(d: CameraDevice) {
                closedLatch?.countDown()
            }
        }

        try {
            mgr.openCamera(cameraId, stateCb, h)
        } catch (e: SecurityException) {
            Log.e(TAG, "openCamera SecurityException: $e")
            return null
        } catch (e: Exception) {
            Log.e(TAG, "openCamera failed: $e")
            return null
        }

        if (!awaitLatch(latch, CAMERA_TIMEOUT_SECONDS) || openedDevice == null) {
            Log.e(TAG, "openCamera timeout or error ($openError)")
        }
        return openedDevice
    }

    private fun createSession(
        device: CameraDevice,
        reader: ImageReader,
        h: Handler,
    ): CameraCaptureSession? {
        val latch = CountDownLatch(1)
        var configuredSession: CameraCaptureSession? = null
        val sessionCb = object : CameraCaptureSession.StateCallback() {
            override fun onConfigured(s: CameraCaptureSession) {
                configuredSession = s
                latch.countDown()
            }

            override fun onConfigureFailed(s: CameraCaptureSession) {
                Log.e(TAG, "session onConfigureFailed")
                latch.countDown()
            }
        }

        try {
            @Suppress("DEPRECATION")
            device.createCaptureSession(listOf(reader.surface), sessionCb, h)
        } catch (e: Exception) {
            Log.e(TAG, "createCaptureSession failed: $e")
            return null
        }

        if (!awaitLatch(latch, CAMERA_TIMEOUT_SECONDS) || configuredSession == null) {
            Log.e(TAG, "session configure timeout")
        }
        return configuredSession
    }

    private fun failOpen(message: String, closeCamera: Boolean = false) {
        Log.e(TAG, message)
        if (closeCamera) closeCamera()
        setNegotiatedResolution(0, 0)
    }

    private fun stopRepeating(reason: String) {
        try {
            session?.stopRepeating()
        } catch (e: Exception) {
            Log.w(TAG, "stopRepeating ($reason): $e")
        }
    }

    private val Size.area: Long
        get() = width.toLong() * height

    private fun sizePreference(size: Size): Int = when {
        size.width == TARGET_W && size.height == TARGET_H -> 3
        size.isFourThree && minOf(size.width, size.height) >= TARGET_H -> 2
        minOf(size.width, size.height) >= TARGET_H -> 1
        else -> 0
    }

    private val Size.isFourThree: Boolean
        get() = width * 3 == height * 4 || height * 3 == width * 4

    private fun awaitLatch(latch: CountDownLatch, seconds: Long): Boolean = try {
        latch.await(seconds, TimeUnit.SECONDS)
    } catch (_: InterruptedException) {
        false
    }

    private val imageListener = ImageReader.OnImageAvailableListener { r ->
        var img: Image? = null
        try {
            img = r.acquireLatestImage() ?: return@OnImageAvailableListener
            val planes = img.planes
            val y = planes[0]
            val u = planes[1]
            val v = planes[2]
            y.buffer.position(0); u.buffer.position(0); v.buffer.position(0)
            deliverCameraFrame(
                y.buffer, y.rowStride,
                u.buffer, u.rowStride, u.pixelStride,
                v.buffer, v.rowStride, v.pixelStride,
                img.width, img.height,
            )
        } catch (e: Exception) {
            Log.w(TAG, "deliver frame: $e")
        } finally {
            img?.close()
        }
    }
}
