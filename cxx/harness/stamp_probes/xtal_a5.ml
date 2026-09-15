module X = struct type t = int let compare = compare end
type t = Stdlib.Set.Make(X).t
