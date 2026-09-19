#pragma once

#include "seriona/control/control_contracts.h"
#include "seriona/scanner/scanner_contracts.h"

#include <map>
#include <string>
#include <vector>

namespace seriona::control {

// 每个文件夹内子节点顺序的权威排序（决策⑦：排序施加在控制层持有的树副本上，
// 前端与播放都消费这棵已排序的树，「可见序 == 播放序」由构造保证）。
//
// 语义：
//  - 仅当 rulesByFolderNodeId 含该节点 id 时排序其 childNodeIds；否则保持树序（决策⑥）。
//  - 树节点 id 唯一（树不变式，见 playback_context_builder 的 indexNodes），故 key 用
//    folderNodeId 即可，无需 rootPath。
//  - 同键相等时保持原树序（stable_sort），使结果确定。
void sortTreeChildOrderByRules(scanner::PlaylistTreeSnapshot& tree,
                               const std::map<std::string, std::vector<FolderSortRule>>& rulesByFolderNodeId);

}
