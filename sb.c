/* Copywrong © 2023 Ratakor. See LICENSE file for license details. */

#include <X11/Xlib.h>
#include <libconfig.h>
#include <linux/wireless.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/un.h>

#include <err.h>
#include <ifaddrs.h>
#include <limits.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define BATTERY        "BAT0"
#define ETH_INTERFACE  "eth0"
#define WIFI_INTERFACE "wlan0"
#define THERMAL_ZONE   "thermal_zone0"
#define BASE           1000

#define NEWS_CMD       "newsboat -x print-unread"
#define MIC_CMD        "wpctl get-volume @DEFAULT_AUDIO_SOURCE@"
#define VOLUME_CMD     "wpctl get-volume @DEFAULT_AUDIO_SINK@"
#define PUBLICIP_URL   "ifconfig.me"
#define WEATHER_URL    "wttr.in?format=1"
#define CAPACITY_PATH  "/sys/class/power_supply/" BATTERY "/capacity"
#define STATUS_PATH    "/sys/class/power_supply/" BATTERY "/status"
#define CPUTEMP_PATH   "/sys/devices/virtual/thermal/" THERMAL_ZONE "/temp"
#define WIFI_OPERSTATE "/sys/class/net/" WIFI_INTERFACE "/operstate"
#define NETSPEED_RX    "/sys/class/net/%s/statistics/rx_bytes"
#define NETSPEED_TX    "/sys/class/net/%s/statistics/tx_bytes"
#define MUSIC_PAUSE    "{\"command\":[\"get_property_string\",\"pause\"]}\n"
#define MUSIC_TITLE    "{\"command\":[\"get_property\",\"media-title\"]}\n"

#define NORM           "\x1"
#define SEL            "\x2"
#define BLUE           "\x3"
#define GREEN          "\x4"
#define ORANGE         "\x5"
#define RED            "\x6"
#define YELLOW         "\x7"
#define MAGENTA        "\x8"
#define CYAN           "\x9"

#define LENGTH(X)      (sizeof(X) / sizeof(X[0]))
#define STRLEN(X)      (sizeof(X) - 1)
#define HOUR(t)        ((t / 3600) % 24)
#define MINUTE(t)      ((t / 60) % 60)
#define CONFIG_FILE    "sb/config"
#define RESTARTSIG     SIGUSR1
#define OUTPUT_MAX     27
#define CMDLENGTH      (OUTPUT_MAX + STRLEN(delim))
#define STATUSLENGTH   (LENGTH(blocks) * CMDLENGTH + 1)

typedef struct {
	const char *name;
	int (*func)(char *);
	int active;
	unsigned int interval;
} Block;

static time_t ltime(void);
static int xsnprintf(char *str, size_t siz, const char *fmt, ...);
static int execcmd(char *output, size_t siz, const char *cmd);
static intmax_t fgetsn(const char *path);

static int music(char *output);
static int cputemp(char *output);
static int cpu(char *output);
static int memory(char *output);
static int battery(char *output);
static int wifi(char *output);
static int netspeed(char *output);
static int localip(char *output);
static int publicip(char *output);
static int volume(char *output);
static int mic(char *output);
static int news(char *output);
static int weather(char *output);
static int daypercent(char *output);
static int date(char *output);
static int sb_time(char *output);

static void run(Block *block);
static void *blockloop(void *block);
static void statusloop(void);
static void termhandler(int sig);
static void sighandler(int sig);
static void getcfg(void);

static Block blocks[] = {
	{ "music",      music,      0, 0,     },
	{ "cputemp",    cputemp,    0, 10,    },
	{ "cpu",        cpu,        0, 10,    },
	{ "memory",     memory,     0, 10,    },
	{ "battery",    battery,    0, 30,    },
	{ "wifi",       wifi,       0, 10,    },
	{ "netspeed",   netspeed,   0, 1,     },
	{ "localip",    localip,    0, 3600,  },
	{ "publicip",   publicip,   0, 3600,  },
	{ "volume",     volume,     0, 0,     },
	{ "mic",        mic,        0, 0,     },
	{ "news",       news,       0, 3600,  },
	{ "weather",    weather,    0, 18000, },
	{ "daypercent", daypercent, 0, 1800,  },
	{ "date",       date,       0, 3600,  },
	{ "time",       sb_time,    1, 60,    },
};
static char delim[] = " \03|\01 ";

