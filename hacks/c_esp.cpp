#include "c_esp.h"
#include "c_aimhelper.h"
#include "../utils/math.h"
#include "../sdk/c_model_info_client.h"
#include "../utils/c_config.h"
#include "../sdk/c_weapon_system.h"
#include "../sdk/c_view_beams.h"
#include "../sdk/c_debug_overlay.h"

float esp_alpha_fade[64];

std::string tostr(int x)
{
	std::stringstream str;
	str << x;
	return str.str();
}

void c_esp::draw()
{
	if (!engine_client()->is_ingame())
	{
		if (!info.players.empty() || info.players.size() > 0)
			info.players.clear();

		return;
	}

	std::lock_guard<std::mutex> lock(info.mutex);

	draw_players();
	draw_nades();

	draw_scope();
}

void c_esp::draw_local_impact(c_vector3d start, c_vector3d end)
{
	if (!config.esp.team.impacts)
		return;

	start.z *= 1.05f;

	draw_impact(start, end, config.esp.team.impacts_color);
}

void c_esp::draw_enemy_impact(c_game_event* event)
{
	if (!config.esp.enemy.impacts)
		return;

	const auto userid = event->get_int(_("userid"));
	const auto index = engine_client()->get_player_for_user_id(userid);
	const auto player = reinterpret_cast<c_cs_player*>(client_entity_list()->get_client_entity(index));

	if (player && player->is_enemy() && player->is_alive()
		&& !player->is_dormant() && config.esp.enemy.impacts)
		draw_impact(player->get_shoot_position(), { event->get_float(_("x")),
			event->get_float(_("y")),
			event->get_float(_("z")) }, config.esp.enemy.impacts_color);
}

void c_esp::store_data()
{
	if (!engine_client()->is_ingame())
		return;

	std::lock_guard<std::mutex> lock(info.mutex);

	info.world_to_screen_matrix = renderer->world_to_screen_matrix();

	std::unordered_map<c_base_handle, std::pair<int, int>> old_health;
	client_entity_list()->for_each_player([&] (c_cs_player* player) -> void
	{
		for (auto& old_player : info.players)
			if (old_player.player == player
				&& old_player.handle == player->get_handle()
				&& old_player.last_health > 0)
				old_health[old_player.handle] = std::make_pair(old_player.last_health, old_player.health_fade);
	});

	info.players.clear();
	info.players.reserve(64);

	client_entity_list()->for_each_player_fixed_z_order([&] (c_cs_player* player) -> void
	{
		const auto anim_info = animation_system->get_animation_info(player);

		player_info current;
		current.handle = player->get_handle();
		current.player = player;
		current.index = player->index();
		memcpy(current.name, player->get_info().name, 15);
		current.name[15] = '\0';
		current.origin = player->get_abs_origin();
		current.angles = player->get_abs_angles();
		current.duck_amount = player->get_duck_amount();
		current.health = std::clamp(player->get_health(), 0, 100);
		current.is_enemy = player->is_enemy();
		current.is_on_ground = player->is_on_ground();
		current.dormant = player->is_dormant();
		current.layers = *player->get_animation_layers();
		current.reload = player->get_sequence_activity(current.layers[1].sequence) == act_csgo_reload;
		current.good_bones = player->setup_bones(current.bones, 128, bone_used_by_anything, global_vars_base->curtime);
		current.head_position = player->get_hitbox_position(c_cs_player::hitbox::head, current.bones);

		current.has_kevlar = player->get_armor() > 0;
		current.has_helmet = player->has_helmet();
		current.is_defusing = player->get_defusing();
		current.is_scoped = player->is_scoped();

		const auto last_health = old_health.find(current.handle);
		if (!old_health.empty() && last_health != old_health.end())
		{
			current.last_health = last_health->second.first;
			current.health_fade = last_health->second.second;
		}
		else
		{
			current.last_health = current.health;
			current.health_fade = 255;
		}

		if (anim_info)
		{
			current.shots_missed = anim_info->missed_due_to_resolver + anim_info->missed_due_to_spread;

			switch (anim_info->brute_state)
			{
			case resolver_start:
				current.resolver_mode = 0;
				break;
			case resolver_inverse:
				current.resolver_mode = 1;
				break;
			case resolver_no_desync:
				current.resolver_mode = 2;
				break;
			default:
			case resolver_jitter:
				current.resolver_mode = 3;
				break;
			}
		}

		const auto weapon = reinterpret_cast<c_base_combat_weapon*>(
			client_entity_list()->get_client_entity_from_handle(player->get_current_weapon_handle()));

		if (weapon)
		{
			const auto info = weapon_system->get_weapon_data(weapon->get_item_definition());
			current.ammo = weapon->get_current_clip();
			current.ammo_max = info->iMaxClip1;
			current.weapon = weapon;
		}
		else
		{
			current.weapon = nullptr;
			current.ammo = 0;
			current.ammo_max = 1;
		}

		info.players.push_back(current);
	});

	info.nades.clear();

	client_entity_list()->for_each([](c_client_entity* entity) -> void {
		static const auto is_nade_projectile = [](uint32_t id) {
			switch (id) {
			case cbasecsgrenadeprojectile:
			case cdecoyprojectile:
			case csmokegrenadeprojectile:
			case cmolotovprojectile:
				return true;
			default:
				return false;
			}
		};

		const auto base = reinterpret_cast<c_base_combat_weapon*>(entity);

		if (!is_nade_projectile(base->get_class_id()))
			return;

		nade_info current;
		current.origin = base->get_render_origin();
		current.type = base->get_class_id();
		info.nades.push_back(current);
	});

	const auto local = c_cs_player::get_local_player();

	if (!local)
		return;

	info.shoot_position = local->get_shoot_position();
	info.is_scoped = local->is_scoped();
}

