//========= Copyright Valve Corporation, All rights reserved. ============//
//
// Purpose: 
//
//=============================================================================//

#include "cbase.h"
#include "player.h"
#include "usercmd.h"
#include "igamemovement.h"
#include "mathlib/mathlib.h"
#include "client.h"
#include "player_command.h"
#include "movehelper_server.h"
#include "tier0/vprof.h"

#include "in_buttons.h"
#include "movevars_shared.h"
#include "momentum/mom_player.h"
#include "mom_system_gamemode.h"

// memdbgon must be the last include file in a .cpp file!!!
#include "tier0/memdbgon.h"

extern IGameMovement *g_pGameMovement;
extern CMoveData *g_pMoveData;	// This is a global because it is subclassed by each game.
extern ConVar sv_noclipduringpause;

ConVar sv_maxusrcmdprocessticks_warning( "sv_maxusrcmdprocessticks_warning", "-1", FCVAR_NONE, "Print a warning when user commands get dropped due to insufficient usrcmd ticks allocated, number of seconds to throttle, negative disabled" );
static ConVar sv_maxusrcmdprocessticks_holdaim( "sv_maxusrcmdprocessticks_holdaim", "1", FCVAR_CHEAT, "Hold client aim for multiple server sim ticks when client-issued usrcmd contains multiple actions (0: off; 1: hold this server tick; 2+: hold multiple ticks)" );

//-----------------------------------------------------------------------------
// Purpose: 
//-----------------------------------------------------------------------------
CPlayerMove::CPlayerMove( void )
{
}

//-----------------------------------------------------------------------------
// Purpose: We're about to run this usercmd for the specified player.  We can set up groupinfo and masking here, etc.
//  This is the time to examine the usercmd for anything extra.  This call happens even if think does not.
// Input  : *player - 
//			*cmd - 
//-----------------------------------------------------------------------------
void CPlayerMove::StartCommand( CBasePlayer *player, CUserCmd *cmd )
{
	VPROF( "CPlayerMove::StartCommand" );

#if !defined( NO_ENTITY_PREDICTION )
	CPredictableId::ResetInstanceCounters();
#endif

	player->m_pCurrentCommand = cmd;
	CBaseEntity::SetPredictionRandomSeed( cmd );
	CBaseEntity::SetPredictionPlayer( player );
	
#if defined (HL2_DLL)
	// pull out backchannel data and move this out

	int i;
	for (i = 0; i < cmd->entitygroundcontact.Count(); i++)
	{
		int entindex =  cmd->entitygroundcontact[i].entindex;
		CBaseEntity *pEntity = CBaseEntity::Instance( engine->PEntityOfEntIndex( entindex) );
		if (pEntity)
		{
			CBaseAnimating *pAnimating = pEntity->GetBaseAnimating();
			if (pAnimating)
			{
				pAnimating->SetIKGroundContactInfo( cmd->entitygroundcontact[i].minheight, cmd->entitygroundcontact[i].maxheight );
			}
		}
	}

#endif
}

//-----------------------------------------------------------------------------
// Purpose: We've finished running a user's command
// Input  : *player - 
//-----------------------------------------------------------------------------
void CPlayerMove::FinishCommand( CBasePlayer *player )
{
	VPROF( "CPlayerMove::FinishCommand" );

	player->m_pCurrentCommand = NULL;
	CBaseEntity::SetPredictionRandomSeed( NULL );
	CBaseEntity::SetPredictionPlayer( NULL );
}

//-----------------------------------------------------------------------------
// Purpose: Checks if the player is standing on a moving entity and adjusts velocity and 
//  basevelocity appropriately
// Input  : *player - 
//			frametime - 
//-----------------------------------------------------------------------------
void CPlayerMove::CheckMovingGround( CBasePlayer *player, double frametime )
{
	VPROF( "CPlayerMove::CheckMovingGround()" );

	CBaseEntity	    *groundentity;

	if ( player->GetFlags() & FL_ONGROUND )
	{
		groundentity = player->GetGroundEntity();
		if ( groundentity && ( groundentity->GetFlags() & FL_CONVEYOR) )
		{
			Vector vecNewVelocity;
			groundentity->GetGroundVelocityToApply( vecNewVelocity );
			if ( player->GetFlags() & FL_BASEVELOCITY )
			{
				vecNewVelocity += player->GetBaseVelocity();
			}
			player->SetBaseVelocity( vecNewVelocity );
			player->AddFlag( FL_BASEVELOCITY );
		}
	}

	if ( !( player->GetFlags() & FL_BASEVELOCITY ) )
	{
		// Apply momentum (add in half of the previous frame of velocity first)
		player->ApplyAbsVelocityImpulse( (1.0 + ( frametime * 0.5 )) * player->GetBaseVelocity() );
		player->SetBaseVelocity( vec3_origin );
	}

	player->RemoveFlag( FL_BASEVELOCITY );
}

