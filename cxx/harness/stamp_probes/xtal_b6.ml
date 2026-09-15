module X = struct type t = int let compare = compare end
module Y = struct type t = bool let compare = compare end
type t = Stdlib__Set.Make(X).t
type u = Stdlib__Set.Make(Y).t
