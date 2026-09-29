module X = struct type t = int let compare = compare end
module S = Set.Make(X)
module type T = sig val v : Set.Make(X).t end
