/* Musor — aim assist for DDNet on Android.
 * aarch64, plain C, no libc++ dependency.
 *
 * Loaded through a DT_NEEDED entry added to libDDNet.so, so it initialises with
 * the game library, before any Java code runs. The hook replaces the three
 * instructions of CGameClient::OnSnapInput that tail-call CControls::SnapInput,
 * which hands us the CGameClient pointer and the raw input array. After the
 * game has built the honest input we rewrite only the aim fields.
 *
 * Layout constants below were read out of libDDNet.so 20.2.1 (arm64):
 *   CGameClient::m_Controls        this + 0x520bd0
 *   CGameClient::m_aLocalIds       this + 0x7092f4
 *   CGameClient::m_LocalCharacterPos this + 0x53d750
 *   CGameClient::m_aClients        this + 0x54aec0   (stride 0x3770)
 *     CClientData::m_Team          +0x50
 *     CClientData::m_Predicted.m_Pos +0x90  (m_Vel follows at +0x98)
 */

#include <stdarg.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <link.h>
#include <android/log.h>

#define TAG "musor"

#define OFF_ONSNAP_TAIL 0xb3ec60u
#define OFF_SNAP_INPUT 0xa50e6cu
#define OFF_M_CONTROLS 0x520bd0u
#define OFF_M_LOCAL_IDS 0x7092f4u
#define OFF_LOCAL_POS 0x53d750u
#define OFF_A_CLIENTS 0x54aec0u
#define OFF_CONFIG_PTR 0xcc2fe0u
#define OFF_CLDummy 3688
#define STRIDE_CLIENT 0x3770u
#define OFF_TEAM 0x50u
#define OFF_PRED_POS 0x90u
#define MAX_CLIENTS 64

/* CNetObj_PlayerInput, int fields */
enum
{
	IN_DIRECTION = 0,
	IN_TARGET_X,
	IN_TARGET_Y,
	IN_JUMP,
	IN_FIRE,
	IN_HOOK,
	IN_FLAGS,
	IN_WANTED,
	IN_NEXT,
	IN_PREV,
	IN_NUM,
};

typedef struct
{
	float x, y;
} vec2;

static uintptr_t g_base;
static int (*g_snap_input)(void *self, int *pData);
static int g_patched;

/* ---------------------------------------------------------------- settings */

static struct SSettings
{
	int aim;
	int fov;
	int pred;
	int team;
	int smooth;
	int trigger;
	int trig_deg;
	int verbose;
	float gun_speed, gun_curv;
	float sg_speed, sg_curv;
	float gr_speed, gr_curv;
} g_cfg = {
	1,      /* aim */
	1100,   /* fov */
	1,      /* pred */
	1,      /* team filter */
	0,      /* smooth */
	0,      /* trigger */
	9,      /* trigger tolerance, degrees */
	0,      /* verbose */
	2200.f, 1.25f,
	2750.f, 1.25f,
	1000.f, 7.0f,
};

/* ------------------------------------------------------------------ output */

#define MAX_SINKS 4

static FILE *g_sinks[MAX_SINKS];
static int g_nsinks;
static int64_t g_last_note;
static void note(const char *fmt, ...);

/* every place the log and the config are looked for */
static const char *const g_dirs[] = {
	"/sdcard",
	"/storage/emulated/0/Android/data/org.ddnet.client/files",
	"/data/user/0/org.ddnet.client/files",
	"/data/data/org.ddnet.client/files",
	NULL,
};

static void mkdirs(const char *path)
{
	char tmp[256];
	const size_t n = strlen(path);
	if(n == 0 || n >= sizeof(tmp))
		return;
	memcpy(tmp, path, n + 1);
	for(char *p = tmp + 1; *p; p++)
	{
		if(*p != '/')
			continue;
		*p = 0;
		mkdir(tmp, 0770);
		*p = '/';
	}
	mkdir(tmp, 0770);
}

static void log_open(const char *dir)
{
	if(g_nsinks == MAX_SINKS)
		return;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s/musor.log", dir);
	FILE *f = fopen(buf, "a");
	if(!f)
		return;
	setvbuf(f, NULL, _IOLBF, 0);
	g_sinks[g_nsinks++] = f;
}