void c_esp::draw_players()
{
	if (info.players.empty())
		return;

	for (auto& player : info.players)
		draw_player(player);
}

void c_esp::draw_nade(nade_info& nade)
{
	if (!config.esp.nade_type)
		return;

	c_vector2d origin_screen;

	if (!renderer->screen_transform(nade.origin, origin_screen, info.world_to_screen_matrix))
		return;

	static const auto get_nade_string = [](int32_t definition) {
		switch (definition)
		{
		case cbasecsgrenadeprojectile:
			return std::string(_("GRENADE"));
		case cdecoyprojectile:
			return std::string(_("DECOY"));
		case csmokegrenadeprojectile:
			return std::string(_("SMOKE"));
		case cmolotovprojectile:
			return std::string(_("FIRE-BOMB"));
		default:
			return std::string();
		}
	};

	if (config.esp.nade_type == 2)
	{
		c_vector3d forward;
		math::angle_vectors(engine_client()->get_view_angles(), forward);
		const auto transform = math::rotation_matrix(forward);
		renderer->ball(nade.origin, 6.f, transform, config.esp.nade_color, info.world_to_screen_matrix);
	}

	renderer->text(origin_screen - c_vector2d(0, 18), get_nade_string(nade.type).c_str(), config.esp.nade_color, fnv1a("pro12"), esp_flags);
}

void c_esp::draw_nades()
{
	if (info.nades.empty())
		return;

	for (auto& nade : info.nades)
		draw_nade(nade);
}

void c_esp::draw_scope()
{
	if (!config.misc.no_scope || !info.is_scoped)
		return;

	const auto center = renderer->get_center();
	

	renderer->line({ center.x, 0 }, { center.x, 2.f * center.y }, c_color(0, 0, 0));
	renderer->line({ 0, center.y }, { 2.f * center.x, center.y }, c_color(0, 0, 0));
}

void c_esp::draw_impact(c_vector3d start, c_vector3d end, c_color color)
{
	beam_info info;

	info.type = 0;
	info.model_name = _("sprites/physbeam.vmt");
	info.model_index = -1;
	info.halo_scale = 0.f;
	info.life = 2.f;
	info.width = 2.f;
	info.end_width = 2.f;
	info.fade_length = 0.f;
	info.amplitude = 2.f;
	info.brightness = 255.f;
	info.speed = .2f;
	info.start_frame = 0;
	info.frame_rate = 0.f;
	info.red = float(color.red);
	info.green = float(color.green);
	info.blue = float(color.blue);
	info.segments = 2;
	info.renderable = true;
	info.flags = 0;
	info.start = start;
	info.end = end;

	auto beam = view_render_beams->create_beam_points(info);

	if (beam)
		view_render_beams->draw_beam(beam);

	debug_overlay()->add_box_overlay(end, c_vector3d(-2, -2, -2), c_vector3d(2, 2, 2), c_qangle(),
		color.red, color.green, color.blue, 255, 2.f);
}

void c_esp::esp_name(player_info& player, const c_vector2d& from, const float width, const c_color& color)
{
	renderer->text(from + c_vector2d(width / 2.f, -9.f), player.name, c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]), fnv1a("pro13"), esp_flags);
}

