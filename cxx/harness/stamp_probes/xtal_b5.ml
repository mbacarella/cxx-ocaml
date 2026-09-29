module X = struct type t = int let compare = compare end
type t = Set.Make(X).t
type u = Set.Make(X).t
