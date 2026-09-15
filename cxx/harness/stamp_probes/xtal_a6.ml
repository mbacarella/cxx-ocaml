module X = struct type t = int let compare = compare end
type t = MoreLabels.Set.Make(X).t
