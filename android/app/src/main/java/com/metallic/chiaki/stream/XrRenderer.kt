// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.stream

import android.app.Activity
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
	private val exitCallback: () -> Unit)
{
	companion object
	{
		init { System.loadLibrary("chiaki-jni") }
	}

	private val mainHandler = Handler(Looper.getMainLooper())
	private var ptr = 0L
	private var lastInput = ControllerState()

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
		val state = ControllerState(
			buttons = buttons.toUInt(),
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

	private external fun nativeStart(activity: Activity, width: Int, height: Int): Long
	private external fun nativeStop(ptr: Long)
}
