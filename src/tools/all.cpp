#include "tools/all.h"

#include "tools/color.h"
#include "tools/media.h"
#include "tools/subtitles.h"
#include "tools/timeline.h"

namespace et::tools {

void register_all_tools(mcp::Server& server, std::shared_ptr<Session> session) {
  register_media_tools(server);
  register_timeline_tools(server, session);
  register_subtitle_tools(server, session);
  register_color_tools(server, session);
}

}  // namespace et::tools
