module X = struct type t = int let compare = compare end
module S = Set.Make(X)
type t = Set.Make(X).t
type u = S.t
