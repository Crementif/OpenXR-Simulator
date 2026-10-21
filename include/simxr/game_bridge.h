#ifndef SIMXR_GAME_BRIDGE_H
#define SIMXR_GAME_BRIDGE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SIMXR_GAME_BRIDGE_VERSION 1u
#define SIMXR_GAME_CAN_RESET 1u
#define SIMXR_GAME_CAN_STEP 2u
#define SIMXR_GAME_CAN_COMMAND 4u
#define SIMXR_GAME_RESET 1u
#define SIMXR_GAME_STEP 2u
#define SIMXR_GAME_COMMAND 3u
#define SIMXR_GAME_PAYLOAD_CAPACITY 4096u

typedef struct SimxrGameCommand
{
    uint64_t id;
    uint32_t kind;
    uint32_t payloadLength;
    char payload[SIMXR_GAME_PAYLOAD_CAPACITY];
} SimxrGameCommand;

typedef struct SimxrGameBridgeApi
{
    uint32_t version;
    uint32_t size;
    void* context;
    /* Register once for the game's lifetime. No callbacks run on XR threads. */
    uint64_t (*registerGame)(void* context, uint32_t capabilities);
    void (*unregisterGame)(void* context, uint64_t game);
    /* Poll ONLY at a game-defined safe update point. One means an actual command. */
    int (*poll)(void* context, uint64_t game, SimxrGameCommand* command);
    /* Acknowledge only AFTER actual game-side application, including failures. */
    int (*complete)(void* context, uint64_t game, uint64_t command, int success,
                    const char* result, uint32_t length);
    /* Publish bounded game-specific telemetry/checkpoints independently of XR. */
    int (*publish)(void* context, uint64_t game, const char* telemetry, uint32_t length);
    /* Before unloading a runtime used only through this bridge, stop its workers
     * outside the DLL loader lock. Requires all XR instances destroyed; after a
     * successful shutdown this runtime cannot create another XR instance. */
    int (*shutdown)(void* context);
} SimxrGameBridgeApi;

/* Resolve this optional symbol from the simulator's loaded runtime library.
 * Zero means unsupported. The API/context expire when that library unloads.
 * The caller initializes size=sizeof(SimxrGameBridgeApi) before requesting v1.
 * The bridge is never required for XR frame control, capture or image export. */
typedef int (*PFN_simxrGetGameBridgeApi)(uint32_t version, SimxrGameBridgeApi* api);

#ifdef __cplusplus
}
#endif
#endif
