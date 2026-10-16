// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

package com.metallic.chiaki.session

import android.os.Build
import android.os.CombinedVibration
import android.os.VibrationEffect
import android.view.InputDevice

/** Console rumble on connected gamepads (Xbox controllers and the like), Android 12+ */
class GamepadRumble
{
	private var last = 0

	fun rumble(left: Int, right: Int)
	{
		if(Build.VERSION.SDK_INT < Build.VERSION_CODES.S)
			return
		val amplitude = maxOf(left, right).coerceIn(0, 255)
		if(amplitude == last)
			return
		last = amplitude
		for(id in InputDevice.getDeviceIds())
		{
			val device = InputDevice.getDevice(id) ?: continue
			if(device.sources and InputDevice.SOURCE_GAMEPAD != InputDevice.SOURCE_GAMEPAD)
				continue
			val manager = device.vibratorManager
			if(manager.vibratorIds.isEmpty())
				continue
			if(amplitude == 0)
				manager.cancel()
			else
			{
				// Both motors when there are two: left is the heavy one, right the light one
				val ids = manager.vibratorIds
				val combined = if(ids.size >= 2)
					CombinedVibration.startParallel()
						.addVibrator(ids[0], VibrationEffect.createOneShot(10_000, left.coerceIn(1, 255)))
						.addVibrator(ids[1], VibrationEffect.createOneShot(10_000, right.coerceIn(1, 255)))
						.combine()
				else
					CombinedVibration.createParallel(VibrationEffect.createOneShot(10_000, amplitude))
				manager.vibrate(combined)
			}
		}
	}
}