static void paths_open(void)
{
	mkdirs("/storage/emulated/0/Android/data/org.ddnet.client/files");
	mkdirs("/data/user/0/org.ddnet.client/files");
	for(int i = 0; g_dirs[i]; i++)
		log_open(g_dirs[i]);
	note("musor: %d log sink(s)", g_nsinks);
}

static void note(const char *fmt, ...)
{
	char buf[512];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	__android_log_print(ANDROID_LOG_INFO, TAG, "%s", buf);
	for(int i = 0; i < g_nsinks; i++)
	{
		fputs(buf, g_sinks[i]);
		fputc('\n', g_sinks[i]);
	}
}

static void config_read(void)
{
	char buf[512];
	for(int i = 0; g_dirs[i]; i++)
	{
		snprintf(buf, sizeof(buf), "%s/musor.cfg", g_dirs[i]);
		FILE *f = fopen(buf, "r");
		if(!f)
			continue;
		char line[128];
		while(fgets(line, sizeof(line), f))
		{
			char key[64];
			char *eq = strpbrk(line, "=:");
			if(!eq)
				continue;
			size_t klen = (size_t)(eq - line);
			while(klen && (line[klen - 1] == ' ' || line[klen - 1] == '\t'))
				klen--;
			if(klen == 0 || klen >= sizeof(key))
				continue;
			memcpy(key, line, klen);
			key[klen] = 0;
			const float val = strtof(eq + 1, NULL);
			if(!strcmp(key, "aim")) g_cfg.aim = (int)val;
			else if(!strcmp(key, "fov")) g_cfg.fov = (int)val;
			else if(!strcmp(key, "pred")) g_cfg.pred = (int)val;
			else if(!strcmp(key, "team")) g_cfg.team = (int)val;
			else if(!strcmp(key, "smooth")) g_cfg.smooth = (int)val;
			else if(!strcmp(key, "trigger")) g_cfg.trigger = (int)val;
			else if(!strcmp(key, "trigger_deg")) g_cfg.trig_deg = (int)val;
			else if(!strcmp(key, "verbose")) g_cfg.verbose = (int)val;
			else if(!strcmp(key, "gun_speed")) g_cfg.gun_speed = val;
			else if(!strcmp(key, "gun_curv")) g_cfg.gun_curv = val;
			else if(!strcmp(key, "shotgun_speed")) g_cfg.sg_speed = val;
			else if(!strcmp(key, "shotgun_curv")) g_cfg.sg_curv = val;
			else if(!strcmp(key, "grenade_speed")) g_cfg.gr_speed = val;
			else if(!strcmp(key, "grenade_curv")) g_cfg.gr_curv = val;
		}
		fclose(f);
		note("musor: config %s", buf);
		break;
	}
}

/* ------------------------------------------------------------- code writing */

static int code_write(void *at, const void *src, size_t len)
{
	uintptr_t page = (uintptr_t)at & ~(uintptr_t)4095;
	size_t span = ((uintptr_t)at + len - page + 4095) & ~(size_t)4095;
	if(mprotect((void *)page, span, PROT_READ | PROT_WRITE | PROT_EXEC) == 0)
	{
		memcpy(at, src, len);
		__builtin___clear_cache((char *)at, (char *)at + len);
		mprotect((void *)page, span, PROT_READ | PROT_EXEC);
		return 0;
	}
	/* fallback: the kernel lets us write our own read-only private mappings through here */
	int fd = open("/proc/self/mem", O_RDWR);
	if(fd < 0)
		return -1;
	ssize_t n = pwrite(fd, src, len, (off_t)(uintptr_t)at);
	close(fd);
	if(n != (ssize_t)len)
		return -1;
	__builtin___clear_cache((char *)at, (char *)at + len);
	return 0;
}

/* adrp x17, page(target) ; add x17, x17, #lo12(target) ; br x17 */
static int patch_branch12(void *at, void *target)
{
	uintptr_t pc = (uintptr_t)at;
	uintptr_t tp = (uintptr_t)target;
	int64_t delta = (int64_t)(tp & ~(uintptr_t)4095) - (int64_t)(pc & ~(uintptr_t)4095);
	if(delta < -(1LL << 32) || delta > (1LL << 32))
		return -1;
	uint64_t pages = (uint64_t)(delta >> 12);
	uint32_t immlo = (uint32_t)(pages & 3);
	uint32_t immhi = (uint32_t)((pages >> 2) & 0x7ffff);
	uint32_t adrp = 0x90000000u | (immlo << 29) | (immhi << 5) | 17u;
	uint32_t add = 0x91000000u | (((uint32_t)(tp & 0xfff)) << 10) | (17u << 5) | 17u;
	uint32_t br = 0xd61f0000u | (17u << 5);
	uint32_t code[3] = {adrp, add, br};
	return code_write(at, code, sizeof(code));
}

