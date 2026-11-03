#include "PluginHostApi.h"

#include "PluginVersion.h"

namespace casioemu::plugin {

int HostPluginApiVersion() {
	return kPluginHostApiVersion;
}

} // namespace casioemu::plugin