//-----------------------------------------------------------------------------
// Purpose: Prepares for running movement
// Input  : *player - 
//			*ucmd - 
//			*pHelper - 
//			*move - 
//			time - 
//-----------------------------------------------------------------------------
void CPlayerMove::SetupMove( CBasePlayer *player, CUserCmd *ucmd, IMoveHelper *pHelper, CMoveData *move )
{
	VPROF( "CPlayerMove::SetupMove" );

	// Allow sound, etc. to be created by movement code
	move->m_bFirstRunOfFunctions = true;
	move->m_bGameCodeMovedPlayer = false;
	if ( player->GetPreviouslyPredictedOrigin() != player->GetAbsOrigin() )
	{
		move->m_bGameCodeMovedPlayer = true;
	}

	// Prepare the usercmd fields
	move->m_nImpulseCommand		= ucmd->impulse;	
	move->m_vecViewAngles		= ucmd->viewangles;

	CBaseEntity *pMoveParent = player->GetMoveParent();
	if (!pMoveParent)
	{
		move->m_vecAbsViewAngles = move->m_vecViewAngles;
	}
	else
	{
		matrix3x4_t viewToParent, viewToWorld;
		AngleMatrix( move->m_vecViewAngles, viewToParent );
		ConcatTransforms( pMoveParent->EntityToWorldTransform(), viewToParent, viewToWorld );
		MatrixAngles( viewToWorld, move->m_vecAbsViewAngles );
	}

	move->m_nButtons			= ucmd->buttons;

	// Ignore buttons for movement if at controls
	if ( player->GetFlags() & FL_ATCONTROLS )
	{
		move->m_flForwardMove		= 0;
		move->m_flSideMove			= 0;
		move->m_flUpMove				= 0;
	}
	else
	{
		move->m_flForwardMove		= ucmd->forwardmove;
		move->m_flSideMove			= ucmd->sidemove;
		move->m_flUpMove				= ucmd->upmove;
	}

	// Prepare remaining fields
	move->m_flClientMaxSpeed		= player->m_flMaxspeed;
	move->m_nOldButtons			= player->m_Local.m_nOldButtons;
	move->m_vecAngles			= player->pl.v_angle.Get();

	move->m_vecVelocity			= player->GetAbsVelocity();

	move->m_nPlayerHandle		= player;

	move->SetAbsOrigin( player->GetAbsOrigin() );

	// Copy constraint information
	if ( player->m_hConstraintEntity.Get() )
		move->m_vecConstraintCenter = player->m_hConstraintEntity.Get()->GetAbsOrigin();
	else
		move->m_vecConstraintCenter = player->m_vecConstraintCenter;
	move->m_flConstraintRadius = player->m_flConstraintRadius;
	move->m_flConstraintWidth = player->m_flConstraintWidth;
	move->m_flConstraintSpeedFactor = player->m_flConstraintSpeedFactor;
}

//-----------------------------------------------------------------------------
// Purpose: Finishes running movement
// Input  : *player - 
//			*move - 
//			*ucmd - 
//			time - 
//-----------------------------------------------------------------------------
void CPlayerMove::FinishMove( CBasePlayer *player, CUserCmd *ucmd, CMoveData *move )
{
	VPROF( "CPlayerMove::FinishMove" );

	// NOTE: Don't copy this.  the movement code modifies its local copy but is not expecting to be authoritative
	//player->m_flMaxspeed			= move->m_flClientMaxSpeed;
	player->SetAbsOrigin( move->GetAbsOrigin() );
	player->SetAbsVelocity( move->m_vecVelocity );
	player->SetPreviouslyPredictedOrigin( move->GetAbsOrigin() );

	player->m_Local.m_nOldButtons			= move->m_nButtons;

	// Convert final pitch to body pitch
	float pitch = move->m_vecAngles[ PITCH ];
	if ( pitch > 180.0f )
	{
		pitch -= 360.0f;
	}
	pitch = clamp( pitch, -90.f, 90.f );

	move->m_vecAngles[ PITCH ] = pitch;

	player->SetBodyPitch( pitch );

	player->SetLocalAngles( move->m_vecAngles );

	// The class had better not have changed during the move!!
	if ( player->m_hConstraintEntity )
		Assert( move->m_vecConstraintCenter == player->m_hConstraintEntity.Get()->GetAbsOrigin() );
	else
		Assert( move->m_vecConstraintCenter == player->m_vecConstraintCenter );
	Assert( move->m_flConstraintRadius == player->m_flConstraintRadius );
	Assert( move->m_flConstraintWidth == player->m_flConstraintWidth );
	Assert( move->m_flConstraintSpeedFactor == player->m_flConstraintSpeedFactor );
}

