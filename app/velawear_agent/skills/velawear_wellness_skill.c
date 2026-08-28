/*
 * Install the team-owned VelaWear Wellness Skill into the Agent runtime.
 *
 * The official Agent creates /data/agent/skills during its own startup.  The
 * VelaWear app may start before or after it, so callers retry this operation
 * until the shared data directory is available.
 */

#include <stdio.h>
#include <errno.h>
#include <sys/stat.h>
#include <syslog.h>

#include "skills/velawear_skill.h"

#define VELAWEAR_SKILL_PATH "/data/agent/skills/velawear-wellness.md"

static const char g_velawear_wellness_skill[] =
    "# VelaWear Wellness\n"
    "\n"
    "Turn a wellness request into a device-local reminder that works with Vela AI Agent.\n"
    "\n"
    "## When to use\n"
    "\n"
    "Use when the user asks for a movement break, sedentary reminder, hydration break, or a scheduled wellness check on VelaWear.\n"
    "\n"
    "## How to use\n"
    "\n"
    "1. Call `get_current_time` and calculate a future UNIX timestamp.\n"
    "2. Call `get_steps` when the user asks for current activity context; report only returned values.\n"
    "3. Call `cron_add` with `schedule_type=\"at\"`, a concise message, `channel=\"system\"`, and `chat_id=\"vela-wellness\"`.\n"
    "4. For a recurring check, use `schedule_type=\"every\"` and `interval_s`; keep the interval at least 60 seconds.\n"
    "5. Use `cron_list` to confirm the job. Do not claim that a physical alert happened until the device reports the corresponding action.\n"
    "\n"
    "## Example\n"
    "\n"
    "User: Remind me to move in 10 minutes.\n"
    "\n"
    "-> `get_current_time`\n"
    "-> `cron_add(name=\"move_break\", schedule_type=\"at\", at_epoch=<current_epoch+600>, message=\"Time to move.\", channel=\"system\", chat_id=\"vela-wellness\")`\n"
    "-> `cron_list`\n";

static int ensure_directory(const char *path)
{
    if (mkdir(path, 0755) == 0 || errno == EEXIST) {
        return 0;
    }

    return -errno;
}

int velawear_skill_install(void)
{
    FILE *file;
    size_t length;
    size_t written;

    if (ensure_directory("/data/agent") < 0 ||
        ensure_directory("/data/agent/skills") < 0) {
        return -errno;
    }

    file = fopen(VELAWEAR_SKILL_PATH, "r");
    if (file != NULL) {
        fclose(file);
        return 0;
    }

    file = fopen(VELAWEAR_SKILL_PATH, "w");
    if (file == NULL) {
        return -errno;
    }

    length = sizeof(g_velawear_wellness_skill) - 1;
    written = fwrite(g_velawear_wellness_skill, 1, length, file);
    if (fclose(file) != 0 || written != length) {
        return -EIO;
    }

    syslog(LOG_INFO, "[VelaWear] Installed custom Skill: %s\n",
           VELAWEAR_SKILL_PATH);
    return 0;
}
