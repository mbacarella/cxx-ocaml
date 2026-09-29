module X = struct type t = int let compare = compare end
type t = Stdlib__Set.Make(X).t
