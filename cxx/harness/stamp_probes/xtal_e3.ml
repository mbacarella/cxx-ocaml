module X = struct type t = int let compare = compare end
module Y = struct type t = bool let compare = compare end
module S = Set.Make(X)
type t = Set.Make(Y).t
