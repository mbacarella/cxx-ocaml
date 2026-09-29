module X = struct type t = int let compare = compare end
module Y = struct type t = bool let compare = compare end
type t = Set.Make(X).t
type u = Set.Make(Y).t
