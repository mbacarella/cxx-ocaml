module X = struct type t = int let compare = compare end
type t = [ `A of Set.Make(X).t ]