#if BASE == 1000
static const char *prefix[] = { "", "k", "M", "G", "T", "P", "E", "Z", "Y" };
#elif BASE == 1024
static const char *prefix[] = {
	"", "Ki", "Mi", "Gi", "Ti", "Pi", "Ei", "Zi", "Yi"
};
#endif /* BASE */

static Display *dpy;
static pthread_t thr[LENGTH(blocks)];
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static char statusbar[LENGTH(blocks)][CMDLENGTH];
static char statusstr[STATUSLENGTH];
static volatile sig_atomic_t running;
static volatile sig_atomic_t restart;

time_t
ltime(void)
{
	static time_t tz;
	static int once;

	if (!once) {
		const time_t t = time(NULL);
		const struct tm *tm = localtime(&t);
		time_t tz_hour, tz_min;

		tz_hour = tm->tm_hour - HOUR(t);
		tz_min = tm->tm_min - MINUTE(t);
		tz = ((tz_hour * 60) + tz_min) * 60;
		once = 1;
	}

	return time(NULL) + tz;
}

/* return -1 on failure, truncation is not a failure */
int
xsnprintf(char *str, size_t siz, const char *fmt, ...)
{
	va_list ap;
	int rv;

	va_start(ap, fmt);
	rv = vsnprintf(str, siz, fmt, ap);
	va_end(ap);

	if (rv < 0) {
		warn("snprintf");
		return -1;
	} else if ((size_t)rv >= siz) {
		warnx("snprintf: String truncation");
	}

	return rv;
}

/* return -1 on failure */
int
execcmd(char *output, size_t siz, const char *cmd)
{
	FILE *fp;
	char *rv;

	fp = popen(cmd, "r");
	if (fp == NULL) {
		warn("popen '%s'", cmd);
		return -1;
	}
	rv = fgets(output, siz, fp);
	pclose(fp);
	if (rv == NULL)
		return -1;

	return 0;
}

/* return -1 on failure (or 0) */
intmax_t
fgetsn(const char *path)
{
	FILE *fp;
	char buf[32] = "";

	if ((fp = fopen(path, "r")) == NULL) {
		warn("fopen '%s'", path);
		return -1;
	}
	if (fgets(buf, sizeof(buf), fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	return atoll(buf);
}

int
music(char *output)
{
	const struct sockaddr_un addr = {
		.sun_path = "/tmp/mpvsocket",
		.sun_family = AF_UNIX
	};
	const char properties[][45] = { MUSIC_PAUSE, MUSIC_TITLE };
	char buf[2 * OUTPUT_MAX], *start, *end;
	int i, fd, len, pause;

	for (i = 0; i < 2; i++) {
		if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
			warn("socket");
			close(fd);
			return -1;
		}
		if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
			warn("connect '%s'", addr.sun_path);
			close(fd);
			return -1;
		}
		if (send(fd, properties[i], sizeof(properties[i]), 0) < 0) {
			warn("send '%s'", properties[i]);
			close(fd);
			return -1;
		}
		if ((len = recv(fd, buf, STRLEN(buf), MSG_PEEK)) < 0) {
			warn("recv");
			close(fd);
			return -1;
		}
		buf[len] = '\0';

		if (close(fd) < 0) {
			warn("close");
			return -1;
		}

		start = buf + STRLEN("{\"data\":\"");
		end = strchr(start, '"');
		if (end)
			*end = '\0';
		if (i == 0)
			pause = (strcmp(start, "yes") == 0);
	}

	/* don't warn about truncation */
	if (pause)
		return snprintf(output, OUTPUT_MAX, " %s", start);
	return snprintf(output, OUTPUT_MAX, " %s", start);
}

