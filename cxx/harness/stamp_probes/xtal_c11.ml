module X = struct type t = int let compare = compare end
type t = < m : Set.Make(X).t >
