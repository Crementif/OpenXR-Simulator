-- Optional: the game must register SIMXR_GAME_CAN_RESET and poll at safe update points.
local game = simxr.game_state()
assert(tonumber(game.capabilities) % 2 == 1, "A cooperating game with reset support is required")
local operation, identity = simxr.game_command(1, "checkpoint:start")
local acknowledged = simxr.await_game(identity, operation)
simxr.checkpoint("game acknowledged reset", acknowledged.result ~= "")