int
cputemp(char *output)
{
	int temp;

	temp = fgetsn(CPUTEMP_PATH);
	if (temp <= 0)
		return -1;

	temp /= 1000;
	if (temp >= 70)
		return xsnprintf(output, OUTPUT_MAX, RED" %02d°C"NORM, temp);
	return xsnprintf(output, OUTPUT_MAX, " %02d°C", temp);
}

int
cpu(char *output)
{
	static long double a[7];
	long double b[7], sum;
	FILE *fp;

	memcpy(b, a, sizeof(b));
	if ((fp = fopen("/proc/stat", "r")) == NULL) {
		warn("fopen '%s'", "/proc/stat");
		return -1;
	}

	/* cpu user nice system idle iowait irq softirq */
	if (fscanf(fp, "%*s %Lf %Lf %Lf %Lf %Lf %Lf %Lf",
	                &a[0], &a[1], &a[2], &a[3], &a[4], &a[5], &a[6])
	                != 7) {
		fclose (fp);
		return -1;
	}
	fclose(fp);

	sum = (b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6]) -
	      (a[0] + a[1] + a[2] + a[3] + a[4] + a[5] + a[6]);

	if (sum == 0)
		return -1;

	return xsnprintf(output, OUTPUT_MAX, " %d%%", (int)(100 *
	                 ((b[0] + b[1] + b[2] + b[5] + b[6]) -
	                  (a[0] + a[1] + a[2] + a[5] + a[6])) / sum));
}

int
memory(char *output)
{
	FILE *fp;
	uintmax_t total, free, buffers, cached;
	double dtotal, used;
	size_t i, j;

	if ((fp = fopen("/proc/meminfo", "r")) == NULL) {
		warn("fopen '%s'", "/proc/meminfo");
		return -1;
	}

	if (fscanf(fp, "MemTotal: %ju kB\n"
	                "MemFree: %ju kB\n"
	                "MemAvailable: %ju kB\n"
	                "Buffers: %ju kB\n"
	                "Cached: %ju kB\n",
	                &total, &free, &buffers, &buffers, &cached) != 5) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	used = (total - free - buffers - cached) * BASE;
	dtotal = total * BASE;
	for (i = 0; i < LENGTH(prefix) && used >= BASE; i++)
		used /= BASE;
	for (j = 0; j < LENGTH(prefix) && dtotal >= BASE; j++)
		dtotal /= BASE;

	return xsnprintf(output, OUTPUT_MAX, "ﳔ %.1f%s/%.1f%s",
	                 used, prefix[i], dtotal, prefix[j]);
}

int
battery(char *output)
{
	static const char *icons[] = {
		"󰂎", "󰁺", "󰁻", "󰁼", "󰁽", "󰁾", "󰁿", "󰂀", "󰂁", "󰂂", "󰁹",
		"󰢟", "󰢜", "󰂆", "󰂇", "󰂈", "󰢝", "󰂉", "󰢞", "󰂊", "󰂋", "󰂅"
	};
	const char *icon, *color;
	FILE *fp;
	char status[32] = "";
	int capacity, charging;

	if ((capacity = fgetsn(CAPACITY_PATH)) < 0)
		return -1;

	if ((fp = fopen(STATUS_PATH, "r")) == NULL) {
		warn("fopen '%s'", STATUS_PATH);
		return -1;
	}
	if (fgets(status, sizeof(status), fp) == NULL) {
		fclose(fp);
		return -1;
	}
	fclose(fp);

	charging = (strcmp(status, "Charging") == 0);
	icon = icons[(capacity / 10) + (charging * LENGTH(icons) / 2)];
	color = charging ? NORM : "";

	if (capacity >= 90)
		color = GREEN;
	else if (!charging) {
		if (capacity < 40 && capacity >= 30)
			color = YELLOW;
		else if (capacity < 30 && capacity >= 20)
			color = ORANGE;
		else if (capacity < 20)
			color = RED;
	}

	if (charging) {
		if (capacity >= 99) {
			execlp("notify-send", "notify-send", "󰂅 Battery full",
			       NULL);
		}
		return xsnprintf(output, OUTPUT_MAX, YELLOW"%s %s%d%%"NORM,
		                 icon, color, capacity);
	}

	if (capacity <= 15)
		execlp("notify-send", "notify-send", "󰁺 Battery low", NULL);
	return xsnprintf(output, OUTPUT_MAX, "%s %s%d%%"NORM,
	                 icon, color, capacity);
}

