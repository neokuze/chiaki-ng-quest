// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.app.Activity
import android.graphics.Bitmap
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RadialGradient
import android.graphics.Shader
import android.graphics.Typeface
import java.nio.ByteBuffer
import android.os.Handler
import android.os.Looper
import android.view.Surface
import com.metallic.chiaki.lib.ControllerState

/**
 * Drives the native OpenXR session (xr-renderer.c). Callbacks arrive on the XR thread
 * and are forwarded to the main thread.
 */
class XrRenderer(
	private val activity: Activity,
	private val surfaceCallback: (Surface?) -> Unit,
	private val inputCallback: (ControllerState) -> Unit,
	private val menuCallback: (Boolean) -> Unit,
	private val exitCallback: () -> Unit)
{
	companion object
	{
		init { System.loadLibrary("chiaki-jni") }
		private const val MENU_OPEN_BIT = 1 shl 30
	}

	private val mainHandler = Handler(Looper.getMainLooper())
	private var ptr = 0L
	private var lastInput = ControllerState()

	/** True while the secret menu is open; the activity must not forward gamepad events then */
	@Volatile var menuOpen = false
		private set

	fun start(width: Int, height: Int)
	{
		if(ptr == 0L)
			ptr = nativeStart(activity, width, height)
	}

	fun stop()
	{
		if(ptr != 0L)
			nativeStop(ptr)
		ptr = 0L
	}

	@Suppress("unused") // called from native
	private fun onSurface(surface: Surface?)
	{
		mainHandler.post { surfaceCallback(surface) }
	}

	@Suppress("unused") // called from native, once per frame
	private fun onInput(buttons: Int, l2: Int, r2: Int, lx: Short, ly: Short, rx: Short, ry: Short)
	{
		val open = buttons and MENU_OPEN_BIT != 0
		if(open != menuOpen)
		{
			menuOpen = open
			mainHandler.post { menuCallback(open) }
		}
		val state = ControllerState(
			buttons = (buttons and MENU_OPEN_BIT.inv()).toUInt(),
			l2State = l2.coerceIn(0, 255).toUByte(),
			r2State = r2.coerceIn(0, 255).toUByte(),
			leftX = lx, leftY = ly, rightX = rx, rightY = ry)
		if(state == lastInput)
			return
		lastInput = state
		mainHandler.post { inputCallback(state) }
	}

	@Suppress("unused") // called from native
	private fun onExit()
	{
		mainHandler.post { exitCallback() }
	}

	/**
	 * Secret menu images for the native side, RGBA bytes (premultiplied), top row first.
	 * 0 idle, 1..4 left/up/right/down highlighted, 5 resize hint, 6 move hint.
	 * Style: translucent black smoke (~40% opacity) with thin light accents.
	 */
	@Suppress("unused") // called from native
	private fun menuImage(index: Int, size: Int): ByteArray
	{
		val bmp = Bitmap.createBitmap(size, size, Bitmap.Config.ARGB_8888)
		val c = Canvas(bmp)
		val s = size / 512f
		val mid = size / 2f
		val smokeAlpha = 102 // 40%

		fun smoke(cx: Float, cy: Float, radius: Float) = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			shader = RadialGradient(cx, cy, radius,
				intArrayOf(Color.argb(smokeAlpha, 0, 0, 0), Color.argb(smokeAlpha, 8, 8, 12), Color.argb(0, 0, 0, 0)),
				floatArrayOf(0f, 0.72f, 1f), Shader.TileMode.CLAMP)
		}
		val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			style = Paint.Style.STROKE
			strokeWidth = 2f * s
			color = Color.argb(90, 255, 255, 255)
		}
		val label = Paint(Paint.ANTI_ALIAS_FLAG).apply {
			color = Color.argb(235, 255, 255, 255)
			textAlign = Paint.Align.CENTER
			textSize = 26f * s
			typeface = Typeface.create("sans-serif-light", Typeface.NORMAL)
			letterSpacing = 0.08f
			setShadowLayer(6f * s, 0f, 0f, Color.BLACK)
		}
		val caption = Paint(label).apply { textSize = 19f * s; color = Color.argb(170, 255, 255, 255) }
		val title = Paint(label).apply {
			textSize = 28f * s
			typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
		}

		if(index >= 5)
		{
			c.drawRect(0f, 0f, size.toFloat(), size.toFloat(), smoke(mid, mid, mid))
			val lines = if(index == 5)
				listOf("RESIZE", "Stick up  ·  bigger", "Stick down  ·  smaller", "A  ·  done")
			else
				listOf("REPOSITION", "Look where you want it", "Stick up / down  ·  distance", "A  ·  place here")
			c.drawLine(mid - 70f * s, 218f * s, mid + 70f * s, 218f * s, ring)
			lines.forEachIndexed { i, line -> c.drawText(line, mid, (200f + i * 46f + if(i > 0) 12f else 0f) * s, if(i == 0) title else caption) }
			return toBytes(bmp)
		}

		c.drawRect(0f, 0f, size.toFloat(), size.toFloat(), smoke(mid, mid, mid))
		c.drawCircle(mid, mid, 200f * s, ring)

		// left, up, right, down
		val opts = listOf(
			Triple(118f, 256f, "RESIZE"),
			Triple(256f, 118f, "CLOSE"),
			Triple(394f, 256f, "MOVE"),
			Triple(256f, 394f, "EXIT"))
		val glow = Paint(Paint.ANTI_ALIAS_FLAG)
		opts.forEachIndexed { i, (x, y, text) ->
			if(index == i + 1)
			{
				glow.shader = RadialGradient(x * s, y * s, 80f * s,
					intArrayOf(Color.argb(150, 255, 255, 255), Color.argb(40, 255, 255, 255), Color.argb(0, 255, 255, 255)),
					floatArrayOf(0f, 0.45f, 1f), Shader.TileMode.CLAMP)
				c.drawCircle(x * s, y * s, 80f * s, glow)
			}
			c.drawText(text, x * s, (y + 9f) * s, if(index == i + 1) title else label)
		}

		// virtual stick knob, nudged toward the selection
		val dx = when(index) { 1 -> -36f; 3 -> 36f; else -> 0f } * s
		val dy = when(index) { 2 -> -36f; 4 -> 36f; else -> 0f } * s
		val knob = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(70, 255, 255, 255) }
		c.drawCircle(mid + dx, mid + dy, 30f * s, knob)
		c.drawCircle(mid + dx, mid + dy, 30f * s, ring)
		c.drawText("A", mid + dx, mid + dy + 9f * s, caption)
		return toBytes(bmp)
	}

	private fun toBytes(bmp: Bitmap): ByteArray
	{
		val buf = ByteBuffer.allocate(bmp.byteCount)
		bmp.copyPixelsToBuffer(buf) // ARGB_8888 is RGBA in memory
		bmp.recycle()
		return buf.array()
	}

	private external fun nativeStart(activity: Activity, width: Int, height: Int): Long
	private external fun nativeStop(ptr: Long)
}
