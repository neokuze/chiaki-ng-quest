// SPDX-License-Identifier: LicenseRef-AGPL-3.0-only-OpenSSL

// Immersive Quest mode: an OpenXR session whose only content is a quad layer backed by an
// Android surface swapchain. The video decoder renders straight into that surface, so the
// compositor shows the stream on a virtual screen without any GL drawing of our own.
// Quest Touch controllers are read through OpenXR actions and reported as a DualSense state.

#include <jni.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <dlfcn.h>
#include <time.h>
#include <unistd.h>
#include <android/log.h>

#include <EGL/egl.h>
#include <GLES3/gl3.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define LOG_TAG "ChiakiXR"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

// Bits match ControllerState in Chiaki.kt
#define BTN_CROSS    (1 << 0)
#define BTN_MOON     (1 << 1)
#define BTN_BOX      (1 << 2)
#define BTN_PYRAMID  (1 << 3)
#define BTN_DPAD_LEFT  (1 << 4)
#define BTN_DPAD_RIGHT (1 << 5)
#define BTN_DPAD_UP    (1 << 6)
#define BTN_DPAD_DOWN  (1 << 7)
#define BTN_L1       (1 << 8)
#define BTN_R1       (1 << 9)
#define BTN_L3       (1 << 10)
#define BTN_R3       (1 << 11)
#define BTN_OPTIONS  (1 << 12)
#define BTN_SHARE    (1 << 13)
#define BTN_TOUCHPAD (1 << 14)
#define BTN_PS       (1 << 15)
#define BTN_MENU_OPEN (1 << 30) // not a DualSense button, stripped in XrRenderer.kt
#define BTN_SHAKE     (1 << 29) // Menu + L3 held: XrRenderer.kt shakes the virtual controller

#define DPAD_THRESHOLD 0.5f
#define OPTIONS_TAP_FRAMES 8 // ~90 ms at 90 Hz, long enough for the console to see the press

#define SCREEN_WIDTH_M 3.2f
#define SCREEN_DISTANCE_M 2.4f

// Secret menu: Menu + L1 + L2 + R1 + R2. Images come from Kotlin (XrRenderer.menuImage)
#define MENU_IMG_SIZE 512
#define MENU_IMG_COUNT 13 // 0 idle, 1 left, 2 up, 3 right, 4 down, 5 resize hint, 6 move hint,
                          // 7..10 Bad Network Mode notice for 1080p, 720p, 540p, 360p,
                          // 11 touchpad panel, 12 touchpad hold hint
#define IMG_TOUCHPAD 11
#define IMG_TOUCH_HOLD 12

// Touchpad mode: hold Menu alone, thumbs on the sticks become fingers on the touchpad
#define TOUCH_HOLD_S 2.8
#define TOUCH_HINT_S 0.4 // the hold hint shows up after this, so a plain Options tap stays clean
#define TOUCHPAD_W 1920
#define TOUCHPAD_H 942
// Panel rectangle and hold bar inside the menu images, top-left origin (see XrRenderer.menuImage)
#define PANEL_X0 56
#define PANEL_X1 456
#define PANEL_Y0 158
#define PANEL_Y1 354
#define HOLD_BAR_X0 106
#define HOLD_BAR_X1 406
#define HOLD_BAR_Y0 300
#define HOLD_BAR_Y1 310
#ifdef CHIAKI_ROOMS
// Room engine (android/app/src/main/rust/rooms), linked on arm64 only
typedef struct Room Room;
typedef const void *(*GlLoader)(const char *name);
Room *room_create(GlLoader loader, const uint8_t *glb, size_t len, float *screen_out);
void room_draw(const Room *room, const float *pos, const float *quat, const float *fov, const float *tint);
void room_destroy(Room *room);
#endif

// Scenes behind the screen: 0 black, 1 passthrough, 2.. panoramas (XrRenderer.panoramaImage),
// then 3D rooms (XrRenderer.roomData)
#define SCENE_BLACK 0
#define SCENE_PASSTHROUGH 1
#define SCENE_PANORAMA0 2

#define MENU_SIZE_M 0.6f
#define MENU_DISTANCE_M 1.0f
#define MENU_DIR_THRESHOLD 0.6f

enum { MODE_NONE, MODE_MENU, MODE_RESIZE, MODE_MOVE };

enum {
	A_CROSS, A_MOON, A_BOX, A_PYRAMID, A_L3, A_R3, A_MENU,
	A_L2, A_R2, A_L1, A_R1, A_LSTICK, A_RSTICK, A_LTOUCH, A_RTOUCH,
	A_RPOSE, A_LHAPTIC, A_RHAPTIC, A_COUNT
};

typedef struct XrRenderer
{
	JavaVM *vm;
	jobject activity; // global ref
	jobject callback; // global ref, the Kotlin XrRenderer
	jmethodID on_surface, on_input, on_exit, menu_image;
	int width, height;

	pthread_t thread;
	volatile bool quit;

	EGLDisplay egl_display;
	EGLConfig egl_config;
	EGLContext egl_context;
	EGLSurface egl_surface;

	XrInstance instance;
	XrSystemId system;
	XrSession session;
	XrSpace space;
	XrSwapchain swapchain;
	XrSessionState state;
	bool running;

	XrActionSet action_set;
	XrAction actions[A_COUNT];
	bool menu_held;
	bool menu_combo;
	int options_frames;

	// Screen placement, editable from the secret menu
	XrPosef screen_pose;
	float screen_width;
	float screen_distance;
	bool flip_ext; // XR_FB_composition_layer_image_layout available

	// Secret menu
	XrSpace view_space;
	XrSwapchain menu_swapchain;
	GLuint menu_textures[MENU_IMG_COUNT];
	GLuint menu_fbo[2];
	int menu_shown_img; // image currently in the swapchain, -1 = none yet
	int mode;
	int menu_dir; // 0 none, 1..4 like the image indices
	XrPosef menu_pose;
	bool prev_confirm, prev_combo;
	bool release_guard; // menu just closed: keep the pad neutral until everything is let go
	bool user_exit;
	volatile int notice; // menu image shown over the screen while no menu is open, -1 = none

	// Scene behind the screen
	bool passthrough_ext, equirect_ext;
	XrPassthroughFB passthrough;
	XrPassthroughLayerFB passthrough_layer;
	bool passthrough_running;
	PFN_xrPassthroughStartFB pt_start;
	PFN_xrPassthroughPauseFB pt_pause;
	PFN_xrPassthroughLayerResumeFB pt_layer_resume;
	PFN_xrPassthroughLayerPauseFB pt_layer_pause;
	PFN_xrDestroyPassthroughFB pt_destroy;
	PFN_xrDestroyPassthroughLayerFB pt_layer_destroy;
	int scene, scene_count, scene_applied;
	bool scene_next; // set by the secret menu, handled on the render thread
	XrSwapchain pano_swapchain;
	int pano_w, pano_h, pano_loaded; // scene index in the swapchain, -1 = none
	bool prev_back;
	jmethodID panorama_count, panorama_image, saved_scene, save_scene;
	int pano_count, room_count; // rooms come after the panoramas in the scene list
	jmethodID room_count_m, room_data_m;
#ifdef CHIAKI_ROOMS
	Room *room;
	int room_loaded; // scene index of the loaded room, -1 = none
	XrSwapchain eye_swapchain[2];
	uint32_t eye_w[2], eye_h[2];
	XrSwapchainImageOpenGLESKHR eye_images[2][4];
	uint32_t eye_image_count[2];
	GLuint eye_depth[2];
	GLuint eye_fbo;
	XrPosef saved_screen_pose; // placement before entering a room, restored when leaving it
	float saved_screen_width;
#endif

	// Motion from the right controller, rumble on both
	XrSpace rpose_space;
	XrVector3f prev_velocity;
	XrVector3f accel_filtered;
	XrTime prev_motion_time;
	jmethodID on_motion;
	volatile int rumble[2]; // 0..255 from the console, left = big motor, right = small motor
	int rumble_applied[2];
	double rumble_time[2];

	// Touchpad mode (Quest controllers)
	double menu_down_time;
	bool hold_clean; // Menu held alone since it went down
	float hold_progress; // 0..1 while the hold hint shows, -1 otherwise
	bool touch_mode;
	int finger[2]; // touchpad position packed as x | y << 16, -1 = lifted

	// Touchpad overlay requested from Kotlin (gamepad touchpad mode)
	volatile float ext_progress;
	volatile bool ext_active;
	volatile int ext_finger[2];
} XrRenderer;

#define XR_CHECK(expr) do { XrResult _r = (expr); if(XR_FAILED(_r)) { LOGE("%s failed: %d", #expr, (int)_r); goto fail; } } while(0)

