// Port of utils/strongly_connected_components.ml: Kosaraju's algorithm on
// an Identifiable graph (Id.Set.t Id.Map.t), with OCaml's orders (the
// components and their members come out in ocamlopt's order).
#pragma once

#include <utility>
#include <vector>

#include "cppcaml/typing/misc.hpp"
#include "cppcaml/typing/ocaml_map.hpp"

namespace cppcaml::typing::strongly_connected_components {

namespace kosaraju {
struct ComponentGraph {
  std::vector<std::vector<long>> sorted_connected_components;  // int list array
  std::vector<std::vector<long>> component_edges;
};
ComponentGraph component_graph(const std::vector<std::vector<long>>& graph);
}  // namespace kosaraju

// component = Has_loop of Id.t list | No_loop of Id.t
template <class K>
struct Component {
  bool has_loop;
  std::vector<K> ids;  // (one id: No_loop)
};

// Make (Id).component_graph / connected_components_sorted_from_roots_to_leaf
template <class K, class Cmp>
std::vector<std::pair<Component<K>, std::vector<long>>> component_graph(const OMap<K, OSet<K, Cmp>, Cmp>& graph) {
  // number graph
  std::vector<std::pair<K, OSet<K, Cmp>>> a = graph.bindings();
  std::size_t size = a.size();
  std::vector<K> forth;
  forth.reserve(size);
  for (auto& b : a) forth.push_back(b.first);
  OMap<K, long, Cmp> back;
  for (std::size_t i = 0; i < size; ++i) back = back.add(forth[i], static_cast<long>(i));
  std::vector<std::vector<long>> integer_graph(size);
  for (std::size_t i = 0; i < size; ++i) {
    // Id.Set.fold (fun dest acc -> v :: acc) dests []: the largest first
    std::vector<long> l;
    a[i].second.iter([&](const K& dest) {
      const long* v = back.find_opt(dest);
      if (!v) misc::fatal_error("Strongly_connected_components: missing dependency");
      l.push_back(*v);
    });
    integer_graph[i] = std::vector<long>(l.rbegin(), l.rend());
  }
  kosaraju::ComponentGraph cg = kosaraju::component_graph(integer_graph);
  std::vector<std::pair<Component<K>, std::vector<long>>> out;
  for (std::size_t c = 0; c < cg.sorted_connected_components.size(); ++c) {
    const std::vector<long>& nodes = cg.sorted_connected_components[c];
    Component<K> comp;
    if (nodes.empty()) misc::fatal_error("Strongly_connected_components.component_graph");
    if (nodes.size() == 1) {
      long node = nodes[0];
      const std::vector<long>& succ = integer_graph[static_cast<std::size_t>(node)];
      bool self = false;
      for (long x : succ) self = self || x == node;
      comp.has_loop = self;
      comp.ids.push_back(forth[static_cast<std::size_t>(node)]);
    } else {
      comp.has_loop = true;
      for (long node : nodes) comp.ids.push_back(forth[static_cast<std::size_t>(node)]);
    }
    out.emplace_back(std::move(comp), cg.component_edges[c]);
  }
  return out;
}

template <class K, class Cmp>
std::vector<Component<K>> connected_components_sorted_from_roots_to_leaf(const OMap<K, OSet<K, Cmp>, Cmp>& graph) {
  std::vector<Component<K>> out;
  for (auto& p : component_graph(graph)) out.push_back(std::move(p.first));
  return out;
}

}  // namespace cppcaml::typing::strongly_connected_components
