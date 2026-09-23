/*
 * mcp_car_tools.h — MCP (Model Context Protocol) tools for XiaoZhi.
 *
 * Every tool validates its inputs and goes through the safe car command
 * layer.  Direct access to g.tgt_l, g.tgt_r, g.mode, g.estop is NOT
 * exposed.  Dangerous actions (factory reset, motor start, E-stop clear)
 * are intentionally absent.
 */

#ifndef MCP_CAR_TOOLS_H
#define MCP_CAR_TOOLS_H

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Register all car tools with the given MCP engine. */
esp_err_t mcp_car_tools_register(void *mcp_engine);

/** Deregister (called on shutdown). */
void      mcp_car_tools_unregister(void);

#ifdef __cplusplus
}
#endif

#endif /* MCP_CAR_TOOLS_H */