void c_esp::esp_box(const c_vector2d& from, const float width, const float height, const c_color& color)
{
	renderer->rect(from - c_vector2d(1, 1), c_vector2d(width + 2, height + 2), c_color::shadow(120));
	renderer->rect(from + c_vector2d(1, 1), c_vector2d(width - 2, height - 2), c_color::shadow(120));
	renderer->rect(from, c_vector2d(width, height), color);
}

void c_esp::esp_player_flags(player_info& player, const c_vector2d& from, const float width, const float height, const c_color& color)
{
#ifdef PRINT_DEBUG
	printf("draw_player_flags \n");
#endif

	//Do all player flags inside of this
	int _x = from.x + width + 5, _y = from.y + 3;

	auto draw_flag = [&](c_color color, const char* text, ...) -> void
	{
		renderer->text(c_vector2d(_x, _y), text, color, fnv1a("pro12"), c_font::centered_y | c_font::font_flags::drop_shadow);
		_y += 10;
	};

	std::string text;

	if (player.has_helmet) text += "H";
	if (player.has_kevlar) text += "K";

	if (player.has_helmet || player.has_kevlar)
	{
		//if (config.esp.enemy_flags.kevlar)
			draw_flag(c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]), text.c_str());
	}

	if (player.is_scoped /*&& config.esp.enemy_flags.zoom*/)
		draw_flag(c_color(50, 50, 255, 255 * esp_alpha_fade[player.index]), "ZOOM");

	if (player.reload && player.layers[1].cycle < 0.99f /*&& config.esp.enemy_flags.reload*/)
		draw_flag(c_color(255, 50, 50, 255 * esp_alpha_fade[player.index]), "RELOAD");

	if (player.is_defusing /*&& config.esp.enemy_flags.defuse*/)
		draw_flag(c_color(255, 50, 50, 255 * esp_alpha_fade[player.index]), "DEFUSE");

	if (player.shots_missed >= 1)
	{
		//static const auto resolver_display = __("R MODE [%i]");
		//_rt(r_mode, resolver_display);

		//char r_msg[255];
		//sprintf_s(r_msg, r_mode, player.resolver_mode);

		static const auto shot_display = __("MISSED: %i");
		_rt(s_dis, shot_display);

		char shot_msg[255];
		sprintf_s(shot_msg, s_dis, player.shots_missed);

		draw_flag(c_color(255, 50, 50, 255 * esp_alpha_fade[player.index]), shot_msg);
		//draw_flag(c_color(64, 190, 59, 255 * esp_alpha_fade[player.index]), r_msg);
	}
}

void c_esp::esp_health(player_info& player, const c_vector2d& from, const float height, const c_color& color)
{
	// grey background
	renderer->rect_filled_linear_gradient(c_vector2d(from.x - 8, from.y),
		c_vector2d(4, height), c_color(20, 20, 20, 200 * esp_alpha_fade[player.index]), c_color(80, 80, 80, 200 * esp_alpha_fade[player.index]));

	const auto health_removed = 100 - player.health;
	const auto health_scaled = static_cast<float>(health_removed) / 100.f * height;

	// actual health
	renderer->rect_filled_linear_gradient(c_vector2d(from.x - 8, from.y + health_scaled),
		c_vector2d(4, height - health_scaled), c_color(50, 150, 50, 200 * esp_alpha_fade[player.index]), c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]));

	if (player.health != player.last_health && player.health_fade == 0)
	{
		player.last_health = player.health;
		player.health_fade = 255;
	} 
	else if (player.health == player.last_health && health_removed > 0)
	{
		renderer->rect_filled(c_vector2d(from.x - 8, from.y + health_scaled - 2),
			c_vector2d(4, 2), c_color(200, 200, 200, 255 * esp_alpha_fade[player.index]));
	}
	else if (player.health != player.last_health)
	{
		linear_fade(player.health_fade, 0, 255, fade_frequency, false);

		const auto damage = static_cast<float>(player.last_health - player.health) / 100.f * height;

		// draw white filled rect
		renderer->rect_filled_linear_gradient(c_vector2d(from.x - 8, from.y - damage + health_scaled),
			c_vector2d(4, damage), c_color(150, 150, 150, player.health_fade * esp_alpha_fade[player.index]),
			c_color(225, 225, 225, player.health_fade * esp_alpha_fade[player.index]));
	}

	// black outline
	renderer->rect(c_vector2d(from.x - 8, from.y),
		c_vector2d(4, height), c_color(0, 0, 0, 255 * esp_alpha_fade[player.index]));

	// health text
	if (player.health < 100 && player.health > 10)
		renderer->text(from + c_vector2d(-17, health_scaled - 3), tostr(player.health).c_str(), color, fnv1a("pro11"), esp_flags);
}

