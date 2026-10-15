// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.content.Context
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.Paint
import android.graphics.RectF
import android.graphics.Typeface
import android.view.View
import com.metallic.chiaki.lib.ControllerState
import com.metallic.chiaki.session.TouchOverlay

/** Touchpad mode overlay for gamepads: translucent smoke panel with the fingers on it */
class TouchpadOverlayView(context: Context): View(context)
{
	var overlay = TouchOverlay.NONE
		set(value)
		{
			field = value
			visibility = if(value.active || value.progress >= 0f) VISIBLE else GONE
			invalidate()
		}

	private val density = resources.displayMetrics.density
	private val smoke = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(102, 0, 0, 0) }
	private val panelFill = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(40, 255, 255, 255) }
	private val ring = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		style = Paint.Style.STROKE
		strokeWidth = 1.5f * density
		color = Color.argb(110, 255, 255, 255)
	}
	private val bar = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(215, 255, 255, 255) }
	private val finger = Paint(Paint.ANTI_ALIAS_FLAG).apply { color = Color.argb(230, 255, 255, 255) }
	private val title = Paint(Paint.ANTI_ALIAS_FLAG).apply {
		color = Color.argb(235, 255, 255, 255)
		textAlign = Paint.Align.CENTER
		textSize = 16f * density
		letterSpacing = 0.08f
		typeface = Typeface.create("sans-serif-medium", Typeface.NORMAL)
	}
	private val caption = Paint(title).apply {
		textSize = 12f * density
		color = Color.argb(170, 255, 255, 255)
		typeface = Typeface.create("sans-serif-light", Typeface.NORMAL)
	}

	init
	{
		visibility = GONE
		isClickable = false
		isFocusable = false
	}

	override fun onDraw(canvas: Canvas)
	{
		val o = overlay
		val w = minOf(width * 0.42f, 420f * density)
		val ph = w * ControllerState.TOUCHPAD_HEIGHT.toFloat() / ControllerState.TOUCHPAD_WIDTH.toFloat()
		val cx = width / 2f
		val top = height - ph - 96f * density
		val back = RectF(cx - w / 2f - 20f * density, top - 44f * density, cx + w / 2f + 20f * density, top + ph + 50f * density)
		canvas.drawRoundRect(back, 22f * density, 22f * density, smoke)
		canvas.drawText("TOUCHPAD", cx, top - 16f * density, title)

		if(!o.active)
		{
			val barRect = RectF(cx - w / 2f, top + ph / 2f - 3f * density, cx + w / 2f, top + ph / 2f + 3f * density)
			canvas.drawRect(barRect, ring)
			canvas.drawRect(barRect.left, barRect.top, barRect.left + barRect.width() * o.progress.coerceIn(0f, 1f), barRect.bottom, bar)
			canvas.drawText("Keep holding Menu", cx, top + ph + 30f * density, caption)
			return
		}

		val panel = RectF(cx - w / 2f, top, cx + w / 2f, top + ph)
		canvas.drawRoundRect(panel, 12f * density, 12f * density, panelFill)
		canvas.drawRoundRect(panel, 12f * density, 12f * density, ring)
		for((i, f) in listOf(o.finger0, o.finger1).withIndex())
		{
			if(f < 0)
				continue
			val fx = panel.left + (f and 0xffff) / (ControllerState.TOUCHPAD_WIDTH - 1U).toFloat() * panel.width()
			val fy = panel.top + (f ushr 16) / (ControllerState.TOUCHPAD_HEIGHT - 1U).toFloat() * panel.height()
			finger.alpha = if(i == 0) 230 else 150
			canvas.drawCircle(fx, fy, 9f * density, finger)
		}
		canvas.drawText("Right stick  ·  touch     R3  ·  click     release Menu to exit", cx, top + ph + 30f * density, caption)
	}
}
