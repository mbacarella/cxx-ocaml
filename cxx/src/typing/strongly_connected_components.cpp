// Port of utils/strongly_connected_components.ml (see
// strongly_connected_components.hpp): the Kosaraju module.
#include "cppcaml/typing/strongly_connected_components.hpp"

#include <algorithm>
#include <set>

namespace cppcaml::typing::strongly_connected_components::kosaraju {

namespace {
using Graph = std::vector<std::vector<long>>;

Graph transpose(const Graph& graph) {
  Graph transposed(graph.size());
  // add src dst = transposed.(src) <- dst :: transposed.(src), as
  // List.iter (fun dst -> add dst src) dsts
  // (appended, then reversed: the same lists without front insertions)
  for (std::size_t src = 0; src < graph.size(); ++src)
    for (long dst : graph[src]) transposed[static_cast<std::size_t>(dst)].push_back(static_cast<long>(src));
  for (auto& l : transposed) std::reverse(l.begin(), l.end());
  return transposed;
}

std::vector<long> depth_first_order(const Graph& graph) {
  std::size_t size = graph.size();
  std::vector<bool> marked(size, false);
  std::vector<long> stack(size, -1);
  std::size_t pos = 0;
  // aux node: mark, List.iter aux graph.(node), push node -- with an
  // explicit stack of (node, next successor)
  std::vector<std::pair<long, std::size_t>> work;
  for (std::size_t i = 0; i < size; ++i) {
    if (marked[i]) continue;
    marked[i] = true;
    work.emplace_back(static_cast<long>(i), 0);
    while (!work.empty()) {
      auto& [node, k] = work.back();
      const std::vector<long>& succ = graph[static_cast<std::size_t>(node)];
      if (k < succ.size()) {
        long next = succ[k++];
        if (!marked[static_cast<std::size_t>(next)]) {
          marked[static_cast<std::size_t>(next)] = true;
          work.emplace_back(next, 0);
        }
      } else {
        stack[pos++] = node;
        work.pop_back();
      }
    }
  }
  return stack;
}

std::pair<std::vector<long>, long> mark(const std::vector<long>& order, const Graph& graph0) {
  std::size_t size = graph0.size();
  Graph graph = transpose(graph0);
  std::vector<bool> marked(size, false);
  std::vector<long> id(size, -1);
  long count = 0;
  std::vector<std::pair<long, std::size_t>> work;
  for (std::size_t j = size; j-- > 0;) {
    long node = order[j];
    if (marked[static_cast<std::size_t>(node)]) continue;
    // aux order.(i): mark, id, List.iter aux graph.(node)
    marked[static_cast<std::size_t>(node)] = true;
    id[static_cast<std::size_t>(node)] = count;
    work.emplace_back(node, 0);
    while (!work.empty()) {
      auto& [n, k] = work.back();
      const std::vector<long>& succ = graph[static_cast<std::size_t>(n)];
      if (k < succ.size()) {
        long next = succ[k++];
        if (!marked[static_cast<std::size_t>(next)]) {
          marked[static_cast<std::size_t>(next)] = true;
          id[static_cast<std::size_t>(next)] = count;
          work.emplace_back(next, 0);
        }
      } else {
        work.pop_back();
      }
    }
    ++count;
  }
  return {id, count};
}
}  // namespace

ComponentGraph component_graph(const Graph& graph) {
  std::vector<long> dfo = depth_first_order(graph);
  auto [components, ncomponents] = mark(dfo, graph);
  std::vector<std::vector<long>> id_scc(static_cast<std::size_t>(ncomponents));
  std::vector<std::set<long>> component_graph(static_cast<std::size_t>(ncomponents));
  // Array.iteri: id_scc.(component) <- node :: id_scc.(component)
  for (std::size_t node = 0; node < components.size(); ++node) {
    std::size_t component = static_cast<std::size_t>(components[node]);
    id_scc[component].push_back(static_cast<long>(node));  // (reversed below)
    for (long dep : graph[node]) component_graph[component].insert(components[static_cast<std::size_t>(dep)]);
  }
  for (auto& l : id_scc) std::reverse(l.begin(), l.end());
  ComponentGraph r;
  r.sorted_connected_components = std::move(id_scc);
  for (auto& s : component_graph) r.component_edges.emplace_back(s.begin(), s.end());  // Int.Set.elements
  return r;
}

}  // namespace cppcaml::typing::strongly_connected_components::kosaraju