/* ------------------------------------------------------------------- aiming */

static int solve_traj(vec2 from, vec2 tpos, vec2 tvel, float speed, float curv, float *out_angle, float *out_time)
{
	if(speed <= 1.0f)
		return 0;
	const float step = 0.01f;
	float prev_t = step, prev = 0.0f;
	int have_prev = 0;
	for(float t = step; t < 2.0f; t += step)
	{
		const vec2 aim = {tpos.x + tvel.x * t, tpos.y + tvel.y * t};
		const float reach = t * speed;
		const float drop = curv / 10000.0f * reach * reach;
		const vec2 d = {aim.x - from.x, aim.y - from.y - drop};
		const float miss = sqrtf(d.x * d.x + d.y * d.y) - reach;
		if(have_prev && prev > 0.0f && miss <= 0.0f)
		{
			float lo = prev_t, hi = t;
			for(int i = 0; i < 24; i++)
			{
				const float mid = 0.5f * (lo + hi);
				const vec2 a = {tpos.x + tvel.x * mid, tpos.y + tvel.y * mid};
				const float r = mid * speed;
				const float dr = curv / 10000.0f * r * r;
				const vec2 dd = {a.x - from.x, a.y - from.y - dr};
				if(sqrtf(dd.x * dd.x + dd.y * dd.y) - r > 0.0f)
					lo = mid;
				else
					hi = mid;
			}
			const float t_hit = 0.5f * (lo + hi);
			const vec2 a = {tpos.x + tvel.x * t_hit, tpos.y + tvel.y * t_hit};
			const float r = t_hit * speed;
			const float dr = curv / 10000.0f * r * r;
			*out_angle = atan2f(a.y - from.y - dr, a.x - from.x);
			*out_time = t_hit;
			return 1;
		}
		prev_t = t;
		prev = miss;
		have_prev = 1;
	}
	return 0;
}

static float angle_deg_diff(float a, float b)
{
	float d = fabsf(a - b) * 57.2957795f;
	d = fmodf(d, 360.0f);
	return d > 180.0f ? 360.0f - d : d;
}

static float g_last_angle;

