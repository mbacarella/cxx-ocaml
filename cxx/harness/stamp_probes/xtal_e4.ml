module X = struct type t = int let compare = compare end
module S = Stdlib__Set.Make(X)
type t = Stdlib__Set.Make(X).t
