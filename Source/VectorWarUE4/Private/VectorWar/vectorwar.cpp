#include "vectorwar.h"
#include <windows.h>
#include <gl/gl.h>
#include <gl/glu.h>
#include <math.h>
#include <stdio.h>
#include "gamestate.h"

// UE4: allow Windows platform types to avoid naming collisions
//  this must be undone at the bottom of this file
#include "Windows/AllowWindowsPlatformTypes.h"
#include "Windows/prewindowsapi.h"

#include <timeapi.h>

//#define SYNC_TEST    // test: turn on synctest
#define MAX_PLAYERS     64

GGPOSession *ggpo = NULL;

/* 
 * Simple checksum function stolen from wikipedia:
 *
 *   http://en.wikipedia.org/wiki/Fletcher%27s_checksum
 */

int
fletcher32_checksum(short *data, size_t len)
{
   int sum1 = 0xffff, sum2 = 0xffff;

   while (len) {
      size_t tlen = len > 360 ? 360 : len;
      len -= tlen;
      do {
         sum1 += *data++;
         sum2 += sum1;
      } while (--tlen);
      sum1 = (sum1 & 0xffff) + (sum1 >> 16);
      sum2 = (sum2 & 0xffff) + (sum2 >> 16);
   }

   /* Second reduction step to reduce sums to 16 bits */
   sum1 = (sum1 & 0xffff) + (sum1 >> 16);
   sum2 = (sum2 & 0xffff) + (sum2 >> 16);
   return sum2 << 16 | sum1;
}

/*
 * vw_begin_game_callback --
 *
 * The begin game callback.  We don't need to do anything special here,
 * so just return true.
 */
bool __cdecl
vw_begin_game_callback(const char *)
{
   return true;
}

/*
 * vw_on_event_callback --
 *
 * Notification from GGPO that something has happened.  Update the status
 * text at the bottom of the screen to notify the user.
 */
bool __cdecl
vw_on_event_callback(GGPOEvent *info)
{
   int progress;
   switch (info->code) {
   case GGPO_EVENTCODE_CONNECTED_TO_PEER:
      ngs.SetConnectState(info->u.connected.player, EPlayerConnectState::Synchronizing);
      break;
   case GGPO_EVENTCODE_SYNCHRONIZING_WITH_PEER:
      progress = 100 * info->u.synchronizing.count / info->u.synchronizing.total;
      ngs.UpdateConnectProgress(info->u.synchronizing.player, progress);
      break;
   case GGPO_EVENTCODE_SYNCHRONIZED_WITH_PEER:
      ngs.UpdateConnectProgress(info->u.synchronized.player, 100);
      break;
   case GGPO_EVENTCODE_RUNNING:
      ngs.SetConnectState(EPlayerConnectState::Running);
      break;
   case GGPO_EVENTCODE_CONNECTION_INTERRUPTED:
      ngs.SetDisconnectTimeout(info->u.connection_interrupted.player,
                               timeGetTime(),
                               info->u.connection_interrupted.disconnect_timeout);
      break;
   case GGPO_EVENTCODE_CONNECTION_RESUMED:
      ngs.SetConnectState(info->u.connection_resumed.player, EPlayerConnectState::Running);
      break;
   case GGPO_EVENTCODE_DISCONNECTED_FROM_PEER:
      ngs.SetConnectState(info->u.disconnected.player, EPlayerConnectState::Disconnected);
      break;
   case GGPO_EVENTCODE_TIMESYNC:
      Sleep(1000 * info->u.timesync.frames_ahead / 60);
      break;
   }
   return true;
}


/*
 * vw_advance_frame_callback --
 *
 * Notification from GGPO we should step foward exactly 1 frame
 * during a rollback.
 */
bool __cdecl
vw_advance_frame_callback(int)
{
   int inputs[MAX_SHIPS] = { 0 };
   int disconnect_flags;

   // Make sure we fetch new inputs from GGPO and use those to update
   // the game state instead of reading from the keyboard.
   GGPONet::ggpo_synchronize_input(ggpo, (void *)inputs, sizeof(int) * MAX_SHIPS, &disconnect_flags);
   VectorWar_AdvanceFrame(inputs, disconnect_flags);
   return true;
}

/*
 * vw_load_game_state_callback --
 *
 * Makes our current state match the state passed in by GGPO.
 */
bool __cdecl
vw_load_game_state_callback(unsigned char *buffer, int len)
{
   memcpy(&gs, buffer, len);
   return true;
}

/*
 * vw_save_game_state_callback --
 *
 * Save the current state to a buffer and return it to GGPO via the
 * buffer and len parameters.
 */
bool __cdecl
vw_save_game_state_callback(unsigned char **buffer, int *len, int *checksum, int)
{
   *len = sizeof(gs);
   *buffer = (unsigned char *)malloc(*len);
   if (!*buffer) {
      return false;
   }
   memcpy(*buffer, &gs, *len);
   *checksum = fletcher32_checksum((short *)*buffer, *len / 2);
   return true;
}

/*
 * vw_log_game_state --
 *
 * Log the gamestate.  Used by the synctest debugging tool.
 */