int
wifi(char *output)
{
	char ssid[IW_ESSID_MAX_SIZE + 1] = "";
	const struct iwreq wreq = {
		.ifr_name = WIFI_INTERFACE,
		.u.essid.length = sizeof(ssid),
		.u.essid.pointer = ssid
	};
	FILE *fp;
	char buf[128], *p;
	int i, fd, quality;

	if ((fp = fopen(WIFI_OPERSTATE, "r")) == NULL) {
		warn("fopen '%s'", WIFI_OPERSTATE);
		return -1;
	}
	p = fgets(buf, sizeof(buf), fp);
	fclose(fp);
	if (p == NULL || strcmp(buf, "up\n") != 0)
		return snprintf(output, OUTPUT_MAX, "󰤯");
	if ((fp = fopen("/proc/net/wireless", "r")) == NULL) {
		warn("fopen '/proc/net/wireless'");
		return -1;
	}
	/* skip to line 3 */
	for (i = 0; i < 3; i++) {
		if ((p = fgets(buf, sizeof(buf), fp)) == NULL)
			break;
	}
	fclose(fp);
	if (i != 3 || p == NULL || (p = strstr(buf, WIFI_INTERFACE)) == NULL)
		return -1;
	p += sizeof(WIFI_INTERFACE) + 1;
	if (sscanf(p, "%*d %d %*d", &quality) != 1)
		return -1;
	quality = (float)quality / 70 * 100;

	if ((fd = socket(AF_INET, SOCK_DGRAM, 0)) < 0) {
		warn("socket 'AF_INET'");
		return -1;
	}
	if (ioctl(fd, SIOCGIWESSID, &wreq) < 0) {
		warn("ioctl 'SIOCGIWESSID'");
		close(fd);
		return -1;
	}
	close(fd);

	if (quality >= 70)
		return snprintf(output, OUTPUT_MAX, "󰤨 %s", ssid);
	if (quality >= 30)
		return snprintf(output, OUTPUT_MAX, "󰤢 %s", ssid);
	return snprintf(output, OUTPUT_MAX, "󰤟 %s", ssid);
}

int
netspeed(char *output)
{
	static intmax_t rx, tx;
	static char interface[16];
	char path[sizeof(NETSPEED_RX) + sizeof(interface)];
	intmax_t tmprx, tmptx;
	double drx, dtx;
	size_t i, j;

	if (interface[0] == 0) {
		FILE * fp;
		char buf[4], *p;

		if ((fp = fopen(WIFI_OPERSTATE, "r")) != NULL) {
			p = fgets(buf, sizeof(buf), fp);
			fclose(fp);
			if (p == NULL || strcmp(buf, "up\n") != 0) {
				xsnprintf(interface, sizeof(interface),
				          ETH_INTERFACE);
			} else {
				xsnprintf(interface, sizeof(interface),
				          WIFI_INTERFACE);
			}
		} else {
			xsnprintf(interface, sizeof(interface), ETH_INTERFACE);
		}
	}

	tmprx = rx;
	xsnprintf(path, sizeof(path), NETSPEED_RX, interface);
	if ((rx = fgetsn(path)) < 0) {
		rx = 0;
		return -1;
	}
	tmptx = tx;
	xsnprintf(path, sizeof(path), NETSPEED_TX, interface);
	if ((tx = fgetsn(path)) < 0) {
		tx = 0;
		return -1;
	}
	if (rx == 0 || tx == 0)
		return -1;

	drx = rx - tmprx;
	dtx = tx - tmptx;
	for (i = 0; i < LENGTH(prefix) && drx >= BASE; i++)
		drx /= BASE;
	for (j = 0; j < LENGTH(prefix) && dtx >= BASE; j++)
		dtx /= BASE;

	return xsnprintf(output, OUTPUT_MAX, " %.1f%s%s %.1f%s",
	                 drx, prefix[i], delim, dtx, prefix[j]);
}