static bool egl_init(XrRenderer *r)
{
	r->egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
	if(!eglInitialize(r->egl_display, NULL, NULL))
		return false;
	const EGLint cfg_attr[] = {
		EGL_RENDERABLE_TYPE, 0x40 /* EGL_OPENGL_ES3_BIT */,
		EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
		EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
		EGL_NONE
	};
	EGLint n = 0;
	if(!eglChooseConfig(r->egl_display, cfg_attr, &r->egl_config, 1, &n) || n < 1)
		return false;
	const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
	r->egl_context = eglCreateContext(r->egl_display, r->egl_config, EGL_NO_CONTEXT, ctx_attr);
	if(r->egl_context == EGL_NO_CONTEXT)
		return false;
	const EGLint pb_attr[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
	r->egl_surface = eglCreatePbufferSurface(r->egl_display, r->egl_config, pb_attr);
	return eglMakeCurrent(r->egl_display, r->egl_surface, r->egl_surface, r->egl_context);
}

static void egl_fini(XrRenderer *r)
{
	if(r->egl_display == EGL_NO_DISPLAY)
		return;
	eglMakeCurrent(r->egl_display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
	if(r->egl_surface != EGL_NO_SURFACE)
		eglDestroySurface(r->egl_display, r->egl_surface);
	if(r->egl_context != EGL_NO_CONTEXT)
		eglDestroyContext(r->egl_display, r->egl_context);
	eglTerminate(r->egl_display);
}

static XrPath path(XrRenderer *r, const char *s)
{
	XrPath p = XR_NULL_PATH;
	xrStringToPath(r->instance, s, &p);
	return p;
}

static bool create_action(XrRenderer *r, int idx, const char *name, XrActionType type)
{
	XrActionCreateInfo info = { XR_TYPE_ACTION_CREATE_INFO };
	info.actionType = type;
	strncpy(info.actionName, name, sizeof(info.actionName) - 1);
	strncpy(info.localizedActionName, name, sizeof(info.localizedActionName) - 1);
	return XR_SUCCEEDED(xrCreateAction(r->action_set, &info, &r->actions[idx]));
}

static bool input_init(XrRenderer *r)
{
	XrActionSetCreateInfo set_info = { XR_TYPE_ACTION_SET_CREATE_INFO };
	strcpy(set_info.actionSetName, "gamepad");
	strcpy(set_info.localizedActionSetName, "Gamepad");
	if(XR_FAILED(xrCreateActionSet(r->instance, &set_info, &r->action_set)))
		return false;

	static const struct { int idx; const char *name; XrActionType type; const char *binding; } map[] = {
		{ A_CROSS,   "cross",   XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/a/click" },
		{ A_MOON,    "moon",    XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/b/click" },
		{ A_BOX,     "box",     XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/x/click" },
		{ A_PYRAMID, "pyramid", XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/y/click" },
		{ A_L3,      "l3",      XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/click" },
		{ A_R3,      "r3",      XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/click" },
		{ A_MENU,    "menu",    XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/menu/click" },
		{ A_L2,      "l2",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/left/input/trigger/value" },
		{ A_R2,      "r2",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/right/input/trigger/value" },
		{ A_L1,      "l1",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/left/input/squeeze/value" },
		{ A_R1,      "r1",      XR_ACTION_TYPE_FLOAT_INPUT,   "/user/hand/right/input/squeeze/value" },
		{ A_LSTICK,  "lstick",  XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/left/input/thumbstick" },
		{ A_RSTICK,  "rstick",  XR_ACTION_TYPE_VECTOR2F_INPUT, "/user/hand/right/input/thumbstick" },
		{ A_LTOUCH,  "ltouch",  XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/left/input/thumbstick/touch" },
		{ A_RTOUCH,  "rtouch",  XR_ACTION_TYPE_BOOLEAN_INPUT, "/user/hand/right/input/thumbstick/touch" },
		{ A_RPOSE,   "rpose",   XR_ACTION_TYPE_POSE_INPUT,    "/user/hand/right/input/grip/pose" },
		{ A_LHAPTIC, "lhaptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/user/hand/left/output/haptic" },
		{ A_RHAPTIC, "rhaptic", XR_ACTION_TYPE_VIBRATION_OUTPUT, "/user/hand/right/output/haptic" },
	};

	XrActionSuggestedBinding bindings[A_COUNT];
	for(int i = 0; i < A_COUNT; i++)
	{
		if(!create_action(r, map[i].idx, map[i].name, map[i].type))
			return false;
		bindings[i].action = r->actions[map[i].idx];
		bindings[i].binding = path(r, map[i].binding);
	}

	XrInteractionProfileSuggestedBinding suggested = { XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING };
	suggested.interactionProfile = path(r, "/interaction_profiles/oculus/touch_controller");
	suggested.suggestedBindings = bindings;
	suggested.countSuggestedBindings = A_COUNT;
	if(XR_FAILED(xrSuggestInteractionProfileBindings(r->instance, &suggested)))
		return false;

	XrSessionActionSetsAttachInfo attach = { XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO };
	attach.countActionSets = 1;
	attach.actionSets = &r->action_set;
	if(XR_FAILED(xrAttachSessionActionSets(r->session, &attach)))
		return false;

	XrActionSpaceCreateInfo space_info = { XR_TYPE_ACTION_SPACE_CREATE_INFO };
	space_info.action = r->actions[A_RPOSE];
	space_info.poseInActionSpace.orientation.w = 1.0f;
	if(XR_FAILED(xrCreateActionSpace(r->session, &space_info, &r->rpose_space)))
		LOGE("Right controller pose space failed, no motion from the controller");
	return true;
}

static bool get_bool(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateBoolean s = { XR_TYPE_ACTION_STATE_BOOLEAN };
	return XR_SUCCEEDED(xrGetActionStateBoolean(r->session, &gi, &s)) && s.isActive && s.currentState;
}

static float get_float(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateFloat s = { XR_TYPE_ACTION_STATE_FLOAT };
	return (XR_SUCCEEDED(xrGetActionStateFloat(r->session, &gi, &s)) && s.isActive) ? s.currentState : 0.0f;
}

static XrVector2f get_vec2(XrRenderer *r, int idx)
{
	XrActionStateGetInfo gi = { XR_TYPE_ACTION_STATE_GET_INFO };
	gi.action = r->actions[idx];
	XrActionStateVector2f s = { XR_TYPE_ACTION_STATE_VECTOR2F };
	XrVector2f zero = { 0, 0 };
	return (XR_SUCCEEDED(xrGetActionStateVector2f(r->session, &gi, &s)) && s.isActive) ? s.currentState : zero;
}

static jshort stick_axis(float v)
{
	if(v > 1.0f) v = 1.0f;
	if(v < -1.0f) v = -1.0f;
	return (jshort)(v * 32767.0f);
}

static XrQuaternionf quat_mul(XrQuaternionf a, XrQuaternionf b)
{
	XrQuaternionf q = {
		a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
		a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
		a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
		a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z
	};
	return q;
}

static XrTime frame_time; // predicted display time of the frame being built

// Pose `distance` metres in front of the head, kept upright (yaw only)
static bool pose_in_front_of_head(XrRenderer *r, float distance, bool follow_pitch, XrPosef *out)
{
	if(r->view_space == XR_NULL_HANDLE || !frame_time)
		return false;
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION };
	if(XR_FAILED(xrLocateSpace(r->view_space, r->space, frame_time, &loc))
			|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			|| !(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT))
		return false;
	XrQuaternionf q = loc.pose.orientation;
	// forward = q * (0,0,-1), flattened onto the floor plane
	float fx = -2.0f * (q.x * q.z + q.w * q.y);
	float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
	float len = sqrtf(fx * fx + fz * fz);
	if(len < 1e-4f)
		return false;
	fx /= len; fz /= len;
	float yaw = atan2f(-fx, -fz);
	float pitch = 0.0f;
	if(follow_pitch)
	{
		float fy = 2.0f * (q.w * q.x - q.y * q.z);
		if(fy > 1.0f) fy = 1.0f;
		if(fy < -1.0f) fy = -1.0f;
		pitch = asinf(fy);
		// small nods keep the screen upright, only a clear look up/down tilts it
		const float dead = 8.0f * 3.14159265f / 180.0f;
		if(fabsf(pitch) < dead)
			pitch = 0.0f;
	}
	XrQuaternionf qyaw = { 0.0f, sinf(yaw * 0.5f), 0.0f, cosf(yaw * 0.5f) };
	XrQuaternionf qpitch = { sinf(pitch * 0.5f), 0.0f, 0.0f, cosf(pitch * 0.5f) };
	// the quad faces +Z, so tilting it by the head pitch keeps it facing the viewer
	out->orientation = quat_mul(qyaw, qpitch);
	float cp = cosf(pitch);
	out->position.x = loc.pose.position.x + fx * cp * distance;
	out->position.y = loc.pose.position.y + sinf(pitch) * distance;
	out->position.z = loc.pose.position.z + fz * cp * distance;
	return true;
}

static int stick_dir(XrVector2f v)
{
	if(fabsf(v.x) < MENU_DIR_THRESHOLD && fabsf(v.y) < MENU_DIR_THRESHOLD)
		return 0;
	if(fabsf(v.x) > fabsf(v.y))
		return v.x < 0 ? 1 : 3;
	return v.y > 0 ? 2 : 4;
}

static float stronger_y(XrVector2f a, XrVector2f b)
{
	return fabsf(a.y) > fabsf(b.y) ? a.y : b.y;
}

// Returns true while the secret menu (or one of its modes) owns the controllers
static bool handle_menu(XrRenderer *r)
{
	bool combo = get_bool(r, A_MENU) && get_float(r, A_L1) > 0.5f && get_float(r, A_R1) > 0.5f
		&& get_float(r, A_L2) > 0.5f && get_float(r, A_R2) > 0.5f;
	bool confirm = get_bool(r, A_CROSS);
	bool confirm_edge = confirm && !r->prev_confirm;
	bool combo_edge = combo && !r->prev_combo;
	bool back = get_bool(r, A_MOON);
	bool back_edge = back && !r->prev_back;
	r->prev_back = back;
	r->prev_confirm = confirm;
	r->prev_combo = combo;

	if(r->mode == MODE_NONE && r->release_guard)
	{
		XrVector2f gl = get_vec2(r, A_LSTICK), gr = get_vec2(r, A_RSTICK);
		bool any = get_bool(r, A_MENU) || confirm || get_bool(r, A_MOON) || get_bool(r, A_BOX)
			|| get_bool(r, A_PYRAMID) || get_bool(r, A_L3) || get_bool(r, A_R3)
			|| get_float(r, A_L1) > 0.2f || get_float(r, A_R1) > 0.2f
			|| get_float(r, A_L2) > 0.2f || get_float(r, A_R2) > 0.2f
			|| fabsf(gl.x) > 0.3f || fabsf(gl.y) > 0.3f || fabsf(gr.x) > 0.3f || fabsf(gr.y) > 0.3f;
		if(any)
			return true;
		r->release_guard = false;
		r->menu_held = false;
		return false;
	}

	if(r->mode == MODE_NONE)
	{
		if(!combo_edge || r->menu_swapchain == XR_NULL_HANDLE)
			return false;
		if(!pose_in_front_of_head(r, MENU_DISTANCE_M, false, &r->menu_pose))
		{
			r->menu_pose.orientation = (XrQuaternionf){ 0, 0, 0, 1 };
			r->menu_pose.position = (XrVector3f){ 0, 0, -MENU_DISTANCE_M };
		}
		r->mode = MODE_MENU;
		r->menu_dir = 0;
		r->menu_combo = true; // releasing Menu must not send Options
		LOGI("Secret menu opened");
		return true;
	}

	XrVector2f ls = get_vec2(r, A_LSTICK);
	XrVector2f rs = get_vec2(r, A_RSTICK);
	switch(r->mode)
	{
		case MODE_MENU:
		{
			int d = stick_dir(rs);
			if(!d)
				d = stick_dir(ls);
			if(d)
				r->menu_dir = d;
			if(confirm_edge && r->menu_dir)
			{
				switch(r->menu_dir)
				{
					case 1: r->mode = MODE_RESIZE; break;
					case 2: r->scene_next = true; break; // stays open to keep cycling
					case 3: r->mode = MODE_MOVE; break;
					case 4: r->mode = MODE_NONE; r->user_exit = true; r->quit = true; break;
				}
				if(r->mode != MODE_MENU)
					r->menu_dir = 0;
			}
			else if(back_edge)
				r->mode = MODE_NONE;
			break;
		}
		case MODE_RESIZE:
		{
			float y = stronger_y(rs, ls);
			if(fabsf(y) > 0.15f)
				r->screen_width *= 1.0f + y * 0.015f;
			if(r->screen_width < 0.8f) r->screen_width = 0.8f;
			if(r->screen_width > 12.0f) r->screen_width = 12.0f;
			if(confirm_edge)
				r->mode = MODE_NONE;
			break;
		}
		case MODE_MOVE:
		{
			float y = stronger_y(rs, ls);
			if(fabsf(y) > 0.15f)
				r->screen_distance *= 1.0f - y * 0.015f;
			if(r->screen_distance < 0.8f) r->screen_distance = 0.8f;
			if(r->screen_distance > 10.0f) r->screen_distance = 10.0f;
			pose_in_front_of_head(r, r->screen_distance, true, &r->screen_pose);
			if(confirm_edge)
				r->mode = MODE_NONE;
			break;
		}
	}
	if(r->mode == MODE_NONE)
		r->release_guard = true;
	return true;
}

#define GET_PROC(name, dst) xrGetInstanceProcAddr(r->instance, name, (PFN_xrVoidFunction *)&(dst))

static void passthrough_init(XrRenderer *r)
{
	if(!r->passthrough_ext)
		return;
	PFN_xrCreatePassthroughFB create = NULL;
	PFN_xrCreatePassthroughLayerFB create_layer = NULL;
	GET_PROC("xrCreatePassthroughFB", create);
	GET_PROC("xrCreatePassthroughLayerFB", create_layer);
	GET_PROC("xrPassthroughStartFB", r->pt_start);
	GET_PROC("xrPassthroughPauseFB", r->pt_pause);
	GET_PROC("xrPassthroughLayerResumeFB", r->pt_layer_resume);
	GET_PROC("xrPassthroughLayerPauseFB", r->pt_layer_pause);
	GET_PROC("xrDestroyPassthroughFB", r->pt_destroy);
	GET_PROC("xrDestroyPassthroughLayerFB", r->pt_layer_destroy);
	if(!create || !create_layer || !r->pt_start || !r->pt_pause || !r->pt_layer_resume || !r->pt_layer_pause)
	{
		r->passthrough_ext = false;
		return;
	}
	XrPassthroughCreateInfoFB info = { XR_TYPE_PASSTHROUGH_CREATE_INFO_FB };
	XrResult res = create(r->session, &info, &r->passthrough);
	if(XR_SUCCEEDED(res))
	{
		XrPassthroughLayerCreateInfoFB layer_info = { XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB };
		layer_info.passthrough = r->passthrough;
		layer_info.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
		res = create_layer(r->session, &layer_info, &r->passthrough_layer);
	}
	LOGI("Passthrough setup: %d", (int)res);
	if(XR_FAILED(res))
		r->passthrough_ext = false;
}

static void passthrough_set(XrRenderer *r, bool on)
{
	if(!r->passthrough_ext || on == r->passthrough_running)
		return;
	if(on)
	{
		r->pt_start(r->passthrough);
		r->pt_layer_resume(r->passthrough_layer);
	}
	else
	{
		r->pt_layer_pause(r->passthrough_layer);
		r->pt_pause(r->passthrough);
	}
	r->passthrough_running = on;
}

static bool scene_is_room(XrRenderer *r, int scene)
{
	return scene >= SCENE_PANORAMA0 + r->pano_count;
}

static bool scene_available(XrRenderer *r, int scene)
{
	if(scene == SCENE_PASSTHROUGH)
		return r->passthrough_ext;
	if(scene_is_room(r, scene))
	{
#ifdef CHIAKI_ROOMS
		return true;
#else
		return false;
#endif
	}
	if(scene >= SCENE_PANORAMA0)
		return r->equirect_ext;
	return true;
}

#ifdef CHIAKI_ROOMS
static const void *gl_loader(const char *name)
{
	static void *gles;
	if(!gles)
		gles = dlopen("libGLESv3.so", RTLD_NOW | RTLD_LOCAL);
	void *f = gles ? dlsym(gles, name) : NULL;
	return f ? f : (const void *)eglGetProcAddress(name);
}

// One color swapchain and depth buffer per eye, created the first time a room is shown
static bool eyes_init(XrRenderer *r)
{
	if(r->eye_swapchain[0] != XR_NULL_HANDLE)
		return true;
	XrViewConfigurationView cfg[2] = { { XR_TYPE_VIEW_CONFIGURATION_VIEW }, { XR_TYPE_VIEW_CONFIGURATION_VIEW } };
	uint32_t n = 0;
	if(XR_FAILED(xrEnumerateViewConfigurationViews(r->instance, r->system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &n, cfg)) || n != 2)
		return false;
	glGenFramebuffers(1, &r->eye_fbo);
	glGenRenderbuffers(2, r->eye_depth);
	for(int e = 0; e < 2; e++)
	{
		r->eye_w[e] = cfg[e].recommendedImageRectWidth;
		r->eye_h[e] = cfg[e].recommendedImageRectHeight;
		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		info.format = GL_SRGB8_ALPHA8;
		info.width = r->eye_w[e];
		info.height = r->eye_h[e];
		info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
		XrResult res = xrCreateSwapchain(r->session, &info, &r->eye_swapchain[e]);
		if(XR_FAILED(res))
		{
			info.format = GL_RGBA8;
			res = xrCreateSwapchain(r->session, &info, &r->eye_swapchain[e]);
		}
		if(XR_FAILED(res))
		{
			LOGE("Eye swapchain failed: %d", (int)res);
			return false;
		}
		for(int i = 0; i < 4; i++)
			r->eye_images[e][i] = (XrSwapchainImageOpenGLESKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
		xrEnumerateSwapchainImages(r->eye_swapchain[e], 4, &r->eye_image_count[e], (XrSwapchainImageBaseHeader *)r->eye_images[e]);
		glBindRenderbuffer(GL_RENDERBUFFER, r->eye_depth[e]);
		glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, r->eye_w[e], r->eye_h[e]);
	}
	glBindRenderbuffer(GL_RENDERBUFFER, 0);
	LOGI("Eye swapchains %ux%u", r->eye_w[0], r->eye_h[0]);
	return true;
}

static void room_unload(XrRenderer *r)
{
	if(!r->room)
		return;
	room_destroy(r->room);
	r->room = NULL;
	r->room_loaded = -1;
	r->screen_pose = r->saved_screen_pose;
	r->screen_width = r->saved_screen_width;
}

static bool room_load(XrRenderer *r, JNIEnv *env, int scene)
{
	if(!eyes_init(r))
		return false;
	jbyteArray arr = (*env)->CallObjectMethod(env, r->callback, r->room_data_m, scene - SCENE_PANORAMA0 - r->pano_count);
	if(!arr)
		return false;
	jsize len = (*env)->GetArrayLength(env, arr);
	jbyte *data = (*env)->GetByteArrayElements(env, arr, NULL);
	float screen[9];
	Room *room = room_create(gl_loader, (const uint8_t *)data, (size_t)len, screen);
	(*env)->ReleaseByteArrayElements(env, arr, data, JNI_ABORT);
	(*env)->DeleteLocalRef(env, arr);
	glBindFramebuffer(GL_FRAMEBUFFER, 0);
	if(!room)
		return false;
	if(!r->room)
	{
		r->saved_screen_pose = r->screen_pose;
		r->saved_screen_width = r->screen_width;
	}
	else
		room_destroy(r->room);
	r->room = room;
	r->room_loaded = scene;
	// The video goes exactly where the room has its screen
	r->screen_pose.position = (XrVector3f){ screen[0], screen[1], screen[2] };
	r->screen_pose.orientation = (XrQuaternionf){ screen[5], screen[6], screen[7], screen[8] };
	r->screen_width = screen[3];
	return true;
}

// Renders the room for both eyes and fills the projection layer views
static bool room_render(XrRenderer *r, XrTime time, XrCompositionLayerProjectionView *pv)
{
	XrViewLocateInfo locate = { XR_TYPE_VIEW_LOCATE_INFO };
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = time;
	locate.space = r->space;
	XrViewState view_state = { XR_TYPE_VIEW_STATE };
	XrView views[2] = { { XR_TYPE_VIEW }, { XR_TYPE_VIEW } };
	uint32_t n = 0;
	if(XR_FAILED(xrLocateViews(r->session, &locate, &view_state, 2, &n, views)) || n != 2
			|| !(view_state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
		return false;
	const float tint[3] = { 0.0f, 0.0f, 0.0f }; // TODO: average color of the video
	for(int e = 0; e < 2; e++)
	{
		uint32_t index = 0;
		XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
		if(XR_FAILED(xrAcquireSwapchainImage(r->eye_swapchain[e], &acq, &index)))
			return false;
		XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
		wait.timeout = XR_INFINITE_DURATION;
		xrWaitSwapchainImage(r->eye_swapchain[e], &wait);
		glBindFramebuffer(GL_FRAMEBUFFER, r->eye_fbo);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->eye_images[e][index].image, 0);
		glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, r->eye_depth[e]);
		glViewport(0, 0, r->eye_w[e], r->eye_h[e]);
		glDisable(GL_SCISSOR_TEST);
		glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
		glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		const XrPosef *pose = &views[e].pose;
		float pos[3] = { pose->position.x, pose->position.y, pose->position.z };
		float quat[4] = { pose->orientation.x, pose->orientation.y, pose->orientation.z, pose->orientation.w };
		float fov[4] = { views[e].fov.angleLeft, views[e].fov.angleRight, views[e].fov.angleUp, views[e].fov.angleDown };
		room_draw(r->room, pos, quat, fov, tint);
		// Tiled GPUs: depth is never needed after the eye is drawn
		const GLenum discard[] = { GL_DEPTH_ATTACHMENT };
		glInvalidateFramebuffer(GL_FRAMEBUFFER, 1, discard);
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
		xrReleaseSwapchainImage(r->eye_swapchain[e], &rel);

		pv[e] = (XrCompositionLayerProjectionView){ XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW };
		pv[e].pose = views[e].pose;
		pv[e].fov = views[e].fov;
		pv[e].subImage.swapchain = r->eye_swapchain[e];
		pv[e].subImage.imageRect.extent.width = r->eye_w[e];
		pv[e].subImage.imageRect.extent.height = r->eye_h[e];
	}
	return true;
}
#endif

// Panorama from Kotlin: 8 byte header (width, height as little endian ints), then RGBA rows top first
static bool panorama_load(XrRenderer *r, JNIEnv *env, int scene)
{
	jbyteArray arr = (*env)->CallObjectMethod(env, r->callback, r->panorama_image, scene - SCENE_PANORAMA0);
	if(!arr)
		return false;
	jsize len = (*env)->GetArrayLength(env, arr);
	jbyte *data = (*env)->GetByteArrayElements(env, arr, NULL);
	int w = 0, h = 0;
	memcpy(&w, data, 4);
	memcpy(&h, data + 4, 4);
	bool ok = false;
	if(w > 0 && h > 0 && len >= 8 + w * h * 4)
	{
		if(r->pano_swapchain != XR_NULL_HANDLE)
			xrDestroySwapchain(r->pano_swapchain);
		r->pano_swapchain = XR_NULL_HANDLE;
		XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		info.format = GL_SRGB8_ALPHA8; // photos are sRGB
		info.width = (uint32_t)w;
		info.height = (uint32_t)h;
		info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
		XrResult res = xrCreateSwapchain(r->session, &info, &r->pano_swapchain);
		if(XR_FAILED(res))
		{
			info.format = GL_RGBA8;
			res = xrCreateSwapchain(r->session, &info, &r->pano_swapchain);
		}
		if(XR_SUCCEEDED(res))
		{
			GLuint tex, fbo[2];
			glGenTextures(1, &tex);
			glBindTexture(GL_TEXTURE_2D, tex);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data + 8);
			glGenFramebuffers(2, fbo);
			uint32_t index = 0;
			XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
			XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
			wait.timeout = XR_INFINITE_DURATION;
			XrSwapchainImageOpenGLESKHR images[8];
			for(int i = 0; i < 8; i++)
				images[i] = (XrSwapchainImageOpenGLESKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
			uint32_t n = 0;
			xrEnumerateSwapchainImages(r->pano_swapchain, 8, &n, (XrSwapchainImageBaseHeader *)images);
			// Fill every image so it never matters which one the runtime shows
			for(uint32_t k = 0; k < n; k++)
			{
				if(XR_FAILED(xrAcquireSwapchainImage(r->pano_swapchain, &acq, &index)))
					break;
				xrWaitSwapchainImage(r->pano_swapchain, &wait);
				glBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
				glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
				glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
				glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
				glBlitFramebuffer(0, 0, w, h, 0, h, w, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
				glFinish();
				XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
				xrReleaseSwapchainImage(r->pano_swapchain, &rel);
			}
			glBindFramebuffer(GL_FRAMEBUFFER, 0);
			glDeleteFramebuffers(2, fbo);
			glDeleteTextures(1, &tex);
			r->pano_w = w;
			r->pano_h = h;
			ok = true;
		}
		LOGI("Panorama %d (%dx%d): %d", scene - SCENE_PANORAMA0, w, h, (int)res);
	}
	(*env)->ReleaseByteArrayElements(env, arr, data, JNI_ABORT);
	(*env)->DeleteLocalRef(env, arr);
	return ok;
}

static void scene_apply(XrRenderer *r, JNIEnv *env)
{
	if(r->scene_next)
	{
		r->scene_next = false;
		int s = r->scene;
		for(int i = 0; i < r->scene_count; i++)
		{
			s = (s + 1) % r->scene_count;
			if(scene_available(r, s))
				break;
		}
		r->scene = s;
		(*env)->CallVoidMethod(env, r->callback, r->save_scene, s);
	}
	if(r->scene == r->scene_applied)
		return;
	if(!scene_available(r, r->scene))
		r->scene = SCENE_BLACK;
#ifdef CHIAKI_ROOMS
	if(scene_is_room(r, r->scene))
	{
		if(r->room_loaded != r->scene && !room_load(r, env, r->scene))
			r->scene = SCENE_BLACK;
	}
	if(!scene_is_room(r, r->scene))
		room_unload(r);
#endif
	if(r->scene >= SCENE_PANORAMA0 && !scene_is_room(r, r->scene) && r->pano_loaded != r->scene)
	{
		if(panorama_load(r, env, r->scene))
			r->pano_loaded = r->scene;
		else
			r->scene = SCENE_BLACK;
	}
	passthrough_set(r, r->scene == SCENE_PASSTHROUGH);
	r->scene_applied = r->scene;
	LOGI("Scene %d", r->scene);
}

// Loads the menu images from Kotlin into GL textures and creates the menu swapchain
static bool menu_init(XrRenderer *r, JNIEnv *env)
{
	XrReferenceSpaceCreateInfo view_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	view_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
	view_info.poseInReferenceSpace.orientation.w = 1.0f;
	XrResult res = xrCreateReferenceSpace(r->session, &view_info, &r->view_space);
	if(XR_FAILED(res)) { LOGE("menu: view space %d", res); return false; }

	// Quest supports these; try them in order instead of trusting the format enumeration
	static const int64_t formats[] = { GL_SRGB8_ALPHA8, GL_RGBA8 };
	XrSwapchainCreateInfo info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	info.width = info.height = MENU_IMG_SIZE;
	info.sampleCount = info.faceCount = info.arraySize = info.mipCount = 1;
	res = XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	for(int i = 0; i < 2 && XR_FAILED(res); i++)
	{
		info.format = formats[i];
		info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
		res = xrCreateSwapchain(r->session, &info, &r->menu_swapchain);
		if(XR_FAILED(res))
		{
			info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
			res = xrCreateSwapchain(r->session, &info, &r->menu_swapchain);
		}
		LOGI("menu: swapchain format 0x%x: %d", (int)info.format, res);
	}
	if(XR_FAILED(res))
		return false;

	glGenTextures(MENU_IMG_COUNT, r->menu_textures);
	glGenFramebuffers(2, r->menu_fbo);
	for(int i = 0; i < MENU_IMG_COUNT; i++)
	{
		jbyteArray pixels = (*env)->CallObjectMethod(env, r->callback, r->menu_image, i, MENU_IMG_SIZE);
		if((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); }
		if(!pixels) { LOGE("menu: image %d missing", i); return false; }
		jbyte *data = (*env)->GetByteArrayElements(env, pixels, NULL);
		glBindTexture(GL_TEXTURE_2D, r->menu_textures[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
		(*env)->ReleaseByteArrayElements(env, pixels, data, JNI_ABORT);
		(*env)->DeleteLocalRef(env, pixels);
	}
	r->menu_shown_img = -1;
	return true;
}

static double now_s(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static int pack_finger(XrVector2f v)
{
	float x = (v.x + 1.0f) * 0.5f, y = (1.0f - v.y) * 0.5f; // stick up = top of the touchpad
	if(x < 0.0f) x = 0.0f; if(x > 1.0f) x = 1.0f;
	if(y < 0.0f) y = 0.0f; if(y > 1.0f) y = 1.0f;
	return (int)(x * (TOUCHPAD_W - 1)) | ((int)(y * (TOUCHPAD_H - 1)) << 16);
}

// Rect in image pixels with top-left origin, drawn into the bound draw framebuffer (bottom-left origin)
static void fill_rect(int x0, int y0, int x1, int y1, float a)
{
	glScissor(x0, MENU_IMG_SIZE - y1, x1 - x0, y1 - y0);
	glClearColor(a, a, a, a); // premultiplied white
	glClear(GL_COLOR_BUFFER_BIT);
}

static void draw_finger(int packed, float a)
{
	if(packed < 0)
		return;
	int fx = PANEL_X0 + (packed & 0xffff) * (PANEL_X1 - PANEL_X0) / (TOUCHPAD_W - 1);
	int fy = PANEL_Y0 + (packed >> 16) * (PANEL_Y1 - PANEL_Y0) / (TOUCHPAD_H - 1);
	const int rad = 11;
	for(int dy = -rad; dy < rad; dy += 2)
	{
		int half = (int)sqrtf((float)(rad * rad - dy * dy));
		fill_rect(fx - half, fy + dy, fx + half, fy + dy + 2, a);
	}
}

static void menu_draw_ex(XrRenderer *r, int img, float progress, const int *fingers);

static void menu_draw(XrRenderer *r, int img)
{
	menu_draw_ex(r, img, -1.0f, NULL);
}

static void menu_draw_ex(XrRenderer *r, int img, float progress, const int *fingers)
{
	bool dynamic = progress >= 0.0f || fingers;
	if(img == r->menu_shown_img && !dynamic)
		return; // the last released image is still valid
	uint32_t index = 0;
	XrSwapchainImageAcquireInfo acq = { XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO };
	if(XR_FAILED(xrAcquireSwapchainImage(r->menu_swapchain, &acq, &index)))
		return;
	XrSwapchainImageWaitInfo wait = { XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO };
	wait.timeout = XR_INFINITE_DURATION;
	xrWaitSwapchainImage(r->menu_swapchain, &wait);

	XrSwapchainImageOpenGLESKHR images[8];
	uint32_t n = 0;
	for(int i = 0; i < 8; i++)
		images[i] = (XrSwapchainImageOpenGLESKHR){ XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR };
	xrEnumerateSwapchainImages(r->menu_swapchain, 8, &n, (XrSwapchainImageBaseHeader *)images);
	if(index < n)
	{
		glBindFramebuffer(GL_READ_FRAMEBUFFER, r->menu_fbo[0]);
		glFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, r->menu_textures[img], 0);
		glBindFramebuffer(GL_DRAW_FRAMEBUFFER, r->menu_fbo[1]);
		glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, images[index].image, 0);
		// Bitmaps are top row first, GL images bottom row first
		glBlitFramebuffer(0, 0, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, MENU_IMG_SIZE, MENU_IMG_SIZE, 0, GL_COLOR_BUFFER_BIT, GL_LINEAR);
		if(dynamic)
		{
			glEnable(GL_SCISSOR_TEST);
			if(progress >= 0.0f)
				fill_rect(HOLD_BAR_X0, HOLD_BAR_Y0, HOLD_BAR_X0 + (int)((HOLD_BAR_X1 - HOLD_BAR_X0) * progress), HOLD_BAR_Y1, 0.85f);
			if(fingers)
			{
				draw_finger(fingers[0], 0.9f);
				draw_finger(fingers[1], 0.6f);
			}
			glDisable(GL_SCISSOR_TEST);
		}
		glBindFramebuffer(GL_FRAMEBUFFER, 0);
		glFinish();
	}
	XrSwapchainImageReleaseInfo rel = { XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO };
	xrReleaseSwapchainImage(r->menu_swapchain, &rel);
	r->menu_shown_img = dynamic ? -1 : img;
}

static XrVector3f quat_rotate(XrQuaternionf q, XrVector3f v)
{
	// v' = q * v * q^-1, expanded
	XrVector3f u = { q.x, q.y, q.z };
	float s = q.w;
	float dot_uv = u.x * v.x + u.y * v.y + u.z * v.z;
	float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
	XrVector3f cross = { u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x };
	XrVector3f out = {
		2.0f * dot_uv * u.x + (s * s - dot_uu) * v.x + 2.0f * s * cross.x,
		2.0f * dot_uv * u.y + (s * s - dot_uu) * v.y + 2.0f * s * cross.y,
		2.0f * dot_uv * u.z + (s * s - dot_uu) * v.z + 2.0f * s * cross.z
	};
	return out;
}

#define GRAVITY 9.80665f

// The right controller as the DualSense motion sensor: gyro in rad/s and accelerometer in g,
// both in the controller's own frame (x right, y up, z toward the player), plus its orientation
static void motion_update(XrRenderer *r, JNIEnv *env)
{
	if(r->rpose_space == XR_NULL_HANDLE || !frame_time || !r->on_motion)
		return;
	XrSpaceVelocity vel = { XR_TYPE_SPACE_VELOCITY };
	XrSpaceLocation loc = { XR_TYPE_SPACE_LOCATION, &vel };
	if(XR_FAILED(xrLocateSpace(r->rpose_space, r->space, frame_time, &loc))
			|| !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
			|| !(vel.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT))
		return;
	XrQuaternionf q = loc.pose.orientation;
	XrQuaternionf qi = { -q.x, -q.y, -q.z, q.w };
	XrVector3f gyro = quat_rotate(qi, vel.angularVelocity);

	XrVector3f accel_world = { 0, 0, 0 };
	if((vel.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) && r->prev_motion_time && frame_time > r->prev_motion_time)
	{
		float dt = (float)((frame_time - r->prev_motion_time) * 1e-9);
		if(dt > 0.0f && dt < 0.1f)
		{
			accel_world.x = (vel.linearVelocity.x - r->prev_velocity.x) / dt;
			accel_world.y = (vel.linearVelocity.y - r->prev_velocity.y) / dt;
			accel_world.z = (vel.linearVelocity.z - r->prev_velocity.z) / dt;
		}
	}
	r->prev_velocity = vel.linearVelocity;
	r->prev_motion_time = frame_time;
	// Differentiated velocity is noisy, smooth it a little but keep shakes sharp
	r->accel_filtered.x += (accel_world.x - r->accel_filtered.x) * 0.5f;
	r->accel_filtered.y += (accel_world.y - r->accel_filtered.y) * 0.5f;
	r->accel_filtered.z += (accel_world.z - r->accel_filtered.z) * 0.5f;
	// An accelerometer at rest reads 1 g upward
	XrVector3f felt = { r->accel_filtered.x, r->accel_filtered.y + GRAVITY, r->accel_filtered.z };
	XrVector3f accel = quat_rotate(qi, felt);

	(*env)->CallVoidMethod(env, r->callback, r->on_motion,
		gyro.x, gyro.y, gyro.z,
		accel.x / GRAVITY, accel.y / GRAVITY, accel.z / GRAVITY,
		q.x, q.y, q.z, q.w);
}

// Console rumble on the Touch controllers. Vibrations are short pulses renewed while it lasts.
static void rumble_update(XrRenderer *r)
{
	if(r->action_set == XR_NULL_HANDLE)
		return;
	double now = now_s();
	for(int i = 0; i < 2; i++)
	{
		int v = r->rumble[i];
		XrHapticActionInfo info = { XR_TYPE_HAPTIC_ACTION_INFO };
		info.action = r->actions[i == 0 ? A_LHAPTIC : A_RHAPTIC];
		if(v <= 0)
		{
			if(r->rumble_applied[i] > 0)
				xrStopHapticFeedback(r->session, &info);
			r->rumble_applied[i] = 0;
			continue;
		}
		if(v == r->rumble_applied[i] && now - r->rumble_time[i] < 0.08)
			continue;
		XrHapticVibration vib = { XR_TYPE_HAPTIC_VIBRATION };
		vib.duration = 120000000; // 120 ms, renewed every 80 ms while the console keeps rumbling
		vib.frequency = XR_FREQUENCY_UNSPECIFIED;
		vib.amplitude = (float)v / 255.0f;
		xrApplyHapticFeedback(r->session, &info, (XrHapticBaseHeader *)&vib);
		r->rumble_applied[i] = v;
		r->rumble_time[i] = now;
	}
}

static void poll_input(XrRenderer *r, JNIEnv *env)
{
	jint buttons = 0;
	jint l2 = 0, r2 = 0;
	jshort lx = 0, ly = 0, rx = 0, ry = 0;

	if(r->state == XR_SESSION_STATE_FOCUSED)
	{
		XrActiveActionSet active = { r->action_set, XR_NULL_PATH };
		XrActionsSyncInfo sync = { XR_TYPE_ACTIONS_SYNC_INFO };
		sync.countActiveActionSets = 1;
		sync.activeActionSets = &active;
		bool synced = xrSyncActions(r->session, &sync) == XR_SUCCESS;
		if(synced && handle_menu(r))
		{
			r->touch_mode = false;
			r->hold_clean = false;
			r->hold_progress = -1.0f;
			r->finger[0] = r->finger[1] = -1;
		}
		else if(synced)
		{
			bool menu = get_bool(r, A_MENU);
			static int dbg;
			if(++dbg % 90 == 0)
			{
				XrVector2f l = get_vec2(r, A_LSTICK), rr = get_vec2(r, A_RSTICK);
				LOGI("in: menu=%d A=%d X=%d L1=%.2f R1=%.2f L2=%.2f R2=%.2f ls=%.2f,%.2f rs=%.2f,%.2f",
					menu, get_bool(r, A_CROSS), get_bool(r, A_BOX), get_float(r, A_L1), get_float(r, A_R1),
					get_float(r, A_L2), get_float(r, A_R2), l.x, l.y, rr.x, rr.y);
			}
			bool cross = get_bool(r, A_CROSS);
			bool moon = get_bool(r, A_MOON);
			XrVector2f ls = get_vec2(r, A_LSTICK);
			XrVector2f rs = get_vec2(r, A_RSTICK);

			// Touchpad mode: Menu held alone for TOUCH_HOLD_S. Thumbs resting on the sticks are
			// fingers on the touchpad, stick position is the finger position, R3 clicks it.
			bool other = cross || moon || get_bool(r, A_BOX) || get_bool(r, A_PYRAMID)
				|| get_bool(r, A_L3) || get_bool(r, A_R3)
				|| get_float(r, A_L1) > 0.2f || get_float(r, A_R1) > 0.2f
				|| get_float(r, A_L2) > 0.2f || get_float(r, A_R2) > 0.2f
				|| fabsf(ls.x) > 0.3f || fabsf(ls.y) > 0.3f || fabsf(rs.x) > 0.3f || fabsf(rs.y) > 0.3f;
			r->hold_progress = -1.0f;
			if(menu && !r->menu_held)
			{
				r->menu_down_time = now_s();
				r->hold_clean = true;
			}
			if(!r->touch_mode && (other || !menu))
				r->hold_clean = false;
			if(menu && r->hold_clean && !r->touch_mode)
			{
				double held = now_s() - r->menu_down_time;
				if(held >= TOUCH_HOLD_S)
				{
					r->touch_mode = true;
					r->menu_combo = true; // no Options tap when Menu goes up
					LOGI("Touchpad mode on");
				}
				else if(held >= TOUCH_HINT_S)
					r->hold_progress = (float)((held - TOUCH_HINT_S) / (TOUCH_HOLD_S - TOUCH_HINT_S));
			}
			if(r->touch_mode && !menu)
			{
				r->touch_mode = false;
				r->finger[0] = r->finger[1] = -1;
				r->release_guard = true; // nothing reaches the console until everything is let go
				LOGI("Touchpad mode off");
			}
			if(r->touch_mode)
			{
				r->finger[0] = get_bool(r, A_RTOUCH) ? pack_finger(rs) : -1;
				r->finger[1] = get_bool(r, A_LTOUCH) ? pack_finger(ls) : -1;
				if(get_bool(r, A_R3))
					buttons |= BTN_TOUCHPAD;
				r->menu_held = menu;
				goto send;
			}

			// Menu is a modifier: Menu + A = PS, Menu + B = Share, Menu + R3 = touchpad click, Menu + right stick = D-pad.
			// Options is only sent as a tap when Menu is released without any combo.
			if(menu)
			{
				if(!r->menu_held)
					r->menu_combo = false;
				if(cross) { buttons |= BTN_PS; r->menu_combo = true; }
				if(moon) { buttons |= BTN_SHARE; r->menu_combo = true; }
				if(get_bool(r, A_R3)) { buttons |= BTN_TOUCHPAD; r->menu_combo = true; }
				if(get_bool(r, A_L3)) { buttons |= BTN_SHAKE; r->menu_combo = true; }
				if(rs.y > DPAD_THRESHOLD) { buttons |= BTN_DPAD_UP; r->menu_combo = true; }
				if(rs.y < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_DOWN; r->menu_combo = true; }
				if(rs.x < -DPAD_THRESHOLD) { buttons |= BTN_DPAD_LEFT; r->menu_combo = true; }
				if(rs.x > DPAD_THRESHOLD) { buttons |= BTN_DPAD_RIGHT; r->menu_combo = true; }
				rs.x = rs.y = 0.0f; // the stick is the D-pad while Menu is held
			}
			else
			{
				if(r->menu_held && !r->menu_combo)
					r->options_frames = OPTIONS_TAP_FRAMES;
				if(cross) buttons |= BTN_CROSS;
				if(moon) buttons |= BTN_MOON;
			}
			r->menu_held = menu;
			if(r->options_frames > 0)
			{
				buttons |= BTN_OPTIONS;
				r->options_frames--;
			}
			if(get_bool(r, A_PYRAMID)) buttons |= BTN_PYRAMID;
			if(get_bool(r, A_BOX)) buttons |= BTN_BOX;
			if(get_bool(r, A_L3) && !menu) buttons |= BTN_L3; // Menu + L3 shakes the controller
			if(get_bool(r, A_R3) && !menu) buttons |= BTN_R3; // Menu + R3 is the touchpad click
			// While Menu is held the shoulders and triggers belong to the secret menu combo,
			// so the console never sees them
			if(!menu)
			{
				if(get_float(r, A_L1) > 0.5f) buttons |= BTN_L1;
				if(get_float(r, A_R1) > 0.5f) buttons |= BTN_R1;
				l2 = (jint)(get_float(r, A_L2) * 255.0f);
				r2 = (jint)(get_float(r, A_R2) * 255.0f);
			}
			else if(get_float(r, A_L1) > 0.5f || get_float(r, A_R1) > 0.5f
					|| get_float(r, A_L2) > 0.5f || get_float(r, A_R2) > 0.5f)
				r->menu_combo = true; // no Options tap on release either
			// OpenXR y is up, DualSense y is down
			lx = stick_axis(ls.x); ly = stick_axis(-ls.y);
			rx = stick_axis(rs.x); ry = stick_axis(-rs.y);
		}
	}

send:;
	jint f0 = r->touch_mode ? r->finger[0] : -1;
	jint f1 = r->touch_mode ? r->finger[1] : -1;
	if(r->mode != MODE_NONE || r->release_guard)
	{
		buttons = BTN_MENU_OPEN; // neutral pad, and tells Kotlin to ignore Android gamepad events
		f0 = f1 = -1;
	}
	(*env)->CallVoidMethod(env, r->callback, r->on_input, buttons, l2, r2, lx, ly, rx, ry, f0, f1);
}

static void handle_state(XrRenderer *r, XrSessionState state)
{
	r->state = state;
	LOGI("Session state %d", (int)state);
	switch(state)
	{
		case XR_SESSION_STATE_READY:
		{
			XrSessionBeginInfo begin = { XR_TYPE_SESSION_BEGIN_INFO };
			begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
			if(XR_SUCCEEDED(xrBeginSession(r->session, &begin)))
				r->running = true;
			break;
		}
		case XR_SESSION_STATE_STOPPING:
			xrEndSession(r->session);
			r->running = false;
			break;
		case XR_SESSION_STATE_EXITING:
		case XR_SESSION_STATE_LOSS_PENDING:
			r->quit = true;
			break;
		default:
			break;
	}
}

static void *xr_thread(void *user)
{
	XrRenderer *r = user;
	JNIEnv *env = NULL;
	(*r->vm)->AttachCurrentThread(r->vm, &env, NULL);
	bool exit_requested_by_runtime = false;

	if(!egl_init(r))
	{
		LOGE("EGL init failed");
		goto fail;
	}

	PFN_xrInitializeLoaderKHR init_loader = NULL;
	XR_CHECK(xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&init_loader));
	XrLoaderInitInfoAndroidKHR loader_info = { XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR };
	loader_info.applicationVM = r->vm;
	loader_info.applicationContext = r->activity;
	XR_CHECK(init_loader((XrLoaderInitInfoBaseHeaderKHR *)&loader_info));

	// The decoder writes the surface top row first, which the compositor shows upside down;
	// XR_FB_composition_layer_image_layout flips it, otherwise the quad is turned around instead
	uint32_t ext_count = 0;
	xrEnumerateInstanceExtensionProperties(NULL, 0, &ext_count, NULL);
	XrExtensionProperties *ext_props = calloc(ext_count ? ext_count : 1, sizeof(XrExtensionProperties));
	for(uint32_t i = 0; i < ext_count; i++)
		ext_props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
	xrEnumerateInstanceExtensionProperties(NULL, ext_count, &ext_count, ext_props);
	for(uint32_t i = 0; i < ext_count; i++)
	{
		if(!strcmp(ext_props[i].extensionName, XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME))
			r->flip_ext = true;
		else if(!strcmp(ext_props[i].extensionName, XR_FB_PASSTHROUGH_EXTENSION_NAME))
			r->passthrough_ext = true;
		else if(!strcmp(ext_props[i].extensionName, XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME))
			r->equirect_ext = true;
	}
	free(ext_props);
	LOGI("Extensions: image layout flip %d, passthrough %d, equirect2 %d", r->flip_ext, r->passthrough_ext, r->equirect_ext);
	const char *exts[6] = {
		XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
		XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
		XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
	};
	uint32_t ext_used = 3;
	if(r->flip_ext) exts[ext_used++] = XR_FB_COMPOSITION_LAYER_IMAGE_LAYOUT_EXTENSION_NAME;
	if(r->passthrough_ext) exts[ext_used++] = XR_FB_PASSTHROUGH_EXTENSION_NAME;
	if(r->equirect_ext) exts[ext_used++] = XR_KHR_COMPOSITION_LAYER_EQUIRECT2_EXTENSION_NAME;
	XrInstanceCreateInfoAndroidKHR android_info = { XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR };
	android_info.applicationVM = r->vm;
	android_info.applicationActivity = r->activity;
	XrInstanceCreateInfo inst_info = { XR_TYPE_INSTANCE_CREATE_INFO };
	inst_info.next = &android_info;
	strcpy(inst_info.applicationInfo.applicationName, "Chiaki Immersive");
	inst_info.applicationInfo.apiVersion = XR_API_VERSION_1_0;
	inst_info.enabledExtensionCount = ext_used;
	inst_info.enabledExtensionNames = exts;
	XR_CHECK(xrCreateInstance(&inst_info, &r->instance));

	XrSystemGetInfo sys_info = { XR_TYPE_SYSTEM_GET_INFO };
	sys_info.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
	XR_CHECK(xrGetSystem(r->instance, &sys_info, &r->system));

	PFN_xrGetOpenGLESGraphicsRequirementsKHR get_gles_req = NULL;
	XR_CHECK(xrGetInstanceProcAddr(r->instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&get_gles_req));
	XrGraphicsRequirementsOpenGLESKHR gles_req = { XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR };
	XR_CHECK(get_gles_req(r->instance, r->system, &gles_req));

	XrGraphicsBindingOpenGLESAndroidKHR binding = { XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR };
	binding.display = r->egl_display;
	binding.config = r->egl_config;
	binding.context = r->egl_context;
	XrSessionCreateInfo sess_info = { XR_TYPE_SESSION_CREATE_INFO };
	sess_info.next = &binding;
	sess_info.systemId = r->system;
	XR_CHECK(xrCreateSession(r->instance, &sess_info, &r->session));

	XrReferenceSpaceCreateInfo space_info = { XR_TYPE_REFERENCE_SPACE_CREATE_INFO };
	space_info.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	space_info.poseInReferenceSpace.orientation.w = 1.0f;
	XR_CHECK(xrCreateReferenceSpace(r->session, &space_info, &r->space));

	if(!input_init(r))
		LOGE("Controller input setup failed, only physical gamepads will work");
	if(!menu_init(r, env))
		LOGE("Secret menu setup failed");

	passthrough_init(r);
	r->pano_count = (*env)->CallIntMethod(env, r->callback, r->panorama_count);
	r->room_count = (*env)->CallIntMethod(env, r->callback, r->room_count_m);
	r->scene_count = SCENE_PANORAMA0 + r->pano_count + r->room_count;
#ifdef CHIAKI_ROOMS
	r->room_loaded = -1;
#endif
	r->scene = (*env)->CallIntMethod(env, r->callback, r->saved_scene);
	if(r->scene < 0 || r->scene >= r->scene_count)
		r->scene = SCENE_BLACK;
	r->scene_applied = -1;
	r->pano_loaded = -1;

	PFN_xrCreateSwapchainAndroidSurfaceKHR create_surface_swapchain = NULL;
	XR_CHECK(xrGetInstanceProcAddr(r->instance, "xrCreateSwapchainAndroidSurfaceKHR", (PFN_xrVoidFunction *)&create_surface_swapchain));
	XrSwapchainCreateInfo sc_info = { XR_TYPE_SWAPCHAIN_CREATE_INFO };
	sc_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	sc_info.width = (uint32_t)r->width;
	sc_info.height = (uint32_t)r->height;
	sc_info.sampleCount = 1;
	sc_info.faceCount = 1;
	sc_info.arraySize = 1;
	sc_info.mipCount = 1;
	jobject surface = NULL;
	// Runtimes disagree on what a surface swapchain may carry; try the known variants in turn
	XrResult sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
	LOGI("surface swapchain (sampled|color, counts 1): %d", sc_res);
	if(XR_FAILED(sc_res))
	{
		sc_info.usageFlags = 0;
		sc_info.sampleCount = sc_info.faceCount = sc_info.arraySize = sc_info.mipCount = 0;
		sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
		LOGI("surface swapchain (no usage, counts 0): %d", sc_res);
	}
	if(XR_FAILED(sc_res))
	{
		sc_info.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
		sc_info.format = 0x8058; // GL_RGBA8
		sc_info.sampleCount = sc_info.faceCount = sc_info.arraySize = sc_info.mipCount = 1;
		sc_res = create_surface_swapchain(r->session, &sc_info, &r->swapchain, &surface);
		LOGI("surface swapchain (RGBA8, sampled): %d", sc_res);
	}
	XR_CHECK(sc_res);
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, surface);
	// The runtime owns this reference (Meta hands out a global ref); don't delete it
	LOGI("Surface swapchain %dx%d handed to the decoder", r->width, r->height);

	while(!r->quit)
	{
		XrEventDataBuffer ev = { XR_TYPE_EVENT_DATA_BUFFER };
		while(xrPollEvent(r->instance, &ev) == XR_SUCCESS)
		{
			if(ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
			{
				XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
				if(s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING)
					exit_requested_by_runtime = true;
				handle_state(r, s);
			}
			else if(ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING)
			{
				exit_requested_by_runtime = true;
				r->quit = true;
			}
			ev.type = XR_TYPE_EVENT_DATA_BUFFER;
		}

		if(!r->running)
		{
			poll_input(r, env); // reports a neutral state while unfocused
			usleep(20000);
			continue;
		}

		XrFrameWaitInfo wait_info = { XR_TYPE_FRAME_WAIT_INFO };
		XrFrameState frame_state = { XR_TYPE_FRAME_STATE };
		if(XR_FAILED(xrWaitFrame(r->session, &wait_info, &frame_state)))
			continue;
		XrFrameBeginInfo begin_info = { XR_TYPE_FRAME_BEGIN_INFO };
		xrBeginFrame(r->session, &begin_info);

		frame_time = frame_state.predictedDisplayTime;
		poll_input(r, env);
		motion_update(r, env);
		rumble_update(r);
		scene_apply(r, env);

		XrCompositionLayerImageLayoutFB layout = { XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB };
		layout.flags = XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB;
		XrCompositionLayerQuad quad = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		quad.space = r->space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
		quad.subImage.swapchain = r->swapchain;
		quad.subImage.imageRect.extent.width = r->width;
		quad.subImage.imageRect.extent.height = r->height;
		quad.pose = r->screen_pose;
		if(r->flip_ext)
			quad.next = &layout;
		else
		{
			// Seen from behind after half a turn around X: a pure vertical flip
			XrQuaternionf flip = { 1.0f, 0.0f, 0.0f, 0.0f };
			quad.pose.orientation = quat_mul(quad.pose.orientation, flip);
		}
		quad.size.width = r->screen_width;
		quad.size.height = r->screen_width * (float)r->height / (float)r->width;
		const XrCompositionLayerBaseHeader *layers[3];
		uint32_t layer_count = 0;

		XrCompositionLayerPassthroughFB pt_layer = { XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB };
		XrCompositionLayerEquirect2KHR pano = { XR_TYPE_COMPOSITION_LAYER_EQUIRECT2_KHR };
#ifdef CHIAKI_ROOMS
		XrCompositionLayerProjection proj = { XR_TYPE_COMPOSITION_LAYER_PROJECTION };
		XrCompositionLayerProjectionView proj_views[2];
		if(r->room && frame_state.shouldRender && room_render(r, frame_state.predictedDisplayTime, proj_views))
		{
			proj.space = r->space;
			proj.viewCount = 2;
			proj.views = proj_views;
			layers[layer_count++] = (XrCompositionLayerBaseHeader *)&proj;
		}
		else
#endif
		if(r->scene_applied == SCENE_PASSTHROUGH && r->passthrough_running)
		{
			pt_layer.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			pt_layer.layerHandle = r->passthrough_layer;
			layers[layer_count++] = (XrCompositionLayerBaseHeader *)&pt_layer;
		}
		else if(r->scene_applied >= SCENE_PANORAMA0 && !scene_is_room(r, r->scene_applied) && r->pano_swapchain != XR_NULL_HANDLE)
		{
			pano.space = r->space;
			pano.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			pano.subImage.swapchain = r->pano_swapchain;
			pano.subImage.imageRect.extent.width = r->pano_w;
			pano.subImage.imageRect.extent.height = r->pano_h;
			pano.pose.orientation.w = 1.0f;
			pano.radius = 0.0f; // infinitely far away
			pano.centralHorizontalAngle = 2.0f * (float)M_PI;
			pano.upperVerticalAngle = 0.5f * (float)M_PI;
			pano.lowerVerticalAngle = -0.5f * (float)M_PI;
			layers[layer_count++] = (XrCompositionLayerBaseHeader *)&pano;
		}
		layers[layer_count++] = (XrCompositionLayerBaseHeader *)&quad;

		XrCompositionLayerQuad menu = { XR_TYPE_COMPOSITION_LAYER_QUAD };
		// Over the screen while no menu is open: touchpad overlay first, then the notice
		int notice = r->notice;
		int ext_finger[2] = { r->ext_finger[0], r->ext_finger[1] };
		float ext_progress = r->ext_progress;
		int ovl_img = -1;
		float ovl_progress = -1.0f;
		const int *ovl_fingers = NULL;
		if(r->mode == MODE_NONE)
		{
			if(r->touch_mode) { ovl_img = IMG_TOUCHPAD; ovl_fingers = r->finger; }
			else if(r->hold_progress >= 0.0f) { ovl_img = IMG_TOUCH_HOLD; ovl_progress = r->hold_progress; }
			else if(r->ext_active) { ovl_img = IMG_TOUCHPAD; ovl_fingers = ext_finger; }
			else if(ext_progress >= 0.0f) { ovl_img = IMG_TOUCH_HOLD; ovl_progress = ext_progress > 1.0f ? 1.0f : ext_progress; }
			else if(notice >= 0 && notice < MENU_IMG_COUNT) ovl_img = notice;
		}
		bool show_notice = ovl_img >= 0;
		if((r->mode != MODE_NONE || show_notice) && frame_state.shouldRender && r->menu_swapchain != XR_NULL_HANDLE)
		{
			int img = show_notice ? ovl_img : r->mode == MODE_RESIZE ? 5 : r->mode == MODE_MOVE ? 6 : r->menu_dir;
			menu_draw_ex(r, img, show_notice ? ovl_progress : -1.0f, show_notice ? ovl_fingers : NULL);
			menu.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
			menu.space = r->space;
			menu.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
			menu.subImage.swapchain = r->menu_swapchain;
			menu.subImage.imageRect.extent.width = MENU_IMG_SIZE;
			menu.subImage.imageRect.extent.height = MENU_IMG_SIZE;
			menu.pose = r->menu_pose;
			menu.size.width = menu.size.height = MENU_SIZE_M;
			if(show_notice)
			{
				// Centred on the virtual screen, composited on top of it
				menu.pose = r->screen_pose;
				menu.size.width = menu.size.height = r->screen_width * 0.45f;
			}
			layers[layer_count++] = (XrCompositionLayerBaseHeader *)&menu;
		}

		XrFrameEndInfo end_info = { XR_TYPE_FRAME_END_INFO };
		end_info.displayTime = frame_state.predictedDisplayTime;
		end_info.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
		end_info.layerCount = frame_state.shouldRender ? layer_count : 0;
		end_info.layers = layers;
		xrEndFrame(r->session, &end_info);
	}

fail:
	(*env)->CallVoidMethod(env, r->callback, r->on_surface, NULL);
	if(r->swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->swapchain);
	if(r->menu_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->menu_swapchain);
	if(r->rpose_space != XR_NULL_HANDLE) xrDestroySpace(r->rpose_space);
#ifdef CHIAKI_ROOMS
	if(r->room) room_destroy(r->room);
	for(int e = 0; e < 2; e++)
		if(r->eye_swapchain[e] != XR_NULL_HANDLE) xrDestroySwapchain(r->eye_swapchain[e]);
#endif
	if(r->pano_swapchain != XR_NULL_HANDLE) xrDestroySwapchain(r->pano_swapchain);
	if(r->passthrough_layer != XR_NULL_HANDLE && r->pt_layer_destroy) r->pt_layer_destroy(r->passthrough_layer);
	if(r->passthrough != XR_NULL_HANDLE && r->pt_destroy) r->pt_destroy(r->passthrough);
	if(r->view_space != XR_NULL_HANDLE) xrDestroySpace(r->view_space);
	if(r->action_set != XR_NULL_HANDLE) xrDestroyActionSet(r->action_set);
	if(r->space != XR_NULL_HANDLE) xrDestroySpace(r->space);
	if(r->session != XR_NULL_HANDLE) xrDestroySession(r->session);
	if(r->instance != XR_NULL_HANDLE) xrDestroyInstance(r->instance);
	egl_fini(r);
	// Tell the activity to finish unless it asked us to stop in the first place
	if(exit_requested_by_runtime || !r->quit || r->user_exit)
		(*env)->CallVoidMethod(env, r->callback, r->on_exit);
	(*r->vm)->DetachCurrentThread(r->vm);
	return NULL;
}

JNIEXPORT jlong JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeStart(JNIEnv *env, jobject obj, jobject activity, jint width, jint height)
{
	XrRenderer *r = calloc(1, sizeof(XrRenderer));
	if(!r)
		return 0;
	(*env)->GetJavaVM(env, &r->vm);
	r->activity = (*env)->NewGlobalRef(env, activity);
	r->callback = (*env)->NewGlobalRef(env, obj);
	jclass cls = (*env)->GetObjectClass(env, obj);
	r->on_surface = (*env)->GetMethodID(env, cls, "onSurface", "(Landroid/view/Surface;)V");
	r->on_input = (*env)->GetMethodID(env, cls, "onInput", "(IIISSSSII)V");
	r->on_exit = (*env)->GetMethodID(env, cls, "onExit", "()V");
	r->menu_image = (*env)->GetMethodID(env, cls, "menuImage", "(II)[B");
	r->panorama_count = (*env)->GetMethodID(env, cls, "panoramaCount", "()I");
	r->on_motion = (*env)->GetMethodID(env, cls, "onMotion", "(FFFFFFFFFF)V");
	r->room_count_m = (*env)->GetMethodID(env, cls, "roomCount", "()I");
	r->room_data_m = (*env)->GetMethodID(env, cls, "roomData", "(I)[B");
	r->panorama_image = (*env)->GetMethodID(env, cls, "panoramaImage", "(I)[B");
	r->saved_scene = (*env)->GetMethodID(env, cls, "savedScene", "()I");
	r->save_scene = (*env)->GetMethodID(env, cls, "saveScene", "(I)V");
	r->width = width;
	r->height = height;
	r->screen_pose.orientation.w = 1.0f;
	r->screen_pose.position.z = -SCREEN_DISTANCE_M;
	r->screen_width = SCREEN_WIDTH_M;
	r->notice = -1;
	r->hold_progress = -1.0f;
	r->finger[0] = r->finger[1] = -1;
	r->ext_progress = -1.0f;
	r->ext_finger[0] = r->ext_finger[1] = -1;
	r->screen_distance = SCREEN_DISTANCE_M;
	r->egl_display = EGL_NO_DISPLAY;
	r->egl_context = EGL_NO_CONTEXT;
	r->egl_surface = EGL_NO_SURFACE;
	if(pthread_create(&r->thread, NULL, xr_thread, r) != 0)
	{
		(*env)->DeleteGlobalRef(env, r->activity);
		(*env)->DeleteGlobalRef(env, r->callback);
		free(r);
		return 0;
	}
	return (jlong)r;
}

JNIEXPORT void JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeSetNotice(JNIEnv *env, jobject obj, jlong ptr, jint img)
{
	XrRenderer *r = (XrRenderer *)ptr;
	if(r)
		r->notice = img;
}

JNIEXPORT void JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeSetTouchOverlay(JNIEnv *env, jobject obj, jlong ptr, jfloat progress, jboolean active, jint f0, jint f1)
{
	XrRenderer *r = (XrRenderer *)ptr;
	if(!r)
		return;
	r->ext_finger[0] = f0;
	r->ext_finger[1] = f1;
	r->ext_progress = progress;
	r->ext_active = active;
}

JNIEXPORT void JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeSetRumble(JNIEnv *env, jobject obj, jlong ptr, jint left, jint right)
{
	XrRenderer *r = (XrRenderer *)ptr;
	if(!r)
		return;
	r->rumble[0] = left;
	r->rumble[1] = right;
}

JNIEXPORT void JNICALL Java_com_metallic_chiaki_stream_XrRenderer_nativeStop(JNIEnv *env, jobject obj, jlong ptr)
{
	XrRenderer *r = (XrRenderer *)ptr;
	if(!r)
		return;
	r->quit = true;
	pthread_join(r->thread, NULL);
	(*env)->DeleteGlobalRef(env, r->activity);
	(*env)->DeleteGlobalRef(env, r->callback);
	free(r);
}