void c_esp::esp_ammo(player_info& player, const c_vector2d& from, const float width, const float height, const c_color& color)
{
	if ((player.ammo <= 0 || player.ammo_max <= 1) && !player.reload)
		return;

	// grey background
	renderer->rect_filled_linear_gradient(c_vector2d(from.x, from.y + height + 4),
		c_vector2d(width, 4), c_color(20, 20, 20, 200 * esp_alpha_fade[player.index]), c_color(80, 80, 80, 200 * esp_alpha_fade[player.index]));

	auto ammo = static_cast<float>(player.ammo) / static_cast<float>(player.ammo_max) * width;

	if (player.reload && player.layers[1].cycle > 0.f)
		ammo = player.layers[1].cycle * width;

	// actual health
	renderer->rect_filled_linear_gradient(c_vector2d(from.x, from.y + height + 4),
		c_vector2d(ammo, 4), c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]), c_color(210, 190, 40, 200 * esp_alpha_fade[player.index]), true);

	// black outline
	renderer->rect(c_vector2d(from.x, from.y + height + 4),
		c_vector2d(width, 4), c_color(0, 0, 0, 255 * esp_alpha_fade[player.index]));
}

void c_esp::esp_radar(std::optional<c_vector3d> position, const c_color& color)
{
	if (!position.has_value())
return;

const auto local = c_cs_player::get_local_player();

if (!local)
return;

const auto view = engine_client()->get_view_angles();
const auto angle_to = math::calc_angle(info.shoot_position, position.value());
auto target_angle = angle_to - view;
math::normalize(target_angle);

const auto angle = target_angle.y;
const auto height = 400.f;

const auto a = renderer->get_center() - c_vector2d(
(height - 20) * sin(deg2rad(angle + 2)),
(height - 20) * cos(deg2rad(angle + 2))
);

const auto b = renderer->get_center() - c_vector2d(
	height * sin(deg2rad(angle)),
	height * cos(deg2rad(angle))
);

const auto c = renderer->get_center() - c_vector2d(
(height - 20) * sin(deg2rad(angle - 2)),
(height - 20) * cos(deg2rad(angle - 2))
);

renderer->triangle_filled_linear_gradient(a, b, c, c_color(40, 40, 40, 200), color, color);
renderer->triangle(a, b, c, c_color(0, 0, 0, 255));
}

void c_esp::esp_skeleton(player_info& player, const c_color& color, const bool retro)
{
	c_vector2d bone, bone_parent;

	const auto entity = reinterpret_cast<c_cs_player*>(client_entity_list()->get_client_entity(player.index));
	if (!entity)
		return;

	const auto model = entity->get_model();
	if (!model)
		return;

	const auto studio_model = model_info_client()->get_studio_model(model);

	if (!studio_model)
		return;

	if (!player.good_bones)
		return;

	const auto head_id = retro ? entity->get_hitbox_bone_attachment(c_cs_player::hitbox::head) : std::nullopt;
	//const auto left_hand_id = retro ? entity->get_hitbox_bone_attachment(c_cs_player::hitbox::left_hand) : std::nullopt;
	//const auto right_hand_id = retro ? entity->get_hitbox_bone_attachment(c_cs_player::hitbox::right_hand) : std::nullopt;
	//i hate hand skeleton esp lol
	for (auto i = 0; i < studio_model->numbones; i++)
	{
		if (head_id.has_value() && head_id.value() == i)
			continue;

		const auto p_bone = studio_model->get_bone(i);
		if (!p_bone || p_bone->parent == -1)
			continue;

		/*if ((!p_bone->has_parent(studio_model, left_hand_id.value_or(-2))
				&& !p_bone->has_parent(studio_model, right_hand_id.value_or(-2)))
			|| !retro)*/
		if (!(p_bone->flags & bone_used_by_hitbox))
			continue;

		if (!renderer->screen_transform(c_vector3d(player.bones[i][0][3], player.bones[i][1][3], player.bones[i][2][3]), bone,
			info.world_to_screen_matrix) || !renderer->screen_transform(c_vector3d(player.bones[p_bone->parent][0][3],
				player.bones[p_bone->parent][1][3], player.bones[p_bone->parent][2][3]), bone_parent, info.world_to_screen_matrix))
			continue;

		renderer->line(bone, bone_parent, c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]));
	}

	if (retro && player.head_position.has_value())
	{
		c_vector3d forward;
		math::angle_vectors(player.angles, forward);
		const auto transform = math::rotation_matrix(forward);
		renderer->ball(player.head_position.value(), head_radius, transform,
			c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]), info.world_to_screen_matrix);
	}
}