//-----------------------------------------------------------------------------
// Purpose: Called before player thinks
// Input  : *player - 
//			thinktime - 
//-----------------------------------------------------------------------------
void CPlayerMove::RunPreThink( CBasePlayer *player )
{
	VPROF( "CPlayerMove::RunPreThink" );

	// Run think functions on the player
	VPROF_SCOPE_BEGIN( "player->PhysicsRunThink()" );
	if ( !player->PhysicsRunThink() )
		return;
	VPROF_SCOPE_END();

	VPROF_SCOPE_BEGIN( "g_pGameRules->PlayerThink( player )" );
	// Called every frame to let game rules do any specific think logic for the player
	g_pGameRules->PlayerThink( player );
	VPROF_SCOPE_END();

	VPROF_SCOPE_BEGIN( "player->PreThink()" );
	player->PreThink();
	VPROF_SCOPE_END();
}

//-----------------------------------------------------------------------------
// Purpose: Runs the PLAYER's thinking code if time.  There is some play in the exact time the think
//  function will be called, because it is called before any movement is done
//  in a frame.  Not used for pushmove objects, because they must be exact.
//  Returns false if the entity removed itself.
// Input  : *ent - 
//			frametime - 
//			clienttimebase - 
// Output : void CPlayerMove::RunThink
//-----------------------------------------------------------------------------
void CPlayerMove::RunThink (CBasePlayer *player, double frametime )
{
	VPROF( "CPlayerMove::RunThink" );
	int thinktick = player->GetNextThinkTick();

	if ( thinktick <= 0 || thinktick > player->m_nTickBase )
		return;
		
	//gpGlobals->curtime = thinktime;
	player->SetNextThink( TICK_NEVER_THINK );

	// Think
	player->Think();
}

//-----------------------------------------------------------------------------
// Purpose: Called after player movement
// Input  : *player - 
//			thinktime - 
//			frametime - 
//-----------------------------------------------------------------------------
void CPlayerMove::RunPostThink( CBasePlayer *player )
{
	VPROF( "CPlayerMove::RunPostThink" );

	// Run post-think
	player->PostThink();
}

void CommentarySystem_PePlayerRunCommand( CBasePlayer *player, CUserCmd *ucmd );

void CPlayerMove::PreventJumpBug(CBasePlayer *player, IMoveHelper *moveHelper)
{
    Vector mins = player->CollisionProp()->OBBMins();
    Vector maxs = player->CollisionProp()->OBBMaxs();

    Vector origin;
    VectorCopy(player->GetAbsOrigin(), origin);

    // CMomentumGameMovement::CanUnduck
    // (excluding on-ground logic)
    trace_t trace;
    Vector newOrigin;

    VectorCopy(player->GetAbsOrigin(), newOrigin);

    // If in air and letting go of crouch, make sure we can offset origin to make
    //  up for uncrouching
    Vector hullSizeNormal = VEC_HULL_MAX - VEC_HULL_MIN;
    Vector hullSizeCrouch = VEC_DUCK_HULL_MAX - VEC_DUCK_HULL_MIN;

    newOrigin += -g_pGameModeSystem->GetGameMode()->GetViewScale() * (hullSizeNormal - hullSizeCrouch);

    UTIL_TraceHull(origin, newOrigin, VEC_HULL_MIN, VEC_HULL_MAX, MASK_PLAYERSOLID, player,
                    COLLISION_GROUP_PLAYER_MOVEMENT, &trace);

    if (trace.startsolid || (trace.fraction != 1.0f))
    {
        // Can't unduck now, no fix needed
        return;
    }

    // Pretend we unducked now
    VectorCopy(newOrigin, origin);
    VectorCopy(VEC_HULL_MIN, mins);
    VectorCopy(VEC_HULL_MAX, maxs);

    Vector offset(0.0f, 0.0f, sv_considered_on_ground.GetFloat());

    // CGameMovement::TryTouchGround
    trace_t pm;
    Ray_t ray;
    ray.Init(origin, origin - offset, mins, maxs);
    UTIL_TraceRay(ray, MASK_PLAYERSOLID, player, COLLISION_GROUP_PLAYER_MOVEMENT, &pm);

    // Don't worry about CGameMovement::TryTouchGroundInQuadrants to keep this fix from duplicating too much code that will probably not matter.
    if (pm.DidHit() && pm.plane.normal[2] >= 0.7f)
    {
        // Extend collision to ground
        Vector newMins(mins.x, mins.y, mins.z - (player->GetAbsOrigin().z - pm.endpos.z));

        //Msg("[%i] PreventBounce extending mins (%.6f)\n", gpGlobals->tickcount, (newMins - mins).z);

        player->SetCollisionBounds(newMins, maxs);

        moveHelper->ProcessImpacts();
                
        // Restore normal bounds
        player->SetCollisionBounds(mins, maxs);

        // ProcessImpacts causes trigger Touch() functions to fire no matter what.
        // We still need to call ProcessImpacts() at the end of this tick, which means some Touch() functions may fire twice in one tick.
        // The basevelocity system assumes sources of basevelocity -- like trigger_push Touch() functions -- are accumulated only once per tick.
        // Removing this flag before we run ProcessImpacts() again will keep from double counting sources of basevelocity this tick.
        // Without doing this, the player would usually just get a double boost for 1 tick, but if timed very precisely it can produce a permanent double boost.
        player->RemoveFlag(FL_BASEVELOCITY);
    }
}