int
localip(char *output)
{
	struct ifaddrs *ifaddr, *ifa;
	int s;
	char host[NI_MAXHOST];

	if (getifaddrs(&ifaddr) < 0) {
		warn("getifaddrs");
		return -1;
	}

	for (ifa = ifaddr; ifa != NULL; ifa = ifa->ifa_next) {
		if (ifa->ifa_addr == NULL)
			continue;

		s = getnameinfo(ifa->ifa_addr, sizeof(struct sockaddr_in6),
		                host, NI_MAXHOST, NULL, 0, NI_NUMERICHOST);
		if ((ifa->ifa_addr->sa_family == AF_INET) &&
		                (strcmp(ifa->ifa_name, ETH_INTERFACE) == 0 ||
		                 strcmp(ifa->ifa_name, WIFI_INTERFACE) == 0)) {
			freeifaddrs(ifaddr);
			if (s != 0) {
				warnx("getnameinfo: %s", gai_strerror(s));
				return -1;
			}
			return xsnprintf(output, OUTPUT_MAX, "ﯱ %s", host);
		}
	}

	freeifaddrs(ifaddr);
	return -1;
}

int
publicip(char *output)
{
	char buf[32];

	if (execcmd(buf, sizeof(buf), "curl -s " PUBLICIP_URL) < 0)
		return -1;

	return xsnprintf(output, OUTPUT_MAX, " %s", buf);
}

int
volume(char *output)
{
	char buf[32];
	int vol;

	if (execcmd(buf, sizeof(buf), VOLUME_CMD) < 0)
		return -1;

	vol = atof(buf + STRLEN("Volume: ")) * 100;

	if (strstr(buf, "MUTED"))
		return xsnprintf(output, OUTPUT_MAX, CYAN" %d%%"NORM, vol);
	return xsnprintf(output, OUTPUT_MAX, " %d%%", vol);
}

int
mic(char *output)
{
	char buf[32];

	if (execcmd(buf, sizeof(buf), MIC_CMD) < 0)
		return -1;

	if (strstr(buf, "MUTED"))
		return xsnprintf(output, OUTPUT_MAX, ORANGE""NORM);
	return xsnprintf(output, OUTPUT_MAX, "");
}

int
news(char *output)
{
	char buf[32];

	if (execcmd(buf, sizeof(buf), NEWS_CMD) < 0)
		return -1;
	if (buf[0] == 'E')
		return -1;

	return xsnprintf(output, OUTPUT_MAX, "📰 %d", atoi(buf));
}

int
weather(char *output)
{
	char buf[32];
	int i, j;

	if (execcmd(buf, sizeof(buf), "curl -s " WEATHER_URL) < 0)
		return -1;

	for (i = 0, j = 0; j < OUTPUT_MAX; i++) {
		switch (buf[i]) {
		case ' ':
			continue;
		case '\n':
			output[j] = '\0';
			break;
		case '+':
		case '-':
			output[j++] = ' ';
		default:
			output[j++] = buf[i];
			continue;
		}
		break;
	}

	return (j == OUTPUT_MAX) ? -1 : j;
}

int
daypercent(char *output)
{
	time_t t;
	int percent;

	t = ltime();
	percent = ((HOUR(t) * 60 + MINUTE(t)) * 100) / (60 * 24);

	return xsnprintf(output, OUTPUT_MAX, " %d%%", percent);
}

int
date(char *output)
{
	time_t t;
	size_t rv;

	t = ltime();
	rv = strftime(output, OUTPUT_MAX, " %b %d (%a)", gmtime(&t));
	if (rv == 0) {
		warnx("strftime: String truncation");
		return -1;
	}
	return rv;
}