void c_esp::esp_weapon(player_info& player, const c_vector2d& from, const float width, const float height, const c_color& color, bool draw_ammo)
{
	if (!player.weapon)
		return;

	float additive = 4;

	if (draw_ammo)
		additive += 12;
	//if (config.esp.wep_icons) additive += 5;

	//if (config.esp.wep_icons)
	//	renderer->text(c_vector2d(from.x + (width / 2), from.y + height + additive), player.weapon->GetGunIcon(), color, fnv1a("weapons"), esp_flags);
	//else
		renderer->text(c_vector2d(from.x + (width / 2), from.y + height + additive), player.weapon->get_weapon_name(), c_color(color.red, color.green, color.blue, 255 * esp_alpha_fade[player.index]), fnv1a("pro11"), esp_flags);
		additive += 9;

	if (draw_ammo)
	{
		std::string ammunition = "[" + tostr(player.ammo) + "/" + tostr(player.ammo_max) + "]";
		renderer->text(from + c_vector2d(width / 2.f, height + additive), ammunition.c_str(), color, fnv1a("pro11"), esp_flags);
	}
}

void c_esp::draw_player(player_info& player)
{
	if (!player.head_position.has_value())
		return;

	if (!engine_client()->is_ingame() || !engine_client()->is_connected())
		return;

	int idx = player.index;

	float in = (1.f / 0.2f) * global_vars_base->frametime;
	float out = (1.f / 0.2f) * global_vars_base->frametime;

	if (!player.dormant)
	{
		if (esp_alpha_fade[idx] < 1.f)
			esp_alpha_fade[idx] += in;
	}
	else
	{
		if (esp_alpha_fade[idx] > 0.f)
			esp_alpha_fade[idx] -= out;
	}

	esp_alpha_fade[idx] = (esp_alpha_fade[idx] > 1.f ? 1.f : esp_alpha_fade[idx] < 0.f ? 0.f : esp_alpha_fade[idx]);


	const auto& esp = player.is_enemy ? config.esp.enemy : config.esp.team;

	c_vector2d origin_screen, top_screen;

	const auto collision_box_interpolated = collision_box_top - collision_box_mod * player.duck_amount;
	const auto view_height = player.head_position.value().z - player.origin.z;

	auto top = player.origin + c_vector3d(0.f, 0.f, collision_box_interpolated);

	if (view_height == 46.f && !player.is_on_ground)
		top = player.head_position
		.value_or(c_vector3d()) + c_vector3d(0.f, 0.f, 9.f);

	if (esp.radar == 2)
		esp_radar(player.head_position, esp.color);

	if (!renderer->screen_transform(player.origin, origin_screen, info.world_to_screen_matrix)
		|| !renderer->screen_transform(top, top_screen, info.world_to_screen_matrix))
	{
		if (esp.radar == 1)
			esp_radar(player.head_position, esp.color);
		return;
	}

	const auto height = origin_screen.y - top_screen.y + 6;
	const auto width = height / 1.9f;
	const auto from = c_vector2d(top_screen.x - width / 2.f, top_screen.y);

	if (esp.radar == 1 && !renderer->is_on_screen(
		player.head_position.value_or(player.origin), width, info.world_to_screen_matrix))
		esp_radar(player.head_position, esp.color);

	if (esp.skeleton)
		esp_skeleton(player, esp.color, esp.skeleton == 2);

	if (esp.box)
		esp_box(from, width, height, c_color(esp.color.red, esp.color.green, esp.color.blue, 255 * esp_alpha_fade[player.index]));

	if (esp.bar)
		esp_health(player, from, height, esp.color);

	bool ammo = false;

	if (esp.bar == 2)
	{
		esp_ammo(player, from, width, height, esp.color);
		ammo = true;
	}
	else
		ammo = false;

	if (esp.info)
	{
		esp_name(player, from, width, esp.color);
		esp_player_flags(player, from, width, height, esp.color);
		esp_weapon(player, from, width, height, esp.color, ammo);
	}
}
