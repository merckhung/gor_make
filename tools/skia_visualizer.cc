#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <functional>

#include <SDL2/SDL.h>

#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkFont.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"
#include "include/core/SkRect.h"
#include "include/core/SkSurface.h"
#include "include/core/SkBitmap.h"
#include "include/core/SkFontMgr.h"
#include "include/ports/SkFontMgr_empty.h"

namespace {

struct Node {
  std::string name;
  std::string type;
  std::string src_dir;
  int srcs_count = 0;
  int level = 0;
  float x = 0.0f;
  float y = 0.0f;
  float width = 150.0f;
  float height = 44.0f;
  SkColor color = SK_ColorGRAY;
  std::vector<std::string> raw_deps;

  bool is_folder = false;
  bool expanded = false;
  int parent_idx = -1;
  std::vector<int> children;
};

struct Edge {
  int from_index;
  int to_index;
  std::string dep_type;
  SkColor color;
};

struct Graph {
  std::vector<Node> nodes;
  std::vector<Edge> edges;
  std::unordered_map<std::string, int> name_to_index;
  std::string format_name;
  int total_items = 0;
};

struct VisibleGraph {
  std::vector<int> visible_node_indices;
  std::vector<Edge> visible_edges;
};

// Color mapping for target types
SkColor GetTypeColor(const std::string& type) {
  if (type == "cc_binary" || type == "executable" || type == "rust_binary")
    return SkColorSetRGB(76, 175, 80);    // Green
  if (type == "cc_library" || type == "cc_library_shared" || type == "shared_library" || type == "component")
    return SkColorSetRGB(33, 150, 243);   // Blue
  if (type == "cc_library_static" || type == "static_library" || type == "rust_library")
    return SkColorSetRGB(156, 39, 176);   // Purple
  if (type == "cc_library_headers" || type == "config" || type == "header_libs")
    return SkColorSetRGB(255, 152, 0);    // Orange
  if (type == "cc_test" || type == "test" || type == "rust_test")
    return SkColorSetRGB(244, 67, 54);    // Red
  if (type == "source_set")
    return SkColorSetRGB(0, 150, 136);    // Teal
  if (type == "action" || type == "genrule" || type == "action_foreach")
    return SkColorSetRGB(96, 125, 139);   // Blue Grey
  if (type == "group")
    return SkColorSetRGB(158, 158, 158);  // Grey
  return SkColorSetRGB(120, 144, 156);     // Default Slate
}

// Simple JSON extraction helper
std::string ExtractJsonString(const std::string& str, size_t& pos) {
  size_t start = str.find('"', pos);
  if (start == std::string::npos) return "";
  size_t end = start + 1;
  while (end < str.size()) {
    if (str[end] == '\\') {
      end += 2;
      continue;
    }
    if (str[end] == '"') break;
    end++;
  }
  pos = end + 1;
  return str.substr(start + 1, end - start - 1);
}

std::vector<std::string> ExtractJsonArray(const std::string& str, size_t& pos) {
  std::vector<std::string> result;
  size_t open_bracket = str.find('[', pos);
  if (open_bracket == std::string::npos) return result;
  size_t close_bracket = str.find(']', open_bracket);
  if (close_bracket == std::string::npos) return result;

  pos = open_bracket + 1;
  while (pos < close_bracket) {
    size_t next_quote = str.find('"', pos);
    if (next_quote == std::string::npos || next_quote > close_bracket) break;
    pos = next_quote;
    result.push_back(ExtractJsonString(str, pos));
  }
  pos = close_bracket + 1;
  return result;
}

bool ParseGorMakeJson(const std::string& filepath, Graph& graph) {
  std::ifstream file(filepath);
  if (!file.is_open()) return false;

  std::stringstream ss;
  ss << file.rdbuf();
  std::string content = ss.str();

  // Determine format
  if (content.find("\"format\": \"android.bp\"") != std::string::npos ||
      content.find("\"modules\":") != std::string::npos) {
    graph.format_name = "Android.bp (Soong Target Tree)";
  } else if (content.find("\"format\": \"build.gn\"") != std::string::npos ||
             content.find("\"targets\":") != std::string::npos) {
    graph.format_name = "Chromium (BUILD.gn Target Tree)";
  } else {
    graph.format_name = "Target Relationship Tree";
  }

  // Parse items
  size_t pos = 0;
  while ((pos = content.find("\"name\":", pos)) != std::string::npos) {
    pos += 7;
    Node node;
    node.name = ExtractJsonString(content, pos);
    if (node.name.empty()) continue;

    // Create a localized view of the current node to prevent O(N^2) searching
    size_t chunk_end = content.find("\"name\":", pos);
    if (chunk_end == std::string::npos) chunk_end = content.size();
    std::string chunk = content.substr(pos, chunk_end - pos);

    // Type
    size_t type_pos = chunk.find("\"type\":");
    if (type_pos != std::string::npos) {
      type_pos += 7;
      node.type = ExtractJsonString(chunk, type_pos);
    }

    // Src dir
    size_t srcdir_pos = chunk.find("\"src_dir\":");
    if (srcdir_pos != std::string::npos) {
      srcdir_pos += 10;
      node.src_dir = ExtractJsonString(chunk, srcdir_pos);
    }

    // Dependencies (collect all types)
    std::vector<std::string> dep_keys = {
      "\"shared_libs\":", "\"static_libs\":", "\"whole_static_libs\":",
      "\"header_libs\":", "\"deps\":", "\"public_deps\":"
    };

    for (const auto& key : dep_keys) {
      size_t kpos = chunk.find(key);
      if (kpos != std::string::npos) {
        kpos += key.size();
        auto arr = ExtractJsonArray(chunk, kpos);
        node.raw_deps.insert(node.raw_deps.end(), arr.begin(), arr.end());
      }
    }

    pos = chunk_end;

    node.color = GetTypeColor(node.type);
    graph.name_to_index[node.name] = (int)graph.nodes.size();
    graph.nodes.push_back(node);
  }

  // Build edges
  for (int i = 0; i < (int)graph.nodes.size(); ++i) {
    for (const auto& dep_name : graph.nodes[i].raw_deps) {
      auto it = graph.name_to_index.find(dep_name);
      if (it != graph.name_to_index.end()) {
        Edge edge;
        edge.from_index = i;
        edge.to_index = it->second;
        edge.dep_type = "dep";
        edge.color = SkColorSetARGB(180, 33, 150, 243);
        graph.edges.push_back(edge);
      }
    }
  }

  graph.total_items = (int)graph.nodes.size();
  std::cout << "Parsed " << graph.nodes.size() << " nodes and "
            << graph.edges.size() << " edges from " << filepath << std::endl;
  return !graph.nodes.empty();
}

void BuildFolderHierarchy(Graph& graph) {
  std::unordered_map<std::string, int> dir_to_node_idx;
  int original_target_count = (int)graph.nodes.size();

  std::string common_prefix = "";
  if (original_target_count > 0) {
      for (int i = 0; i < original_target_count; ++i) {
          if (!graph.nodes[i].src_dir.empty() && graph.nodes[i].src_dir != "/") {
              common_prefix = graph.nodes[i].src_dir;
              break;
          }
      }
      for (int i = 0; i < original_target_count; ++i) {
          if (graph.nodes[i].src_dir.empty() || graph.nodes[i].src_dir == "/") continue;
          size_t j = 0;
          while (j < common_prefix.size() && j < graph.nodes[i].src_dir.size() && 
                 common_prefix[j] == graph.nodes[i].src_dir[j]) {
              j++;
          }
          common_prefix = common_prefix.substr(0, j);
      }
      // Trim to the last complete directory boundary
      size_t last_slash = common_prefix.find_last_of('/');
      if (last_slash != std::string::npos && last_slash < common_prefix.size()) {
          common_prefix = common_prefix.substr(0, last_slash);
      }
  }

  std::cerr << "Computed source root prefix: " << common_prefix << std::endl;

  for (int i = 0; i < original_target_count; ++i) {
      if (!graph.nodes[i].src_dir.empty() && graph.nodes[i].src_dir.find(common_prefix) == 0) {
          graph.nodes[i].src_dir = graph.nodes[i].src_dir.substr(common_prefix.size());
          if (!graph.nodes[i].src_dir.empty() && graph.nodes[i].src_dir[0] == '/') {
              graph.nodes[i].src_dir = graph.nodes[i].src_dir.substr(1);
          }
      }
  }

  // Recursive folder creator
  std::function<int(const std::string&)> get_or_create_folder = [&](const std::string& path) -> int {
      if (path.empty() || path == "/") return -1;
      auto it = dir_to_node_idx.find(path);
      if (it != dir_to_node_idx.end()) return it->second;

      size_t last_slash = path.find_last_of('/');
      int parent_idx = -1;
      if (last_slash != std::string::npos && last_slash > 0) {
          std::string parent_path = path.substr(0, last_slash);
          parent_idx = get_or_create_folder(parent_path);
      }

      Node folder_node;
      folder_node.name = path;
      if (last_slash != std::string::npos && last_slash < path.size() - 1) {
          folder_node.name = "📁 " + path.substr(last_slash + 1);
      } else {
          folder_node.name = "📁 " + path;
      }
      folder_node.type = "folder";
      folder_node.src_dir = path;
      folder_node.width = 200.0f;
      folder_node.height = 50.0f;
      folder_node.is_folder = true;
      folder_node.expanded = false; // Initially collapsed
      folder_node.parent_idx = parent_idx;
      folder_node.color = SkColorSetRGB(33, 150, 243);

      int idx = graph.nodes.size();
      graph.nodes.push_back(folder_node);
      dir_to_node_idx[path] = idx;
      return idx;
  };

  for (int i = 0; i < original_target_count; ++i) {
      if (graph.nodes[i].src_dir.empty()) continue;
      int folder_idx = get_or_create_folder(graph.nodes[i].src_dir);
      if (folder_idx != -1) {
          graph.nodes[i].parent_idx = folder_idx;
      }
  }
  
  // Re-establish children links (safe as vector push_back is done)
  for (int i = 0; i < (int)graph.nodes.size(); ++i) {
      int p = graph.nodes[i].parent_idx;
      if (p != -1) {
          graph.nodes[p].children.push_back(i);
      }
  }
}

void CompileVisibleGraph(const Graph& full_graph, VisibleGraph& v_graph) {
    v_graph.visible_node_indices.clear();
    v_graph.visible_edges.clear();

    std::vector<bool> is_visible(full_graph.nodes.size(), false);
    std::vector<int> queue;

    // Roots
    for (int i = 0; i < (int)full_graph.nodes.size(); ++i) {
        if (full_graph.nodes[i].parent_idx == -1) {
            is_visible[i] = true;
            queue.push_back(i);
        }
    }

    while (!queue.empty()) {
        int idx = queue.back();
        queue.pop_back();
        
        v_graph.visible_node_indices.push_back(idx);

        if (full_graph.nodes[idx].is_folder && full_graph.nodes[idx].expanded) {
            for (int child_idx : full_graph.nodes[idx].children) {
                is_visible[child_idx] = true;
                queue.push_back(child_idx);
            }
        }
    }

    auto get_visible_ancestor = [&](int node_idx) -> int {
        int curr = node_idx;
        while (curr != -1 && !is_visible[curr]) {
            curr = full_graph.nodes[curr].parent_idx;
        }
        return curr;
    };

    std::set<std::pair<int, int>> edge_dedup;
    for (const auto& edge : full_graph.edges) {
        int vis_from = get_visible_ancestor(edge.from_index);
        int vis_to = get_visible_ancestor(edge.to_index);

        if (vis_from != -1 && vis_to != -1 && vis_from != vis_to) {
            if (edge_dedup.insert({vis_from, vis_to}).second) {
                Edge v_edge = edge;
                v_edge.from_index = vis_from;
                v_edge.to_index = vis_to;
                v_graph.visible_edges.push_back(v_edge);
            }
        }
    }
}

void ComputeGraphLayout(Graph& graph, const VisibleGraph& v_graph, float layout_width = 5000.0f, float layout_height = 4000.0f) {
  int vn = (int)v_graph.visible_node_indices.size();
  if (vn == 0) return;

  std::unordered_map<int, int> node_to_vidx;
  for(int i = 0; i < vn; ++i) {
      node_to_vidx[v_graph.visible_node_indices[i]] = i;
  }

  std::vector<std::vector<int>> adj(vn);
  std::vector<int> in_degree(vn, 0);

  for (const auto& edge : v_graph.visible_edges) {
    int u = node_to_vidx[edge.from_index];
    int v = node_to_vidx[edge.to_index];
    adj[u].push_back(v);
    in_degree[v]++;
  }

  std::vector<int> level(vn, 0);
  std::vector<int> queue;
  for (int i = 0; i < vn; ++i) {
    if (in_degree[i] == 0) queue.push_back(i);
  }

  int head = 0;
  while (head < (int)queue.size()) {
    int u = queue[head++];
    for (int v : adj[u]) {
      level[v] = std::max(level[v], level[u] + 1);
      in_degree[v]--;
      if (in_degree[v] == 0) {
        queue.push_back(v);
      }
    }
  }

  int max_level = 0;
  for (int i = 0; i < vn; ++i) {
    graph.nodes[v_graph.visible_node_indices[i]].level = level[i];
    max_level = std::max(max_level, level[i]);
  }

  std::vector<std::vector<int>> level_nodes(max_level + 1);
  for (int i = 0; i < vn; ++i) {
    level_nodes[level[i]].push_back(v_graph.visible_node_indices[i]);
  }

  float level_spacing_y = layout_height / std::max(max_level + 1, 1);
  if (level_spacing_y < 140.0f) level_spacing_y = 140.0f;

  for (int l = 0; l <= max_level; ++l) {
    int count = (int)level_nodes[l].size();
    float node_spacing_x = layout_width / std::max(count + 1, 1);
    if (node_spacing_x < 200.0f) node_spacing_x = 200.0f;

    for (int i = 0; i < count; ++i) {
      int idx = level_nodes[l][i];
      graph.nodes[idx].x = (i + 1) * node_spacing_x;
      graph.nodes[idx].y = 120.0f + l * level_spacing_y;
    }
  }
}

}  // namespace

