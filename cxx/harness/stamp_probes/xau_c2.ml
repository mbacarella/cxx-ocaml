module Q = struct type t = int let compare = compare end
module A = Q
module S = Set.Make (A)