//-----------------------------------------------------------------------------
// Purpose: Runs movement commands for the player
// Input  : *player - 
//			*ucmd - 
//			*moveHelper - 
// Output : void CPlayerMove::RunCommand
//-----------------------------------------------------------------------------
void CPlayerMove::RunCommand ( CBasePlayer *player, CUserCmd *ucmd, IMoveHelper *moveHelper )
{
	const float playerCurTime = player->m_nTickBase * TICK_INTERVAL; 
	const float playerFrameTime = player->m_bGamePaused ? 0 : TICK_INTERVAL;
	const float flTimeAllowedForProcessing = player->ConsumeMovementTimeForUserCmdProcessing( playerFrameTime );
	if ( !player->IsBot() && ( flTimeAllowedForProcessing < playerFrameTime ) )
	{
		// Make sure that the activity in command is erased because player cheated or dropped too many packets
		double dblWarningFrequencyThrottle = sv_maxusrcmdprocessticks_warning.GetFloat();
		if ( dblWarningFrequencyThrottle >= 0 )
		{
			static double s_dblLastWarningTime = 0;
			double dblTimeNow = Plat_FloatTime();
			if ( !s_dblLastWarningTime || ( dblTimeNow - s_dblLastWarningTime >= dblWarningFrequencyThrottle ) )
			{
				s_dblLastWarningTime = dblTimeNow;
				Warning( "sv_maxusrcmdprocessticks_warning at server tick %u: Ignored client %s usrcmd (%.6f < %.6f)!\n", gpGlobals->tickcount, player->GetPlayerName(), flTimeAllowedForProcessing, playerFrameTime );
			}
		}
		return; // Don't process this command
	}

	StartCommand( player, ucmd );

	// Set globals appropriately
	gpGlobals->curtime		=  playerCurTime;
	gpGlobals->frametime	=  playerFrameTime;

	// Prevent hacked clients from sending us invalid view angles to try to get leaf server code to crash
	if ( !ucmd->viewangles.IsValid() || !IsEntityQAngleReasonable(ucmd->viewangles) )
	{
		ucmd->viewangles = vec3_angle;
	}

	// Add and subtract buttons we're forcing on the player
	ucmd->buttons |= player->m_afButtonForced;
	ucmd->buttons &= ~player->m_afButtonDisabled;

	if ( player->m_bGamePaused )
	{
		// If no clipping and cheats enabled and noclip during game enabled, then leave
		//  forwardmove and angles stuff in usercmd
		if ( player->GetMoveType() == MOVETYPE_NOCLIP &&
			 sv_cheats->GetBool() && 
			 sv_noclipduringpause.GetBool() )
		{
			gpGlobals->frametime = TICK_INTERVAL;
		}
	}

	/*
	// TODO:  We can check whether the player is sending more commands than elapsed real time
	cmdtimeremaining -= ucmd->msec;
	if ( cmdtimeremaining < 0 )
	{
	//	return;
	}
	*/

	g_pGameMovement->StartTrackPredictionErrors( player );

	CommentarySystem_PePlayerRunCommand( player, ucmd );

	// Do weapon selection
	if ( ucmd->weaponselect != 0 )
	{
		CBaseCombatWeapon *weapon = dynamic_cast< CBaseCombatWeapon * >( CBaseEntity::Instance( ucmd->weaponselect ) );
		if ( weapon )
		{
			VPROF( "player->SelectItem()" );
			player->SelectItem( weapon->GetName(), ucmd->weaponsubtype );
		}
	}

	// Latch in impulse.
	if ( ucmd->impulse )
	{
		player->m_nImpulse = ucmd->impulse;
	}

	// Update player input button states
	VPROF_SCOPE_BEGIN( "player->UpdateButtonState" );
	player->UpdateButtonState( ucmd->buttons );
	VPROF_SCOPE_END();

	CheckMovingGround( player, TICK_INTERVAL );

	g_pMoveData->m_vecOldAngles = player->pl.v_angle.Get();

	// Copy from command to player unless game .dll has set angle using fixangle
	if ( player->pl.fixangle == FIXANGLE_NONE )
	{
		player->pl.v_angle.GetForModify() = ucmd->viewangles;
	}
	else if( player->pl.fixangle == FIXANGLE_RELATIVE )
	{
		player->pl.v_angle.GetForModify() = ucmd->viewangles + player->pl.anglechange;
	}

	// Call standard client pre-think
	RunPreThink( player );

	// Call Think if one is set
	RunThink( player, TICK_INTERVAL );

	player->m_vecOldOrigin = player->GetAbsOrigin();

	CMomentumPlayer* pMomPlayer = static_cast<CMomentumPlayer*>(player);

	// If the player unducks while in the air, but their feet end up being within sv_considered_on_ground above
	// standable ground, they can unduck and jump on the same tick to perform a jumpbug. This allows the player to get
	// grounded and jump, but because triggers are first checked after all player movement is done, it is possible to
	// exit a trigger the player unducked into with the jump. This has do be done here because ProcessImpacts()
	// uses the player entity and not the move data that is used in the game movement.
	if (sv_ground_trigger_fix.GetBool() &&
		player->GetGroundEntity() == nullptr && !pMomPlayer->m_CurrentSlideTrigger &&                         // In air
		player->GetFlags() & FL_DUCKING && player->m_afButtonReleased & IN_DUCK &&                            // Tries to unduck
		(player->m_afButtonPressed & IN_JUMP || (pMomPlayer->HasAutoBhop() && player->m_nButtons & IN_JUMP))) // Tries to jump
	{
		PreventJumpBug(player, moveHelper);
	}

	// Setup input.
	SetupMove( player, ucmd, moveHelper, g_pMoveData );

	// Let the game do the movement.
	VPROF( "g_pGameMovement->ProcessMovement()" );
	Assert( g_pGameMovement );
	g_pGameMovement->ProcessMovement( player, g_pMoveData );

	// Copy output
	FinishMove( player, ucmd, g_pMoveData );

	// If we have to restore the view angle then do so right now
	if ( !player->IsBot() && ( gpGlobals->tickcount - player->GetLockViewanglesTickNumber() < sv_maxusrcmdprocessticks_holdaim.GetInt() ) )
	{
		player->pl.v_angle.GetForModify() = player->GetLockViewanglesData();
	}

	// If the player is grounded, there is the possibility that they are bit above the ground and therefore might be
	// above a trigger. The player could avoid this trigger by doing a jump, so to prevent this we extend the player
	// collision by how much they are above the ground when checking for triggers
    Vector mins = player->CollisionProp()->OBBMins();
    Vector maxs = player->CollisionProp()->OBBMaxs();

    if (sv_ground_trigger_fix.GetBool() && player->GetGroundEntity() != nullptr &&
        (g_pMoveData->m_vecGroundPosition - player->GetAbsOrigin()).z < mins.z)
    {
        Vector newMins(mins.x, mins.y, (g_pMoveData->m_vecGroundPosition - player->GetAbsOrigin()).z);

        //Msg("[%i] Trigger Fix: setting min z to %.6f (player z: %.6f, ground z: %.6f)\n", gpGlobals->tickcount,
        //    newMins.z, player->GetAbsOrigin().z, g_pMoveData->m_vecGroundPosition.z);

        player->SetCollisionBounds(newMins, maxs);
        
        VPROF_SCOPE_BEGIN("moveHelper->ProcessImpacts");
        moveHelper->ProcessImpacts();
        VPROF_SCOPE_END();

        player->SetCollisionBounds(mins, maxs);
    }
	else
	{
		// Let server invoke any needed impact functions
		VPROF_SCOPE_BEGIN( "moveHelper->ProcessImpacts" );
		moveHelper->ProcessImpacts();
		VPROF_SCOPE_END();
	}

	RunPostThink( player );

	g_pGameMovement->FinishTrackPredictionErrors( player );

	FinishCommand( player );

	// Let time pass
	if ( gpGlobals->frametime > 0 )
	{
		player->m_nTickBase++;
	}
}
