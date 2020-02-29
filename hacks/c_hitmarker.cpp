#include "c_hitmarker.h"
#include "c_esp.h"
#include "../sdk/c_client_entity_list.h"
#include "../BASS/API.h"
#include "../hooks/idirect3ddevice9.h"

std::vector<impact_info> impacts;
std::vector<hitmarker_info> hitmarkers;

void c_hitmarker::draw()
{
	float time = global_vars_base->curtime;

	for (int i = 0; i < hitmarkers.size(); i++)
	{
		bool expired = time >= hitmarkers.at(i).impact.time + 1.f;

		if (expired)
			hitmarkers.at(i).alpha -= 1;

		if (expired && hitmarkers.at(i).alpha <= 0) {
			hitmarkers.erase(hitmarkers.begin() + i);
			continue;
		}

		c_vector3d pos3D = c_vector3d(hitmarkers.at(i).impact.x, hitmarkers.at(i).impact.y, hitmarkers.at(i).impact.z);
		c_vector2d pos2D;

		if (!renderer->screen_transform(pos3D, pos2D, c_esp::info.world_to_screen_matrix))
			continue;

		auto linesize = 8;


		c_color::set_alpha_override(hitmarkers.at(i).alpha);

		renderer->triangle_linear_gradient(pos2D + c_vector2d(-8, 6),
			pos2D + c_vector2d(8, 6),
			pos2D - c_vector2d(0, 8),
			c_color::primary(), c_color::accent(), c_color::accent());
		c_color::set_alpha_override(std::nullopt);
	}

}

void c_hitmarker::on_round_start(c_game_event* event)
{
	if (!config.esp.hitmarker)
		return;

	if (hitmarkers.size() > 0)
		hitmarkers.clear();
}

void c_hitmarker::on_bullet_impact(c_game_event* event)
{
	if (!config.esp.hitmarker)
		return;

	auto local = c_cs_player::get_local_player();

	if (!local || !local->is_alive())
		return;

	impact_info info;

	info.x = event->get_float("x");
	info.y = event->get_float("y");
	info.z = event->get_float("z");

	info.time = global_vars_base->curtime;

	impacts.push_back(info);

}

void c_hitmarker::on_player_hurt(c_game_event* event)
{
	if (!config.esp.hitmarker)
		return;

	auto local = c_cs_player::get_local_player();
	if (!local || !local->is_alive())
		return;

	const auto attacker = client_entity_list()->get_client_entity(engine_client()->get_player_for_user_id(event->get_int(_("attacker"))));
	const auto target = reinterpret_cast<c_cs_player*>(client_entity_list()->get_client_entity(
		engine_client()->get_player_for_user_id(event->get_int(_("userid")))));

	if (attacker && target && attacker == local && target->is_enemy())
	{
		uint32_t hitsound = 0;

		switch (config.esp.hitsound)
		{
		case 1:
			hitsound = BASS::stream_sounds.cod_hitsound;
			break;
		case 2:
			hitsound = BASS::stream_sounds.ut_hitsound;
			break;
		case 3:
			hitsound = BASS::stream_sounds.q3_hitsound;
			break;
		case 4:
			hitsound = BASS::stream_sounds.roblox_hitsound;
			break;
		case 5:
			hitsound = BASS::stream_sounds.uff_hitsound;
			break;
		case 6:
			hitsound = BASS::stream_sounds.laser;
			break;
		default:
			break;
		}

		if (hitsound)
		{
			BASS_SET_VOLUME(hitsound, config.esp.hitsound_volume / 100.f);
			BASS_ChannelPlay(hitsound, true);
		}

		impact_info best_impact;

		float best_impact_distance = -1;

		float time = global_vars_base->curtime;

		for (int i = 0; i < impacts.size(); i++)
		{
			auto iter = impacts[i];

			if (time > iter.time + 1.f)
			{
				impacts.erase(impacts.begin() + i);
				continue;
			}

			c_vector3d position = c_vector3d(iter.x, iter.y, iter.z);

			c_vector3d enemy_pos = target->get_origin();

			float distance = position.dist_to(enemy_pos);

			if (distance < best_impact_distance || best_impact_distance == -1)
			{
				best_impact_distance = distance;
				best_impact = iter;
			}
		}

		if (best_impact_distance == -1)
			return;

		hitmarker_info info;

		info.impact = best_impact;
		info.alpha = 255;

		hitmarkers.push_back(info);
	}
}
