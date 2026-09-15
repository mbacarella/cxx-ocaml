module X = struct type t = int let compare = compare end
type t = private Set.Make(X).t
