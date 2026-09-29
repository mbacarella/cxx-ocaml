module X = struct type t = int let compare = compare end
exception E of Set.Make(X).t
