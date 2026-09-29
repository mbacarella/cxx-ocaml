module M = struct type t = int let compare = compare end
module A = Map.Make (M)
let f (module X : Map.OrderedType) = 0