bool __cdecl
vw_log_game_state(char *filename, unsigned char *buffer, int)
{
   FILE* fp = nullptr;
   fopen_s(&fp, filename, "w");
   if (fp) {
      GameState *gamestate = (GameState *)buffer;
      fprintf(fp, "GameState object.\n");
      fprintf(fp, "  bounds: %ld,%ld x %ld,%ld.\n", gamestate->_bounds.left, gamestate->_bounds.top,
              gamestate->_bounds.right, gamestate->_bounds.bottom);
      fprintf(fp, "  num_ships: %d.\n", gamestate->_num_ships);
      for (int i = 0; i < gamestate->_num_ships; i++) {
         Ship *ship = gamestate->_ships + i;
         fprintf(fp, "  ship %d position:  %.4f, %.4f\n", i, ship->position.x, ship->position.y);
         fprintf(fp, "  ship %d velocity:  %.4f, %.4f\n", i, ship->velocity.dx, ship->velocity.dy);
         fprintf(fp, "  ship %d radius:    %d.\n", i, ship->radius);
         fprintf(fp, "  ship %d heading:   %d.\n", i, ship->heading);
         fprintf(fp, "  ship %d health:    %d.\n", i, ship->health);
         fprintf(fp, "  ship %d speed:     %d.\n", i, ship->speed);
         fprintf(fp, "  ship %d cooldown:  %d.\n", i, ship->cooldown);
         fprintf(fp, "  ship %d score:     %d.\n", i, ship->score);
         for (int j = 0; j < MAX_BULLETS; j++) {
            Bullet *bullet = ship->bullets + j;
            fprintf(fp, "  ship %d bullet %d: %.2f %.2f -> %.2f %.2f.\n", i, j,
                    bullet->position.x, bullet->position.y,
                    bullet->velocity.dx, bullet->velocity.dy);
         }
      }
      fclose(fp);
   }
   return true;
}

/*
 * vw_free_buffer --
 *
 * Free a save state buffer previously returned in vw_save_game_state_callback.
 */
void __cdecl 
vw_free_buffer(void *buffer)
{
   free(buffer);
}


/*
 * VectorWar_Init --
 *
 * Initialize the vector war game.  This initializes the game state and
 * creates a new network session.
 */
void
VectorWarHost::VectorWar_Init(unsigned short localport, int num_players, GGPOPlayer *players, int num_spectators)
{
   GGPOErrorCode result;

   // Initialize the game state
   gs.Init(num_players);
   ngs.num_players = num_players;

   // Fill in a ggpo callbacks structure to pass to start_session.
   GGPOSessionCallbacks cb = { 0 };
   cb.begin_game      = vw_begin_game_callback;
   cb.advance_frame	 = vw_advance_frame_callback;
   cb.load_game_state = vw_load_game_state_callback;
   cb.save_game_state = vw_save_game_state_callback;
   cb.free_buffer     = vw_free_buffer;
   cb.on_event        = vw_on_event_callback;
   cb.log_game_state  = vw_log_game_state;

#if defined(SYNC_TEST)
   result = GGPONet::ggpo_start_synctest(&ggpo, &cb, "vectorwar", num_players, sizeof(int), 1);
#else
   result = GGPONet::ggpo_start_session(&ggpo, &cb, "vectorwar", num_players, sizeof(int), localport);
#endif

   // automatically disconnect clients after 3000 ms and start our count-down timer
   // for disconnects after 1000 ms.   To completely disable disconnects, simply use
   // a value of 0 for ggpo_set_disconnect_timeout.
   GGPONet::ggpo_set_disconnect_timeout(ggpo, 3000);
   GGPONet::ggpo_set_disconnect_notify_start(ggpo, 1000);

   int i;
   for (i = 0; i < num_players + num_spectators; i++) {
      GGPOPlayerHandle handle;
      result = GGPONet::ggpo_add_player(ggpo, players + i, &handle);
      ngs.players[i].handle = handle;
      ngs.players[i].type = players[i].type;
      if (players[i].type == EGGPOPlayerType::LOCAL) {
         ngs.players[i].connect_progress = 100;
         ngs.local_player_handle = handle;
         ngs.SetConnectState(handle, EPlayerConnectState::Connecting);
         GGPONet::ggpo_set_frame_delay(ggpo, handle, FRAME_DELAY);
      } else {
         ngs.players[i].connect_progress = 0;
      }
   }

   //ggpoutil_perfmon_init(hwnd);

   GGPONet::ggpo_try_synchronize_local(ggpo);
}

/*
 * VectorWar_InitSpectator --
 *
 * Create a new spectator session
 */