int
sb_time(char *output)
{
	time_t t;

	t = ltime();
	if (HOUR(t) >= 22) {
		return xsnprintf(output, OUTPUT_MAX, ORANGE" %02d:%02d"NORM,
		                 HOUR(t), MINUTE(t));
	} else if (HOUR(t) <= 5) {
		return xsnprintf(output, OUTPUT_MAX, RED" %02d:%02d"NORM,
		                 HOUR(t), MINUTE(t));
	}
	return xsnprintf(output, OUTPUT_MAX, " %02d:%02d", HOUR(t), MINUTE(t));
}

void
run(Block *block)
{
	char output[CMDLENGTH] = "";
	size_t len, i = block - blocks;

	if ((len = block->func(output)) <= 0)
		return;

	len = len > OUTPUT_MAX ? OUTPUT_MAX - 1 : len;
	len = output[len - 1] == '\n' ? len - 1 : len;
	strcpy(output + len, delim);

	pthread_mutex_lock(&mutex);
	if (strcmp(output, statusbar[i]) == 0) {
		pthread_mutex_unlock(&mutex);
		return;
	}

	memcpy(statusbar[i], output, len + STRLEN(delim));
	statusbar[i][len + STRLEN(delim)] = '\0';
	statusstr[0] = '\0';
	for (i = 0; i < LENGTH(blocks); i++)
		strcat(statusstr, statusbar[i]);
	statusstr[strlen(statusstr) - STRLEN(delim)] = '\0';
	XStoreName(dpy, DefaultRootWindow(dpy), statusstr);
	XSync(dpy, False);
	pthread_mutex_unlock(&mutex);
}

void *
blockloop(void *block)
{
	unsigned int interval = ((Block *)block)->interval;

	run(block);
	if (interval == 0)
		pthread_exit(NULL);

	while (running) {
		sleep(interval);
		run(block);
	}
	pthread_exit(NULL);
}

void
statusloop(void)
{
	pthread_attr_t attr;
	size_t i;

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
	for (i = 0; i < LENGTH(blocks); i++) {
		statusbar[i][0] = '\0';
		if (blocks[i].active)
			pthread_create(thr + i, &attr, blockloop, blocks + i);
	}
	pthread_attr_destroy(&attr);

	while (running)
		pause();

	for (i = 0; i < LENGTH(blocks); i++) {
		if (blocks[i].active)
			pthread_cancel(thr[i]);
	}
}

void
sighandler(int sig)
{
	if (blocks[sig - SIGRTMIN].active)
		run(blocks + sig - SIGRTMIN);
}

void
termhandler(int sig)
{
	restart = (sig == RESTARTSIG);
	running = 0;
}

void
getcfg(void)
{
	config_t cfg;
	FILE *fp;
	char path[PATH_MAX], *env;
	size_t i;

	env = getenv("XDG_CONFIG_HOME");
	if (env) {
		xsnprintf(path, sizeof(path), "%s/%s", env, CONFIG_FILE);
	} else {
		env = getenv("HOME");
		if (env == NULL)
			errx(1, "HOME is not defined");
		xsnprintf(path, sizeof(path), "%s/.config/%s", env, CONFIG_FILE);
	}

	if ((fp = fopen(path, "r")) == NULL) {
		warn("fopen '%s'", path);
		return;
	}

	config_init(&cfg);
	if (config_read(&cfg, fp) == CONFIG_FALSE) {
		errx(1, "%s:%d: %s", path, config_error_line(&cfg),
		     config_error_text(&cfg));
	}
	for (i = 0; i < LENGTH(blocks); i++)
		config_lookup_bool(&cfg, blocks[i].name, &blocks[i].active);
	config_destroy(&cfg);
	fclose(fp);
}

int
main(void)
{
	size_t i;

	if ((dpy = XOpenDisplay(NULL)) == NULL)
		errx(1, "Failed to open display");
	signal(SIGTERM, termhandler);
	signal(SIGINT, termhandler);
	signal(RESTARTSIG, termhandler);
	for (i = 0; i < LENGTH(blocks); i++)
		signal(SIGRTMIN + i, sighandler);

start:
	getcfg();
	restart = 0;
	running = 1;
	statusloop();
	if (restart)
		goto start;

	XStoreName(dpy, DefaultRootWindow(dpy), "");
	XCloseDisplay(dpy);

	return 0;
}