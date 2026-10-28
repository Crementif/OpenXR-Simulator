#include "app/mcp/McpPch.h"
#include "app/mcp/McpSocket.h"

namespace simxr::app
{
void ConfigureMcpListener(socket_t socket)
{
#ifdef _WIN32
	if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, 1))
		throw std::runtime_error("Cannot make the local MCP listener exclusive");
#else
	// Permit a restarted listener to reclaim connections in TIME_WAIT; SO_REUSEPORT
	// remains disabled so another live listener cannot share this address.
	if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEADDR, 1))
		throw std::runtime_error("Cannot configure the local MCP listener");
#ifdef SO_REUSEPORT
	if (!httplib::set_socket_opt(socket, SOL_SOCKET, SO_REUSEPORT, 0))
		throw std::runtime_error("Cannot make the local MCP listener exclusive");
#endif
#endif
}
}
