module X = struct type t = int let compare = compare end
type t = Z of { fld : Set.Make(X).t }
