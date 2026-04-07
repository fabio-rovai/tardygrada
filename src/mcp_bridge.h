/*
 * Tardygrada — MCP Bridge
 * Bridges Claude Code's MCP protocol (stdin/stdout JSON-RPC)
 * to the Tardygrada daemon (Unix socket JSON-line).
 *
 * Claude Code <--MCP(stdin/stdout)--> tardy mcp-bridge <--JSON(socket)--> tardy daemon
 */

#ifndef TARDY_MCP_BRIDGE_H
#define TARDY_MCP_BRIDGE_H

/* Run the MCP bridge — blocks, reads stdin, proxies to daemon, writes stdout.
 * Returns 0 on clean shutdown, 1 on error. */
int tardy_mcp_bridge_run(void);

#endif /* TARDY_MCP_BRIDGE_H */
