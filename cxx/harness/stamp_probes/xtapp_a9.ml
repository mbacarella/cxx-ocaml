module X = struct type t = int let compare = compare end
module S = Set.Make(X)
let f (x : Set.Make(X).t) = x
