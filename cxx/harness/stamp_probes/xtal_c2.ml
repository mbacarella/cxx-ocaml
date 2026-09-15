module X = struct type t = int let compare = compare end
type t = { fld : Set.Make(X).t list }
