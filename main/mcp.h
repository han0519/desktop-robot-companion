/*
 * mcp.h — 小智 MCP 服务端 (设备侧)
 *
 * 小智服务端会通过 WebSocket 下发:
 *     { "session_id":"..", "type":"mcp", "payload": <JSON-RPC 2.0 请求> }
 * 设备实现 JSON-RPC 方法 initialize / tools/list / tools/call / ping,
 * 把工具能力暴露给大模型 —— AI 就能"自己"开灯、调节亮度、读温湿度。
 *
 * 本模块只负责: 解析 payload → 执行 → 生成要回发的完整 JSON 文本。
 * 发送由 xiaozhi.c 负责(它持有 WebSocket 句柄)。
 */
#pragma once

/*
 * 处理一条 MCP 消息。
 * @param payload_json 服务端下发的 payload 对象(已序列化成 JSON 文本)
 * @param session_id   当前会话 id, 原样回填(可以传 NULL)
 * @return 需要回给服务端的完整 JSON 文本(调用者负责 free); 无需回复时返回 NULL
 */
char *mcp_handle_payload(const char *payload_json, const char *session_id);