void
VectorWarHost::VectorWar_InitSpectator(unsigned short localport, int num_players, char *host_ip, unsigned short host_port)
{
   GGPOErrorCode result;

   // Initialize the game state
   gs.Init(num_players);
   ngs.num_players = num_players;

   // Fill in a ggpo callbacks structure to pass to start_session.
   GGPOSessionCallbacks cb = { 0 };
   cb.begin_game      = vw_begin_game_callback;
   cb.advance_frame	  = vw_advance_frame_callback;
   cb.load_game_state = vw_load_game_state_callback;
   cb.save_game_state = vw_save_game_state_callback;
   cb.free_buffer     = vw_free_buffer;
   cb.on_event        = vw_on_event_callback;
   cb.log_game_state  = vw_log_game_state;

   result = GGPONet::ggpo_start_spectating(&ggpo, &cb, "vectorwar", num_players, sizeof(int), localport, host_ip, host_port);

   //ggpoutil_perfmon_init(hwnd);
}


/*
 * VectorWar_DisconnectPlayer --
 *
 * Disconnects a player from this session.
 */

void
VectorWar_DisconnectPlayer(int player)
{
   if (player < ngs.num_players) {
      char logbuf[128];
      GGPOErrorCode result = GGPONet::ggpo_disconnect_player(ggpo, ngs.players[player].handle);
      if (GGPO_SUCCEEDED(result)) {
         sprintf_s(logbuf, ARRAYSIZE(logbuf), "Disconnected player %d.\n", player);
      } else {
         sprintf_s(logbuf, ARRAYSIZE(logbuf), "Error while disconnecting player (err:%d).\n", result);
      }
   }
}

/*
 * VectorWar_AdvanceFrame --
 *
 * Advances the game state by exactly 1 frame using the inputs specified
 * for player 1 and player 2.
 */
void VectorWar_AdvanceFrame(int inputs[], int disconnect_flags)
{
   gs.Update(inputs, disconnect_flags);

   // update the checksums to display in the top of the window.  this
   // helps to detect desyncs.
   ngs.now.framenumber = gs._framenumber;
   ngs.now.checksum = fletcher32_checksum((short *)&gs, sizeof(gs) / 2);
   if ((gs._framenumber % 90) == 0) {
      ngs.periodic = ngs.now;
   }

   // Notify ggpo that we've moved forward exactly 1 frame.
   GGPONet::ggpo_advance_frame(ggpo);

   // Update the performance monitor display.
   GGPOPlayerHandle handles[MAX_PLAYERS];
   int count = 0;
   for (int i = 0; i < ngs.num_players; i++) {
      if (ngs.players[i].type == EGGPOPlayerType::REMOTE) {
         handles[count++] = ngs.players[i].handle;
      }
   }
   //ggpoutil_perfmon_update(ggpo, handles, count);
}

/*
 * VectorWar_RunFrame --
 *
 * Run a single frame of the game.
 */
void
VectorWarHost::VectorWar_RunFrame(int local_input)
{
  GGPOErrorCode result = GGPO_OK;
  int disconnect_flags;
  int inputs[MAX_SHIPS] = { 0 };

  if (ngs.local_player_handle != GGPO_INVALID_HANDLE) {
#if defined(SYNC_TEST)
     local_input = rand(); // test: use random inputs to demonstrate sync testing
#endif
     result = GGPONet::ggpo_add_local_input(ggpo, ngs.local_player_handle, &local_input, sizeof(local_input));
  }

   // synchronize these inputs with ggpo.  If we have enough input to proceed
   // ggpo will modify the input list with the correct inputs to use and
   // return 1.
  if (GGPO_SUCCEEDED(result)) {
     result = GGPONet::ggpo_synchronize_input(ggpo, (void *)inputs, sizeof(int) * MAX_SHIPS, &disconnect_flags);
     if (GGPO_SUCCEEDED(result)) {
         // inputs[0] and inputs[1] contain the inputs for p1 and p2.  Advance
         // the game by 1 frame using those inputs.
         VectorWar_AdvanceFrame(inputs, disconnect_flags);
     }
  }
}

/*
 * VectorWar_Idle --
 *
 * Spend our idle time in ggpo so it can use whatever time we have left over
 * for its internal bookkeeping.
 */
void
VectorWarHost::VectorWar_Idle(int time)
{
    GGPONet::ggpo_idle(ggpo, time);
}

void
VectorWarHost::VectorWar_Exit()
{
   memset(&gs, 0, sizeof(gs));
   memset(&ngs, 0, sizeof(ngs));

   if (ggpo) {
       GGPONet::ggpo_close_session(ggpo);
      ggpo = NULL;
   }
}

TArray<FGGPONetworkStats>
VectorWarHost::VectorWar_GetNetworkStats()
{
    GGPOPlayerHandle RemoteHandles[MAX_PLAYERS];
    int Count = 0;
    for (int i = 0; i < ngs.num_players; i++) {
        if (ngs.players[i].type == EGGPOPlayerType::REMOTE) {
            RemoteHandles[Count++] = ngs.players[i].handle;
        }
    }

    TArray<FGGPONetworkStats> Result;
    for (int i = 0; i < Count; i++)
    {
        FGGPONetworkStats Stats = { 0 };
        GGPONet::ggpo_get_network_stats(ggpo, RemoteHandles[i], &Stats);
        Result.Add(Stats);
    }

    return Result;
}

// UE4: disallow windows platform types
//  this was enabled at the top of the file
#include "Windows/PostWindowsApi.h"
#include "Windows/HideWindowsPlatformTypes.h"

