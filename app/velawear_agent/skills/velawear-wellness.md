# VelaWear Wellness

Turn a wellness request into a device-local reminder that works with Vela AI Agent.

## When to use

Use when the user asks for a movement break, sedentary reminder, hydration break, or a scheduled wellness check on VelaWear.

## How to use

1. Call `get_current_time` and calculate a future UNIX timestamp.
2. Call `get_steps` when the user asks for current activity context; report only returned values.
3. Call `cron_add` with `schedule_type="at"`, a concise message, `channel="system"`, and `chat_id="vela-wellness"`.
4. For a recurring check, use `schedule_type="every"` and `interval_s`; keep the interval at least 60 seconds.
5. Use `cron_list` to confirm the job. Do not claim that a physical alert happened until the device reports the corresponding action.

## Example

User: Remind me to move in 10 minutes.

→ `get_current_time`
→ `cron_add(name="move_break", schedule_type="at", at_epoch=<current_epoch+600>, message="Time to move.", channel="system", chat_id="vela-wellness")`
→ `cron_list`
