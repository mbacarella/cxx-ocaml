module X = struct type t = int let compare = compare end
type t = Z of Set.Make(X).t