int main(int argc, char** argv) {
  std::string json_path;
  if (argc > 1) {
    json_path = argv[1];
  } else {
    std::string home = getenv("HOME") ? getenv("HOME") : "/tmp";
    if (std::ifstream(home + "/android_bp.json").good() &&
        std::ifstream(home + "/android_bp.json").tellg() > 0) {
      json_path = home + "/android_bp.json";
    } else if (std::ifstream(home + "/chromium_gn.json").good()) {
      json_path = home + "/chromium_gn.json";
    } else {
      json_path = "demos/android_bp.json";
    }
  }

  Graph graph;
  if (!ParseGorMakeJson(json_path, graph)) {
    std::cerr << "Failed to parse JSON from " << json_path << std::endl;
    return 1;
  }

  BuildFolderHierarchy(graph);
  VisibleGraph v_graph;
  CompileVisibleGraph(graph, v_graph);
  ComputeGraphLayout(graph, v_graph);

  if (SDL_Init(SDL_INIT_VIDEO) != 0) {
    std::cerr << "SDL_Init Error: " << SDL_GetError() << std::endl;
    return 1;
  }

  int win_width = 1280;
  int win_height = 800;
  SDL_Window* window = SDL_CreateWindow(
      "GorMake Target Relationship Visualizer (Skia Engine)",
      SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, win_width, win_height,
      SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE);
  if (!window) {
    std::cerr << "SDL_CreateWindow Error: " << SDL_GetError() << std::endl;
    SDL_Quit();
    return 1;
  }

  // Camera state
  float pan_x = 0;
  float pan_y = 0;
  float zoom = 1.0f;

  auto fit_camera_to_bounds = [&]() {
      float min_x = std::numeric_limits<float>::max();
      float max_x = std::numeric_limits<float>::lowest();
      float min_y = std::numeric_limits<float>::max();
      float max_y = std::numeric_limits<float>::lowest();

      for (int idx : v_graph.visible_node_indices) {
          float hx = graph.nodes[idx].width / 2.0f;
          float hy = graph.nodes[idx].height / 2.0f;
          min_x = std::min(min_x, graph.nodes[idx].x - hx);
          max_x = std::max(max_x, graph.nodes[idx].x + hx);
          min_y = std::min(min_y, graph.nodes[idx].y - hy);
          max_y = std::max(max_y, graph.nodes[idx].y + hy);
      }

      if (max_x > min_x && max_y > min_y) {
          float bounds_w = max_x - min_x + 300.0f; // 150 padding L/R
          float bounds_h = max_y - min_y + 300.0f;
          float zoom_x = (float)win_width / bounds_w;
          float zoom_y = (float)win_height / bounds_h;
          zoom = std::min(zoom_x, zoom_y);
          if (zoom > 1.2f) zoom = 1.2f;
          if (zoom < 0.02f) zoom = 0.02f;

          float cx = (min_x + max_x) / 2.0f;
          float cy = (min_y + max_y) / 2.0f;
          pan_x = win_width / 2.0f - cx * zoom;
          pan_y = win_height / 2.0f - cy * zoom;
      } else {
          pan_x = win_width / 2.0f;
          pan_y = win_height / 2.0f;
          zoom = 1.0f;
      }
  };

  fit_camera_to_bounds();
  bool dragging = false;
  int drag_start_x = 0;
  int drag_start_y = 0;
  int selected_node = -1;
  int hovered_node = -1;

  bool is_animating = false;
  uint32_t anim_start_time = 0;
  uint32_t anim_duration_ms = 0;
  float path_start_x = 0;
  float path_start_y = 0;
  float path_target_x = 0;
  float path_target_y = 0;

  struct PopupRect {
      int node_idx;
      SkRect rect;
  };
  std::vector<PopupRect> popup_click_rects;
  std::vector<int> popup_inbound;
  std::vector<int> popup_outbound;

  auto update_popup = [&](int new_selected_node) {
      selected_node = new_selected_node;
      popup_inbound.clear();
      popup_outbound.clear();
      popup_click_rects.clear();
      if (selected_node >= 0) {
          for (const auto& edge : v_graph.visible_edges) {
              if (edge.to_index == selected_node) popup_inbound.push_back(edge.from_index);
              if (edge.from_index == selected_node) popup_outbound.push_back(edge.to_index);
          }
      }
  };

  bool running = true;
  SDL_Event event;

  while (running) {
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_QUIT) {
        std::cerr << "Received SDL_QUIT event, exiting..." << std::endl;
        running = false;
      } else if (event.type == SDL_WINDOWEVENT) {
        if (event.window.event == SDL_WINDOWEVENT_RESIZED) {
          win_width = event.window.data1;
          win_height = event.window.data2;
        }
      } else if (event.type == SDL_MOUSEBUTTONDOWN) {
        float mouse_x = event.button.x;
        float mouse_y = event.button.y;
        float mouse_graph_x = (mouse_x - pan_x) / zoom;
        float mouse_graph_y = (mouse_y - pan_y) / zoom;

        // Perform hit detection on nodes
        int new_selection = -1;
        // Iterate in reverse for painter's algorithm hit testing
        for (int i = (int)v_graph.visible_node_indices.size() - 1; i >= 0; --i) {
          int node_idx = v_graph.visible_node_indices[i];
          const auto& node = graph.nodes[node_idx];
          if (mouse_graph_x >= node.x - node.width/2 && mouse_graph_x <= node.x + node.width/2 &&
              mouse_graph_y >= node.y - node.height/2 && mouse_graph_y <= node.y + node.height/2) {
            new_selection = node_idx;
            break;
          }
        }

        if (event.button.button == SDL_BUTTON_LEFT) {
          bool clicked_popup = false;
          if (selected_node >= 0) {
              for (const auto& pr : popup_click_rects) {
                  if (mouse_x >= pr.rect.fLeft && mouse_x <= pr.rect.fRight &&
                      mouse_y >= pr.rect.fTop && mouse_y <= pr.rect.fBottom) {
                      
                      // Walk up the hierarchy and expand everything
                      int curr = pr.node_idx;
                      while (curr != -1) {
                          graph.nodes[curr].expanded = true;
                          curr = graph.nodes[curr].parent_idx;
                      }
                      
                      // Re-layout before flying
                      CompileVisibleGraph(graph, v_graph);
                      ComputeGraphLayout(graph, v_graph);
                      
                      // Fly to node
                      update_popup(pr.node_idx);
                      
                      float curr_pan_x = pan_x;
                      float curr_pan_y = pan_y;
                      float target_pan_x = win_width / 2.0f - graph.nodes[pr.node_idx].x * zoom;
                      float target_pan_y = win_height / 2.0f - graph.nodes[pr.node_idx].y * zoom;

                      // Make distance calculation relative to zoom scale so flying across wide maps takes longer
                      float dist = std::sqrt(std::pow(target_pan_x - curr_pan_x, 2) + std::pow(target_pan_y - curr_pan_y, 2));
                      float duration = std::max(1000.0f, dist * 0.5f); // 1.0 second minimum, scales up to distance
                      if (duration > 4000.0f) duration = 4000.0f; // Max 4 seconds

                      path_start_x = curr_pan_x;
                      path_start_y = curr_pan_y;
                      path_target_x = target_pan_x;
                      path_target_y = target_pan_y;
                      anim_start_time = SDL_GetTicks();
                      anim_duration_ms = duration;
                      is_animating = true;
                      
                      clicked_popup = true;
                      break;
                  }
              }
          }

          if (!clicked_popup) {
              dragging = true;
              drag_start_x = event.button.x;
              drag_start_y = event.button.y;

              if (new_selection != -1) {
                  if (graph.nodes[new_selection].is_folder) {
                      // Toggle folder expansion
                      graph.nodes[new_selection].expanded = !graph.nodes[new_selection].expanded;
                      CompileVisibleGraph(graph, v_graph);
                      ComputeGraphLayout(graph, v_graph);
                  }
              } else {
                  // Clicked empty space on left click, cancel popup
                  update_popup(-1);
              }
          }
        } else if (event.button.button == SDL_BUTTON_RIGHT) {
            // Right click shows popup window
            update_popup(new_selection);
        }
      } else if (event.type == SDL_MOUSEBUTTONUP) {
        if (event.button.button == SDL_BUTTON_LEFT) {
          dragging = false;
        }
      } else if (event.type == SDL_MOUSEMOTION) {
        if (dragging) {
          pan_x += (event.motion.x - drag_start_x);
          pan_y += (event.motion.y - drag_start_y);
          drag_start_x = event.motion.x;
          drag_start_y = event.motion.y;
        }

        float mouse_graph_x = (event.motion.x - pan_x) / zoom;
        float mouse_graph_y = (event.motion.y - pan_y) / zoom;
        hovered_node = -1;
        for (int i = (int)v_graph.visible_node_indices.size() - 1; i >= 0; --i) {
          int node_idx = v_graph.visible_node_indices[i];
          const auto& node = graph.nodes[node_idx];
          if (mouse_graph_x >= node.x - node.width/2 && mouse_graph_x <= node.x + node.width/2 &&
              mouse_graph_y >= node.y - node.height/2 && mouse_graph_y <= node.y + node.height/2) {
            hovered_node = node_idx;
            break;
          }
        }
      } else if (event.type == SDL_MOUSEWHEEL) {
        float zoom_factor = (event.wheel.y > 0) ? 1.15f : 0.85f;
        zoom *= zoom_factor;
        zoom = std::max(0.05f, std::min(5.0f, zoom));
      } else if (event.type == SDL_KEYDOWN) {
        if (event.key.keysym.sym == SDLK_r) {
          fit_camera_to_bounds();
          selected_node = -1;
          update_popup(-1);
        } else if (event.key.keysym.sym == SDLK_p) {
          SDL_SaveBMP(SDL_GetWindowSurface(window), "/tmp/vis_screenshot.bmp");
          std::cerr << "Saved screenshot to /tmp/vis_screenshot.bmp" << std::endl;
        } else {
          switch (event.key.keysym.sym) {
            case SDLK_ESCAPE:
              break;
            case SDLK_UP:
            case SDLK_w:
              pan_y += 50.0f;
              break;
            case SDLK_DOWN:
            case SDLK_s:
              pan_y -= 50.0f;
              break;
            case SDLK_LEFT:
            case SDLK_a:
              pan_x += 50.0f;
              break;
            case SDLK_RIGHT:
            case SDLK_d:
              pan_x -= 50.0f;
              break;
            case SDLK_PLUS:
            case SDLK_EQUALS:
              zoom *= 1.25f;
              break;
            case SDLK_MINUS:
              zoom *= 0.8f;
              break;
          }
        }
      }
    }

    static SkBitmap bitmap;
    static std::unique_ptr<SkCanvas> canvas;
    
    uint32_t current_time = SDL_GetTicks();
    if (is_animating) {
        float t = (float)(current_time - anim_start_time) / anim_duration_ms;
        if (t >= 1.0f) {
            t = 1.0f;
            is_animating = false;
        }
        // Cubic ease in-out
        float u = t < 0.5f ? 4.0f * t * t * t : 1.0f - std::pow(-2.0f * t + 2.0f, 3.0f) / 2.0f;
        
        pan_x = path_start_x + (path_target_x - path_start_x) * u;
        pan_y = path_start_y + (path_target_y - path_start_y) * u;
    }

    if (bitmap.width() != win_width || bitmap.height() != win_height) {
        bitmap.allocN32Pixels(win_width, win_height);
        canvas = std::make_unique<SkCanvas>(bitmap);
        printf("Recreated SkBitmap %dx%d\n", win_width, win_height);
        fflush(stdout);
    }
    
    if (!canvas) continue;
    canvas->clear(SkColorSetRGB(245, 247, 250));

    // Draw grid background
    SkPaint grid_paint;
    grid_paint.setColor(SkColorSetARGB(40, 200, 200, 220));
    grid_paint.setStrokeWidth(1.0f);

    float grid_size = 50.0f * zoom;
    float start_x = fmod(pan_x, grid_size);
    float start_y = fmod(pan_y, grid_size);

    for (float x = start_x; x < win_width; x += grid_size) {
      canvas->drawLine(x, 0, x, win_height, grid_paint);
    }
    for (float y = start_y; y < win_height; y += grid_size) {
      canvas->drawLine(0, y, win_width, y, grid_paint);
    }

    // Save canvas state for transformed graph drawing
    canvas->save();
    canvas->translate(pan_x, pan_y);
    canvas->scale(zoom, zoom);

    // Viewport Culling & LOD
    float vis_min_x = (0.0f - pan_x) / zoom - 200.0f;
    float vis_max_x = (win_width - pan_x) / zoom + 200.0f;
    float vis_min_y = (0.0f - pan_y) / zoom - 200.0f;
    float vis_max_y = (win_height - pan_y) / zoom + 200.0f;

    std::vector<bool> node_visible(graph.nodes.size(), false);
    for (int i = 0; i < (int)v_graph.visible_node_indices.size(); ++i) {
      int idx = v_graph.visible_node_indices[i];
      if (graph.nodes[idx].x >= vis_min_x && graph.nodes[idx].x <= vis_max_x &&
          graph.nodes[idx].y >= vis_min_y && graph.nodes[idx].y <= vis_max_y) {
        node_visible[idx] = true;
      }
    }

    // Draw edges
    SkPaint edge_paint;
    edge_paint.setAntiAlias(zoom >= 0.15f);

    if (zoom >= 0.15f || selected_node >= 0) {
      for (const auto& edge : v_graph.visible_edges) {
        bool is_highlighted = (selected_node >= 0 && (edge.from_index == selected_node || edge.to_index == selected_node));
        
        // Skip normal edges if we are strongly zoomed out unless highlighted
        if (!is_highlighted && zoom < 0.15f) continue;

        // Extremely fast visibility check
        if (!is_highlighted && !node_visible[edge.from_index] && !node_visible[edge.to_index]) {
          continue;
        }

        const auto& src = graph.nodes[edge.from_index];
        const auto& dst = graph.nodes[edge.to_index];

        if (is_highlighted) {
          edge_paint.setColor(SkColorSetRGB(255, 111, 0));
          edge_paint.setStrokeWidth(3.5f * (1.0f / zoom));
        } else {
          edge_paint.setColor(edge.color);
          edge_paint.setStrokeWidth(1.2f);
        }

        float control_y = (src.y + dst.y) / 2.0f;
        SkPathBuilder builder;
        builder.moveTo(src.x, src.y);
        builder.cubicTo(src.x, control_y, dst.x, control_y, dst.x, dst.y);
        SkPath path = builder.detach();

        edge_paint.setStyle(SkPaint::kStroke_Style);
        canvas->drawPath(path, edge_paint);
      }
    }

    // Draw nodes
    static sk_sp<SkTypeface> global_typeface = nullptr;
    if (!global_typeface) {
        if (auto mgr = SkFontMgr_New_Custom_Empty()) {
            global_typeface = mgr->makeFromFile("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
        }
    }
    
    SkFont font(global_typeface, 12.0f);

    for (int i = 0; i < (int)v_graph.visible_node_indices.size(); ++i) {
      int idx = v_graph.visible_node_indices[i];
      const auto& node = graph.nodes[idx];

      // Viewport culling
      if (node.x < vis_min_x || node.x > vis_max_x || node.y < vis_min_y || node.y > vis_max_y) {
        if (idx != selected_node && idx != hovered_node) continue;
      }

      SkRect rect = SkRect::MakeXYWH(node.x - node.width/2, node.y - node.height/2, node.width, node.height);

      // Node background
      SkPaint node_paint;
      node_paint.setAntiAlias(zoom >= 0.15f);
      node_paint.setStyle(SkPaint::kFill_Style);
      node_paint.setColor(node.color);

      if (idx == selected_node) {
        node_paint.setColor(SkColorSetRGB(255, 193, 7)); // Amber highlight
      } else if (idx == hovered_node) {
        node_paint.setColor(SkColorSetRGB(64, 196, 255)); // Light blue highlight
      }

      canvas->drawRoundRect(rect, 8.0f, 8.0f, node_paint);

      // Node border
      if (zoom >= 0.2f || idx == selected_node || idx == hovered_node) {
        SkPaint border_paint;
        border_paint.setAntiAlias(true);
        border_paint.setStyle(SkPaint::kStroke_Style);
        border_paint.setColor(SkColorSetRGB(40, 40, 40));
        border_paint.setStrokeWidth(idx == selected_node ? 3.0f : (node.is_folder ? 2.0f : 1.0f));
        canvas->drawRoundRect(rect, 8.0f, 8.0f, border_paint);
      }

      // Node label (LOD: skip text when zoomed far out for speed, unless folder)
      if (zoom >= 0.25f || idx == selected_node || idx == hovered_node || (node.is_folder && zoom >= 0.05f)) {
        SkPaint text_paint;
        text_paint.setColor(SK_ColorWHITE);
        text_paint.setAntiAlias(true);

        std::string label = node.name;
        if (!node.is_folder && label.size() > 18) label = label.substr(0, 16) + "..";

        canvas->drawString(label.c_str(), node.x - node.width/2 + 10.0f, node.y + (node.is_folder ? 6.0f : 4.0f), font, text_paint);
      }
    }

    canvas->restore(); // Restore transformed canvas

    // --- Draw HUD Overlay (Screen Space) ---
    SkPaint hud_bg;
    hud_bg.setColor(SkColorSetARGB(230, 20, 25, 35));
    hud_bg.setStyle(SkPaint::kFill_Style);
    canvas->drawRect(SkRect::MakeXYWH(0, 0, win_width, 60), hud_bg);

    SkFont hud_font(global_typeface, 16.0f);
    SkPaint hud_text;
    hud_text.setColor(SK_ColorWHITE);
    hud_text.setAntiAlias(true);

    std::string title = "GorMake Visualizer (Skia Engine) | " + graph.format_name + " | Total Modules: " +
                        std::to_string(graph.total_items) + " | Edges: " + std::to_string(graph.edges.size());
    canvas->drawString(title.c_str(), 20.0f, 28.0f, hud_font, hud_text);

    hud_font.setSize(12.0f);
    hud_text.setColor(SkColorSetRGB(180, 190, 200));
    canvas->drawString("Controls: Mouse Drag (Pan) | Scroll (Zoom) | Click (Select) | R (Reset) | Arrow Keys/WASD", 20.0f, 48.0f, hud_font, hud_text);

    // Selected node details panel
    if (selected_node >= 0 && selected_node < (int)graph.nodes.size()) {
      const auto& snode = graph.nodes[selected_node];
      canvas->drawRect(SkRect::MakeXYWH(20, win_height - 120, 480, 100), hud_bg);

      hud_font.setSize(14.0f);
      hud_text.setColor(SkColorSetRGB(255, 193, 7));
      canvas->drawString(("Selected: " + snode.name).c_str(), 35.0f, win_height - 95.0f, hud_font, hud_text);

      hud_font.setSize(12.0f);
      hud_text.setColor(SK_ColorWHITE);
      canvas->drawString(("Type: " + snode.type + " | Level: " + std::to_string(snode.level)).c_str(), 35.0f, win_height - 75.0f, hud_font, hud_text);
      canvas->drawString(("Source Dir: " + snode.src_dir).c_str(), 35.0f, win_height - 55.0f, hud_font, hud_text);
      canvas->drawString(("Dependencies: " + std::to_string(snode.raw_deps.size()) + " target(s)").c_str(), 35.0f, win_height - 35.0f, hud_font, hud_text);
    }

    // Node connections pop-up
    popup_click_rects.clear();
    if (selected_node >= 0 && selected_node < (int)graph.nodes.size()) {
        float popup_w = 400.0f;
        float popup_h = 40.0f + popup_inbound.size() * 20.0f + popup_outbound.size() * 20.0f;
        if (popup_h > win_height - 100) popup_h = win_height - 100;

        const auto& snode = graph.nodes[selected_node];
        float popup_x = snode.x * zoom + pan_x + (snode.width / 2.0f) * zoom + 10.0f;
        float popup_y = snode.y * zoom + pan_y - popup_h / 2.0f;

        // Clamp popup to window boundaries
        if (popup_x + popup_w > win_width - 10.0f) popup_x = win_width - popup_w - 10.0f;
        if (popup_y + popup_h > win_height - 10.0f) popup_y = win_height - popup_h - 10.0f;
        if (popup_x < 10.0f) popup_x = 10.0f;
        if (popup_y < 80.0f) popup_y = 80.0f; // leave room for top HUD
        
        canvas->drawRect(SkRect::MakeXYWH(popup_x, popup_y, popup_w, popup_h), hud_bg);
        
        hud_font.setSize(14.0f);
        hud_text.setColor(SkColorSetRGB(255, 193, 7));
        canvas->drawString("Node Connections (Click to fly):", popup_x + 15.0f, popup_y + 25.0f, hud_font, hud_text);
        
        hud_font.setSize(12.0f);
        float current_y = popup_y + 45.0f;
        
        hud_text.setColor(SkColorSetRGB(100, 255, 100));
        for (int in_node : popup_inbound) {
            if (current_y > popup_y + popup_h - 10.0f) break; // clamp rendering
            std::string label = "<- " + graph.nodes[in_node].name;
            canvas->drawString(label.c_str(), popup_x + 15.0f, current_y, hud_font, hud_text);
            
            PopupRect pr;
            pr.node_idx = in_node;
            pr.rect = SkRect::MakeXYWH(popup_x + 10.0f, current_y - 12.0f, popup_w - 20.0f, 16.0f);
            popup_click_rects.push_back(pr);
            
            current_y += 20.0f;
        }
        
        hud_text.setColor(SkColorSetRGB(255, 100, 100));
        for (int out_node : popup_outbound) {
            if (current_y > popup_y + popup_h - 10.0f) break;
            std::string label = "-> " + graph.nodes[out_node].name;
            canvas->drawString(label.c_str(), popup_x + 15.0f, current_y, hud_font, hud_text);
            
            PopupRect pr;
            pr.node_idx = out_node;
            pr.rect = SkRect::MakeXYWH(popup_x + 10.0f, current_y - 12.0f, popup_w - 20.0f, 16.0f);
            popup_click_rects.push_back(pr);
            
            current_y += 20.0f;
        }
    }

    canvas->restore();
    
    SDL_Surface* window_surface = SDL_GetWindowSurface(window);
    if (!window_surface || !window_surface->pixels) continue;
    
    if (SDL_MUSTLOCK(window_surface)) {
        SDL_LockSurface(window_surface);
    }

    // Copy rendered pixels to the SDL window
    if (bitmap.readyToDraw() && bitmap.getPixels()) {
        uint32_t rmask, gmask, bmask, amask;
#if SDL_BYTEORDER == SDL_BIG_ENDIAN
        rmask = 0xff000000; gmask = 0x00ff0000; bmask = 0x0000ff00; amask = 0x000000ff;
#else
        rmask = 0x00ff0000; gmask = 0x0000ff00; bmask = 0x000000ff; amask = 0xff000000;
#endif
        SDL_Surface* skia_surface = SDL_CreateRGBSurfaceFrom(
            bitmap.getPixels(), win_width, win_height, 32, bitmap.rowBytes(),
            rmask, gmask, bmask, amask);
            
        if (skia_surface) {
            SDL_BlitSurface(skia_surface, NULL, window_surface, NULL);
            SDL_FreeSurface(skia_surface);
        }
    }

    if (SDL_MUSTLOCK(window_surface)) {
        SDL_UnlockSurface(window_surface);
    }

    // Update SDL Window
    SDL_UpdateWindowSurface(window);

    // Auto screenshot every 5 seconds for OCR
    static uint32_t last_scrot = 0;
    if (SDL_GetTicks() - last_scrot > 5000) {
      last_scrot = SDL_GetTicks();
    }

    SDL_Delay(16);
  }

  std::cerr << "Exited main loop, shutting down SDL" << std::endl;
  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
