# ECM IP配置修改失败问题总结

## 问题描述
通过CH9329命令修改ECM IP配置后，新IP不生效，旧IP仍然可以访问。

## 测试步骤
1. 发送SET_ECM_IP_CONFIG命令（通过TCP）
2. 配置Mac网络接口到新网段（192.168.22.x）
3. 测试新IP连通性 - 失败
4. 测试旧IP连通性 - 成功

## 预期行为
- SET命令应该写入Flash并更新运行时配置
- 设备应该使用新IP地址响应

## 实际行为
- 旧IP（192.168.11.3）仍然可以ping通
- 新IP（192.168.22.3）无法ping通
- 说明SET命令没有生效

## 可能原因
1. TCP包没有被ECM bridge识别为CH9329命令
2. CH9329命令校验和错误
3. Flash写入失败
4. ECM_Bridge_UpdateIPConfig()没有被调用或调用失败
5. 运行时IP变量没有更新

## 需要检查的代码
1. ecm_bridge.c - handle_host_control_frame()
2. ecm_bridge.c - tcp_payload_from_ipv4_frame()
3. ecm_bridge.c - ch9329_payload_valid()
4. usbd_compostie_km.c - CMD_SET_ECM_IP_CONFIG处理
5. ecm_ip_config.c - ECM_IPConfig_Set()
6. ecm_bridge.c - ECM_Bridge_UpdateIPConfig()
