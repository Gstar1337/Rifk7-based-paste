#pragma once

#include "../includes.h"

class c_movement
{
public:
	static void run(c_cs_player* local, c_user_cmd* cmd, bool& send_packet);
private:
    static void bhop( c_cs_player* local, c_user_cmd* cmd );
	static void fakewalk(c_cs_player* local, c_user_cmd* cmd, bool& bSendPacket);
    static void autostrafe( c_cs_player* local, c_user_cmd* cmd );
};
