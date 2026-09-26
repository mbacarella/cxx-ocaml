type 'a tree = Leaf | Node of 'a tree * 'a * 'a tree
let rec size = function Leaf -> 0 | Node (l, _, r) -> size l + 1 + size r
