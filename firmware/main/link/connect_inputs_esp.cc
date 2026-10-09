#include "connect_inputs_esp.h"

#include "board.h"
#include "settings.h"
#include "system_info.h"

namespace stackchan::link {

ConnectInputs ReadConnectInputs() {
    ConnectInputs in;
    {
        Settings settings("websocket", false);
        in.targets.nvs_url = settings.GetString("url");
        in.targets.nvs_fallback = settings.GetString("fallback_url");
        in.nvs_token = settings.GetString("token");
        in.nvs_version = settings.GetInt("version");
    }
#ifdef CONFIG_DEFAULT_WEBSOCKET_URL
    in.targets.kconfig_url = CONFIG_DEFAULT_WEBSOCKET_URL;
#ifdef CONFIG_FORCE_DEFAULT_WEBSOCKET_URL
    in.targets.force_kconfig = !in.targets.kconfig_url.empty();
#endif
#endif
#ifdef CONFIG_DEFAULT_WEBSOCKET_FALLBACK_URL
    in.targets.kconfig_fallback = CONFIG_DEFAULT_WEBSOCKET_FALLBACK_URL;
#endif
#ifdef CONFIG_DEFAULT_WEBSOCKET_TOKEN
    in.kconfig_token = CONFIG_DEFAULT_WEBSOCKET_TOKEN;
#endif
    in.device_id = SystemInfo::GetMacAddress();
    in.client_id = Board::GetInstance().GetUuid();
    return in;
}

}  // namespace stackchan::link
