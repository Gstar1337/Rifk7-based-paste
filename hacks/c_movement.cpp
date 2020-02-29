#include "c_movement.h"

void c_movement::run(c_cs_player* local, c_user_cmd* cmd, bool& send_packet)
{
	if (!local || !local->is_alive())
		return;

	bhop(local, cmd);
	fakewalk(local, cmd, send_packet);
}

void c_movement::autostrafe(c_cs_player* local, c_user_cmd* cmd)
{
	const auto vel = local->get_velocity().length2d();

	if (vel < 1.f)
		return;

	if (cmd->mousedx > 1 || cmd->mousedx < -1)
		cmd->sidemove = cmd->mousedx < 0.f ? -450.f : 450.f;
	else
	{
		cmd->forwardmove = std::clamp(10000.f / vel, -450.0f, 450.0f);
		cmd->sidemove = cmd->command_number % 2 == 0 ? -450.f : 450.f;
	}
};

void c_movement::bhop(c_cs_player* local, c_user_cmd* cmd)
{
	static auto last_jumped = false;
	static auto should_fake = false;

	const auto move_type = local->get_move_type();
	const auto flags = local->get_flags();

	if (move_type != c_cs_player::movetype_ladder && move_type != c_cs_player::movetype_noclip &&
		!(flags & c_cs_player::in_water))
	{
		if (!last_jumped && should_fake)
		{
			should_fake = false;
			cmd->buttons |= c_user_cmd::jump;
		}
		else if (cmd->buttons & c_user_cmd::jump)
		{
			autostrafe(local, cmd);

			if (flags & c_cs_player::on_ground)
			{
				last_jumped = true;
				should_fake = true;
			}
			else
			{
				cmd->buttons &= ~c_user_cmd::jump;
				last_jumped = false;
			}
		}
		else
		{
			last_jumped = false;
			should_fake = false;
		}
	}
}

void c_movement::fakewalk(c_cs_player* local, c_user_cmd* cmd, bool& send_packet)
{
	if (config.rage.slow_walk && GetAsyncKeyState(config.rage.slow_walk))
	{
		static int iChoked = -1;
		iChoked++;
		if (cmd->forwardmove > 0)
		{
			cmd->buttons |= c_user_cmd::back;
			cmd->buttons &= ~c_user_cmd::forward;
		}
		if (cmd->forwardmove < 0)
		{
			cmd->buttons |= c_user_cmd::forward;
			cmd->buttons &= ~c_user_cmd::back;
		}
		if (cmd->sidemove < 0)
		{
			cmd->buttons |= c_user_cmd::move_right;
			cmd->buttons &= ~c_user_cmd::move_left;
		}
		if (cmd->sidemove > 0)
		{
			cmd->buttons |= c_user_cmd::move_left;
			cmd->buttons &= ~c_user_cmd::move_right;
		}
		static int choked = 0;
		choked = choked > 14 ? 0 : choked + 1;

		float nani = 40 / 14;

		cmd->forwardmove = choked < nani || choked > 14 ? 0 : cmd->forwardmove;
		cmd->sidemove = choked < nani || choked > 14 ? 0 : cmd->sidemove; //100:6 are about 16,6, quick maths
		send_packet = choked < 1;
	}
}