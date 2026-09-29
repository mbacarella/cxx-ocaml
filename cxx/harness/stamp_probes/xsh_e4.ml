module M = struct type t = int let compare = compare end
module N : Map.OrderedType = M
let _ = let open Map.Make (M) in empty