static void aim_apply(int *p)
{
	const uint8_t *b = (const uint8_t *)g_base;
	const void *cfg = *(void *const *)(b + OFF_CONFIG_PTR);
	int dummy_idx = 0;
	if(cfg)
	{
		const int cd = *(const int *)((const uint8_t *)cfg + OFF_CLDummy);
		if(cd == 0 || cd == 1)
			dummy_idx = cd;
	}
	const int32_t *local_ids = (const int32_t *)(b + OFF_M_LOCAL_IDS);
	int me = local_ids[dummy_idx];
	if(me < 0 || me >= MAX_CLIENTS)
		me = local_ids[0];
	if(me < 0 || me >= MAX_CLIENTS)
		return;

	const vec2 from = *(const vec2 *)(b + OFF_LOCAL_POS);
	if(from.x == 0.0f && from.y == 0.0f)
		return;

	const uint8_t *clients = b + OFF_A_CLIENTS;
	const uint8_t *self = clients + (size_t)me * STRIDE_CLIENT;
	const int my_team = *(const int *)(self + OFF_TEAM);

	int best = -1;
	float best_dist = (float)g_cfg.fov;
	const int own_team = my_team > 0 ? my_team : -999;
	for(int i = 0; i < MAX_CLIENTS; i++)
	{
		if(i == me)
			continue;
		const uint8_t *c = clients + (size_t)i * STRIDE_CLIENT;
		if(g_cfg.team && *(const int *)(c + OFF_TEAM) == own_team)
			continue;
		const vec2 pos = *(const vec2 *)(c + OFF_PRED_POS);
		if(pos.x == 0.0f && pos.y == 0.0f)
			continue;
		const float dx = pos.x - from.x, dy = pos.y - from.y;
		const float dist = sqrtf(dx * dx + dy * dy);
		if(dist < 1.0f || dist >= best_dist)
			continue;
		best = i;
		best_dist = dist;
	}
	if(best < 0)
		return;

	const vec2 tpos = *(const vec2 *)(clients + (size_t)best * STRIDE_CLIENT + OFF_PRED_POS);
	const vec2 tvel = *(const vec2 *)(clients + (size_t)best * STRIDE_CLIENT + OFF_PRED_POS + 8);

	const int weapon = p[IN_WANTED] - 1;
	float speed = 0.0f, curv = 0.0f;
	switch(weapon)
	{
	case 1: /* WEAPON_GUN */
		speed = g_cfg.gun_speed;
		curv = g_cfg.gun_curv;
		break;
	case 2: /* WEAPON_SHOTGUN */
		speed = g_cfg.sg_speed;
		curv = g_cfg.sg_curv;
		break;
	case 3: /* WEAPON_GRENADE */
		speed = g_cfg.gr_speed;
		curv = g_cfg.gr_curv;
		break;
	default:
		break;
	}

	const float direct = atan2f(tpos.y - from.y, tpos.x - from.x);
	float angle = direct;
	float t_hit = 0.0f;
	if(g_cfg.pred && speed > 1.0f && solve_traj(from, tpos, tvel, speed, curv, &angle, &t_hit))
	{
	}
	else if(g_cfg.pred && speed > 1.0f)
	{
		const float t = best_dist / speed;
		const vec2 aim = {tpos.x + tvel.x * t, tpos.y + tvel.y * t};
		angle = atan2f(aim.y - from.y, aim.x - from.x);
		t_hit = t;
	}

	if(g_cfg.smooth > 0)
	{
		const float step = (float)g_cfg.smooth / 57.2957795f;
		float d = angle - g_last_angle;
		while(d > 3.14159265f)
			d -= 6.2831853f;
		while(d < -3.14159265f)
			d += 6.2831853f;
		if(d > step)
			angle = g_last_angle + step;
		else if(d < -step)
			angle = g_last_angle - step;
	}
	g_last_angle = angle;

	p[IN_TARGET_X] = (int)lroundf(cosf(angle) * 256.0f);
	p[IN_TARGET_Y] = (int)lroundf(sinf(angle) * 256.0f);

	if(g_cfg.trigger && angle_deg_diff(angle, direct) <= (float)g_cfg.trig_deg)
		p[IN_FIRE] = 1;

	if(g_cfg.verbose)
	{
		const int64_t now = (int64_t)(clock() / (CLOCKS_PER_SEC / 1000));
		if(now - g_last_note > 1000)
		{
			g_last_note = now;
			note("musor: target %d dist %.0f err %.1f weapon %d", best, best_dist,
				angle_deg_diff(angle, direct), weapon);
		}
	}
}

/* -------------------------------------------------------------------- hook */

static int musor_snap_input(void *gc, int *pData)
{
	const int n = g_snap_input((uint8_t *)gc + OFF_M_CONTROLS, pData);
	if(n == (int)(IN_NUM * sizeof(int)) && g_cfg.aim && g_base)
		aim_apply(pData);
	return n;
}

static int find_base(struct dl_phdr_info *info, size_t size, void *data)
{
	(void)size;
	if(info->dlpi_name && strstr(info->dlpi_name, "libDDNet.so"))
	{
		*(uintptr_t *)data = (uintptr_t)info->dlpi_addr;
		return 1;
	}
	return 0;
}

__attribute__((constructor)) static void musor_start(void)
{
	g_base = 0;
	dl_iterate_phdr(find_base, &g_base);
	if(!g_base)
	{
		/* the library can also be mapped without a name we recognise */
		note("musor: libDDNet.so not found, hook skipped");
		return;
	}
	paths_open();
	config_read();
	g_snap_input = (void *)(g_base + OFF_SNAP_INPUT);
	if(patch_branch12((void *)(g_base + OFF_ONSNAP_TAIL), (void *)musor_snap_input) != 0)
	{
		note("musor: patch failed");
		return;
	}
	g_patched = 1;
	note("musor: hook in at %p, aim %d fov %d", (void *)(g_base + OFF_ONSNAP_TAIL), g_cfg.aim, g_cfg.fov);
}
