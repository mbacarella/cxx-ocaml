module X = struct type t = int let compare = compare end
let f (x : Set.Make(X).t) = x
